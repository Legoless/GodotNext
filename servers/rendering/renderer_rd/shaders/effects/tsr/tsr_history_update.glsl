#[compute]

#version 450

#VERSION_DEFINES

#include "tsr_inc.glsl"

layout(local_size_x = TSR_GROUP_SIZE, local_size_y = TSR_GROUP_SIZE, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D scene_color_tex;
layout(set = 0, binding = 1) uniform sampler2D dilated_velocity_tex;
layout(set = 0, binding = 2) uniform sampler2D rejection_tex;
layout(set = 0, binding = 3) uniform sampler2D history_color_prev_tex;
layout(set = 0, binding = 4) uniform sampler2D history_meta_prev_tex;
#ifdef MODE_HISTORY_RGBA16F
layout(rgba16f, set = 0, binding = 5) uniform restrict writeonly image2D history_color_curr_out;
#else
layout(r11f_g11f_b10f, set = 0, binding = 5) uniform restrict writeonly image2D history_color_curr_out;
#endif
layout(r8, set = 0, binding = 6) uniform restrict writeonly image2D history_meta_curr_out;

void main() {
	ivec2 hpx = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(hpx, params.history_size))) {
		return;
	}

	vec2 u = (vec2(hpx) + 0.5) / vec2(params.history_size);

	// Current jittered input sample position corresponding to unjittered UV u.
	vec2 x_in = u * vec2(params.internal_size) + params.jitter_pixels - 0.5;
	ivec2 base_in = ivec2(round(x_in));
	vec2 subpixel_delta = x_in - vec2(base_in);
	ivec2 max_in = params.internal_size - ivec2(1);

	vec3 m1 = vec3(0.0);
	vec3 m2 = vec3(0.0);
	vec3 sharp_sum = vec3(0.0);
	float sharp_w_sum = 0.0;
	vec3 wide_sum = vec3(0.0);
	float wide_w_sum = 0.0;

	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			ivec2 sc = clamp(base_in + ivec2(dx, dy), ivec2(0), max_in);
			vec3 s_rgb = sanitize_hdr(texelFetch(scene_color_tex, sc, 0).rgb);
			vec3 s_ycocg = rgb_to_perceptual_ycocg(s_rgb);

			m1 += s_ycocg;
			m2 += s_ycocg * s_ycocg;

			vec2 d = vec2(float(dx), float(dy)) - subpixel_delta;
			float r2 = dot(d, d);

			float w_sharp = exp(-3.0 * r2);
			sharp_sum += s_ycocg * w_sharp;
			sharp_w_sum += w_sharp;

			float w_wide = exp(-1.15 * r2);
			wide_sum += s_ycocg * w_wide;
			wide_w_sum += w_wide;
		}
	}

	vec3 sharp_ycocg = sharp_sum / max(sharp_w_sum, 1e-5);
	vec3 wide_ycocg = wide_sum / max(wide_w_sum, 1e-5);

	if ((params.flags & TSR_FLAG_RESET) != 0u) {
		vec3 seed_rgb = perceptual_ycocg_to_rgb(wide_ycocg);
		imageStore(history_color_curr_out, hpx, vec4(seed_rgb, 1.0));
		imageStore(history_meta_curr_out, hpx, vec4(0.0625, 0.0, 0.0, 0.0));
		return;
	}

	vec2 vel = sanitize_vec2(textureLod(dilated_velocity_tex, u, 0.0).rg);
	vec2 uv_prev = u + vel;
	if (any(lessThan(uv_prev, vec2(0.0))) || any(greaterThan(uv_prev, vec2(1.0)))) {
		vec3 seed_rgb = perceptual_ycocg_to_rgb(wide_ycocg);
		imageStore(history_color_curr_out, hpx, vec4(seed_rgb, 1.0));
		imageStore(history_meta_curr_out, hpx, vec4(0.0625, 0.0, 0.0, 0.0));
		return;
	}

	float rej = clamp(textureLod(rejection_tex, u, 0.0).r, 0.0, 1.0);
	float prev_conf = clamp(textureLod(history_meta_prev_tex, uv_prev, 0.0).r, 0.0, 1.0);

	vec3 curr_ycocg = mix(sharp_ycocg, wide_ycocg, rej);

	vec3 mu = m1 * (1.0 / 9.0);
	vec3 sigma = sqrt(max(m2 * (1.0 / 9.0) - mu * mu, vec3(1e-6)));

	float upscale_ratio = float(params.history_size.x) / max(float(params.internal_size.x), 1.0);
	float upscale_boost = mix(1.0, 1.15, clamp((upscale_ratio - 1.0) * 0.5, 0.0, 1.0) * (1.0 - rej));
	float gamma = mix(1.25, 0.50, rej) * upscale_boost;

	vec3 cmin = mu - gamma * sigma;
	vec3 cmax = mu + gamma * sigma;

	vec3 hist_rgb = sample_catmull_rom_5tap(history_color_prev_tex, uv_prev, vec2(params.history_size));
	vec3 hist_ycocg = rgb_to_perceptual_ycocg(hist_rgb);
	vec3 clipped_hist = clip_aabb(cmin, cmax, hist_ycocg);

	float eff_conf = prev_conf * (1.0 - rej);
	float align_w = exp(-2.5 * dot(subpixel_delta, subpixel_delta));
	float base_alpha = mix(0.04, 0.12, align_w);
	float bootstrap_alpha = 1.0 / (1.0 + eff_conf * 16.0);
	float alpha = clamp(max(mix(base_alpha, 1.0, rej), bootstrap_alpha * mix(0.35, 1.0, max(align_w, rej))), 0.03, 1.0);

	vec3 out_ycocg = mix(clipped_hist, curr_ycocg, alpha);
	vec3 out_rgb = perceptual_ycocg_to_rgb(out_ycocg);

	float conf_curr = clamp(mix(min(eff_conf + 0.0625, 1.0), 0.0625, rej), 0.0625, 1.0);

	imageStore(history_color_curr_out, hpx, vec4(out_rgb, 1.0));
	imageStore(history_meta_curr_out, hpx, vec4(conf_curr, 0.0, 0.0, 0.0));
}
