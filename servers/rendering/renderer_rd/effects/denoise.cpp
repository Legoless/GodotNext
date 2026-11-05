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

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

NextGIDenoise::NextGIDenoise() {
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
}

NextGIDenoise::~NextGIDenoise() {
	for (int i = 0; i < MODE_MAX; i++) {
		RD::get_singleton()->free_rid(pipelines[i]);
	}
	RD::get_singleton()->free_rid(temporal_params_buffer);
	shader.version_free(shader_version);
	RD::get_singleton()->free_rid(specular_temporal);
	RD::get_singleton()->free_rid(specular_spatial);
	RD::get_singleton()->free_rid(specular_upsample);
	RD::get_singleton()->free_rid(specular_params_buffer);
	specular_shader.version_free(specular_version);
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
	} push_constant;
	push_constant.trace_size[0] = p_params.trace_size.width;
	push_constant.trace_size[1] = p_params.trace_size.height;
	push_constant.screen_size[0] = p_params.screen_size.width;
	push_constant.screen_size[1] = p_params.screen_size.height;
	push_constant.depth_tolerance = 0.1f;
	push_constant.normal_tolerance = 0.9f;
	push_constant.velocity_enabled = p_params.velocity_enabled ? 1u : 0u;
	push_constant.history_cap_min = p_params.history_cap_min;

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
	RID uniform_set = uniform_set_cache->get_cache(shader_rid, 0, u_raw, u_history, u_depth, u_normal_roughness, u_velocity, u_hit_distance, u_output, u_history_length, u_params);
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
		gpu_params.inv_projection[i] = p_params.inv_projection.columns[i / 4][i % 4];
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
