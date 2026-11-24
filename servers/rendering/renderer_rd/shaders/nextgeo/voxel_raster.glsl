#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: nextgeo_voxels (#165, plan §21.4 contract N10, GN-7, GN-12, N6, MN3).
// Software-only compute splat of sparse 4^3 (64-voxel) Nanite Voxel bricks into the 64-bit
// visibility buffer (`vis64`, image or VIS64_BUFFER SSBO form).
// One 64-thread workgroup per visible brick: thread `gl_LocalInvocationIndex` (0..63) owns
// voxel `(x, y, z)`, projects it, splats a <= 2x2 px footprint with frame-stable stochastic
// coverage hash, and writes atomicMax(vis64, depth << 32 | voxel_vis_id).

#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#ifdef VIS64_BUFFER
#extension GL_EXT_shader_atomic_int64 : require
#else
#extension GL_EXT_shader_image_int64 : require
#endif

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// clang-format off
#include "nextgeo_records_inc.glsl"
#include "raster_features_inc.glsl"
#include "sw_raster_setup_inc.glsl"
#include "voxel_inc.glsl"
// clang-format on

layout(set = 0, binding = 0, std430) buffer restrict readonly Instances {
	NextGeoInstanceRecord data[];
}
instances;

layout(set = 0, binding = 1, std430) buffer restrict readonly Pages {
	uint data[];
}
pages;

layout(set = 0, binding = 2, std430) buffer restrict readonly VisibleVoxelBricks {
	uvec4 data[];
}
visible_voxel_bricks;

layout(set = 0, binding = 3, std430) buffer restrict readonly RasterParams {
	mat4 proj;
	mat4 view;
	mat4 prev_proj;
	mat4 prev_view;
	vec2 taa_jitter;
	vec2 prev_taa_jitter;
	uint visible_capacity;
	uint reset_velocity;
	float cull_sign;
	uint bucket_table_words;
}
raster;

layout(set = 0, binding = 4, std430) buffer restrict readonly BucketTable {
	uint data[];
}
bucket_table;

layout(set = 0, binding = 5, std430) buffer restrict Counters {
	uint data[];
}
counters;

#ifdef VIS64_BUFFER
layout(set = 0, binding = 6, std430) buffer restrict Vis64 {
	uint64_t data[];
}
vis64;
#else
layout(set = 0, binding = 6, r64ui) uniform restrict u64image2D vis64;
#endif

const uint NEXTGEO_COUNTER_RASTER_SW_WRITES = 35u;
const uint NEXTGEO_COUNTER_VOXEL_BRICKS = 46u; // voxel_bricks[2] at words 46..47
const uint VOXEL_FLAG_STATS = 1u << 1u;

layout(push_constant, std430) uniform Params {
	uint list_base; // First entry of the phase's region in visible_voxel_bricks.
	uint phase;
	uint flags;
	uint phase_capacity;
	ivec2 size;
	uvec2 pad2;
}
params;

shared uint s_writes;

// Never read the return value of 64-bit atomicMax (Metal compatibility, #175).
void vis64_max(ivec2 p_pixel, uint64_t p_value) {
#ifdef VIS64_BUFFER
	atomicMax(vis64.data[uint(p_pixel.y) * uint(params.size.x) + uint(p_pixel.x)], p_value);
#else
	imageAtomicMax(vis64, p_pixel, p_value);
#endif
}

void main() {
	uint local_idx = gl_WorkGroupID.y * NEXTGEO_SW_DISPATCH_WIDTH + gl_WorkGroupID.x;
	uint phase_count = min(counters.data[NEXTGEO_COUNTER_VOXEL_BRICKS + params.phase], params.phase_capacity);
	if (local_idx >= phase_count) {
		return;
	}

	uint global_brick_idx = params.list_base + local_idx;
	if (global_brick_idx >= NEXTGEO_VIS_VOXEL_MAX_COUNT) {
		return;
	}

	uvec4 brick_entry = visible_voxel_bricks.data[global_brick_idx];
	uint record_idx = brick_entry.x;
	uint brick_word = brick_entry.y;

	NextGeoInstanceRecord record = instances.data[record_idx];
	if ((record.flags & NEXTGEO_RECORD_VALID) == 0u) {
		return;
	}

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

		bool surface_ok = coverage_u8 > 0u && surface_id < 32u && (record.surface_mask & (1u << surface_id)) != 0u;
		if (surface_ok && raster.bucket_table_words > 0u) {
			uint bucket_slot = record.bucket_base + surface_id;
			if (bucket_slot >= raster.bucket_table_words || (bucket_table.data[bucket_slot] & NEXTGEO_BUCKET_ID_MASK) == NEXTGEO_BUCKET_INVALID) {
				surface_ok = false;
			}
		}

		if (surface_ok) {
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
			vec4 clip = raster.proj * (raster.view * vec4(world_pos, 1.0));
			if (clip.w > 0.0) {
				float depth = clip.z / clip.w;
				if (depth > 0.0 && depth <= 1.0) {
					vec2 px_center = nextgeo_sw_clip_to_pixel(clip, params.size).xy;
					float inst_scale = max(record.world_aabb_min.w, 1e-6);
					bool is_ortho = abs(raster.proj[2][3]) < 1e-4;
					float px_scale = 0.5 * float(params.size.y) * abs(raster.proj[1][1]);
					float diam_px = voxel_size * inst_scale * px_scale / (is_ortho ? 1.0 : max(clip.w, 1e-4));

					// Clamp splat strictly to <= 2x2 pixels.
					ivec2 p0;
					ivec2 p1;
					if (diam_px <= 1.0) {
						p0 = ivec2(floor(px_center));
						p1 = p0;
					} else {
						p0 = ivec2(floor(px_center - 0.5));
						p1 = p0 + ivec2(1, 1);
					}
					p0 = max(p0, ivec2(0));
					p1 = min(p1, params.size - ivec2(1));

					if (p0.x <= p1.x && p0.y <= p1.y) {
						uint vis_id = nextgeo_vis_pack_voxel_id(global_brick_idx, v);
						uint64_t packed = (uint64_t(floatBitsToUint(min(depth, 1.0))) << 32u) | uint64_t(vis_id);
						for (int py = p0.y; py <= p1.y; py++) {
							for (int px = p0.x; px <= p1.x; px++) {
								if (nextgeo_voxel_coverage_pass(ivec2(px, py), record_idx, brick_word, v, coverage_u8)) {
									vis64_max(ivec2(px, py), packed);
									writes++;
								}
							}
						}
					}
				}
			}
		}
	}

	if ((params.flags & VOXEL_FLAG_STATS) != 0u) {
		if (writes > 0u) {
			atomicAdd(s_writes, writes);
		}
		barrier();
		if (gl_LocalInvocationIndex == 0u && s_writes > 0u) {
			atomicAdd(counters.data[NEXTGEO_COUNTER_RASTER_SW_WRITES], s_writes);
		}
	}
}
