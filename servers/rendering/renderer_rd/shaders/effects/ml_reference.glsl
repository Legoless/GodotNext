#[compute]

#version 460

#VERSION_DEFINES

#ifdef MODE_RAY_QUERY
#extension GL_EXT_ray_query : require
#endif

// GodotNext: megalights_sampling (#142, plan §21.3 contract M3, GM-1).
// Exhaustive MegaLights reference shading & unbiasedness verification shader.
// Variants:
//   - MODE_RAY_QUERY: exhaustive per-light shading with trace_shadow() (#145), Welford-accumulated
//     into RGBA32F (mean RGB + luminance M2 for exact per-pixel RSE convergence measurement).
//   - MODE_UNSHADOWED: exhaustive per-light shading with visibility = 1.0 (no ray-query dependency;
//     serves as exact ground truth for unbiasedness checks and non-RT fallback).
//   - MODE_VERIFY_SAMPLES: evaluates K generated samples from ml_generate_samples.glsl with
//     visibility = 1.0 and Welford-accumulates per full-res pixel (including half-res 2x2 rotation).

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#define ML_DECLARE_COMMON_BINDINGS
#include "ml_light_eval_inc.glsl"

layout(set = 0, binding = 20, std430) restrict readonly buffer MLLayerGroupMasks {
	uint data[];
}
layer_group_masks;

// RGBA32F accumulation target: .rgb = running Welford mean radiance, .a = luminance Welford M2.
layout(rgba32f, set = 0, binding = 21) uniform restrict image2D ml_reference_accum_img;

#ifdef MODE_VERIFY_SAMPLES

layout(set = 0, binding = 22, std430) restrict readonly buffer MLSampleBufferIn {
	uvec4 data[];
}
ml_sample_buffer;

layout(set = 0, binding = 23, std430) restrict buffer MLVerifyCounts {
	uint data[];
}
ml_verify_counts;

#endif // MODE_VERIFY_SAMPLES

#ifdef MODE_RAY_QUERY

layout(set = 0, binding = 22) uniform accelerationStructureEXT ml_shadow_tlas;
#define NEXTGI_SHADOW_TLAS ml_shadow_tlas
#define NEXTGI_SHADOW_SET 0
#define NEXTGI_SHADOW_INSTANCE_BINDING 23
#define NEXTGI_SHADOW_ALPHA_BINDING 24
#ifdef NEXTGI_SHADOW_ALPHA_TEXTURES
#define NEXTGI_SHADOW_CHUNK0_BINDING 26
#define NEXTGI_SHADOW_CHUNK1_BINDING 27
#define NEXTGI_SHADOW_CHUNK2_BINDING 28
#define NEXTGI_SHADOW_CHUNK3_BINDING 29
#define NEXTGI_SHADOW_SAMPLER_BINDING 30
#define NEXTGI_SHADOW_TEXTURE_SET 1
#endif
#include "../environment/nextgi_shadow_ray_inc.glsl"

// Per-light shadow policy words (light_mask_mode, caster_mask, flags, light_type) in order:
// [0..omni_count), [omni..omni+spot), [omni+spot..omni+spot+area), [total_local..total_local+dir).
layout(set = 0, binding = 25, std430) restrict readonly buffer MLLightShadowPolicies {
	uvec4 data[];
}
ml_light_shadow_policies;

float ml_reference_visibility(vec3 p_origin_ws, vec3 p_dir_ws, float p_tmax, uint p_policy_idx, bool p_force_visible) {
	if (p_force_visible) {
		return 1.0;
	}
	uvec4 policy = ml_light_shadow_policies.data[p_policy_idx];
	return trace_shadow(p_origin_ws, p_dir_ws, p_tmax, policy.x, policy.y);
}

#else

float ml_reference_visibility(vec3 p_origin_ws, vec3 p_dir_ws, float p_tmax, uint p_policy_idx, bool p_force_visible) {
	return 1.0;
}

#endif // MODE_RAY_QUERY

void ml_welford_accumulate(ivec2 p_px, vec3 p_sample_rgb, uint p_n) {
	if (p_n <= 1u) {
		imageStore(ml_reference_accum_img, p_px, vec4(p_sample_rgb, 0.0));
		return;
	}
	vec4 prev = imageLoad(ml_reference_accum_img, p_px);
	float y_val = ml_luminance(p_sample_rgb);
	float y_prev = ml_luminance(prev.rgb);
	float delta = y_val - y_prev;
	vec3 mean_rgb = prev.rgb + (p_sample_rgb - prev.rgb) / float(p_n);
	float y_new = ml_luminance(mean_rgb);
	float delta2 = y_val - y_new;
	float m2 = max(prev.a + delta * delta2, 0.0);
	imageStore(ml_reference_accum_img, p_px, vec4(mean_rgb, m2));
}

void main() {
	bool use_lf = (params.sample_config.w & ML_FLAG_USE_LIGHT_FUNCTION) != 0u;
	bool use_soft_shadows = (params.sample_config.w & ML_FLAG_USE_SOFT_SHADOWS) != 0u;
	float falloff_threshold = params.thresholds.x;
	uint frame = params.sample_config.z;

#ifdef MODE_VERIFY_SAMPLES
	uvec2 spx = gl_GlobalInvocationID.xy;
	if (spx.x >= params.screen_sample_size.z || spx.y >= params.screen_sample_size.w) {
		return;
	}

	bool downsample_half = params.sample_config.y != 0u;
	uvec2 fpx = ml_sample_to_full_pixel(spx, params.screen_sample_size.xy, downsample_half, frame);
	uint px_flat = fpx.y * params.screen_sample_size.x + fpx.x;
	uint n = ml_verify_counts.data[px_flat] + 1u;
	ml_verify_counts.data[px_flat] = n;

	uint layer_group = ml_read_layer_group_index(fpx);
	uint layer_mask = (layer_group < params.guiding_tile_info.z) ? layer_group_masks.data[layer_group] : 0xFFFFFFFFu;
	MLSurface surf;
	if (!ml_unpack_surface(fpx, layer_mask, surf)) {
		ml_welford_accumulate(ivec2(fpx), vec3(0.0), n);
		return;
	}

	uint spp = clamp(params.sample_config.x, 1u, ML_MAX_SAMPLES_PER_PIXEL);
	uint base_idx = (spx.y * params.screen_sample_size.z + spx.x) * spp;
	vec3 est = vec3(0.0);
	for (uint k = 0u; k < spp; k++) {
		uvec4 s = ml_sample_buffer.data[base_idx + k];
		if (s.x == ML_SAMPLE_INVALID) {
			continue;
		}
		float inv_pdf = uintBitsToFloat(s.y);
		MLLightEval ev = ml_eval_packed_light(s.x, surf, falloff_threshold, use_soft_shadows, use_lf, vec2(0.5), false);
		if (ev.valid) {
			est += (ev.diffuse + ev.specular) * inv_pdf;
		}
	}
	est /= float(spp);
	ml_welford_accumulate(ivec2(fpx), est, n);
#else
	uvec2 fpx = gl_GlobalInvocationID.xy;
	if (fpx.x >= params.screen_sample_size.x || fpx.y >= params.screen_sample_size.y) {
		return;
	}

	uint n = max(params.guiding_tile_info.w, 1u);
	uint layer_group = ml_read_layer_group_index(fpx);
	uint layer_mask = (layer_group < params.guiding_tile_info.z) ? layer_group_masks.data[layer_group] : 0xFFFFFFFFu;
	MLSurface surf;
	if (!ml_unpack_surface(fpx, layer_mask, surf)) {
		ml_welford_accumulate(ivec2(fpx), vec3(0.0), n);
		return;
	}

	bool force_visible = (params.sample_config.w & ML_FLAG_FORCE_VISIBLE_ONE) != 0u;
	bool include_directional = (params.sample_config.w & ML_FLAG_DIRECTIONAL_LIGHTS) != 0u;
#ifdef MODE_RAY_QUERY
	bool need_ray = !force_visible;
#else
	bool need_ray = false;
#endif
	vec3 ray_origin = surf.world_pos + surf.world_normal * max(0.002, length(surf.view_pos) * 0.001);

	vec3 total_radiance = vec3(0.0);
	uint omni_count = params.light_counts.x;
	uint spot_count = params.light_counts.y;
	uint area_count = params.light_counts.z;
	uint dir_count = include_directional ? min(params.light_counts.w, uint(MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS)) : 0u;

	for (uint i = 0u; i < omni_count; i++) {
		uint lid = ml_pack_sample_light(ML_LIGHT_TYPE_OMNI, i);
		vec2 u_emit = need_ray ? ml_stbn_sample_uv(fpx, frame, lid * 31u + 7u) : vec2(0.5);
		MLLightEval ev = ml_eval_omni(ml_omni_lights.data[i], surf, falloff_threshold, use_soft_shadows, use_lf, u_emit, need_ray);
		if (!ev.valid) {
			continue;
		}
		float vis = ml_reference_visibility(ray_origin, ev.ray_dir_ws, max(ev.ray_dist - 0.005, 0.0), i, force_visible);
		total_radiance += (ev.diffuse + ev.specular) * vis;
	}

	uint spot_policy_base = omni_count;
	for (uint i = 0u; i < spot_count; i++) {
		uint lid = ml_pack_sample_light(ML_LIGHT_TYPE_SPOT, i);
		vec2 u_emit = need_ray ? ml_stbn_sample_uv(fpx, frame, lid * 31u + 7u) : vec2(0.5);
		MLLightEval ev = ml_eval_spot(ml_spot_lights.data[i], surf, falloff_threshold, use_soft_shadows, use_lf, u_emit, need_ray);
		if (!ev.valid) {
			continue;
		}
		float vis = ml_reference_visibility(ray_origin, ev.ray_dir_ws, max(ev.ray_dist - 0.005, 0.0), spot_policy_base + i, force_visible);
		total_radiance += (ev.diffuse + ev.specular) * vis;
	}

	uint area_policy_base = omni_count + spot_count;
	for (uint i = 0u; i < area_count; i++) {
		uint lid = ml_pack_sample_light(ML_LIGHT_TYPE_AREA, i);
		vec2 u_emit = need_ray ? ml_stbn_sample_uv(fpx, frame, lid * 31u + 7u) : vec2(0.5);
		MLLightEval ev = ml_eval_area(ml_area_lights.data[i], surf, falloff_threshold, use_lf, u_emit, need_ray);
		if (!ev.valid) {
			continue;
		}
		float vis = ml_reference_visibility(ray_origin, ev.ray_dir_ws, max(ev.ray_dist - 0.005, 0.0), area_policy_base + i, force_visible);
		total_radiance += (ev.diffuse + ev.specular) * vis;
	}

	uint dir_policy_base = omni_count + spot_count + area_count;
	for (uint i = 0u; i < dir_count; i++) {
		uint lid = ml_pack_sample_light(ML_LIGHT_TYPE_DIRECTIONAL, i);
		vec2 u_emit = need_ray ? ml_stbn_sample_uv(fpx, frame, lid * 31u + 7u) : vec2(0.5);
		MLLightEval ev = ml_eval_directional(ml_directional_lights.data[i], surf, use_soft_shadows, use_lf, u_emit, need_ray);
		if (!ev.valid) {
			continue;
		}
		float vis = ml_reference_visibility(ray_origin, ev.ray_dir_ws, ev.ray_dist, dir_policy_base + i, force_visible);
		total_radiance += (ev.diffuse + ev.specular) * vis;
	}

	ml_welford_accumulate(ivec2(fpx), total_radiance, n);
#endif
}
