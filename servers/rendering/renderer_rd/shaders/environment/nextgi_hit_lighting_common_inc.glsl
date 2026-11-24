/**************************************************************************/
/*  nextgi_hit_lighting_common_inc.glsl                                   */
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

// GodotNext: nextgi_hit_lighting (#129, plan §21.2 contract L7). Not a standalone shader.
//
// Shared structs, packing helpers, 64^3 world light grid indexing, Godot PBR BRDF, area-light shadow jitter,
// translucent skip transmittance, and fog helpers used by Mark (nextgi_reflect.glsl), Sort (nextgi_hit_sort.glsl),
// World Light Grid (nextgi_light_grid.glsl), and Re-trace & Shade (nextgi_hit_lighting.glsl).
// Kept modular so GI hit lighting (#128) and Lumen World Cache (#294) reuse the same records and shading.

#ifndef NEXTGI_HIT_LIGHTING_COMMON_INC_GLSL
#define NEXTGI_HIT_LIGHTING_COMMON_INC_GLSL

const float NEXTGI_HL_PI = 3.14159265;

const uint NEXTGI_LIGHT_GRID_DIM = 64u;
const uint NEXTGI_LIGHT_GRID_CELLS = 262144u; // 64^3
const uint NEXTGI_LIGHT_GRID_CELL_CAP = 32u;
// Each cell stores 1 uint count followed by 16 uints packing 32 uint16 light indices.
const uint NEXTGI_LIGHT_GRID_CELL_WORDS = 17u;

const uint NEXTGI_HL_MAX_SHADOWED_LIGHTS = 4u;
const uint NEXTGI_HL_GI_MAX_SHADOWED_LIGHTS = 1u;
const uint NEXTGI_HL_MAX_TRANSLUCENT_SKIPS = 2u;
const float NEXTGI_HL_MIRROR_ROUGHNESS = 0.1;

// Disjoint 16-bit material sort key ranges so a single 16-bit counting sort separates
// reflection hits [0x0000..0x7FFF] from diffuse GI hits [0x8000..0xFFFF] (#128).
const uint NEXTGI_HL_KEY_REFLECTION_BASE = 0x0000u;
const uint NEXTGI_HL_KEY_REFLECTION_FALLBACK = 0x7FFFu;
const uint NEXTGI_HL_KEY_GI_BASE = 0x8000u;
const uint NEXTGI_HL_KEY_GI_FALLBACK = 0xFFFFu;

// TLAS instance mask bits (next_gi_shadow_policy.h) and instance record rt_flags (next_gi_rt_scene.h).
const uint NEXTGI_HL_MASK_GI = 0x80u;
const uint NEXTGI_HL_MASK_TRANSLUCENT = 0x02u;
const uint NEXTGI_HL_RT_FLAG_TRANSLUCENT = 1u << 5u;

// NextGIHitLightRecord.id_pixel.x upper 8 bits (bits 24..31).
const uint NEXTGI_HL_RECORD_FLAG_FRONT_FACE = 1u << 24u;
const uint NEXTGI_HL_RECORD_FLAG_TRANSLUCENT = 1u << 25u;
const uint NEXTGI_HL_RECORD_FLAG_MIRROR = 1u << 26u;
const uint NEXTGI_HL_RECORD_FLAG_DIFFUSE_GI = 1u << 27u;

// NextGIHitLightGPU.shadow_flags.x bits.
const uint NEXTGI_HL_LIGHT_FLAG_SHADOW = 1u << 0u;
const uint NEXTGI_HL_LIGHT_FLAG_STATIC = 1u << 1u;

// NextGIHitLightParamsGPU.counts1.w bits.
const uint NEXTGI_HL_PARAM_FLAG_TRANSLUCENT = 1u << 0u;
const uint NEXTGI_HL_PARAM_FLAG_REFLECT_DEBUG = 1u << 1u;
const uint NEXTGI_HL_PARAM_FLAG_BLUE_NOISE = 1u << 2u;
const uint NEXTGI_HL_PARAM_FLAG_SRC_INDIRECT = 1u << 3u;

// Compact per-ray hit record (48 bytes, std430).
// origin_t: xyz = ray origin (after any screen-trace resume), w = hit distance t from origin.
// id_pixel: x = instance_index (24-bit) | record flags (bits 24..31),
//           y = geometry_index (16-bit) | (material_key << 16),
//           z = primitive_index,
//           w = pixel.x | (pixel.y << 16).
// packed_geom: x = packHalf2x16(bary.xy),
//              y = packSnorm2x16(oct_encode(ray_dir)),
//              z = packHalf2x16(vec2(cone_width, cone_spread)),
//              w = floatBitsToUint(resume_distance).
struct NextGIHitLightRecord {
	vec4 origin_t;
	uvec4 id_pixel;
	uvec4 packed_geom;
};

// Direct light entry in the D14 scenario light set (80 bytes, std430).
struct NextGIHitLightGPU {
	vec4 color_energy; // rgb linear color, a = direct energy * exposure
	vec4 position_inv_range; // xyz world pos, w = 1/range (0 for directional)
	vec4 direction_type; // xyz travel direction, w = float(0 directional, 1 omni, 2 spot)
	vec4 spot_atten_radius; // x = attenuation, y = cos_spot, z = inv_spot_atten, w = source_radius
	uvec4 shadow_flags; // x = flags, y = #145 shadow light_mask_mode, z = caster_mask, w = pad
};

// GPU counters and #277-safe atomic sink (64 bytes, std430).
struct NextGIHitLightCounters {
	uint marked_hits;
	uint sorted_hits;
	uint shadow_rays;
	uint unshadowed_lights;
	uint bounce_rays;
	uint translucent_skips;
	uint light_grid_overflow;
	uint sink;
	uint refl_sorted_hits;
	uint gi_marked_hits;
	uint gi_sorted_hits;
	uint gi_shaded_hits;
	uint gi_shadow_rays;
	uint pad0;
	uint pad1;
	uint pad2;
};

// 16-bit material sort key partitioned into two disjoint ranges (#128):
//   Reflections: [0x0000..0x7FFF] (7-bit shader slot + 8-bit material id, fallback 0x7FFF).
//   Diffuse GI:  [0x8000..0xFFFF] (0x8000 | 7-bit shader slot + 8-bit material id, fallback 0xFFFF).
uint nextgi_hit_light_material_key(uint p_shader_slot, uint p_material_id) {
	if (p_material_id == 0xFFFFFFFFu || p_shader_slot == 0xFFFFFFFFu) {
		return NEXTGI_HL_KEY_REFLECTION_FALLBACK;
	}
	uint key = ((p_shader_slot & 0x7Fu) << 8u) | (p_material_id & 0xFFu);
	return key == NEXTGI_HL_KEY_REFLECTION_FALLBACK ? (NEXTGI_HL_KEY_REFLECTION_FALLBACK - 1u) : key;
}

uint nextgi_hit_light_gi_material_key(uint p_shader_slot, uint p_material_id) {
	if (p_material_id == 0xFFFFFFFFu || p_shader_slot == 0xFFFFFFFFu) {
		return NEXTGI_HL_KEY_GI_FALLBACK;
	}
	uint key = NEXTGI_HL_KEY_GI_BASE | ((p_shader_slot & 0x7Fu) << 8u) | (p_material_id & 0xFFu);
	return key == NEXTGI_HL_KEY_GI_FALLBACK ? (NEXTGI_HL_KEY_GI_FALLBACK - 1u) : key;
}

uint nextgi_hl_record_material_key(NextGIHitLightRecord p_rec) {
	return p_rec.id_pixel.y >> 16u;
}

vec2 nextgi_hl_oct_encode(vec3 p_n) {
	vec3 n = p_n / max(abs(p_n.x) + abs(p_n.y) + abs(p_n.z), 1e-6);
	vec2 e = n.xy;
	if (n.z < 0.0) {
		e = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
	}
	return e;
}

vec3 nextgi_hl_oct_decode(vec2 p_e) {
	vec3 v = vec3(p_e.xy, 1.0 - abs(p_e.x) - abs(p_e.y));
	float t = max(-v.z, 0.0);
	v.xy += t * vec2(v.x >= 0.0 ? -1.0 : 1.0, v.y >= 0.0 ? -1.0 : 1.0);
	return normalize(v);
}

// Maps world position to a 64^3 world light grid cell index, or 0xFFFFFFFF if outside.
uint nextgi_light_grid_world_to_cell(vec3 p_world, vec3 p_grid_min, float p_inv_cell_size) {
	vec3 f = floor((p_world - p_grid_min) * p_inv_cell_size);
	if (any(lessThan(f, vec3(0.0))) || any(greaterThanEqual(f, vec3(float(NEXTGI_LIGHT_GRID_DIM))))) {
		return 0xFFFFFFFFu;
	}
	uvec3 c = uvec3(f);
	return c.x + c.y * NEXTGI_LIGHT_GRID_DIM + c.z * NEXTGI_LIGHT_GRID_DIM * NEXTGI_LIGHT_GRID_DIM;
}

float nextgi_hl_omni_attenuation(float p_distance, float p_inv_range, float p_decay) {
	float nd = p_distance * p_inv_range;
	nd *= nd;
	nd *= nd;
	nd = max(1.0 - nd, 0.0);
	nd *= nd;
	return nd * pow(max(p_distance, 0.0001), -p_decay);
}

float nextgi_hl_spot_attenuation(float p_distance, float p_inv_range, float p_decay, float p_facing, float p_cos_spot, float p_inv_spot_atten) {
	float atten = nextgi_hl_omni_attenuation(p_distance, p_inv_range, p_decay);
	float scos = max(p_facing, p_cos_spot);
	float denom = max(1.0 - p_cos_spot, 1e-4);
	float spot_rim = max(1e-4, (1.0 - scos) / denom);
	float power = p_inv_spot_atten > 0.0 ? 1.0 / p_inv_spot_atten : 1.0;
	return atten * (1.0 - pow(spot_rim, power));
}

// Godot PBR direct lighting BRDF (Lambert diffuse + Trowbridge-Reitz GGX specular + Smith-GGX visibility + Schlick Fresnel).
vec3 nextgi_hl_eval_brdf(vec3 p_albedo, float p_metallic, float p_roughness, vec3 p_n, vec3 p_v, vec3 p_l) {
	float n_dot_v = max(dot(p_n, p_v), 1e-4);
	float n_dot_l = max(dot(p_n, p_l), 0.0);
	if (n_dot_l <= 0.0) {
		return vec3(0.0);
	}
	vec3 diffuse = (p_albedo * (1.0 - p_metallic)) * (1.0 / NEXTGI_HL_PI);
	vec3 h = normalize(p_v + p_l);
	float n_dot_h = max(dot(p_n, h), 0.0);
	float v_dot_h = max(dot(p_v, h), 0.0);

	float r = clamp(p_roughness, 0.045, 1.0);
	float a = r * r;
	float a2 = a * a;

	float d_denom = (n_dot_h * n_dot_h) * (a2 - 1.0) + 1.0;
	float d_term = a2 / max(NEXTGI_HL_PI * d_denom * d_denom, 1e-7);

	float gv = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2);
	float gl = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2);
	float vis = 0.5 / max(gv + gl, 1e-5);

	vec3 f0 = mix(vec3(0.04), p_albedo, p_metallic);
	vec3 fresnel = f0 + (1.0 - f0) * pow(clamp(1.0 - v_dot_h, 0.0, 1.0), 5.0);
	vec3 specular = (d_term * vis) * fresnel;
	return diffuse + specular;
}

// Area-light shadow ray jitter on the light disc perpendicular to p_to_light.
// For directional lights (p_is_directional), p_radius is sin(angular_radius) and p_tmax is unchanged.
void nextgi_hl_area_shadow_ray(vec3 p_to_light, float p_tmax, float p_radius, bool p_is_directional, vec2 p_u, out vec3 r_dir, out float r_tmax) {
	r_dir = p_to_light;
	r_tmax = p_tmax;
	if (!(p_radius > 1e-5)) {
		return;
	}
	float angle = 6.2831853 * p_u.x;
	float r = sqrt(clamp(p_u.y, 0.0, 1.0));
	vec2 disk = vec2(cos(angle), sin(angle)) * r;
	vec3 up = abs(p_to_light.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 t1 = normalize(cross(up, p_to_light));
	vec3 t2 = cross(p_to_light, t1);
	if (p_is_directional) {
		float sin_r = min(p_radius, 0.5);
		r_dir = normalize(p_to_light + (t1 * disk.x + t2 * disk.y) * sin_r);
	} else {
		float clamped_r = min(p_radius, p_tmax * 0.45);
		vec3 target = p_to_light * p_tmax + (t1 * disk.x + t2 * disk.y) * clamped_r;
		float len = length(target);
		if (len > 1e-4) {
			r_dir = target / len;
			r_tmax = max(len - clamped_r * 0.25, 0.01);
		}
	}
}

// Energy-conserving tinted transmittance through one alpha-blended translucent layer.
vec3 nextgi_hl_translucent_transmittance(vec3 p_albedo, float p_alpha) {
	float a = clamp(p_alpha, 0.0, 1.0);
	vec3 tint = mix(vec3(1.0), clamp(p_albedo, vec3(0.0), vec3(1.0)), a);
	return (1.0 - a) * tint;
}

vec3 nextgi_hl_reflection_fog(vec3 p_radiance, vec3 p_world, float p_distance, vec4 p_fog0, vec4 p_fog1, vec4 p_fog_color) {
	if (p_fog0.x < 0.5 || p_distance >= 65504.0) {
		return p_radiance;
	}
	float amount;
	if (p_fog0.y > 0.5) {
		float fog_z = smoothstep(p_fog1.y, p_fog1.z, p_distance);
		amount = pow(fog_z, p_fog1.w) * p_fog0.z;
	} else {
		amount = 1.0 - exp(min(0.0, -p_distance * p_fog0.z));
	}
	if (abs(p_fog1.x) >= 0.0001) {
		float y_dist = p_world.y - p_fog0.w;
		amount = max(1.0 - exp(min(0.0, y_dist * p_fog1.x)), amount);
	}
	return mix(p_radiance, p_fog_color.rgb * p_fog_color.a, clamp(amount, 0.0, 1.0));
}

#endif // NEXTGI_HIT_LIGHTING_COMMON_INC_GLSL
