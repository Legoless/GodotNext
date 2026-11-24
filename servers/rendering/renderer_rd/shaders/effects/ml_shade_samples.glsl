#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: megalights_denoiser (#135, plan §21.3 contract M5, GM-1, GM-7, budget M-5).
// MegaLights sample shade & demodulation pass (plain compute, compiles on all tiers):
//   - MODE_SHADE: turns (sample, visibility) from #142/#146 into demodulated diffuse and specular
//     direct radiance using the #136 M1 inputs, dividing diffuse by max(diffuse_albedo, floor) and
//     specular by the multiscatter split-sum EnvBRDF term with safe floor, and writes two RGBA16F
//     targets at sampling resolution (diffuse.rgb + effective hit distance; specular.rgb + valid).
//     Structured cleanly so #137 (M7) can add SSS back-face transmission evaluation per sample.
//   - MODE_FILL_HALF: when downsample == half, preserves the sampled pixel in each 2x2 block
//     untouched and fills the remaining 3 pixels via a 2x2 joint-bilateral stencil guided by
//     linear depth, normal, roughness, and shading model.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#ifdef MODE_SHADE
#define ML_DECLARE_COMMON_BINDINGS
#endif

#include "ml_light_eval_inc.glsl"

#ifdef MODE_SHADE

layout(set = 0, binding = 20, std430) restrict readonly buffer MLLayerGroupMasks {
	uint data[];
}
layer_group_masks;

layout(set = 0, binding = 21, std430) restrict readonly buffer MLSampleBufferIn {
	uvec4 data[];
}
ml_sample_buffer;

layout(set = 0, binding = 22, std430) restrict readonly buffer MLVisibilityBufferIn {
	uint data[];
}
ml_visibility_buffer;

layout(set = 0, binding = 23) uniform texture2DArray ml_hit_distance_tex;

// Output at sampling resolution:
//   ml_shaded_diffuse_out:  .rgb = demodulated diffuse direct radiance, .a = effective blocker hit distance (miss = 65504.0).
//   ml_shaded_specular_out: .rgb = demodulated specular direct radiance, .a = 1.0 if valid MegaLights pixel else 0.0.
layout(rgba16f, set = 0, binding = 24) uniform restrict writeonly image2D ml_shaded_diffuse_out;
layout(rgba16f, set = 0, binding = 25) uniform restrict writeonly image2D ml_shaded_specular_out;

void main() {
	uvec2 spx = gl_GlobalInvocationID.xy;
	if (spx.x >= params.screen_sample_size.z || spx.y >= params.screen_sample_size.w) {
		return;
	}

	bool downsample_half = params.sample_config.y != 0u;
	uint frame = params.sample_config.z;
	uvec2 fpx = ml_sample_to_full_pixel(spx, params.screen_sample_size.xy, downsample_half, frame);

	uint layer_group = ml_read_layer_group_index(fpx);
	uint layer_mask = (layer_group < params.guiding_tile_info.z) ? layer_group_masks.data[layer_group] : 0xFFFFFFFFu;

	MLSurface surf;
	if (!ml_unpack_surface(fpx, layer_mask, surf)) {
		imageStore(ml_shaded_diffuse_out, ivec2(spx), vec4(0.0, 0.0, 0.0, ML_HIT_DIST_MISS));
		imageStore(ml_shaded_specular_out, ivec2(spx), vec4(0.0, 0.0, 0.0, 0.0));
		return;
	}

	uint spp = clamp(params.sample_config.x, 1u, ML_MAX_SAMPLES_PER_PIXEL);
	uint base_idx = (spx.y * params.screen_sample_size.z + spx.x) * spp;
	float falloff_threshold = params.thresholds.x;
	bool use_lf = (params.sample_config.w & ML_FLAG_USE_LIGHT_FUNCTION) != 0u;
	bool use_soft_shadows = (params.sample_config.w & ML_FLAG_USE_SOFT_SHADOWS) != 0u;
	bool force_visible = (params.sample_config.w & ML_FLAG_FORCE_VISIBLE_ONE) != 0u;

	vec3 diffuse_sum = vec3(0.0);
	vec3 specular_sum = vec3(0.0);
	float occ_hit_weighted = 0.0;
	float occ_weight_sum = 0.0;

	for (uint k = 0u; k < spp; k++) {
		uvec4 sample_rec = ml_sample_buffer.data[base_idx + k];
		if (sample_rec.x == ML_SAMPLE_INVALID) {
			continue;
		}
		float inv_pdf = uintBitsToFloat(sample_rec.y);
		if (!(inv_pdf > 0.0)) {
			continue;
		}

		vec3 vis_rgb = force_visible ? vec3(1.0) : ml_unpack_visibility_rgb(ml_visibility_buffer.data[base_idx + k]);
		float hit_dist = force_visible
				? ML_HIT_DIST_MISS
				: texelFetch(sampler2DArray(ml_hit_distance_tex, ml_sampler_nearest_clamp), ivec3(int(spx.x), int(spx.y), int(k)), 0).r;

		MLLightEval ev = ml_eval_packed_light(sample_rec.x, surf, falloff_threshold, use_soft_shadows, use_lf, vec2(0.5), false);
		if (!ev.valid) {
			continue;
		}

		// Extensibility point for #137 (M7 SSS back-face transmission per sample):
		// surf.shading_model == ML_MODEL_SSS && (surf.model_flags & ML_FLAG_TRANSMISSION) != 0u
		// can evaluate thin/thick back-face transmission here from hit_dist and vis_rgb.

		diffuse_sum += ev.diffuse * vis_rgb * inv_pdf;
		specular_sum += ev.specular * vis_rgb * inv_pdf;

		float occ = clamp(1.0 - ml_luminance(vis_rgb), 0.0, 1.0);
		if (occ > 0.01 && hit_dist >= 0.0 && hit_dist < ML_HIT_DIST_MISS * 0.99) {
			float w_occ = max(ml_luminance(abs(ev.diffuse + ev.specular)) * inv_pdf * occ, 1e-4);
			occ_hit_weighted += hit_dist * w_occ;
			occ_weight_sum += w_occ;
		}
	}

	float inv_spp = 1.0 / float(spp);
	vec3 rad_diff = max(diffuse_sum * inv_spp, vec3(0.0));
	vec3 rad_spec = max(specular_sum * inv_spp, vec3(0.0));

	vec3 demod_diff = rad_diff / ml_diffuse_demod_factor(surf.diffuse_albedo);
	vec3 demod_spec = rad_spec / ml_specular_demod_factor(surf.spec_env_brdf);

	demod_diff = min(max(demod_diff, vec3(0.0)), vec3(65000.0));
	demod_spec = min(max(demod_spec, vec3(0.0)), vec3(65000.0));
	if (any(isnan(demod_diff)) || any(isinf(demod_diff))) {
		demod_diff = vec3(0.0);
	}
	if (any(isnan(demod_spec)) || any(isinf(demod_spec))) {
		demod_spec = vec3(0.0);
	}

	float effective_hit_dist = occ_weight_sum > 0.0 ? clamp(occ_hit_weighted / occ_weight_sum, 0.0, ML_HIT_DIST_MISS) : ML_HIT_DIST_MISS;
	imageStore(ml_shaded_diffuse_out, ivec2(spx), vec4(demod_diff, effective_hit_dist));
	imageStore(ml_shaded_specular_out, ivec2(spx), vec4(demod_spec, 1.0));
}

#elif defined(MODE_FILL_HALF)

layout(set = 0, binding = 0, std140) uniform MLParamsUBO {
	MLSceneParams params;
};

layout(set = 0, binding = 1) uniform texture2D ml_depth_tex;
layout(set = 0, binding = 2) uniform texture2D ml_normal_roughness_tex;
layout(set = 0, binding = 3) uniform texture2D ml_albedo_tex;
layout(set = 0, binding = 4) uniform texture2D ml_material_tex;
layout(set = 0, binding = 5) uniform texture2D ml_sample_diffuse_tex;
layout(set = 0, binding = 6) uniform texture2D ml_sample_specular_tex;
layout(set = 0, binding = 7) uniform sampler ml_sampler_nearest_clamp;

layout(rgba16f, set = 0, binding = 8) uniform restrict writeonly image2D ml_noisy_diffuse_out;
layout(rgba16f, set = 0, binding = 9) uniform restrict writeonly image2D ml_noisy_specular_out;

vec3 ml_reconstruct_view_pos(uvec2 p_px, float p_depth) {
	vec2 uv = (vec2(p_px) + vec2(0.5)) / vec2(params.screen_sample_size.xy);
	vec4 h = params.inv_projection * vec4(uv * 2.0 - vec2(1.0), p_depth, 1.0);
	float safe_w = abs(h.w) > 1e-7 ? h.w : (h.w >= 0.0 ? 1e-7 : -1e-7);
	return h.xyz / safe_w;
}

void main() {
	uvec2 fpx = gl_GlobalInvocationID.xy;
	uvec2 full_size = params.screen_sample_size.xy;
	uvec2 sample_size = params.screen_sample_size.zw;
	if (fpx.x >= full_size.x || fpx.y >= full_size.y) {
		return;
	}

	float center_depth = texelFetch(sampler2D(ml_depth_tex, ml_sampler_nearest_clamp), ivec2(fpx), 0).r;
	if (!(center_depth > 0.0)) {
		imageStore(ml_noisy_diffuse_out, ivec2(fpx), vec4(0.0, 0.0, 0.0, ML_HIT_DIST_MISS));
		imageStore(ml_noisy_specular_out, ivec2(fpx), vec4(0.0, 0.0, 0.0, 0.0));
		return;
	}

	vec4 center_mat = texelFetch(sampler2D(ml_material_tex, ml_sampler_nearest_clamp), ivec2(fpx), 0);
	uint center_model = uint(clamp(round(center_mat.b * 255.0), 0.0, 255.0)) & 3u;
	if (center_model == ML_MODEL_FALLBACK) {
		imageStore(ml_noisy_diffuse_out, ivec2(fpx), vec4(0.0, 0.0, 0.0, ML_HIT_DIST_MISS));
		imageStore(ml_noisy_specular_out, ivec2(fpx), vec4(0.0, 0.0, 0.0, 0.0));
		return;
	}

	uint frame = params.sample_config.z;
	uvec2 off = ml_downsample_offset(frame);
	uvec2 own_spx = min(fpx >> 1u, sample_size - uvec2(1u));
	uvec2 own_fpx = ml_sample_to_full_pixel(own_spx, full_size, true, frame);

	// The sampled pixel in this 2x2 block keeps its exact shaded value untouched.
	if (fpx == own_fpx) {
		vec4 s_diff = texelFetch(sampler2D(ml_sample_diffuse_tex, ml_sampler_nearest_clamp), ivec2(own_spx), 0);
		vec4 s_spec = texelFetch(sampler2D(ml_sample_specular_tex, ml_sampler_nearest_clamp), ivec2(own_spx), 0);
		imageStore(ml_noisy_diffuse_out, ivec2(fpx), s_diff);
		imageStore(ml_noisy_specular_out, ivec2(fpx), s_spec);
		return;
	}

	vec3 center_view_pos = ml_reconstruct_view_pos(fpx, center_depth);
	float center_z = max(-center_view_pos.z, 1e-4);
	vec4 center_nr = texelFetch(sampler2D(ml_normal_roughness_tex, ml_sampler_nearest_clamp), ivec2(fpx), 0);
	vec3 center_normal;
	float center_rough;
	ml_unpack_normal_roughness(center_nr, center_normal, center_rough);

	// 2x2 joint-bilateral fill over the 4 bracketing sampled pixels surrounding fpx.
	vec2 s_coord = (vec2(fpx) - vec2(off)) * 0.5;
	ivec2 s0 = ivec2(floor(s_coord));

	vec3 diff_accum = vec3(0.0);
	float diff_w_sum = 0.0;
	vec3 spec_accum = vec3(0.0);
	float spec_w_sum = 0.0;
	float hit_accum = 0.0;
	float hit_w_sum = 0.0;

	vec4 fallback_diff = vec4(0.0, 0.0, 0.0, ML_HIT_DIST_MISS);
	vec4 fallback_spec = vec4(0.0, 0.0, 0.0, 1.0);
	float best_fallback_w = -1.0;

	float inv_plane_tol = 1.0 / max(0.03 * center_z, 0.01);

	for (int dy = 0; dy <= 1; dy++) {
		for (int dx = 0; dx <= 1; dx++) {
			ivec2 q_spx = s0 + ivec2(dx, dy);
			if (q_spx.x < 0 || q_spx.y < 0 || q_spx.x >= int(sample_size.x) || q_spx.y >= int(sample_size.y)) {
				continue;
			}
			vec4 q_spec = texelFetch(sampler2D(ml_sample_specular_tex, ml_sampler_nearest_clamp), q_spx, 0);
			if (q_spec.a < 0.5) {
				continue;
			}
			vec4 q_diff = texelFetch(sampler2D(ml_sample_diffuse_tex, ml_sampler_nearest_clamp), q_spx, 0);

			uvec2 q_fpx = ml_sample_to_full_pixel(uvec2(q_spx), full_size, true, frame);
			float q_depth = texelFetch(sampler2D(ml_depth_tex, ml_sampler_nearest_clamp), ivec2(q_fpx), 0).r;
			if (!(q_depth > 0.0)) {
				continue;
			}

			vec3 q_view_pos = ml_reconstruct_view_pos(q_fpx, q_depth);
			vec4 q_nr = texelFetch(sampler2D(ml_normal_roughness_tex, ml_sampler_nearest_clamp), ivec2(q_fpx), 0);
			vec3 q_normal;
			float q_rough;
			ml_unpack_normal_roughness(q_nr, q_normal, q_rough);

			vec4 q_mat = texelFetch(sampler2D(ml_material_tex, ml_sampler_nearest_clamp), ivec2(q_fpx), 0);
			uint q_model = uint(clamp(round(q_mat.b * 255.0), 0.0, 255.0)) & 3u;

			vec2 delta_px = vec2(q_fpx) - vec2(fpx);
			float w_spatial = exp(-0.35 * dot(delta_px, delta_px));
			float plane_dist = abs(dot(q_view_pos - center_view_pos, center_normal));
			float w_depth = exp(-plane_dist * inv_plane_tol);
			float ndot = max(dot(center_normal, q_normal), 0.0);
			float ndot2 = ndot * ndot;
			float ndot4 = ndot2 * ndot2;
			float w_normal = ndot4 * ndot4; // pow(ndot, 8)
			float w_model = (q_model == center_model) ? 1.0 : 0.15;

			float rough_diff = abs(q_rough - center_rough);
			float w_diff = w_spatial * w_depth * w_normal * w_model * exp(-rough_diff * 2.0);
			float w_spec = w_spatial * w_depth * (w_normal * w_normal) * w_model * exp(-rough_diff * 8.0);

			if (w_diff > best_fallback_w) {
				best_fallback_w = w_diff;
				fallback_diff = q_diff;
				fallback_spec = q_spec;
			}

			diff_accum += q_diff.rgb * w_diff;
			diff_w_sum += w_diff;
			spec_accum += q_spec.rgb * w_spec;
			spec_w_sum += w_spec;

			if (q_diff.a < ML_HIT_DIST_MISS * 0.99 && q_diff.a >= 0.0) {
				hit_accum += q_diff.a * w_diff;
				hit_w_sum += w_diff;
			}
		}
	}

	vec3 out_diff = diff_w_sum > 1e-6 ? (diff_accum / diff_w_sum) : fallback_diff.rgb;
	vec3 out_spec = spec_w_sum > 1e-6 ? (spec_accum / spec_w_sum) : fallback_spec.rgb;
	float out_hit = hit_w_sum > 1e-6 ? clamp(hit_accum / hit_w_sum, 0.0, ML_HIT_DIST_MISS) : fallback_diff.a;

	imageStore(ml_noisy_diffuse_out, ivec2(fpx), vec4(out_diff, out_hit));
	imageStore(ml_noisy_specular_out, ivec2(fpx), vec4(out_spec, 1.0));
}

#endif
