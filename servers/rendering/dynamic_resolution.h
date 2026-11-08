/**************************************************************************/
/*  dynamic_resolution.h                                                  */
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

#include "core/math/math_funcs.h"
#include "core/math/vector2.h"

#include <algorithm>
#include <cmath>

// GodotNext: pt_dynamic_resolution (#191).
// Opt-in GPU-time dynamic resolution controller with 16 evenly spaced scale
// buckets over the intersection of [min_scale, max_scale] and the active
// upscaler's supported scale range, a fixed 30-sample GPU frame-time ring,
// and at most one bucket change per 30 frames.
class DynamicResolutionController {
public:
	static constexpr int BUCKET_COUNT = 16;
	static constexpr int RING_SIZE = 30;
	static constexpr int MIN_FRAMES_BETWEEN_CHANGES = 30;

private:
	float buckets[BUCKET_COUNT] = {};
	Vector2 effective_bounds = Vector2(0.5f, 1.0f);
	double gpu_times_ms[RING_SIZE] = {};
	int write_cursor = 0;
	int sample_count = 0;
	int current_bucket = BUCKET_COUNT - 1;
	int frames_since_change = MIN_FRAMES_BETWEEN_CHANGES;
	bool configured = false;

public:
	static Vector2 intersect_scale_bounds(float p_min_scale, float p_max_scale, const Vector2 &p_upscaler_range) {
		const float user_min = MIN(p_min_scale, p_max_scale);
		const float user_max = MAX(p_min_scale, p_max_scale);
		const float range_min = MIN(p_upscaler_range.x, p_upscaler_range.y);
		const float range_max = MAX(p_upscaler_range.x, p_upscaler_range.y);

		float eff_min = CLAMP(user_min, range_min, range_max);
		float eff_max = CLAMP(user_max, range_min, range_max);
		if (eff_min > eff_max) {
			eff_min = eff_max;
		}
		return Vector2(eff_min, eff_max);
	}

	static void build_buckets(float p_min_scale, float p_max_scale, const Vector2 &p_upscaler_range, float r_buckets[BUCKET_COUNT]) {
		const Vector2 bounds = intersect_scale_bounds(p_min_scale, p_max_scale, p_upscaler_range);
		r_buckets[0] = bounds.x;
		for (int i = 1; i < BUCKET_COUNT - 1; i++) {
			r_buckets[i] = bounds.x + (bounds.y - bounds.x) * (float(i) / float(BUCKET_COUNT - 1));
		}
		r_buckets[BUCKET_COUNT - 1] = bounds.y;
	}

	static int nearest_bucket_for_scale(const float p_buckets[BUCKET_COUNT], float p_scale) {
		int best = BUCKET_COUNT - 1;
		float best_dist = Math::abs(p_buckets[best] - p_scale);
		for (int i = BUCKET_COUNT - 2; i >= 0; i--) {
			const float dist = Math::abs(p_buckets[i] - p_scale);
			if (dist < best_dist) {
				best_dist = dist;
				best = i;
			}
		}
		return best;
	}

	static int nearest_bucket_rounded_down(const float p_buckets[BUCKET_COUNT], float p_desired_scale, int p_max_bucket) {
		const int hi = CLAMP(p_max_bucket, 0, BUCKET_COUNT - 1);
		int chosen = 0;
		for (int i = 0; i <= hi; i++) {
			if (p_buckets[i] <= p_desired_scale + 1e-6f) {
				chosen = i;
			} else {
				break;
			}
		}
		return chosen;
	}

	static double compute_p95(const double *p_samples, int p_count) {
		if (p_count <= 0) {
			return 0.0;
		}
		double sorted[RING_SIZE];
		const int n = MIN(p_count, RING_SIZE);
		for (int i = 0; i < n; i++) {
			sorted[i] = p_samples[i];
		}
		std::sort(sorted, sorted + n);
		const int idx = CLAMP(int(std::ceil(0.95 * double(n))) - 1, 0, n - 1);
		return sorted[idx];
	}

	void reset() {
		for (int i = 0; i < RING_SIZE; i++) {
			gpu_times_ms[i] = 0.0;
		}
		write_cursor = 0;
		sample_count = 0;
		current_bucket = BUCKET_COUNT - 1;
		frames_since_change = MIN_FRAMES_BETWEEN_CHANGES;
		configured = false;
	}

	// Updates the 16-bucket table for (min_scale, max_scale, upscaler_range).
	// Returns true when the effective scale changed because the bounds shifted
	// or the controller was initialized.
	bool configure_bounds(float p_min_scale, float p_max_scale, const Vector2 &p_upscaler_range, float p_initial_scale = -1.0f) {
		const Vector2 new_bounds = intersect_scale_bounds(p_min_scale, p_max_scale, p_upscaler_range);
		if (configured && Math::is_equal_approx(new_bounds.x, effective_bounds.x) && Math::is_equal_approx(new_bounds.y, effective_bounds.y)) {
			return false;
		}

		const float prev_scale = configured ? buckets[current_bucket] : ((p_initial_scale > 0.0f) ? p_initial_scale : new_bounds.y);
		effective_bounds = new_bounds;
		build_buckets(p_min_scale, p_max_scale, p_upscaler_range, buckets);
		current_bucket = nearest_bucket_for_scale(buckets, prev_scale);
		const bool was_configured = configured;
		configured = true;
		return !was_configured || !Math::is_equal_approx(buckets[current_bucket], prev_scale);
	}

	// Records one completed GPU frame time (in milliseconds) and advances the
	// controller by one rendered frame. Returns true iff the active bucket changed.
	bool push_gpu_frame_time_ms(double p_gpu_time_ms, int p_target_frame_rate) {
		if (!configured || p_target_frame_rate <= 0 || !std::isfinite(p_gpu_time_ms) || p_gpu_time_ms <= 0.0) {
			return false;
		}

		gpu_times_ms[write_cursor] = p_gpu_time_ms;
		write_cursor = (write_cursor + 1) % RING_SIZE;
		if (sample_count < RING_SIZE) {
			sample_count++;
		}
		if (frames_since_change < MIN_FRAMES_BETWEEN_CHANGES) {
			frames_since_change++;
		}

		if (sample_count < RING_SIZE || frames_since_change < MIN_FRAMES_BETWEEN_CHANGES) {
			return false;
		}

		const double target_ms = 1000.0 / double(p_target_frame_rate);
		const double p95_ms = compute_p95(gpu_times_ms, sample_count);
		int new_bucket = current_bucket;

		if (p95_ms > target_ms && current_bucket > 0) {
			const float current_scale = buckets[current_bucket];
			const float desired_scale = float(double(current_scale) * std::sqrt(0.95 * target_ms / p95_ms));
			new_bucket = nearest_bucket_rounded_down(buckets, desired_scale, current_bucket - 1);
		} else if (current_bucket < BUCKET_COUNT - 1) {
			const double up_threshold_ms = 0.85 * target_ms;
			bool all_below = true;
			for (int i = 0; i < RING_SIZE; i++) {
				if (gpu_times_ms[i] >= up_threshold_ms) {
					all_below = false;
					break;
				}
			}
			if (all_below) {
				new_bucket = MIN(current_bucket + 1, BUCKET_COUNT - 1);
			}
		}

		if (new_bucket != current_bucket && !Math::is_equal_approx(buckets[new_bucket], buckets[current_bucket])) {
			current_bucket = new_bucket;
			frames_since_change = 0;
			sample_count = 0;
			write_cursor = 0;
			return true;
		}

		return false;
	}

	bool is_configured() const { return configured; }
	int get_current_bucket() const { return current_bucket; }
	float get_current_scale() const { return buckets[current_bucket]; }
	float get_bucket_scale(int p_index) const { return buckets[CLAMP(p_index, 0, BUCKET_COUNT - 1)]; }
	Vector2 get_effective_bounds() const { return effective_bounds; }
	int get_sample_count() const { return sample_count; }
	int get_frames_since_change() const { return frames_since_change; }
};
