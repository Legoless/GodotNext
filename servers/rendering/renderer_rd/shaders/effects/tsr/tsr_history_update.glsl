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

#ifdef TSR_STABILITY
// Stability set (#198). Nothing outside these blocks changes: without TSR_STABILITY this
// is the #194 shader.
#define TSR_STABILITY_UBO_BINDING 7
#include "tsr_stability_inc.glsl"

layout(set = 0, binding = 8) uniform sampler2D depth_tex;
layout(set = 0, binding = 9) uniform sampler2DArray persistent_tex;
layout(set = 0, binding = 10) uniform sampler2D flicker_tex;
layout(set = 0, binding = 11) uniform sampler2D thin_tex;
layout(set = 0, binding = 12) uniform sampler2D reactive_tex;
#endif

void main() {
	ivec2 hpx = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(hpx, params.history_size))) {
		return;
	}

	vec2 u = (vec2(hpx) + 0.5) / vec2(params.history_size);

	// Current jittered input sample position corresponding to unjittered UV u.
	vec2 x_in = u * vec2(params.internal_size) + params.jitter_pixels - 0.5;
	// roundEven, not round: x_in.x can land on an exact .5 tie (the x jitter is a base-2 Halton
	// value, and history and internal sizes often differ by a power of two), where GLSL leaves
	// round()'s tie-breaking direction to the implementation. roundEven maps to SPIR-V RoundEven,
	// DXIL round_ne and MSL rint, so exact .5 ties round to even on all three backends (floating-
	// point evaluation of x_in itself can still differ across drivers or fast-math modes).
	ivec2 base_in = ivec2(roundEven(x_in));
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
#ifdef TSR_STABILITY
	// Reprojection field: the velocity of the nearest rendered pixel, carried inside it by the
	// local 2x2 Jacobian of the dilated velocity (derived here from four neighbors, not
	// stored). It is exact at rendered-pixel centers, linear inside a rendered pixel (rotation,
	// zoom) and stays discontinuous at silhouettes instead of being blended across them by a
	// bilinear tap. Worked in internal pixels; field_axis_* are the columns of the Jacobian of
	// the current-to-previous mapping.
	vec2 field_axis_x = vec2(1.0, 0.0);
	vec2 field_axis_y = vec2(0.0, 1.0);
	if ((stability.features & TSR_FEATURE_REPROJECTION_FIELD) != 0u) {
		vec2 in_size = vec2(params.internal_size);
		ivec2 c = clamp(base_in, ivec2(0), max_in);
		vec2 v_c = sanitize_vec2(texelFetch(dilated_velocity_tex, c, 0).rg) * in_size;
		vec2 v_l = sanitize_vec2(texelFetch(dilated_velocity_tex, clamp(c - ivec2(1, 0), ivec2(0), max_in), 0).rg) * in_size;
		vec2 v_r = sanitize_vec2(texelFetch(dilated_velocity_tex, clamp(c + ivec2(1, 0), ivec2(0), max_in), 0).rg) * in_size;
		vec2 v_t = sanitize_vec2(texelFetch(dilated_velocity_tex, clamp(c - ivec2(0, 1), ivec2(0), max_in), 0).rg) * in_size;
		vec2 v_b = sanitize_vec2(texelFetch(dilated_velocity_tex, clamp(c + ivec2(0, 1), ivec2(0), max_in), 0).rg) * in_size;
		vec2 dvdx = tsr_one_sided(v_c, v_l, v_r, TSR_FIELD_MAX_GRADIENT);
		vec2 dvdy = tsr_one_sided(v_c, v_t, v_b, TSR_FIELD_MAX_GRADIENT);
		// subpixel_delta is this history pixel's offset from the rendered pixel's sample, in internal pixels.
		vel = (v_c + dvdx * subpixel_delta.x + dvdy * subpixel_delta.y) / in_size;
		field_axis_x += dvdx;
		field_axis_y += dvdy;
	}
#endif
	vec2 uv_prev = u + vel;
	if (any(lessThan(uv_prev, vec2(0.0))) || any(greaterThan(uv_prev, vec2(1.0)))) {
		vec3 seed_rgb = perceptual_ycocg_to_rgb(wide_ycocg);
		imageStore(history_color_curr_out, hpx, vec4(seed_rgb, 1.0));
		imageStore(history_meta_curr_out, hpx, vec4(0.0625, 0.0, 0.0, 0.0));
		return;
	}

	float rej = clamp(textureLod(rejection_tex, u, 0.0).r, 0.0, 1.0);
	float prev_conf = clamp(textureLod(history_meta_prev_tex, uv_prev, 0.0).r, 0.0, 1.0);

#ifdef TSR_STABILITY
	// History resurrection: where the previous history was rejected, look for a persistent
	// frame that agrees with the current neighbourhood. The pixel is reprojected with its
	// current depth and the cameras only, so a moving object lands in the wrong place, fails
	// the match and is left alone. Translucency keeps its rejection: resurrection is off where
	// the reactive mask is above TSR_RESURRECT_MAX_REACTIVE, and never lowers the rejection
	// below the reactive mask.
	vec3 res_ycocg = vec3(0.0);
	float res_w = 0.0;
	float reactive = clamp(textureLod(reactive_tex, u, 0.0).r, 0.0, 1.0);
	if ((stability.features & TSR_FEATURE_RESURRECTION) != 0u && rej > TSR_RESURRECT_MIN_REJECTION && reactive <= TSR_RESURRECT_MAX_REACTIVE) {
		vec3 r_mu = m1 * (1.0 / 9.0);
		vec3 r_sigma = sqrt(max(m2 * (1.0 / 9.0) - r_mu * r_mu, vec3(1e-6)));
		float raw_d = texelFetch(depth_tex, clamp(base_in, ivec2(0), max_in), 0).r;
		uint covered;
		float best = tsr_resurrection_match(persistent_tex, u, raw_d, r_mu, r_sigma, res_ycocg, covered);
		// The better the match and the stronger the rejection, the more of the persistent
		// frame is taken: the pixel keeps a converged colour and part of its confidence
		// instead of restarting from one jittered sample.
		res_w = (1.0 - best) * smoothstep(TSR_RESURRECT_MIN_REJECTION, 1.0, rej);
		rej = max(rej * (1.0 - res_w), reactive);
		prev_conf = mix(prev_conf, stability.persistent_confidence, res_w);
	}
#endif

	vec3 curr_ycocg = mix(sharp_ycocg, wide_ycocg, rej);

	vec3 mu = m1 * (1.0 / 9.0);
	vec3 sigma = sqrt(max(m2 * (1.0 / 9.0) - mu * mu, vec3(1e-6)));

	float upscale_ratio = float(params.history_size.x) / max(float(params.internal_size.x), 1.0);
	float upscale_boost = mix(1.0, 1.15, clamp((upscale_ratio - 1.0) * 0.5, 0.0, 1.0) * (1.0 - rej));
	float gamma = mix(1.25, 0.50, rej) * upscale_boost;
#ifdef TSR_STABILITY
	// Where the flicker analysis found periodic flicker, or the thin-geometry analysis found
	// sub-pixel features, a wider clip box lets the history hold instead of following every swing
	// of the input. Without it, a frame whose samples missed a thin wire would clip the wire out
	// of the history whatever the rejection says.
	float relax = 0.0;
	if ((stability.features & TSR_FEATURE_FLICKER) != 0u) {
		relax = tsr_flicker_weight(textureLod(flicker_tex, u, 0.0).r) * TSR_FLICKER_MAX_RELAX;
	}
	if ((stability.features & TSR_FEATURE_THIN_GEOMETRY) != 0u) {
		relax = max(relax, tsr_thin_weight(textureLod(thin_tex, u, 0.0).r));
	}
	gamma = mix(gamma, TSR_RELAX_GAMMA, relax);
#endif

	vec3 cmin = mu - gamma * sigma;
	vec3 cmax = mu + gamma * sigma;

	vec3 hist_rgb = sample_catmull_rom_5tap(history_color_prev_tex, uv_prev, vec2(params.history_size));
#ifdef TSR_STABILITY
	// Reprojection field: where the previous history is minified (one current pixel covers more
	// than one previous texel), blend the Catmull-Rom fetch toward a box over the footprint.
	if ((stability.features & TSR_FEATURE_REPROJECTION_FIELD) != 0u) {
		float footprint = max(length(field_axis_x), length(field_axis_y));
		float box_w = smoothstep(1.0, 1.5, footprint);
		if (box_w > 0.0) {
			hist_rgb = mix(hist_rgb, tsr_footprint_fetch(history_color_prev_tex, uv_prev, field_axis_x, field_axis_y, vec2(params.history_size)), box_w);
		}
	}
#endif
	vec3 hist_ycocg = rgb_to_perceptual_ycocg(hist_rgb);
#ifdef TSR_STABILITY
	hist_ycocg = mix(hist_ycocg, res_ycocg, res_w);
#endif
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
