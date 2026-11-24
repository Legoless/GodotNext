#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: megalights_visibility (#146, plan §21.3 contract M4, GM-1, GM-8, GM-12, budget M-4).
// MegaLights visibility chain front-end and resolve shader (plain compute, compiles on all tiers):
//   - MODE_SCREEN_TRACE: Hi-Z depth march using contact_shadow/surface_thickness, quality step cap,
//     longer distant_screen_traces_length for far pixels, 2x2 block ray deduplication for origins
//     within 1 texel in depth picking the same light, and stream compaction into ray_list / vsm_list.
//   - MODE_FINALIZE: writes indirect compute dispatch arguments for ml_trace_rays / ml_trace_rays_swrt
//     and ml_trace_vsm.
//   - MODE_RETRACE_RESOLVE: applies #130 material-retrace results back into ray_list in place.
//   - MODE_RESOLVE_VISIBILITY: scatters compacted ray_list visibility (RGBA8) and hit distance (R16F)
//     to all leader and deduplicated follower samples.

#define ML_LIGHT_TYPE_OMNI 0u
#define ML_LIGHT_TYPE_SPOT 1u
#define ML_LIGHT_TYPE_AREA 2u
#define ML_LIGHT_TYPE_DIRECTIONAL 3u

#define ML_SAMPLE_INDEX_MASK 0xFFFFu
#define ML_SAMPLE_TYPE_SHIFT 16u
#define ML_SAMPLE_TYPE_MASK 0x3u
#define ML_SAMPLE_INVALID 0xFFFFFFFFu
#define ML_MAX_SAMPLES_PER_PIXEL 16u

#define ML_POLICY_FLAG_CASTS_SHADOW (1u << 0u)
#define ML_POLICY_FLAG_USE_VSM (1u << 1u)
#define ML_POLICY_FLAG_FULL_CASTER_MASK (1u << 2u)

#define ML_VIS_FLAG_SCREEN_TRACES (1u << 0u)
#define ML_VIS_FLAG_DEDUPE (1u << 1u)
#define ML_VIS_FLAG_ORTHOGONAL (1u << 2u)
#define ML_VIS_FLAG_FAR_FIELD (1u << 3u)
#define ML_VIS_FLAG_SWRT (1u << 4u)
#define ML_VIS_FLAG_VSM_ENABLED (1u << 5u)
#define ML_VIS_FLAG_NO_RT_FALLBACK (1u << 6u)
#define ML_VIS_FLAG_USE_HIZ (1u << 7u)

#define ML_RAY_MAP_RESOLVED 0xFFFFFFFFu
#define ML_RAY_MAP_VSM_BIT 0x80000000u
#define ML_HIT_DIST_MISS 65504.0

// std140, 272 bytes. Must match RendererRD::MegaLights::VisParamsUBO.
struct MLVisParams {
	mat4 inv_projection;
	mat4 depth_projection;
	vec4 inv_view_rows[3];
	uvec4 screen_sample_size; // xy = full internal_size, zw = sample_grid_size
	uvec4 sample_config; // x = spp, y = downsample_mode, z = frame_index, w = vis_flags
	uvec4 light_counts; // x = omni, y = spot, z = area, w = directional
	vec4 screen_trace_params; // x = surface_thickness, y = near_length, z = distant_length, w = distant_start_depth
	uvec4 trace_config; // x = max_screen_steps, y = hiz_max_mip, z = ray_capacity, w = vsm_capacity
	vec4 rt_params; // x = near_field_radius, y = far_field_distance, z = camera_z_far, w = swrt_max_steps
};

#ifdef MODE_SCREEN_TRACE

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform MLVisParamsUBO {
	MLVisParams params;
};

layout(set = 0, binding = 1) uniform texture2D ml_depth_tex;
layout(set = 0, binding = 2) uniform texture2D ml_normal_roughness_tex;
layout(set = 0, binding = 3) uniform texture2D ml_hiz_tex;
layout(set = 0, binding = 4) uniform sampler ml_sampler_nearest_clamp;

layout(set = 0, binding = 5, std430) restrict readonly buffer MLSampleBufferIn {
	uvec4 data[];
}
ml_sample_buffer;

layout(set = 0, binding = 6, std430) restrict readonly buffer MLLightShadowPolicies {
	uvec4 data[];
}
ml_light_shadow_policies;

// Counters: [0]=ray_count, [1]=vsm_count, [2]=screen_hit_count, [3]=deduped_count,
//           [4]=unshadowed_count, [5]=far_field_count, [6]=retrace_appended_count, [7]=valid_sample_count.
layout(set = 0, binding = 7, std430) restrict buffer MLVisCounters {
	uint data[];
}
ml_vis_counters;

// Compacted RT ray list: x = leader sample index, y = floatBitsToUint(min_t_resume).
layout(set = 0, binding = 8, std430) restrict buffer MLRayListOut {
	uvec2 data[];
}
ml_ray_list;

// Compacted VSM sample list: sample index.
layout(set = 0, binding = 9, std430) restrict writeonly buffer MLVsmListOut {
	uint data[];
}
ml_vsm_list;

// Per-sample map: ML_RAY_MAP_RESOLVED, ray_idx (bit 31 clear), or ML_RAY_MAP_VSM_BIT | vsm_idx.
layout(set = 0, binding = 10, std430) restrict writeonly buffer MLSampleRayMapOut {
	uint data[];
}
ml_sample_ray_map;

// Packed RGBA8 transmittance output (written immediately for resolved samples).
layout(set = 0, binding = 11, std430) restrict writeonly buffer MLVisibilityOut {
	uint data[];
}
ml_visibility_buffer;

// Per-sample hit distance output (R16F 2DArray, layer = k, miss = 65504.0).
layout(r16f, set = 0, binding = 12) uniform restrict writeonly image2DArray ml_hit_distance_img;

// Shared state across the 8x8 workgroup for 2x2 block ray deduplication (13.0 KiB total < 16 KiB floor).
// s_geom: x = floatBitsToUint(linear_depth), y = floatBitsToUint(depth_tol), z = packed world normal oct, w = unused.
shared uvec4 s_geom[64];
shared uint s_light[64][ML_MAX_SAMPLES_PER_PIXEL];
shared uint s_dir_oct[64][ML_MAX_SAMPLES_PER_PIXEL];
shared uint s_ray_idx[64][ML_MAX_SAMPLES_PER_PIXEL];

uint ml_vis_pcg_hash(uint p_v) {
	uint state = p_v * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

float ml_vis_hash_to_float(uint p_h) {
	return float(p_h >> 8u) * (1.0 / 16777216.0);
}

vec2 ml_vis_oct_encode(vec3 p_dir) {
	float l1 = abs(p_dir.x) + abs(p_dir.y) + abs(p_dir.z);
	if (!(l1 > 1e-12)) {
		return vec2(0.0);
	}
	vec3 n = p_dir / l1;
	if (n.z < 0.0) {
		vec2 s = vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
		n.xy = (vec2(1.0) - abs(n.yx)) * s;
	}
	return clamp(n.xy, vec2(-1.0), vec2(1.0));
}

vec3 ml_vis_oct_decode(vec2 p_e) {
	vec3 n = vec3(p_e.x, p_e.y, 1.0 - abs(p_e.x) - abs(p_e.y));
	if (n.z < 0.0) {
		vec2 s = vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
		n.xy = (vec2(1.0) - abs(n.yx)) * s;
	}
	return normalize(n);
}

uint ml_vis_pack_oct(vec3 p_dir) {
	return packSnorm2x16(ml_vis_oct_encode(p_dir));
}

vec3 ml_vis_unpack_oct(uint p_word) {
	return ml_vis_oct_decode(unpackSnorm2x16(p_word));
}

uvec2 ml_vis_downsample_offset(uint p_phase) {
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

uvec2 ml_vis_sample_to_full_pixel(uvec2 p_sample_px, uvec2 p_full_size, bool p_downsample_half, uint p_frame_index) {
	if (!p_downsample_half) {
		return min(p_sample_px, p_full_size - uvec2(1u));
	}
	return min(p_sample_px * 2u + ml_vis_downsample_offset(p_frame_index), p_full_size - uvec2(1u));
}

vec3 ml_vis_view_to_world_dir(vec3 p_view_dir) {
	return vec3(dot(params.inv_view_rows[0].xyz, p_view_dir), dot(params.inv_view_rows[1].xyz, p_view_dir), dot(params.inv_view_rows[2].xyz, p_view_dir));
}

vec3 ml_vis_world_to_view_dir(vec3 p_world_dir) {
	// inv_view_rows[0..2].xyz are the rows of cam_transform.basis (orthonormal camera axes).
	return vec3(
			params.inv_view_rows[0].x * p_world_dir.x + params.inv_view_rows[1].x * p_world_dir.y + params.inv_view_rows[2].x * p_world_dir.z,
			params.inv_view_rows[0].y * p_world_dir.x + params.inv_view_rows[1].y * p_world_dir.y + params.inv_view_rows[2].y * p_world_dir.z,
			params.inv_view_rows[0].z * p_world_dir.x + params.inv_view_rows[1].z * p_world_dir.y + params.inv_view_rows[2].z * p_world_dir.z);
}

float ml_vis_depth_to_linear_z(vec2 p_uv, float p_depth) {
	vec4 h = params.inv_projection * vec4(p_uv * 2.0 - vec2(1.0), p_depth, 1.0);
	float safe_w = abs(h.w) > 1e-7 ? h.w : (h.w >= 0.0 ? 1e-7 : -1e-7);
	return -(h.z / safe_w);
}

uint ml_vis_policy_index(uint p_light_packed) {
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

// Conservative Hi-Z screen-space shadow march with mip-0 depth confirmation and SSCS surface_thickness.
// Returns true only on a high-confidence screen hit (keeping false occlusion <= 0.5%).
// When unresolved, outputs a safe pullback resume distance r_resume_t for optional SWRT continuation.
bool ml_screen_trace_march(vec3 p_view_pos, vec3 p_normal_vs, vec3 p_ray_dir_vs, float p_max_dist, uvec2 p_full_px, vec2 p_full_size_f, out float r_hit_t, out float r_resume_t) {
	r_hit_t = ML_HIT_DIST_MISS;
	r_resume_t = 0.0;

	if (!(p_max_dist > 0.02)) {
		return false;
	}
	// Skip grazing rays where depth quantization along the receiver surface causes false self-shadowing.
	if (dot(p_normal_vs, p_ray_dir_vs) < 0.12) {
		return false;
	}

	bool is_ortho = (params.sample_config.w & ML_VIS_FLAG_ORTHOGONAL) != 0u;
	bool use_hiz = (params.sample_config.w & ML_VIS_FLAG_USE_HIZ) != 0u;
	float linear_z0 = max(-p_view_pos.z, 1e-4);
	float origin_bias = clamp(0.008 + linear_z0 * 0.003, 0.008, 0.08);
	vec3 p0 = p_view_pos + p_normal_vs * origin_bias;
	vec3 p1 = p0 + p_ray_dir_vs * p_max_dist;

	vec4 clip0 = params.depth_projection * vec4(p0, 1.0);
	vec4 clip1 = params.depth_projection * vec4(p1, 1.0);
	const float min_w = 1e-3;
	if (!(clip0.w > min_w)) {
		return false;
	}
	float w1 = clip1.w;
	if (!(w1 > min_w)) {
		float t_clip = clamp((clip0.w - min_w) / max(clip0.w - clip1.w, 1e-6), 0.0, 1.0);
		p1 = mix(p0, p1, t_clip);
		clip1 = params.depth_projection * vec4(p1, 1.0);
		w1 = max(clip1.w, min_w);
	}

	vec2 uv0 = (clip0.xy / clip0.w) * 0.5 + vec2(0.5);
	vec2 uv1 = (clip1.xy / w1) * 0.5 + vec2(0.5);
	float px_len = length((uv1 - uv0) * p_full_size_f);
	if (!(px_len >= 2.0)) {
		return false;
	}

	uint max_steps = clamp(params.trace_config.x, 4u, 32u);
	float hiz_mip = use_hiz ? clamp(floor(log2(max(px_len / float(max_steps), 1.0))), 0.0, float(params.trace_config.y)) : 0.0;
	float surface_thickness = max(params.screen_trace_params.x, 0.001);

	// Advance at least 1.5 pixels away from the start pixel before testing any depth texel.
	float t_start_px = min(1.5 / px_len, 0.9);
	float jitter = 0.25 + 0.5 * ml_vis_hash_to_float(ml_vis_pcg_hash(p_full_px.x ^ (p_full_px.y * 0x9E3779B9u) ^ (params.sample_config.z * 0x85EBCA6Bu)));

	bool seen_clear = false;
	float last_clear_t = 0.0;

	for (uint step_i = 0u; step_i < max_steps; step_i++) {
		float u = mix(t_start_px, 1.0, (float(step_i) + jitter) / float(max_steps));
		vec2 uv = mix(uv0, uv1, u);
		if (uv.x <= 0.0 || uv.y <= 0.0 || uv.x >= 1.0 || uv.y >= 1.0) {
			break;
		}
		float s_ray = is_ortho ? u : (u * clip0.w / max(mix(w1, clip0.w, u), 1e-6));
		vec3 sample_vs = mix(p0, p1, s_ray);
		float ray_linear_z = -sample_vs.z;
		float ray_t = length(sample_vs - p0);

		// Fast coarse Hi-Z empty-space skip (Hi-Z stores nearest depth in the mip footprint).
		if (use_hiz && hiz_mip > 0.5) {
			float hiz_d = textureLod(sampler2D(ml_hiz_tex, ml_sampler_nearest_clamp), uv, hiz_mip).r;
			if (!(hiz_d > 0.0)) {
				seen_clear = true;
				last_clear_t = ray_t;
				continue;
			}
			float hiz_z = ml_vis_depth_to_linear_z(uv, hiz_d);
			if (ray_linear_z <= hiz_z) {
				seen_clear = true;
				last_clear_t = ray_t;
				continue;
			}
		}

		// Exact mip-0 confirmation prevents coarse-mip silhouette bloat from causing false occlusion.
		ivec2 px = ivec2(clamp(floor(uv * p_full_size_f), vec2(0.0), p_full_size_f - vec2(1.0)));
		float d0 = texelFetch(sampler2D(ml_depth_tex, ml_sampler_nearest_clamp), px, 0).r;
		if (!(d0 > 0.0)) {
			seen_clear = true;
			last_clear_t = ray_t;
			continue;
		}
		float scene_z = ml_vis_depth_to_linear_z(uv, d0);
		float z_bias = max(0.015, scene_z * 0.004);
		float thickness = max(0.02, scene_z * surface_thickness);
		float depth_diff = ray_linear_z - scene_z;

		if (depth_diff <= z_bias) {
			seen_clear = true;
			last_clear_t = ray_t;
			continue;
		}
		if (depth_diff <= thickness) {
			if (seen_clear || step_i >= 1u) {
				r_hit_t = ray_t;
				return true;
			}
		}
		// Passed behind a foreground occluder beyond surface thickness: hand off to world trace.
		r_resume_t = max(last_clear_t - 0.08, 0.0);
		return false;
	}

	r_resume_t = max(last_clear_t - 0.08, 0.0);
	return false;
}

void main() {
	uvec2 spx = gl_GlobalInvocationID.xy;
	uint lid = gl_LocalInvocationID.y * 8u + gl_LocalInvocationID.x;
	uint spp = clamp(params.sample_config.x, 1u, ML_MAX_SAMPLES_PER_PIXEL);
	bool in_bounds = spx.x < params.screen_sample_size.z && spx.y < params.screen_sample_size.w;

	s_geom[lid] = uvec4(floatBitsToUint(-1.0), floatBitsToUint(0.0), 0u, 0u);
	for (uint k = 0u; k < ML_MAX_SAMPLES_PER_PIXEL; k++) {
		s_light[lid][k] = ML_SAMPLE_INVALID;
		s_dir_oct[lid][k] = 0u;
		s_ray_idx[lid][k] = ML_RAY_MAP_RESOLVED;
	}

	float local_t_resume[ML_MAX_SAMPLES_PER_PIXEL];
	for (uint k = 0u; k < ML_MAX_SAMPLES_PER_PIXEL; k++) {
		local_t_resume[k] = 0.0;
	}

	bool downsample_half = params.sample_config.y != 0u;
	uint frame_index = params.sample_config.z;
	uint vis_flags = params.sample_config.w;
	bool screen_traces_on = (vis_flags & ML_VIS_FLAG_SCREEN_TRACES) != 0u;
	bool dedupe_on = (vis_flags & ML_VIS_FLAG_DEDUPE) != 0u;
	bool is_ortho = (vis_flags & ML_VIS_FLAG_ORTHOGONAL) != 0u;
	bool vsm_enabled = (vis_flags & ML_VIS_FLAG_VSM_ENABLED) != 0u;
	bool no_rt_fallback = (vis_flags & ML_VIS_FLAG_NO_RT_FALLBACK) != 0u;

	uint base_idx = (spx.y * params.screen_sample_size.z + spx.x) * spp;
	vec3 world_normal = vec3(0.0, 1.0, 0.0);
	float linear_z = -1.0;
	float depth_tol = 0.0;

	if (in_bounds) {
		uvec2 full_size = params.screen_sample_size.xy;
		vec2 full_size_f = vec2(full_size);
		uvec2 fpx = ml_vis_sample_to_full_pixel(spx, full_size, downsample_half, frame_index);
		float depth = texelFetch(sampler2D(ml_depth_tex, ml_sampler_nearest_clamp), ivec2(fpx), 0).r;

		if (!(depth > 0.0)) {
			for (uint k = 0u; k < spp; k++) {
				uint s_idx = base_idx + k;
				ml_visibility_buffer.data[s_idx] = 0u;
				ml_sample_ray_map.data[s_idx] = ML_RAY_MAP_RESOLVED;
				imageStore(ml_hit_distance_img, ivec3(int(spx.x), int(spx.y), int(k)), vec4(ML_HIT_DIST_MISS, 0.0, 0.0, 0.0));
			}
		} else {
			vec2 uv = (vec2(fpx) + vec2(0.5)) / full_size_f;
			vec4 view_h = params.inv_projection * vec4(uv * 2.0 - vec2(1.0), depth, 1.0);
			vec3 view_pos = view_h.xyz / view_h.w;
			linear_z = max(-view_pos.z, 1e-4);

			vec4 nr = texelFetch(sampler2D(ml_normal_roughness_tex, ml_sampler_nearest_clamp), ivec2(fpx), 0);
			vec3 n_vs = nr.xyz * 2.0 - vec3(1.0);
			float n_len2 = dot(n_vs, n_vs);
			vec3 normal_vs = n_len2 > 1e-12 ? n_vs * inversesqrt(n_len2) : vec3(0.0, 0.0, 1.0);
			world_normal = normalize(ml_vis_view_to_world_dir(normal_vs));

			// 1-texel depth tolerance for 2x2 block ray deduplication.
			float proj_y = max(abs(params.depth_projection[1][1]) * full_size_f.y, 1e-4);
			float pixel_world = is_ortho ? (2.0 / proj_y) : (2.0 * linear_z / proj_y);
			float step_scale = downsample_half ? 2.0 : 1.0;
			float slope_factor = clamp(sqrt(max(1.0 - normal_vs.z * normal_vs.z, 0.0)) / max(abs(normal_vs.z), 0.25), 0.5, 2.5);
			depth_tol = max(pixel_world * step_scale * slope_factor, 0.01);

			s_geom[lid] = uvec4(floatBitsToUint(linear_z), floatBitsToUint(depth_tol), ml_vis_pack_oct(world_normal), 0u);

			// Screen trace max distance: short near camera, longer distant_screen_traces_length for far pixels.
			float dist_blend = smoothstep(params.screen_trace_params.w * 0.5, params.screen_trace_params.w * 2.0, linear_z);
			float screen_trace_len = mix(params.screen_trace_params.y, params.screen_trace_params.z, dist_blend);

			uint policy_total = params.light_counts.x + params.light_counts.y + params.light_counts.z + params.light_counts.w;

			for (uint k = 0u; k < spp; k++) {
				uint s_idx = base_idx + k;
				uvec4 samp = ml_sample_buffer.data[s_idx];
				if (samp.x == ML_SAMPLE_INVALID) {
					ml_visibility_buffer.data[s_idx] = 0u;
					ml_sample_ray_map.data[s_idx] = ML_RAY_MAP_RESOLVED;
					imageStore(ml_hit_distance_img, ivec3(int(spx.x), int(spx.y), int(k)), vec4(ML_HIT_DIST_MISS, 0.0, 0.0, 0.0));
					continue;
				}
				atomicAdd(ml_vis_counters.data[7], 1u);

				uint p_idx = ml_vis_policy_index(samp.x);
				uvec4 policy = (p_idx < policy_total) ? ml_light_shadow_policies.data[p_idx] : uvec4(0u);
				if ((policy.z & ML_POLICY_FLAG_CASTS_SHADOW) == 0u) {
					// Unshadowed light: fully visible immediately.
					ml_visibility_buffer.data[s_idx] = 0xFFFFFFFFu;
					ml_sample_ray_map.data[s_idx] = ML_RAY_MAP_RESOLVED;
					imageStore(ml_hit_distance_img, ivec3(int(spx.x), int(spx.y), int(k)), vec4(ML_HIT_DIST_MISS, 0.0, 0.0, 0.0));
					atomicAdd(ml_vis_counters.data[4], 1u);
					continue;
				}

				if (vsm_enabled && (policy.z & ML_POLICY_FLAG_USE_VSM) != 0u) {
					// VSM-method light skips ray queries and routes to ml_trace_vsm.
					uint vsm_idx = atomicAdd(ml_vis_counters.data[1], 1u);
					if (vsm_idx < params.trace_config.w) {
						ml_vsm_list.data[vsm_idx] = s_idx;
						ml_sample_ray_map.data[s_idx] = ML_RAY_MAP_VSM_BIT | vsm_idx;
					} else {
						ml_visibility_buffer.data[s_idx] = 0xFFFFFFFFu;
						ml_sample_ray_map.data[s_idx] = ML_RAY_MAP_RESOLVED;
						imageStore(ml_hit_distance_img, ivec3(int(spx.x), int(spx.y), int(k)), vec4(ML_HIT_DIST_MISS, 0.0, 0.0, 0.0));
					}
					continue;
				}

				uint ltype = (samp.x >> ML_SAMPLE_TYPE_SHIFT) & ML_SAMPLE_TYPE_MASK;
				float raw_max_dist = uintBitsToFloat(samp.w);
				float total_tmax = (ltype == ML_LIGHT_TYPE_DIRECTIONAL) ? raw_max_dist : max(raw_max_dist - 0.005, 0.0);

				float t_resume = 0.0;
				if (screen_traces_on && (policy.z & ML_POLICY_FLAG_FULL_CASTER_MASK) != 0u) {
					vec3 ray_dir_ws = ml_vis_unpack_oct(samp.z);
					vec3 ray_dir_vs = normalize(ml_vis_world_to_view_dir(ray_dir_ws));
					float hit_t;
					if (ml_screen_trace_march(view_pos, normal_vs, ray_dir_vs, min(total_tmax, screen_trace_len), fpx, full_size_f, hit_t, t_resume)) {
						ml_visibility_buffer.data[s_idx] = 0xFF000000u; // RGB = 0, A = 1
						ml_sample_ray_map.data[s_idx] = ML_RAY_MAP_RESOLVED;
						imageStore(ml_hit_distance_img, ivec3(int(spx.x), int(spx.y), int(k)), vec4(min(hit_t, ML_HIT_DIST_MISS), 0.0, 0.0, 0.0));
						atomicAdd(ml_vis_counters.data[2], 1u);
						continue;
					}
				}

				if (no_rt_fallback) {
					ml_visibility_buffer.data[s_idx] = 0xFFFFFFFFu;
					ml_sample_ray_map.data[s_idx] = ML_RAY_MAP_RESOLVED;
					imageStore(ml_hit_distance_img, ivec3(int(spx.x), int(spx.y), int(k)), vec4(ML_HIT_DIST_MISS, 0.0, 0.0, 0.0));
					continue;
				}

				// Unresolved RT sample: stage for 2x2 block dedupe and compaction.
				s_light[lid][k] = samp.x;
				s_dir_oct[lid][k] = samp.z;
				local_t_resume[k] = t_resume;
			}
		}
	}

	barrier();

	// 2x2 block deduplication & stream compaction into ml_ray_list.
	// Sub-pixels within each 2x2 block execute in order 0 -> 1 -> 2 -> 3 so earlier sub-pixels
	// act as deterministic leaders without transitivity chains.
	uint bx = gl_LocalInvocationID.x & ~1u;
	uint by = gl_LocalInvocationID.y & ~1u;
	uint sub = (gl_LocalInvocationID.x & 1u) | ((gl_LocalInvocationID.y & 1u) << 1u);

	for (uint turn = 0u; turn < 4u; turn++) {
		if (in_bounds && sub == turn) {
			for (uint k = 0u; k < spp; k++) {
				uint my_light = s_light[lid][k];
				if (my_light == ML_SAMPLE_INVALID) {
					continue;
				}
				uint s_idx = base_idx + k;
				uint assigned_ray = ML_RAY_MAP_RESOLVED;

				if (dedupe_on && turn > 0u) {
					vec3 my_dir = ml_vis_unpack_oct(s_dir_oct[lid][k]);
					// Pass 1: prefer matching the same sample slot k (preserves multi-spp stratification).
					// Pass 2: allow any slot in an earlier sub-pixel of the 2x2 block.
					for (uint pass = 0u; pass < 2u && assigned_ray == ML_RAY_MAP_RESOLVED; pass++) {
						for (uint prev_sub = 0u; prev_sub < turn && assigned_ray == ML_RAY_MAP_RESOLVED; prev_sub++) {
							uint ox = bx + (prev_sub & 1u);
							uint oy = by + (prev_sub >> 1u);
							uint olid = oy * 8u + ox;
							uvec4 ogeom = s_geom[olid];
							float odepth = uintBitsToFloat(ogeom.x);
							if (!(odepth > 0.0)) {
								continue;
							}
							float otol = uintBitsToFloat(ogeom.y);
							if (abs(linear_z - odepth) > max(depth_tol, otol)) {
								continue;
							}
							vec3 onormal = ml_vis_unpack_oct(ogeom.z);
							if (dot(world_normal, onormal) < 0.90) {
								continue;
							}
							uint k_begin = (pass == 0u) ? k : 0u;
							uint k_end = (pass == 0u) ? (k + 1u) : spp;
							for (uint ok = k_begin; ok < k_end; ok++) {
								if (s_light[olid][ok] == my_light && s_ray_idx[olid][ok] != ML_RAY_MAP_RESOLVED) {
									vec3 odir = ml_vis_unpack_oct(s_dir_oct[olid][ok]);
									if (dot(my_dir, odir) >= 0.995) {
										assigned_ray = s_ray_idx[olid][ok];
										break;
									}
								}
							}
						}
					}
				}

				if (assigned_ray != ML_RAY_MAP_RESOLVED) {
					// Follower shares the leader's ray and tightens min_t_resume conservatively.
					ml_sample_ray_map.data[s_idx] = assigned_ray;
					atomicMin(ml_ray_list.data[assigned_ray].y, floatBitsToUint(max(local_t_resume[k], 0.0)));
					atomicAdd(ml_vis_counters.data[3], 1u);
				} else {
					// Allocate a compacted leader ray in ml_ray_list.
					uint ray_idx = atomicAdd(ml_vis_counters.data[0], 1u);
					if (ray_idx < params.trace_config.z) {
						ml_ray_list.data[ray_idx] = uvec2(s_idx, floatBitsToUint(max(local_t_resume[k], 0.0)));
						ml_sample_ray_map.data[s_idx] = ray_idx;
						s_ray_idx[lid][k] = ray_idx;
					} else {
						ml_visibility_buffer.data[s_idx] = 0xFFFFFFFFu;
						ml_sample_ray_map.data[s_idx] = ML_RAY_MAP_RESOLVED;
						imageStore(ml_hit_distance_img, ivec3(int(spx.x), int(spx.y), int(k)), vec4(ML_HIT_DIST_MISS, 0.0, 0.0, 0.0));
					}
				}
			}
		}
		if (turn < 3u) {
			barrier();
		}
	}
}

#endif // MODE_SCREEN_TRACE

#ifdef MODE_FINALIZE

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform MLVisParamsUBO {
	MLVisParams params;
};

layout(set = 0, binding = 1, std430) restrict readonly buffer MLVisCountersIn {
	uint data[];
}
ml_vis_counters;

// [0] = dispatch args for ml_trace_rays / ml_trace_rays_swrt, [1] = dispatch args for ml_trace_vsm.
layout(set = 0, binding = 2, std430) restrict writeonly buffer MLVisIndirectOut {
	uvec4 data[];
}
ml_vis_indirect;

void main() {
	uint ray_count = min(ml_vis_counters.data[0], params.trace_config.z);
	uint vsm_count = min(ml_vis_counters.data[1], params.trace_config.w);
	ml_vis_indirect.data[0] = uvec4((ray_count + 63u) / 64u, 1u, 1u, 0u);
	ml_vis_indirect.data[1] = uvec4((vsm_count + 63u) / 64u, 1u, 1u, 0u);
}

#endif // MODE_FINALIZE

#ifdef MODE_RETRACE_RESOLVE

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict readonly buffer MLRetraceRecordsIn {
	uvec4 header;
	uvec4 data[];
}
ml_retrace_records;

layout(set = 0, binding = 1, std430) restrict buffer MLRayListRW {
	uvec2 data[];
}
ml_ray_list;

void main() {
	uint i = gl_GlobalInvocationID.x;
	uint count = min(ml_retrace_records.header.x, ml_retrace_records.header.y);
	if (i >= count) {
		return;
	}
	uvec4 info = ml_retrace_records.data[i * 3u];
	uint ray_idx = info.x;
	uint result = info.w; // 1 = occluded, 2 = clear
	if (result == 1u) {
		float tmax = uintBitsToFloat(ml_retrace_records.data[i * 3u + 1u].w);
		float approx_t = min(max(tmax * 0.5, 0.01), ML_HIT_DIST_MISS);
		ml_ray_list.data[ray_idx] = uvec2(0xFF000000u, floatBitsToUint(approx_t));
	}
}

#endif // MODE_RETRACE_RESOLVE

#ifdef MODE_RESOLVE_VISIBILITY

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std140) uniform MLVisParamsUBO {
	MLVisParams params;
};

layout(set = 0, binding = 1, std430) restrict readonly buffer MLSampleRayMapIn {
	uint data[];
}
ml_sample_ray_map;

layout(set = 0, binding = 2, std430) restrict readonly buffer MLRayListIn {
	uvec2 data[];
}
ml_ray_list;

layout(set = 0, binding = 3, std430) restrict writeonly buffer MLVisibilityOut {
	uint data[];
}
ml_visibility_buffer;

layout(r16f, set = 0, binding = 4) uniform restrict writeonly image2DArray ml_hit_distance_img;

void main() {
	uint s_idx = gl_GlobalInvocationID.x;
	uint spp = clamp(params.sample_config.x, 1u, ML_MAX_SAMPLES_PER_PIXEL);
	uint sample_w = params.screen_sample_size.z;
	uint sample_h = params.screen_sample_size.w;
	uint total_samples = sample_w * sample_h * spp;
	if (s_idx >= total_samples) {
		return;
	}
	uint entry = ml_sample_ray_map.data[s_idx];
	if ((entry & ML_RAY_MAP_VSM_BIT) != 0u) {
		return; // Already resolved in screen trace (0xFFFFFFFF) or traced directly by ml_trace_vsm.
	}
	uvec2 res = ml_ray_list.data[entry];
	ml_visibility_buffer.data[s_idx] = res.x;

	uint px_flat = s_idx / spp;
	uint k = s_idx - px_flat * spp;
	uint sx = px_flat % sample_w;
	uint sy = px_flat / sample_w;
	imageStore(ml_hit_distance_img, ivec3(int(sx), int(sy), int(k)), vec4(uintBitsToFloat(res.y), 0.0, 0.0, 0.0));
}

#endif // MODE_RESOLVE_VISIBILITY
