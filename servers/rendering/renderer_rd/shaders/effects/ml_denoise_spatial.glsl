#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: megalights_denoiser (#135, plan §21.3 contract M5, GM-1, GM-7, budget M-5).
// MegaLights variance-guided spatial à-trous filter (2 iterations, step widths 1 and 2) and
// on-demand debug visualization pass (ML_NOISY, ML_DENOISED, ML_VARIANCE):
//   - MODE_SPATIAL: 3x3 à-trous edge-stopping filter whose kernel width and luminance tolerance
//     follow temporal variance, guided by linear depth (plane distance), view normal, roughness,
//     shading model, and hit distance (#146 hit_distance_texture, tightening contact shadows while
//     smoothing soft penumbrae). Propagates filtered variance across iterations.
//   - MODE_DEBUG_VIS: renders tone-mapped ML_NOISY, ML_DENOISED, or false-color ML_VARIANCE on demand.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "../environment/nextgi_view_inc.glsl"

#define ML_HIT_DIST_MISS 65504.0

// std140, 336 bytes. Must match RendererRD::NextGIDenoise::MegaLightsDenoiseParamsGPU.
struct MLDenoiseParams {
	mat4 inv_projection;
	mat4 inv_view_proj;
	mat4 prev_view_proj;
	vec4 view_to_world[3];
	vec4 cur_depth_coeffs;
	vec4 prev_depth_coeffs;
	vec4 camera_pos;
	vec4 prev_camera_pos;
	uvec4 screen_reset_size; // xy = screen_size, zw = reset_tiles_xy
	uvec4 config; // x = frame_index, y = flags, z = max_history_diff, w = max_history_spec
};

layout(set = 0, binding = 0, std140) uniform MLDenoiseParamsUBO {
	MLDenoiseParams params;
};

float ml_spatial_luma(vec3 p_c) {
	return dot(p_c, vec3(0.2126, 0.7152, 0.0722));
}

void ml_spatial_unpack_nr(vec4 p_nr, out vec3 r_view_normal, out float r_roughness) {
	float r = p_nr.w > 0.5 ? (1.0 - p_nr.w) : p_nr.w;
	r_roughness = clamp(r / (127.0 / 255.0), 0.0, 1.0);
	vec3 n = p_nr.xyz * 2.0 - vec3(1.0);
	float len2 = dot(n, n);
	r_view_normal = len2 > 1e-12 ? n * inversesqrt(len2) : vec3(0.0, 0.0, 1.0);
}

#ifdef MODE_SPATIAL

layout(set = 0, binding = 1) uniform sampler2D source_diffuse_tex;
layout(set = 0, binding = 2) uniform sampler2D source_specular_tex;
layout(set = 0, binding = 3) uniform sampler2D noisy_diffuse_tex;
layout(set = 0, binding = 4) uniform sampler2D noisy_specular_tex;
layout(set = 0, binding = 5) uniform sampler2D depth_tex;
layout(set = 0, binding = 6) uniform sampler2D normal_roughness_tex;
layout(set = 0, binding = 7) uniform sampler2D ml_material_tex;

layout(rgba16f, set = 0, binding = 8) uniform restrict writeonly image2D output_diffuse_img;
layout(rgba16f, set = 0, binding = 9) uniform restrict writeonly image2D output_specular_img;

layout(push_constant, std430) uniform Push {
	uint step_size;
	uint pass_index;
	uint pad0;
	uint pad1;
}
push;

const float ATROUS_KERNEL[3] = float[3](0.25, 0.5, 0.25);

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 screen_size = ivec2(params.screen_reset_size.xy);
	if (px.x >= screen_size.x || px.y >= screen_size.y) {
		return;
	}

	float center_depth = texelFetch(depth_tex, px, 0).r;
	float center_valid = texelFetch(noisy_specular_tex, px, 0).a;
	if (nextgi_view_is_background(center_depth) || center_valid < 0.5) {
		imageStore(output_diffuse_img, px, vec4(0.0));
		imageStore(output_specular_img, px, vec4(0.0));
		return;
	}

	vec4 center_diff = texelFetch(source_diffuse_tex, px, 0);
	vec4 center_spec = texelFetch(source_specular_tex, px, 0);
	float center_diff_luma = ml_spatial_luma(center_diff.rgb);
	float center_spec_luma = ml_spatial_luma(center_spec.rgb);

	vec2 uv = (vec2(px) + vec2(0.5)) / vec2(screen_size);
	vec3 center_view_pos = nextgi_view_reconstruct(params.inv_projection, uv, center_depth);
	float center_z = max(-center_view_pos.z, 1e-4);

	vec3 center_normal;
	float center_rough;
	ml_spatial_unpack_nr(texelFetch(normal_roughness_tex, px, 0), center_normal, center_rough);

	uint center_model = uint(clamp(round(texelFetch(ml_material_tex, px, 0).b * 255.0), 0.0, 255.0)) & 3u;

	// Local 3x3 variance smoothing + contact-shadow minimum blocker distance check.
	float filt_var_diff = 0.0;
	float filt_var_spec = 0.0;
	float var_w_sum = 0.0;
	float min_hit = texelFetch(noisy_diffuse_tex, px, 0).a;

	for (int oy = -1; oy <= 1; oy++) {
		for (int ox = -1; ox <= 1; ox++) {
			ivec2 q = clamp(px + ivec2(ox, oy), ivec2(0), screen_size - ivec2(1));
			float gw = ATROUS_KERNEL[ox + 1] * ATROUS_KERNEL[oy + 1];
			filt_var_diff += max(texelFetch(source_diffuse_tex, q, 0).a, 0.0) * gw;
			filt_var_spec += max(texelFetch(source_specular_tex, q, 0).a, 0.0) * gw;
			var_w_sum += gw;
			if (abs(ox) + abs(oy) == 1) {
				float qh = texelFetch(noisy_diffuse_tex, q, 0).a;
				if (qh >= 0.0 && qh < min_hit) {
					min_hit = qh;
				}
			}
		}
	}
	filt_var_diff /= max(var_w_sum, 1e-6);
	filt_var_spec /= max(var_w_sum, 1e-6);

	float sigma_diff = sqrt(max(filt_var_diff, 0.0));
	float sigma_spec = sqrt(max(filt_var_spec, 0.0));

	// Variance-guided edge-stopping scale: high variance widens filter, low variance preserves crisp detail.
	float phi_diff = max(4.0 * sigma_diff, 1e-4);
	float phi_spec = max(mix(2.0, 4.0, center_rough) * sigma_spec, 1e-4);

	// Hit-distance & roughness-guided kernel width: tightens near contact shadows, widens for soft penumbrae.
	float hit_scale = 1.0;
	if (min_hit < ML_HIT_DIST_MISS * 0.5) {
		hit_scale = clamp(0.30 + 0.70 * smoothstep(0.05, 1.5, min_hit), 0.30, 1.0);
	}
	float diff_var_boost = clamp(0.5 + 1.5 * sigma_diff / max(center_diff_luma + 0.05, 0.05), 0.5, 1.5);
	float spec_var_boost = clamp(0.4 + 1.4 * sigma_spec / max(center_spec_luma + 0.05, 0.05), 0.4, 1.5);
	float diff_radius = max(hit_scale * diff_var_boost, 0.25);
	float spec_radius = max(hit_scale * clamp(center_rough * 2.5, 0.20, 1.0) * spec_var_boost, 0.20);

	float inv_plane_tol = 1.0 / max(0.025 * center_z, 0.01);
	float center_hit = texelFetch(noisy_diffuse_tex, px, 0).a;
	int step_px = int(max(push.step_size, 1u));

	vec3 diff_sum = center_diff.rgb;
	float diff_var_sum = max(center_diff.a, 0.0);
	float diff_w_sum = 1.0;

	vec3 spec_sum = center_spec.rgb;
	float spec_var_sum = max(center_spec.a, 0.0);
	float spec_w_sum = 1.0;

	for (int oy = -1; oy <= 1; oy++) {
		for (int ox = -1; ox <= 1; ox++) {
			if (ox == 0 && oy == 0) {
				continue;
			}
			ivec2 q = px + ivec2(ox, oy) * step_px;
			if (q.x < 0 || q.y < 0 || q.x >= screen_size.x || q.y >= screen_size.y) {
				continue;
			}
			float q_depth = texelFetch(depth_tex, q, 0).r;
			if (nextgi_view_is_background(q_depth)) {
				continue;
			}
			if (texelFetch(noisy_specular_tex, q, 0).a < 0.5) {
				continue;
			}

			vec2 q_uv = (vec2(q) + vec2(0.5)) / vec2(screen_size);
			vec3 q_view_pos = nextgi_view_reconstruct(params.inv_projection, q_uv, q_depth);
			float plane_dist = abs(dot(q_view_pos - center_view_pos, center_normal));
			float w_depth = exp(-plane_dist * inv_plane_tol);
			if (w_depth < 1e-4) {
				continue;
			}

			vec3 q_normal;
			float q_rough;
			ml_spatial_unpack_nr(texelFetch(normal_roughness_tex, q, 0), q_normal, q_rough);
			float ndot = max(dot(center_normal, q_normal), 0.0);
			float ndot2 = ndot * ndot;
			float ndot4 = ndot2 * ndot2;
			float w_normal_diff = ndot4 * ndot4; // pow(ndot, 8)
			float w_normal_spec = w_normal_diff * w_normal_diff; // pow(ndot, 16)

			uint q_model = uint(clamp(round(texelFetch(ml_material_tex, q, 0).b * 255.0), 0.0, 255.0)) & 3u;
			float w_model = (q_model == center_model) ? 1.0 : 0.15;

			float rough_delta = abs(q_rough - center_rough);
			float w_rough_diff = exp(-rough_delta * 2.0);
			float w_rough_spec = exp(-rough_delta * 6.0);

			float q_hit = texelFetch(noisy_diffuse_tex, q, 0).a;
			float w_hit = 1.0;
			if (center_hit < ML_HIT_DIST_MISS * 0.5 && q_hit < ML_HIT_DIST_MISS * 0.5) {
				w_hit = exp(-abs(log(max(q_hit, 0.05) / max(center_hit, 0.05))) * 0.5);
			}

			float k_base = ATROUS_KERNEL[ox + 1] * ATROUS_KERNEL[oy + 1] * 4.0; // Normalized so center=1.0
			float dist2 = float(ox * ox + oy * oy);
			float w_spatial_diff = k_base * exp(-dist2 / (2.0 * diff_radius * diff_radius + 1e-4));
			float w_spatial_spec = k_base * exp(-dist2 / (2.0 * spec_radius * spec_radius + 1e-4));

			vec4 q_diff = texelFetch(source_diffuse_tex, q, 0);
			vec4 q_spec = texelFetch(source_specular_tex, q, 0);

			float w_luma_diff = exp(-abs(ml_spatial_luma(q_diff.rgb) - center_diff_luma) / (phi_diff + 1e-4));
			float w_luma_spec = exp(-abs(ml_spatial_luma(q_spec.rgb) - center_spec_luma) / (phi_spec + 1e-4));

			float wd = w_spatial_diff * w_depth * w_normal_diff * w_rough_diff * w_model * w_hit * w_luma_diff;
			float ws = w_spatial_spec * w_depth * w_normal_spec * w_rough_spec * w_model * w_hit * w_luma_spec;

			diff_sum += q_diff.rgb * wd;
			diff_var_sum += max(q_diff.a, 0.0) * (wd * wd);
			diff_w_sum += wd;

			spec_sum += q_spec.rgb * ws;
			spec_var_sum += max(q_spec.a, 0.0) * (ws * ws);
			spec_w_sum += ws;
		}
	}

	vec3 out_diff = diff_sum / max(diff_w_sum, 1e-6);
	float out_var_diff = diff_var_sum / max(diff_w_sum * diff_w_sum, 1e-6);
	vec3 out_spec = spec_sum / max(spec_w_sum, 1e-6);
	float out_var_spec = spec_var_sum / max(spec_w_sum * spec_w_sum, 1e-6);

	imageStore(output_diffuse_img, px, vec4(min(max(out_diff, vec3(0.0)), vec3(65000.0)), clamp(out_var_diff, 0.0, 65000.0)));
	imageStore(output_specular_img, px, vec4(min(max(out_spec, vec3(0.0)), vec3(65000.0)), clamp(out_var_spec, 0.0, 65000.0)));
}

#elif defined(MODE_DEBUG_VIS)

layout(set = 0, binding = 1) uniform sampler2D noisy_diffuse_tex;
layout(set = 0, binding = 2) uniform sampler2D noisy_specular_tex;
layout(set = 0, binding = 3) uniform sampler2D denoised_diffuse_tex;
layout(set = 0, binding = 4) uniform sampler2D denoised_specular_tex;
layout(set = 0, binding = 5) uniform sampler2D variance_diffuse_tex;
layout(set = 0, binding = 6) uniform sampler2D variance_specular_tex;
layout(set = 0, binding = 7) uniform sampler2D depth_tex;
layout(rgba16f, set = 0, binding = 8) uniform restrict writeonly image2D debug_vis_out;

layout(push_constant, std430) uniform Push {
	uint debug_mode; // 0 = ML_NOISY, 1 = ML_DENOISED, 2 = ML_VARIANCE
	uint rr_bypass;
	uint pad0;
	uint pad1;
}
push;

vec3 ml_reinhard_vis(vec3 p_c) {
	vec3 c = max(p_c, vec3(0.0));
	return c / (vec3(1.0) + c);
}

vec3 ml_variance_heatmap(float p_t) {
	float t = clamp(p_t, 0.0, 1.0);
	// Blue (converged low variance) -> Cyan -> Green -> Yellow -> Red (high variance).
	vec3 c0 = vec3(0.08, 0.20, 0.55);
	vec3 c1 = vec3(0.10, 0.72, 0.80);
	vec3 c2 = vec3(0.20, 0.85, 0.25);
	vec3 c3 = vec3(0.95, 0.85, 0.12);
	vec3 c4 = vec3(0.95, 0.15, 0.10);
	if (t < 0.25) {
		return mix(c0, c1, t * 4.0);
	}
	if (t < 0.50) {
		return mix(c1, c2, (t - 0.25) * 4.0);
	}
	if (t < 0.75) {
		return mix(c2, c3, (t - 0.50) * 4.0);
	}
	return mix(c3, c4, (t - 0.75) * 4.0);
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	ivec2 screen_size = ivec2(params.screen_reset_size.xy);
	if (px.x >= screen_size.x || px.y >= screen_size.y) {
		return;
	}

	float depth = texelFetch(depth_tex, px, 0).r;
	if (nextgi_view_is_background(depth)) {
		imageStore(debug_vis_out, px, vec4(0.0, 0.0, 0.0, 1.0));
		return;
	}

	vec3 out_rgb = vec3(0.0);
	if (push.debug_mode == 0u) {
		// ML_NOISY: demodulated noisy direct lighting (diffuse + specular).
		vec3 nd = max(texelFetch(noisy_diffuse_tex, px, 0).rgb, vec3(0.0));
		vec3 ns = max(texelFetch(noisy_specular_tex, px, 0).rgb, vec3(0.0));
		out_rgb = ml_reinhard_vis(nd + ns);
	} else if (push.debug_mode == 1u) {
		// ML_DENOISED: denoised direct lighting (or noisy passthrough in rr_bypass).
		vec3 dd = max(texelFetch(denoised_diffuse_tex, px, 0).rgb, vec3(0.0));
		vec3 ds = max(texelFetch(denoised_specular_tex, px, 0).rgb, vec3(0.0));
		out_rgb = ml_reinhard_vis(dd + ds);
	} else {
		// ML_VARIANCE: temporal variance heatmap across diffuse + specular.
		float vd = push.rr_bypass != 0u ? 0.0 : max(texelFetch(variance_diffuse_tex, px, 0).a, 0.0);
		float vs = push.rr_bypass != 0u ? 0.0 : max(texelFetch(variance_specular_tex, px, 0).a, 0.0);
		float sigma = sqrt(vd + vs);
		float mapped = sigma / (0.25 + sigma);
		out_rgb = ml_variance_heatmap(mapped);
	}

	imageStore(debug_vis_out, px, vec4(out_rgb, 1.0));
}

#endif
