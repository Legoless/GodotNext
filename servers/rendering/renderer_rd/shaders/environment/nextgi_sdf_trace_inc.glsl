// GodotNext: ps_swrt_trace (#186). Included by the software gather and shadow
// shaders. Not a standalone shader. The includer defines EXPLICIT_FP16 and
// includes half_inc.glsl first. This file declares the buffers the march
// reads, then the voxel-lighting include.

#ifndef NEXTGI_SDF_TRACE_INC_GLSL
#define NEXTGI_SDF_TRACE_INC_GLSL

struct SWRTMeshInfo {
	vec4 bounds_voxel;
	ivec4 dimensions_flags;
	ivec4 brick_grid_band_bits;
	uvec4 offsets;
};

struct SWRTInstance {
	vec4 inverse_row0;
	vec4 inverse_row1;
	vec4 inverse_row2;
	vec4 world_min;
	vec4 world_max;
	uvec4 flags;
};

struct SWRTCard {
	vec4 origin_half_u;
	vec4 axis_u_half_v;
	vec4 axis_v_texel;
	vec4 axis_n_instance;
	vec4 radiance;
};

struct SWRTParams {
	mat4 inv_view_proj;
	vec4 camera;
	vec4 screen;
	vec4 trace;
	vec4 sky;
	vec4 wrc;
	ivec4 origins[6];
	vec4 voxel_size0;
	vec4 voxel_size1;
	ivec4 clipmap[4];
	vec4 froxel;
	vec4 light_dir;
	vec4 grid;
	vec4 extra;
};

layout(set = 0, binding = 0, std430) buffer SWRTCounters {
	uint steps;
	uint hits;
	uint misses;
	uint shadow_rays;
}
swrt_counters;

layout(set = 0, binding = 1, std430) readonly buffer SWRTParamsBuffer {
	SWRTParams data;
}
swrt_params_buffer;

layout(set = 0, binding = 2, std430) readonly buffer SWRTStaticPool {
	float16_t data[];
}
swrt_static_pool;

layout(set = 0, binding = 3, std430) readonly buffer SWRTDynamicPool {
	float16_t data[];
}
swrt_dynamic_pool;

layout(set = 0, binding = 4, std430) readonly buffer SWRTCoverage {
	uint data[];
}
swrt_coverage;

layout(set = 0, binding = 5, std430) readonly buffer SWRTPage0 {
	int data[];
}
swrt_page0;
layout(set = 0, binding = 6, std430) readonly buffer SWRTPage1 {
	int data[];
}
swrt_page1;
layout(set = 0, binding = 7, std430) readonly buffer SWRTPage2 {
	int data[];
}
swrt_page2;
layout(set = 0, binding = 8, std430) readonly buffer SWRTPage3 {
	int data[];
}
swrt_page3;
layout(set = 0, binding = 9, std430) readonly buffer SWRTPage4 {
	int data[];
}
swrt_page4;
layout(set = 0, binding = 10, std430) readonly buffer SWRTPage5 {
	int data[];
}
swrt_page5;

layout(set = 0, binding = 11, std430) readonly buffer SWRTCoarse0 {
	float data[];
}
swrt_coarse0;
layout(set = 0, binding = 12, std430) readonly buffer SWRTCoarse1 {
	float data[];
}
swrt_coarse1;
layout(set = 0, binding = 13, std430) readonly buffer SWRTCoarse2 {
	float data[];
}
swrt_coarse2;
layout(set = 0, binding = 14, std430) readonly buffer SWRTCoarse3 {
	float data[];
}
swrt_coarse3;
layout(set = 0, binding = 15, std430) readonly buffer SWRTCoarse4 {
	float data[];
}
swrt_coarse4;
layout(set = 0, binding = 16, std430) readonly buffer SWRTCoarse5 {
	float data[];
}
swrt_coarse5;

layout(set = 0, binding = 17, std430) readonly buffer NextGIVoxelRadiance {
	uint data[];
}
voxel_radiance;

layout(set = 0, binding = 18, std430) readonly buffer NextGIVoxelWeights {
	uint data[];
}
voxel_weights;

#include "nextgi_voxel_lighting_inc.glsl"

layout(set = 0, binding = 19) uniform sampler2D swrt_depth;

layout(set = 0, binding = 20, std430) readonly buffer SWRTInstances {
	SWRTInstance data[];
}
swrt_instances;

layout(set = 0, binding = 21, std430) readonly buffer SWRTMeshes {
	SWRTMeshInfo data[];
}
swrt_meshes;

layout(set = 0, binding = 22, std430) readonly buffer SWRTMeshPages {
	int data[];
}
swrt_mesh_pages;

layout(set = 0, binding = 23, std430) readonly buffer SWRTMeshBricks {
	uint data[];
}
swrt_mesh_bricks;

layout(set = 0, binding = 24, std430) readonly buffer SWRTFroxel {
	uint counts[64];
	uint indices[512];
}
swrt_froxel;

layout(set = 0, binding = 25, std430) readonly buffer SWRTCards {
	SWRTCard data[];
}
swrt_cards;

layout(set = 0, binding = 26, std430) buffer SWRTShadeOut {
	uint data[];
}
swrt_shade_out;

SWRTParams swrt_params() {
	return swrt_params_buffer.data;
}

float swrt_level_voxel(uint level) {
	SWRTParams params = swrt_params();
	if (level == 0u) {
		return params.voxel_size0.x;
	}
	if (level == 1u) {
		return params.voxel_size0.y;
	}
	if (level == 2u) {
		return params.voxel_size0.z;
	}
	if (level == 3u) {
		return params.voxel_size0.w;
	}
	if (level == 4u) {
		return params.voxel_size1.x;
	}
	return params.voxel_size1.y;
}

int swrt_page_slot(uint level, uint index) {
	if (level == 0u) {
		return swrt_page0.data[index];
	}
	if (level == 1u) {
		return swrt_page1.data[index];
	}
	if (level == 2u) {
		return swrt_page2.data[index];
	}
	if (level == 3u) {
		return swrt_page3.data[index];
	}
	if (level == 4u) {
		return swrt_page4.data[index];
	}
	return swrt_page5.data[index];
}

float swrt_coarse_at(uint level, uint index) {
	if (level == 0u) {
		return swrt_coarse0.data[index];
	}
	if (level == 1u) {
		return swrt_coarse1.data[index];
	}
	if (level == 2u) {
		return swrt_coarse2.data[index];
	}
	if (level == 3u) {
		return swrt_coarse3.data[index];
	}
	if (level == 4u) {
		return swrt_coarse4.data[index];
	}
	return swrt_coarse5.data[index];
}

uint swrt_level_for_distance(float distance_from_camera) {
	float half_extent = 128.0 * swrt_level_voxel(0u) * 0.5;
	if (!(distance_from_camera > half_extent)) {
		return 0u;
	}
	uint level = uint(ceil(log2(distance_from_camera / half_extent)));
	return min(level, 5u);
}

float swrt_sample_global(uint level, vec3 world, out bool coverage, out bool missing) {
	coverage = false;
	missing = true;
	float voxel_size = swrt_level_voxel(level);
	if (!(voxel_size > 0.0)) {
		return 65504.0;
	}
	ivec3 origin = swrt_params().origins[level].xyz;
	ivec3 voxel = ivec3(floor(world / voxel_size)) - origin;
	if (any(lessThan(voxel, ivec3(0))) || any(greaterThanEqual(voxel, ivec3(128)))) {
		return 65504.0;
	}
	ivec3 brick = voxel / 8;
	ivec3 local = voxel % 8;
	uint page_index = uint(brick.x + 16 * (brick.y + 16 * brick.z));
	int slot = swrt_page_slot(level, page_index);
	if (slot < 0 || slot >= 16384) {
		return 65504.0;
	}
	uint voxel_index = uint(local.x + 8 * (local.y + 8 * local.z));
	uint pool_index = uint(slot) * 512u + voxel_index;
	uint coverage_index = uint(slot) * 16u + voxel_index / 8u;
	float value = min(float(swrt_static_pool.data[pool_index]), float(swrt_dynamic_pool.data[pool_index]));
	uint coverage_word = swrt_coverage.data[coverage_index];
	coverage = (coverage_word & (1u << (voxel_index & 7u))) != 0u;
	missing = false;
	return value;
}

float swrt_mesh_voxel(SWRTMeshInfo mesh, ivec3 voxel) {
	ivec3 dimensions = max(mesh.dimensions_flags.xyz, ivec3(1));
	ivec3 clamped = clamp(voxel, ivec3(0), dimensions - ivec3(1));
	ivec3 brick_grid = max(mesh.brick_grid_band_bits.xyz, ivec3(1));
	ivec3 brick = clamped / 8;
	ivec3 local = clamped % 8;
	uint page_index = mesh.offsets.x + uint(brick.x + brick_grid.x * (brick.y + brick_grid.y * brick.z));
	int entry = swrt_mesh_pages.data[page_index];
	float band = uintBitsToFloat(mesh.offsets.w);
	if (entry == -1) {
		return band;
	}
	if (entry == -2) {
		return -band;
	}
	uint voxel_index = uint(local.x + 8 * (local.y + 8 * local.z));
	uint brick_index = mesh.offsets.y + uint(entry) * 128u + voxel_index / 4u;
	uint packed = swrt_mesh_bricks.data[brick_index];
	uint encoded = (packed >> ((voxel_index % 4u) * 8u)) & 255u;
	return (float(encoded) / 255.0 * 2.0 - 1.0) * band;
}

float swrt_sample_mesh(uint instance_index, vec3 world) {
	SWRTInstance instance = swrt_instances.data[instance_index];
	SWRTMeshInfo mesh = swrt_meshes.data[instance_index];
	vec3 local = vec3(
			dot(instance.inverse_row0.xyz, world) + instance.inverse_row0.w,
			dot(instance.inverse_row1.xyz, world) + instance.inverse_row1.w,
			dot(instance.inverse_row2.xyz, world) + instance.inverse_row2.w);
	float voxel_size = mesh.bounds_voxel.w;
	if (!(voxel_size > 0.0)) {
		return 65504.0;
	}
	vec3 f = (local - mesh.bounds_voxel.xyz) / voxel_size - vec3(0.5);
	ivec3 dimensions = max(mesh.dimensions_flags.xyz, ivec3(1));
	f = clamp(f, vec3(0.0), vec3(dimensions - ivec3(1)));
	ivec3 base = ivec3(floor(f));
	vec3 frac = f - vec3(base);
	float c000 = swrt_mesh_voxel(mesh, base);
	float c100 = swrt_mesh_voxel(mesh, base + ivec3(1, 0, 0));
	float c010 = swrt_mesh_voxel(mesh, base + ivec3(0, 1, 0));
	float c110 = swrt_mesh_voxel(mesh, base + ivec3(1, 1, 0));
	float c001 = swrt_mesh_voxel(mesh, base + ivec3(0, 0, 1));
	float c101 = swrt_mesh_voxel(mesh, base + ivec3(1, 0, 1));
	float c011 = swrt_mesh_voxel(mesh, base + ivec3(0, 1, 1));
	float c111 = swrt_mesh_voxel(mesh, base + ivec3(1, 1, 1));
	float x00 = mix(c000, c100, frac.x);
	float x10 = mix(c010, c110, frac.x);
	float x01 = mix(c001, c101, frac.x);
	float x11 = mix(c011, c111, frac.x);
	return mix(mix(x00, x10, frac.y), mix(x01, x11, frac.y), frac.z) * instance.world_min.w;
}

uint swrt_froxel_cell(vec3 world) {
	vec3 local = (world - swrt_params().froxel.xyz) / max(swrt_params().froxel.w, 0.001);
	if (any(lessThan(local, vec3(0.0))) || any(greaterThanEqual(local, vec3(4.0)))) {
		return 0xffffffffu;
	}
	ivec3 cell = ivec3(local);
	return uint(cell.x + 4 * (cell.y + 4 * cell.z));
}

vec3 swrt_normal_from_taps(vec3 world, uint level) {
	float step_size = swrt_level_voxel(level);
	bool unused_coverage;
	bool unused_missing;
	float taps_px = swrt_sample_global(level, world + vec3(step_size, 0.0, 0.0), unused_coverage, unused_missing);
	float taps_nx = swrt_sample_global(level, world - vec3(step_size, 0.0, 0.0), unused_coverage, unused_missing);
	float taps_py = swrt_sample_global(level, world + vec3(0.0, step_size, 0.0), unused_coverage, unused_missing);
	float taps_ny = swrt_sample_global(level, world - vec3(0.0, step_size, 0.0), unused_coverage, unused_missing);
	float taps_pz = swrt_sample_global(level, world + vec3(0.0, 0.0, step_size), unused_coverage, unused_missing);
	float taps_nz = swrt_sample_global(level, world - vec3(0.0, 0.0, step_size), unused_coverage, unused_missing);
	vec3 gradient = vec3(taps_px - taps_nx, taps_py - taps_ny, taps_pz - taps_nz);
	if (dot(gradient, gradient) <= 1e-8) {
		return vec3(0.0, 1.0, 0.0);
	}
	return normalize(gradient);
}

void swrt_march(vec3 ray_origin, vec3 ray_direction, float tmax, bool detail, out float hit_t, out vec3 hit_normal, out uint hit_flag, out uint hit_instance, out uint hit_steps) {
	hit_t = 0.0;
	hit_normal = -ray_direction;
	hit_flag = 0u;
	hit_instance = 0xffffffffu;
	hit_steps = 0u;
	float direction_length = length(ray_direction);
	if (!(direction_length > 1e-6) || !(tmax > 0.0)) {
		return;
	}
	vec3 direction = ray_direction / direction_length;
	float t = 0.0;
	SWRTParams params = swrt_params();
	uint instance_count = uint(max(params.extra.w, 0.0));
	while (hit_steps < 64u && t < tmax) {
		vec3 position = ray_origin + direction * t;
		if (detail && t < 2.0 && instance_count > 0u) {
			uint cell = swrt_froxel_cell(position);
			uint cell_count = cell == 0xffffffffu ? 0u : min(swrt_froxel.counts[cell], 8u);
			float best = 65504.0;
			uint best_instance = 0xffffffffu;
			for (uint i = 0u; i < cell_count; i++) {
				uint source = swrt_froxel.indices[cell * 8u + i];
				if (source >= instance_count) {
					continue;
				}
				float sampled = swrt_sample_mesh(source, position);
				if (sampled < best) {
					best = sampled;
					best_instance = source;
				}
			}
			hit_steps += 1u;
			float mesh_voxel = 0.1;
			if (best_instance != 0xffffffffu) {
				mesh_voxel = max(swrt_meshes.data[best_instance].bounds_voxel.w, 0.025);
			}
			if (best < 60000.0 && abs(best) <= 0.5 * mesh_voxel) {
				bool two_sided = best_instance != 0xffffffffu && (swrt_instances.data[best_instance].flags.x & 2u) != 0u;
				vec3 normal = -direction;
				if (!two_sided || dot(direction, normal) > 0.0) {
					hit_flag = 1u;
					hit_t = t;
					hit_normal = normal;
					hit_instance = best_instance;
					return;
				}
				t += mesh_voxel;
				continue;
			}
			float advance = best >= 60000.0 ? 0.5 : max(abs(best), 0.25 * mesh_voxel);
			t += min(advance, max(tmax - t, 0.0));
			continue;
		}
		uint level = swrt_level_for_distance(length(position - params.camera.xyz));
		float voxel_size = max(swrt_level_voxel(level), 0.025);
		bool coverage = false;
		bool missing = true;
		float sampled = swrt_sample_global(level, position, coverage, missing);
		float coarse_skip = 0.0;
		if (missing) {
			ivec3 coarse_voxel = ivec3(floor(position / (voxel_size * 4.0)));
			ivec3 coarse_origin = params.origins[level].xyz / 4;
			ivec3 coarse_local = coarse_voxel - coarse_origin;
			if (all(greaterThanEqual(coarse_local, ivec3(0))) && all(lessThan(coarse_local, ivec3(32)))) {
				uint coarse_index = uint(coarse_local.x + 32 * (coarse_local.y + 32 * coarse_local.z));
				float coarse = swrt_coarse_at(level, coarse_index);
				if (coarse > 4.0 * voxel_size && coarse < 60000.0) {
					coarse_skip = coarse - 2.0 * voxel_size;
				}
			}
		}
		hit_steps += 1u;
		if (!missing && abs(sampled) <= voxel_size) {
			vec3 normal = swrt_normal_from_taps(position, level);
			if (dot(normal, normal) <= 1e-8) {
				normal = -direction;
			}
			// Unsigned shells have no gradient on the surface. The instance
			// bounds center stands in for the outward normal.
			if (coverage) {
				vec3 outward = normal;
				float best_d = 1e20;
				bool found = false;
				uint count = min(instance_count, 64u);
				for (uint i = 0u; i < count; i++) {
					SWRTInstance inst = swrt_instances.data[i];
					if ((inst.flags.x & 2u) == 0u) {
						continue;
					}
					vec3 bmin = inst.world_min.xyz;
					vec3 bmax = inst.world_max.xyz;
					if (any(lessThan(position, bmin)) || any(greaterThan(position, bmax))) {
						continue;
					}
					vec3 center = (bmin + bmax) * 0.5;
					float d = dot(position - center, position - center);
					if (d < best_d) {
						best_d = d;
						vec3 away = position - center;
						if (dot(away, away) > 1e-8) {
							outward = normalize(away);
							found = true;
						}
					}
				}
				if (!found && dot(normal, direction) > 0.0) {
					outward = normal;
				}
				if (dot(direction, outward) <= 0.0) {
					t += voxel_size;
					continue;
				}
				normal = outward;
			}
			hit_flag = 1u;
			hit_t = t;
			hit_normal = normal;
			hit_instance = 0xffffffffu;
			return;
		}
		float advance = missing ? max(voxel_size, coarse_skip) : max(abs(sampled), 0.25 * voxel_size);
		t += min(max(advance, voxel_size * 0.25), max(tmax - t, 0.0));
	}
}

vec3 swrt_unproject(vec2 pixel) {
	SWRTParams params = swrt_params();
	vec2 screen = max(params.screen.xy, vec2(1.0));
	vec2 uv = (pixel + vec2(0.5)) / screen;
	float depth = texelFetch(swrt_depth, ivec2(pixel), 0).r;
	vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
	vec4 world_h = params.inv_view_proj * ndc;
	return world_h.xyz / max(abs(world_h.w), 1e-6) * sign(world_h.w);
}

vec3 swrt_geometric_normal(ivec2 pixel, vec3 center) {
	SWRTParams params = swrt_params();
	ivec2 size = max(ivec2(params.screen.xy), ivec2(1));
	ivec2 clamped = clamp(pixel, ivec2(0), size - ivec2(1));
	vec3 dx = swrt_unproject(vec2(clamp(clamped + ivec2(1, 0), ivec2(0), size - ivec2(1)))) - center;
	vec3 dy = swrt_unproject(vec2(clamp(clamped + ivec2(0, 1), ivec2(0), size - ivec2(1)))) - center;
	vec3 normal = cross(dx, dy);
	vec3 to_camera = params.camera.xyz - center;
	if (dot(normal, normal) <= 1e-8) {
		normal = to_camera;
	}
	if (dot(normal, to_camera) < 0.0) {
		normal = -normal;
	}
	if (dot(normal, normal) <= 1e-8) {
		return vec3(0.0, 1.0, 0.0);
	}
	return normalize(normal);
}

vec3 swrt_hemisphere(vec3 normal, uint seed) {
	float u = fract(float(seed) * 0.754877666);
	float v = fract(float(seed) * 0.569840296);
	float radius = sqrt(u);
	float theta = 6.2831853 * v;
	vec3 local = vec3(radius * cos(theta), radius * sin(theta), sqrt(max(1.0 - u, 0.0)));
	vec3 helper = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(helper, normal));
	vec3 bitangent = cross(normal, tangent);
	return normalize(tangent * local.x + bitangent * local.y + normal * local.z);
}

float swrt_handoff(vec3 world) {
	float dist = length(world - swrt_params().camera.xyz);
	float spacing = 1.0;
	if (dist > 32.0) {
		spacing = 8.0;
	} else if (dist > 16.0) {
		spacing = 4.0;
	} else if (dist > 8.0) {
		spacing = 2.0;
	}
	return 1.5 * spacing;
}

// Screen march is off until a Hi-Z texture is bound (extra.y). The loop is
// still compiled so the trace order stays screen, then SDF.
float swrt_screen_trace(vec3 origin, vec3 direction, vec2 pixel, float tmax) {
	if (swrt_params().extra.y < 0.5 || !(tmax > 0.0)) {
		return 0.0;
	}
	vec2 screen = max(swrt_params().screen.xy, vec2(1.0));
	vec3 end_point = origin + direction * tmax;
	vec4 end_clip = inverse(swrt_params().inv_view_proj) * vec4(end_point, 1.0);
	vec2 end_uv = (end_clip.xy / max(abs(end_clip.w), 1e-6) * sign(end_clip.w)) * 0.5 + 0.5;
	vec2 start_uv = (pixel + vec2(0.5)) / screen;
	for (uint step_index = 0u; step_index < 24u; step_index++) {
		float alpha = float(step_index + 1u) / 24.0;
		vec2 uv = mix(start_uv, end_uv, alpha);
		if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) {
			return 0.0;
		}
		ivec2 sample_pixel = ivec2(clamp(uv * screen, vec2(0.0), screen - vec2(1.0)));
		float stored = texelFetch(swrt_depth, sample_pixel, 0).r;
		float ray_depth = mix(texelFetch(swrt_depth, ivec2(pixel), 0).r, end_clip.z / max(abs(end_clip.w), 1e-6) * 0.5 + 0.5, alpha);
		if (stored > ray_depth && stored > 0.0) {
			return tmax * alpha;
		}
	}
	return 0.0;
}

vec3 swrt_shade(vec3 world, vec3 normal, uint detail_hit, uint instance_index, bool allow_wrc) {
	SWRTParams params = swrt_params();
	uint card_count = min(uint(max(params.extra.z, 0.0)), 64u);
	if (detail_hit == 1u) {
		float best_align = -2.0;
		uint best_card = 0xffffffffu;
		for (uint card_index = 0u; card_index < card_count; card_index++) {
			SWRTCard card = swrt_cards.data[card_index];
			if (uint(card.axis_n_instance.w) != instance_index) {
				continue;
			}
			float texel = max(card.axis_v_texel.w, 0.001);
			vec3 relative = world - card.origin_half_u.xyz;
			float u = dot(card.axis_u_half_v.xyz, relative);
			float v = dot(card.axis_v_texel.xyz, relative);
			if (abs(u) > card.origin_half_u.w || abs(v) > card.axis_u_half_v.w) {
				continue;
			}
			if (abs(dot(card.axis_n_instance.xyz, relative)) > 2.0 * texel) {
				continue;
			}
			float align = dot(card.axis_n_instance.xyz, normal);
			if (align > best_align) {
				best_align = align;
				best_card = card_index;
			}
		}
		if (best_card != 0xffffffffu && best_align > 0.0) {
			return max(swrt_cards.data[best_card].radiance.rgb, vec3(0.0));
		}
	}
	float weight = 0.0;
	vec3 voxel_color = params.sky.rgb;
	if (params.extra.x > 0.5) {
		NextGIVoxelClipmap clipmap;
		clipmap.origin_size[0] = params.clipmap[0];
		clipmap.origin_size[1] = params.clipmap[1];
		clipmap.origin_size[2] = params.clipmap[2];
		clipmap.origin_size[3] = params.clipmap[3];
		voxel_color = nextgi_voxel_lighting_lookup(world, normal, params.wrc.rgb, clipmap, weight);
	}
	if (weight >= 0.5) {
		return max(voxel_color, vec3(0.0));
	}
	if (allow_wrc && params.wrc.a > 0.5) {
		return max(params.wrc.rgb, vec3(0.0));
	}
	return max(params.sky.rgb, vec3(0.0));
}

#endif
