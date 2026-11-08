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
	live_context_count.decrement();
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

		shaders.shading_rejection.initialize(single_mode);
		shaders.shading_rejection_version = shaders.shading_rejection.version_create();
		shaders.shading_rejection_pipeline = rd->compute_pipeline_create(shaders.shading_rejection.version_get_shader(shaders.shading_rejection_version, 0));
	}

	{
		Vector<String> history_modes;
		history_modes.push_back("");
		history_modes.push_back("\n#define MODE_HISTORY_RGBA16F");

		shaders.history_update.initialize(history_modes);
		shaders.history_update_version = shaders.history_update.version_create();
		for (uint32_t i = 0; i < HISTORY_MODE_MAX; i++) {
			shaders.history_update_pipelines[i] = rd->compute_pipeline_create(shaders.history_update.version_get_shader(shaders.history_update_version, i));
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

	if (shaders.shading_rejection_pipeline.is_valid()) {
		rd->free_rid(shaders.shading_rejection_pipeline);
	}
	shaders.shading_rejection.version_free(shaders.shading_rejection_version);

	for (uint32_t i = 0; i < HISTORY_MODE_MAX; i++) {
		if (shaders.history_update_pipelines[i].is_valid()) {
			rd->free_rid(shaders.history_update_pipelines[i]);
		}
		if (shaders.rejection_aa_pipelines[i].is_valid()) {
			rd->free_rid(shaders.rejection_aa_pipelines[i]);
		}
	}
	shaders.history_update.version_free(shaders.history_update_version);
	shaders.rejection_aa.version_free(shaders.rejection_aa_version);

	for (uint32_t i = 0; i < RESOLVE_MODE_MAX; i++) {
		if (shaders.resolve_pipelines[i].is_valid()) {
			rd->free_rid(shaders.resolve_pipelines[i]);
		}
	}
	shaders.resolve.version_free(shaders.resolve_version);
}

TSRContext *TSR::create_context(const Size2i &p_internal_size, const Size2i &p_output_size, RSE::ViewportTSRQuality p_quality) const {
	return memnew(TSRContext(p_internal_size, p_output_size, p_quality));
}

void TSR::ensure_textures(Ref<RenderSceneBuffersRD> p_render_buffers, const Size2i &p_internal_size, const Size2i &p_history_size) const {
	ERR_FAIL_COND(p_render_buffers.is_null());
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL(rd);

	const StringName history_color_0_name("history_color_0");
	const StringName dilated_velocity_name("dilated_velocity");

	bool need_recreate = !p_render_buffers->has_texture(RB_SCOPE_TSR, history_color_0_name) || !p_render_buffers->has_texture(RB_SCOPE_TSR, dilated_velocity_name);
	if (!need_recreate) {
		const RD::TextureFormat hist_tf = p_render_buffers->get_texture_format(RB_SCOPE_TSR, history_color_0_name);
		const RD::TextureFormat int_tf = p_render_buffers->get_texture_format(RB_SCOPE_TSR, dilated_velocity_name);
		if (int(hist_tf.width) != p_history_size.width || int(hist_tf.height) != p_history_size.height ||
				int(int_tf.width) != p_internal_size.width || int(int_tf.height) != p_internal_size.height) {
			need_recreate = true;
		}
	}

	if (!need_recreate) {
		return;
	}

	p_render_buffers->clear_context(RB_SCOPE_TSR);

	const uint32_t usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	const uint32_t view_count = p_render_buffers->get_view_count();

	for (const TSRTextureFormatEntry &entry : TSR_FORMAT_TABLE) {
		RD::DataFormat fmt = entry.format;
		if (history_use_rgba16f && fmt == RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32) {
			fmt = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		}
		const Size2i tex_size = (entry.domain == TSR_DOMAIN_HISTORY) ? p_history_size : p_internal_size;
		const RID tex = p_render_buffers->create_texture(RB_SCOPE_TSR, StringName(entry.name), fmt, usage_bits, RD::TEXTURE_SAMPLES_1, tex_size);
		if (tex.is_valid()) {
			rd->texture_clear(tex, Color(0, 0, 0, 0), 0, 1, 0, view_count);
		}
	}
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

	if (p_params.view == 0) {
		ensure_textures(p_params.render_buffers, internal_size, history_size);
	}

	const int preset_idx = CLAMP(int(p_params.quality), 0, int(RSE::VIEWPORT_TSR_QUALITY_MAX) - 1);
	const TSRPreset &preset = TSR_PRESETS[preset_idx];
	const bool do_reset = p_params.context->consume_reset();

	const uint32_t read_idx = p_params.context->get_read_index();
	const uint32_t write_idx = p_params.context->get_write_index();
	const uint32_t view = p_params.view;

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

	// Pass 3: Shading rejection + prev_depth update (8x8 at internal resolution).
	{
		rd->draw_command_begin_label("TSR Shading Rejection");
		RID shader = shaders.shading_rejection.version_get_shader(shaders.shading_rejection_version, 0);
		RD::Uniform u_color(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, { linear_sampler, p_params.color });
		RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, { nearest_sampler, p_params.depth });
		RD::Uniform u_dil_vel(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, { linear_sampler, dilated_velocity });
		RD::Uniform u_hist_prev(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, { linear_sampler, history_color_read });
		RD::Uniform u_reactive(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, { nearest_sampler, p_params.reactive });
		RD::Uniform u_disocc(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, { nearest_sampler, disocclusion });
		RD::Uniform u_rej_out(RD::UNIFORM_TYPE_IMAGE, 6, { rejection });
		RD::Uniform u_prev_depth_out(RD::UNIFORM_TYPE_IMAGE, 7, { prev_depth });

		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, shaders.shading_rejection_pipeline);
		rd->compute_list_bind_uniform_set(cl, uniform_set_cache->get_cache(shader, 0, u_color, u_depth, u_dil_vel, u_hist_prev, u_reactive, u_disocc, u_rej_out, u_prev_depth_out), 0);
		rd->compute_list_set_push_constant(cl, &pc, sizeof(PushConstant));
		rd->compute_list_dispatch_threads(cl, internal_size.width, internal_size.height, 1);
		rd->compute_list_end();
		rd->draw_command_end_label();
	}

	// Pass 4: History update (8x8 at history_scale * output resolution).
	{
		rd->draw_command_begin_label("TSR History Update");
		RID shader = shaders.history_update.version_get_shader(shaders.history_update_version, history_mode);
		RD::Uniform u_color(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, { linear_sampler, p_params.color });
		RD::Uniform u_dil_vel(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, { linear_sampler, dilated_velocity });
		RD::Uniform u_rej(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, { linear_sampler, rejection });
		RD::Uniform u_hist_prev(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, { linear_sampler, history_color_read });
		RD::Uniform u_meta_prev(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, { linear_sampler, history_meta_read });
		RD::Uniform u_hist_out(RD::UNIFORM_TYPE_IMAGE, 5, { history_color_write });
		RD::Uniform u_meta_out(RD::UNIFORM_TYPE_IMAGE, 6, { history_meta_write });

		RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, shaders.history_update_pipelines[history_mode]);
		rd->compute_list_bind_uniform_set(cl, uniform_set_cache->get_cache(shader, 0, u_color, u_dil_vel, u_rej, u_hist_prev, u_meta_prev, u_hist_out, u_meta_out), 0);
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

	p_params.context->advance_frame();
}
