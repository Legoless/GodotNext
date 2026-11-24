#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_query : require

// GodotNext: megalights_visibility (#146, plan §21.3 contract M4, GM-1, GM-8, GM-12, budget M-4).
// Compacted ray-query shadow trace for unresolved non-VSM MegaLights samples:
//   - Traces near-field TLAS via #145 trace_shadow() (20-layer caster masks, alpha-tested cutouts,
//     translucent-as-opaque LQ6, and #130 material-retrace defer list).
//   - Optional far-field continuation (#126 far-field TLAS, ML_USE_FAR_FIELD, off by default):
//     rays exiting the near-field radius continue against the far-field TLAS.
//   - Overwrites ml_ray_list[ray_idx] in place with (packed RGBA8 transmittance, hit_t float bits).

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#define ML_LIGHT_TYPE_OMNI 0u
#define ML_LIGHT_TYPE_SPOT 1u
#define ML_LIGHT_TYPE_AREA 2u
#define ML_LIGHT_TYPE_DIRECTIONAL 3u

#define ML_SAMPLE_INDEX_MASK 0xFFFFu
#define ML_SAMPLE_TYPE_SHIFT 16u
#define ML_SAMPLE_TYPE_MASK 0x3u
#define ML_SAMPLE_INVALID 0xFFFFFFFFu
#define ML_MAX_SAMPLES_PER_PIXEL 16u

#define ML_VIS_FLAG_FAR_FIELD (1u << 3u)
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

layout(set = 0, binding = 8) uniform accelerationStructureEXT ml_near_tlas;
#define NEXTGI_SHADOW_TLAS ml_near_tlas
#define NEXTGI_SHADOW_SET 0
#define NEXTGI_SHADOW_INSTANCE_BINDING 9
#define NEXTGI_SHADOW_ALPHA_BINDING 10
#define NEXTGI_SHADOW_RETRACE_BINDING 11
#ifdef NEXTGI_SHADOW_ALPHA_TEXTURES
#define NEXTGI_SHADOW_CHUNK0_BINDING 12
#define NEXTGI_SHADOW_CHUNK1_BINDING 13
#define NEXTGI_SHADOW_CHUNK2_BINDING 14
#define NEXTGI_SHADOW_CHUNK3_BINDING 15
#define NEXTGI_SHADOW_SAMPLER_BINDING 16
#define NEXTGI_SHADOW_TEXTURE_SET 1
#endif
#include "../environment/nextgi_shadow_ray_inc.glsl"

#ifdef ML_USE_FAR_FIELD
layout(set = 0, binding = 17) uniform accelerationStructureEXT ml_far_tlas;
#endif

vec3 ml_ray_oct_decode(vec2 p_e) {
	vec3 n = vec3(p_e.x, p_e.y, 1.0 - abs(p_e.x) - abs(p_e.y));
	if (n.z < 0.0) {
		vec2 s = vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
		n.xy = (vec2(1.0) - abs(n.yx)) * s;
	}
	return normalize(n);
}

vec3 ml_ray_unpack_oct(uint p_word) {
	return ml_ray_oct_decode(unpackSnorm2x16(p_word));
}

uvec2 ml_ray_downsample_offset(uint p_phase) {
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

uvec2 ml_ray_sample_to_full_pixel(uvec2 p_sample_px, uvec2 p_full_size, bool p_downsample_half, uint p_frame_index) {
	if (!p_downsample_half) {
		return min(p_sample_px, p_full_size - uvec2(1u));
	}
	return min(p_sample_px * 2u + ml_ray_downsample_offset(p_frame_index), p_full_size - uvec2(1u));
}

vec3 ml_ray_view_to_world_pos(vec3 p_view_pos) {
	vec4 p = vec4(p_view_pos, 1.0);
	return vec3(dot(params.inv_view_rows[0], p), dot(params.inv_view_rows[1], p), dot(params.inv_view_rows[2], p));
}

vec3 ml_ray_view_to_world_dir(vec3 p_view_dir) {
	return vec3(dot(params.inv_view_rows[0].xyz, p_view_dir), dot(params.inv_view_rows[1].xyz, p_view_dir), dot(params.inv_view_rows[2].xyz, p_view_dir));
}

uint ml_ray_policy_index(uint p_light_packed) {
	uint ltype = (p_light_packed >> ML_SAMPLE_TYPE_SHIFT) & ML_SAMPLE_TYPE_MASK;
	uint lidx = p_light_packed & ML_SAMPLE_INDEX_MASK;
	if (ltype == ML_LIGHT_TYPE_OMNI) {
		return lidx;
	}
	if (ltype == ML_LIGHT_TYPE_SPOT) {
		return params.light_counts.x + lidx;
	}
	if (ltype == ML_LIGHT_TYPE_AREA) {
		return params.light_counts.x + params.light_counts.y + lidx;
	}
	return params.light_counts.x + params.light_counts.y + params.light_counts.z + lidx;
}

void main() {
	uint ray_idx = gl_GlobalInvocationID.x;
	uint ray_count = min(ml_vis_counters.data[0], params.trace_config.z);
	if (ray_idx >= ray_count) {
		return;
	}

	uvec2 ray_in = ml_ray_list.data[ray_idx];
	uint s_idx = ray_in.x;
	uint spp = clamp(params.sample_config.x, 1u, ML_MAX_SAMPLES_PER_PIXEL);
	uint sample_w = params.screen_sample_size.z;
	uint px_flat = s_idx / spp;
	uvec2 spx = uvec2(px_flat % sample_w, px_flat / sample_w);

	bool downsample_half = params.sample_config.y != 0u;
	uvec2 full_size = params.screen_sample_size.xy;
	uvec2 fpx = ml_ray_sample_to_full_pixel(spx, full_size, downsample_half, params.sample_config.z);

	float depth = texelFetch(sampler2D(ml_depth_tex, ml_sampler_nearest_clamp), ivec2(fpx), 0).r;
	uvec4 samp = ml_sample_buffer.data[s_idx];
	if (!(depth > 0.0) || samp.x == ML_SAMPLE_INVALID) {
		ml_ray_list.data[ray_idx] = uvec2(0xFFFFFFFFu, floatBitsToUint(ML_HIT_DIST_MISS));
		return;
	}

	vec2 uv = (vec2(fpx) + vec2(0.5)) / vec2(full_size);
	vec4 view_h = params.inv_projection * vec4(uv * 2.0 - vec2(1.0), depth, 1.0);
	vec3 view_pos = view_h.xyz / view_h.w;
	vec3 world_pos = ml_ray_view_to_world_pos(view_pos);

	vec4 nr = texelFetch(sampler2D(ml_normal_roughness_tex, ml_sampler_nearest_clamp), ivec2(fpx), 0);
	vec3 n_vs = nr.xyz * 2.0 - vec3(1.0);
	float n_len2 = dot(n_vs, n_vs);
	vec3 normal_vs = n_len2 > 1e-12 ? n_vs * inversesqrt(n_len2) : vec3(0.0, 0.0, 1.0);
	vec3 world_normal = normalize(ml_ray_view_to_world_dir(normal_vs));

	// Exact same origin bias as ml_reference.glsl so visibility matches brute-force trace_shadow().
	// Back-facing rays (emitted only for #137 SSS back-face transmission) offset inward along -normal
	// so the ray measures local occluder thickness without self-intersecting the front face.
	vec3 ray_dir_ws = ml_ray_unpack_oct(samp.z);
	float origin_side = dot(world_normal, ray_dir_ws) < 0.0 ? -1.0 : 1.0;
	vec3 ray_origin = world_pos + world_normal * (origin_side * max(0.002, length(view_pos) * 0.001));
	uint ltype = (samp.x >> ML_SAMPLE_TYPE_SHIFT) & ML_SAMPLE_TYPE_MASK;
	float raw_max_dist = uintBitsToFloat(samp.w);
	float total_tmax = (ltype == ML_LIGHT_TYPE_DIRECTIONAL) ? raw_max_dist : max(raw_max_dist - 0.005, 0.0);

	uint policy_total = params.light_counts.x + params.light_counts.y + params.light_counts.z + params.light_counts.w;
	uint p_idx = ml_ray_policy_index(samp.x);
	uvec4 policy = (p_idx < policy_total) ? ml_light_shadow_policies.data[p_idx] : uvec4(0u);

	float vis = trace_shadow(ray_origin, ray_dir_ws, total_tmax, policy.x, policy.y);
	float hit_t = (vis < 0.5 && nextgi_shadow_hit_t >= 0.0) ? min(nextgi_shadow_hit_t, ML_HIT_DIST_MISS) : ML_HIT_DIST_MISS;
	bool deferred_retrace = nextgi_shadow_retrace;

#ifdef ML_USE_FAR_FIELD
	// Optional far-field continuation (#126 far-field TLAS, off by default):
	// continue rays exiting the near-field radius up to far_field_distance.
	if (vis > 0.5 && (params.sample_config.w & ML_VIS_FLAG_FAR_FIELD) != 0u) {
		vec3 cam_pos = vec3(params.inv_view_rows[0].w, params.inv_view_rows[1].w, params.inv_view_rows[2].w);
		float near_radius = max(params.rt_params.x, 1.0);
		vec3 oc = ray_origin - cam_pos;
		float b = dot(oc, ray_dir_ws);
		float c = dot(oc, oc) - near_radius * near_radius;
		float t_exit = (c < 0.0) ? (-b + sqrt(max(b * b - c, 0.0))) : 0.0;
		float far_start = max(t_exit - 1.0, 0.0);
		float far_end = min(total_tmax, params.rt_params.y);
		if (far_end > far_start + 0.05) {
			atomicAdd(ml_vis_counters.data[5], 1u);
			rayQueryEXT far_rq;
			rayQueryInitializeEXT(far_rq, ml_far_tlas, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, 0xFFu, ray_origin, far_start, ray_dir_ws, far_end);
			rayQueryProceedEXT(far_rq);
			if (rayQueryGetIntersectionTypeEXT(far_rq, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
				vis = 0.0;
				hit_t = min(rayQueryGetIntersectionTEXT(far_rq, true), ML_HIT_DIST_MISS);
				deferred_retrace = false;
			}
		}
	}
#endif

	if (deferred_retrace && vis > 0.5) {
		if (nextgi_shadow_retrace_append(ray_origin, ray_dir_ws, total_tmax, policy.x, ray_idx, 0u)) {
			atomicAdd(ml_vis_counters.data[6], 1u);
		}
	}

	uint vis_packed = packUnorm4x8(vec4(vis, vis, vis, 1.0));
	ml_ray_list.data[ray_idx] = uvec2(vis_packed, floatBitsToUint(hit_t));
}
