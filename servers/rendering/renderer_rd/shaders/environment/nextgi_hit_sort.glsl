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

layout(set = 0, binding = 0, std430) restrict readonly buffer SortHitRecords {
	NextGIHitLightRecord data[];
}
hit_records;

layout(set = 0, binding = 1, std430) restrict buffer SortHistogram {
	uint data[];
}
histogram;

layout(set = 0, binding = 2, std430) restrict buffer SortBlockSums {
	uint data[];
}
block_sums;

layout(set = 0, binding = 3, std430) restrict buffer SortSortedIndices {
	uint data[];
}
sorted_indices;

layout(set = 0, binding = 4, std430) restrict buffer SortCounters {
	NextGIHitLightCounters data;
}
counters;

// Indirect commands: [0..2] = RT raygen indirect dispatch (active_count, 1, 1);
//                    [4..6] = compute sort indirect dispatch ((active_count + 255) / 256, 1, 1).
layout(set = 0, binding = 5, std430) restrict buffer SortIndirect {
	uint data[];
}
indirect_cmds;

layout(push_constant, std430) uniform SortPush {
	uint max_rays;
	uint pad0;
	uint pad1;
	uint pad2;
}
sort_push;

#if defined(MODE_SCAN_LOCAL) || defined(MODE_SCAN_BLOCKS)
shared uint s_scan[SORT_BLOCK_SIZE];
#endif

void main() {
	uint gid = gl_GlobalInvocationID.x;
	uint active_count = min(counters.data.marked_hits, sort_push.max_rays);

#if defined(MODE_CLEAR)
	if (gid < SORT_BINS) {
		histogram.data[gid] = 0u;
	}
	if (gid == 0u) {
		counters.data.sorted_hits = active_count;
		indirect_cmds.data[0] = active_count;
		indirect_cmds.data[1] = active_count > 0u ? 1u : 0u;
		indirect_cmds.data[2] = active_count > 0u ? 1u : 0u;
		indirect_cmds.data[3] = 0u;
		uint groups = active_count > 0u ? (active_count + 255u) / 256u : 0u;
		indirect_cmds.data[4] = groups;
		indirect_cmds.data[5] = groups > 0u ? 1u : 0u;
		indirect_cmds.data[6] = groups > 0u ? 1u : 0u;
		indirect_cmds.data[7] = 0u;
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
	barrier();

	// Up-sweep (reduction).
	for (uint stride = 1u; stride < SORT_BLOCK_SIZE; stride <<= 1u) {
		uint idx = (lid + 1u) * (stride << 1u) - 1u;
		if (idx < SORT_BLOCK_SIZE) {
			s_scan[idx] += s_scan[idx - stride];
		}
		barrier();
	}
	if (lid == 0u) {
		block_sums.data[gl_WorkGroupID.x] = s_scan[SORT_BLOCK_SIZE - 1u];
		s_scan[SORT_BLOCK_SIZE - 1u] = 0u;
	}
	barrier();

	// Down-sweep (exclusive scan).
	for (uint stride = SORT_BLOCK_SIZE >> 1u; stride > 0u; stride >>= 1u) {
		uint idx = (lid + 1u) * (stride << 1u) - 1u;
		if (idx < SORT_BLOCK_SIZE) {
			uint t = s_scan[idx - stride];
			s_scan[idx - stride] = s_scan[idx];
			s_scan[idx] += t;
		}
		barrier();
	}
	if (gid < SORT_BINS) {
		histogram.data[gid] = s_scan[lid];
	}
#elif defined(MODE_SCAN_BLOCKS)
	uint lid = gl_LocalInvocationID.x;
	s_scan[lid] = lid < SORT_BLOCKS ? block_sums.data[lid] : 0u;
	barrier();

	for (uint stride = 1u; stride < SORT_BLOCK_SIZE; stride <<= 1u) {
		uint idx = (lid + 1u) * (stride << 1u) - 1u;
		if (idx < SORT_BLOCK_SIZE) {
			s_scan[idx] += s_scan[idx - stride];
		}
		barrier();
	}
	if (lid == 0u) {
		s_scan[SORT_BLOCK_SIZE - 1u] = 0u;
	}
	barrier();

	for (uint stride = SORT_BLOCK_SIZE >> 1u; stride > 0u; stride >>= 1u) {
		uint idx = (lid + 1u) * (stride << 1u) - 1u;
		if (idx < SORT_BLOCK_SIZE) {
			uint t = s_scan[idx - stride];
			s_scan[idx - stride] = s_scan[idx];
			s_scan[idx] += t;
		}
		barrier();
	}
	if (lid < SORT_BLOCKS) {
		block_sums.data[lid] = s_scan[lid];
	}
#elif defined(MODE_SCAN_ADD)
	if (gid < SORT_BINS) {
		histogram.data[gid] += block_sums.data[gl_WorkGroupID.x];
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
