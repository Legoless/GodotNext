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

#include "core/math/basis.h"
#include "core/math/math_funcs.h"
#include "core/math/projection.h"
#include "core/math/vector2i.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/renderer_rd/effects/denoise.h"
#include "servers/rendering/rendering_device.h"

class RenderDataRD;
class RenderSceneBuffersRD;
class RenderSceneDataRD;

namespace RendererRD {

// 1/16 ray per pixel: one probe per 16x16 tile, 16 rays each (#25).
_FORCE_INLINE_ uint32_t nextgi_gather_probe_count_axis(uint32_t p_pixels) {
	return (p_pixels + 15u) / 16u;
}

_FORCE_INLINE_ uint32_t nextgi_gather_ray_count(uint32_t p_width, uint32_t p_height) {
	return nextgi_gather_probe_count_axis(p_width) * nextgi_gather_probe_count_axis(p_height) * 16u;
}

// GodotNext: nextgi_reflect_divisor (#274). The specular trace has one pixel per
// reflection_divisor x reflection_divisor block of the internal size, rounded down and at least
// 1x1: Epic (1) traces every pixel, High (2) halves each axis, Medium (4) quarters it. The trace
// textures, RB_TEX_SPECULAR_HIT, the reflect dispatch and the specular denoiser all use this
// size; the reflect kernel reads it back from its output image.
_FORCE_INLINE_ uint32_t nextgi_reflect_trace_axis(uint32_t p_pixels, uint32_t p_divisor) {
	return MAX(p_pixels / MAX(p_divisor, 1u), 1u);
}

_FORCE_INLINE_ Size2i nextgi_reflect_trace_size(const Size2i &p_internal_size, uint32_t p_divisor) {
	return Size2i(int32_t(nextgi_reflect_trace_axis(uint32_t(MAX(p_internal_size.x, 0)), p_divisor)), int32_t(nextgi_reflect_trace_axis(uint32_t(MAX(p_internal_size.y, 0)), p_divisor)));
}

// GodotNext: nextgi_front_layer_hit (#276). The hit distance texture a gather's specular trace
// writes (binding 35) and its specular denoiser reads. The view's gather uses the view's
// RB_TEX_SPECULAR_HIT; the front layer's gather uses its own target and never the view's, which
// keeps the view's trace for the readers after the frame block (the RR hit distance guide).
_FORCE_INLINE_ RID nextgi_specular_hit_target(bool p_front_layer, const RID &p_front_layer_target, const RID &p_view_target) {
	return p_front_layer ? p_front_layer_target : p_view_target;
}

// 8x8 workgroups, one thread per trace pixel.
_FORCE_INLINE_ uint32_t nextgi_reflect_dispatch_groups(uint32_t p_trace_pixels) {
	return MAX((p_trace_pixels + 7u) / 8u, 1u);
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
	// GodotNext: ps_swrt_trace (#186). The software tier's sibling (nextgi_gather_swrt.glsl): no
	// ray query, so SWRT never constructs the hardware ShaderRD. Set 1 holds the SDF trace.
	struct SWRTShaderState;
	SWRTShaderState *swrt_shader = nullptr;
	RID swrt_trace_set;
	bool bound_swrt = false;
	String bound_tier = "NONE";

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
	bool rr_specular_filter_enabled = true;
	uint32_t rr_sampling_mode = 0;
	NextGIDenoise *denoise = nullptr;
	RID uniform_set;
	RID gather_hl_uniform_set;
	// GodotNext: nextgi_world_cache (#294). World Cache gather & reflect uniform sets (variants 8 & 9).
	RID gather_wc_uniform_set;
	RID reflect_wc_uniform_set;
	bool bound_world_cache = false;
	LocalVector<RenderingDevice::Uniform> last_gather_uniforms;
	RID reflect_uniform_set;
	RID reflect_far_uniform_set;
	RID reflect_counter_buffer;
	RID specular_debug;
	RID clamp_totals_buffer;
	RID blue_noise_texture;
	RID specular_raw;
	RID specular_history;
	RID specular_scratch;
	RID specular_denoised;
	RID specular_history_length;
	RID specular_debug_variance;
	RID specular_debug_rejected;
	RID specular_final;
	RID last_reflection_output;
	RID last_specular_hit;
	Projection last_inv_view_proj;
	// GodotNext: nextgi_hit_lighting (#129). View inputs captured in _bind() for hit-lighting reflections.
	Basis last_view_to_world;
	RID last_hiz_texture;
	RID last_frame_color_texture;
	RID last_sky_texture;
	uint32_t last_hiz_max_mip = 0;
	float last_rt_radius = 200.0f;
	float last_max_origin_offset = 0.01f;
	float last_max_trace_roughness = 0.4f;
	bool last_sky_enabled = false;
	bool last_reflect_enabled = true;
	bool last_reflect_debug = false;
	Color last_ambient_color;
	bool last_fog_enabled = false;
	bool last_fog_depth_mode = false;
	float last_fog_density = 0.0f;
	float last_fog_height = 0.0f;
	float last_fog_height_density = 0.0f;
	float last_fog_depth_begin = 0.0f;
	float last_fog_depth_end = 1.0f;
	float last_fog_depth_curve = 1.0f;
	Color last_fog_light_color;
	float last_fog_light_energy = 1.0f;
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
	uint32_t diffuse_dispatches = 0;
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
	Size2i last_trace_size; // GodotNext: nextgi_reflect_divisor (#274)
	uint32_t target_stride = 0;
	uint32_t ray_stride = 4;
	uint32_t ray_count = 16;
	uint32_t last_clamp_removed = 0;
	uint32_t last_clamp_total = 0;
	uint32_t last_reflection_traced = 0;
	uint32_t last_reflection_hits = 0;
	uint32_t last_reflection_misses = 0;
	// GodotNext: ps_swrt_trace (#186). Software-tier trace counters (0 on the hardware tier).
	uint32_t last_sdf_hits = 0;
	uint32_t last_sdf_exhausted = 0;
	uint32_t last_sdf_near = 0;
	uint32_t last_sdf_detail_hits = 0;
	uint32_t last_sdf_d6_resolved = 0;
	uint32_t last_sdf_hit_cm_sum = 0;
	// GodotNext: nextgi_card_normal (#251)
	uint32_t last_feedback_rays = 0;
	uint32_t last_feedback_escapes = 0;
	uint32_t last_reflect_sdf_hits = 0;
	uint32_t last_reflect_sdf_exhausted = 0;
	uint32_t last_reflect_sdf_near = 0;
	// GodotNext: nextgi_async_counters (#186). One asynchronous readback of the counter, clamp
	// and reflect buffers; a synchronous read inside the frame block restarts the device frame
	// and drops the block's timestamps.
	RID readback_buffer;
	uint64_t counter_token = 0;
	bool counters_pending = false;
	bool hit_radiance_reference = false;
	bool front_layer = false;
	RID front_depth;
	RID front_normal_roughness;
	RID front_reflection;
	// GodotNext: nextgi_front_layer_hit (#276). The front layer's own hit distances (R16F, trace
	// size), which this gather allocates; the view's gather writes RB_TEX_SPECULAR_HIT instead.
	RID front_hit_distance;

	bool _ensure_shader();
	bool _ensure_swrt_shader();
	// The bound tier's pipeline for a variant (0 gather, 1 integrate, 2 feedback, 3 coverage,
	// 4 hit-radiance reference, 5 reflect, 6 far field).
	RID _pipeline(int p_variant) const;
	void _bind_trace_set(int64_t p_list) const;
	bool _ensure_targets(uint32_t p_screen_w, uint32_t p_screen_h);
	bool _bind(RenderDataRD *p_render_data, uint32_t p_feedback_count, uint32_t p_feedback_stride);
	void _denoise_probes();
	void _denoise_specular();
	bool _ensure_debug_textures();
	void _stabilize_ambient(Ref<RenderSceneBuffersRD> p_render_buffers);
	void _request_counters();
	static void _counters_received(const Vector<uint8_t> &p_data, uint64_t p_token);
	void _free_uniform_set();

public:
	static NextGIGather *get_singleton() { return singleton; }

	void init();
	void free_resources();
	void gather(RenderDataRD *p_render_data, bool p_diffuse = true);
	void set_front_layer_inputs(RID p_depth, RID p_normal_roughness, RID p_reflection);
	// GodotNext: nextgi_front_layer_hit (#276). The size of the front layer's own hit distance
	// target as allocated (0x0 for the view's gather and before the first front-layer trace).
	Size2i get_front_hit_distance_size() const;
	uint32_t get_shader_version_count() const;
	RID get_specular_history() const { return specular_history; }
	void integrate(RenderDataRD *p_render_data);
	void fill_info(Dictionary &r_info) const;
	// #53 debug views: 10 raw, 11 variance, 12 history, 13 rejected (NextGI::DebugViewId order).
	RID get_debug_texture(int p_view) const;
	// Log-scaled specular hit distance for DEBUG_VIEW_SPECULAR_HIT (#60).
	RID get_specular_debug_texture() const;
	RID get_specular_filter_debug_texture(int p_view) const;

	// Expanded gather source (variant 0) for the kernel-rule doctest. No GPU compile.
	static String build_shader_source(int p_variant = 0);
	// GodotNext: ps_swrt_trace (#186). The software sibling's expanded source, for the doctests.
	static String build_swrt_shader_source(int p_variant = 0);
	// GodotNext: nextgi_vulkan (#50). The variant defines the runtime ShaderRD is initialized with, shared
	// with the shader baker's bake-only instance (NextGIBakeShaders) so both resolve to the same cache path.
	static Vector<String> get_shader_variant_defines();
	// GodotNext: nextgi_bake_swrt (#261). Initializes the software sibling with its variants.
	static void initialize_swrt_shader(ShaderRD &r_shader);

	// GodotNext: nextgi_view_math. The view matrices the gather uploads, all from NextGIView:
	// the inverse of the depth prepass's view-projection, the previous frame's view-projection,
	// the depth-to-view inverse projection, and the camera basis that turns the prepass's
	// view-space normals into world space (nextgi_view_world_normal()).
	struct ViewMatrices {
		Projection inv_view_proj;
		Projection prev_view_proj;
		Projection inv_projection;
		Basis view_to_world;
	};
	static ViewMatrices view_matrices(const RenderSceneDataRD &p_scene_data);

	NextGIGather();
	~NextGIGather();
};

} // namespace RendererRD
