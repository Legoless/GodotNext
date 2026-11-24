/**************************************************************************/
/*  nextgi_world_cache_trace.glsl                                         */
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

#[raygen]

#version 460
#extension GL_EXT_ray_query : require

#VERSION_DEFINES

// GodotNext: nextgi_world_cache (#294, plan §21.2.1 / §26.1 / §26.2).
// Raygen stage 3 of the NextGI hit pipeline (NEXTGI_HIT_RAYGEN_WORLD_CACHE): runs in both Full
// and Simplified Material HWRT modes. Traces short 2-bounce camera-centered paths from the screen
// depth/normal G-buffer into #130's hit shaders, evaluates direct lighting via the 64^3 world-space
// light grid (#129 / §21.0.3) and #145 inline shadow rays, feeds multi-bounce indirect lighting from
// the previous frame's World Cache, and accumulates surface exitant radiance into the sparse
// 8-way set-associative world-space hash table.

#include "../oct_inc.glsl"
#include "nextgi_view_inc.glsl"
#include "nextgi_hit_layout_inc.glsl"

layout(set = 2, binding = 0, std430) restrict readonly buffer HitMaterialParams {
	uint data[];
}
hit_material_params[];

// Keeps set 2's parameter table a run-time array for pipeline library layout matching (#130).
uint nextgi_hit_params_probe(uint p_slot) {
	return hit_material_params[nonuniformEXT(p_slot)].data[0];
}

layout(location = 0) rayPayloadEXT NextGIHitPayload payload;

// Reuse #145's shared shadow visibility policy (inline ray query + bindless texture alpha).
#define NEXTGI_SHADOW_TLAS hit_tlas
#define NEXTGI_SHADOW_SET 0
#define NEXTGI_SHADOW_INSTANCE_BINDING 43
#define NEXTGI_SHADOW_ALPHA_BINDING 44
#define NEXTGI_SHADOW_ALPHA_TEXTURES
#define NEXTGI_SHADOW_USE_HIT_LAYOUT
#include "nextgi_shadow_ray_inc.glsl"

uint wc_pcg(inout uint p_state) {
	p_state = p_state * 747796405u + 2891336453u;
	uint word = ((p_state >> ((p_state >> 28u) + 4u)) ^ p_state) * 277803737u;
	return (word >> 22u) ^ word;
}

float wc_rand(inout uint p_state) {
	return float(wc_pcg(p_state)) * 2.3283064365386963e-10;
}

vec2 wc_blue_noise(ivec2 p_pixel, uint p_frame) {
	vec2 offset = fract(vec2(p_frame) * vec2(0.7548776662, 0.5698402910)) * 64.0;
	ivec2 coord = (p_pixel + ivec2(offset)) & ivec2(63);
	return texelFetch(hit_light_blue_noise, coord, 0).rg;
}

vec3 wc_sample_cosine_hemisphere(vec3 p_normal, vec2 p_u) {
	float phi = 6.2831853 * p_u.x;
	float cos_theta = sqrt(clamp(1.0 - p_u.y, 0.0, 1.0));
	float sin_theta = sqrt(clamp(p_u.y, 0.0, 1.0));
	vec3 up = abs(p_normal.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 t1 = normalize(cross(up, p_normal));
	vec3 t2 = cross(p_normal, t1);
	return normalize(t1 * (cos(phi) * sin_theta) + t2 * (sin(phi) * sin_theta) + p_normal * cos_theta);
}

vec3 wc_miss_radiance(vec3 p_dir) {
	if (hit_wc_params.ambient_color_sky.w > 0.5) {
		return textureLod(sampler2D(hit_light_sky, SAMPLER_LINEAR_CLAMP), vec3_to_oct(p_dir), 0.0).rgb;
	}
	return hit_wc_params.ambient_color_sky.rgb;
}

// Read-only lookup into the previous frame's resolved World Cache radiance for multi-bounce feedback.
bool wc_lookup_previous(vec3 p_world, vec3 p_normal, out vec3 r_radiance) {
	r_radiance = vec3(0.0);
	float dist = length(p_world - hit_wc_params.camera_pos_radius.xyz);
	uint base_level = nextgi_wc_level_for_distance(dist);
	uint face = nextgi_wc_normal_face(p_normal);
	for (uint l = 0u; l < 2u; l++) {
		uint level = base_level + l;
		if (level >= NEXTGI_WC_LEVELS) {
			break;
		}
		ivec3 cell = nextgi_wc_quantize(p_world, level);
		uint key = nextgi_wc_make_key(cell, level, face);
		uint base_slot = nextgi_wc_hash_bucket(cell, level, face) * NEXTGI_WC_SLOTS_PER_BUCKET;
		for (uint w = 0u; w < NEXTGI_WC_SLOTS_PER_BUCKET; w++) {
			uint k = hit_wc_keys.data[base_slot + w];
			if (k == key) {
				vec4 rad = hit_wc_radiance.data[base_slot + w];
				if (rad.a > 0.0) {
					r_radiance = rad.rgb;
					return true;
				}
				return false;
			}
		}
	}
	return false;
}

// Lock-free 8-way set-associative cell insertion and fixed-point radiance accumulation.
void wc_insert_and_accum(vec3 p_world, vec3 p_normal, vec3 p_radiance) {
	if (any(isnan(p_radiance)) || any(isinf(p_radiance))) {
		return;
	}
	vec3 clamped = clamp(p_radiance, vec3(0.0), vec3(NEXTGI_WC_MAX_RADIANCE));
	float dist = length(p_world - hit_wc_params.camera_pos_radius.xyz);
	uint level = nextgi_wc_level_for_distance(dist);
	uint face = nextgi_wc_normal_face(p_normal);
	ivec3 cell = nextgi_wc_quantize(p_world, level);
	uint key = nextgi_wc_make_key(cell, level, face);
	uint base_slot = nextgi_wc_hash_bucket(cell, level, face) * NEXTGI_WC_SLOTS_PER_BUCKET;

	uint target_slot = 0xFFFFFFFFu;
	uint first_empty = 0xFFFFFFFFu;
	for (uint w = 0u; w < NEXTGI_WC_SLOTS_PER_BUCKET; w++) {
		uint slot = base_slot + w;
		uint existing = hit_wc_keys.data[slot];
		if (existing == key) {
			target_slot = slot;
			break;
		}
		if (existing == NEXTGI_WC_EMPTY_KEY && first_empty == 0xFFFFFFFFu) {
			first_empty = w;
		}
	}
	if (target_slot == 0xFFFFFFFFu && first_empty != 0xFFFFFFFFu) {
		for (uint w = 0u; w < NEXTGI_WC_SLOTS_PER_BUCKET; w++) {
			uint slot = base_slot + ((first_empty + w) % NEXTGI_WC_SLOTS_PER_BUCKET);
			uint prev = atomicCompSwap(hit_wc_keys.data[slot], NEXTGI_WC_EMPTY_KEY, key);
			if (prev == NEXTGI_WC_EMPTY_KEY) {
				uint cprev = atomicAdd(hit_wc_counters.data.cells_inserted, 1u);
				if (cprev == 0xFFFFFFFFu) {
					hit_wc_counters.data.sink = cprev;
				}
				target_slot = slot;
				break;
			}
			if (prev == key) {
				target_slot = slot;
				break;
			}
		}
	}
	if (target_slot == 0xFFFFFFFFu) {
		uint hprev = atomicAdd(hit_wc_counters.data.hash_collisions, 1u);
		if (hprev == 0xFFFFFFFFu) {
			hit_wc_counters.data.sink = hprev;
		}
		return;
	}
	uvec3 fixed_rad = uvec3(round(clamped * NEXTGI_WC_FIXED_SCALE));
	uint rprev = atomicAdd(hit_wc_accum.data[target_slot].x, fixed_rad.x);
	uint gprev = atomicAdd(hit_wc_accum.data[target_slot].y, fixed_rad.y);
	uint bprev = atomicAdd(hit_wc_accum.data[target_slot].z, fixed_rad.z);
	uint aprev = atomicAdd(hit_wc_accum.data[target_slot].w, 1u);
	if ((rprev | gprev | bprev | aprev) == 0xFFFFFFFFu) {
		hit_wc_counters.data.sink = rprev;
	}
}

// Evaluates diffuse direct lighting at a hit point from the 64^3 world-space light grid (§21.0.3)
// using #145 inline shadow rays (trace_shadow). Matches SRC radiometric units: (albedo*(1-m)/PI) * E.
vec3 wc_eval_direct_diffuse(vec3 p_hit_pos, vec3 p_normal, vec3 p_diffuse_albedo, float p_hit_eps, ivec2 p_pixel, uint p_bounce, uint p_max_shadows) {
	vec3 direct = vec3(0.0);
	uint cell = nextgi_light_grid_world_to_cell(p_hit_pos, hit_wc_params.grid_min_inv_cell.xyz, hit_wc_params.grid_min_inv_cell.w);
	uint light_count_total = hit_wc_params.counts1.y;
	if (cell == 0xFFFFFFFFu || light_count_total == 0u) {
		return direct;
	}
	uint cell_base = cell * NEXTGI_LIGHT_GRID_CELL_WORDS;
	uint cell_count = min(hit_light_grid.data[cell_base], NEXTGI_LIGHT_GRID_CELL_CAP);
	uint shadowed_used = 0u;
	uint traced_shadows = 0u;
	vec3 shadow_origin = p_hit_pos + p_normal * max(p_hit_eps, 0.015);
	vec3 diffuse_brdf = p_diffuse_albedo * (1.0 / NEXTGI_HL_PI);

	for (uint i = 0u; i < cell_count; i++) {
		uint packed = hit_light_grid.data[cell_base + 1u + (i >> 1u)];
		uint light_idx = ((i & 1u) == 0u) ? (packed & 0xFFFFu) : (packed >> 16u);
		if (light_idx >= light_count_total) {
			continue;
		}
		NextGIHitLightGPU light = hit_light_lights.data[light_idx];
		uint ltype = uint(light.direction_type.w + 0.5);
		vec3 to_light;
		float dist = hit_wc_params.camera_pos_radius.w;
		float atten = 1.0;
		bool is_dir = (ltype == 0u);
		if (is_dir) {
			to_light = -light.direction_type.xyz;
		} else {
			vec3 delta = light.position_inv_range.xyz - p_hit_pos;
			dist = length(delta);
			if (!(dist > 1e-4)) {
				continue;
			}
			to_light = delta / dist;
			if (ltype == 2u) {
				atten = nextgi_hl_spot_attenuation(dist, light.position_inv_range.w, light.spot_atten_radius.x, dot(-to_light, light.direction_type.xyz), light.spot_atten_radius.y, light.spot_atten_radius.z);
			} else {
				atten = nextgi_hl_omni_attenuation(dist, light.position_inv_range.w, light.spot_atten_radius.x);
			}
		}
		float n_dot_l = dot(p_normal, to_light);
		if (!(n_dot_l > 0.0) || !(atten > 0.0)) {
			continue;
		}
		float vis = 1.0;
		if ((light.shadow_flags.x & NEXTGI_HL_LIGHT_FLAG_SHADOW) != 0u && shadowed_used < p_max_shadows) {
			vec2 bn = wc_blue_noise(p_pixel, hit_wc_params.counts1.x + shadowed_used * 19u + p_bounce * 47u);
			vec3 shadow_dir;
			float shadow_tmax;
			nextgi_hl_area_shadow_ray(to_light, max(dist - max(p_hit_eps, 0.015), 0.0), light.spot_atten_radius.w, is_dir, bn, shadow_dir, shadow_tmax);
			vis = trace_shadow(shadow_origin, shadow_dir, shadow_tmax, light.shadow_flags.y, light.shadow_flags.z);
			shadowed_used++;
			traced_shadows++;
		}
		if (vis > 0.0) {
			direct += diffuse_brdf * (light.color_energy.rgb * light.color_energy.a * atten * n_dot_l * vis);
		}
	}
	if (traced_shadows > 0u) {
		uint sprev = atomicAdd(hit_wc_counters.data.shadow_rays, traced_shadows);
		if (sprev == 0xFFFFFFFFu) {
			hit_wc_counters.data.sink = sprev;
		}
	}
	return direct;
}

bool wc_trace_hit(vec3 p_origin, vec3 p_dir, float p_tmax, vec2 p_cone, out vec3 r_pos, out vec3 r_normal, out vec3 r_diffuse_albedo, out vec3 r_emission, out float r_eps) {
	payload.shading = uvec4(0u);
	payload.hit = uvec4(floatBitsToUint(p_cone.x), floatBitsToUint(p_cone.y), 0u, 0u);
	traceRayEXT(hit_tlas, gl_RayFlagsOpaqueEXT, NEXTGI_WC_MASK_GI, 0u, 1u, 0u, p_origin, 0.0, p_dir, p_tmax, 0);
	uint pflags = payload.shading.z >> 24u;
	if ((pflags & NEXTGI_HIT_PAYLOAD_HIT) == 0u) {
		return false;
	}
	float hit_t = uintBitsToFloat(payload.hit.x);
	uint hit_custom = payload.hit.w;
	r_pos = p_origin + p_dir * hit_t;
	r_eps = hit_custom < hit_params.record_count ? hit_records.data[hit_custom].origin_offset : 0.005;
	vec3 albedo = unpackUnorm4x8(payload.shading.x).rgb;
	float metallic = float((payload.shading.z >> 8u) & 0xFFu) / 255.0;
	r_diffuse_albedo = clamp(albedo * (1.0 - metallic), vec3(0.0), vec3(0.95));
	r_emission = nextgi_hit_unpack_r11g11b10(payload.shading.w);
	r_normal = nextgi_hit_oct_decode(unpackSnorm2x16(payload.shading.y));
	if (dot(r_normal, -p_dir) < 0.0) {
		r_normal = -r_normal;
	}
	return true;
}

void main() {
	uvec2 gid = gl_LaunchIDEXT.xy;
	if (gid.x >= hit_wc_params.counts0.z || gid.y >= hit_wc_params.counts0.w) {
		return;
	}
	uint frame = hit_wc_params.counts1.x;
	uint stride = max(hit_wc_params.counts1.z, 1u);
	ivec2 screen_size = ivec2(hit_wc_params.counts0.xy);
	vec2 tile_jitter = fract(vec2(frame) * vec2(0.7548776662, 0.5698402910));
	ivec2 pixel = ivec2(gid * stride) + ivec2(floor(tile_jitter * float(stride)));
	pixel = clamp(pixel, ivec2(0), screen_size - ivec2(1));

	float depth = texelFetch(hit_wc_depth, pixel, 0).r;
	if (depth <= 0.0) {
		return;
	}
	vec2 uv = (vec2(pixel) + 0.5) / vec2(screen_size);
	vec3 world_pos = nextgi_view_reconstruct(hit_wc_params.inv_view_proj, uv, depth);
	vec4 guide = texelFetch(hit_wc_normal_roughness, pixel, 0);
	mat3 v2w = mat3(hit_wc_params.view_to_world[0].xyz, hit_wc_params.view_to_world[1].xyz, hit_wc_params.view_to_world[2].xyz);
	vec3 surf_normal = nextgi_view_world_normal(v2w, guide.xyz);
	vec3 cam_pos = hit_wc_params.camera_pos_radius.xyz;
	vec3 to_surface = world_pos - cam_pos;
	float cam_dist = length(to_surface);
	if (!(cam_dist > 1e-4)) {
		return;
	}
	vec3 view_ray = to_surface / cam_dist;
	if (dot(surf_normal, -view_ray) < 0.0) {
		surf_normal = -surf_normal;
	}

	uint rng = (gid.x * 1973u) ^ (gid.y * 9277u) ^ (frame * 26699u) ^ 0x9E3779B9u;
	float rt_radius = max(hit_wc_params.camera_pos_radius.w, 1.0);
	uint tprev = atomicAdd(hit_wc_counters.data.paths_traced, 1u);
	if (tprev == 0xFFFFFFFFu) {
		hit_wc_counters.data.sink = tprev;
	}

	// Step 1: On alternating probes, trace a short ray onto the visible G-buffer surface to seed
	// its own World Cache cell directly via the hit shader (both Full and Simplified Material modes).
	bool seed_visible = ((gid.x ^ gid.y ^ frame) & 1u) == 0u;
	bool has_primary = false;
	vec3 p0_pos = world_pos;
	vec3 p0_normal = surf_normal;
	vec3 p0_diffuse = vec3(0.5);
	vec3 p0_emission = vec3(0.0);
	vec3 p0_direct = vec3(0.0);
	float p0_eps = 0.01;
	if (seed_visible) {
		float pull = min(cam_dist * 0.5, 0.35);
		vec3 seed_origin = world_pos - view_ray * pull;
		if (wc_trace_hit(seed_origin, view_ray, pull + 0.35, vec2(0.01, 0.02), p0_pos, p0_normal, p0_diffuse, p0_emission, p0_eps)) {
			has_primary = true;
			uint pprev = atomicAdd(hit_wc_counters.data.primary_hits, 1u);
			if (pprev == 0xFFFFFFFFu) {
				hit_wc_counters.data.sink = pprev;
			}
			p0_direct = wc_eval_direct_diffuse(p0_pos, p0_normal, p0_diffuse, p0_eps, pixel, 0u, NEXTGI_WC_MAX_SHADOWED_LIGHTS);
		}
	}

	// Step 2: Trace bounce 1 from the screen surface (cosine-weighted hemisphere).
	vec2 u1 = vec2(wc_rand(rng), wc_rand(rng));
	vec3 dir1 = wc_sample_cosine_hemisphere(p0_normal, u1);
	vec3 origin1 = p0_pos + p0_normal * max(p0_eps, 0.015);
	vec3 p1_pos;
	vec3 p1_normal;
	vec3 p1_diffuse;
	vec3 p1_emission;
	float p1_eps;
	vec3 bounce0_incoming;

	if (wc_trace_hit(origin1, dir1, rt_radius, vec2(0.02, 0.12), p1_pos, p1_normal, p1_diffuse, p1_emission, p1_eps)) {
		uint b1prev = atomicAdd(hit_wc_counters.data.bounce_hits, 1u);
		if (b1prev == 0xFFFFFFFFu) {
			hit_wc_counters.data.sink = b1prev;
		}
		vec3 direct1 = wc_eval_direct_diffuse(p1_pos, p1_normal, p1_diffuse, p1_eps, pixel, 1u, NEXTGI_WC_MAX_SHADOWED_LIGHTS);

		// Step 3: Trace bounce 2 from bounce 1's hit for multi-bounce color bleed, backed by previous World Cache.
		vec2 u2 = vec2(wc_rand(rng), wc_rand(rng));
		vec3 dir2 = wc_sample_cosine_hemisphere(p1_normal, u2);
		vec3 origin2 = p1_pos + p1_normal * max(p1_eps, 0.015);
		vec3 p2_pos;
		vec3 p2_normal;
		vec3 p2_diffuse;
		vec3 p2_emission;
		float p2_eps;
		vec3 bounce1_incoming;

		if (wc_trace_hit(origin2, dir2, min(rt_radius, 64.0), vec2(0.05, 0.2), p2_pos, p2_normal, p2_diffuse, p2_emission, p2_eps)) {
			uint b2prev = atomicAdd(hit_wc_counters.data.bounce_hits, 1u);
			if (b2prev == 0xFFFFFFFFu) {
				hit_wc_counters.data.sink = b2prev;
			}
			vec3 direct2 = wc_eval_direct_diffuse(p2_pos, p2_normal, p2_diffuse, p2_eps, pixel, 2u, 1u);
			vec3 prev_rad2;
			vec3 bounce2_indirect = vec3(0.0);
			if (wc_lookup_previous(p2_pos, p2_normal, prev_rad2)) {
				bounce2_indirect = p2_diffuse * prev_rad2;
			} else {
				bounce2_indirect = p2_diffuse * hit_wc_params.ambient_color_sky.rgb;
			}
			vec3 rad2 = p2_emission + direct2 + bounce2_indirect;
			wc_insert_and_accum(p2_pos, p2_normal, rad2);
			bounce1_incoming = rad2;
		} else {
			bounce1_incoming = wc_miss_radiance(dir2);
		}

		vec3 rad1 = p1_emission + direct1 + p1_diffuse * bounce1_incoming;
		wc_insert_and_accum(p1_pos, p1_normal, rad1);
		bounce0_incoming = rad1;
	} else {
		bounce0_incoming = wc_miss_radiance(dir1);
	}

	if (has_primary) {
		vec3 rad0 = p0_emission + p0_direct + p0_diffuse * bounce0_incoming;
		wc_insert_and_accum(p0_pos, p0_normal, rad0);
	}
}
