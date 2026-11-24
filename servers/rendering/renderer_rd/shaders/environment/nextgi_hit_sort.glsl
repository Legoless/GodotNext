/**************************************************************************/
/*  nextgi_hit_sort.glsl                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#[compute]

#version 460

#VERSION_DEFINES

// GodotNext: nextgi_hit_lighting (#129, plan §21.2 contract L7). Sorted-deferred 16-bit counting sort:
// compacts active marked hit records and sorts them by their 16-bit material key (65536 buckets, 1 KiB
// workgroup shared memory). All atomic operations consume their return values into a sink (#277).
// Variants:
//   0: MODE_CLEAR       - zero 65536 histogram bins and write indirect dispatch commands
//   1: MODE_HISTOGRAM   - count per-key frequencies
//   2: MODE_SCAN_LOCAL  - 256-bin Blelloch exclusive prefix sum per workgroup
//   3: MODE_SCAN_BLOCKS - exclusive prefix sum over 256 block sums
//   4: MODE_SCAN_ADD    - add block prefix offsets to local offsets
//   5: MODE_SCATTER     - scatter record indices into sorted order by 16-bit material key

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

#include "nextgi_hit_lighting_common_inc.glsl"

const uint SORT_BINS = 65536u;
const uint SORT_BLOCK_SIZE = 256u;
const uint SORT_BLOCKS = 256u;

#if defined(MODE_HISTOGRAM) || defined(MODE_SCATTER)
layout(set = 0, binding = 0, std430) restrict readonly buffer SortHitRecords {
	NextGIHitLightRecord data[];
}
hit_records;
#endif

#if defined(MODE_CLEAR) || defined(MODE_HISTOGRAM) || defined(MODE_SCAN_LOCAL) || defined(MODE_SCAN_ADD) || defined(MODE_SCATTER)
layout(set = 0, binding = 1, std430) restrict buffer SortHistogram {
	uint data[];
}
histogram;
#endif

#if defined(MODE_SCAN_LOCAL) || defined(MODE_SCAN_BLOCKS)
layout(set = 0, binding = 2, std430) restrict buffer SortBlockSums {
	uint data[];
}
block_sums;
#elif defined(MODE_SCAN_ADD)
layout(set = 0, binding = 2, std430) restrict readonly buffer SortBlockSums {
	uint data[];
}
block_sums;
#endif

#if defined(MODE_SCATTER)
layout(set = 0, binding = 3, std430) restrict writeonly buffer SortSortedIndices {
	uint data[];
}
sorted_indices;
#endif

#if defined(MODE_CLEAR) || defined(MODE_HISTOGRAM) || defined(MODE_SCAN_ADD) || defined(MODE_SCATTER)
layout(set = 0, binding = 4, std430) restrict buffer SortCounters {
	NextGIHitLightCounters data;
}
counters;
#endif

// Indirect commands: [0..3]  = Reflection RT raygen indirect dispatch (refl_count, 1, 1);
//                    [4..7]  = Compute sort indirect dispatch ((active_count + 255) / 256, 1, 1);
//                    [8..11] = Diffuse GI RT raygen indirect dispatch (gi_count, 1, 1) (#128).
// Declared only in variants that write it so MODE_HISTOGRAM/MODE_SCATTER keep indirect_buffer
// exclusively in RESOURCE_USAGE_INDIRECT_BUFFER_READ for proper RDG barriers.
#if defined(MODE_CLEAR) || defined(MODE_SCAN_ADD)
layout(set = 0, binding = 5, std430) restrict writeonly buffer SortIndirect {
	uint data[];
}
indirect_cmds;
#endif

#if defined(MODE_CLEAR) || defined(MODE_HISTOGRAM) || defined(MODE_SCAN_ADD) || defined(MODE_SCATTER)
layout(push_constant, std430) uniform SortPush {
	uint max_rays;
	uint pad0;
	uint pad1;
	uint pad2;
}
sort_push;
#endif

#if defined(MODE_SCAN_LOCAL) || defined(MODE_SCAN_BLOCKS)
shared uint s_scan[SORT_BLOCK_SIZE];
#endif

void main() {
	uint gid = gl_GlobalInvocationID.x;
#if defined(MODE_CLEAR) || defined(MODE_HISTOGRAM) || defined(MODE_SCAN_ADD) || defined(MODE_SCATTER)
	uint active_count = min(counters.data.marked_hits, sort_push.max_rays);
#endif

#if defined(MODE_CLEAR)
	if (gid < SORT_BINS) {
		histogram.data[gid] = 0u;
	}
	if (gid == 0u) {
		counters.data.sorted_hits = active_count;
		counters.data.refl_sorted_hits = 0u;
		counters.data.gi_sorted_hits = 0u;
		indirect_cmds.data[0] = 0u;
		indirect_cmds.data[1] = 0u;
		indirect_cmds.data[2] = 0u;
		indirect_cmds.data[3] = 0u;
		uint groups = active_count > 0u ? (active_count + 255u) / 256u : 0u;
		indirect_cmds.data[4] = groups;
		indirect_cmds.data[5] = groups > 0u ? 1u : 0u;
		indirect_cmds.data[6] = groups > 0u ? 1u : 0u;
		indirect_cmds.data[7] = 0u;
		indirect_cmds.data[8] = 0u;
		indirect_cmds.data[9] = 0u;
		indirect_cmds.data[10] = 0u;
		indirect_cmds.data[11] = 0u;
	}
#elif defined(MODE_HISTOGRAM)
	if (gid >= active_count) {
		return;
	}
	uint key = nextgi_hl_record_material_key(hit_records.data[gid]);
	uint prev = atomicAdd(histogram.data[key], 1u);
	if (prev == 0xFFFFFFFFu) {
		counters.data.sink = prev;
	}
#elif defined(MODE_SCAN_LOCAL)
	uint lid = gl_LocalInvocationID.x;
	uint val = gid < SORT_BINS ? histogram.data[gid] : 0u;
	s_scan[lid] = val;
	memoryBarrierShared();
	barrier();

	// Up-sweep (reduction).
	for (uint stride = 1u; stride < SORT_BLOCK_SIZE; stride <<= 1u) {
		uint idx = (lid + 1u) * (stride << 1u) - 1u;
		if (idx < SORT_BLOCK_SIZE) {
			s_scan[idx] += s_scan[idx - stride];
		}
		memoryBarrierShared();
		barrier();
	}
	if (lid == 0u) {
		block_sums.data[gl_WorkGroupID.x] = s_scan[SORT_BLOCK_SIZE - 1u];
		s_scan[SORT_BLOCK_SIZE - 1u] = 0u;
	}
	memoryBarrierShared();
	barrier();

	// Down-sweep (exclusive scan).
	for (uint stride = SORT_BLOCK_SIZE >> 1u; stride > 0u; stride >>= 1u) {
		uint idx = (lid + 1u) * (stride << 1u) - 1u;
		if (idx < SORT_BLOCK_SIZE) {
			uint t = s_scan[idx - stride];
			s_scan[idx - stride] = s_scan[idx];
			s_scan[idx] += t;
		}
		memoryBarrierShared();
		barrier();
	}
	if (gid < SORT_BINS) {
		histogram.data[gid] = s_scan[lid];
	}
#elif defined(MODE_SCAN_BLOCKS)
	uint lid = gl_LocalInvocationID.x;
	s_scan[lid] = lid < SORT_BLOCKS ? block_sums.data[lid] : 0u;
	memoryBarrierShared();
	barrier();

	for (uint stride = 1u; stride < SORT_BLOCK_SIZE; stride <<= 1u) {
		uint idx = (lid + 1u) * (stride << 1u) - 1u;
		if (idx < SORT_BLOCK_SIZE) {
			s_scan[idx] += s_scan[idx - stride];
		}
		memoryBarrierShared();
		barrier();
	}
	if (lid == 0u) {
		s_scan[SORT_BLOCK_SIZE - 1u] = 0u;
	}
	memoryBarrierShared();
	barrier();

	for (uint stride = SORT_BLOCK_SIZE >> 1u; stride > 0u; stride >>= 1u) {
		uint idx = (lid + 1u) * (stride << 1u) - 1u;
		if (idx < SORT_BLOCK_SIZE) {
			uint t = s_scan[idx - stride];
			s_scan[idx - stride] = s_scan[idx];
			s_scan[idx] += t;
		}
		memoryBarrierShared();
		barrier();
	}
	if (lid < SORT_BLOCKS) {
		block_sums.data[lid] = s_scan[lid];
	}
#elif defined(MODE_SCAN_ADD)
	if (gid < SORT_BINS) {
		uint val = histogram.data[gid] + block_sums.data[gl_WorkGroupID.x];
		histogram.data[gid] = val;
		// Bin NEXTGI_HL_KEY_GI_BASE (0x8000) holds the exact exclusive prefix sum of all reflection keys
		// [0x0000..0x7FFF], giving the split point between reflection hits and diffuse GI hits (#128).
		if (gid == NEXTGI_HL_KEY_GI_BASE) {
			uint refl_count = min(val, active_count);
			uint gi_count = active_count - refl_count;
			counters.data.refl_sorted_hits = refl_count;
			counters.data.gi_sorted_hits = gi_count;
			indirect_cmds.data[0] = refl_count;
			indirect_cmds.data[1] = refl_count > 0u ? 1u : 0u;
			indirect_cmds.data[2] = refl_count > 0u ? 1u : 0u;
			indirect_cmds.data[3] = 0u;
			indirect_cmds.data[8] = gi_count;
			indirect_cmds.data[9] = gi_count > 0u ? 1u : 0u;
			indirect_cmds.data[10] = gi_count > 0u ? 1u : 0u;
			indirect_cmds.data[11] = 0u;
		}
	}
#elif defined(MODE_SCATTER)
	if (gid >= active_count) {
		return;
	}
	uint key = nextgi_hl_record_material_key(hit_records.data[gid]);
	uint dst = atomicAdd(histogram.data[key], 1u);
	if (dst < active_count) {
		sorted_indices.data[dst] = gid;
	} else if (dst == 0xFFFFFFFFu) {
		counters.data.sink = dst;
	}
#endif
}
