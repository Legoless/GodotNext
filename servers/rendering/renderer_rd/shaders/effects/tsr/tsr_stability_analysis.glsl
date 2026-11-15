#[compute]

#version 450

#VERSION_DEFINES

#include "tsr_inc.glsl"

// Clean-room TSR stability analysis (#198, docs/godotnext/tsr-sources.md). MODE_FLICKER
// advances the one-byte flicker state of each texel (described in tsr_stability.h) from the
// luma of the internal pixel at its center against the reprojected history, which is the
// running mean a flickering input swings around. There is no luma ring: the state counts
// time in 1/120 s ticks, so it is one byte at any frame rate. MODE_THIN advances the
// thin-geometry coverage of each texel (half the internal size): whether the 2x2 internal
// pixels at its center hold a feature one pixel wide, in depth or in luma, averaged over
// about one jitter cycle along the reprojection.

layout(local_size_x = TSR_GROUP_SIZE, local_size_y = TSR_GROUP_SIZE, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D scene_color_tex;
layout(set = 0, binding = 1) uniform sampler2D reactive_tex;
layout(set = 0, binding = 2) uniform sampler2D dilated_velocity_tex;
#ifdef MODE_FLICKER
layout(set = 0, binding = 3) uniform sampler2D history_color_prev_tex;
layout(set = 0, binding = 4) uniform sampler2D disocclusion_tex;
#endif
#ifdef MODE_THIN
layout(set = 0, binding = 3) uniform sampler2D depth_tex;
#endif

#define TSR_STABILITY_UBO_BINDING 5
#include "tsr_stability_inc.glsl"

layout(set = 0, binding = 6) uniform sampler2D state_prev_tex;
layout(r8, set = 0, binding = 7) uniform restrict writeonly image2D state_out;

#ifdef MODE_FLICKER
void main() {
	ivec2 t = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(t, stability.flicker_size))) {
		return;
	}

	uint next_state = TSR_FLICKER_STATE_DISABLED;
	if ((params.flags & TSR_FLAG_RESET) == 0u) {
		// The internal pixel at this texel's center: the state may be stored smaller than the
		// internal image when T6 leaves no room for the full size.
		ivec2 max_in = params.internal_size - ivec2(1);
		ivec2 px = clamp(ivec2((vec2(t) + 0.5) * vec2(params.internal_size) / vec2(stability.flicker_size)), ivec2(0), max_in);

		vec2 vel = sanitize_vec2(texelFetch(dilated_velocity_tex, px, 0).rg);
		// The history at the input sample's own jittered position, reprojected.
		vec2 uv_prev = (vec2(px) + 0.5 - params.jitter_pixels) / vec2(params.internal_size) + vel;
		bool on_screen = all(greaterThanEqual(uv_prev, vec2(0.0))) && all(lessThanEqual(uv_prev, vec2(1.0)));
		bool still = length(vel * vec2(params.internal_size)) <= stability.flicker_max_motion;
		float disocc = texelFetch(disocclusion_tex, px, 0).r;
		float reactive = clamp(texelFetch(reactive_tex, px, 0).r, 0.0, 1.0);

		// Off for moving pixels, disocclusions and translucency: the state restarts there.
		if (on_screen && still && disocc <= TSR_FLICKER_MAX_DISOCCLUSION && reactive <= TSR_FLICKER_MAX_REACTIVE) {
			float y_in = rgb_to_perceptual_ycocg(texelFetch(scene_color_tex, px, 0).rgb).x;
			float y_hist = rgb_to_perceptual_ycocg(textureLod(history_color_prev_tex, uv_prev, 0.0).rgb).x;
			float d = y_in - y_hist;
			uint state = uint(round(texelFetch(state_prev_tex, t, 0).r * 255.0));
			next_state = tsr_flicker_step(state, d > 0.0, abs(d) > TSR_FLICKER_MIN_DELTA, stability.flicker_ticks, stability.flicker_decay_steps);
		}
	}

	imageStore(state_out, t, vec4(float(next_state) / 255.0, 0.0, 0.0, 0.0));
}
#endif

#ifdef MODE_THIN
void main() {
	ivec2 t = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(t, stability.thin_size))) {
		return;
	}

	float coverage = 0.0;
	if ((params.flags & TSR_FLAG_RESET) == 0u) {
		// The 2x2 internal pixels at this texel's center: the coverage may be stored smaller than
		// half the internal image when T6 leaves no room for the full size.
		ivec2 max_in = params.internal_size - ivec2(1);
		vec2 center = (vec2(t) + 0.5) * vec2(params.internal_size) / vec2(stability.thin_size);
		ivec2 base = ivec2(floor(center - 0.5));

		// Linear view depth and perceptual luma of the 4x4 pixels around them. A neighbor
		// clamped to the image edge equals the pixel, so the edge never reads as a feature.
		float z[4][4];
		float y[4][4];
		for (int j = 0; j < 4; j++) {
			for (int i = 0; i < 4; i++) {
				ivec2 p = clamp(base + ivec2(i - 1, j - 1), ivec2(0), max_in);
				z[j][i] = linearize_depth(texelFetch(depth_tex, p, 0).r);
				y[j][i] = rgb_to_perceptual_ycocg(texelFetch(scene_color_tex, p, 0).rgb).x;
			}
		}

		// A pixel holds a thin feature when, along the horizontal, the vertical or a diagonal
		// through it, both opposite neighbors are farther than it by more than
		// TSR_THIN_DEPTH_STEP of its depth (a depth ridge one pixel wide), or both differ from
		// it in luma with the same sign by more than TSR_THIN_LUMA_STEP (a luma line). Depth
		// ridges and luma lines are detected separately: a wire in front of a surface of the
		// same colour has the first, a painted line the second.
		const ivec2 axes[4] = ivec2[](ivec2(1, 0), ivec2(0, 1), ivec2(1, 1), ivec2(1, -1));
		float thin_now = 0.0;
		float reactive = 0.0;
		for (int j = 1; j <= 2; j++) {
			for (int i = 1; i <= 2; i++) {
				reactive = max(reactive, clamp(texelFetch(reactive_tex, clamp(base + ivec2(i - 1, j - 1), ivec2(0), max_in), 0).r, 0.0, 1.0));
				float z_c = z[j][i];
				float y_c = y[j][i];
				float z_step = TSR_THIN_DEPTH_STEP * z_c;
				for (int a = 0; a < 4; a++) {
					ivec2 d = axes[a];
					float z_a = z[j - d.y][i - d.x] - z_c;
					float z_b = z[j + d.y][i + d.x] - z_c;
					float y_a = y[j - d.y][i - d.x] - y_c;
					float y_b = y[j + d.y][i + d.x] - y_c;
					bool ridge = z_a > z_step && z_b > z_step;
					bool line = (y_a > TSR_THIN_LUMA_STEP && y_b > TSR_THIN_LUMA_STEP) || (y_a < -TSR_THIN_LUMA_STEP && y_b < -TSR_THIN_LUMA_STEP);
					if (ridge || line) {
						thin_now = 1.0;
					}
				}
			}
		}

		// Averaged along the reprojection; it restarts off the screen, and translucency keeps
		// its rejection (no coverage where the reactive mask is set).
		ivec2 pc = clamp(ivec2(center), ivec2(0), max_in);
		vec2 vel = sanitize_vec2(texelFetch(dilated_velocity_tex, pc, 0).rg);
		vec2 uv_prev = (vec2(t) + 0.5) / vec2(stability.thin_size) + vel;
		bool on_screen = all(greaterThanEqual(uv_prev, vec2(0.0))) && all(lessThanEqual(uv_prev, vec2(1.0)));
		if (on_screen && reactive <= TSR_THIN_MAX_REACTIVE) {
			float coverage_prev = textureLod(state_prev_tex, uv_prev, 0.0).r;
			coverage = mix(coverage_prev, thin_now, TSR_THIN_COVERAGE_RATE);
		}
	}

	// Stored truncated to the byte, so a coverage with no new detection decays to 0 instead of
	// rounding back up to its old value.
	imageStore(state_out, t, vec4(floor(coverage * 255.0 + 1e-3) / 255.0, 0.0, 0.0, 0.0));
}
#endif
