/**************************************************************************/
/*  next_gi_gather.cpp                                                    */
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

#include "next_gi_gather.h"

#include "core/config/project_settings.h"
#include "core/math/projection.h"
#include "servers/rendering/renderer_rd/effects/ss_effects.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_reference.h"
#include "servers/rendering/renderer_rd/environment/next_gi_relight.h"
#include "servers/rendering/renderer_rd/environment/next_gi_src.h"
#include "servers/rendering/renderer_rd/environment/next_gi_wrc.h"
#include "servers/rendering/renderer_rd/environment/nextgi_blue_noise.h"
#include "servers/rendering/renderer_rd/environment/sky.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_gather.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/rendering_device.h"

#include <cstddef>

using namespace RendererRD;

NextGIGather *NextGIGather::singleton = nullptr;

static uint64_t nextgi_coverage_hits = 0;
static uint64_t nextgi_coverage_rays = 0;

uint64_t RendererRD::nextgi_gather_coverage_hits() {
	return nextgi_coverage_hits;
}

uint64_t RendererRD::nextgi_gather_coverage_rays() {
	return nextgi_coverage_rays;
}

struct GatherParamsGPU {
	float inv_view_proj[16];
	float prev_view_proj[16];
	float camera_pos[4];
	float prev_camera_pos[4];
	float ambient_color[4];
	float screen_jitter[4];
	uint32_t frame;
	uint32_t probes_x;
	uint32_t probes_y;
	float rt_radius;
	float max_origin_offset;
	uint32_t sky_enabled;
	uint32_t feedback_count;
	uint32_t feedback_stride;
	uint32_t record_count;
	uint32_t velocity_enabled;
	uint32_t page_rect_count;
	uint32_t pad0;
	uint32_t pad1;
	uint32_t pad2;
	float max_trace_roughness;
	float reflect_divisor;
	uint32_t reflect_enabled;
	uint32_t reflect_debug;
	uint32_t sampling_mode;
	uint32_t pad4;
};
static_assert(sizeof(GatherParamsGPU) == 272, "Gather params must match nextgi_gather.glsl.");
static_assert(offsetof(GatherParamsGPU, record_count) == 224, "record_count must follow feedback_stride.");
static_assert(offsetof(GatherParamsGPU, page_rect_count) == 232, "page_rect_count must follow velocity_enabled.");

struct ReflectCountersGPU {
	uint32_t traced = 0;
	uint32_t hits = 0;
	uint32_t misses = 0;
	uint32_t pad = 0;
};

struct GatherCountersGPU {
	uint32_t rays = 0;
	uint32_t misses = 0;
	uint32_t history_resets = 0;
	uint32_t uncovered = 0;
	uint32_t coverage_hits = 0;
	uint32_t coverage_rays = 0;
	uint32_t miss_radiance_bits = 0;
	uint32_t wrc_samples = 0;
	uint32_t sky_samples = 0;
	uint32_t screen_hits = 0;
};

struct NextGIGather::ShaderState {
	NextgiGatherShaderRD shader;
	RID version;
	RID pipelines[6];
};

static void _store_projection(const Projection &p_projection, float r_matrix[16]) {
	for (int column = 0; column < 4; column++) {
		for (int row = 0; row < 4; row++) {
			r_matrix[column * 4 + row] = p_projection.columns[column][row];
		}
	}
}

static float _radical_inverse(uint32_t p_bits) {
	p_bits = (p_bits << 16u) | (p_bits >> 16u);
	p_bits = ((p_bits & 0x55555555u) << 1u) | ((p_bits & 0xAAAAAAAAu) >> 1u);
	p_bits = ((p_bits & 0x33333333u) << 2u) | ((p_bits & 0xCCCCCCCCu) >> 2u);
	p_bits = ((p_bits & 0x0F0F0F0Fu) << 4u) | ((p_bits & 0xF0F0F0F0u) >> 4u);
	p_bits = ((p_bits & 0x00FF00FFu) << 8u) | ((p_bits & 0xFF00FF00u) >> 8u);
	return float(p_bits) * 2.3283064365386963e-10f;
}

NextGIGather::NextGIGather() {
	if (singleton == nullptr) {
		singleton = this;
	}
}

NextGIGather::~NextGIGather() {
	free_resources();
	if (singleton == this) {
		singleton = nullptr;
	}
}

void NextGIGather::_free_uniform_set() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr) {
		if (uniform_set.is_valid()) {
			rd->free_rid(uniform_set);
		}
		if (reflect_uniform_set.is_valid()) {
			rd->free_rid(reflect_uniform_set);
		}
	}
	uniform_set = RID();
	reflect_uniform_set = RID();
}

void NextGIGather::free_resources() {
	_free_uniform_set();
	if (denoise != nullptr) {
		memdelete(denoise);
		denoise = nullptr;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (shader != nullptr) {
		if (rd != nullptr) {
			for (int i = 0; i < 4; i++) {
				if (shader->pipelines[i].is_valid()) {
					rd->free_rid(shader->pipelines[i]);
				}
			}
		}
		if (shader->version.is_valid()) {
			shader->shader.version_free(shader->version);
		}
		memdelete(shader);
		shader = nullptr;
	}
	if (rd == nullptr) {
		return;
	}
	RID resources[29] = { blue_noise_texture, specular_debug_variance, specular_debug_rejected, probe_image, probe_scratch, probe_history, history_length, debug_raw, debug_variance, debug_history, debug_rejected, reflect_counter_buffer, specular_debug, clamp_totals_buffer, specular_raw, specular_history, specular_scratch, specular_denoised, specular_history_length, reset_mask_buffer, ambient_scratch, prev_ambient, params_buffer, counter_buffer, feedback_buffer, confidence_buffer, dummy_color, dummy_storage, dummy_buffer };
	for (RID resource : resources) {
		if (resource.is_valid()) {
			rd->free_rid(resource);
		}
	}
	if (sampler.is_valid()) {
		rd->free_rid(sampler);
	}
	if (sampler_nearest.is_valid()) {
		rd->free_rid(sampler_nearest);
	}
	probe_image = RID();
	debug_raw = RID();
	debug_variance = RID();
	debug_history = RID();
	debug_rejected = RID();
	reflect_counter_buffer = RID();
	specular_debug = RID();
	clamp_totals_buffer = RID();
	specular_raw = RID();
	specular_history = RID();
	specular_scratch = RID();
	specular_denoised = RID();
	specular_history_length = RID();
	specular_debug_variance = RID();
	specular_debug_rejected = RID();
	specular_final = RID();
	blue_noise_texture = RID();
	params_buffer = RID();
	counter_buffer = RID();
	feedback_buffer = RID();
	confidence_buffer = RID();
	dummy_color = RID();
	dummy_storage = RID();
	dummy_buffer = RID();
	sampler = RID();
	sampler_nearest = RID();
}

bool NextGIGather::_ensure_shader() {
	if (shader != nullptr && shader->version.is_valid()) {
		return true;
	}
	NextGI *next_gi = NextGI::get_singleton();
	ERR_FAIL_NULL_V(next_gi, false);
	ERR_FAIL_COND_V(!next_gi->can_initialize_ray_query_shaders(), false);
	shader = memnew(ShaderState);
	Vector<String> defines;
	defines.push_back("");
	defines.push_back("#define MODE_INTEGRATE\n");
	defines.push_back("#define MODE_SRC_FEEDBACK\n");
	defines.push_back("#define MODE_COVERAGE\n");
	defines.push_back("#define MODE_HIT_RADIANCE_REFERENCE\n");
	defines.push_back("#define MODE_REFLECT\n");
	shader->shader.initialize(defines);
	// GodotNext: shader_baker_ray_query
	// Embedded so the shader baker bakes or skips the ray-query version per target (#40).
	shader->version = shader->shader.version_create(true);
	if (!shader->version.is_valid()) {
		memdelete(shader);
		shader = nullptr;
		ERR_FAIL_V_MSG(false, "The NextGI gather shader failed to compile.");
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	for (int i = 0; i < 6; i++) {
		shader->pipelines[i] = rd->compute_pipeline_create(shader->shader.version_get_shader(shader->version, i));
		if (!shader->pipelines[i].is_valid()) {
			shader->shader.version_free(shader->version);
			memdelete(shader);
			shader = nullptr;
			ERR_FAIL_V_MSG(false, "The NextGI gather pipeline failed to compile.");
		}
	}
	next_gi->notify_ray_query_shader_initialized();
	return true;
}

bool NextGIGather::_ensure_targets(uint32_t p_screen_w, uint32_t p_screen_h) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	const uint32_t probes_x = nextgi_gather_probe_count_axis(p_screen_w);
	const uint32_t probes_y = nextgi_gather_probe_count_axis(p_screen_h);
	if (!sampler.is_valid()) {
		RD::SamplerState state;
		state.mag_filter = RD::SAMPLER_FILTER_LINEAR;
		state.min_filter = RD::SAMPLER_FILTER_LINEAR;
		sampler = rd->sampler_create(state);
		state.mag_filter = RD::SAMPLER_FILTER_NEAREST;
		state.min_filter = RD::SAMPLER_FILTER_NEAREST;
		sampler_nearest = rd->sampler_create(state);
	}
	if (!dummy_color.is_valid()) {
		RD::TextureFormat tf;
		tf.width = 1;
		tf.height = 1;
		tf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		dummy_color = rd->texture_create(tf, RD::TextureView());
		rd->texture_clear(dummy_color, Color(0, 0, 0, 1), 0, 1, 0, 1);
	}
	if (!dummy_storage.is_valid()) {
		RD::TextureFormat tf;
		tf.width = 1;
		tf.height = 1;
		tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		dummy_storage = rd->texture_create(tf, RD::TextureView());
		rd->texture_clear(dummy_storage, Color(0, 0, 0, 0), 0, 1, 0, 1);
	}
	if (!dummy_buffer.is_valid()) {
		dummy_buffer = rd->storage_buffer_create(16);
	}
	if (!params_buffer.is_valid()) {
		params_buffer = rd->storage_buffer_create(sizeof(GatherParamsGPU));
	}
	if (!counter_buffer.is_valid()) {
		counter_buffer = rd->storage_buffer_create(sizeof(GatherCountersGPU));
	}
	if (!ambient_scratch.is_valid() || ambient_w != p_screen_w || ambient_h != p_screen_h) {
		if (ambient_scratch.is_valid()) {
			rd->free_rid(ambient_scratch);
			ambient_scratch = RID();
		}
		if (prev_ambient.is_valid()) {
			rd->free_rid(prev_ambient);
			prev_ambient = RID();
		}
		RD::TextureFormat ambient_tf;
		ambient_tf.width = MAX(p_screen_w, 1u);
		ambient_tf.height = MAX(p_screen_h, 1u);
		ambient_tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		ambient_tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		ambient_scratch = rd->texture_create(ambient_tf, RD::TextureView());
		ERR_FAIL_COND_V(ambient_scratch.is_null(), false);
		rd->texture_clear(ambient_scratch, Color(0, 0, 0, 0), 0, 1, 0, 1);
		prev_ambient = rd->texture_create(ambient_tf, RD::TextureView());
		ERR_FAIL_COND_V(prev_ambient.is_null(), false);
		rd->texture_clear(prev_ambient, Color(0, 0, 0, 0), 0, 1, 0, 1);
		ambient_w = p_screen_w;
		ambient_h = p_screen_h;
		ambient_history_valid = false;
	}

	if (probe_image.is_valid() && probe_w == probes_x && probe_h == probes_y) {
		if (!confidence_buffer.is_valid()) {
			confidence_buffer = rd->storage_buffer_create(sizeof(float) * MAX(probes_x * probes_y, 1u));
		}
		return probe_image.is_valid() && confidence_buffer.is_valid();
	}
	_free_uniform_set();
	if (probe_image.is_valid()) {
		rd->free_rid(probe_image);
	}
	if (probe_scratch.is_valid()) {
		rd->free_rid(probe_scratch);
		probe_scratch = RID();
	}
	if (probe_history.is_valid()) {
		rd->free_rid(probe_history);
		probe_history = RID();
	}
	if (history_length.is_valid()) {
		rd->free_rid(history_length);
		history_length = RID();
	}
	if (debug_raw.is_valid()) {
		rd->free_rid(debug_raw);
		debug_raw = RID();
	}
	if (debug_variance.is_valid()) {
		rd->free_rid(debug_variance);
		debug_variance = RID();
	}
	if (debug_history.is_valid()) {
		rd->free_rid(debug_history);
		debug_history = RID();
	}
	if (debug_rejected.is_valid()) {
		rd->free_rid(debug_rejected);
		debug_rejected = RID();
	}
	if (reset_mask_buffer.is_valid()) {
		rd->free_rid(reset_mask_buffer);
		reset_mask_buffer = RID();
	}
	history_valid = false;
	if (confidence_buffer.is_valid()) {
		rd->free_rid(confidence_buffer);
		confidence_buffer = RID();
	}
	RD::TextureFormat tf;
	tf.width = MAX(probes_x * 4, 1u);
	tf.height = MAX(probes_y * 4, 1u);
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	// GodotNext: the diffuse denoiser reads the probe texels as a sampler and writes the scratch copy.
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	probe_image = rd->texture_create(tf, RD::TextureView());
	ERR_FAIL_COND_V(probe_image.is_null(), false);
	rd->texture_clear(probe_image, Color(0, 0, 0, 0), 0, 1, 0, 1);
	probe_scratch = rd->texture_create(tf, RD::TextureView());
	ERR_FAIL_COND_V(probe_scratch.is_null(), false);
	rd->texture_clear(probe_scratch, Color(0, 0, 0, 0), 0, 1, 0, 1);
	probe_history = rd->texture_create(tf, RD::TextureView());
	ERR_FAIL_COND_V(probe_history.is_null(), false);
	rd->texture_clear(probe_history, Color(0, 0, 0, 0), 0, 1, 0, 1);

	RD::TextureFormat tf_length;
	tf_length.width = MAX(probes_x, 1u);
	tf_length.height = MAX(probes_y, 1u);
	tf_length.format = RD::DATA_FORMAT_R8_UINT;
	tf_length.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT;
	history_length = rd->texture_create(tf_length, RD::TextureView());
	ERR_FAIL_COND_V(history_length.is_null(), false);

	// No per-probe dirty AABB list exists yet, so the mask starts clear. select_reset_probes()
	// already turns such a list into the mask the temporal pass reads.
	const PackedByteArray reset_mask = NextGIDenoise::select_reset_probes(Vector<Rect2i>(), Size2i(probes_x, probes_y));
	Vector<uint32_t> reset_values;
	reset_values.resize(reset_mask.size());
	for (int i = 0; i < reset_mask.size(); i++) {
		reset_values.write[i] = reset_mask[i] != 0 ? 1u : 0u;
	}
	reset_mask_buffer = rd->storage_buffer_create(MAX(uint32_t(reset_values.size()), 1u) * sizeof(uint32_t));
	ERR_FAIL_COND_V(reset_mask_buffer.is_null(), false);
	if (!reset_values.is_empty()) {
		rd->buffer_update(reset_mask_buffer, 0, reset_values.size() * sizeof(uint32_t), reset_values.ptr());
	}
	confidence_buffer = rd->storage_buffer_create(sizeof(float) * MAX(probes_x * probes_y, 1u));
	probe_w = probes_x;
	probe_h = probes_y;
	return true;
}

void NextGIGather::init() {
	denoise = memnew(NextGIDenoise);
#ifdef DEBUG_ENABLED
	// GodotNext: nextgi_rr_hooks
	if (OS::get_singleton() != nullptr) {
		const NextGIDenoise::RRHooks hooks = NextGIDenoise::parse_rr_hooks(OS::get_singleton()->get_environment("GODOT_NEXTGI_RR_HOOKS"));
		rr_stabilizer_enabled = hooks.stabilizer_enabled;
		rr_probe_history_cap = hooks.probe_temporal_max_history;
		rr_specular_filter_enabled = hooks.specular_filter_enabled;
		rr_sampling_mode = uint32_t(hooks.sampling_mode);
	}
#endif
}

bool NextGIGather::_bind(RenderDataRD *p_render_data, uint32_t p_feedback_count, uint32_t p_feedback_stride) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	NextGI *next_gi = NextGI::get_singleton();
	NextGIRelight *relight = NextGIRelight::get_singleton();
	NextGISRC *src = NextGISRC::get_singleton();
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	RendererSceneRenderRD *scene = RendererSceneRenderRD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	ERR_FAIL_NULL_V(p_render_data, false);
	ERR_FAIL_NULL_V(next_gi, false);
	ERR_FAIL_NULL_V(next_gi->get_rt_scene(), false);
	ERR_FAIL_NULL_V(relight, false);
	ERR_FAIL_NULL_V(src, false);
	ERR_FAIL_NULL_V(scene, false);
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	ERR_FAIL_COND_V(rb.is_null(), false);
	const Size2i screen = rb->get_internal_size();
	last_screen_w = screen.x;
	last_screen_h = screen.y;
	if (!_ensure_shader() || !_ensure_targets(screen.x, screen.y)) {
		return false;
	}
	const RID tlas = next_gi->get_rt_scene()->get_tlas();
	const RID depth = rb->get_depth_texture();
	// A frame that did not rebuild the TLAS still holds the previous build, which a freed mesh invalidates.
	if (!tlas.is_valid() || !next_gi->get_rt_scene()->is_tlas_traceable() || !depth.is_valid() || !rb->has_texture(RB_SCOPE_GI, RB_TEX_AMBIENT)) {
		return false;
	}

	RID chunks[4] = { dummy_buffer, dummy_buffer, dummy_buffer, dummy_buffer };
	if (mesh_storage != nullptr) {
		const uint32_t chunk_count = MIN(mesh_storage->mesh_nextgi_rt_get_chunk_count(), 4u);
		for (uint32_t i = 0; i < chunk_count; i++) {
			const RID chunk = mesh_storage->mesh_nextgi_rt_get_chunk_buffer(i);
			if (chunk.is_valid()) {
				chunks[i] = chunk;
			}
		}
	}
	RID sky_tex = dummy_color;
	uint32_t sky_enabled = 0;
	Color ambient(0, 0, 0, 1);
	if (p_render_data->environment.is_valid()) {
		ambient = scene->environment_get_ambient_light(p_render_data->environment);
		ambient *= scene->environment_get_ambient_light_energy(p_render_data->environment);
		const RID sky = scene->environment_get_sky(p_render_data->environment);
		SkyRD *sky_rd = scene->get_sky();
		if (sky.is_valid() && sky_rd != nullptr) {
			const RID radiance = sky_rd->sky_get_radiance_texture_2d(sky);
			if (radiance.is_valid()) {
				sky_tex = radiance;
				sky_enabled = 1;
			}
		}
	}
	RenderSceneDataRD *scene_data = p_render_data->scene_data;
	ERR_FAIL_NULL_V(scene_data, false);
	const Projection view_proj = scene_data->cam_projection * Projection(scene_data->cam_transform.affine_inverse());
	last_inv_view_proj = view_proj.inverse();
	// prev_view_projection is the projection only. Match the current matrix, including the previous camera.
	const Projection prev_view_proj = scene_data->prev_cam_projection * Projection(scene_data->prev_cam_transform.affine_inverse());
	GatherParamsGPU params;
	_store_projection(view_proj.inverse(), params.inv_view_proj);
	_store_projection(prev_view_proj, params.prev_view_proj);
	last_prev_view_proj = prev_view_proj;
	last_camera_pos = scene_data->cam_transform.origin;
	last_prev_camera_pos = scene_data->prev_cam_transform.origin;
	params.camera_pos[0] = scene_data->cam_transform.origin.x;
	params.camera_pos[1] = scene_data->cam_transform.origin.y;
	params.camera_pos[2] = scene_data->cam_transform.origin.z;
	params.prev_camera_pos[0] = scene_data->prev_cam_transform.origin.x;
	params.prev_camera_pos[1] = scene_data->prev_cam_transform.origin.y;
	params.prev_camera_pos[2] = scene_data->prev_cam_transform.origin.z;
	params.ambient_color[0] = ambient.r;
	params.ambient_color[1] = ambient.g;
	params.ambient_color[2] = ambient.b;
	params.ambient_color[3] = 1.0f;
	params.screen_jitter[0] = float(MAX(screen.x, 1));
	params.screen_jitter[1] = float(MAX(screen.y, 1));
	params.screen_jitter[2] = float(frame % 256) / 256.0f;
	params.screen_jitter[3] = _radical_inverse(frame % 256);
	params.frame = frame;
	params.probes_x = nextgi_gather_probe_count_axis(screen.x);
	params.probes_y = nextgi_gather_probe_count_axis(screen.y);
	const float rt_radius = float(GLOBAL_GET("rendering/global_illumination/nextgi/rt_scene_radius"));
	params.rt_radius = rt_radius > 0.0f ? rt_radius : 1.0f;
	params.max_origin_offset = MAX(relight->get_max_origin_offset(), 0.001f);
	params.sky_enabled = sky_enabled;
	params.feedback_count = p_feedback_count;
	params.feedback_stride = MAX(p_feedback_stride, 1u);
	params.record_count = next_gi->get_rt_scene()->get_stats().instance_count;
	params.page_rect_count = relight->get_page_rect_count();
	const NextGI::QualityParams &quality = NextGI::get_quality_params(next_gi->get_quality_setting());
	params.max_trace_roughness = quality.max_trace_roughness;
	params.reflect_divisor = float(quality.reflection_divisor);
	params.reflect_enabled = 1u;
	params.reflect_debug = next_gi->get_active_debug_view() == NextGI::DEBUG_VIEW_SPECULAR_HIT ? 1u : 0u;
	params.sampling_mode = rr_sampling_mode;
	params.pad4 = 0u;
	RID hiz_texture = dummy_color;
	RID last_frame_texture = dummy_color;
	if (rb.is_valid() && rb->has_texture(RB_SCOPE_SSR, RB_HIZ) && rb->has_texture(RB_SCOPE_SSLF, RB_LAST_FRAME)) {
		const RD::TextureFormat hiz_format = rd->texture_get_format(rb->get_texture(RB_SCOPE_SSR, RB_HIZ));
		const RD::TextureFormat color_format = rd->texture_get_format(rb->get_texture(RB_SCOPE_SSLF, RB_LAST_FRAME));
		hiz_texture = rb->get_texture_slice(RB_SCOPE_SSR, RB_HIZ, 0, 0, 1, hiz_format.mipmaps);
		last_frame_texture = rb->get_texture_slice(RB_SCOPE_SSLF, RB_LAST_FRAME, 0, 0, 1, color_format.mipmaps);
		params.pad0 = hiz_texture.is_valid() && last_frame_texture.is_valid() ? 1u : 0u;
		params.pad1 = hiz_format.mipmaps > 0 ? hiz_format.mipmaps - 1 : 0;
	}
	RID normal_roughness = dummy_color;
	RID velocity = dummy_color;
	last_normal_roughness = RID();
	if (rb->has_texture(SNAME("forward_clustered"), SNAME("normal_roughness")) && rb->has_velocity_buffer(false)) {
		normal_roughness = rb->get_texture(SNAME("forward_clustered"), SNAME("normal_roughness"));
		velocity = rb->get_velocity_buffer(false);
		params.velocity_enabled = velocity.is_valid() && normal_roughness.is_valid() ? 1u : 0u;
		last_normal_roughness = normal_roughness;
	} else {
		params.velocity_enabled = 0;
	}
	last_depth = depth;
	last_inv_projection = scene_data->cam_projection.inverse();
	last_velocity = velocity.is_valid() ? velocity : RID();
	rd->buffer_update(params_buffer, 0, sizeof(params), &params);

	_free_uniform_set();
	RID page_rects = relight->get_page_rect_buffer();
	if (!page_rects.is_valid()) {
		page_rects = dummy_buffer;
	}
	RID surfaces = next_gi->get_rt_scene()->get_surface_record_buffer();
	if (!surfaces.is_valid()) {
		surfaces = dummy_buffer;
	}
	RID records = next_gi->get_rt_scene()->get_instance_record_buffer();
	if (!records.is_valid()) {
		records = dummy_buffer;
	}
	RID cards = src->get_card_buffer();
	RID card_sets = src->get_card_set_buffer();
	if (!cards.is_valid()) {
		cards = dummy_buffer;
	}
	if (!card_sets.is_valid()) {
		card_sets = dummy_buffer;
	}
	if (!feedback_buffer.is_valid()) {
		feedback_buffer = rd->storage_buffer_create(sizeof(uint32_t) * 4);
	}
	RID indirect = relight->get_indirect_texture();
	if (!indirect.is_valid()) {
		indirect = dummy_storage;
	}
	RID radiance = relight->get_radiance_texture();
	if (!radiance.is_valid()) {
		radiance = dummy_color;
	}
	RID material_depth = src->get_atlas_texture(NextGISRC::ATLAS_DEPTH);
	if (!material_depth.is_valid()) {
		material_depth = dummy_color;
	}
	hit_radiance_reference = false;
	RID ref_materials = dummy_buffer;
	RID ref_lights = dummy_buffer;
	RID ref_params = dummy_buffer;
	if (int(GLOBAL_GET("rendering/global_illumination/nextgi/debug/gather_hit_radiance")) == 1 && next_gi->get_reference() != nullptr) {
		RID materials;
		RID lights;
		RID estimator;
		if (next_gi->get_reference()->prepare_estimator(p_render_data, materials, lights, estimator)) {
			ref_materials = materials;
			ref_lights = lights;
			ref_params = estimator;
			hit_radiance_reference = true;
		}
	}
	NextGIWRC *wrc = next_gi->get_wrc();
	RID wrc_radiance = dummy_color;
	RID wrc_visibility = dummy_color;
	RID wrc_params = dummy_buffer;
	if (wrc != nullptr && wrc->is_ready()) {
		if (wrc->get_radiance().is_valid()) {
			wrc_radiance = wrc->get_radiance();
		}
		if (wrc->get_visibility().is_valid()) {
			wrc_visibility = wrc->get_visibility();
		}
		if (wrc->get_params_buffer().is_valid()) {
			wrc_params = wrc->get_params_buffer();
		}
	}
	if (!reflect_counter_buffer.is_valid()) {
		reflect_counter_buffer = rd->storage_buffer_create(sizeof(ReflectCountersGPU));
	}
	if (!clamp_totals_buffer.is_valid()) {
		clamp_totals_buffer = rd->storage_buffer_create(sizeof(uint32_t) * 2);
	}
	if (!blue_noise_texture.is_valid()) {
		RD::TextureFormat noise_format;
		noise_format.width = 64;
		noise_format.height = 64;
		noise_format.format = RD::DATA_FORMAT_R8G8_UNORM;
		noise_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		Vector<Vector<uint8_t>> noise_data;
		Vector<uint8_t> noise;
		noise.resize(64 * 64 * 2);
		memcpy(noise.ptrw(), nextgi_blue_noise_tile, 64 * 64 * 2);
		noise_data.push_back(noise);
		blue_noise_texture = rd->texture_create(noise_format, RD::TextureView(), noise_data);
	}
	last_reflection_output = rb->has_texture(RB_SCOPE_GI, RB_TEX_REFLECTION) ? rb->get_texture(RB_SCOPE_GI, RB_TEX_REFLECTION) : RID();
	last_specular_hit = rb->has_texture(RB_SCOPE_GI, RB_TEX_SPECULAR_HIT) ? rb->get_texture(RB_SCOPE_GI, RB_TEX_SPECULAR_HIT) : RID();
	if (last_screen_w > 0 && last_screen_h > 0) {
		const uint32_t trace_w = MAX(last_screen_w / 2u, 1u);
		const uint32_t trace_h = MAX(last_screen_h / 2u, 1u);
		const RD::TextureFormat specular_format = rd->texture_get_format(specular_raw.is_valid() ? specular_raw : probe_image);
		if (!specular_raw.is_valid() || specular_format.width != trace_w || specular_format.height != trace_h) {
			RID *textures[7] = { &specular_raw, &specular_history, &specular_scratch, &specular_denoised, &specular_history_length, &specular_debug_variance, &specular_debug_rejected };
			for (RID *texture : textures) {
				if (texture->is_valid()) {
					rd->free_rid(*texture);
				}
			}
			RD::TextureFormat trace_format;
			trace_format.width = trace_w;
			trace_format.height = trace_h;
			trace_format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
			trace_format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
			for (RID *texture : textures) {
				*texture = rd->texture_create(trace_format, RD::TextureView());
				rd->texture_clear(*texture, Color(0, 0, 0, 0), 0, 1, 0, 1);
			}
		}
	}
	if (params.reflect_debug != 0u && !specular_debug.is_valid()) {
		RD::TextureFormat debug_tf;
		debug_tf.width = MAX(last_screen_w / 2u, 1u);
		debug_tf.height = MAX(last_screen_h / 2u, 1u);
		debug_tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		debug_tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		specular_debug = rd->texture_create(debug_tf, RD::TextureView());
	}
	RID confidence = confidence_buffer.is_valid() ? confidence_buffer : dummy_buffer;
	RD::Uniform uniforms[39] = {
		RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, tlas),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler_nearest, depth })),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, radiance })),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ sampler_nearest, material_depth })),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ sampler, sky_tex })),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ sampler, indirect })),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, cards),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, card_sets),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, records),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, surfaces),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, chunks[0]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, chunks[1]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, chunks[2]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, chunks[3]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, page_rects),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 15, probe_image),
		// GodotNext: nextgi_rr_hooks — with the stabilizer off, the integration writes ambient directly.
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 16, rr_stabilizer_enabled ? ambient_scratch : rb->get_texture(RB_SCOPE_GI, RB_TEX_AMBIENT)),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 17, indirect),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 18, params_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 19, counter_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 20, feedback_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 23, ref_materials),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 24, ref_lights),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 25, ref_params),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 26, Vector<RID>({ sampler, wrc_radiance })),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 27, Vector<RID>({ sampler, wrc_visibility })),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 28, wrc_params),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 29, confidence),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 30, Vector<RID>({ sampler_nearest, hiz_texture })),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 31, Vector<RID>({ sampler, last_frame_texture })),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 32, Vector<RID>({ sampler_nearest, normal_roughness })),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 33, Vector<RID>({ specular_raw.is_valid() ? specular_raw : probe_image })),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 34, reflect_counter_buffer.is_valid() ? reflect_counter_buffer : dummy_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 35, Vector<RID>({ rb->has_texture(RB_SCOPE_GI, RB_TEX_SPECULAR_HIT) ? rb->get_texture(RB_SCOPE_GI, RB_TEX_SPECULAR_HIT) : probe_image })),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 36, Vector<RID>({ specular_debug.is_valid() ? specular_debug : probe_image })),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 37, clamp_totals_buffer.is_valid() ? clamp_totals_buffer : dummy_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 38, Vector<RID>({ sampler_nearest, blue_noise_texture.is_valid() ? blue_noise_texture : dummy_color })),
	};
	const RID shader_rid = shader->shader.version_get_shader(shader->version, 0);
	uniform_set = rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 37), shader_rid, 0);
	// GodotNext: nextgi_reflect (#60). The reflect variant takes the same set plus the reflection image.
	const RID reflect_shader_rid = shader->shader.version_get_shader(shader->version, 5);
	reflect_uniform_set = rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 37), reflect_shader_rid, 0);
	return uniform_set.is_valid();
}

void NextGIGather::_read_counters() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr || !counter_buffer.is_valid()) {
		return;
	}
	if (clamp_totals_buffer.is_valid()) {
		const Vector<uint8_t> clamp_data = rd->buffer_get_data(clamp_totals_buffer);
		if (clamp_data.size() >= 8) {
			uint32_t values[2] = { 0u, 0u };
			memcpy(values, clamp_data.ptr(), sizeof(values));
			last_clamp_removed = values[0];
			last_clamp_total = values[1];
		}
	}
	if (reflect_counter_buffer.is_valid()) {
		const Vector<uint8_t> reflect_data = rd->buffer_get_data(reflect_counter_buffer);
		if (reflect_data.size() >= int(sizeof(ReflectCountersGPU))) {
			ReflectCountersGPU reflect_got;
			memcpy(&reflect_got, reflect_data.ptr(), sizeof(ReflectCountersGPU));
			last_reflection_traced = reflect_got.traced;
			last_reflection_hits = reflect_got.hits;
			last_reflection_misses = reflect_got.misses;
		}
	}
	const Vector<uint8_t> data = rd->buffer_get_data(counter_buffer);
	if (data.size() < int(sizeof(GatherCountersGPU))) {
		return;
	}
	GatherCountersGPU got;
	memcpy(&got, data.ptr(), sizeof(got));
	last_rays = got.rays;
	last_misses = got.misses;
	last_resets = got.history_resets;
	last_uncovered = got.uncovered;
	last_coverage_hits = got.coverage_hits;
	last_coverage_rays = got.coverage_rays;
	last_miss_luminance = float(got.miss_radiance_bits) / 64.0f;
	last_wrc_samples = got.wrc_samples;
	last_sky_samples = got.sky_samples;
	last_screen_hits = got.screen_hits;
	nextgi_coverage_hits = got.coverage_hits;
	nextgi_coverage_rays = got.coverage_rays;
}

void NextGIGather::gather(RenderDataRD *p_render_data) {
	last_rays = 0;
	last_misses = 0;
	last_resets = 0;
	last_uncovered = 0;
	NextGIRelight *relight = NextGIRelight::get_singleton();
	LocalVector<uint32_t> feedback;
	if (relight != nullptr) {
		for (const NextGIRelight::ScheduledPage &page : relight->get_scheduled_pages()) {
			feedback.push_back(page.indirect_x | (page.indirect_y << 16));
			feedback.push_back(page.record_index);
			feedback.push_back(page.card_index);
			feedback.push_back(0);
		}
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	// Drop the previous set before replacing the buffer it may still reference.
	_free_uniform_set();
	if (rd != nullptr && !feedback.is_empty()) {
		if (feedback_buffer.is_valid()) {
			rd->free_rid(feedback_buffer);
			feedback_buffer = RID();
		}
		feedback_buffer = rd->storage_buffer_create(feedback.size() * sizeof(uint32_t));
		rd->buffer_update(feedback_buffer, 0, feedback.size() * sizeof(uint32_t), feedback.ptr());
	}
	const uint32_t feedback_count = feedback.size() / 4;
	// local_size is 8x8. Stride matches local_size_x so gid.x + gid.y * stride covers the list once.
	const uint32_t stride = 8u;
	if (!_bind(p_render_data, feedback_count, stride)) {
		return;
	}
	GatherCountersGPU zero;
	rd->buffer_update(counter_buffer, 0, sizeof(zero), &zero);
	const uint32_t clamp_zero[2] = { 0u, 0u };
	rd->buffer_update(clamp_totals_buffer, 0, sizeof(clamp_zero), clamp_zero);
	const uint32_t groups_x = MAX((nextgi_gather_probe_count_axis(last_screen_w) + 7u) / 8u, 1u);
	const uint32_t groups_y = MAX((nextgi_gather_probe_count_axis(last_screen_h) + 7u) / 8u, 1u);
	{
		const RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, shader->pipelines[hit_radiance_reference ? 4 : 0]);
		rd->compute_list_bind_uniform_set(list, uniform_set, 0);
		rd->compute_list_dispatch(list, groups_x, groups_y, 1);
		rd->compute_list_end();
	}
	if (feedback_count > 0) {
		const uint32_t feedback_groups_y = MAX((feedback_count + 63u) / 64u, 1u);
		const RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, shader->pipelines[2]);
		rd->compute_list_bind_uniform_set(list, uniform_set, 0);
		rd->compute_list_dispatch(list, 1, feedback_groups_y, 1);
		rd->compute_list_end();
	}
	if (bool(GLOBAL_GET("rendering/global_illumination/nextgi/debug/coverage_counter"))) {
		const uint32_t half_x = MAX((last_screen_w / 2u + 7u) / 8u, 1u);
		const uint32_t half_y = MAX((last_screen_h / 2u + 7u) / 8u, 1u);
		const RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, shader->pipelines[3]);
		rd->compute_list_bind_uniform_set(list, uniform_set, 0);
		rd->compute_list_dispatch(list, half_x, half_y, 1);
		rd->compute_list_end();
	}
	// GodotNext: nextgi_denoise
	// Spatial denoise of the fresh probe texels, between the gather and the ambient integration.
	// The temporal pass replaces the gather's own history blend when it lands (#53 step 4).
	_denoise_probes();
	// GodotNext: nextgi_reflect (#60). One specular trace per half-resolution pixel.
	if (reflect_uniform_set.is_valid() && last_screen_w > 0 && last_screen_h > 0) {
		ReflectCountersGPU zero_reflect;
		rd->buffer_update(reflect_counter_buffer, 0, sizeof(zero_reflect), &zero_reflect);
		const uint32_t width = MAX((last_screen_w + 1u) / 2u, 1u);
		const uint32_t height = MAX((last_screen_h + 1u) / 2u, 1u);
		const RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, shader->pipelines[5]);
		rd->compute_list_bind_uniform_set(list, reflect_uniform_set, 0);
		rd->compute_list_dispatch(list, MAX((width + 7u) / 8u, 1u), MAX((height + 7u) / 8u, 1u), 1);
		rd->compute_list_end();
	}
	// GodotNext: nextgi_denoise_specular (#54). Temporal accumulation, then a three-pass a-trous
	// spatial filter, then the joint-bilateral upsample into RB_TEX_REFLECTION.
	_denoise_specular();
	_read_counters();
	frame++;
}

void NextGIGather::_denoise_probes() {
	if (denoise == nullptr || probe_w == 0 || probe_h == 0 || !probe_image.is_valid() || !probe_scratch.is_valid()) {
		return;
	}
	if (!last_depth.is_valid() || !last_normal_roughness.is_valid()) {
		return;
	}

	RenderingDevice *rd = RenderingDevice::get_singleton();
	NextGI *next_gi = NextGI::get_singleton();
	const int debug_view = next_gi != nullptr ? int(next_gi->get_active_debug_view()) : -1;
	const bool raw_view = debug_view == int(NextGI::DEBUG_VIEW_DENOISE_RAW);
	const bool variance_view = debug_view == int(NextGI::DEBUG_VIEW_DENOISE_VARIANCE);
	const bool history_view = debug_view == int(NextGI::DEBUG_VIEW_DENOISE_HISTORY);
	const bool rejected_view = debug_view == int(NextGI::DEBUG_VIEW_DENOISE_REJECTED);
	if ((raw_view || variance_view || history_view || rejected_view) && _ensure_debug_textures()) {
		if (raw_view) {
			const Vector3i extent(probe_w * 4, probe_h * 4, 1);
			rd->texture_copy(probe_image, debug_raw, Vector3(), Vector3(), extent, 0, 0, 0, 0);
		}
	}

	NextGIDenoise::DiffuseParams params;
	params.probe_radiance = probe_image;
	params.probe_radiance_out = probe_scratch;
	params.depth = last_depth;
	params.normal_roughness = last_normal_roughness;
	params.probe_size = Size2i(probe_w * 4, probe_h * 4);
	params.screen_size = Size2i(last_screen_w, last_screen_h);
	params.inv_projection = last_inv_projection;
	denoise->process_diffuse(params);

	// Second iteration, swapped, so the filtered result ends in probe_image for the integration.
	SWAP(params.probe_radiance, params.probe_radiance_out);
	denoise->process_diffuse(params);

	const Vector3i probe_extent(probe_w * 4, probe_h * 4, 1);
	if (!history_valid) {
		// First frame: the raw radiance seeds the history and there is nothing to reproject.
		rd->texture_copy(probe_image, probe_history, Vector3(0, 0, 0), Vector3(0, 0, 0), Vector3(probe_extent), 0, 0, 0, 0);
		history_valid = true;
		return;
	}

	NextGIDenoise::TemporalParams temporal;
	temporal.probe_radiance = probe_image;
	temporal.probe_radiance_out = probe_scratch;
	temporal.probe_history = probe_history;
	temporal.depth = last_depth;
	temporal.normal_roughness = last_normal_roughness;
	temporal.velocity = last_velocity.is_valid() ? last_velocity : dummy_color;
	temporal.history_length = history_length;
	temporal.reset_mask = reset_mask_buffer;
	temporal.probe_size = Size2i(probe_extent.x, probe_extent.y);
	temporal.screen_size = Size2i(last_screen_w, last_screen_h);
	temporal.prev_view_proj = last_prev_view_proj;
	temporal.inv_projection = last_inv_projection;
	temporal.camera_pos = last_camera_pos;
	temporal.prev_camera_pos = last_prev_camera_pos;
	temporal.history_cap = rr_probe_history_cap;
	temporal.velocity_enabled = last_velocity.is_valid();
	temporal.debug_views = variance_view || history_view || rejected_view;
	if (temporal.debug_views) {
		temporal.debug_variance = debug_variance;
		temporal.debug_history = debug_history;
		temporal.debug_rejected = debug_rejected;
	}
	denoise->process_temporal(temporal);

	// The integration reads probe_image, so land the accumulated result there, then keep this
	// frame's output as next frame's history.
	rd->texture_copy(probe_scratch, probe_image, Vector3(0, 0, 0), Vector3(0, 0, 0), Vector3(probe_extent), 0, 0, 0, 0);
	SWAP(probe_history, probe_scratch);
}

bool NextGIGather::_ensure_debug_textures() {
	if (debug_raw.is_valid() && debug_variance.is_valid() && debug_history.is_valid() && debug_rejected.is_valid()) {
		return true;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr) {
		return false;
	}
	RD::TextureFormat tf;
	tf.width = MAX(probe_w * 4, 1u);
	tf.height = MAX(probe_h * 4, 1u);
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	RID *textures[4] = { &debug_raw, &debug_variance, &debug_history, &debug_rejected };
	for (RID *texture : textures) {
		*texture = rd->texture_create(tf, RD::TextureView());
		if (texture->is_null()) {
			return false;
		}
		rd->texture_clear(*texture, Color(0, 0, 0, 1), 0, 1, 0, 1);
	}
	return true;
}

RID NextGIGather::get_specular_debug_texture() const {
	return specular_debug;
}

RID NextGIGather::get_debug_texture(int p_view) const {
	switch (NextGI::DebugViewId(p_view)) {
		case NextGI::DEBUG_VIEW_DENOISE_RAW:
			return debug_raw;
		case NextGI::DEBUG_VIEW_DENOISE_VARIANCE:
			return debug_variance;
		case NextGI::DEBUG_VIEW_DENOISE_HISTORY:
			return debug_history;
		case NextGI::DEBUG_VIEW_DENOISE_REJECTED:
			return debug_rejected;
		default:
			return RID();
	}
}

void NextGIGather::_denoise_specular() {
	if (denoise == nullptr || !specular_raw.is_valid() || !specular_history.is_valid() || !specular_denoised.is_valid() || !specular_scratch.is_valid() || !specular_history_length.is_valid()) {
		return;
	}
	if (!last_depth.is_valid() || !last_normal_roughness.is_valid() || !last_reflection_output.is_valid() || last_screen_w == 0 || last_screen_h == 0) {
		return;
	}
	const Size2i trace_size(MAX(last_screen_w / 2u, 1u), MAX(last_screen_h / 2u, 1u));
	const Size2i screen_size(last_screen_w, last_screen_h);
	RenderingDevice *rd = RenderingDevice::get_singleton();

	if (!rr_specular_filter_enabled) {
		// DLSS-RR bypass hook (#206): raw specular goes straight to the upsample and the history
		// is dropped so re-enabling starts fresh.
		if (specular_history.is_valid()) {
			rd->texture_clear(specular_history, Color(0, 0, 0, 0), 0, 1, 0, 1);
			rd->texture_clear(specular_history_length, Color(0, 0, 0, 0), 0, 1, 0, 1);
		}
		NextGIDenoise::SpecularUpsampleParams upsample;
		upsample.raw = specular_raw;
		upsample.depth = last_depth;
		upsample.normal_roughness = last_normal_roughness;
		upsample.output = last_reflection_output;
		upsample.trace_size = trace_size;
		upsample.screen_size = screen_size;
		denoise->process_specular_upsample(upsample);
		specular_final = specular_raw;
		return;
	}

	NextGIDenoise::SpecularTemporalParams temporal;
	temporal.raw = specular_raw;
	temporal.history = specular_history;
	temporal.depth = last_depth;
	temporal.normal_roughness = last_normal_roughness;
	temporal.velocity = last_velocity.is_valid() ? last_velocity : dummy_color;
	temporal.hit_distance = last_specular_hit.is_valid() ? last_specular_hit : dummy_color;
	temporal.output = specular_denoised;
	temporal.history_length = specular_history_length;
	temporal.trace_size = trace_size;
	temporal.screen_size = screen_size;
	temporal.prev_view_proj = last_prev_view_proj;
	temporal.inv_view_proj = last_inv_view_proj;
	temporal.camera_pos = last_camera_pos;
	temporal.prev_camera_pos = last_prev_camera_pos;
	temporal.velocity_enabled = last_velocity.is_valid();
	const NextGI::DebugViewId active_view = NextGI::get_singleton() != nullptr ? NextGI::get_singleton()->get_active_debug_view() : NextGI::DEBUG_VIEW_MAX;
	temporal.debug_views = active_view == NextGI::DEBUG_VIEW_SPECULAR_VARIANCE || active_view == NextGI::DEBUG_VIEW_SPECULAR_REJECTED;
	temporal.debug_variance = specular_debug_variance;
	temporal.debug_rejected = specular_debug_rejected;
	denoise->process_specular_temporal(temporal);

	const Vector3i extent(trace_size.width, trace_size.height, 1);
	rd->texture_copy(specular_denoised, specular_history, Vector3(), Vector3(), extent, 0, 0, 0, 0);

	RID source = specular_denoised;
	RID destination = specular_scratch;
	const int steps[3] = { 1, 2, 4 };
	for (int i = 0; i < 3; i++) {
		NextGIDenoise::SpecularSpatialParams spatial;
		spatial.source = source;
		spatial.depth = last_depth;
		spatial.normal_roughness = last_normal_roughness;
		spatial.output = destination;
		spatial.trace_size = trace_size;
		spatial.screen_size = screen_size;
		spatial.step = steps[i];
		denoise->process_specular_spatial(spatial);
		SWAP(source, destination);
	}

	NextGIDenoise::SpecularUpsampleParams upsample;
	upsample.raw = source;
	upsample.depth = last_depth;
	upsample.normal_roughness = last_normal_roughness;
	upsample.output = last_reflection_output;
	upsample.trace_size = trace_size;
	upsample.screen_size = screen_size;
	denoise->process_specular_upsample(upsample);
	specular_final = source;
}

RID NextGIGather::get_specular_filter_debug_texture(int p_view) const {
	switch (NextGI::DebugViewId(p_view)) {
		case NextGI::DEBUG_VIEW_SPECULAR_RAW:
			return specular_raw;
		case NextGI::DEBUG_VIEW_SPECULAR_VARIANCE:
			return specular_debug_variance;
		case NextGI::DEBUG_VIEW_SPECULAR_HISTORY:
			return specular_history_length;
		case NextGI::DEBUG_VIEW_SPECULAR_REJECTED:
			return specular_debug_rejected;
		case NextGI::DEBUG_VIEW_SPECULAR_DENOISED:
			return specular_final;
		default:
			return RID();
	}
}

void NextGIGather::integrate(RenderDataRD *p_render_data) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	NextGI *next_gi = NextGI::get_singleton();
	if (rd == nullptr || shader == nullptr || !uniform_set.is_valid() || next_gi == nullptr || next_gi->get_rt_scene() == nullptr || !next_gi->get_rt_scene()->is_tlas_traceable()) {
		return;
	}
	uint32_t width = last_screen_w;
	uint32_t height = last_screen_h;
	if (p_render_data != nullptr && p_render_data->render_buffers.is_valid() && p_render_data->render_buffers->has_texture(RB_SCOPE_GI, RB_TEX_AMBIENT)) {
		const RD::TextureFormat format = rd->texture_get_format(p_render_data->render_buffers->get_texture(RB_SCOPE_GI, RB_TEX_AMBIENT));
		width = format.width;
		height = format.height;
	}
	const uint32_t groups_x = MAX((width + 7u) / 8u, 1u);
	const uint32_t groups_y = MAX((height + 7u) / 8u, 1u);
	const RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, shader->pipelines[1]);
	rd->compute_list_bind_uniform_set(list, uniform_set, 0);
	rd->compute_list_dispatch(list, groups_x, groups_y, 1);
	rd->compute_list_end();
	_stabilize_ambient(p_render_data->render_buffers);
	_free_uniform_set();
}

void NextGIGather::_stabilize_ambient(Ref<RenderSceneBuffersRD> p_render_buffers) {
	if (!rr_stabilizer_enabled || denoise == nullptr || !ambient_scratch.is_valid() || !prev_ambient.is_valid() || p_render_buffers.is_null()) {
		return;
	}
	if (!p_render_buffers->has_texture(RB_SCOPE_GI, RB_TEX_AMBIENT)) {
		return;
	}
	RID ambient = p_render_buffers->get_texture(RB_SCOPE_GI, RB_TEX_AMBIENT);
	const RD::TextureFormat format = RenderingDevice::get_singleton()->texture_get_format(ambient);

	NextGIDenoise::StabilizeParams params;
	params.current = ambient_scratch;
	params.previous = prev_ambient;
	params.output = ambient;
	params.screen_size = Size2i(format.width, format.height);
	// The first frame has no previous output: write the current value through unchanged.
	params.stabilizer_enabled = ambient_history_valid;
	denoise->process_stabilize(params);

	SWAP(ambient_scratch, prev_ambient);
	ambient_history_valid = true;
}

String NextGIGather::build_shader_source(int p_variant) {
	NextgiGatherShaderRD shader;
	Vector<String> defines;
	defines.push_back("");
	defines.push_back("#define MODE_INTEGRATE\n");
	defines.push_back("#define MODE_SRC_FEEDBACK\n");
	defines.push_back("#define MODE_COVERAGE\n");
	defines.push_back("#define MODE_HIT_RADIANCE_REFERENCE\n");
	defines.push_back("#define MODE_REFLECT\n");
	shader.initialize(defines);
	const RID version = shader.version_create(false);
	ERR_FAIL_COND_V(!version.is_valid(), String());
	const Vector<String> sources = shader.version_build_variant_stage_sources(version, p_variant);
	shader.version_free(version);
	if (sources.size() <= RenderingDevice::SHADER_STAGE_COMPUTE) {
		return String();
	}
	return sources[RenderingDevice::SHADER_STAGE_COMPUTE];
}

void NextGIGather::fill_info(Dictionary &r_info) const {
	r_info["src_gather_rays"] = int64_t(last_rays);
	r_info["src_gather_misses"] = int64_t(last_misses);
	r_info["src_gather_history_resets"] = int64_t(last_resets);
	r_info["src_gather_uncovered"] = int64_t(last_uncovered);
	r_info["src_gather_screen_width"] = int64_t(last_screen_w);
	r_info["src_gather_screen_height"] = int64_t(last_screen_h);
	r_info["src_gather_expected_rays"] = int64_t(nextgi_gather_ray_count(last_screen_w, last_screen_h));
	r_info["src_gather_miss_luminance"] = last_miss_luminance;
	r_info["src_coverage_hits"] = int64_t(last_coverage_hits);
	r_info["src_coverage_rays"] = int64_t(last_coverage_rays);
	r_info["wrc_gather_samples"] = int64_t(last_wrc_samples);
	r_info["wrc_sky_samples"] = int64_t(last_sky_samples);
	r_info["screen_trace_hits"] = int64_t(last_screen_hits);
	r_info["gather_hit_radiance_reference"] = hit_radiance_reference;
	r_info["clamped_energy_fraction"] = last_clamp_total > 0 ? double(last_clamp_removed) / double(last_clamp_total) : 0.0;
	r_info["clamped_energy_removed"] = int64_t(last_clamp_removed);
	r_info["clamped_energy_total"] = int64_t(last_clamp_total);
	r_info["reflection_rays"] = int64_t(last_reflection_traced);
	r_info["reflection_hits"] = int64_t(last_reflection_hits);
	r_info["reflection_misses"] = int64_t(last_reflection_misses);
}
