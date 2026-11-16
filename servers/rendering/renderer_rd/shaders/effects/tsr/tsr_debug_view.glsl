#[compute]

#version 450

#VERSION_DEFINES

#include "tsr_inc.glsl"

// Clean-room TSR pass visualizer (#198, docs/godotnext/tsr-sources.md). Draws one TSR pass,
// chosen by stability.debug_view (TSRDebugView in tsr_stability.h), over the scene's luma at 25%,
// at the output size. It runs after the resolve and only reads: no TSR texture changes.
// Legend: green helps the image, red or yellow costs it, blue is below a threshold, and magenta
// means the feature is off, for the pixel or for the whole view.

layout(local_size_x = TSR_GROUP_SIZE, local_size_y = TSR_GROUP_SIZE, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D output_color_tex;
layout(set = 0, binding = 1) uniform sampler2D history_meta_tex;
layout(set = 0, binding = 2) uniform sampler2D rejection_tex;
layout(set = 0, binding = 3) uniform sampler2D disocclusion_tex;
layout(set = 0, binding = 4) uniform sampler2D depth_tex;
layout(set = 0, binding = 5) uniform sampler2D scene_color_tex;
layout(set = 0, binding = 6) uniform sampler2DArray persistent_tex;
layout(set = 0, binding = 7) uniform sampler2D flicker_tex;
layout(set = 0, binding = 8) uniform sampler2D thin_tex;

#define TSR_STABILITY_UBO_BINDING 9
#include "tsr_stability_inc.glsl"

layout(rgba8, set = 0, binding = 10) uniform restrict writeonly image2D debug_out;
layout(set = 0, binding = 11) uniform sampler2D reactive_tex;

// TSRDebugView in tsr_stability.h.
#define TSR_DEBUG_VIEW_HISTORY_VALIDITY 0u
#define TSR_DEBUG_VIEW_REJECTION 1u
#define TSR_DEBUG_VIEW_DISOCCLUSION 2u
#define TSR_DEBUG_VIEW_RESURRECTION_HITS 3u
#define TSR_DEBUG_VIEW_FLICKER_PERIOD 4u
#define TSR_DEBUG_VIEW_THIN_COVERAGE 5u

const vec3 TSR_DEBUG_GREEN = vec3(0.1, 0.9, 0.2);
const vec3 TSR_DEBUG_RED = vec3(0.95, 0.1, 0.1);
const vec3 TSR_DEBUG_YELLOW = vec3(0.95, 0.85, 0.1);
const vec3 TSR_DEBUG_BLUE = vec3(0.15, 0.35, 0.95);
const vec3 TSR_DEBUG_MAGENTA = vec3(1.0, 0.0, 1.0);

void main() {
	ivec2 opx = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(opx, params.output_size))) {
		return;
	}

	// Output and history pixels share the unjittered UV.
	vec2 u = (vec2(opx) + 0.5) / vec2(params.output_size);

	// The background: the scene's tone-mapped luma, display-encoded, at 25%.
	float luma = rgb_to_perceptual_ycocg(textureLod(output_color_tex, u, 0.0).rgb).x;
	vec3 bg = vec3(0.25 * sqrt(clamp(luma, 0.0, 1.0)));
	vec3 color = bg;

	uint mode = stability.debug_view;
	if (mode == TSR_DEBUG_VIEW_HISTORY_VALIDITY) {
		// The confidence the history update wrote: 1/16, one frame (red), to 1, converged (green).
		float conf = clamp(textureLod(history_meta_tex, u, 0.0).r, 0.0, 1.0);
		float t = clamp((conf - 0.0625) / (1.0 - 0.0625), 0.0, 1.0);
		color = mix(bg, mix(TSR_DEBUG_RED, TSR_DEBUG_GREEN, t), 0.75);
	} else if (mode == TSR_DEBUG_VIEW_REJECTION) {
		// The rejection weight, after the flicker and thin-geometry relaxation.
		color = mix(bg, TSR_DEBUG_RED, clamp(textureLod(rejection_tex, u, 0.0).r, 0.0, 1.0));
	} else if (mode == TSR_DEBUG_VIEW_DISOCCLUSION) {
		// The parallax disocclusion weight.
		color = mix(bg, TSR_DEBUG_YELLOW, clamp(textureLod(disocclusion_tex, u, 0.0).r, 0.0, 1.0));
	} else if (mode == TSR_DEBUG_VIEW_RESURRECTION_HITS) {
		if ((stability.features & TSR_FEATURE_RESURRECTION) == 0u) {
			color = TSR_DEBUG_MAGENTA;
		} else {
			// The history update's resurrection test, recomputed here: the previous history is
			// overwritten by now, and a mask texture would not fit T6.
			float rej = clamp(textureLod(rejection_tex, u, 0.0).r, 0.0, 1.0);
			float reactive = clamp(textureLod(reactive_tex, u, 0.0).r, 0.0, 1.0);
			if (rej > TSR_RESURRECT_MIN_REJECTION && reactive > TSR_RESURRECT_MAX_REACTIVE) {
				// Rejected and translucent: resurrection is off for the pixel.
				color = mix(bg, TSR_DEBUG_MAGENTA, 0.75);
			} else if (rej > TSR_RESURRECT_MIN_REJECTION) {
				vec2 x_in = u * vec2(params.internal_size) + params.jitter_pixels - 0.5;
				ivec2 base_in = ivec2(roundEven(x_in)); // As the history update rounds it.
				ivec2 max_in = params.internal_size - ivec2(1);
				vec3 m1 = vec3(0.0);
				vec3 m2 = vec3(0.0);
				for (int dy = -1; dy <= 1; dy++) {
					for (int dx = -1; dx <= 1; dx++) {
						vec3 s = rgb_to_perceptual_ycocg(sanitize_hdr(texelFetch(scene_color_tex, clamp(base_in + ivec2(dx, dy), ivec2(0), max_in), 0).rgb));
						m1 += s;
						m2 += s * s;
					}
				}
				vec3 mu = m1 * (1.0 / 9.0);
				vec3 sigma = sqrt(max(m2 * (1.0 / 9.0) - mu * mu, vec3(1e-6)));
				float raw_d = texelFetch(depth_tex, clamp(base_in, ivec2(0), max_in), 0).r;
				vec3 res_ycocg;
				uint covered;
				float best = tsr_resurrection_match(persistent_tex, u, raw_d, mu, sigma, res_ycocg, covered);
				if (covered == 0u) {
					// Rejected, and no valid persistent frame covers the pixel.
					color = mix(bg, TSR_DEBUG_MAGENTA, 0.75);
				} else {
					color = mix(bg, TSR_DEBUG_GREEN, (1.0 - best) * smoothstep(TSR_RESURRECT_MIN_REJECTION, 1.0, rej));
				}
			}
		}
	} else if (mode == TSR_DEBUG_VIEW_FLICKER_PERIOD) {
		if ((stability.features & TSR_FEATURE_FLICKER) == 0u) {
			color = TSR_DEBUG_MAGENTA;
		} else {
			// The state byte of this pixel's texel: brightness by the score; hue by the time since
			// the last reversal against the window (green inside it, yellow at its edge, blue slower).
			ivec2 st = clamp(ivec2(u * vec2(stability.flicker_size)), ivec2(0), stability.flicker_size - ivec2(1));
			uint state = uint(round(texelFetch(flicker_tex, st, 0).r * 255.0));
			int since = int(state & 7u);
			float score = float(state >> 4u) / float(TSR_FLICKER_SCORE_MAX);
			if (since == TSR_FLICKER_SINCE_DISABLED) {
				color = mix(bg, TSR_DEBUG_MAGENTA, 0.75);
			} else {
				vec3 hue = since < TSR_FLICKER_HALF_PERIOD_TICKS ? TSR_DEBUG_GREEN : (since == TSR_FLICKER_HALF_PERIOD_TICKS ? TSR_DEBUG_YELLOW : TSR_DEBUG_BLUE);
				color = mix(bg, hue, 0.2 + 0.8 * score);
			}
		}
	} else if (mode == TSR_DEBUG_VIEW_THIN_COVERAGE) {
		if ((stability.features & TSR_FEATURE_THIN_GEOMETRY) == 0u) {
			color = TSR_DEBUG_MAGENTA;
		} else {
			// Green by the relax weight; blue by the raw coverage where it is below the threshold.
			float coverage = clamp(textureLod(thin_tex, u, 0.0).r, 0.0, 1.0);
			float w = tsr_thin_weight(coverage);
			if (w > 0.0) {
				color = mix(bg, TSR_DEBUG_GREEN, w / TSR_THIN_MAX_RELAX);
			} else {
				color = mix(bg, TSR_DEBUG_BLUE, 0.75 * clamp(coverage / TSR_THIN_WEIGHT_LO, 0.0, 1.0));
			}
		}
	}

	imageStore(debug_out, opx, vec4(color, 1.0));
}
