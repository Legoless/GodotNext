#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: megalights_visibility (#146, plan §21.3 contract M4, GM-1, GM-12).
// VSM-method light visibility pass for MegaLights samples belonging to lights with
// Light3D.megalights_shadow_method == VIRTUAL. Skips ray-query dispatches completely and calls
// #222 vsm_shadow_directional() / vsm_shadow_local() with SMRT rays = 2.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

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
#define ML_MAX_SAMPLES_PER_PIXEL 16u
#define ML_HIT_DIST_MISS 65504.0

#include "../light_data_inc.glsl"
#include "../vsm_mark_inc.glsl"

struct MLVisParams {
	mat4 inv_projection;
	mat4 depth_projection;
	vec4 inv_view_rows[3];
	uvec4 screen_sample_size;
	uvec4 sample_config;
	uvec4 light_counts;
	vec4 screen_trace_params;
	uvec4 trace_config;
	vec4 rt_params;
};

struct VSMProjectionParams {
	vec4 footprint;
	ivec4 bias;
	uvec4 counts; // x = table_maps, y = rays_local (2), z = rays_directional (2), w = steps
	uvec4 misc; // x = prefiltered_distant
};

layout(set = 0, binding = 0, std140) uniform MLVisParamsUBO {
	MLVisParams params;
};

layout(set = 0, binding = 1) uniform texture2D ml_depth_tex;
layout(set = 0, binding = 2) uniform texture2D ml_normal_roughness_tex;
layout(set = 0, binding = 3) uniform sampler ml_sampler_nearest_clamp;

layout(set = 0, binding = 4, std430) restrict readonly buffer MLSampleBufferIn {
	uvec4 data[];
}
ml_sample_buffer;

layout(set = 0, binding = 5, std430) restrict readonly buffer MLVisCountersIn {
	uint data[];
}
ml_vis_counters;

layout(set = 0, binding = 6, std430) restrict readonly buffer MLVsmListIn {
	uint data[];
}
ml_vsm_list;

layout(set = 0, binding = 7, std430) restrict writeonly buffer MLVisibilityOut {
	uint data[];
}
ml_visibility_buffer;

layout(r16f, set = 0, binding = 8) uniform restrict writeonly image2DArray ml_hit_distance_img;

layout(set = 0, binding = 9, std430) restrict readonly buffer MLOmniLights {
	LightData data[];
}
ml_omni_lights;

layout(set = 0, binding = 10, std430) restrict readonly buffer MLSpotLights {
	LightData data[];
}
ml_spot_lights;

layout(set = 0, binding = 11, std430) restrict readonly buffer MLAreaLights {
	LightData data[];
}
ml_area_lights;

layout(set = 0, binding = 12, std140) uniform MLDirectionalLights {
	DirectionalLightData data[MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS];
}
ml_directional_lights;

// VSM sampling resources (#222 vsm_smrt_inc.glsl).
layout(set = 0, binding = 13) uniform utexture2DArray vsm_pool_tex;
layout(set = 0, binding = 14, std430) restrict readonly buffer VSMPageTable {
	uint entries[];
}
vsm_page_table;
layout(set = 0, binding = 15, std430) restrict readonly buffer VSMMaps {
	VSMMapData data[];
}
vsm_maps;
layout(set = 0, binding = 16, std430) restrict readonly buffer VSMMapState {
	uint data[];
}
vsm_map_state;
layout(set = 0, binding = 17, std430) restrict readonly buffer VSMProjection {
	VSMProjectionParams data;
}
vsm_projection;
layout(set = 0, binding = 18) uniform texture2D vsm_blue_noise;
layout(set = 0, binding = 19, std430) restrict readonly buffer VSMCoarseMiss {
	uint data[];
}
vsm_coarse_miss;

#define VSM_NO_COARSE_MISS_ATOMICS
#define VSM_COARSE_NEAREST_SAMPLER ml_sampler_nearest_clamp
#define SAMPLER_NEAREST_CLAMP ml_sampler_nearest_clamp

bool sc_use_vsm_coarse() {
	return false;
}

#include "../vsm_page_table_inc.glsl"
#include "../vsm_raster_inc.glsl"
#include "../vsm_coarse_inc.glsl"
#include "../vsm_smrt_inc.glsl"

uvec2 ml_vsm_downsample_offset(uint p_phase) {
	uint p = p_phase & 3u;
	if (p == 1u) {
		return uvec2(1u, 1u);
	}
	if (p == 2u) {
		return uvec2(1u, 0u);
	}
	if (p == 3u) {
		return uvec2(0u, 1u);
	}
	return uvec2(0u, 0u);
}

uvec2 ml_vsm_sample_to_full_pixel(uvec2 p_sample_px, uvec2 p_full_size, bool p_downsample_half, uint p_frame_index) {
	if (!p_downsample_half) {
		return min(p_sample_px, p_full_size - uvec2(1u));
	}
	return min(p_sample_px * 2u + ml_vsm_downsample_offset(p_frame_index), p_full_size - uvec2(1u));
}

vec3 ml_vsm_view_to_world_pos(vec3 p_view_pos) {
	vec4 p = vec4(p_view_pos, 1.0);
	return vec3(dot(params.inv_view_rows[0], p), dot(params.inv_view_rows[1], p), dot(params.inv_view_rows[2], p));
}

vec3 ml_vsm_view_to_world_dir(vec3 p_view_dir) {
	return vec3(dot(params.inv_view_rows[0].xyz, p_view_dir), dot(params.inv_view_rows[1].xyz, p_view_dir), dot(params.inv_view_rows[2].xyz, p_view_dir));
}

void main() {
	uint vsm_idx = gl_GlobalInvocationID.x;
	uint vsm_count = min(ml_vis_counters.data[1], params.trace_config.w);
	if (vsm_idx >= vsm_count) {
		return;
	}

	uint s_idx = ml_vsm_list.data[vsm_idx];
	uint spp = clamp(params.sample_config.x, 1u, ML_MAX_SAMPLES_PER_PIXEL);
	uint sample_w = params.screen_sample_size.z;
	uint px_flat = s_idx / spp;
	uint k = s_idx - px_flat * spp;
	uvec2 spx = uvec2(px_flat % sample_w, px_flat / sample_w);

	bool downsample_half = params.sample_config.y != 0u;
	uvec2 full_size = params.screen_sample_size.xy;
	uvec2 fpx = ml_vsm_sample_to_full_pixel(spx, full_size, downsample_half, params.sample_config.z);

	float depth = texelFetch(sampler2D(ml_depth_tex, ml_sampler_nearest_clamp), ivec2(fpx), 0).r;
	uvec4 samp = ml_sample_buffer.data[s_idx];
	if (!(depth > 0.0) || samp.x == ML_SAMPLE_INVALID) {
		ml_visibility_buffer.data[s_idx] = 0xFFFFFFFFu;
		imageStore(ml_hit_distance_img, ivec3(int(spx.x), int(spx.y), int(k)), vec4(ML_HIT_DIST_MISS, 0.0, 0.0, 0.0));
		return;
	}

	vec2 uv = (vec2(fpx) + vec2(0.5)) / vec2(full_size);
	vec4 view_h = params.inv_projection * vec4(uv * 2.0 - vec2(1.0), depth, 1.0);
	vec3 view_pos = view_h.xyz / view_h.w;
	vec3 world_pos = ml_vsm_view_to_world_pos(view_pos);
	float view_depth = max(-view_pos.z, 1e-4);

	vec4 nr = texelFetch(sampler2D(ml_normal_roughness_tex, ml_sampler_nearest_clamp), ivec2(fpx), 0);
	vec3 n_vs = nr.xyz * 2.0 - vec3(1.0);
	float n_len2 = dot(n_vs, n_vs);
	vec3 normal_vs = n_len2 > 1e-12 ? n_vs * inversesqrt(n_len2) : vec3(0.0, 0.0, 1.0);
	vec3 world_normal = normalize(ml_vsm_view_to_world_dir(normal_vs));

	uint ltype = (samp.x >> ML_SAMPLE_TYPE_SHIFT) & ML_SAMPLE_TYPE_MASK;
	uint lidx = samp.x & ML_SAMPLE_INDEX_MASK;
	vec2 noise = vsm_blue_noise_sample(fpx, float(params.sample_config.z + k * 13u));

	float vis = 1.0;
	float hit_t = ML_HIT_DIST_MISS;

	if (ltype == ML_LIGHT_TYPE_DIRECTIONAL) {
		if (lidx < min(params.light_counts.w, uint(MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS))) {
			DirectionalLightData dl = ml_directional_lights.data[lidx];
			float cos_angle = max(1.0 - dl.size, 0.0);
			float tan_half = cos_angle < 1.0 ? sqrt(max(1.0 - cos_angle * cos_angle, 0.0)) / max(cos_angle, 1e-4) : 0.0;
			// Godot stores angular diameter in size = 1 - cos(diameter); half angle tan is ~0.5 * tan(diameter).
			tan_half *= 0.5;
			vis = vsm_shadow_directional(dl.vsm_map_base, dl.vsm_flags, world_pos, world_normal, tan_half, view_depth, noise);
			if (vis < 0.999) {
				float dist = vsm_transmittance_directional(dl.vsm_map_base, dl.vsm_flags, world_pos, view_depth);
				if (dist >= 0.0) {
					hit_t = min(dist, ML_HIT_DIST_MISS);
				}
			}
		}
	} else if (ltype == ML_LIGHT_TYPE_OMNI) {
		if (lidx < params.light_counts.x) {
			LightData ld = ml_omni_lights.data[lidx];
			vec3 light_ws = ml_vsm_view_to_world_pos(ld.position);
			vis = vsm_shadow_local(ld.vsm_map_base, ld.vsm_flags, world_pos, world_normal, light_ws, ld.size, vec3(0.0), vec3(0.0), view_depth, noise);
			if (vis < 0.999) {
				float dist = vsm_transmittance_local(ld.vsm_map_base, ld.vsm_flags, world_pos, view_depth);
				if (dist >= 0.0) {
					hit_t = min(dist, ML_HIT_DIST_MISS);
				}
			}
		}
	} else if (ltype == ML_LIGHT_TYPE_SPOT) {
		if (lidx < params.light_counts.y) {
			LightData ld = ml_spot_lights.data[lidx];
			vec3 light_ws = ml_vsm_view_to_world_pos(ld.position);
			vis = vsm_shadow_local(ld.vsm_map_base, ld.vsm_flags, world_pos, world_normal, light_ws, ld.size, vec3(0.0), vec3(0.0), view_depth, noise);
			if (vis < 0.999) {
				float dist = vsm_transmittance_local(ld.vsm_map_base, ld.vsm_flags, world_pos, view_depth);
				if (dist >= 0.0) {
					hit_t = min(dist, ML_HIT_DIST_MISS);
				}
			}
		}
	} else if (ltype == ML_LIGHT_TYPE_AREA) {
		if (lidx < params.light_counts.z) {
			LightData ld = ml_area_lights.data[lidx];
			vec3 light_ws = ml_vsm_view_to_world_pos(ld.position);
			vec3 area_a = ml_vsm_view_to_world_dir(ld.area_width);
			vec3 area_b = ml_vsm_view_to_world_dir(ld.area_height);
			vis = vsm_shadow_local(ld.vsm_map_base, ld.vsm_flags, world_pos, world_normal, light_ws, ld.size, area_a, area_b, view_depth, noise);
			if (vis < 0.999) {
				float dist = vsm_transmittance_local(ld.vsm_map_base, ld.vsm_flags, world_pos, view_depth);
				if (dist >= 0.0) {
					hit_t = min(dist, ML_HIT_DIST_MISS);
				}
			}
		}
	}

	vis = clamp(vis, 0.0, 1.0);
	ml_visibility_buffer.data[s_idx] = packUnorm4x8(vec4(vis, vis, vis, 1.0));
	imageStore(ml_hit_distance_img, ivec3(int(spx.x), int(spx.y), int(k)), vec4(hit_t, 0.0, 0.0, 0.0));
}
