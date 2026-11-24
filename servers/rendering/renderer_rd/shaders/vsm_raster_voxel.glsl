#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: nextgeo_voxels (#165, plan §21.4 contract N10) + vsm_nanite (#217).
// Depth-only software voxel brick rasterizer into dirty Virtual Shadow Map physical pages:
// one 64-thread workgroup per visible (voxel brick, dirty page) pair, <= 2x2 texel splat with
// deterministic frame-stable stochastic coverage hash and 32-bit imageAtomicMax.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// clang-format off
#include "vsm_mark_inc.glsl"
#include "vsm_raster_inc.glsl"
#include "nextgeo/nextgeo_records_inc.glsl"
#include "nextgeo/raster_features_inc.glsl"
#include "nextgeo/voxel_inc.glsl"
// clang-format on

layout(set = 0, binding = 0, std430) buffer restrict readonly Instances {
	NextGeoInstanceRecord data[];
}
instances;

layout(set = 0, binding = 1, std430) buffer restrict readonly Pages {
	uint data[];
}
pages;

// x = record_idx, y = brick_word_offset, z = dirty_idx, w = layer (bit 0).
layout(set = 0, binding = 2, std430) buffer restrict readonly VSMVoxelBricks {
	uvec4 data[];
}
vsm_voxel_bricks;

layout(set = 0, binding = 3, std430) buffer restrict readonly VSMMaps {
	VSMMapData data[];
}
vsm_maps;

layout(set = 0, binding = 4, std430) buffer restrict readonly VSMDirty {
	uvec4 data[];
}
vsm_dirty;

layout(r32ui, set = 0, binding = 5) uniform restrict uimage2DArray vsm_pool;

layout(set = 0, binding = 6, std430) buffer restrict VSMCounters {
	uint data[];
}
vsm_counters;

layout(push_constant, std430) uniform PushParams {
	uint brick_capacity;
	uint flags; // bit 0 = count voxel writes
	uint pad0;
	uint pad1;
}
push_params;

const uint NEXTGEO_VSM_COUNTER_SINK = 21u;
const uint NEXTGEO_VSM_COUNTER_VOXEL_BRICKS = 29u;
const uint NEXTGEO_VSM_COUNTER_VOXEL_WRITES = 30u;

shared uint s_writes;

void main() {
	uint local_idx = gl_WorkGroupID.y * NEXTGEO_SW_DISPATCH_WIDTH + gl_WorkGroupID.x;
	uint brick_count = min(vsm_counters.data[NEXTGEO_VSM_COUNTER_VOXEL_BRICKS], push_params.brick_capacity);
	if (local_idx >= brick_count) {
		return;
	}

	uvec4 pair = vsm_voxel_bricks.data[local_idx];
	uint record_idx = pair.x;
	uint brick_word = pair.y;
	uint dirty_idx = pair.z;
	int layer = int(pair.w & 1u);

	NextGeoInstanceRecord record = instances.data[record_idx];
	if ((record.flags & NEXTGEO_RECORD_VALID) == 0u || (record.flags & NEXTGEO_RECORD_CAST_SHADOW_MASK) == NEXTGEO_RECORD_CAST_SHADOW_OFF) {
		return;
	}

	uvec4 rec = vsm_dirty.data[dirty_idx];
	uint map = rec.x;
	uint mip = rec.y & 0xFFu;
	uvec2 slot = uvec2((rec.y >> 8u) & 0xFFu, (rec.y >> 16u) & 0xFFu);
	uvec2 phys_page = vsm_raster_entry_page(rec.z);
	ivec2 phys_origin = ivec2(phys_page * VSM_PAGE_SIZE);
	VSMMapData md = vsm_maps.data[map];
	bool is_directional = md.kind == VSM_MAP_KIND_DIRECTIONAL;

	if (gl_LocalInvocationIndex == 0u) {
		s_writes = 0u;
	}
	barrier();

	uint v = gl_LocalInvocationIndex; // 0..63
	uint occ_lo = pages.data[brick_word + 4u];
	uint occ_hi = pages.data[brick_word + 5u];
	uint writes = 0u;

	if (nextgeo_voxel_is_occupied(occ_lo, occ_hi, v)) {
		uint rank = nextgeo_voxel_rank(occ_lo, occ_hi, v);
		uint vword = brick_word + NEXTGEO_VOXEL_BRICK_HEADER_WORDS + rank * NEXTGEO_VOXEL_DATA_WORDS;
		uint w1 = pages.data[vword + 1u];
		uint coverage_u8 = (w1 >> 16u) & 0xFFu;
		uint surface_id = (w1 >> 24u) & 0xFFu;

		if (coverage_u8 > 0u && surface_id < 32u && (record.surface_mask & (1u << surface_id)) != 0u) {
			vec3 origin = vec3(
					uintBitsToFloat(pages.data[brick_word + 0u]),
					uintBitsToFloat(pages.data[brick_word + 1u]),
					uintBitsToFloat(pages.data[brick_word + 2u]));
			float voxel_size = uintBitsToFloat(pages.data[brick_word + 3u]);

			uvec3 vc = nextgeo_voxel_coord(v);
			vec3 local_pos = origin + (vec3(vc) + 0.5) * voxel_size;
			vec3 world_pos = vec3(
					dot(record.transform[0].xyz, local_pos) + record.transform[0].w,
					dot(record.transform[1].xyz, local_pos) + record.transform[1].w,
					dot(record.transform[2].xyz, local_pos) + record.transform[2].w);

			float depth = 0.0;
			vec4 clip = vsm_page_clip_position(md, mip, slot, world_pos, depth);
			bool valid = false;
			vec2 px_center = vec2(0.0);
			float texel_size = md.texel_scale * float(1u << mip);
			float inst_scale = max(record.world_aabb_min.w, 1e-6);
			float diam_tx = 1.0;

			if (is_directional) {
				px_center = (clip.xy * 0.5 + 0.5) * float(VSM_PAGE_SIZE);
				diam_tx = texel_size > 0.0 ? (voxel_size * inst_scale / texel_size) : 1.0;
				valid = true;
			} else if (clip.w > 0.0 && depth >= md.near_w && depth <= md.max_distance) {
				float inv_w = 1.0 / clip.w;
				px_center = (clip.xy * inv_w * 0.5 + 0.5) * float(VSM_PAGE_SIZE);
				diam_tx = texel_size > 0.0 ? (voxel_size * inst_scale / max(texel_size * clip.w, 1e-6)) : 1.0;
				valid = true;
			}

			if (valid) {
				ivec2 p0;
				ivec2 p1;
				if (diam_tx <= 1.0) {
					p0 = ivec2(floor(px_center));
					p1 = p0;
				} else {
					p0 = ivec2(floor(px_center - 0.5));
					p1 = p0 + ivec2(1, 1);
				}
				ivec2 page_max = ivec2(int(VSM_PAGE_SIZE) - 1);
				p0 = max(p0, ivec2(0));
				p1 = min(p1, page_max);

				if (p0.x <= p1.x && p0.y <= p1.y) {
					uint packed_depth = vsm_depth_pack(depth);
					if (packed_depth != 0u) {
						for (int py = p0.y; py <= p1.y; py++) {
							for (int px = p0.x; px <= p1.x; px++) {
								if (nextgeo_voxel_coverage_pass(ivec2(px, py), record_idx, brick_word, v, coverage_u8)) {
									// #277: every atomic's result is used.
									if (imageAtomicMax(vsm_pool, ivec3(phys_origin + ivec2(px, py), layer), packed_depth) == 0xFFFFFFFFu) {
										vsm_counters.data[NEXTGEO_VSM_COUNTER_SINK] = 1u;
									}
									writes++;
								}
							}
						}
					}
				}
			}
		}
	}

	if ((push_params.flags & 1u) != 0u) {
		if (writes > 0u) {
			if (atomicAdd(s_writes, writes) == 0xFFFFFFFFu) {
				vsm_counters.data[NEXTGEO_VSM_COUNTER_SINK] = 1u;
			}
		}
		barrier();
		if (gl_LocalInvocationIndex == 0u && s_writes > 0u) {
			if (atomicAdd(vsm_counters.data[NEXTGEO_VSM_COUNTER_VOXEL_WRITES], s_writes) == 0xFFFFFFFFu) {
				vsm_counters.data[NEXTGEO_VSM_COUNTER_SINK] = 1u;
			}
		}
	}
}
