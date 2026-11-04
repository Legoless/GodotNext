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
};
static_assert(sizeof(GatherParamsGPU) == 248, "Gather params must match nextgi_gather.glsl.");
static_assert(offsetof(GatherParamsGPU, record_count) == 224, "record_count must follow feedback_stride.");
static_assert(offsetof(GatherParamsGPU, page_rect_count) == 232, "page_rect_count must follow velocity_enabled.");

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
	RID pipelines[5];
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
	if (rd != nullptr && uniform_set.is_valid()) {
		rd->free_rid(uniform_set);
	}
	uniform_set = RID();
}

void NextGIGather::free_resources() {
	_free_uniform_set();
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
	RID resources[8] = { probe_image, params_buffer, counter_buffer, feedback_buffer, confidence_buffer, dummy_color, dummy_storage, dummy_buffer };
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
	for (int i = 0; i < 5; i++) {
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
		tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
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
	if (confidence_buffer.is_valid()) {
		rd->free_rid(confidence_buffer);
		confidence_buffer = RID();
	}
	RD::TextureFormat tf;
	tf.width = MAX(probes_x * 4, 1u);
	tf.height = MAX(probes_y * 4, 1u);
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	probe_image = rd->texture_create(tf, RD::TextureView());
	ERR_FAIL_COND_V(probe_image.is_null(), false);
	rd->texture_clear(probe_image, Color(0, 0, 0, 0), 0, 1, 0, 1);
	confidence_buffer = rd->storage_buffer_create(sizeof(float) * MAX(probes_x * probes_y, 1u));
	probe_w = probes_x;
	probe_h = probes_y;
	return true;
}

void NextGIGather::init() {
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
	// prev_view_projection is the projection only. Match the current matrix, including the previous camera.
	const Projection prev_view_proj = scene_data->prev_cam_projection * Projection(scene_data->prev_cam_transform.affine_inverse());
	GatherParamsGPU params;
	_store_projection(view_proj.inverse(), params.inv_view_proj);
	_store_projection(prev_view_proj, params.prev_view_proj);
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
	if (rb->has_texture(SNAME("forward_clustered"), SNAME("normal_roughness")) && rb->has_velocity_buffer(false)) {
		normal_roughness = rb->get_texture(SNAME("forward_clustered"), SNAME("normal_roughness"));
		velocity = rb->get_velocity_buffer(false);
		params.velocity_enabled = velocity.is_valid() && normal_roughness.is_valid() ? 1u : 0u;
	} else {
		params.velocity_enabled = 0;
	}
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
	RID confidence = confidence_buffer.is_valid() ? confidence_buffer : dummy_buffer;
	RD::Uniform uniforms[32] = {
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
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 16, rb->get_texture(RB_SCOPE_GI, RB_TEX_AMBIENT)),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 17, indirect),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 18, params_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 19, counter_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 20, feedback_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 21, Vector<RID>({ sampler_nearest, normal_roughness })),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 22, Vector<RID>({ sampler_nearest, velocity })),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 23, ref_materials),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 24, ref_lights),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 25, ref_params),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 26, Vector<RID>({ sampler, wrc_radiance })),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 27, Vector<RID>({ sampler, wrc_visibility })),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 28, wrc_params),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 29, confidence),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 30, Vector<RID>({ sampler_nearest, hiz_texture })),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 31, Vector<RID>({ sampler, last_frame_texture })),
	};
	const RID shader_rid = shader->shader.version_get_shader(shader->version, 0);
	uniform_set = rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 32), shader_rid, 0);
	return uniform_set.is_valid();
}

void NextGIGather::_read_counters() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr || !counter_buffer.is_valid()) {
		return;
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
	_read_counters();
	frame++;
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
	_free_uniform_set();
}

String NextGIGather::build_shader_source(int p_variant) {
	NextgiGatherShaderRD shader;
	Vector<String> defines;
	defines.push_back("");
	defines.push_back("#define MODE_INTEGRATE\n");
	defines.push_back("#define MODE_SRC_FEEDBACK\n");
	defines.push_back("#define MODE_COVERAGE\n");
	defines.push_back("#define MODE_HIT_RADIANCE_REFERENCE\n");
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
}
