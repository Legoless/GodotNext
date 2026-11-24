#[compute]

#version 460

#VERSION_DEFINES

// GodotNext: megalights_visibility (#146, plan §21.3 contract M4, GM-1, GM-12).
// Optional SWRT continuation shader for MegaLights on the non-RT tier (labeled extension):
// traces unresolved compacted rays from ml_ray_list against #182 global SDF clipmaps via
// nextgi_sdf_trace_inc.glsl. Uses plain compute only so non-RT devices compile cleanly.

#define EXPLICIT_FP16
#define NEXTGI_TRACE_SDF
#define SWRT_SET 1
#define SWRT_EXTERNAL_VIEW_INPUTS

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#include "../half_inc.glsl"
#include "../environment/nextgi_sdf_trace_inc.glsl"

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

layout(set = 0, binding = 5, std430) restrict readonly buffer MLLightShadowPolicies {
	uvec4 data[];
}
ml_light_shadow_policies;

layout(set = 0, binding = 6, std430) restrict buffer MLVisCounters {
	uint data[];
}
ml_vis_counters;

layout(set = 0, binding = 7, std430) restrict buffer MLRayListRW {
	uvec2 data[];
}
ml_ray_list;

vec3 ml_swrt_oct_decode(vec2 p_e) {
	vec3 n = vec3(p_e.x, p_e.y, 1.0 - abs(p_e.x) - abs(p_e.y));
	if (n.z < 0.0) {
		vec2 s = vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
		n.xy = (vec2(1.0) - abs(n.yx)) * s;
	}
	return normalize(n);
}

vec3 ml_swrt_unpack_oct(uint p_word) {
	return ml_swrt_oct_decode(unpackSnorm2x16(p_word));
}

uvec2 ml_swrt_downsample_offset(uint p_phase) {
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

uvec2 ml_swrt_sample_to_full_pixel(uvec2 p_sample_px, uvec2 p_full_size, bool p_downsample_half, uint p_frame_index) {
	if (!p_downsample_half) {
		return min(p_sample_px, p_full_size - uvec2(1u));
	}
	return min(p_sample_px * 2u + ml_swrt_downsample_offset(p_frame_index), p_full_size - uvec2(1u));
}

vec3 ml_swrt_view_to_world_pos(vec3 p_view_pos) {
	vec4 p = vec4(p_view_pos, 1.0);
	return vec3(dot(params.inv_view_rows[0], p), dot(params.inv_view_rows[1], p), dot(params.inv_view_rows[2], p));
}

vec3 ml_swrt_view_to_world_dir(vec3 p_view_dir) {
	return vec3(dot(params.inv_view_rows[0].xyz, p_view_dir), dot(params.inv_view_rows[1].xyz, p_view_dir), dot(params.inv_view_rows[2].xyz, p_view_dir));
}

void main() {
	uint ray_idx = gl_GlobalInvocationID.x;
	uint ray_count = min(ml_vis_counters.data[0], params.trace_config.z);
	if (ray_idx >= ray_count) {
		return;
	}

	uvec2 ray_in = ml_ray_list.data[ray_idx];
	uint s_idx = ray_in.x;
	float t_resume = max(uintBitsToFloat(ray_in.y), 0.0);

	uint spp = clamp(params.sample_config.x, 1u, ML_MAX_SAMPLES_PER_PIXEL);
	uint sample_w = params.screen_sample_size.z;
	uint px_flat = s_idx / spp;
	uvec2 spx = uvec2(px_flat % sample_w, px_flat / sample_w);

	bool downsample_half = params.sample_config.y != 0u;
	uvec2 full_size = params.screen_sample_size.xy;
	uvec2 fpx = ml_swrt_sample_to_full_pixel(spx, full_size, downsample_half, params.sample_config.z);

	float depth = texelFetch(sampler2D(ml_depth_tex, ml_sampler_nearest_clamp), ivec2(fpx), 0).r;
	uvec4 samp = ml_sample_buffer.data[s_idx];
	if (!(depth > 0.0) || samp.x == ML_SAMPLE_INVALID) {
		ml_ray_list.data[ray_idx] = uvec2(0xFFFFFFFFu, floatBitsToUint(ML_HIT_DIST_MISS));
		return;
	}

	vec2 uv = (vec2(fpx) + vec2(0.5)) / vec2(full_size);
	vec4 view_h = params.inv_projection * vec4(uv * 2.0 - vec2(1.0), depth, 1.0);
	vec3 view_pos = view_h.xyz / view_h.w;
	vec3 world_pos = ml_swrt_view_to_world_pos(view_pos);

	vec4 nr = texelFetch(sampler2D(ml_normal_roughness_tex, ml_sampler_nearest_clamp), ivec2(fpx), 0);
	vec3 n_vs = nr.xyz * 2.0 - vec3(1.0);
	float n_len2 = dot(n_vs, n_vs);
	vec3 normal_vs = n_len2 > 1e-12 ? n_vs * inversesqrt(n_len2) : vec3(0.0, 0.0, 1.0);
	vec3 world_normal = normalize(ml_swrt_view_to_world_dir(normal_vs));

	vec3 ray_dir_ws = ml_swrt_unpack_oct(samp.z);
	uint ltype = (samp.x >> ML_SAMPLE_TYPE_SHIFT) & ML_SAMPLE_TYPE_MASK;
	float raw_max_dist = uintBitsToFloat(samp.w);
	float total_tmax = (ltype == ML_LIGHT_TYPE_DIRECTIONAL) ? min(raw_max_dist, params.rt_params.y) : max(raw_max_dist - 0.12, 0.0);

	// Self-intersection offset of 1.5 voxels of the global SDF level at the receiver.
	vec3 base_origin = swrt_offset_origin(world_pos, world_normal);
	float start_t = (t_resume > 0.05 && t_resume < total_tmax) ? t_resume : 0.0;
	vec3 trace_origin = base_origin + ray_dir_ws * start_t;
	float trace_tmax = max(total_tmax - start_t, 0.0);
	bool in_flight = start_t > 0.0;

	SWRTHit hit = swrt_trace_global(trace_origin, ray_dir_ws, 0.0, trace_tmax, 0u, in_flight);
	bool occluded = (hit.flags & SWRT_HIT_SURFACE) != 0u;
	float vis = occluded ? 0.0 : 1.0;
	float hit_t = occluded ? min(start_t + max(hit.t, 0.0), ML_HIT_DIST_MISS) : ML_HIT_DIST_MISS;

	ml_ray_list.data[ray_idx] = uvec2(packUnorm4x8(vec4(vis, vis, vis, 1.0)), floatBitsToUint(hit_t));
}
