#[compute]

#version 450
#extension GL_EXT_samplerless_texture_functions : enable

#VERSION_DEFINES

// GodotNext: nextgeo_shading_bins (#158, plan §21.4 N7, N3).
// Per-pixel material shading bins over RB_TEX_NEXTGEO_VISIBILITY, in four passes:
// - CLEAR: one invocation per bin zeroes its state and its indirect arguments.
// - COUNT: one 8x8 workgroup per tile; each pixel finds its bucket in the bucket table (as
//   resolve_classify.glsl does) and the bucket's bin in the per-bucket bin words, the tile counts its
//   pixels per bin in shared memory, and the first pixel of each bin in the tile adds the tile's count to
//   the bin's global count.
// - PREFIX: one workgroup takes the exclusive prefix sum of the counts (each bin's offset in the pixel
//   list) and writes each bin's indirect arguments, ceil(count / 64) workgroups folded over x and y.
// - SCATTER: the COUNT tiles again; the first pixel of each bin in the tile reserves the tile's run in
//   the bin, and every pixel writes its packed 16+16-bit coordinate at offset + run + rank in tile.
// A pixel outside the render region, with no visibility ID, or whose surface keeps the tile quads has
// no bin. The CPU reference is nextgeo_shading_bins_build() in nextgeo_shading_bins.h.

// clang-format off
#include "nextgeo_records_inc.glsl"
#include "shading_bins_inc.glsl"
// clang-format on

#if defined(MODE_CLEAR) || defined(MODE_PREFIX)
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
#else
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
#endif

layout(push_constant, std430) uniform PushParams {
	uvec2 viewport_size;
	uint bin_count;
	uint pixel_capacity;
	uvec2 region_min;
	uvec2 region_max;
}
params;

layout(set = 0, binding = 0) uniform utexture2D visibility_tex;

layout(set = 0, binding = 1, std430) buffer restrict readonly VisibleClusters {
	uvec2 data[];
}
visible_clusters;

layout(set = 0, binding = 2, std430) buffer restrict readonly Instances {
	NextGeoInstanceRecord data[];
}
records;

layout(set = 0, binding = 3, std430) buffer restrict readonly Meshes {
	NextGeoMeshRecord data[];
}
meshes;

layout(set = 0, binding = 4, std430) buffer restrict readonly ClusterTable {
	uvec4 data[];
}
cluster_table;

// Per bucket: the bucket's bin, or NEXTGEO_SHADING_BIN_NONE when it keeps the tile quads.
layout(set = 0, binding = 5, std430) buffer restrict readonly BucketBins {
	uint data[];
}
bucket_bins;

layout(set = 0, binding = 6, std430) buffer restrict BinState {
	uint data[];
}
bin_state;

layout(set = 0, binding = 7, std430) buffer restrict IndirectArgs {
	uvec4 data[];
}
indirect_args;

layout(set = 0, binding = 8, std430) buffer restrict writeonly PixelList {
	uint data[];
}
pixel_list;

// The per-(instance, surface) bucket words of the tile quads (NextGeoResolve::get_bucket_table_buffer()).
layout(set = 0, binding = 9, std430) buffer restrict readonly BucketTable {
	uint data[];
}
bucket_table;

#if defined(MODE_COUNT) || defined(MODE_SCATTER)
// The pixel's shading bin, or NEXTGEO_SHADING_BIN_NONE. The visibility-ID decode of resolve_classify.glsl.
uint pixel_bin(ivec2 p_px) {
	if (p_px.x >= int(params.viewport_size.x) || p_px.y >= int(params.viewport_size.y)) {
		return NEXTGEO_SHADING_BIN_NONE;
	}
	if (any(lessThan(uvec2(p_px), params.region_min)) || any(greaterThanEqual(uvec2(p_px), params.region_max))) {
		return NEXTGEO_SHADING_BIN_NONE;
	}
	uint vis_id = texelFetch(visibility_tex, p_px, 0).r;
	if (vis_id == 0xFFFFFFFFu) {
		return NEXTGEO_SHADING_BIN_NONE;
	}
	uvec2 entry = visible_clusters.data[vis_id >> 7u];
	NextGeoInstanceRecord record = records.data[entry.x];
	if ((record.flags & NEXTGEO_RECORD_VALID) == 0u) {
		return NEXTGEO_SHADING_BIN_NONE;
	}
	NextGeoMeshRecord mesh = meshes.data[record.virtual_mesh];
	if (mesh.page_count == 0u || entry.y < mesh.cluster_offset) {
		return NEXTGEO_SHADING_BIN_NONE;
	}
	uint local_cluster = entry.y - mesh.cluster_offset;
	if (local_cluster >= mesh.cluster_count) {
		return NEXTGEO_SHADING_BIN_NONE;
	}
	uvec4 desc = cluster_table.data[mesh.page_table_offset + local_cluster];
	uint surface_id = desc.w & 0xFFFFu;
	if (surface_id >= 32u || (record.surface_mask & (1u << surface_id)) == 0u) {
		return NEXTGEO_SHADING_BIN_NONE;
	}
	uint bucket = bucket_table.data[record.bucket_base + surface_id] & NEXTGEO_BUCKET_ID_MASK;
	if (bucket >= params.bin_count) {
		return NEXTGEO_SHADING_BIN_NONE;
	}
	uint bin = bucket_bins.data[bucket];
	return bin < params.bin_count ? bin : NEXTGEO_SHADING_BIN_NONE;
}

shared uint tile_bins[64];
#endif

#ifdef MODE_SCATTER
shared uint tile_runs[64];
#endif

#ifdef MODE_PREFIX
shared uint scan[64];
#endif

void main() {
#ifdef MODE_CLEAR
	uint bin = gl_GlobalInvocationID.x;
	if (bin < params.bin_count) {
		bin_state.data[bin * NEXTGEO_SHADING_BIN_STATE_WORDS + 0u] = 0u;
		bin_state.data[bin * NEXTGEO_SHADING_BIN_STATE_WORDS + 1u] = 0u;
		bin_state.data[bin * NEXTGEO_SHADING_BIN_STATE_WORDS + 2u] = 0u;
		bin_state.data[bin * NEXTGEO_SHADING_BIN_STATE_WORDS + 3u] = 0u;
		indirect_args.data[bin] = uvec4(0u, 0u, 1u, 0u);
	}
#endif

#if defined(MODE_COUNT) || defined(MODE_SCATTER)
	uint tid = gl_LocalInvocationIndex;
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	uint my_bin = pixel_bin(px);
	tile_bins[tid] = my_bin;
	barrier();

	// This pixel's rank among the tile's pixels of its bin, the tile's count of the bin, and the first
	// pixel of the bin in the tile (the one that talks to the global state).
	uint rank = 0u;
	uint total = 0u;
	uint first = tid;
	if (my_bin != NEXTGEO_SHADING_BIN_NONE) {
		for (uint i = 0u; i < 64u; i++) {
			if (tile_bins[i] == my_bin) {
				if (i < tid) {
					rank++;
					first = min(first, i);
				}
				total++;
			}
		}
	}

#ifdef MODE_COUNT
	if (my_bin != NEXTGEO_SHADING_BIN_NONE && rank == 0u) {
		atomicAdd(bin_state.data[my_bin * NEXTGEO_SHADING_BIN_STATE_WORDS + NEXTGEO_SHADING_BIN_STATE_COUNT], total);
	}
#else
	if (my_bin != NEXTGEO_SHADING_BIN_NONE && rank == 0u) {
		tile_runs[tid] = atomicAdd(bin_state.data[my_bin * NEXTGEO_SHADING_BIN_STATE_WORDS + NEXTGEO_SHADING_BIN_STATE_CURSOR], total);
	}
	barrier();
	if (my_bin != NEXTGEO_SHADING_BIN_NONE) {
		uint dst = bin_state.data[my_bin * NEXTGEO_SHADING_BIN_STATE_WORDS + NEXTGEO_SHADING_BIN_STATE_OFFSET] + tile_runs[first] + rank;
		if (dst < params.pixel_capacity) {
			pixel_list.data[dst] = nextgeo_shading_pack_pixel(uvec2(px));
		}
	}
#endif
#endif

#ifdef MODE_PREFIX
	// One workgroup, 64 bins per step: an inclusive scan in shared memory, plus the running total.
	uint tid = gl_LocalInvocationIndex;
	uint running = 0u;
	for (uint base = 0u; base < params.bin_count; base += 64u) {
		uint bin = base + tid;
		uint count = bin < params.bin_count ? bin_state.data[bin * NEXTGEO_SHADING_BIN_STATE_WORDS + NEXTGEO_SHADING_BIN_STATE_COUNT] : 0u;
		scan[tid] = count;
		barrier();
		for (uint offset = 1u; offset < 64u; offset <<= 1u) {
			uint add = tid >= offset ? scan[tid - offset] : 0u;
			barrier();
			scan[tid] += add;
			barrier();
		}
		if (bin < params.bin_count) {
			bin_state.data[bin * NEXTGEO_SHADING_BIN_STATE_WORDS + NEXTGEO_SHADING_BIN_STATE_OFFSET] = running + scan[tid] - count;
			bin_state.data[bin * NEXTGEO_SHADING_BIN_STATE_WORDS + NEXTGEO_SHADING_BIN_STATE_CURSOR] = 0u;
			indirect_args.data[bin] = nextgeo_shading_dispatch_args(count);
		}
		running += scan[63];
		barrier();
	}
#endif
}
