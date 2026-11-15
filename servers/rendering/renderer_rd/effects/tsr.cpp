/**************************************************************************/
/*  tsr.cpp                                                               */
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

#include "tsr.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

SafeNumeric<uint32_t> TSRContext::live_context_count{ 0 };

TSRContext::TSRContext(const Size2i &p_internal_size, const Size2i &p_output_size, RSE::ViewportTSRQuality p_quality) :
		internal_size(p_internal_size),
		output_size(p_output_size),
		history_size(TSR::compute_history_size(p_output_size, p_quality)),
		quality(p_quality) {
	reset_latch.request();
	live_context_count.increment();
}

TSRContext::~TSRContext() {
	// Doctests build contexts without a device; the buffer is null there.
	if (stability_ubo.is_valid() && RD::get_singleton() != nullptr) {
		RD::get_singleton()->free_rid(stability_ubo);
	}
	live_context_count.decrement();
}

void TSRContext::invalidate_persistent() {
	for (PersistentFrame &frame : persistent) {
		frame.valid = false;
	}
	frames_since_reset = 0;
}

const TSRContext::PersistentFrame &TSRContext::get_persistent_frame(uint32_t p_slot) const {
	static const PersistentFrame invalid_frame;
	ERR_FAIL_UNSIGNED_INDEX_V(p_slot, TSR_PERSISTENT_MAX_FRAMES, invalid_frame);
	return persistent[p_slot];
}

void TSRContext::set_persistent_frame(uint32_t p_slot, const Projection &p_projection, const Transform3D &p_camera_transform) {
	ERR_FAIL_UNSIGNED_INDEX(p_slot, TSR_PERSISTENT_MAX_FRAMES);
	persistent[p_slot].valid = true;
	persistent[p_slot].projection = p_projection;
	persistent[p_slot].camera_transform = p_camera_transform;
}

void TSRContext::sync_persistent_storage(RID p_texture, uint64_t p_signature) {
	if (p_texture != persistent_texture || p_signature != persistent_signature) {
		persistent_texture = p_texture;
		persistent_signature = p_signature;
		invalidate_persistent();
	}
}

bool TSRContext::sync_dimensions(const Size2i &p_internal_size, const Size2i &p_output_size, RSE::ViewportTSRQuality p_quality) {
	const Size2i new_history_size = TSR::compute_history_size(p_output_size, p_quality);
	const bool history_resized = (output_size != p_output_size) || (history_size != new_history_size);
	internal_size = p_internal_size;
	output_size = p_output_size;
	history_size = new_history_size;
	quality = p_quality;
	if (history_resized) {
		parity = 0;
		reset_latch.request();
		invalidate_persistent();
	}
	return history_resized;
}

Size2i TSR::compute_history_size(const Size2i &p_output_size, RSE::ViewportTSRQuality p_quality) {
	const int preset_idx = CLAMP(int(p_quality), 0, int(RSE::VIEWPORT_TSR_QUALITY_MAX) - 1);
	const float scale = TSR_PRESETS[preset_idx].history_scale;
	const int width = CLAMP(int(Math::floor(float(p_output_size.width) * scale + 0.5f)), 1, 16384);
	const int height = CLAMP(int(Math::floor(float(p_output_size.height) * scale + 0.5f)), 1, 16384);
	return Size2i(width, height);
}

uint32_t TSR::history_bytes_per_pixel() {
	uint32_t total = 0;
	for (const TSRTextureFormatEntry &entry : TSR_FORMAT_TABLE) {
		if (entry.domain == TSR_DOMAIN_HISTORY) {
			total += entry.bytes_per_pixel;
		}
	}
	return total;
}

uint32_t TSR::internal_bytes_per_pixel() {
	uint32_t total = 0;
	for (const TSRTextureFormatEntry &entry : TSR_FORMAT_TABLE) {
		if (entry.domain == TSR_DOMAIN_INTERNAL) {
			total += entry.bytes_per_pixel;
		}
	}
	return total;
}

uint64_t TSR::estimate_memory_bytes(const Size2i &p_internal_size, const Size2i &p_output_size, RSE::ViewportTSRQuality p_quality, uint32_t p_view_count, bool p_history_rgba16f) {
	const Size2i hist_size = compute_history_size(p_output_size, p_quality);
	const uint64_t internal_pixels = uint64_t(MAX(1, p_internal_size.width)) * uint64_t(MAX(1, p_internal_size.height));
	const uint64_t history_pixels = uint64_t(hist_size.width) * uint64_t(hist_size.height);
	const uint64_t layers = uint64_t(MAX(1u, p_view_count));

	uint64_t hist_bpp = history_bytes_per_pixel();
	if (p_history_rgba16f) {
		// Two history color targets widen from 4 B (B10G11R11) to 8 B (RGBA16F).
		hist_bpp += 8;
	}
	const uint64_t int_bpp = internal_bytes_per_pixel();

	return layers * (history_pixels * hist_bpp + internal_pixels * int_bpp);
}

bool TSRStabilityLayout::has_store(const TSRStabilityStoreEntry &p_entry) const {
	if ((features & p_entry.feature) == 0) {
		return false;
	}
	return p_entry.store != TSR_STABILITY_STORE_PERSISTENT || persistent_count > 0;
}

Size2i TSRStabilityLayout::store_size(TSRStabilityStore p_store) const {
	switch (p_store) {
		case TSR_STABILITY_STORE_PERSISTENT:
			return persistent_size;
		case TSR_STABILITY_STORE_FLICKER:
			return flicker_size;
		case TSR_STABILITY_STORE_THIN:
			return thin_size;
	}
	return Size2i();
}

uint32_t TSRStabilityLayout::store_layers(TSRStabilityStore p_store) const {
	return p_store == TSR_STABILITY_STORE_PERSISTENT ? persistent_count : 1u;
}

uint32_t TSRStabilityLayout::store_bytes_per_pixel(const TSRStabilityStoreEntry &p_entry) const {
	// The persistent frames follow the history colour format, RGBA16F fallback included.
	if (history_rgba16f && p_entry.format == RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32) {
		return 8;
	}
	return p_entry.bytes_per_pixel;
}

uint64_t TSRStabilityLayout::persistent_signature() const {
	if ((features & TSR_FEATURE_RESURRECTION) == 0 || persistent_count == 0) {
		return 0;
	}
	return uint64_t(persistent_count) | (uint64_t(history_rgba16f ? 1 : 0) << 7) | (uint64_t(uint32_t(persistent_size.width)) << 8) | (uint64_t(uint32_t(persistent_size.height)) << 36);
}

// 16.16 scale per axis that brings p_full bytes of stores into p_headroom bytes.
static uint32_t _tsr_stability_fit_q16(int64_t p_headroom, int64_t p_full) {
	if (p_full <= 0 || p_full <= p_headroom) {
		return TSR_FIT_FULL_Q16;
	}
	uint32_t fit = 0;
	if (p_headroom > 0) {
		// One correctly rounded division and square root; everything else is integers.
		fit = uint32_t(Math::floor(65536.0 * Math::sqrt(double(p_headroom) / double(p_full))));
	}
	return CLAMP(fit, TSR_FIT_MIN_Q16, TSR_FIT_FULL_Q16);
}

static Size2i _tsr_stability_scaled(const Size2i &p_size, uint32_t p_fit_q16) {
	return Size2i(
			int(MAX(int64_t(1), (int64_t(p_size.width) * int64_t(p_fit_q16)) >> 16)),
			int(MAX(int64_t(1), (int64_t(p_size.height) * int64_t(p_fit_q16)) >> 16)));
}

static int64_t _tsr_stability_pixels(const Size2i &p_size) {
	return int64_t(p_size.width) * int64_t(p_size.height);
}

TSRStabilityLayout TSR::stability_layout(const Size2i &p_internal_size, const Size2i &p_max_internal_size, const Size2i &p_output_size, RSE::ViewportTSRQuality p_quality, uint32_t p_features, bool p_history_rgba16f, uint32_t p_budget_x100) {
	const int preset_idx = CLAMP(int(p_quality), 0, int(RSE::VIEWPORT_TSR_QUALITY_MAX) - 1);
	const TSRStabilityTuning &tuning = TSR_STABILITY_TUNING[preset_idx];
	const RSE::ViewportTSRQuality quality = RSE::ViewportTSRQuality(preset_idx);

	const Size2i internal_size = p_internal_size.max(Size2i(1, 1));
	// The largest size the internal buffers can take (the top dynamic-resolution bucket).
	const Size2i max_internal_size = p_max_internal_size.max(internal_size);
	const Size2i history_size = compute_history_size(p_output_size, quality);

	TSRStabilityLayout layout;
	layout.features = p_features & TSR_FEATURE_ALL;
	layout.history_rgba16f = p_history_rgba16f;
	layout.core_bytes = estimate_memory_bytes(internal_size, p_output_size, quality, 1, p_history_rgba16f);
	layout.budget_bytes = uint64_t(_tsr_stability_pixels(history_size)) * uint64_t(p_budget_x100) / 100u;

	const bool use_persistent = (layout.features & TSR_FEATURE_RESURRECTION) != 0 && tuning.persistent_frames > 0;
	const bool use_flicker = (layout.features & TSR_FEATURE_FLICKER) != 0;
	const bool use_thin = (layout.features & TSR_FEATURE_THIN_GEOMETRY) != 0;

	// Full-resolution bytes of the state stores at a given internal size: two R8
	// flicker targets, and two R8 thin-coverage targets at half the size.
	auto state_full_bytes = [&](const Size2i &p_size) -> int64_t {
		int64_t bytes = 0;
		if (use_flicker) {
			bytes += 2 * _tsr_stability_pixels(p_size);
		}
		if (use_thin) {
			bytes += 2 * _tsr_stability_pixels((p_size / 2).max(Size2i(1, 1)));
		}
		return bytes;
	};

	const int64_t budget = int64_t(layout.budget_bytes);

	// Stage 1: the persistent frames, fitted with everything at the maximum
	// internal size. They are output-sized, so a dynamic-resolution step must not
	// resize them, which would discard their contents.
	const int64_t persistent_bpp = p_history_rgba16f ? 8 : 4;
	const Size2i persistent_full_size = (p_output_size / int(MAX(uint8_t(1), tuning.persistent_divisor))).max(Size2i(1, 1));
	const int64_t persistent_full_bytes = use_persistent ? int64_t(tuning.persistent_frames) * persistent_bpp * _tsr_stability_pixels(persistent_full_size) : 0;
	const int64_t core_bytes_at_max = int64_t(estimate_memory_bytes(max_internal_size, p_output_size, quality, 1, p_history_rgba16f));
	layout.persistent_fit_q16 = _tsr_stability_fit_q16(budget - core_bytes_at_max, persistent_full_bytes + state_full_bytes(max_internal_size));

	int64_t stability_bytes = 0;
	if (use_persistent) {
		layout.persistent_count = tuning.persistent_frames;
		layout.persistent_size = _tsr_stability_scaled(persistent_full_size, layout.persistent_fit_q16);
		stability_bytes += int64_t(tuning.persistent_frames) * persistent_bpp * _tsr_stability_pixels(layout.persistent_size);
	}

	// Stage 2: the state stores, fitted at the current internal size in what is
	// left. They are internal-sized and recreated at every dynamic-resolution step.
	layout.state_fit_q16 = _tsr_stability_fit_q16(budget - int64_t(layout.core_bytes) - stability_bytes, state_full_bytes(internal_size));
	if (use_flicker) {
		layout.flicker_size = _tsr_stability_scaled(internal_size, layout.state_fit_q16);
		stability_bytes += 2 * _tsr_stability_pixels(layout.flicker_size);
	}
	if (use_thin) {
		layout.thin_size = _tsr_stability_scaled((internal_size / 2).max(Size2i(1, 1)), layout.state_fit_q16);
		stability_bytes += 2 * _tsr_stability_pixels(layout.thin_size);
	}

	layout.stability_bytes = uint64_t(stability_bytes);
	layout.over_budget = layout.total_bytes() > layout.budget_bytes;
	return layout;
}

TSR::TSR() {
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL(rd);

	const uint32_t storage_usage = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	history_use_rgba16f = !rd->texture_is_format_supported_for_usage(RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32, storage_usage);

	{
		Vector<String> single_mode;
		single_mode.push_back("");

		shaders.velocity_dilate.initialize(single_mode);
		shaders.velocity_dilate_version = shaders.velocity_dilate.version_create();
		shaders.velocity_dilate_pipeline = rd->compute_pipeline_create(shaders.velocity_dilate.version_get_shader(shaders.velocity_dilate_version, 0));

		shaders.disocclusion.initialize(single_mode);
		shaders.disocclusion_version = shaders.disocclusion.version_create();
		shaders.disocclusion_pipeline = rd->compute_pipeline_create(shaders.disocclusion.version_get_shader(shaders.disocclusion_version, 0));

		// GodotNext: pt_tsr_stability (#198). Variant 0 is the #194 shader, unchanged.
		Vector<String> rejection_modes = single_mode;
		rejection_modes.push_back("\n#define TSR_STABILITY");

		shaders.shading_rejection.initialize(rejection_modes);
		shaders.shading_rejection_version = shaders.shading_rejection.version_create();
		for (uint32_t i = 0; i < 2; i++) {
			shaders.shading_rejection_pipelines[i] = rd->compute_pipeline_create(shaders.shading_rejection.version_get_shader(shaders.shading_rejection_version, i));
		}

		Vector<String> analysis_modes;
		analysis_modes.push_back("\n#define MODE_FLICKER");
		analysis_modes.push_back("\n#define MODE_THIN");

		shaders.stability_analysis.initialize(analysis_modes);
		shaders.stability_analysis_version = shaders.stability_analysis.version_create();
		for (uint32_t i = 0; i < ANALYSIS_MODE_MAX; i++) {
			shaders.stability_analysis_pipelines[i] = rd->compute_pipeline_create(shaders.stability_analysis.version_get_shader(shaders.stability_analysis_version, i));
		}
	}

	{
		Vector<String> history_modes;
		history_modes.push_back("");
		history_modes.push_back("\n#define MODE_HISTORY_RGBA16F");

		// GodotNext: pt_tsr_stability (#198). The first HISTORY_MODE_MAX variants are
		// the #194 shader, unchanged; the TSR_STABILITY variants follow in the same order.
		Vector<String> history_update_modes = history_modes;
		history_update_modes.push_back("\n#define TSR_STABILITY");
		history_update_modes.push_back("\n#define TSR_STABILITY\n#define MODE_HISTORY_RGBA16F");

		shaders.history_update.initialize(history_update_modes);
		shaders.history_update_version = shaders.history_update.version_create();
		for (uint32_t i = 0; i < 2 * HISTORY_MODE_MAX; i++) {
			shaders.history_update_pipelines[i] = rd->compute_pipeline_create(shaders.history_update.version_get_shader(shaders.history_update_version, i));
		}

		shaders.persistent_store.initialize(history_modes);
		shaders.persistent_store_version = shaders.persistent_store.version_create();
		for (uint32_t i = 0; i < HISTORY_MODE_MAX; i++) {
			shaders.persistent_store_pipelines[i] = rd->compute_pipeline_create(shaders.persistent_store.version_get_shader(shaders.persistent_store_version, i));
		}

		shaders.rejection_aa.initialize(history_modes);
		shaders.rejection_aa_version = shaders.rejection_aa.version_create();
		for (uint32_t i = 0; i < HISTORY_MODE_MAX; i++) {
			shaders.rejection_aa_pipelines[i] = rd->compute_pipeline_create(shaders.rejection_aa.version_get_shader(shaders.rejection_aa_version, i));
		}
	}

	{
		Vector<String> resolve_modes;
		resolve_modes.push_back("");
		resolve_modes.push_back("\n#define MODE_OUTPUT_RGBA16F");
		resolve_modes.push_back("\n#define MODE_OUTPUT_RGB10A2");

		shaders.resolve.initialize(resolve_modes);
		shaders.resolve_version = shaders.resolve.version_create();
		for (uint32_t i = 0; i < RESOLVE_MODE_MAX; i++) {
			shaders.resolve_pipelines[i] = rd->compute_pipeline_create(shaders.resolve.version_get_shader(shaders.resolve_version, i));
		}
	}

	{
		// GodotNext: pt_tsr_stability (#198). The pass visualizer.
		Vector<String> debug_view_modes;
		debug_view_modes.push_back("");

		shaders.debug_view.initialize(debug_view_modes);
		shaders.debug_view_version = shaders.debug_view.version_create();
		shaders.debug_view_pipeline = rd->compute_pipeline_create(shaders.debug_view.version_get_shader(shaders.debug_view_version, 0));
	}

	{
		RD::SamplerState state;
		state.mag_filter = RD::SAMPLER_FILTER_NEAREST;
		state.min_filter = RD::SAMPLER_FILTER_NEAREST;
		state.mip_filter = RD::SAMPLER_FILTER_NEAREST;
		state.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		state.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		state.repeat_w = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		nearest_sampler = rd->sampler_create(state);

		state.mag_filter = RD::SAMPLER_FILTER_LINEAR;
		state.min_filter = RD::SAMPLER_FILTER_LINEAR;
		linear_sampler = rd->sampler_create(state);
	}
}

TSR::~TSR() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return;
	}

	if (nearest_sampler.is_valid()) {
		rd->free_rid(nearest_sampler);
	}
	if (linear_sampler.is_valid()) {
		rd->free_rid(linear_sampler);
	}

	if (shaders.velocity_dilate_pipeline.is_valid()) {
		rd->free_rid(shaders.velocity_dilate_pipeline);
	}
	shaders.velocity_dilate.version_free(shaders.velocity_dilate_version);

	if (shaders.disocclusion_pipeline.is_valid()) {
		rd->free_rid(shaders.disocclusion_pipeline);
	}
	shaders.disocclusion.version_free(shaders.disocclusion_version);

	for (uint32_t i = 0; i < 2; i++) {
		if (shaders.shading_rejection_pipelines[i].is_valid()) {
			rd->free_rid(shaders.shading_rejection_pipelines[i]);
		}
	}
	shaders.shading_rejection.version_free(shaders.shading_rejection_version);

	for (uint32_t i = 0; i < ANALYSIS_MODE_MAX; i++) {
		if (shaders.stability_analysis_pipelines[i].is_valid()) {
			rd->free_rid(shaders.stability_analysis_pipelines[i]);
		}
	}
	shaders.stability_analysis.version_free(shaders.stability_analysis_version);

	for (uint32_t i = 0; i < 2 * HISTORY_MODE_MAX; i++) {
		if (shaders.history_update_pipelines[i].is_valid()) {
			rd->free_rid(shaders.history_update_pipelines[i]);
		}
	}
	for (uint32_t i = 0; i < HISTORY_MODE_MAX; i++) {
		if (shaders.rejection_aa_pipelines[i].is_valid()) {
			rd->free_rid(shaders.rejection_aa_pipelines[i]);
		}
		if (shaders.persistent_store_pipelines[i].is_valid()) {
			rd->free_rid(shaders.persistent_store_pipelines[i]);
		}
	}
	shaders.history_update.version_free(shaders.history_update_version);
	shaders.rejection_aa.version_free(shaders.rejection_aa_version);
	shaders.persistent_store.version_free(shaders.persistent_store_version);

	for (uint32_t i = 0; i < RESOLVE_MODE_MAX; i++) {
		if (shaders.resolve_pipelines[i].is_valid()) {
			rd->free_rid(shaders.resolve_pipelines[i]);
		}
	}
	shaders.resolve.version_free(shaders.resolve_version);

	if (shaders.debug_view_pipeline.is_valid()) {
		rd->free_rid(shaders.debug_view_pipeline);
	}
	shaders.debug_view.version_free(shaders.debug_view_version);
}

TSRContext *TSR::create_context(const Size2i &p_internal_size, const Size2i &p_output_size, RSE::ViewportTSRQuality p_quality) const {
	return memnew(TSRContext(p_internal_size, p_output_size, p_quality));
}

bool TSR::ensure_textures(Ref<RenderSceneBuffersRD> p_render_buffers, const Size2i &p_internal_size, const Size2i &p_history_size) const {
	ERR_FAIL_COND_V(p_render_buffers.is_null(), false);
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);

	const StringName history_color_0_name("history_color_0");
	const StringName dilated_velocity_name("dilated_velocity");

	bool need_recreate_history = !p_render_buffers->has_texture(RB_SCOPE_TSR, history_color_0_name);
	if (!need_recreate_history) {
		const RD::TextureFormat hist_tf = p_render_buffers->get_texture_format(RB_SCOPE_TSR, history_color_0_name);
		if (int(hist_tf.width) != p_history_size.width || int(hist_tf.height) != p_history_size.height) {
			need_recreate_history = true;
		}
	}

	bool need_recreate_internal = !p_render_buffers->has_texture(RB_SCOPE_TSR, dilated_velocity_name);
	if (!need_recreate_internal) {
		const RD::TextureFormat int_tf = p_render_buffers->get_texture_format(RB_SCOPE_TSR, dilated_velocity_name);
		if (int(int_tf.width) != p_internal_size.width || int(int_tf.height) != p_internal_size.height) {
			need_recreate_internal = true;
		}
	}

	if (!need_recreate_history && !need_recreate_internal) {
		return false;
	}

	const uint32_t usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	const uint32_t view_count = p_render_buffers->get_view_count();

	for (const TSRTextureFormatEntry &entry : TSR_FORMAT_TABLE) {
		const bool is_history = entry.domain == TSR_DOMAIN_HISTORY;
		if ((is_history && !need_recreate_history) || (!is_history && !need_recreate_internal)) {
			continue;
		}
		const StringName tex_name(entry.name);
		p_render_buffers->free_texture(RB_SCOPE_TSR, tex_name);

		RD::DataFormat fmt = entry.format;
		if (history_use_rgba16f && fmt == RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32) {
			fmt = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		}
		const Size2i tex_size = is_history ? p_history_size : p_internal_size;
		const RID tex = p_render_buffers->create_texture(RB_SCOPE_TSR, tex_name, fmt, usage_bits, RD::TEXTURE_SAMPLES_1, tex_size);
		if (tex.is_valid()) {
			rd->texture_clear(tex, Color(0, 0, 0, 0), 0, 1, 0, view_count);
		}
	}
	return true;
}

bool TSR::ensure_stability_textures(Ref<RenderSceneBuffersRD> p_render_buffers, const TSRStabilityLayout &p_layout) const {
	ERR_FAIL_COND_V(p_render_buffers.is_null(), false);
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);

	const uint32_t usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	const uint32_t view_count = p_render_buffers->get_view_count();
	bool changed = false;

	for (const TSRStabilityStoreEntry &entry : TSR_STABILITY_STORE_TABLE) {
		const StringName tex_name(entry.name);
		const bool wanted = p_layout.has_store(entry);
		const Size2i tex_size = p_layout.store_size(entry.store);
		const uint32_t layers = p_layout.store_layers(entry.store) * view_count;

		if (p_render_buffers->has_texture(RB_SCOPE_TSR, tex_name)) {
			const RD::TextureFormat tf = p_render_buffers->get_texture_format(RB_SCOPE_TSR, tex_name);
			if (wanted && int(tf.width) == tex_size.width && int(tf.height) == tex_size.height && tf.array_layers == layers) {
				continue;
			}
			p_render_buffers->free_texture(RB_SCOPE_TSR, tex_name);
			changed = true;
		}
		if (!wanted) {
			continue;
		}

		RD::TextureFormat tf;
		tf.format = entry.format;
		if (history_use_rgba16f && tf.format == RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32) {
			tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		}
		// The persistent frames are sampled as an array even with a single layer.
		if (entry.store == TSR_STABILITY_STORE_PERSISTENT || layers > 1) {
			tf.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
		}
		tf.width = tex_size.width;
		tf.height = tex_size.height;
		tf.depth = 1;
		tf.array_layers = layers;
		tf.mipmaps = 1;
		tf.usage_bits = usage_bits;
		tf.samples = RD::TEXTURE_SAMPLES_1;
		const RID tex = p_render_buffers->create_texture_from_format(RB_SCOPE_TSR, tex_name, tf);
		if (tex.is_valid()) {
			rd->texture_clear(tex, Color(entry.clear_value, 0, 0, 0), 0, 1, 0, layers);
		}
		changed = true;
	}
	return changed;
}

void TSR::process(const Parameters &p_params) {
	ERR_FAIL_COND(p_params.render_buffers.is_null());
	ERR_FAIL_NULL(p_params.context);

	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL(rd);
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);

	p_params.context->sync_dimensions(p_params.internal_size, p_params.output_size, p_params.quality);
	if (p_params.reset) {
		p_params.context->request_reset();
	}

	const Size2i internal_size = p_params.internal_size;
	const Size2i output_size = p_params.output_size;
	const Size2i history_size = p_params.context->get_history_size();

	bool textures_changed = false;
	if (p_params.view == 0) {
		textures_changed = ensure_textures(p_params.render_buffers, internal_size, history_size);
	}

	const int preset_idx = CLAMP(int(p_params.quality), 0, int(RSE::VIEWPORT_TSR_QUALITY_MAX) - 1);
	const TSRPreset &preset = TSR_PRESETS[preset_idx];
	const bool do_reset = p_params.context->consume_reset();

	const uint32_t read_idx = p_params.context->get_read_index();
	const uint32_t write_idx = p_params.context->get_write_index();
	const uint32_t view = p_params.view;

	// GodotNext: pt_tsr_stability (#198). Each feature reads TSR_PRESETS[q]; thin
	// geometry reads its project setting. With no feature (Low, thin geometry off)
	// the plan is empty: no texture, no uniform buffer and no pass is added, and
	// the passes below use the #194 shader variants and uniform sets as they are.
	// The pass visualizer (debug_view) only reads, after the resolve: it changes
	// neither the mask nor the plan.
	const uint32_t features = nextgi_tsr_feature_mask(p_params.quality, p_params.thin_geometry);
	const TSRPassPlan plan = nextgi_tsr_pass_plan(features);
	const TSRStabilityLayout layout = stability_layout(internal_size, p_params.max_internal_size, output_size, p_params.quality, features, history_use_rgba16f);
	if (p_params.view == 0) {
		textures_changed = ensure_stability_textures(p_params.render_buffers, layout) || textures_changed;
		if (textures_changed) {
			// T6 accounting: the layout helper and the real allocation must not drift apart.
			const uint64_t allocated_bytes = p_params.render_buffers->get_context_memory_bytes(RB_SCOPE_TSR);
			const uint64_t layout_bytes = layout.total_bytes() * uint64_t(p_params.render_buffers->get_view_count());
			if (allocated_bytes != layout_bytes) {
				ERR_PRINT(vformat("TSR: the textures in the TSR scope take %d bytes, but the stability layout accounts for %d.", allocated_bytes, layout_bytes));
			}
		}
	}

	// Persistent frames do not outlive a history reset, a history resize (handled
	// in sync_dimensions) or their own texture.
	if (do_reset) {
		p_params.context->invalidate_persistent();
	}
	RID persistent_frames;
	if (plan.persistent_store && layout.persistent_count > 0 && p_params.render_buffers->has_texture(RB_SCOPE_TSR, SNAME("persistent_frames"))) {
		persistent_frames = p_params.render_buffers->get_texture(RB_SCOPE_TSR, SNAME("persistent_frames"));
	}
	p_params.context->sync_persistent_storage(persistent_frames, layout.persistent_signature());
	const uint32_t persistent_count = persistent_frames.is_valid() ? layout.persistent_count : 0;

	// Flicker analysis: the state this frame writes and the one it continues from
	// (ping-pong on the context's parity), and this frame's share of the 1/120 s clock.
	const StringName flicker_state_names[2] = { StringName("flicker_state_0"), StringName("flicker_state_1") };
	RID flicker_state_read;
	RID flicker_state_write;
	uint32_t flicker_ticks = 0;
	uint32_t flicker_decay_steps = 0;
	if (plan.analysis_flicker && p_params.render_buffers->has_texture(RB_SCOPE_TSR, flicker_state_names[0]) && p_params.render_buffers->has_texture(RB_SCOPE_TSR, flicker_state_names[1])) {
		flicker_state_read = p_params.render_buffers->get_texture_slice(RB_SCOPE_TSR, flicker_state_names[read_idx], view, 0);
		flicker_state_write = p_params.render_buffers->get_texture_slice(RB_SCOPE_TSR, flicker_state_names[write_idx], view, 0);
		p_params.context->step_flicker_clock(double(p_params.delta_time), flicker_ticks, flicker_decay_steps);
	}
	const bool use_flicker = flicker_state_write.is_valid();

	// Thin geometry: the coverage this frame writes and the one it continues from (ping-pong
	// on the context's parity).
	const StringName thin_coverage_names[2] = { StringName("thin_coverage_0"), StringName("thin_coverage_1") };
	RID thin_coverage_read;
	RID thin_coverage_write;
	if (plan.analysis_thin && p_params.render_buffers->has_texture(RB_SCOPE_TSR, thin_coverage_names[0]) && p_params.render_buffers->has_texture(RB_SCOPE_TSR, thin_coverage_names[1])) {
		thin_coverage_read = p_params.render_buffers->get_texture_slice(RB_SCOPE_TSR, thin_coverage_names[read_idx], view, 0);
		thin_coverage_write = p_params.render_buffers->get_texture_slice(RB_SCOPE_TSR, thin_coverage_names[write_idx], view, 0);
	}
	const bool use_thin = thin_coverage_write.is_valid();

	// The pass visualizer's texture, in a scope of its own so that the T6 accounting never
	// sees it: created while the view is on, freed on the first frame it is off.
	const bool debug_on = p_params.debug_view >= 0;
	const StringName debug_view_name("debug_view");
	if (p_params.view == 0) {
		bool has_debug_texture = p_params.render_buffers->has_texture(RB_SCOPE_TSR_DEBUG, debug_view_name);
		if (has_debug_texture) {
			const RD::TextureFormat debug_tf = p_params.render_buffers->get_texture_format(RB_SCOPE_TSR_DEBUG, debug_view_name);
			if (!debug_on || int(debug_tf.width) != output_size.width || int(debug_tf.height) != output_size.height) {
				p_params.render_buffers->clear_context(RB_SCOPE_TSR_DEBUG);
				has_debug_texture = false;
			}
		}
		if (debug_on && !has_debug_texture) {
			p_params.render_buffers->create_texture(RB_SCOPE_TSR_DEBUG, debug_view_name, RD::DATA_FORMAT_R8G8B8A8_UNORM, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, output_size);
		}
	}
	RID debug_output;
	if (debug_on && p_params.render_buffers->has_texture(RB_SCOPE_TSR_DEBUG, debug_view_name)) {
		debug_output = p_params.render_buffers->get_texture_slice(RB_SCOPE_TSR_DEBUG, debug_view_name, view, 0);
	}

	// The visualizer reads the uniform buffer too, so it exists while the view is on even at Low.
	RID stability_ubo;
	if (plan.stability_ubo || debug_output.is_valid()) {
		stability_ubo = p_params.context->get_stability_ubo();
		if (stability_ubo.is_null()) {
			stability_ubo = rd->uniform_buffer_create(sizeof(StabilityUBO));
			p_params.context->set_stability_ubo(stability_ubo);
		}

		StabilityUBO ubo;
		memset(&ubo, 0, sizeof(StabilityUBO));
		const Projection clip_to_view = p_params.camera_projection.inverse();
		for (uint32_t n = 0; n < persistent_count; n++) {
			const TSRContext::PersistentFrame &frame = p_params.context->get_persistent_frame(n);
			if (!frame.valid) {
				continue;
			}
			// The form `reprojection` has, with the persistent frame's camera in place of the previous frame's.
			const Projection to_persistent = frame.projection * Projection(frame.camera_transform.affine_inverse() * p_params.camera_transform) * clip_to_view;
			MaterialStorage::store_camera(to_persistent, ubo.persistent_reprojection[n]);
			ubo.persistent_valid[n] = 1.0f;
		}
		// A feature whose stores are missing is off for the shaders, so they never read a placeholder as state.
		ubo.features = features;
		if (!use_flicker) {
			ubo.features &= ~uint32_t(TSR_FEATURE_FLICKER);
		}
		if (!use_thin) {
			ubo.features &= ~uint32_t(TSR_FEATURE_THIN_GEOMETRY);
		}
		ubo.debug_view = debug_on ? uint32_t(nextgi_tsr_debug_view_from_setting(p_params.debug_view)) : 0u;
		ubo.persistent_count = persistent_count;
		ubo.persistent_layer_base = view * layout.persistent_count;
		// Smaller, blurrier persistent frames are trusted less.
		ubo.persistent_confidence = TSR_PERSISTENT_CONFIDENCE * float(layout.persistent_fit_q16) / float(TSR_FIT_FULL_Q16);
		if (use_flicker) {
			ubo.flicker_size[0] = layout.flicker_size.width;
			ubo.flicker_size[1] = layout.flicker_size.height;
			ubo.flicker_ticks = flicker_ticks;
			ubo.flicker_decay_steps = flicker_decay_steps;
			ubo.flicker_max_motion = nextgi_tsr_flicker_max_motion(double(p_params.delta_time));
		}
		if (use_thin) {
			ubo.thin_size[0] = layout.thin_size.width;
			ubo.thin_size[1] = layout.thin_size.height;
		}
		rd->buffer_update(stability_ubo, 0, sizeof(StabilityUBO), &ubo);
	}

	const StringName history_color_names[2] = { StringName("history_color_0"), StringName("history_color_1") };
	const StringName history_meta_names[2] = { StringName("history_meta_0"), StringName("history_meta_1") };

	const RID history_color_read = p_params.render_buffers->get_texture_slice(RB_SCOPE_TSR, history_color_names[read_idx], view, 0);
	const RID history_color_write = p_params.render_buffers->get_texture_slice(RB_SCOPE_TSR, history_color_names[write_idx], view, 0);
	const RID history_meta_read = p_params.render_buffers->get_texture_slice(RB_SCOPE_TSR, history_meta_names[read_idx], view, 0);
	const RID history_meta_write = p_params.render_buffers->get_texture_slice(RB_SCOPE_TSR, history_meta_names[write_idx], view, 0);
	const RID dilated_velocity = p_params.render_buffers->get_texture_slice(RB_SCOPE_TSR, SNAME("dilated_velocity"), view, 0);
	const RID prev_depth = p_params.render_buffers->get_texture_slice(RB_SCOPE_TSR, SNAME("prev_depth"), view, 0);
	const RID disocclusion = p_params.render_buffers->get_texture_slice(RB_SCOPE_TSR, SNAME("disocclusion"), view, 0);
	const RID rejection = p_params.render_buffers->get_texture_slice(RB_SCOPE_TSR, SNAME("rejection"), view, 0);

	PushConstant pc;
	memset(&pc, 0, sizeof(PushConstant));
	MaterialStorage::store_camera(p_params.reprojection, pc.reprojection);
	pc.internal_size[0] = internal_size.width;
	pc.internal_size[1] = internal_size.height;
	pc.history_size[0] = history_size.width;
	pc.history_size[1] = history_size.height;
	pc.output_size[0] = output_size.width;
	pc.output_size[1] = output_size.height;
	pc.jitter_pixels[0] = p_params.jitter_pixels.x;
	pc.jitter_pixels[1] = p_params.jitter_pixels.y;
	pc.z_near = p_params.z_near;
	pc.z_far = p_params.z_far;
	pc.flags = 0;
	if (do_reset) {
		pc.flags |= TSR_FLAG_RESET;
	}
	if (p_params.is_orthogonal) {
		pc.flags |= TSR_FLAG_ORTHOGONAL;
	}
	if (preset.rejection_aa) {
		pc.flags |= TSR_FLAG_REJECTION_AA;
	}
	pc.history_scale = preset.history_scale;

	const HistoryMode history_mode = history_use_rgba16f ? HISTORY_MODE_RGBA16F : HISTORY_MODE_R11G11B10F;

	ResolveMode resolve_mode = RESOLVE_MODE_R11G11B10F;
	const RD::DataFormat out_format = rd->texture_get_format(p_params.output).format;
	if (out_format == RD::DATA_FORMAT_R16G16B16A16_SFLOAT) {
		resolve_mode = RESOLVE_MODE_RGBA16F;
	} else if (out_format == RD::DATA_FORMAT_A2B10G10R10_UNORM_PACK32) {
		resolve_mode = RESOLVE_MODE_RGB10A2;
	}

	// Pass 1: Velocity dilation (8x8 at internal resolution).
	{
		rd->draw_command_begin_label("TSR Velocity Dilation");
		RID shader = shaders.velocity_dilate.version_get_shader(shaders.velocity_dilate_version, 0);
		RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, { nearest_sampler, p_params.depth });
		RD::Uniform u_vel(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, { nearest_sampler, p_params.velocity });
		RD::Uniform u_out(RD::UNIFORM_TYPE_IMAGE, 2, { dilated_velocity });

		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, shaders.velocity_dilate_pipeline);
		rd->compute_list_bind_uniform_set(cl, uniform_set_cache->get_cache(shader, 0, u_depth, u_vel, u_out), 0);
		rd->compute_list_set_push_constant(cl, &pc, sizeof(PushConstant));
		rd->compute_list_dispatch_threads(cl, internal_size.width, internal_size.height, 1);
		rd->compute_list_end();
		rd->draw_command_end_label();
	}

	// Pass 2: Parallax disocclusion (8x8 at internal resolution).
	{
		rd->draw_command_begin_label("TSR Parallax Disocclusion");
		RID shader = shaders.disocclusion.version_get_shader(shaders.disocclusion_version, 0);
		RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, { nearest_sampler, p_params.depth });
		RD::Uniform u_dil_vel(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, { linear_sampler, dilated_velocity });
		RD::Uniform u_prev_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, { nearest_sampler, prev_depth });
		RD::Uniform u_out(RD::UNIFORM_TYPE_IMAGE, 3, { disocclusion });

		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, shaders.disocclusion_pipeline);
		rd->compute_list_bind_uniform_set(cl, uniform_set_cache->get_cache(shader, 0, u_depth, u_dil_vel, u_prev_depth, u_out), 0);
		rd->compute_list_set_push_constant(cl, &pc, sizeof(PushConstant));
		rd->compute_list_dispatch_threads(cl, internal_size.width, internal_size.height, 1);
		rd->compute_list_end();
		rd->draw_command_end_label();
	}

	// GodotNext: pt_tsr_stability (#198). Flicker analysis (8x8 at the flicker state's
	// size): advances each state texel from the luma of the internal pixel at its center
	// against the reprojected history. Shading rejection and the history update read it.
	if (use_flicker) {
		rd->draw_command_begin_label("TSR Flicker Analysis");
		RID shader = shaders.stability_analysis.version_get_shader(shaders.stability_analysis_version, ANALYSIS_MODE_FLICKER);
		RD::Uniform u_color(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, { nearest_sampler, p_params.color });
		RD::Uniform u_reactive(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, { nearest_sampler, p_params.reactive });
		RD::Uniform u_dil_vel(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, { nearest_sampler, dilated_velocity });
		RD::Uniform u_hist_prev(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, { linear_sampler, history_color_read });
		RD::Uniform u_disocc(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, { nearest_sampler, disocclusion });
		RD::Uniform u_stability(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 5, stability_ubo);
		RD::Uniform u_state_prev(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, { nearest_sampler, flicker_state_read });
		RD::Uniform u_state_out(RD::UNIFORM_TYPE_IMAGE, 7, { flicker_state_write });

		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, shaders.stability_analysis_pipelines[ANALYSIS_MODE_FLICKER]);
		rd->compute_list_bind_uniform_set(cl, uniform_set_cache->get_cache(shader, 0, u_color, u_reactive, u_dil_vel, u_hist_prev, u_disocc, u_stability, u_state_prev, u_state_out), 0);
		rd->compute_list_set_push_constant(cl, &pc, sizeof(PushConstant));
		rd->compute_list_dispatch_threads(cl, layout.flicker_size.width, layout.flicker_size.height, 1);
		rd->compute_list_end();
		rd->draw_command_end_label();
	}

	// GodotNext: pt_tsr_stability (#198). Thin-geometry analysis (8x8 at the coverage's size,
	// half the internal size): advances each texel's coverage from depth ridges and luma lines
	// one pixel wide in the 2x2 internal pixels at its center. Shading rejection and the history
	// update relax where it is set.
	if (use_thin) {
		rd->draw_command_begin_label("TSR Thin Geometry Analysis");
		RID shader = shaders.stability_analysis.version_get_shader(shaders.stability_analysis_version, ANALYSIS_MODE_THIN);
		RD::Uniform u_color(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, { nearest_sampler, p_params.color });
		RD::Uniform u_reactive(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, { nearest_sampler, p_params.reactive });
		RD::Uniform u_dil_vel(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, { nearest_sampler, dilated_velocity });
		RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, { nearest_sampler, p_params.depth });
		RD::Uniform u_stability(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 5, stability_ubo);
		RD::Uniform u_state_prev(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, { linear_sampler, thin_coverage_read });
		RD::Uniform u_state_out(RD::UNIFORM_TYPE_IMAGE, 7, { thin_coverage_write });

		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, shaders.stability_analysis_pipelines[ANALYSIS_MODE_THIN]);
		rd->compute_list_bind_uniform_set(cl, uniform_set_cache->get_cache(shader, 0, u_color, u_reactive, u_dil_vel, u_depth, u_stability, u_state_prev, u_state_out), 0);
		rd->compute_list_set_push_constant(cl, &pc, sizeof(PushConstant));
		rd->compute_list_dispatch_threads(cl, layout.thin_size.width, layout.thin_size.height, 1);
		rd->compute_list_end();
		rd->draw_command_end_label();
	}

	// Pass 3: Shading rejection + prev_depth update (8x8 at internal resolution).
	{
		rd->draw_command_begin_label("TSR Shading Rejection");
		// GodotNext: pt_tsr_stability (#198). The TSR_STABILITY variant relaxes the shading
		// rejection where the flicker analysis found periodic flicker, and the shading and
		// disocclusion rejection where the thin-geometry analysis found sub-pixel features.
		const uint32_t rejection_variant = plan.rejection_variant;
		RID shader = shaders.shading_rejection.version_get_shader(shaders.shading_rejection_version, rejection_variant);
		RD::Uniform u_color(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, { linear_sampler, p_params.color });
		RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, { nearest_sampler, p_params.depth });
		RD::Uniform u_dil_vel(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, { linear_sampler, dilated_velocity });
		RD::Uniform u_hist_prev(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, { linear_sampler, history_color_read });
		RD::Uniform u_reactive(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, { nearest_sampler, p_params.reactive });
		RD::Uniform u_disocc(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, { nearest_sampler, disocclusion });
		RD::Uniform u_rej_out(RD::UNIFORM_TYPE_IMAGE, 6, { rejection });
		RD::Uniform u_prev_depth_out(RD::UNIFORM_TYPE_IMAGE, 7, { prev_depth });

		RID uniform_set;
		if (rejection_variant != 0) {
			// A store whose feature is off is bound as a black placeholder; `features` skips the read.
			const RID black = TextureStorage::get_singleton()->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
			const RID flicker_source = use_flicker ? flicker_state_write : black;
			const RID thin_source = use_thin ? thin_coverage_write : black;
			RD::Uniform u_stability(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 8, stability_ubo);
			RD::Uniform u_flicker(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 9, { linear_sampler, flicker_source });
			RD::Uniform u_thin(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 10, { linear_sampler, thin_source });
			uniform_set = uniform_set_cache->get_cache(shader, 0, u_color, u_depth, u_dil_vel, u_hist_prev, u_reactive, u_disocc, u_rej_out, u_prev_depth_out, u_stability, u_flicker, u_thin);
		} else {
			uniform_set = uniform_set_cache->get_cache(shader, 0, u_color, u_depth, u_dil_vel, u_hist_prev, u_reactive, u_disocc, u_rej_out, u_prev_depth_out);
		}

		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, shaders.shading_rejection_pipelines[rejection_variant]);
		rd->compute_list_bind_uniform_set(cl, uniform_set, 0);
		rd->compute_list_set_push_constant(cl, &pc, sizeof(PushConstant));
		rd->compute_list_dispatch_threads(cl, internal_size.width, internal_size.height, 1);
		rd->compute_list_end();
		rd->draw_command_end_label();
	}

	// Pass 4: History update (8x8 at history_scale * output resolution).
	{
		rd->draw_command_begin_label("TSR History Update");
		// GodotNext: pt_tsr_stability (#198). The TSR_STABILITY variant adds the stability
		// uniform buffer, the depth, the persistent frames, the flicker state, the thin coverage
		// and the reactive mask (which keeps translucent pixels out of history resurrection).
		const uint32_t history_update_variant = uint32_t(history_mode) + plan.history_variant * HISTORY_MODE_MAX;
		RID shader = shaders.history_update.version_get_shader(shaders.history_update_version, history_update_variant);
		RD::Uniform u_color(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, { linear_sampler, p_params.color });
		RD::Uniform u_dil_vel(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, { linear_sampler, dilated_velocity });
		RD::Uniform u_rej(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, { linear_sampler, rejection });
		RD::Uniform u_hist_prev(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, { linear_sampler, history_color_read });
		RD::Uniform u_meta_prev(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, { linear_sampler, history_meta_read });
		RD::Uniform u_hist_out(RD::UNIFORM_TYPE_IMAGE, 5, { history_color_write });
		RD::Uniform u_meta_out(RD::UNIFORM_TYPE_IMAGE, 6, { history_meta_write });

		RID uniform_set;
		if (plan.history_variant != 0) {
			// A store whose feature is off is bound as a black placeholder; `features` skips the read.
			const RID black = TextureStorage::get_singleton()->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
			const RID persistent_source = persistent_frames.is_valid() ? persistent_frames : TextureStorage::get_singleton()->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK);
			const RID flicker_source = use_flicker ? flicker_state_write : black;
			const RID thin_source = use_thin ? thin_coverage_write : black;
			RD::Uniform u_stability(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 7, stability_ubo);
			RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, { nearest_sampler, p_params.depth });
			RD::Uniform u_persistent(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 9, { linear_sampler, persistent_source });
			RD::Uniform u_flicker(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 10, { linear_sampler, flicker_source });
			RD::Uniform u_thin(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 11, { linear_sampler, thin_source });
			RD::Uniform u_reactive(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 12, { linear_sampler, p_params.reactive });
			uniform_set = uniform_set_cache->get_cache(shader, 0, u_color, u_dil_vel, u_rej, u_hist_prev, u_meta_prev, u_hist_out, u_meta_out, u_stability, u_depth, u_persistent, u_flicker, u_thin, u_reactive);
		} else {
			uniform_set = uniform_set_cache->get_cache(shader, 0, u_color, u_dil_vel, u_rej, u_hist_prev, u_meta_prev, u_hist_out, u_meta_out);
		}

		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, shaders.history_update_pipelines[history_update_variant]);
		rd->compute_list_bind_uniform_set(cl, uniform_set, 0);
		rd->compute_list_set_push_constant(cl, &pc, sizeof(PushConstant));
		rd->compute_list_dispatch_threads(cl, history_size.width, history_size.height, 1);
		rd->compute_list_end();
		rd->draw_command_end_label();
	}

	// Pass 5: Rejection AA (8x8 at history resolution; skipped at Low preset).
	// Writes spatially anti-aliased rejected pixels into the retired history_color[read_idx]
	// target so history_color[write_idx] remains crisp for next frame's reprojection
	// without allocating an extra intermediate texture (T6 budget).
	RID resolve_source = history_color_write;
	if (preset.rejection_aa) {
		rd->draw_command_begin_label("TSR Rejection AA");
		RID shader = shaders.rejection_aa.version_get_shader(shaders.rejection_aa_version, history_mode);
		RD::Uniform u_hist_curr(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, { linear_sampler, history_color_write });
		RD::Uniform u_meta_curr(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, { nearest_sampler, history_meta_write });
		RD::Uniform u_aa_out(RD::UNIFORM_TYPE_IMAGE, 2, { history_color_read });

		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, shaders.rejection_aa_pipelines[history_mode]);
		rd->compute_list_bind_uniform_set(cl, uniform_set_cache->get_cache(shader, 0, u_hist_curr, u_meta_curr, u_aa_out), 0);
		rd->compute_list_set_push_constant(cl, &pc, sizeof(PushConstant));
		rd->compute_list_dispatch_threads(cl, history_size.width, history_size.height, 1);
		rd->compute_list_end();
		rd->draw_command_end_label();
		resolve_source = history_color_read;
	}

	// Pass 6: Resolve to get_upscaled_texture(v), downsampling 200% history when active.
	{
		rd->draw_command_begin_label("TSR Resolve");
		RID shader = shaders.resolve.version_get_shader(shaders.resolve_version, resolve_mode);
		RD::Uniform u_src(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, { linear_sampler, resolve_source });
		RD::Uniform u_out(RD::UNIFORM_TYPE_IMAGE, 1, { p_params.output });

		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, shaders.resolve_pipelines[resolve_mode]);
		rd->compute_list_bind_uniform_set(cl, uniform_set_cache->get_cache(shader, 0, u_src, u_out), 0);
		rd->compute_list_set_push_constant(cl, &pc, sizeof(PushConstant));
		rd->compute_list_dispatch_threads(cl, output_size.width, output_size.height, 1);
		rd->compute_list_end();
		rd->draw_command_end_label();
	}

	// GodotNext: pt_tsr_stability (#198). Pass visualizer (8x8 at output resolution): draws the
	// pass rendering/anti_aliasing/tsr/debug_view selects into RB_SCOPE_TSR_DEBUG, which
	// RenderForwardClustered copies over the frame while Viewport.DEBUG_DRAW_TSR is on. It only
	// reads; stores whose feature is off are bound as black placeholders and `features` skips them.
	if (debug_output.is_valid() && stability_ubo.is_valid()) {
		rd->draw_command_begin_label("TSR Debug View");
		RID shader = shaders.debug_view.version_get_shader(shaders.debug_view_version, 0);
		const RID black = TextureStorage::get_singleton()->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		const RID persistent_source = persistent_frames.is_valid() ? persistent_frames : TextureStorage::get_singleton()->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK);
		RD::Uniform u_output(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, { linear_sampler, p_params.output });
		RD::Uniform u_meta(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, { linear_sampler, history_meta_write });
		RD::Uniform u_rej(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, { linear_sampler, rejection });
		RD::Uniform u_disocc(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, { linear_sampler, disocclusion });
		RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, { nearest_sampler, p_params.depth });
		RD::Uniform u_color(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, { nearest_sampler, p_params.color });
		RD::Uniform u_persistent(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, { linear_sampler, persistent_source });
		RD::Uniform u_flicker(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, { nearest_sampler, use_flicker ? flicker_state_write : black });
		RD::Uniform u_thin(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, { linear_sampler, use_thin ? thin_coverage_write : black });
		RD::Uniform u_stability(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 9, stability_ubo);
		RD::Uniform u_out(RD::UNIFORM_TYPE_IMAGE, 10, { debug_output });
		RD::Uniform u_reactive(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 11, { linear_sampler, p_params.reactive });

		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, shaders.debug_view_pipeline);
		rd->compute_list_bind_uniform_set(cl, uniform_set_cache->get_cache(shader, 0, u_output, u_meta, u_rej, u_disocc, u_depth, u_color, u_persistent, u_flicker, u_thin, u_stability, u_out, u_reactive), 0);
		rd->compute_list_set_push_constant(cl, &pc, sizeof(PushConstant));
		rd->compute_list_dispatch_threads(cl, output_size.width, output_size.height, 1);
		rd->compute_list_end();
		rd->draw_command_end_label();
	}

	// GodotNext: pt_tsr_stability (#198). Persistent store: on a capture frame, keep
	// the history just written as one persistent frame for history resurrection. It reads
	// history_color[write_idx], which rejection AA and the resolve leave alone, and runs
	// last, after every pass that reads the persistent frames (the history update and the
	// pass visualizer), so they all see the frames the stability uniform buffer describes.
	const int32_t capture_slot = persistent_count > 0 ? nextgi_tsr_persistent_capture_slot(p_params.context->get_frames_since_reset(), persistent_count) : -1;
	if (capture_slot >= 0) {
		rd->draw_command_begin_label("TSR Persistent Store");
		RID shader = shaders.persistent_store.version_get_shader(shaders.persistent_store_version, history_mode);
		const RID persistent_layer = p_params.render_buffers->get_texture_slice(RB_SCOPE_TSR, SNAME("persistent_frames"), view * layout.persistent_count + uint32_t(capture_slot), 0);
		RD::Uniform u_src(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, { linear_sampler, history_color_write });
		RD::Uniform u_out(RD::UNIFORM_TYPE_IMAGE, 1, { persistent_layer });

		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, shaders.persistent_store_pipelines[history_mode]);
		rd->compute_list_bind_uniform_set(cl, uniform_set_cache->get_cache(shader, 0, u_src, u_out), 0);
		rd->compute_list_set_push_constant(cl, &pc, sizeof(PushConstant));
		rd->compute_list_dispatch_threads(cl, layout.persistent_size.width, layout.persistent_size.height, 1);
		rd->compute_list_end();
		rd->draw_command_end_label();

		p_params.context->set_persistent_frame(uint32_t(capture_slot), p_params.camera_projection, p_params.camera_transform);
	}

	p_params.context->advance_frame();
}
