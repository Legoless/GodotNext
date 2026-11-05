/**************************************************************************/
/*  next_gi_gather.h                                                      */
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
#include "core/math/projection.h"
#include "core/string/ustring.h"
#include "core/templates/rid.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/renderer_rd/effects/denoise.h"

class RenderDataRD;
class RenderSceneBuffersRD;

namespace RendererRD {

// 1/16 ray per pixel: one probe per 16x16 tile, 16 rays each (#25).
_FORCE_INLINE_ uint32_t nextgi_gather_probe_count_axis(uint32_t p_pixels) {
	return (p_pixels + 15u) / 16u;
}

_FORCE_INLINE_ uint32_t nextgi_gather_ray_count(uint32_t p_width, uint32_t p_height) {
	return nextgi_gather_probe_count_axis(p_width) * nextgi_gather_probe_count_axis(p_height) * 16u;
}

// History resets when the reprojected depth disagrees by more than 10%.
_FORCE_INLINE_ bool nextgi_gather_history_resets(float p_depth, float p_history_depth) {
	if (p_history_depth <= 0.0f) {
		return true;
	}
	return Math::abs(p_depth - p_history_depth) > 0.1f * p_history_depth;
}

// Screen and SRC hits are fully confident. A world-cache hit stays below 1. Sky is 0.
_FORCE_INLINE_ float nextgi_sample_confidence(bool p_screen_hit, bool p_src_hit, bool p_wrc_hit, float p_wrc_weight) {
	if (p_screen_hit || p_src_hit) {
		return 1.0f;
	}
	if (p_wrc_hit) {
		return MIN(MAX(p_wrc_weight, 0.0f), 0.99f);
	}
	return 0.0f;
}

uint64_t nextgi_gather_coverage_hits();
uint64_t nextgi_gather_coverage_rays();

class NextGIGather {
	static NextGIGather *singleton;

	struct ShaderState;
	ShaderState *shader = nullptr;

	RID probe_image;
	RID probe_scratch;
	RID probe_history;
	RID history_length;
	// GodotNext: nextgi_denoise_debug — per-view debug copies for the denoise views (#53).
	RID debug_raw;
	RID debug_variance;
	RID debug_history;
	RID debug_rejected;
	RID reset_mask_buffer;
	RID ambient_scratch;
	RID prev_ambient;
	uint32_t ambient_w = 0;
	uint32_t ambient_h = 0;
	bool ambient_history_valid = false;
	RID last_depth;
	RID last_normal_roughness;
	RID last_velocity;
	Projection last_inv_projection;
	Projection last_prev_view_proj;
	Vector3 last_camera_pos;
	Vector3 last_prev_camera_pos;
	bool history_valid = false;
	bool rr_stabilizer_enabled = true;
	uint32_t rr_probe_history_cap = 16;
	NextGIDenoise *denoise = nullptr;
	RID uniform_set;
	RID reflect_uniform_set;
	RID reflect_counter_buffer;
	RID params_buffer;
	RID counter_buffer;
	RID feedback_buffer;
	RID confidence_buffer;
	RID dummy_color;
	RID dummy_storage;
	RID dummy_buffer;
	RID sampler;
	RID sampler_nearest;
	uint32_t probe_w = 0;
	uint32_t probe_h = 0;
	uint32_t frame = 0;
	uint32_t last_rays = 0;
	uint32_t last_misses = 0;
	uint32_t last_resets = 0;
	uint32_t last_uncovered = 0;
	uint32_t last_coverage_hits = 0;
	uint32_t last_coverage_rays = 0;
	float last_miss_luminance = 0.0f;
	uint32_t last_wrc_samples = 0;
	uint32_t last_sky_samples = 0;
	uint32_t last_screen_hits = 0;
	uint32_t last_screen_w = 0;
	uint32_t last_screen_h = 0;
	uint32_t last_reflection_traced = 0;
	uint32_t last_reflection_hits = 0;
	uint32_t last_reflection_misses = 0;
	bool hit_radiance_reference = false;

	bool _ensure_shader();
	bool _ensure_targets(uint32_t p_screen_w, uint32_t p_screen_h);
	bool _bind(RenderDataRD *p_render_data, uint32_t p_feedback_count, uint32_t p_feedback_stride);
	void _denoise_probes();
	bool _ensure_debug_textures();
	void _stabilize_ambient(Ref<RenderSceneBuffersRD> p_render_buffers);
	void _read_counters();
	void _free_uniform_set();

public:
	static NextGIGather *get_singleton() { return singleton; }

	void init();
	void free_resources();
	void gather(RenderDataRD *p_render_data);
	void integrate(RenderDataRD *p_render_data);
	void fill_info(Dictionary &r_info) const;
	// #53 debug views: 10 raw, 11 variance, 12 history, 13 rejected (NextGI::DebugViewId order).
	RID get_debug_texture(int p_view) const;

	// Expanded gather source (variant 0) for the kernel-rule doctest. No GPU compile.
	static String build_shader_source(int p_variant = 0);

	NextGIGather();
	~NextGIGather();
};

} // namespace RendererRD
