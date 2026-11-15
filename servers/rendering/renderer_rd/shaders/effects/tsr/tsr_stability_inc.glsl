// Clean-room TSR stability set (#198): declarations shared by the TSR_STABILITY shader
// variants and the stability passes (docs/godotnext/tsr-sources.md). Include it after
// tsr_inc.glsl, with TSR_STABILITY_UBO_BINDING defined as the binding of the uniform buffer.

// TSRFeature in tsr_stability.h.
#define TSR_FEATURE_RESURRECTION (1u << 0)
#define TSR_FEATURE_REPROJECTION_FIELD (1u << 1)
#define TSR_FEATURE_FLICKER (1u << 2)
#define TSR_FEATURE_THIN_GEOMETRY (1u << 3)

#define TSR_PERSISTENT_MAX_FRAMES 4

// A persistent frame is only tried for pixels whose history was rejected at least this much.
#define TSR_RESURRECT_MIN_REJECTION 0.5
// ...and never where the reactive (translucency) mask is above this, so translucency keeps its
// rejection, as in the flicker and thin-geometry analyses (provisional, GodotNext's own).
#define TSR_RESURRECT_MAX_REACTIVE 0.25

// TSR::StabilityUBO in tsr.h.
layout(set = 0, binding = TSR_STABILITY_UBO_BINDING, std140) uniform StabilityParams {
	highp mat4 persistent_reprojection[TSR_PERSISTENT_MAX_FRAMES]; // Current clip -> persistent frame clip.
	vec4 persistent_valid; // 1.0 when the slot holds a frame.
	uint features; // TSR_FEATURE_* in effect.
	uint persistent_count;
	uint persistent_layer_base; // First layer of this view in the persistent frames.
	uint debug_view;
	ivec2 flicker_size;
	ivec2 thin_size;
	uint flicker_ticks;
	uint flicker_decay_steps;
	float flicker_max_motion;
	float persistent_confidence; // Confidence given to a resurrected sample.
}
stability;

// How far a perceptual YCoCg sample lies outside a neighbourhood described by its mean
// and standard deviation: the shading-rejection measure of tsr_shading_rejection.glsl
// applied to one sample. 0 inside mu +- 1.25 sigma, 1 at one rejection scale beyond it.
float tsr_box_error(vec3 s, vec3 mu, vec3 sigma) {
	vec3 excess = max(abs(s - mu) - 1.25 * sigma, vec3(0.0));
	vec3 norm_excess = excess / max(sigma * 2.25, vec3(0.02, 0.015, 0.015));
	return clamp(max(norm_excess.x, 0.85 * length(norm_excess.yz)), 0.0, 1.0);
}

// History resurrection: the best match among the valid persistent frames for the pixel at
// unjittered UV u with raw depth raw_d, against the current neighbourhood (mean mu and standard
// deviation sigma, perceptual YCoCg). The pixel is reprojected with its depth and the cameras
// only. Returns the smallest tsr_box_error (1.0 when no frame matches), with that frame's sample
// in r_ycocg and the number of valid frames that cover the pixel in r_covered. Shared by the
// history update and the pass visualizer, so the visualizer shows the test the history uses.
float tsr_resurrection_match(sampler2DArray persistent_frames_tex, vec2 u, float raw_d, vec3 mu, vec3 sigma, out vec3 r_ycocg, out uint r_covered) {
	float best = 1.0;
	r_ycocg = vec3(0.0);
	r_covered = 0u;
	for (uint n = 0u; n < stability.persistent_count; n++) {
		if (stability.persistent_valid[n] < 0.5) {
			continue;
		}
		vec4 clip = stability.persistent_reprojection[n] * vec4(u * 2.0 - 1.0, raw_d * 2.0 - 1.0, 1.0);
		if (clip.w <= 1e-5) {
			continue;
		}
		vec2 uv_n = clip.xy / clip.w * 0.5 + 0.5;
		if (any(lessThan(uv_n, vec2(0.0))) || any(greaterThan(uv_n, vec2(1.0)))) {
			continue;
		}
		r_covered++;
		vec3 s = rgb_to_perceptual_ycocg(sanitize_hdr(textureLod(persistent_frames_tex, vec3(uv_n, float(stability.persistent_layer_base + n)), 0.0).rgb));
		float err = tsr_box_error(s, mu, sigma);
		if (err < best) {
			best = err;
			r_ycocg = s;
		}
	}
	return best;
}

// Flicker analysis. The state byte and the step are described in tsr_stability.h; these
// values mirror TSR_FLICKER_* there.
#define TSR_FLICKER_HALF_PERIOD_TICKS 4
#define TSR_FLICKER_SINCE_MAX 6
#define TSR_FLICKER_SINCE_DISABLED 7
#define TSR_FLICKER_STATE_DISABLED 7u
#define TSR_FLICKER_SCORE_MAX 15
#define TSR_FLICKER_SCORE_RISE 3
#define TSR_FLICKER_SCORE_FALL 2

// Provisional, GodotNext's own (docs/godotnext/tsr-sources.md):
// - a luma change under 1/64 of the perceptual luma range is noise, not a change of direction;
// - the analysis is off where the parallax disocclusion or the reactive (translucency) mask
//   is above these, as it is for moving pixels (stability.flicker_max_motion);
// - at most this share of a rejection is removed, so an opaque panning texture read as flicker
//   (there is no per-material flag) ghosts only boundedly;
// - the history clip box widens toward this many standard deviations at full relaxation.
#define TSR_FLICKER_MIN_DELTA (1.0 / 64.0)
#define TSR_FLICKER_MAX_DISOCCLUSION 0.5
#define TSR_FLICKER_MAX_REACTIVE 0.25
#define TSR_FLICKER_MAX_RELAX 0.75
#define TSR_RELAX_GAMMA 3.0

// One step of a flicker state texel. Mirrors nextgi_tsr_flicker_step in tsr_stability.h:
// the two must change together. Signed arithmetic, so the releases cannot wrap.
uint tsr_flicker_step(uint state, bool positive, bool changed, uint ticks, uint decay_steps) {
	int score = int(state >> 4u);
	int direction = int((state >> 3u) & 1u);
	int since = int(state & 7u);
	if (since == TSR_FLICKER_SINCE_DISABLED) {
		since = TSR_FLICKER_SINCE_MAX;
		score = 0;
	}
	since = min(since + int(min(ticks, uint(TSR_FLICKER_SINCE_MAX))), TSR_FLICKER_SINCE_MAX);
	int pos = positive ? 1 : 0;
	if (changed && pos != direction) {
		// The luma reversed direction: flicker when the previous reversal is at most a half period old.
		score = since <= TSR_FLICKER_HALF_PERIOD_TICKS ? min(score + TSR_FLICKER_SCORE_RISE, TSR_FLICKER_SCORE_MAX) : max(score - TSR_FLICKER_SCORE_FALL, 0);
		direction = pos;
		since = 0;
	} else if (since > TSR_FLICKER_HALF_PERIOD_TICKS) {
		// A half period without a reversal: release.
		score = max(score - int(min(decay_steps, uint(TSR_FLICKER_SCORE_MAX))), 0);
	}
	return uint((score << 4) | (direction << 3) | since);
}

// Relax weight of a flicker state read through a sampler (the byte / 255, possibly filtered
// between texels): about score / 15, and 0 for a disabled texel.
float tsr_flicker_weight(float r) {
	return clamp((r * 255.0 - 15.0) / 225.0, 0.0, 1.0);
}

// Reprojection field: each component of the velocity gradient is clamped to this many
// internal pixels of velocity per internal pixel (provisional, GodotNext's own).
#define TSR_FIELD_MAX_GRADIENT 0.5

// The one-sided difference with the smaller length, so a jump on one side (a silhouette)
// does not enter the gradient; each component clamped to +-max_gradient.
vec2 tsr_one_sided(vec2 c, vec2 lo, vec2 hi, float max_gradient) {
	vec2 fwd = hi - c;
	vec2 bwd = c - lo;
	vec2 d = dot(fwd, fwd) < dot(bwd, bwd) ? fwd : bwd;
	return clamp(d, vec2(-max_gradient), vec2(max_gradient));
}

// Mean of 4 bilinear taps spread over the parallelogram footprint centered on uv whose edges
// are axis_x and axis_y, in texels of tex: a box filter for a minified history fetch.
vec3 tsr_footprint_fetch(sampler2D tex, vec2 uv, vec2 axis_x, vec2 axis_y, vec2 tex_size) {
	vec2 ox = 0.25 * axis_x / tex_size;
	vec2 oy = 0.25 * axis_y / tex_size;
	vec3 sum = sanitize_hdr(textureLod(tex, uv - ox - oy, 0.0).rgb);
	sum += sanitize_hdr(textureLod(tex, uv + ox - oy, 0.0).rgb);
	sum += sanitize_hdr(textureLod(tex, uv - ox + oy, 0.0).rgb);
	sum += sanitize_hdr(textureLod(tex, uv + ox + oy, 0.0).rgb);
	return sum * 0.25;
}

// Thin geometry (opt-in, rendering/anti_aliasing/tsr/thin_geometry_detection). One byte per texel
// at half the internal size: the share of recent frames in which the texel's pixels held a thin
// feature. Provisional, GodotNext's own (docs/godotnext/tsr-sources.md):
// - a pixel is on a depth ridge when, along an axis through it, both opposite neighbors are
//   farther than it by more than this share of its own view depth (a feature one pixel wide);
// - it is on a luma line when both opposite neighbors differ from it in perceptual luma, with the
//   same sign, by more than this much;
// - the coverage moves toward this frame's detection by this much per frame: 8 is the smallest
//   jitter phase count, so the average spans one jitter cycle;
// - the relax weight rises from one detection in four cycles to one detection per cycle, and
//   removes at most this share of the rejection (shading and disocclusion), as for flicker;
// - the analysis is off where the reactive (translucency) mask is above this, so translucency keeps
//   its rejection.
#define TSR_THIN_DEPTH_STEP 0.05
#define TSR_THIN_LUMA_STEP 0.08
#define TSR_THIN_COVERAGE_RATE (1.0 / 8.0)
#define TSR_THIN_WEIGHT_LO (1.0 / 32.0)
#define TSR_THIN_WEIGHT_HI (1.0 / 8.0)
#define TSR_THIN_MAX_RELAX 0.75
#define TSR_THIN_MAX_REACTIVE 0.25

// Relax weight of a thin-coverage value: 0 below TSR_THIN_WEIGHT_LO, TSR_THIN_MAX_RELAX from
// TSR_THIN_WEIGHT_HI up.
float tsr_thin_weight(float coverage) {
	return smoothstep(TSR_THIN_WEIGHT_LO, TSR_THIN_WEIGHT_HI, coverage) * TSR_THIN_MAX_RELAX;
}
