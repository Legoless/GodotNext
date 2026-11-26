/**************************************************************************/
/*  tsr_stability.h                                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "servers/rendering/renderer_rd/effects/tsr_presets.h"

// GodotNext: pt_tsr_stability (#198). Pure helpers for the TSR stability set:
// which features a preset turns on, which passes they add, and the provisional
// tuning. Clean-room (docs/godotnext/tsr-sources.md): every count, interval and
// threshold here is GodotNext's own and provisional until the PQ sweep (§22).

enum TSRFeature : uint32_t {
	TSR_FEATURE_RESURRECTION = 1u << 0,
	TSR_FEATURE_REPROJECTION_FIELD = 1u << 1,
	TSR_FEATURE_FLICKER = 1u << 2,
	TSR_FEATURE_THIN_GEOMETRY = 1u << 3,
	TSR_FEATURE_ALL = 0xFu,
};

// GodotNext: pt_tsr_stability (#198, LQ11, plan v3.9.21/v3.9.23). Whether Nanite foliage is active
// in the current scene: NextGeo is active and at least one Nanite foliage feature (voxels,
// assemblies, foliage wind, or dynamic wind) is enabled.
inline constexpr bool nextgi_tsr_nanite_foliage_active(bool p_using_nextgeo, bool p_voxels_enabled, bool p_assemblies_enabled, bool p_foliage_wind_enabled, bool p_dynamic_wind_active) {
	return p_using_nextgeo && (p_voxels_enabled || p_assemblies_enabled || p_foliage_wind_enabled || p_dynamic_wind_active);
}

// GodotNext: pt_tsr_stability (#198, LQ11, plan v3.9.23). Thin-geometry detection is off by
// default (rendering/anti_aliasing/tsr/thin_geometry_detection) and turns on automatically when
// Nanite foliage is active, matching UE 5.9.
inline constexpr bool nextgi_tsr_thin_geometry_enabled(bool p_setting_enabled, bool p_nanite_foliage_active) {
	return p_setting_enabled || p_nanite_foliage_active;
}

// The stability features in effect: resurrection, reprojection field and flicker
// analysis follow TSR_PRESETS[q]; thin geometry follows its project setting
// (rendering/anti_aliasing/tsr/thin_geometry_detection) or auto-enables when
// Nanite foliage is active (LQ11, plan v3.9.23) at every preset.
inline constexpr uint32_t nextgi_tsr_feature_mask(RSE::ViewportTSRQuality p_quality, bool p_thin_geometry) {
	const TSRPreset &preset = TSR_PRESETS[CLAMP(int(p_quality), 0, int(RSE::VIEWPORT_TSR_QUALITY_MAX) - 1)];
	uint32_t features = 0;
	if (preset.resurrection) {
		features |= TSR_FEATURE_RESURRECTION;
	}
	if (preset.reprojection_field) {
		features |= TSR_FEATURE_REPROJECTION_FIELD;
	}
	if (preset.flicker_analysis) {
		features |= TSR_FEATURE_FLICKER;
	}
	if (p_thin_geometry) {
		features |= TSR_FEATURE_THIN_GEOMETRY;
	}
	return features;
}

// What a feature mask adds to the #194 passes. With no feature every field is
// false or 0: the #194 shader variants, textures and uniform sets are used as they are.
struct TSRPassPlan {
	bool stability_ubo = false; // The stability uniform buffer is created and bound.
	uint32_t rejection_variant = 0; // tsr_shading_rejection: 0 = #194, 1 = TSR_STABILITY.
	uint32_t history_variant = 0; // tsr_history_update: 0 = #194, 1 = TSR_STABILITY (added to the history format mode).
	bool analysis_flicker = false; // tsr_stability_analysis, MODE_FLICKER.
	bool analysis_thin = false; // tsr_stability_analysis, MODE_THIN.
	bool persistent_store = false; // tsr_persistent_store may run on capture frames.
};

inline constexpr TSRPassPlan nextgi_tsr_pass_plan(uint32_t p_features) {
	const uint32_t features = p_features & TSR_FEATURE_ALL;
	TSRPassPlan plan;
	plan.stability_ubo = features != 0;
	plan.history_variant = features != 0 ? 1 : 0;
	plan.rejection_variant = (features & (TSR_FEATURE_FLICKER | TSR_FEATURE_THIN_GEOMETRY)) != 0 ? 1 : 0;
	plan.analysis_flicker = (features & TSR_FEATURE_FLICKER) != 0;
	plan.analysis_thin = (features & TSR_FEATURE_THIN_GEOMETRY) != 0;
	plan.persistent_store = (features & TSR_FEATURE_RESURRECTION) != 0;
	return plan;
}

// Persistent frames for history resurrection: N frames at 1 / divisor of the
// output size per axis. Provisional: the PQ sweep (§22) sets the final values
// within T6, which the "[Upscalers][TSR] Stability layout" doctests bound.
struct TSRStabilityTuning {
	uint8_t persistent_frames;
	uint8_t persistent_divisor;
};

inline constexpr TSRStabilityTuning TSR_STABILITY_TUNING[5] = {
	{ 0, 1 }, // Low: no resurrection.
	{ 2, 3 }, // Medium: 2 frames at 1/3 of the output size.
	{ 2, 3 }, // High.
	{ 2, 2 }, // Epic: 2 frames at 1/2 of the output size (1/4 of the 200% history).
	{ 2, 2 }, // Cinematic.
};

static_assert(sizeof(TSR_STABILITY_TUNING) / sizeof(TSR_STABILITY_TUNING[0]) == RSE::VIEWPORT_TSR_QUALITY_MAX);

// Slots in the stability uniform buffer; TSR_STABILITY_TUNING never exceeds it.
inline constexpr uint32_t TSR_PERSISTENT_MAX_FRAMES = 4;
// K: a slot is overwritten every K frames, in turn. Above two history convergence
// times (2 x 16 frames) and sharing no factor with the jitter phase counts at the
// T-line scales (8, 18 and 32), so successive captures differ in jitter phase.
inline constexpr uint32_t TSR_PERSISTENT_INTERVAL_FRAMES = 35;
// Frames after a history reset before the first capture: #194 raises a pixel's
// confidence by 1/16 per frame, so the history has converged by then.
inline constexpr uint32_t TSR_PERSISTENT_WARMUP_FRAMES = 16;

// Confidence a resurrected sample starts with at a full-size persistent frame
// (the history meta value, 1/16 per accumulated frame): half converged.
inline constexpr float TSR_PERSISTENT_CONFIDENCE = 0.5f;

// The persistent slot to overwrite on this frame, or -1.
inline constexpr int32_t nextgi_tsr_persistent_capture_slot(uint32_t p_frames_since_reset, uint32_t p_count) {
	if (p_count == 0 || p_frames_since_reset < TSR_PERSISTENT_WARMUP_FRAMES) {
		return -1;
	}
	const uint32_t t = p_frames_since_reset - TSR_PERSISTENT_WARMUP_FRAMES;
	return (t % TSR_PERSISTENT_INTERVAL_FRAMES != 0) ? -1 : int32_t((t / TSR_PERSISTENT_INTERVAL_FRAMES) % p_count);
}

// T6 budget for all RB_SCOPE_TSR textures, in hundredths of a byte per history
// pixel. 13.95 leaves a guard under the 14 B line: the plan's megabyte figures
// are 14 B per history pixel rounded down.
inline constexpr uint32_t TSR_STABILITY_BUDGET_X100 = 1395;
// Fit scales are 16.16 fixed point; the stability stores never shrink below 0.25 per axis.
inline constexpr uint32_t TSR_FIT_FULL_Q16 = 65536;
inline constexpr uint32_t TSR_FIT_MIN_Q16 = 16384;

// Flicker analysis (High and above). One byte of state per texel, no luma ring:
// bits 7..4 hold the score (0..15, the relax weight; in the high bits so that a
// bilinear read of the byte stays close to score / 15), bit 3 the direction of the
// last luma change that counted, and bits 2..0 the ticks since that change,
// saturating at TSR_FLICKER_SINCE_MAX. TSR_FLICKER_SINCE_DISABLED marks a texel
// whose analysis was off this frame (reset, motion, disocclusion, translucency).
//
// A tick is 1/120 s. A reversal of the luma's direction at most
// TSR_FLICKER_HALF_PERIOD_TICKS (1/30 s) after the previous one is an oscillation
// of 15 Hz or more, the fastest one a 30 Hz stream can show; it raises the score.
// The threshold is a time, so the same band of real frequencies counts as flicker
// at 30, 60 and 120 Hz, and anything slower counts as animation.
inline constexpr uint32_t TSR_FLICKER_TICK_HZ = 120;
inline constexpr int32_t TSR_FLICKER_HALF_PERIOD_TICKS = 4;
inline constexpr int32_t TSR_FLICKER_SINCE_MAX = 6;
inline constexpr int32_t TSR_FLICKER_SINCE_DISABLED = 7;
inline constexpr uint8_t TSR_FLICKER_STATE_DISABLED = 0x07; // Score 0, "disabled". The stores are cleared to it.
inline constexpr int32_t TSR_FLICKER_SCORE_MAX = 15;
// A qualifying reversal adds 3, so 5 of them reach the full score: detection takes a
// few frames at any rate. A reversal outside the half period takes 2 away.
inline constexpr int32_t TSR_FLICKER_SCORE_RISE = 3;
inline constexpr int32_t TSR_FLICKER_SCORE_FALL = 2;
// Once a half period passes with no reversal, the score is released at 60 levels per
// second: a full score is gone 0.25 s after the half period, at any frame rate.
inline constexpr uint32_t TSR_FLICKER_DECAY_HZ = 60;
// A frame time above this (a hitch) counts as this much, and no frame yields more
// steps than TSR_FLICKER_MAX_STEPS. Below 30 Hz every frame is more than a half
// period, so no reversal qualifies and the analysis has no effect.
inline constexpr double TSR_FLICKER_MAX_FRAME_TIME = 0.1;
inline constexpr uint32_t TSR_FLICKER_MAX_STEPS = 6;
// The analysis is off for pixels moving faster than one internal pixel per full
// flicker period (2 half periods): 15 internal pixels per second.
inline constexpr double TSR_FLICKER_MAX_MOTION_PX_PER_S = double(TSR_FLICKER_TICK_HZ) / double(2 * TSR_FLICKER_HALF_PERIOD_TICKS);

// One step of a flicker state texel. p_event: the luma moved by more than the noise
// floor this frame; p_positive: its direction. p_ticks and p_decay_steps come from
// nextgi_tsr_flicker_ticks and nextgi_tsr_flicker_decay_steps for this frame.
// Mirrors tsr_flicker_step in tsr_stability_inc.glsl: the two must change together.
inline constexpr uint8_t nextgi_tsr_flicker_step(uint8_t p_state, bool p_positive, bool p_event, uint32_t p_ticks, uint32_t p_decay_steps) {
	int32_t score = int32_t(p_state >> 4);
	int32_t direction = int32_t((p_state >> 3) & 1u);
	int32_t since = int32_t(p_state & 7u);
	if (since == TSR_FLICKER_SINCE_DISABLED) {
		since = TSR_FLICKER_SINCE_MAX;
		score = 0;
	}
	since = MIN(since + int32_t(MIN(p_ticks, uint32_t(TSR_FLICKER_SINCE_MAX))), TSR_FLICKER_SINCE_MAX);
	const int32_t positive = p_positive ? 1 : 0;
	if (p_event && positive != direction) {
		// The luma reversed direction: flicker when the previous reversal is at most a half period old.
		score = since <= TSR_FLICKER_HALF_PERIOD_TICKS ? MIN(score + TSR_FLICKER_SCORE_RISE, TSR_FLICKER_SCORE_MAX) : MAX(score - TSR_FLICKER_SCORE_FALL, 0);
		direction = positive;
		since = 0;
	} else if (since > TSR_FLICKER_HALF_PERIOD_TICKS) {
		// A half period without a reversal: release.
		score = MAX(score - int32_t(MIN(p_decay_steps, uint32_t(TSR_FLICKER_SCORE_MAX))), 0);
	}
	return uint8_t((score << 4) | (direction << 3) | since);
}

// Turns a frame time into whole steps at p_rate_hz, carrying the remainder to the
// next frame (per TSR context), so the steps measure time, not frames: over one
// second of frames at any rate they sum to the rate.
inline uint32_t nextgi_tsr_flicker_steps(double &r_remainder, double p_delta, double p_rate_hz) {
	const double delta = p_delta > 0.0 ? MIN(p_delta, TSR_FLICKER_MAX_FRAME_TIME) : 0.0;
	r_remainder += delta * p_rate_hz;
	// The bias absorbs the rounding of a frame time stored as a float (1/60 s and the like).
	const uint32_t steps = uint32_t(r_remainder + 1e-6);
	r_remainder -= double(steps);
	return MIN(steps, TSR_FLICKER_MAX_STEPS);
}

// Flicker ticks (1/120 s) in this frame: 4, 2 and 1 at 30, 60 and 120 Hz.
inline uint32_t nextgi_tsr_flicker_ticks(double &r_remainder, double p_delta) {
	return nextgi_tsr_flicker_steps(r_remainder, p_delta, double(TSR_FLICKER_TICK_HZ));
}

// Score levels released in this frame once a half period has passed without a reversal.
inline uint32_t nextgi_tsr_flicker_decay_steps(double &r_remainder, double p_delta) {
	return nextgi_tsr_flicker_steps(r_remainder, p_delta, double(TSR_FLICKER_DECAY_HZ));
}

// The motion above which the analysis is off, in internal pixels per frame.
inline float nextgi_tsr_flicker_max_motion(double p_delta) {
	const double delta = p_delta > 0.0 ? MIN(p_delta, TSR_FLICKER_MAX_FRAME_TIME) : 0.0;
	return float(delta * TSR_FLICKER_MAX_MOTION_PX_PER_S);
}

// TSR pass visualizer: what Viewport.DEBUG_DRAW_TSR shows, chosen by the project
// setting rendering/anti_aliasing/tsr/debug_view (the debug draw itself is the
// on/off switch, so there is no "off" value). tsr_debug_view.glsl draws each one.
enum TSRDebugView : uint32_t {
	TSR_DEBUG_VIEW_HISTORY_VALIDITY = 0,
	TSR_DEBUG_VIEW_REJECTION = 1,
	TSR_DEBUG_VIEW_DISOCCLUSION = 2,
	TSR_DEBUG_VIEW_RESURRECTION_HITS = 3,
	TSR_DEBUG_VIEW_FLICKER_PERIOD = 4,
	TSR_DEBUG_VIEW_THIN_COVERAGE = 5,
	TSR_DEBUG_VIEW_MAX = 6,
};

// The view to draw for a value of the setting: out-of-range values are clamped.
inline constexpr TSRDebugView nextgi_tsr_debug_view_from_setting(int64_t p_value) {
	return TSRDebugView(CLAMP(p_value, int64_t(0), int64_t(TSR_DEBUG_VIEW_MAX) - 1));
}
