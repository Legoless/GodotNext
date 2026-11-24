/**************************************************************************/
/*  denoise.cpp                                                           */
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

#include "denoise.h"

#include "servers/rendering/renderer_rd/effects/denoiser_bypass.h"
#include "servers/rendering/renderer_rd/environment/next_gi_view.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

NextGIDenoise::NextGIDenoise(bool p_init_gi_shaders) {
	// GodotNext: pu_dlss_rr (#206). Register MegaLights denoiser in the vendor-neutral bypass registry.
	DenoiserBypass::register_denoiser(SNAME("megalights_denoise"));
	if (!p_init_gi_shaders) {
		return;
	}
	Vector<String> shader_modes;
	shader_modes.push_back("\n#define MODE_PROBE_SPATIAL\n");
	shader_modes.push_back("\n#define MODE_PROBE_TEMPORAL\n");
	shader_modes.push_back("\n#define MODE_STABILIZE\n");
	shader.initialize(shader_modes);
	shader_version = shader.version_create();
	Vector<String> specular_modes;
	specular_modes.push_back("\n#define MODE_TEMPORAL\n");
	specular_modes.push_back("\n#define MODE_SPATIAL\n");
	specular_modes.push_back("\n");
	specular_shader.initialize(specular_modes);
	specular_version = specular_shader.version_create();
	specular_temporal = RD::get_singleton()->compute_pipeline_create(specular_shader.version_get_shader(specular_version, 0));
	specular_spatial = RD::get_singleton()->compute_pipeline_create(specular_shader.version_get_shader(specular_version, 1));
	specular_upsample = RD::get_singleton()->compute_pipeline_create(specular_shader.version_get_shader(specular_version, 2));
	specular_params_buffer = RD::get_singleton()->storage_buffer_create(sizeof(float) * 40);
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i] = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, i));
	}
	temporal_params_buffer = RD::get_singleton()->storage_buffer_create(sizeof(TemporalParamsGPU));
	gi_shaders_initialized = true;
}

NextGIDenoise::~NextGIDenoise() {
	RenderingDevice *rd = RD::get_singleton();
	if (gi_shaders_initialized) {
		for (int i = 0; i < MODE_MAX; i++) {
			if (rd != nullptr && pipelines[i].is_valid()) {
				rd->free_rid(pipelines[i]);
			}
		}
		if (rd != nullptr && temporal_params_buffer.is_valid()) {
			rd->free_rid(temporal_params_buffer);
		}
		if (shader_version.is_valid()) {
			shader.version_free(shader_version);
		}
		if (rd != nullptr) {
			if (specular_temporal.is_valid()) {
				rd->free_rid(specular_temporal);
			}
			if (specular_spatial.is_valid()) {
				rd->free_rid(specular_spatial);
			}
			if (specular_upsample.is_valid()) {
				rd->free_rid(specular_upsample);
			}
			if (specular_params_buffer.is_valid()) {
				rd->free_rid(specular_params_buffer);
			}
		}
		if (specular_version.is_valid()) {
			specular_shader.version_free(specular_version);
		}
	}
	if (shadow_version.is_valid()) {
		for (int i = 0; i < SHADOW_MODE_MAX; i++) {
			if (rd != nullptr && shadow_pipelines[i].is_valid()) {
				rd->free_rid(shadow_pipelines[i]);
			}
			shadow_pipelines[i] = RID();
		}
		shadow_shader.version_free(shadow_version);
		shadow_version = RID();
	}
	if (ml_temporal_version.is_valid()) {
		for (int i = 0; i < ML_TEMPORAL_MODE_MAX; i++) {
			if (rd != nullptr && ml_temporal_pipelines[i].is_valid()) {
				rd->free_rid(ml_temporal_pipelines[i]);
			}
			ml_temporal_pipelines[i] = RID();
		}
		ml_temporal_shader.version_free(ml_temporal_version);
		ml_temporal_version = RID();
	}
	if (ml_spatial_version.is_valid()) {
		for (int i = 0; i < ML_SPATIAL_MODE_MAX; i++) {
			if (rd != nullptr && ml_spatial_pipelines[i].is_valid()) {
				rd->free_rid(ml_spatial_pipelines[i]);
			}
			ml_spatial_pipelines[i] = RID();
		}
		ml_spatial_shader.version_free(ml_spatial_version);
		ml_spatial_version = RID();
	}
}

Vector<String> NextGIDenoise::get_shadow_shader_variant_defines() {
	Vector<String> modes;
	modes.push_back("\n#define MODE_TEMPORAL\n");
	modes.push_back("\n#define MODE_SPATIAL\n");
	modes.push_back("\n#define MODE_COPY_PREV_GUIDE\n");
	return modes;
}

bool NextGIDenoise::_ensure_shadow_shader() {
	if (shadow_shader_ready) {
		return true;
	}
	if (shadow_shader_failed) {
		return false;
	}
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	shadow_shader.initialize(get_shadow_shader_variant_defines());
	shadow_version = shadow_shader.version_create(false);
	if (shadow_version.is_null()) {
		shadow_shader_failed = true;
		ERR_FAIL_V_MSG(false, "NextGIDenoise: could not create RT shadow denoiser shader version.");
	}
	for (int i = 0; i < SHADOW_MODE_MAX; i++) {
		const RID shader_rid = shadow_shader.version_get_shader(shadow_version, i);
		if (shader_rid.is_null()) {
			for (int j = 0; j < i; j++) {
				if (shadow_pipelines[j].is_valid()) {
					rd->free_rid(shadow_pipelines[j]);
					shadow_pipelines[j] = RID();
				}
			}
			shadow_shader.version_free(shadow_version);
			shadow_version = RID();
			shadow_shader_failed = true;
			ERR_FAIL_V_MSG(false, "NextGIDenoise: RT shadow denoiser shader failed to compile.");
		}
		shadow_pipelines[i] = rd->compute_pipeline_create(shader_rid);
	}
	shadow_shader_ready = true;
	return true;
}

void NextGIDenoise::shadow_denoise(const ShadowDenoiseParams &p_params) {
	ERR_FAIL_COND(!p_params.raw_mask.is_valid() || !p_params.history_mask_prev.is_valid() || !p_params.history_mask_cur.is_valid());
	ERR_FAIL_COND(!p_params.history_len_prev.is_valid() || !p_params.history_len_cur.is_valid() || !p_params.hit_dist.is_valid());
	ERR_FAIL_COND(!p_params.depth.is_valid() || !p_params.normal_roughness.is_valid() || !p_params.velocity.is_valid() || !p_params.prev_depth_normal.is_valid());
	ERR_FAIL_COND(!p_params.params_buffer.is_valid() || !p_params.lights_buffer.is_valid() || !p_params.tile_list_buffer.is_valid() || !p_params.alloc_state_buffer.is_valid());
	ERR_FAIL_COND(!p_params.cur_lookup_buffer.is_valid() || !p_params.prev_lookup_buffer.is_valid() || !p_params.prev_owner_buffer.is_valid() || !p_params.indirect_buffer.is_valid());
	if (p_params.screen_size.width <= 0 || p_params.screen_size.height <= 0) {
		return;
	}
	if (!_ensure_shadow_shader()) {
		return;
	}

	RenderingDevice *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_COND(rd == nullptr || uniform_set_cache == nullptr || material_storage == nullptr);

	const RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	// 1. Temporal reprojection over allocated tiles: reads raw_mask + history_mask_prev -> writes history_mask_cur.
	{
		const RID shader_rid = shadow_shader.version_get_shader(shadow_version, SHADOW_MODE_TEMPORAL);
		RD::Uniform u_raw(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, p_params.raw_mask }));
		RD::Uniform u_hist_prev(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, p_params.history_mask_prev }));
		RD::Uniform u_len_prev(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ p_params.history_len_prev }));
		RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ nearest_sampler, p_params.depth }));
		RD::Uniform u_normal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest_sampler, p_params.normal_roughness }));
		RD::Uniform u_velocity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ nearest_sampler, p_params.velocity }));
		RD::Uniform u_prev_guide(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ nearest_sampler, p_params.prev_depth_normal }));
		RD::Uniform u_params(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, p_params.params_buffer);
		RD::Uniform u_lights(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, p_params.lights_buffer);
		RD::Uniform u_tiles(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, p_params.tile_list_buffer);
		RD::Uniform u_state(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, p_params.alloc_state_buffer);
		RD::Uniform u_cur_lookup(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, p_params.cur_lookup_buffer);
		RD::Uniform u_prev_lookup(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, p_params.prev_lookup_buffer);
		RD::Uniform u_prev_owner(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, p_params.prev_owner_buffer);
		RD::Uniform u_hist_cur(RD::UNIFORM_TYPE_IMAGE, 14, Vector<RID>({ p_params.history_mask_cur }));
		RD::Uniform u_len_cur(RD::UNIFORM_TYPE_IMAGE, 15, Vector<RID>({ p_params.history_len_cur }));

		const RID uniform_set = uniform_set_cache->get_cache(shader_rid, 0, u_raw, u_hist_prev, u_len_prev, u_depth, u_normal, u_velocity, u_prev_guide, u_params, u_lights, u_tiles, u_state, u_cur_lookup, u_prev_lookup, u_prev_owner, u_hist_cur, u_len_cur);
		ERR_FAIL_COND(uniform_set.is_null());

		const RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, shadow_pipelines[SHADOW_MODE_TEMPORAL]);
		rd->compute_list_bind_uniform_set(cl, uniform_set, 0);
		rd->compute_list_dispatch_indirect(cl, p_params.indirect_buffer, 0);
		rd->compute_list_end();
	}

	// 2. Spatial à-trous pass 1 (step = 1) and pass 2 (step = 2):
	//    pass 1: history_mask_cur -> raw_mask (scratch)
	//    pass 2: raw_mask (scratch) -> history_mask_prev (now dead as previous history; becomes final output).
	struct SpatialPush {
		uint32_t step_size = 1;
		uint32_t pad[3] = {};
	};
	const RID spatial_shader_rid = shadow_shader.version_get_shader(shadow_version, SHADOW_MODE_SPATIAL);
	const RID spatial_sources[2] = { p_params.history_mask_cur, p_params.raw_mask };
	const RID spatial_outputs[2] = { p_params.raw_mask, p_params.history_mask_prev };
	const uint32_t spatial_steps[2] = { 1u, 2u };
	for (int pass = 0; pass < 2; pass++) {
		RD::Uniform u_src(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, spatial_sources[pass] }));
		RD::Uniform u_hit(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, p_params.hit_dist }));
		RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, p_params.depth }));
		RD::Uniform u_normal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ nearest_sampler, p_params.normal_roughness }));
		RD::Uniform u_params(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, p_params.params_buffer);
		RD::Uniform u_lights(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, p_params.lights_buffer);
		RD::Uniform u_tiles(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, p_params.tile_list_buffer);
		RD::Uniform u_state(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, p_params.alloc_state_buffer);
		RD::Uniform u_cur_lookup(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, p_params.cur_lookup_buffer);
		RD::Uniform u_out(RD::UNIFORM_TYPE_IMAGE, 9, Vector<RID>({ spatial_outputs[pass] }));

		const RID uniform_set = uniform_set_cache->get_cache(spatial_shader_rid, 0, u_src, u_hit, u_depth, u_normal, u_params, u_lights, u_tiles, u_state, u_cur_lookup, u_out);
		ERR_FAIL_COND(uniform_set.is_null());

		SpatialPush push;
		push.step_size = spatial_steps[pass];
		const RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, shadow_pipelines[SHADOW_MODE_SPATIAL]);
		rd->compute_list_bind_uniform_set(cl, uniform_set, 0);
		rd->compute_list_set_push_constant(cl, &push, sizeof(push));
		rd->compute_list_dispatch_indirect(cl, p_params.indirect_buffer, 0);
		rd->compute_list_end();
	}

	// 3. Save current view-space normal + linear depth as next frame's disocclusion guide.
	{
		const RID copy_shader_rid = shadow_shader.version_get_shader(shadow_version, SHADOW_MODE_COPY_PREV_GUIDE);
		RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, p_params.depth }));
		RD::Uniform u_normal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, p_params.normal_roughness }));
		RD::Uniform u_params(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, p_params.params_buffer);
		RD::Uniform u_prev_guide(RD::UNIFORM_TYPE_IMAGE, 3, Vector<RID>({ p_params.prev_depth_normal }));

		const RID uniform_set = uniform_set_cache->get_cache(copy_shader_rid, 0, u_depth, u_normal, u_params, u_prev_guide);
		ERR_FAIL_COND(uniform_set.is_null());

		const RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, shadow_pipelines[SHADOW_MODE_COPY_PREV_GUIDE]);
		rd->compute_list_bind_uniform_set(cl, uniform_set, 0);
		rd->compute_list_dispatch_threads(cl, uint32_t(p_params.screen_size.width), uint32_t(p_params.screen_size.height), 1);
		rd->compute_list_end();
	}
}

Vector<String> NextGIDenoise::get_megalights_temporal_shader_variant_defines() {
	Vector<String> modes;
	modes.push_back("\n#define MODE_TEMPORAL\n");
	modes.push_back("\n#define MODE_COPY_PREV_GUIDE\n");
	return modes;
}

Vector<String> NextGIDenoise::get_megalights_spatial_shader_variant_defines() {
	Vector<String> modes;
	modes.push_back("\n#define MODE_SPATIAL\n");
	modes.push_back("\n#define MODE_DEBUG_VIS\n");
	return modes;
}

bool NextGIDenoise::_ensure_megalights_shaders() {
	if (ml_shaders_ready) {
		return true;
	}
	if (ml_shaders_failed) {
		return false;
	}
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);

	ml_temporal_shader.initialize(get_megalights_temporal_shader_variant_defines());
	ml_temporal_version = ml_temporal_shader.version_create(false);
	if (ml_temporal_version.is_null()) {
		ml_shaders_failed = true;
		ERR_FAIL_V_MSG(false, "NextGIDenoise: ml_denoise_temporal.glsl failed to create shader version.");
	}
	for (int i = 0; i < ML_TEMPORAL_MODE_MAX; i++) {
		const RID s = ml_temporal_shader.version_get_shader(ml_temporal_version, i);
		if (s.is_null()) {
			for (int j = 0; j < i; j++) {
				if (ml_temporal_pipelines[j].is_valid()) {
					rd->free_rid(ml_temporal_pipelines[j]);
					ml_temporal_pipelines[j] = RID();
				}
			}
			ml_temporal_shader.version_free(ml_temporal_version);
			ml_temporal_version = RID();
			ml_shaders_failed = true;
			ERR_FAIL_V_MSG(false, "NextGIDenoise: ml_denoise_temporal.glsl failed to compile.");
		}
		ml_temporal_pipelines[i] = rd->compute_pipeline_create(s);
	}

	ml_spatial_shader.initialize(get_megalights_spatial_shader_variant_defines());
	ml_spatial_version = ml_spatial_shader.version_create(false);
	if (ml_spatial_version.is_null()) {
		for (int j = 0; j < ML_TEMPORAL_MODE_MAX; j++) {
			if (ml_temporal_pipelines[j].is_valid()) {
				rd->free_rid(ml_temporal_pipelines[j]);
				ml_temporal_pipelines[j] = RID();
			}
		}
		ml_temporal_shader.version_free(ml_temporal_version);
		ml_temporal_version = RID();
		ml_shaders_failed = true;
		ERR_FAIL_V_MSG(false, "NextGIDenoise: ml_denoise_spatial.glsl failed to create shader version.");
	}
	for (int i = 0; i < ML_SPATIAL_MODE_MAX; i++) {
		const RID s = ml_spatial_shader.version_get_shader(ml_spatial_version, i);
		if (s.is_null()) {
			for (int j = 0; j < i; j++) {
				if (ml_spatial_pipelines[j].is_valid()) {
					rd->free_rid(ml_spatial_pipelines[j]);
					ml_spatial_pipelines[j] = RID();
				}
			}
			ml_spatial_shader.version_free(ml_spatial_version);
			ml_spatial_version = RID();
			for (int j = 0; j < ML_TEMPORAL_MODE_MAX; j++) {
				if (ml_temporal_pipelines[j].is_valid()) {
					rd->free_rid(ml_temporal_pipelines[j]);
					ml_temporal_pipelines[j] = RID();
				}
			}
			ml_temporal_shader.version_free(ml_temporal_version);
			ml_temporal_version = RID();
			ml_shaders_failed = true;
			ERR_FAIL_V_MSG(false, "NextGIDenoise: ml_denoise_spatial.glsl failed to compile.");
		}
		ml_spatial_pipelines[i] = rd->compute_pipeline_create(s);
	}

	ml_shaders_ready = true;
	return true;
}

NextGIDenoise::MegaLightsDenoiseOutput NextGIDenoise::megalights_denoise(const MegaLightsDenoiseParams &p_params) {
	// GodotNext: pu_dlss_rr (#206). Honour vendor-neutral DenoiserBypass contract for MegaLights.
	const bool effective_rr_bypass = p_params.rr_bypass || DenoiserBypass::is_bypassed(p_params.render_buffers);
	MegaLightsDenoiseOutput out;
	out.diffuse = p_params.noisy_diffuse;
	out.specular = p_params.noisy_specular;
	out.bypassed = effective_rr_bypass;

	ERR_FAIL_COND_V(!p_params.noisy_diffuse.is_valid() || !p_params.noisy_specular.is_valid(), out);
	ERR_FAIL_COND_V(!p_params.depth.is_valid() || !p_params.params_ubo.is_valid(), out);
	if (p_params.screen_size.width <= 0 || p_params.screen_size.height <= 0) {
		return out;
	}

	RenderingDevice *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_COND_V(rd == nullptr || uniform_set_cache == nullptr || material_storage == nullptr, out);

	// Fill shared MLDenoiseParams UBO (used by temporal, spatial, and optional debug vis).
	MegaLightsDenoiseParamsGPU ubo;
	NextGIViewMath::store(p_params.inv_projection, ubo.inv_projection);
	NextGIViewMath::store(p_params.inv_view_proj, ubo.inv_view_proj);
	NextGIViewMath::store(p_params.prev_view_proj, ubo.prev_view_proj);
	NextGIViewMath::store_basis(p_params.view_to_world, ubo.view_to_world);
	ubo.cur_depth_coeffs[0] = float(p_params.cur_depth_coeffs.x);
	ubo.cur_depth_coeffs[1] = float(p_params.cur_depth_coeffs.y);
	ubo.cur_depth_coeffs[2] = float(p_params.cur_depth_coeffs.z);
	ubo.cur_depth_coeffs[3] = float(p_params.cur_depth_coeffs.w);
	ubo.prev_depth_coeffs[0] = float(p_params.prev_depth_coeffs.x);
	ubo.prev_depth_coeffs[1] = float(p_params.prev_depth_coeffs.y);
	ubo.prev_depth_coeffs[2] = float(p_params.prev_depth_coeffs.z);
	ubo.prev_depth_coeffs[3] = float(p_params.prev_depth_coeffs.w);
	ubo.camera_pos[0] = float(p_params.camera_pos.x);
	ubo.camera_pos[1] = float(p_params.camera_pos.y);
	ubo.camera_pos[2] = float(p_params.camera_pos.z);
	ubo.camera_pos[3] = 1.0f;
	ubo.prev_camera_pos[0] = float(p_params.prev_camera_pos.x);
	ubo.prev_camera_pos[1] = float(p_params.prev_camera_pos.y);
	ubo.prev_camera_pos[2] = float(p_params.prev_camera_pos.z);
	ubo.prev_camera_pos[3] = 1.0f;
	ubo.screen_reset_size[0] = uint32_t(p_params.screen_size.width);
	ubo.screen_reset_size[1] = uint32_t(p_params.screen_size.height);
	ubo.screen_reset_size[2] = uint32_t(MAX(p_params.reset_tile_dims.width, 0));
	ubo.screen_reset_size[3] = uint32_t(MAX(p_params.reset_tile_dims.height, 0));
	uint32_t flags = 0u;
	if (p_params.has_history) {
		flags |= ML_DENOISE_FLAG_HAS_HISTORY;
	}
	if (p_params.velocity_enabled && p_params.velocity.is_valid()) {
		flags |= ML_DENOISE_FLAG_VELOCITY_ENABLED;
	}
	if (p_params.reset_mask_enabled && p_params.reset_mask.is_valid() && p_params.reset_tile_dims.width > 0 && p_params.reset_tile_dims.height > 0) {
		flags |= ML_DENOISE_FLAG_RESET_MASK_ENABLED;
	}
	if (p_params.orthogonal) {
		flags |= ML_DENOISE_FLAG_ORTHOGONAL;
	}
	if (effective_rr_bypass) {
		flags |= ML_DENOISE_FLAG_RR_BYPASS;
	}
	ubo.config[0] = p_params.frame_index;
	ubo.config[1] = flags;
	ubo.config[2] = MAX(p_params.max_history_diffuse, 1u);
	ubo.config[3] = MAX(p_params.max_history_specular, 1u);

	const RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const RID linear_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	if (effective_rr_bypass) {
		// RR Bypass (DLSS-RR contract, PU+): skip temporal and spatial denoising entirely and
		// expose the noisy demodulated diffuse & specular targets directly at render resolution.
		if (p_params.debug_vis_mode >= 0 && p_params.debug_vis_output.is_valid() && _ensure_megalights_shaders()) {
			rd->buffer_update(p_params.params_ubo, 0, sizeof(ubo), &ubo);
			const RID vis_shader = ml_spatial_shader.version_get_shader(ml_spatial_version, ML_SPATIAL_MODE_DEBUG_VIS);
			LocalVector<RD::Uniform> u;
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_params.params_ubo));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, p_params.noisy_diffuse })));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, p_params.noisy_specular })));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ nearest_sampler, p_params.noisy_diffuse })));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest_sampler, p_params.noisy_specular })));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ nearest_sampler, p_params.noisy_diffuse })));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ nearest_sampler, p_params.noisy_specular })));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ nearest_sampler, p_params.depth })));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, Vector<RID>({ p_params.debug_vis_output })));
			const RID set = uniform_set_cache->get_cache_vec(vis_shader, 0, u);
			if (set.is_valid()) {
				struct DebugPush {
					uint32_t debug_mode = 0;
					uint32_t rr_bypass = 1;
					uint32_t pad[2] = {};
				} push;
				push.debug_mode = uint32_t(CLAMP(p_params.debug_vis_mode, 0, 2));
				push.rr_bypass = 1u;
				const RD::ComputeListID cl = rd->compute_list_begin();
				rd->compute_list_bind_compute_pipeline(cl, ml_spatial_pipelines[ML_SPATIAL_MODE_DEBUG_VIS]);
				rd->compute_list_bind_uniform_set(cl, set, 0);
				rd->compute_list_set_push_constant(cl, &push, sizeof(push));
				rd->compute_list_dispatch_threads(cl, uint32_t(p_params.screen_size.width), uint32_t(p_params.screen_size.height), 1u);
				rd->compute_list_end();
			}
		}
		return out;
	}

	ERR_FAIL_COND_V(!p_params.normal_roughness.is_valid() || !p_params.ml_albedo.is_valid() || !p_params.ml_material.is_valid() || !p_params.dfg_lut.is_valid(), out);
	ERR_FAIL_COND_V(!p_params.prev_depth_normal.is_valid() || !p_params.history_diffuse_prev.is_valid() || !p_params.history_diffuse_cur.is_valid(), out);
	ERR_FAIL_COND_V(!p_params.history_specular_prev.is_valid() || !p_params.history_specular_cur.is_valid(), out);
	ERR_FAIL_COND_V(!p_params.spatial_scratch_diffuse.is_valid() || !p_params.spatial_scratch_specular.is_valid(), out);
	ERR_FAIL_COND_V(!p_params.moments_prev.is_valid() || !p_params.moments_cur.is_valid(), out);
	ERR_FAIL_COND_V(!p_params.history_len_prev.is_valid() || !p_params.history_len_cur.is_valid(), out);
	ERR_FAIL_COND_V(!p_params.clamp_stats_buffer.is_valid() || !p_params.reset_mask.is_valid(), out);

	if (!_ensure_megalights_shaders()) {
		return out;
	}

	rd->buffer_update(p_params.params_ubo, 0, sizeof(ubo), &ubo);
	rd->buffer_clear(p_params.clamp_stats_buffer, 0, 4u * sizeof(uint32_t));

	const RID vel_tex = (p_params.velocity_enabled && p_params.velocity.is_valid()) ? p_params.velocity : p_params.normal_roughness;
	const uint32_t w = uint32_t(p_params.screen_size.width);
	const uint32_t h = uint32_t(p_params.screen_size.height);

	rd->draw_command_begin_label("MegaLights Denoiser");

	// 1. Temporal accumulation with velocity reprojection, disocclusion rejection, firefly clamp, and luminance moments.
	{
		const RID shader_rid = ml_temporal_shader.version_get_shader(ml_temporal_version, ML_TEMPORAL_MODE_ACCUM);
		LocalVector<RD::Uniform> u;
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_params.params_ubo));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, p_params.noisy_diffuse })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, p_params.noisy_specular })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ nearest_sampler, p_params.history_diffuse_prev })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest_sampler, p_params.history_specular_prev })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ nearest_sampler, p_params.moments_prev })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 6, Vector<RID>({ p_params.history_len_prev })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ nearest_sampler, p_params.depth })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ nearest_sampler, p_params.normal_roughness })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 9, Vector<RID>({ nearest_sampler, vel_tex })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 10, Vector<RID>({ nearest_sampler, p_params.prev_depth_normal })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 11, Vector<RID>({ nearest_sampler, p_params.ml_albedo })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 12, Vector<RID>({ nearest_sampler, p_params.ml_material })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 13, Vector<RID>({ linear_sampler, p_params.dfg_lut })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, p_params.reset_mask));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 15, Vector<RID>({ p_params.history_diffuse_cur })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 16, Vector<RID>({ p_params.history_specular_cur })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 17, Vector<RID>({ p_params.moments_cur })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 18, Vector<RID>({ p_params.history_len_cur })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 19, p_params.clamp_stats_buffer));

		const RID set = uniform_set_cache->get_cache_vec(shader_rid, 0, u);
		if (set.is_valid()) {
			const RD::ComputeListID cl = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(cl, ml_temporal_pipelines[ML_TEMPORAL_MODE_ACCUM]);
			rd->compute_list_bind_uniform_set(cl, set, 0);
			rd->compute_list_dispatch_threads(cl, w, h, 1u);
			rd->compute_list_end();
		}
	}

	// 2. Two variance-guided à-trous spatial passes (step widths 1 and 2):
	//    pass 0 (step = 1): history_*_cur -> spatial_scratch_*
	//    pass 1 (step = 2): spatial_scratch_* -> history_*_prev (now dead as previous history; becomes final output).
	{
		struct SpatialPush {
			uint32_t step_size = 1;
			uint32_t pass_index = 0;
			uint32_t pad[2] = {};
		};
		const RID shader_rid = ml_spatial_shader.version_get_shader(ml_spatial_version, ML_SPATIAL_MODE_ATROUS);
		const RID src_diff[2] = { p_params.history_diffuse_cur, p_params.spatial_scratch_diffuse };
		const RID src_spec[2] = { p_params.history_specular_cur, p_params.spatial_scratch_specular };
		const RID dst_diff[2] = { p_params.spatial_scratch_diffuse, p_params.history_diffuse_prev };
		const RID dst_spec[2] = { p_params.spatial_scratch_specular, p_params.history_specular_prev };
		const uint32_t steps[2] = { 1u, 2u };

		for (uint32_t pass = 0u; pass < 2u; pass++) {
			LocalVector<RD::Uniform> u;
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_params.params_ubo));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, src_diff[pass] })));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, src_spec[pass] })));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ nearest_sampler, p_params.noisy_diffuse })));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest_sampler, p_params.noisy_specular })));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ nearest_sampler, p_params.depth })));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ nearest_sampler, p_params.normal_roughness })));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ nearest_sampler, p_params.ml_material })));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, Vector<RID>({ dst_diff[pass] })));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, Vector<RID>({ dst_spec[pass] })));

			const RID set = uniform_set_cache->get_cache_vec(shader_rid, 0, u);
			if (set.is_valid()) {
				SpatialPush push;
				push.step_size = steps[pass];
				push.pass_index = pass;
				const RD::ComputeListID cl = rd->compute_list_begin();
				rd->compute_list_bind_compute_pipeline(cl, ml_spatial_pipelines[ML_SPATIAL_MODE_ATROUS]);
				rd->compute_list_bind_uniform_set(cl, set, 0);
				rd->compute_list_set_push_constant(cl, &push, sizeof(push));
				rd->compute_list_dispatch_threads(cl, w, h, 1u);
				rd->compute_list_end();
			}
		}
	}

	// 3. Save current world-space normal + linear depth as next frame's disocclusion guide.
	{
		const RID shader_rid = ml_temporal_shader.version_get_shader(ml_temporal_version, ML_TEMPORAL_MODE_COPY_PREV_GUIDE);
		LocalVector<RD::Uniform> u;
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_params.params_ubo));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, p_params.depth })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, p_params.normal_roughness })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, Vector<RID>({ p_params.prev_depth_normal })));

		const RID set = uniform_set_cache->get_cache_vec(shader_rid, 0, u);
		if (set.is_valid()) {
			const RD::ComputeListID cl = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(cl, ml_temporal_pipelines[ML_TEMPORAL_MODE_COPY_PREV_GUIDE]);
			rd->compute_list_bind_uniform_set(cl, set, 0);
			rd->compute_list_dispatch_threads(cl, w, h, 1u);
			rd->compute_list_end();
		}
	}

	// 4. Optional on-demand debug visualization (ML_NOISY, ML_DENOISED, ML_VARIANCE).
	if (p_params.debug_vis_mode >= 0 && p_params.debug_vis_output.is_valid()) {
		const RID vis_shader = ml_spatial_shader.version_get_shader(ml_spatial_version, ML_SPATIAL_MODE_DEBUG_VIS);
		LocalVector<RD::Uniform> u;
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_params.params_ubo));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, p_params.noisy_diffuse })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, p_params.noisy_specular })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ nearest_sampler, p_params.history_diffuse_prev })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest_sampler, p_params.history_specular_prev })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ nearest_sampler, p_params.history_diffuse_cur })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ nearest_sampler, p_params.history_specular_cur })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ nearest_sampler, p_params.depth })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, Vector<RID>({ p_params.debug_vis_output })));

		const RID set = uniform_set_cache->get_cache_vec(vis_shader, 0, u);
		if (set.is_valid()) {
			struct DebugPush {
				uint32_t debug_mode = 0;
				uint32_t rr_bypass = 0;
				uint32_t pad[2] = {};
			} push;
			push.debug_mode = uint32_t(CLAMP(p_params.debug_vis_mode, 0, 2));
			push.rr_bypass = 0u;
			const RD::ComputeListID cl = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(cl, ml_spatial_pipelines[ML_SPATIAL_MODE_DEBUG_VIS]);
			rd->compute_list_bind_uniform_set(cl, set, 0);
			rd->compute_list_set_push_constant(cl, &push, sizeof(push));
			rd->compute_list_dispatch_threads(cl, w, h, 1u);
			rd->compute_list_end();
		}
	}

	rd->draw_command_end_label();

	out.diffuse = p_params.history_diffuse_prev;
	out.specular = p_params.history_specular_prev;
	out.variance_diffuse = p_params.history_diffuse_cur;
	out.variance_specular = p_params.history_specular_cur;
	out.bypassed = false;
	return out;
}

void NextGIDenoise::process_specular_temporal(const SpecularTemporalParams &p_params) {
	ERR_FAIL_COND_MSG(!p_params.raw.is_valid(), "NextGIDenoise: specular raw is invalid.");
	ERR_FAIL_COND_MSG(!p_params.history.is_valid(), "NextGIDenoise: specular history is invalid.");
	ERR_FAIL_COND_MSG(!p_params.history_length.is_valid(), "NextGIDenoise: specular history length is invalid.");

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	struct PushConstant {
		int32_t trace_size[2];
		int32_t screen_size[2];
		float depth_tolerance;
		float normal_tolerance;
		uint32_t velocity_enabled;
		uint32_t history_cap_min;
		uint32_t debug_views;
		uint32_t pad;
	} push_constant;
	push_constant.trace_size[0] = p_params.trace_size.width;
	push_constant.trace_size[1] = p_params.trace_size.height;
	push_constant.screen_size[0] = p_params.screen_size.width;
	push_constant.screen_size[1] = p_params.screen_size.height;
	push_constant.depth_tolerance = 0.1f;
	push_constant.normal_tolerance = 0.9f;
	push_constant.velocity_enabled = p_params.velocity_enabled ? 1u : 0u;
	push_constant.history_cap_min = p_params.history_cap_min;
	push_constant.debug_views = p_params.debug_views ? 1u : 0u;
	push_constant.pad = 0u;
	ERR_FAIL_COND_MSG(p_params.debug_views && (!p_params.debug_variance.is_valid() || !p_params.debug_rejected.is_valid()), "NextGIDenoise: the specular debug views need two images.");

	float gpu_params[40];
	for (int i = 0; i < 16; i++) {
		gpu_params[i] = p_params.prev_view_proj.columns[i / 4][i % 4];
		gpu_params[16 + i] = p_params.inv_view_proj.columns[i / 4][i % 4];
	}
	gpu_params[32] = p_params.camera_pos.x;
	gpu_params[33] = p_params.camera_pos.y;
	gpu_params[34] = p_params.camera_pos.z;
	gpu_params[35] = 1.0f;
	gpu_params[36] = p_params.prev_camera_pos.x;
	gpu_params[37] = p_params.prev_camera_pos.y;
	gpu_params[38] = p_params.prev_camera_pos.z;
	gpu_params[39] = 1.0f;
	RD::get_singleton()->buffer_update(specular_params_buffer, 0, sizeof(gpu_params), gpu_params);

	RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID shader_rid = specular_shader.version_get_shader(specular_version, 0);
	RD::Uniform u_raw(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, p_params.raw }));
	RD::Uniform u_history(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, p_params.history }));
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, p_params.depth }));
	RD::Uniform u_normal_roughness(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ nearest_sampler, p_params.normal_roughness }));
	RD::Uniform u_velocity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest_sampler, p_params.velocity }));
	RD::Uniform u_hit_distance(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ nearest_sampler, p_params.hit_distance }));
	RD::Uniform u_output(RD::UNIFORM_TYPE_IMAGE, 6, Vector<RID>({ p_params.output }));
	RD::Uniform u_history_length(RD::UNIFORM_TYPE_IMAGE, 7, Vector<RID>({ p_params.history_length }));
	RD::Uniform u_params(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, specular_params_buffer);
	RD::Uniform u_debug_variance(RD::UNIFORM_TYPE_IMAGE, 9, Vector<RID>({ p_params.debug_variance.is_valid() ? p_params.debug_variance : p_params.output }));
	RD::Uniform u_debug_rejected(RD::UNIFORM_TYPE_IMAGE, 10, Vector<RID>({ p_params.debug_rejected.is_valid() ? p_params.debug_rejected : p_params.output }));
	RID uniform_set = uniform_set_cache->get_cache(shader_rid, 0, u_raw, u_history, u_depth, u_normal_roughness, u_velocity, u_hit_distance, u_output, u_history_length, u_params, u_debug_variance, u_debug_rejected);
	ERR_FAIL_COND_MSG(uniform_set.is_null(), "NextGIDenoise: could not build the specular temporal uniform set.");

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, specular_temporal);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(push_constant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_params.trace_size.width, p_params.trace_size.height, 1);
	RD::get_singleton()->compute_list_end();
}

void NextGIDenoise::process_specular_spatial(const SpecularSpatialParams &p_params) {
	ERR_FAIL_COND_MSG(!p_params.source.is_valid(), "NextGIDenoise: specular source is invalid.");
	ERR_FAIL_COND_MSG(!p_params.output.is_valid(), "NextGIDenoise: specular output is invalid.");

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	struct PushConstant {
		int32_t trace_size[2];
		int32_t screen_size[2];
		int32_t step;
		float normal_power;
	} push_constant;
	push_constant.trace_size[0] = p_params.trace_size.width;
	push_constant.trace_size[1] = p_params.trace_size.height;
	push_constant.screen_size[0] = p_params.screen_size.width;
	push_constant.screen_size[1] = p_params.screen_size.height;
	push_constant.step = p_params.step;
	push_constant.normal_power = p_params.normal_power;

	RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID shader_rid = specular_shader.version_get_shader(specular_version, 1);
	RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, p_params.source }));
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, p_params.depth }));
	RD::Uniform u_normal_roughness(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, p_params.normal_roughness }));
	RD::Uniform u_output(RD::UNIFORM_TYPE_IMAGE, 3, Vector<RID>({ p_params.output }));
	RID uniform_set = uniform_set_cache->get_cache(shader_rid, 0, u_source, u_depth, u_normal_roughness, u_output);
	ERR_FAIL_COND_MSG(uniform_set.is_null(), "NextGIDenoise: could not build the specular spatial uniform set.");

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, specular_spatial);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(push_constant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_params.trace_size.width, p_params.trace_size.height, 1);
	RD::get_singleton()->compute_list_end();
}

void NextGIDenoise::process_specular_upsample(const SpecularUpsampleParams &p_params) {
	ERR_FAIL_COND_MSG(!p_params.raw.is_valid(), "NextGIDenoise: specular raw is invalid.");
	ERR_FAIL_COND_MSG(!p_params.depth.is_valid(), "NextGIDenoise: specular depth is invalid.");
	ERR_FAIL_COND_MSG(!p_params.normal_roughness.is_valid(), "NextGIDenoise: specular normal-roughness is invalid.");
	ERR_FAIL_COND_MSG(!p_params.output.is_valid(), "NextGIDenoise: specular output is invalid.");

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	struct PushConstant {
		int32_t trace_size[2];
		int32_t screen_size[2];
	} push_constant;
	push_constant.trace_size[0] = p_params.trace_size.width;
	push_constant.trace_size[1] = p_params.trace_size.height;
	push_constant.screen_size[0] = p_params.screen_size.width;
	push_constant.screen_size[1] = p_params.screen_size.height;

	RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID shader_rid = specular_shader.version_get_shader(specular_version, 2);
	RD::Uniform u_raw(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, p_params.raw }));
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, p_params.depth }));
	RD::Uniform u_normal_roughness(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest_sampler, p_params.normal_roughness }));
	RD::Uniform u_output(RD::UNIFORM_TYPE_IMAGE, 3, Vector<RID>({ p_params.output }));
	RID uniform_set = uniform_set_cache->get_cache(shader_rid, 0, u_raw, u_depth, u_normal_roughness, u_output);
	ERR_FAIL_COND_MSG(uniform_set.is_null(), "NextGIDenoise: could not build the specular upsample uniform set.");

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, specular_upsample);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(push_constant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_params.screen_size.width, p_params.screen_size.height, 1);
	RD::get_singleton()->compute_list_end();
}

void NextGIDenoise::process_diffuse(const DiffuseParams &p_params) {
	ERR_FAIL_COND_MSG(!p_params.probe_radiance.is_valid(), "NextGIDenoise: probe radiance is invalid.");
	ERR_FAIL_COND_MSG(!p_params.probe_radiance_out.is_valid(), "NextGIDenoise: probe radiance output is invalid.");
	ERR_FAIL_COND_MSG(!p_params.depth.is_valid(), "NextGIDenoise: depth is invalid.");
	ERR_FAIL_COND_MSG(!p_params.normal_roughness.is_valid(), "NextGIDenoise: normal-roughness guide is invalid.");
	ERR_FAIL_COND_MSG(p_params.probe_radiance == p_params.probe_radiance_out, "NextGIDenoise: the spatial pass cannot read and write the same probe texture.");

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	DiffusePushConstant push_constant;
	for (int i = 0; i < 16; i++) {
		push_constant.inv_projection[i] = p_params.inv_projection.columns[i / 4][i % 4];
	}
	push_constant.probe_size[0] = p_params.probe_size.width;
	push_constant.probe_size[1] = p_params.probe_size.height;
	push_constant.screen_size[0] = p_params.screen_size.width;
	push_constant.screen_size[1] = p_params.screen_size.height;
	push_constant.depth_threshold = p_params.depth_threshold;
	push_constant.normal_power = p_params.normal_power;
	push_constant.probe_stride = p_params.probe_stride;
	push_constant.probe_extent = p_params.probe_extent;

	RID default_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID denoise_shader = shader.version_get_shader(shader_version, MODE_PROBE_SPATIAL);

	RD::Uniform u_probe_radiance(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ default_sampler, p_params.probe_radiance }));
	RD::Uniform u_probe_radiance_out(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ p_params.probe_radiance_out }));
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ default_sampler, p_params.depth }));
	RD::Uniform u_normal_roughness(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ default_sampler, p_params.normal_roughness }));

	RID uniform_set = uniform_set_cache->get_cache(denoise_shader, 0, u_probe_radiance, u_probe_radiance_out, u_depth, u_normal_roughness);
	ERR_FAIL_COND_MSG(uniform_set.is_null(), "NextGIDenoise: could not build the spatial uniform set.");

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_PROBE_SPATIAL]);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(DiffusePushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_params.probe_size.width, p_params.probe_size.height, 1);
	RD::get_singleton()->compute_list_end();
}

void NextGIDenoise::process_temporal(const TemporalParams &p_params) {
	ERR_FAIL_COND_MSG(!p_params.probe_radiance.is_valid(), "NextGIDenoise: probe radiance is invalid.");
	ERR_FAIL_COND_MSG(!p_params.probe_radiance_out.is_valid(), "NextGIDenoise: probe radiance output is invalid.");
	ERR_FAIL_COND_MSG(!p_params.probe_history.is_valid(), "NextGIDenoise: probe history is invalid.");
	ERR_FAIL_COND_MSG(!p_params.depth.is_valid(), "NextGIDenoise: depth is invalid.");
	ERR_FAIL_COND_MSG(!p_params.normal_roughness.is_valid(), "NextGIDenoise: normal-roughness guide is invalid.");
	ERR_FAIL_COND_MSG(!p_params.velocity.is_valid(), "NextGIDenoise: velocity is invalid.");
	ERR_FAIL_COND_MSG(!p_params.history_length.is_valid(), "NextGIDenoise: history length is invalid.");
	ERR_FAIL_COND_MSG(!p_params.reset_mask.is_valid(), "NextGIDenoise: reset mask is invalid.");
	ERR_FAIL_COND_MSG(p_params.probe_radiance == p_params.probe_radiance_out, "NextGIDenoise: the temporal pass cannot read and write the same probe texture.");

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	TemporalParamsGPU gpu_params;
	for (int i = 0; i < 16; i++) {
		gpu_params.prev_view_proj[i] = p_params.prev_view_proj.columns[i / 4][i % 4];
		gpu_params.inv_view_proj[i] = p_params.inv_view_proj.columns[i / 4][i % 4];
	}
	gpu_params.camera_pos[0] = p_params.camera_pos.x;
	gpu_params.camera_pos[1] = p_params.camera_pos.y;
	gpu_params.camera_pos[2] = p_params.camera_pos.z;
	gpu_params.camera_pos[3] = 1.0f;
	gpu_params.prev_camera_pos[0] = p_params.prev_camera_pos.x;
	gpu_params.prev_camera_pos[1] = p_params.prev_camera_pos.y;
	gpu_params.prev_camera_pos[2] = p_params.prev_camera_pos.z;
	gpu_params.prev_camera_pos[3] = 1.0f;
	RD::get_singleton()->buffer_update(temporal_params_buffer, 0, sizeof(TemporalParamsGPU), &gpu_params);

	TemporalPushConstant push_constant;
	push_constant.probe_size[0] = p_params.probe_size.width;
	push_constant.probe_size[1] = p_params.probe_size.height;
	push_constant.screen_size[0] = p_params.screen_size.width;
	push_constant.screen_size[1] = p_params.screen_size.height;
	push_constant.depth_tolerance = p_params.depth_tolerance;
	push_constant.normal_tolerance = p_params.normal_tolerance;
	push_constant.history_cap = p_params.history_cap;
	push_constant.velocity_enabled = p_params.velocity_enabled ? 1u : 0u;
	push_constant.probe_stride = p_params.probe_stride;
	push_constant.probe_extent = p_params.probe_extent;
	push_constant.debug_views = p_params.debug_views ? 1u : 0u;
	ERR_FAIL_COND_MSG(p_params.debug_views && (!p_params.debug_variance.is_valid() || !p_params.debug_history.is_valid() || !p_params.debug_rejected.is_valid()), "NextGIDenoise: the debug views need three valid images.");

	// Inactive debug views still need a valid image binding; the output texture is one.
	RID debug_variance = p_params.debug_variance.is_valid() ? p_params.debug_variance : p_params.probe_radiance_out;
	RID debug_history = p_params.debug_history.is_valid() ? p_params.debug_history : p_params.probe_radiance_out;
	RID debug_rejected = p_params.debug_rejected.is_valid() ? p_params.debug_rejected : p_params.probe_radiance_out;

	RID linear_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID denoise_shader = shader.version_get_shader(shader_version, MODE_PROBE_TEMPORAL);

	RD::Uniform u_probe_radiance(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ linear_sampler, p_params.probe_radiance }));
	RD::Uniform u_probe_radiance_out(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ p_params.probe_radiance_out }));
	RD::Uniform u_probe_history(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ linear_sampler, p_params.probe_history }));
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ nearest_sampler, p_params.depth }));
	RD::Uniform u_normal_roughness(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest_sampler, p_params.normal_roughness }));
	RD::Uniform u_velocity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ nearest_sampler, p_params.velocity }));
	RD::Uniform u_reset_mask(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, p_params.reset_mask);
	RD::Uniform u_history_length(RD::UNIFORM_TYPE_IMAGE, 7, Vector<RID>({ p_params.history_length }));
	RD::Uniform u_temporal_params(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, temporal_params_buffer);
	RD::Uniform u_debug_variance(RD::UNIFORM_TYPE_IMAGE, 9, Vector<RID>({ debug_variance }));
	RD::Uniform u_debug_history(RD::UNIFORM_TYPE_IMAGE, 10, Vector<RID>({ debug_history }));
	RD::Uniform u_debug_rejected(RD::UNIFORM_TYPE_IMAGE, 11, Vector<RID>({ debug_rejected }));

	RID uniform_set = uniform_set_cache->get_cache(denoise_shader, 0, u_probe_radiance, u_probe_radiance_out, u_probe_history, u_depth, u_normal_roughness, u_velocity, u_reset_mask, u_history_length, u_temporal_params, u_debug_variance, u_debug_history, u_debug_rejected);
	ERR_FAIL_COND_MSG(uniform_set.is_null(), "NextGIDenoise: could not build the temporal uniform set.");

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_PROBE_TEMPORAL]);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(TemporalPushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_params.probe_size.width, p_params.probe_size.height, 1);
	RD::get_singleton()->compute_list_end();
}

void NextGIDenoise::process_stabilize(const StabilizeParams &p_params) {
	ERR_FAIL_COND_MSG(!p_params.current.is_valid(), "NextGIDenoise: current ambient is invalid.");
	ERR_FAIL_COND_MSG(!p_params.previous.is_valid(), "NextGIDenoise: previous ambient is invalid.");
	ERR_FAIL_COND_MSG(!p_params.output.is_valid(), "NextGIDenoise: ambient output is invalid.");

	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);

	StabilizePushConstant push_constant;
	push_constant.screen_size[0] = p_params.screen_size.width;
	push_constant.screen_size[1] = p_params.screen_size.height;
	push_constant.blend_weight = p_params.blend_weight;
	push_constant.stabilizer_enabled = p_params.stabilizer_enabled ? 1u : 0u;

	RID linear_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID denoise_shader = shader.version_get_shader(shader_version, MODE_STABILIZE);

	RD::Uniform u_current(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ linear_sampler, p_params.current }));
	RD::Uniform u_previous(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ linear_sampler, p_params.previous }));
	RD::Uniform u_output(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ p_params.output }));

	RID uniform_set = uniform_set_cache->get_cache(denoise_shader, 0, u_current, u_previous, u_output);
	ERR_FAIL_COND_MSG(uniform_set.is_null(), "NextGIDenoise: could not build the stabilize uniform set.");

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_STABILIZE]);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(StabilizePushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_params.screen_size.width, p_params.screen_size.height, 1);
	RD::get_singleton()->compute_list_end();
}

String NextGIDenoise::build_shader_source(int p_variant) {
	NextgiDenoiseDiffuseShaderRD shader_rd;
	Vector<String> defines;
	defines.push_back("\n#define MODE_PROBE_SPATIAL\n");
	defines.push_back("\n#define MODE_PROBE_TEMPORAL\n");
	defines.push_back("\n#define MODE_STABILIZE\n");
	shader_rd.initialize(defines);
	const RID version = shader_rd.version_create(false);
	ERR_FAIL_COND_V(!version.is_valid(), String());
	const Vector<String> sources = shader_rd.version_build_variant_stage_sources(version, CLAMP(p_variant, 0, int(MODE_MAX) - 1));
	shader_rd.version_free(version);
	if (sources.size() <= RenderingDevice::SHADER_STAGE_COMPUTE) {
		return String();
	}
	return sources[RenderingDevice::SHADER_STAGE_COMPUTE];
}

NextGIDenoise::RRHooks NextGIDenoise::parse_rr_hooks(const String &p_value) {
	RRHooks hooks;
	for (const String &entry : p_value.split(",")) {
		const String trimmed = entry.strip_edges();
		const int equal = trimmed.find("=");
		if (equal <= 0) {
			continue;
		}
		const String key = trimmed.substr(0, equal).strip_edges();
		const String value = trimmed.substr(equal + 1).strip_edges();
		if (key == "stabilizer") {
			if (value == "0") {
				hooks.stabilizer_enabled = false;
			} else if (value == "1") {
				hooks.stabilizer_enabled = true;
			}
		} else if (key == "probe_history" && value.is_valid_int()) {
			hooks.probe_temporal_max_history = uint32_t(CLAMP(value.to_int(), 0, 4096));
		} else if (key == "specular_filter") {
			if (value == "0") {
				hooks.specular_filter_enabled = false;
			} else if (value == "1") {
				hooks.specular_filter_enabled = true;
			}
		} else if (key == "sampling") {
			if (value == "white") {
				hooks.sampling_mode = 1;
			} else if (value == "blue") {
				hooks.sampling_mode = 0;
			}
		}
	}
	return hooks;
}

PackedByteArray NextGIDenoise::select_reset_probes(const Vector<Rect2i> &p_dirty_regions, const Size2i &p_probe_size) {
	const int width = MAX(p_probe_size.width, 0);
	const int height = MAX(p_probe_size.height, 0);

	PackedByteArray mask;
	mask.resize(width * height);
	mask.fill(0);
	if (width == 0 || height == 0) {
		return mask;
	}

	const Rect2i bounds(Point2i(), Size2i(width, height));
	uint8_t *mask_data = mask.ptrw();
	for (const Rect2i &region : p_dirty_regions) {
		const Rect2i clipped = region.intersection(bounds);
		for (int y = clipped.position.y; y < clipped.position.y + clipped.size.height; y++) {
			for (int x = clipped.position.x; x < clipped.position.x + clipped.size.width; x++) {
				mask_data[y * width + x] = 1;
			}
		}
	}
	return mask;
}
