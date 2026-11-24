// GodotNext: megalights_fog_translucency (#137, plan §21.3 contract M7, GM-6, budget M-8).
// Shared MegaLights volumetric-fog froxel & D17 translucency direct SH-L1 lighting include.
// Included by ml_froxel.glsl (HWRT inline ray query + VSM coarse pages) and ml_froxel_compute.glsl
// (non-RT VSM coarse pages / unshadowed fallback + D17 trilinear resample pass).

#ifndef ML_FROXEL_INC_GLSL
#define ML_FROXEL_INC_GLSL

#ifndef M_PI
#define M_PI 3.14159265359
#endif

#ifndef MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS
#define MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS 8
#endif

#define ML_LIGHT_TYPE_OMNI 0u
#define ML_LIGHT_TYPE_SPOT 1u
#define ML_LIGHT_TYPE_AREA 2u
#define ML_LIGHT_TYPE_DIRECTIONAL 3u

#define ML_SAMPLE_INDEX_MASK 0xFFFFu
#define ML_SAMPLE_TYPE_SHIFT 16u
#define ML_SAMPLE_TYPE_MASK 0x3u
#define ML_SAMPLE_INVALID 0xFFFFFFFFu

#define ML_LIGHT_GRID_TILE_SHIFT 6u
#define ML_LIGHT_GRID_FLAG_ENABLED 1u
#define ML_LIGHT_GRID_FLAG_ORTHOGONAL 2u

#define ML_POLICY_FLAG_CASTS_SHADOW (1u << 0u)
#define ML_POLICY_FLAG_USE_VSM (1u << 1u)

#define ML_FROXEL_FLAG_FOG_ACTIVE (1u << 0u)
#define ML_FROXEL_FLAG_D17_DIRECT_WRITE (1u << 1u)
#define ML_FROXEL_FLAG_WRITE_SH (1u << 2u)
#define ML_FROXEL_FLAG_HISTORY_VALID (1u << 3u)
#define ML_FROXEL_FLAG_USE_LIGHT_FUNCTION (1u << 4u)
#define ML_FROXEL_FLAG_USE_VSM (1u << 5u)
#define ML_FROXEL_FLAG_DIRECTIONAL_LIGHTS (1u << 6u)
#define ML_FROXEL_FLAG_ORTHOGONAL (1u << 7u)
#define ML_FROXEL_FLAG_FORCE_VISIBLE_ONE (1u << 8u)

#define ML_FROXEL_MAX_SAMPLES 4u
#define ML_FROXEL_INV_FOG_FADE 10.0

struct MLFroxelParams {
	vec4 inv_view_rows[3];
	vec4 to_prev_view_rows[3];
	mat4 inv_view_proj;
	mat4 prev_view_proj;
	vec4 depth_coeffs;
	vec4 fog_frustum_size; // xy = frustum_begin, zw = frustum_end
	vec4 fog_z_params; // x = fog_end, y = detail_spread, z = phase_g, w = inv_fog_fade
	vec4 d17_params; // x = d17_near, y = d17_far, z = history_blend, w = falloff_cull
	uvec4 grid_dims; // xyz = active dispatch grid dims, w = flags
	uvec4 d17_dims; // xyz = D17 dims, w = samples_per_froxel
	uvec4 fog_dims_pad; // xyz = fog volume dims, w = use_packed_grid
	uvec4 light_counts; // x = omni, y = spot, z = area, w = directional
	uvec4 light_grid_dims; // xyz = packed light grid dims, w = packed light grid flags
	vec4 light_grid_z_params; // x = z_near, y = z_far, z = z_scale, w = z_bias
	uvec4 screen_frame_vsm; // xy = screen_size, z = frame_index, w = vsm_table_maps
};

layout(set = 0, binding = 0, std140) uniform MLFroxelParamsUBO {
	MLFroxelParams params;
};

#ifndef ML_FROXEL_RESAMPLE_ONLY

#include "../environment/nextgi_view_inc.glsl"
#include "../light_data_inc.glsl"
#define MODE_VISIBILITY_RESOLVE
#include "../area_lights_inc.glsl"
#include "../ies_inc.glsl"
#include "../light_function_inc.glsl"
#include "../vsm_mark_inc.glsl"

layout(set = 0, binding = 1, std430) restrict readonly buffer MLFroxelOmniLights {
	LightData data[];
}
ml_omni_lights;

layout(set = 0, binding = 2, std430) restrict readonly buffer MLFroxelSpotLights {
	LightData data[];
}
ml_spot_lights;

layout(set = 0, binding = 3, std430) restrict readonly buffer MLFroxelAreaLights {
	LightData data[];
}
ml_area_lights;

layout(set = 0, binding = 4, std140) uniform MLFroxelDirectionalLights {
	DirectionalLightData data[MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS];
}
ml_directional_lights;

layout(set = 0, binding = 5, std430) restrict readonly buffer MLFroxelLightGridHeaders {
	uvec2 data[];
}
ml_light_grid_headers;

layout(set = 0, binding = 6, std430) restrict readonly buffer MLFroxelLightGridIndices {
	uint data[];
}
ml_light_grid_indices;

layout(set = 0, binding = 7) uniform texture2DArray ml_stbn_tex;
layout(set = 0, binding = 8) uniform texture2DArray ml_ies_profile_atlas_tex;
layout(set = 0, binding = 9) uniform texture2D ml_light_function_atlas_tex;

layout(set = 0, binding = 10, std430) restrict readonly buffer MLFroxelLightFunctionData {
	LightFunctionEntry data[];
}
ml_light_function_data;

layout(set = 0, binding = 11) uniform texture2D ml_area_light_atlas_tex;
layout(set = 0, binding = 12) uniform sampler ml_sampler_nearest_clamp;
layout(set = 0, binding = 13) uniform sampler ml_sampler_linear_clamp;
layout(set = 0, binding = 14) uniform sampler ml_sampler_linear_mips_clamp;

layout(set = 0, binding = 15, std430) restrict readonly buffer MLFroxelLightShadowPolicies {
	uvec4 data[];
}
ml_light_shadow_policies;

layout(set = 0, binding = 16) uniform utexture2DArray vsm_pool_tex;
layout(set = 0, binding = 17, std430) restrict readonly buffer VSMPageTable {
	uint entries[];
}
vsm_page_table;
layout(set = 0, binding = 18, std430) restrict readonly buffer VSMMaps {
	VSMMapData data[];
}
vsm_maps;
layout(set = 0, binding = 19, std430) restrict readonly buffer VSMCoarseMiss {
	uint data[];
}
vsm_coarse_miss;

#define VSM_NO_COARSE_MISS_ATOMICS
#define VSM_COARSE_NEAREST_SAMPLER ml_sampler_nearest_clamp
#include "../vsm_page_table_inc.glsl"
#include "../vsm_raster_inc.glsl"
#include "../vsm_coarse_inc.glsl"

layout(set = 0, binding = 20) uniform texture3D ml_froxel_fog_prev;
layout(set = 0, binding = 21) uniform texture3D ml_froxel_sh_r_prev;
layout(set = 0, binding = 22) uniform texture3D ml_froxel_sh_g_prev;
layout(set = 0, binding = 23) uniform texture3D ml_froxel_sh_b_prev;

layout(rgba16f, set = 0, binding = 24) uniform restrict writeonly image3D ml_froxel_fog_out;
layout(rgba16f, set = 0, binding = 25) uniform restrict writeonly image3D ml_froxel_sh_r_out;
layout(rgba16f, set = 0, binding = 26) uniform restrict writeonly image3D ml_froxel_sh_g_out;
layout(rgba16f, set = 0, binding = 27) uniform restrict writeonly image3D ml_froxel_sh_b_out;

float ml_froxel_luminance(vec3 p_c) {
	return dot(p_c, vec3(0.2126, 0.7152, 0.0722));
}

vec3 ml_froxel_safe_normalize(vec3 p_v) {
	float len2 = dot(p_v, p_v);
	return len2 > 1e-12 ? p_v * inversesqrt(len2) : vec3(0.0, 0.0, 1.0);
}

uint ml_froxel_pcg(uint p_v) {
	uint state = p_v * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

float ml_froxel_hash_float(uint p_h) {
	return float(p_h >> 8u) * (1.0 / 16777216.0);
}

float ml_froxel_stbn_u(uvec3 p_cell, uint p_frame, uint p_salt) {
	uint h_off = ml_froxel_pcg(p_salt ^ (p_frame * 0x9E3779B9u) ^ (p_cell.z * 0x85EBCA6Bu));
	uvec2 xy = (p_cell.xy + uvec2(h_off & 63u, (h_off >> 6u) & 63u)) & uvec2(63u);
	uint t = (p_frame + p_cell.z + ((h_off >> 12u) & 31u)) & 31u;
	float stbn = texelFetch(sampler2DArray(ml_stbn_tex, ml_sampler_nearest_clamp), ivec3(int(xy.x), int(xy.y), int(t)), 0).r;
	uint h_sub = ml_froxel_pcg(p_cell.x ^ (p_cell.y * 0x9E3779B9u) ^ (p_cell.z * 0xC2B2AE35u) ^ (p_frame * 0x85EBCA6Bu) ^ (p_salt * 0x27D4EB2Fu));
	float bin = floor(stbn * 255.0 + 0.5);
	return (bin + ml_froxel_hash_float(h_sub)) * (1.0 / 256.0);
}

float ml_froxel_henyey_greenstein(float p_cos_theta, float p_g) {
	const float k = 0.07957747154594767; // 1 / (4 * PI)
	return k * (1.0 - p_g * p_g) / (pow(max(1.0 + p_g * p_g - 2.0 * p_g * p_cos_theta, 1e-4), 1.5));
}

float ml_froxel_get_omni_attenuation(float p_distance, float p_inv_range, float p_decay) {
	float nd = p_distance * p_inv_range;
	nd *= nd;
	nd *= nd;
	nd = max(1.0 - nd, 0.0);
	nd *= nd;
	return nd * pow(max(p_distance, 0.0001), -p_decay);
}

vec3 ml_froxel_view_to_world_pos(vec3 p_view_pos) {
	vec4 p = vec4(p_view_pos, 1.0);
	return vec3(dot(params.inv_view_rows[0], p), dot(params.inv_view_rows[1], p), dot(params.inv_view_rows[2], p));
}

vec3 ml_froxel_view_to_world_dir(vec3 p_view_dir) {
	return vec3(dot(params.inv_view_rows[0].xyz, p_view_dir), dot(params.inv_view_rows[1].xyz, p_view_dir), dot(params.inv_view_rows[2].xyz, p_view_dir));
}

vec3 ml_froxel_to_prev_view(vec3 p_view_pos) {
	vec4 p = vec4(p_view_pos, 1.0);
	return vec3(dot(params.to_prev_view_rows[0], p), dot(params.to_prev_view_rows[1], p), dot(params.to_prev_view_rows[2], p));
}

uint ml_froxel_pack_light(uint p_type, uint p_index) {
	return (p_index & ML_SAMPLE_INDEX_MASK) | ((p_type & ML_SAMPLE_TYPE_MASK) << ML_SAMPLE_TYPE_SHIFT);
}

uint ml_froxel_light_grid_cell(vec2 p_screen_uv, float p_linear_depth) {
	uvec2 screen_px = uvec2(clamp(p_screen_uv, vec2(0.0), vec2(0.99999)) * vec2(params.screen_frame_vsm.xy));
	uvec2 tile = min(screen_px >> ML_LIGHT_GRID_TILE_SHIFT, params.light_grid_dims.xy - uvec2(1u));
	float depth = max(p_linear_depth, params.light_grid_z_params.x);
	float z_f = ((params.light_grid_dims.w & ML_LIGHT_GRID_FLAG_ORTHOGONAL) != 0u)
			? (depth * params.light_grid_z_params.z + params.light_grid_z_params.w)
			: (log(depth) * params.light_grid_z_params.z + params.light_grid_z_params.w);
	uint tz = uint(clamp(floor(z_f), 0.0, float(params.light_grid_dims.z - 1u)));
	return (tz * params.light_grid_dims.y + tile.y) * params.light_grid_dims.x + tile.x;
}

// Analytical SH-L1 clamped-cosine projection for incoming radiance p_radiance from unit world direction p_dir_ws.
// Evaluated in Forward+ as: L0 + dot(normal_ws, L1) = (0.25 + 0.5 * dot(normal_ws, L_ws)) * E / PI.
void ml_froxel_project_sh_l1(vec3 p_dir_ws, vec3 p_radiance, inout vec4 r_sh_r, inout vec4 r_sh_g, inout vec4 r_sh_b) {
	const float c0 = 0.25 / M_PI;
	const float c1 = 0.5 / M_PI;
	vec3 l0 = p_radiance * c0;
	vec3 l1 = p_radiance * c1;
	r_sh_r += vec4(l0.r, l1.r * p_dir_ws.x, l1.r * p_dir_ws.y, l1.r * p_dir_ws.z);
	r_sh_g += vec4(l0.g, l1.g * p_dir_ws.x, l1.g * p_dir_ws.y, l1.g * p_dir_ws.z);
	r_sh_b += vec4(l0.b, l1.b * p_dir_ws.x, l1.b * p_dir_ws.y, l1.b * p_dir_ws.z);
}

struct MLFroxelLightEval {
	vec3 radiance; // unphased incoming radiance at froxel (includes color * att * ies * light_function)
	vec3 dir_vs; // unit direction toward light in view space
	vec3 dir_ws; // unit direction toward light in world space
	float ray_dist;
	float fog_energy;
	float shadow_opacity;
	uint vsm_map_base;
	uint vsm_flags;
	uint policy_index;
	bool valid;
};

MLFroxelLightEval ml_froxel_invalid_eval() {
	MLFroxelLightEval ev;
	ev.radiance = vec3(0.0);
	ev.dir_vs = vec3(0.0, 1.0, 0.0);
	ev.dir_ws = vec3(0.0, 1.0, 0.0);
	ev.ray_dist = 0.0;
	ev.fog_energy = 0.0;
	ev.shadow_opacity = 0.0;
	ev.vsm_map_base = 0u;
	ev.vsm_flags = 0u;
	ev.policy_index = 0u;
	ev.valid = false;
	return ev;
}

MLFroxelLightEval ml_froxel_eval_omni(uint p_idx, vec3 p_view_pos, vec3 p_world_pos, bool p_use_lf, float p_falloff_cull) {
	MLFroxelLightEval ev = ml_froxel_invalid_eval();
	LightData light = ml_omni_lights.data[p_idx];
	if ((light.light_flags & LIGHT_FLAG_MEGALIGHTS) == 0u) {
		return ev;
	}
	vec3 rel = light.position - p_view_pos;
	float d = length(rel);
	if (!(d * light.inv_radius < 1.0)) {
		return ev;
	}
	float att = ml_froxel_get_omni_attenuation(d, light.inv_radius, light.attenuation);
	if (!(att > 0.0) || att < p_falloff_cull) {
		return ev;
	}
	vec3 dir_vs = rel / max(d, 1e-6);
	float ies = 1.0;
	if (light.ies_layer != IES_LAYER_NONE) {
		vec3 lx = light.area_width;
		vec3 ly = light.area_height;
		vec3 lz = cross(lx, ly);
		vec3 dir_ls = vec3(dot(-dir_vs, lx), dot(-dir_vs, ly), dot(-dir_vs, lz));
		ies = ies_sample_atlas(light.ies_layer, light.ies_scale, dir_ls, ml_ies_profile_atlas_tex, ml_sampler_linear_clamp);
		if (!(ies > 0.0)) {
			return ev;
		}
	}
	vec3 color = light.color * (att * ies);
	if (p_use_lf && light.light_function_index != LIGHT_FUNCTION_NONE) {
		color *= light_function_mask(ml_light_function_data.data[light.light_function_index], ml_light_function_atlas_tex, ml_sampler_linear_clamp, p_world_pos);
	}
	ev.radiance = color;
	ev.dir_vs = dir_vs;
	ev.dir_ws = ml_froxel_safe_normalize(ml_froxel_view_to_world_dir(dir_vs));
	ev.ray_dist = max(d - 0.005, 0.0);
	ev.fog_energy = light.volumetric_fog_energy;
	ev.shadow_opacity = light.shadow_opacity;
	ev.vsm_map_base = light.vsm_map_base;
	ev.vsm_flags = light.vsm_flags;
	ev.policy_index = p_idx;
	ev.valid = true;
	return ev;
}

MLFroxelLightEval ml_froxel_eval_spot(uint p_idx, vec3 p_view_pos, vec3 p_world_pos, bool p_use_lf, float p_falloff_cull) {
	MLFroxelLightEval ev = ml_froxel_invalid_eval();
	LightData light = ml_spot_lights.data[p_idx];
	if ((light.light_flags & LIGHT_FLAG_MEGALIGHTS) == 0u) {
		return ev;
	}
	vec3 rel = light.position - p_view_pos;
	float d = length(rel);
	if (!(d * light.inv_radius < 1.0)) {
		return ev;
	}
	vec3 dir_vs = rel / max(d, 1e-6);
	float att = ml_froxel_get_omni_attenuation(d, light.inv_radius, light.attenuation);
	float cone_angle = light.cone_angle;
	float scos = max(dot(-dir_vs, light.direction), cone_angle);
	float spot_rim = max(1e-4, (1.0 - scos) / max(1.0 - cone_angle, 1e-6));
	att *= max(1.0 - pow(spot_rim, light.cone_attenuation), 0.0);
	if (!(att > 0.0) || att < p_falloff_cull) {
		return ev;
	}
	float ies = 1.0;
	if (light.ies_layer != IES_LAYER_NONE) {
		vec3 lx = light.area_width;
		vec3 ly = light.area_height;
		vec3 lz = -light.direction;
		vec3 dir_ls = vec3(dot(-dir_vs, lx), dot(-dir_vs, ly), dot(-dir_vs, lz));
		ies = ies_sample_atlas(light.ies_layer, light.ies_scale, dir_ls, ml_ies_profile_atlas_tex, ml_sampler_linear_clamp);
		if (!(ies > 0.0)) {
			return ev;
		}
	}
	vec3 color = light.color * (att * ies);
	if (p_use_lf && light.light_function_index != LIGHT_FUNCTION_NONE) {
		color *= light_function_mask(ml_light_function_data.data[light.light_function_index], ml_light_function_atlas_tex, ml_sampler_linear_clamp, p_world_pos);
	}
	ev.radiance = color;
	ev.dir_vs = dir_vs;
	ev.dir_ws = ml_froxel_safe_normalize(ml_froxel_view_to_world_dir(dir_vs));
	ev.ray_dist = max(d - 0.005, 0.0);
	ev.fog_energy = light.volumetric_fog_energy;
	ev.shadow_opacity = light.shadow_opacity;
	ev.vsm_map_base = light.vsm_map_base;
	ev.vsm_flags = light.vsm_flags;
	ev.policy_index = params.light_counts.x + p_idx;
	ev.valid = true;
	return ev;
}

MLFroxelLightEval ml_froxel_eval_area(uint p_idx, vec3 p_view_pos, vec3 p_world_pos, bool p_use_lf, float p_falloff_cull) {
	MLFroxelLightEval ev = ml_froxel_invalid_eval();
	LightData light = ml_area_lights.data[p_idx];
	if ((light.light_flags & LIGHT_FLAG_MEGALIGHTS) == 0u) {
		return ev;
	}
	vec3 area_width = light.area_width;
	vec3 area_height = light.area_height;
	if (dot(area_width, area_width) < 1e-4 || dot(area_height, area_height) < 1e-4) {
		return ev;
	}
	float a_len = length(area_width);
	float b_len = length(area_height);
	vec3 area_w_norm = area_width / a_len;
	vec3 area_h_norm = area_height / b_len;
	vec2 half_size = vec2(a_len * 0.5, b_len * 0.5);
	vec3 light_to_vert = p_view_pos - light.position;
	vec3 pos_local = vec3(dot(light_to_vert, area_w_norm), dot(light_to_vert, area_h_norm), dot(light_to_vert, -light.direction));
	vec2 clip_min;
	vec2 clip_max;
	if (!barn_door_clip_rect(pos_local, half_size, light.barn_door_angle_rad, light.barn_door_length, clip_min, clip_max)) {
		return ev;
	}
	vec3 closest_local = vec3(clamp(pos_local.x, clip_min.x, clip_max.x), clamp(pos_local.y, clip_min.y, clip_max.y), 0.0);
	vec3 closest_vs = light.position + closest_local.x * area_w_norm + closest_local.y * area_h_norm;
	vec3 rel = closest_vs - p_view_pos;
	float d_raw = length(rel);
	vec3 dir_vs = d_raw > 1e-4 ? (rel / d_raw) : light.direction;
	float d = max(d_raw, 1.0);
	float inv_center_range = light.cone_attenuation;
	if (!(d * inv_center_range < 1.0)) {
		return ev;
	}
	float att = ml_froxel_get_omni_attenuation(d, light.inv_radius, light.attenuation - 1.0);
	float cutoff = smoothstep(0.0, 0.1, dot(light.direction, p_view_pos - light.position));
	if (!(att * cutoff > 0.0) || att * cutoff < p_falloff_cull) {
		return ev;
	}
	vec3 area_rel_center = light.position - p_view_pos;
	vec3 min_x = area_w_norm * clip_min.x;
	vec3 max_x = area_w_norm * clip_max.x;
	vec3 min_y = area_h_norm * clip_min.y;
	vec3 max_y = area_h_norm * clip_max.y;
	vec3 light_points[4];
	light_points[0] = area_rel_center + min_x + min_y;
	light_points[1] = area_rel_center + max_x + min_y;
	light_points[2] = area_rel_center + max_x + max_y;
	light_points[3] = area_rel_center + min_x + max_y;
	vec4 tex_rect = barn_door_clip_texture_rect(light.projector_rect, half_size, clip_min, clip_max);
	float ltc_diffuse = 0.0;
	vec3 texture_color = vec3(1.0);
	ltc_evaluate_diff(dir_vs, light_points, tex_rect, light.cone_angle, ml_area_light_atlas_tex, ml_sampler_linear_mips_clamp, ltc_diffuse, texture_color);
	float area_ies = 1.0;
	if (light.ies_layer != IES_LAYER_NONE) {
		area_ies = ies_sample_atlas(light.ies_layer, light.ies_scale, ml_froxel_safe_normalize(pos_local), ml_ies_profile_atlas_tex, ml_sampler_linear_clamp);
		if (!(area_ies > 0.0)) {
			return ev;
		}
	}
	vec3 color = light.color * texture_color * (att * ltc_diffuse * cutoff * area_ies);
	if (p_use_lf && light.light_function_index != LIGHT_FUNCTION_NONE) {
		color *= light_function_mask(ml_light_function_data.data[light.light_function_index], ml_light_function_atlas_tex, ml_sampler_linear_clamp, p_world_pos);
	}
	ev.radiance = color;
	ev.dir_vs = dir_vs;
	ev.dir_ws = ml_froxel_safe_normalize(ml_froxel_view_to_world_dir(dir_vs));
	ev.ray_dist = max(d_raw - 0.005, 0.0);
	ev.fog_energy = light.volumetric_fog_energy;
	ev.shadow_opacity = light.shadow_opacity;
	ev.vsm_map_base = light.vsm_map_base;
	ev.vsm_flags = light.vsm_flags;
	ev.policy_index = params.light_counts.x + params.light_counts.y + p_idx;
	ev.valid = true;
	return ev;
}

MLFroxelLightEval ml_froxel_eval_directional(uint p_idx, vec3 p_world_pos, bool p_use_lf) {
	MLFroxelLightEval ev = ml_froxel_invalid_eval();
	DirectionalLightData light = ml_directional_lights.data[p_idx];
	if ((light.light_flags & LIGHT_FLAG_MEGALIGHTS) == 0u) {
		return ev;
	}
	vec3 color = light.color * light.energy;
	if (p_use_lf && light.light_function_index != LIGHT_FUNCTION_NONE) {
		color *= light_function_mask(ml_light_function_data.data[light.light_function_index], ml_light_function_atlas_tex, ml_sampler_linear_clamp, p_world_pos);
	}
	vec3 dir_vs = ml_froxel_safe_normalize(light.direction);
	ev.radiance = color;
	ev.dir_vs = dir_vs;
	ev.dir_ws = ml_froxel_safe_normalize(ml_froxel_view_to_world_dir(dir_vs));
	ev.ray_dist = 1000.0;
	ev.fog_energy = light.volumetric_fog_energy;
	ev.shadow_opacity = light.shadow_opacity;
	ev.vsm_map_base = light.vsm_map_base;
	ev.vsm_flags = light.vsm_flags;
	ev.policy_index = params.light_counts.x + params.light_counts.y + params.light_counts.z + p_idx;
	ev.valid = true;
	return ev;
}

MLFroxelLightEval ml_froxel_eval_packed(uint p_packed, vec3 p_view_pos, vec3 p_world_pos, bool p_use_lf, float p_falloff_cull) {
	uint ltype = (p_packed >> ML_SAMPLE_TYPE_SHIFT) & ML_SAMPLE_TYPE_MASK;
	uint lidx = p_packed & ML_SAMPLE_INDEX_MASK;
	if (ltype == ML_LIGHT_TYPE_OMNI && lidx < params.light_counts.x) {
		return ml_froxel_eval_omni(lidx, p_view_pos, p_world_pos, p_use_lf, p_falloff_cull);
	}
	if (ltype == ML_LIGHT_TYPE_SPOT && lidx < params.light_counts.y) {
		return ml_froxel_eval_spot(lidx, p_view_pos, p_world_pos, p_use_lf, p_falloff_cull);
	}
	if (ltype == ML_LIGHT_TYPE_AREA && lidx < params.light_counts.z) {
		return ml_froxel_eval_area(lidx, p_view_pos, p_world_pos, p_use_lf, p_falloff_cull);
	}
	if (ltype == ML_LIGHT_TYPE_DIRECTIONAL && lidx < params.light_counts.w && lidx < MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS) {
		return ml_froxel_eval_directional(lidx, p_world_pos, p_use_lf);
	}
	return ml_froxel_invalid_eval();
}

// Provided by ml_froxel.glsl (HWRT ray query) or ml_froxel_compute.glsl (VSM / fallback).
float ml_froxel_trace_visibility(vec3 p_world_pos, float p_linear_depth, uint p_light_type, MLFroxelLightEval p_ev, bool p_use_vsm, bool p_force_visible);

void ml_froxel_sample_main() {
	uvec3 pos = gl_GlobalInvocationID.xyz;
	uvec3 grid_dims = params.grid_dims.xyz;
	if (pos.x >= grid_dims.x || pos.y >= grid_dims.y || pos.z >= grid_dims.z) {
		return;
	}

	uint flags = params.grid_dims.w;
	bool fog_active = (flags & ML_FROXEL_FLAG_FOG_ACTIVE) != 0u;
	bool d17_direct_write = (flags & ML_FROXEL_FLAG_D17_DIRECT_WRITE) != 0u;
	bool write_sh = (flags & ML_FROXEL_FLAG_WRITE_SH) != 0u;
	bool history_valid = (flags & ML_FROXEL_FLAG_HISTORY_VALID) != 0u;
	bool use_lf = (flags & ML_FROXEL_FLAG_USE_LIGHT_FUNCTION) != 0u;
	bool use_vsm = (flags & ML_FROXEL_FLAG_USE_VSM) != 0u;
	bool force_visible = (flags & ML_FROXEL_FLAG_FORCE_VISIBLE_ONE) != 0u;
	uint frame = params.screen_frame_vsm.z;
	float falloff_cull = params.d17_params.w;

	// Reconstruct froxel center view & world position (matching volumetric_fog_process.glsl for fog,
	// or nextgi_wrc_update_inc.glsl for D17-only mode).
	vec3 unit_pos = (vec3(pos) + vec3(0.5)) / vec3(grid_dims);
	vec2 screen_uv = unit_pos.xy;
	vec3 view_pos;
	vec3 world_pos;
	float linear_depth;

	if (fog_active) {
		float z_spread = pow(unit_pos.z, params.fog_z_params.y);
		view_pos.xy = (unit_pos.xy * 2.0 - vec2(1.0)) * mix(params.fog_frustum_size.xy, params.fog_frustum_size.zw, vec2(z_spread));
		view_pos.z = -params.fog_z_params.x * z_spread;
		view_pos.y = -view_pos.y;
		world_pos = ml_froxel_view_to_world_pos(view_pos);
		linear_depth = max(-view_pos.z, 0.01);
	} else {
		float d17_near = params.d17_params.x;
		float d17_far = params.d17_params.y;
		linear_depth = d17_near * pow(d17_far / d17_near, unit_pos.z);
		world_pos = nextgi_view_reconstruct(params.inv_view_proj, screen_uv, nextgi_view_depth_from_linear(params.depth_coeffs, linear_depth));
		// Convert world_pos to view_pos via transpose of inv_view_rows rotation (R^T * (world - cam_origin)).
		vec3 cam_origin = vec3(params.inv_view_rows[0].w, params.inv_view_rows[1].w, params.inv_view_rows[2].w);
		vec3 delta_ws = world_pos - cam_origin;
		view_pos = vec3(
				params.inv_view_rows[0].x * delta_ws.x + params.inv_view_rows[1].x * delta_ws.y + params.inv_view_rows[2].x * delta_ws.z,
				params.inv_view_rows[0].y * delta_ws.x + params.inv_view_rows[1].y * delta_ws.y + params.inv_view_rows[2].y * delta_ws.z,
				params.inv_view_rows[0].z * delta_ws.x + params.inv_view_rows[1].z * delta_ws.y + params.inv_view_rows[2].z * delta_ws.z);
	}

	uint spp = clamp(params.d17_dims.w, 1u, ML_FROXEL_MAX_SAMPLES);
	uint res_light[ML_FROXEL_MAX_SAMPLES];
	float res_w_sum[ML_FROXEL_MAX_SAMPLES];
	float res_target[ML_FROXEL_MAX_SAMPLES];
	for (uint s = 0u; s < spp; s++) {
		res_light[s] = ML_SAMPLE_INVALID;
		res_w_sum[s] = 0.0;
		res_target[s] = 0.0;
	}

	// Stream all MegaLights-eligible lights affecting this froxel into S weighted reservoirs.
	bool use_packed_grid = params.fog_dims_pad.w != 0u && params.light_grid_dims.x > 0u && params.light_grid_dims.y > 0u && params.light_grid_dims.z > 0u;
	uint candidate_idx = 0u;

	if (use_packed_grid) {
		uint cell = ml_froxel_light_grid_cell(screen_uv, linear_depth);
		uvec2 cell_header = ml_light_grid_headers.data[cell];
		uint offset = cell_header.x;
		uint total_local = cell_header.y;
		for (uint i = 0u; i < total_local; i++) {
			uint entry = ml_light_grid_indices.data[offset + i];
			// Skip lights excluded via Light3D.megalights = DISALLOW (#143 bit 18).
			if ((entry & (1u << 18u)) == 0u) {
				continue;
			}
			uint ltype = (entry >> ML_SAMPLE_TYPE_SHIFT) & ML_SAMPLE_TYPE_MASK;
			uint lidx = entry & ML_SAMPLE_INDEX_MASK;
			uint packed = ml_froxel_pack_light(ltype, lidx);
			MLFroxelLightEval ev = ml_froxel_eval_packed(packed, view_pos, world_pos, use_lf, falloff_cull);
			if (!ev.valid) {
				continue;
			}
			float weight_scale = fog_active ? (write_sh ? max(ev.fog_energy, 1.0) : ev.fog_energy) : 1.0;
			float w = ml_froxel_luminance(ev.radiance) * weight_scale;
			if (!(w > 0.0)) {
				continue;
			}
			for (uint s = 0u; s < spp; s++) {
				res_w_sum[s] += w;
				float u = ml_froxel_stbn_u(pos, frame, (candidate_idx + 1u) * 193u + s * 997u + 17u);
				if (u * res_w_sum[s] < w) {
					res_light[s] = packed;
					res_target[s] = w;
				}
			}
			candidate_idx++;
		}
	} else {
		for (uint i = 0u; i < params.light_counts.x; i++) {
			MLFroxelLightEval ev = ml_froxel_eval_omni(i, view_pos, world_pos, use_lf, falloff_cull);
			if (!ev.valid) {
				continue;
			}
			float weight_scale = fog_active ? (write_sh ? max(ev.fog_energy, 1.0) : ev.fog_energy) : 1.0;
			float w = ml_froxel_luminance(ev.radiance) * weight_scale;
			if (!(w > 0.0)) {
				continue;
			}
			uint packed = ml_froxel_pack_light(ML_LIGHT_TYPE_OMNI, i);
			for (uint s = 0u; s < spp; s++) {
				res_w_sum[s] += w;
				float u = ml_froxel_stbn_u(pos, frame, (candidate_idx + 1u) * 193u + s * 997u + 17u);
				if (u * res_w_sum[s] < w) {
					res_light[s] = packed;
					res_target[s] = w;
				}
			}
			candidate_idx++;
		}
		for (uint i = 0u; i < params.light_counts.y; i++) {
			MLFroxelLightEval ev = ml_froxel_eval_spot(i, view_pos, world_pos, use_lf, falloff_cull);
			if (!ev.valid) {
				continue;
			}
			float weight_scale = fog_active ? (write_sh ? max(ev.fog_energy, 1.0) : ev.fog_energy) : 1.0;
			float w = ml_froxel_luminance(ev.radiance) * weight_scale;
			if (!(w > 0.0)) {
				continue;
			}
			uint packed = ml_froxel_pack_light(ML_LIGHT_TYPE_SPOT, i);
			for (uint s = 0u; s < spp; s++) {
				res_w_sum[s] += w;
				float u = ml_froxel_stbn_u(pos, frame, (candidate_idx + 1u) * 193u + s * 997u + 17u);
				if (u * res_w_sum[s] < w) {
					res_light[s] = packed;
					res_target[s] = w;
				}
			}
			candidate_idx++;
		}
		for (uint i = 0u; i < params.light_counts.z; i++) {
			MLFroxelLightEval ev = ml_froxel_eval_area(i, view_pos, world_pos, use_lf, falloff_cull);
			if (!ev.valid) {
				continue;
			}
			float weight_scale = fog_active ? (write_sh ? max(ev.fog_energy, 1.0) : ev.fog_energy) : 1.0;
			float w = ml_froxel_luminance(ev.radiance) * weight_scale;
			if (!(w > 0.0)) {
				continue;
			}
			uint packed = ml_froxel_pack_light(ML_LIGHT_TYPE_AREA, i);
			for (uint s = 0u; s < spp; s++) {
				res_w_sum[s] += w;
				float u = ml_froxel_stbn_u(pos, frame, (candidate_idx + 1u) * 193u + s * 997u + 17u);
				if (u * res_w_sum[s] < w) {
					res_light[s] = packed;
					res_target[s] = w;
				}
			}
			candidate_idx++;
		}
	}

	uint dir_count = min(params.light_counts.w, uint(MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS));
	for (uint i = 0u; i < dir_count; i++) {
		MLFroxelLightEval ev = ml_froxel_eval_directional(i, world_pos, use_lf);
		if (!ev.valid) {
			continue;
		}
		float weight_scale = fog_active ? (write_sh ? max(ev.fog_energy, 1.0) : ev.fog_energy) : 1.0;
		float w = ml_froxel_luminance(ev.radiance) * weight_scale;
		if (!(w > 0.0)) {
			continue;
		}
		uint packed = ml_froxel_pack_light(ML_LIGHT_TYPE_DIRECTIONAL, i);
		for (uint s = 0u; s < spp; s++) {
			res_w_sum[s] += w;
			float u = ml_froxel_stbn_u(pos, frame, (candidate_idx + 1u) * 193u + s * 997u + 17u);
			if (u * res_w_sum[s] < w) {
				res_light[s] = packed;
				res_target[s] = w;
			}
		}
		candidate_idx++;
	}

	// Evaluate visibility ONCE per selected stochastic sample and feed BOTH fog in-scattering
	// and D17 direct SH-L1 projection from the exact same sample set (Contract M7, GM-6).
	vec3 cur_fog = vec3(0.0);
	vec4 cur_sh_r = vec4(0.0);
	vec4 cur_sh_g = vec4(0.0);
	vec4 cur_sh_b = vec4(0.0);
	vec3 view_dir_away = ml_froxel_safe_normalize(view_pos);

	for (uint s = 0u; s < spp; s++) {
		uint packed = res_light[s];
		if (packed == ML_SAMPLE_INVALID || !(res_target[s] > 0.0)) {
			continue;
		}
		float inv_pdf = res_w_sum[s] / res_target[s];
		MLFroxelLightEval ev = ml_froxel_eval_packed(packed, view_pos, world_pos, use_lf, falloff_cull);
		if (!ev.valid) {
			continue;
		}
		uint ltype = (packed >> ML_SAMPLE_TYPE_SHIFT) & ML_SAMPLE_TYPE_MASK;
		float vis = ml_froxel_trace_visibility(world_pos, linear_depth, ltype, ev, use_vsm, force_visible);
		float shadow_att = mix(1.0 - ev.shadow_opacity, 1.0, vis);
		vec3 weighted_rad = ev.radiance * (shadow_att * inv_pdf);

		if (fog_active && ev.fog_energy > 0.001) {
			float phase = ml_froxel_henyey_greenstein(dot(ev.dir_vs, view_dir_away), params.fog_z_params.z);
			cur_fog += weighted_rad * (phase * ev.fog_energy);
		}
		if (write_sh) {
			ml_froxel_project_sh_l1(ev.dir_ws, weighted_rad, cur_sh_r, cur_sh_g, cur_sh_b);
		}
	}

	float inv_spp = 1.0 / float(spp);
	cur_fog *= inv_spp;
	cur_sh_r *= inv_spp;
	cur_sh_g *= inv_spp;
	cur_sh_b *= inv_spp;

	// Temporal reprojection & clamped history accumulation (reset cleanly on camera cut/resize).
	vec3 prev_uvw = vec3(-1.0);
	bool prev_valid = false;
	if (history_valid) {
		if (fog_active) {
			vec3 pv = ml_froxel_to_prev_view(view_pos);
			pv.y = -pv.y;
			float pz = pv.z / (-params.fog_z_params.x);
			if (pz > 0.0 && pz < 1.0) {
				vec2 frustum_sz = mix(params.fog_frustum_size.xy, params.fog_frustum_size.zw, vec2(pz));
				pv.xy = (pv.xy / max(frustum_sz, vec2(1e-5))) * 0.5 + vec2(0.5);
				pv.z = pow(pz, 1.0 / max(params.fog_z_params.y, 1e-4));
				if (all(greaterThan(pv, vec3(0.0))) && all(lessThan(pv, vec3(1.0)))) {
					prev_uvw = pv;
					prev_valid = true;
				}
			}
		} else {
			vec3 prev_screen = nextgi_view_project(params.prev_view_proj, world_pos);
			if (prev_screen.z >= 0.0) {
				float prev_depth = nextgi_view_linear_depth(params.depth_coeffs, prev_screen.z);
				float prev_slice = log(max(prev_depth, params.d17_params.x) / params.d17_params.x) / log(params.d17_params.y / params.d17_params.x);
				vec3 pv = vec3(prev_screen.xy, prev_slice);
				if (all(greaterThanEqual(pv, vec3(0.0))) && all(lessThanEqual(pv, vec3(1.0)))) {
					prev_uvw = pv;
					prev_valid = true;
				}
			}
		}
	}

	float blend = prev_valid ? clamp(params.d17_params.z, 0.0, 0.95) : 0.0;

	if (fog_active) {
		vec4 prev_fog = prev_valid ? textureLod(sampler3D(ml_froxel_fog_prev, ml_sampler_linear_clamp), prev_uvw, 0.0) : vec4(0.0);
		float prev_count = prev_valid ? clamp(prev_fog.a, 0.0, 16.0) : 0.0;
		float fog_blend = prev_count > 0.5 ? min(prev_count / (prev_count + 1.0), blend) : 0.0;
		vec3 accum_fog = mix(cur_fog, max(prev_fog.rgb, vec3(0.0)), fog_blend);
		accum_fog = clamp(accum_fog, vec3(0.0), vec3(65000.0));
		if (any(isnan(accum_fog)) || any(isinf(accum_fog))) {
			accum_fog = vec3(0.0);
		}
		imageStore(ml_froxel_fog_out, ivec3(pos), vec4(accum_fog, min(prev_count + 1.0, 16.0)));
	}

	if (write_sh) {
		vec4 prev_r = prev_valid ? textureLod(sampler3D(ml_froxel_sh_r_prev, ml_sampler_linear_clamp), prev_uvw, 0.0) : vec4(0.0);
		vec4 prev_g = prev_valid ? textureLod(sampler3D(ml_froxel_sh_g_prev, ml_sampler_linear_clamp), prev_uvw, 0.0) : vec4(0.0);
		vec4 prev_b = prev_valid ? textureLod(sampler3D(ml_froxel_sh_b_prev, ml_sampler_linear_clamp), prev_uvw, 0.0) : vec4(0.0);
		vec4 out_r = clamp(mix(cur_sh_r, prev_r, blend), vec4(-65000.0), vec4(65000.0));
		vec4 out_g = clamp(mix(cur_sh_g, prev_g, blend), vec4(-65000.0), vec4(65000.0));
		vec4 out_b = clamp(mix(cur_sh_b, prev_b, blend), vec4(-65000.0), vec4(65000.0));
		if (any(isnan(out_r)) || any(isinf(out_r))) {
			out_r = vec4(0.0);
		}
		if (any(isnan(out_g)) || any(isinf(out_g))) {
			out_g = vec4(0.0);
		}
		if (any(isnan(out_b)) || any(isinf(out_b))) {
			out_b = vec4(0.0);
		}
		imageStore(ml_froxel_sh_r_out, ivec3(pos), out_r);
		imageStore(ml_froxel_sh_g_out, ivec3(pos), out_g);
		imageStore(ml_froxel_sh_b_out, ivec3(pos), out_b);
	}
}

#endif // !ML_FROXEL_RESAMPLE_ONLY

#endif // ML_FROXEL_INC_GLSL
