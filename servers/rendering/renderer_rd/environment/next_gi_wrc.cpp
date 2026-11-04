/**************************************************************************/
/*  next_gi_wrc.cpp                                                       */
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

#include "next_gi_wrc.h"

#include "core/math/math_funcs.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_relight.h"
#include "servers/rendering/renderer_rd/environment/next_gi_src.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_wrc_update.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/rendering_device.h"

using namespace RendererRD;

struct NextGIWRC::ShaderState {
	NextgiWrcUpdateShaderRD shader;
	RID version;
	RID trace;
	RID project;
};

struct NextGIWRCParamsGPU {
	float camera[4] = {};
	float sky[4] = {};
	float origin_spacing[4] = {};
	uint32_t probe_base = 0;
	uint32_t probe_count = 0;
	uint32_t frame = 0;
	uint32_t record_count = 0;
	uint32_t page_rect_count = 0;
	uint32_t pad[3] = {};
};
static_assert(sizeof(NextGIWRCParamsGPU) == 80, "WRC params must match nextgi_wrc_update.glsl.");

struct NextGIWRCCountersGPU {
	uint32_t rays = 0;
	uint32_t hits = 0;
	uint32_t sky_misses = 0;
	uint32_t energy_bits = 0;
	uint32_t fog_samples = 0;
	uint32_t fog_ambient_bits = 0;
	uint32_t pad_a = 0;
	uint32_t pad_b = 0;
};

NextGIWRC *NextGIWRC::singleton = nullptr;

namespace RendererRD {

uint32_t nextgi_wrc_level_for_distance(float p_distance) {
	const float distance = MAX(p_distance, 0.0f);
	if (distance <= 8.0f) {
		return 0;
	}
	if (distance <= 16.0f) {
		return 1;
	}
	if (distance <= 32.0f) {
		return 2;
	}
	return 3;
}

Vector3 nextgi_wrc_snap_origin(const Vector3 &p_camera, float p_spacing) {
	const float spacing = MAX(p_spacing, 0.001f);
	return Vector3(
			Math::floor(p_camera.x / spacing) * spacing,
			Math::floor(p_camera.y / spacing) * spacing,
			Math::floor(p_camera.z / spacing) * spacing);
}

Vector3 nextgi_wrc_relocate(const Vector3 &p_probe, const Vector3 &p_mean_escape, float p_backface_fraction, float p_spacing) {
	if (p_backface_fraction <= 0.25f) {
		return p_probe;
	}
	const float length = p_mean_escape.length();
	if (length <= 0.0001f) {
		return p_probe;
	}
	const float travel = MIN(length, 0.4f * p_spacing);
	return p_probe + p_mean_escape / length * travel;
}

} // namespace RendererRD

NextGIWRC::NextGIWRC() {
	if (singleton == nullptr) {
		singleton = this;
	}
}

NextGIWRC::~NextGIWRC() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

void NextGIWRC::_free_uniform_set() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr && uniform_set.is_valid() && rd->uniform_set_is_valid(uniform_set)) {
		rd->free_rid(uniform_set);
	}
	uniform_set = RID();
}

void NextGIWRC::free_resources() {
	_free_uniform_set();
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (shader != nullptr) {
		if (shader->version.is_valid()) {
			shader->shader.version_free(shader->version);
		}
		memdelete(shader);
		shader = nullptr;
	}
	if (rd == nullptr) {
		return;
	}
	RID resources[13] = { radiance, mip4, mip2, visibility, sh_r, sh_g, sh_b, offsets, params_buffer, counter_buffer, sampler, dummy_tex, dummy_buffer };
	for (RID resource : resources) {
		if (resource.is_valid()) {
			rd->free_rid(resource);
		}
	}
	radiance = RID();
	mip4 = RID();
	mip2 = RID();
	visibility = RID();
	sh_r = RID();
	sh_g = RID();
	sh_b = RID();
	offsets = RID();
	params_buffer = RID();
	counter_buffer = RID();
	sampler = RID();
	dummy_tex = RID();
	dummy_buffer = RID();
}

bool NextGIWRC::_ensure_textures() {
	if (radiance.is_valid()) {
		return true;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	BitField<RD::TextureUsageBits> usage = RD::TEXTURE_USAGE_STORAGE_BIT;
	usage.set_flag(RD::TEXTURE_USAGE_SAMPLING_BIT);
	usage.set_flag(RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	usage.set_flag(RD::TEXTURE_USAGE_CAN_COPY_TO_BIT);
	packed_radiance = rd->texture_is_format_supported_for_usage(RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32, usage);

	RD::TextureFormat tf;
	tf.texture_type = RD::TEXTURE_TYPE_2D;
	tf.usage_bits = usage;
	tf.width = 1280;
	tf.height = 1280;
	tf.format = packed_radiance ? RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32 : RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	radiance = rd->texture_create(tf, RD::TextureView());
	ERR_FAIL_COND_V(radiance.is_null(), false);
	rd->set_resource_name(radiance, "NextGI WRC Radiance");
	rd->texture_clear(radiance, Color(0, 0, 0, 1), 0, 1, 0, 1);

	tf.width = 1024;
	tf.height = 1024;
	tf.format = RD::DATA_FORMAT_R16G16_SFLOAT;
	visibility = rd->texture_create(tf, RD::TextureView());
	ERR_FAIL_COND_V(visibility.is_null(), false);
	rd->texture_clear(visibility, Color(0, 0, 0, 0), 0, 1, 0, 1);

	tf.width = 768;
	tf.height = 768;
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	mip4 = rd->texture_create(tf, RD::TextureView());
	tf.width = 512;
	tf.height = 512;
	mip2 = rd->texture_create(tf, RD::TextureView());

	tf.texture_type = RD::TEXTURE_TYPE_3D;
	tf.width = 16;
	tf.height = 16;
	tf.depth = 64;
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	sh_r = rd->texture_create(tf, RD::TextureView());
	sh_g = rd->texture_create(tf, RD::TextureView());
	sh_b = rd->texture_create(tf, RD::TextureView());
	offsets = rd->texture_create(tf, RD::TextureView());
	ERR_FAIL_COND_V(sh_r.is_null() || sh_g.is_null() || sh_b.is_null() || offsets.is_null(), false);
	// 3D textures have one layer; depth is the clipmap stack, not array layers.
	rd->texture_clear(sh_r, Color(0, 0, 0, 0), 0, 1, 0, 1);
	rd->texture_clear(sh_g, Color(0, 0, 0, 0), 0, 1, 0, 1);
	rd->texture_clear(sh_b, Color(0, 0, 0, 0), 0, 1, 0, 1);
	rd->texture_clear(offsets, Color(0, 0, 0, 0), 0, 1, 0, 1);

	RD::SamplerState state;
	state.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	state.min_filter = RD::SAMPLER_FILTER_LINEAR;
	sampler = rd->sampler_create(state);
	params_buffer = rd->storage_buffer_create(sizeof(NextGIWRCParamsGPU));
	counter_buffer = rd->storage_buffer_create(sizeof(NextGIWRCCountersGPU));
	RD::TextureFormat dummy_format;
	dummy_format.width = 1;
	dummy_format.height = 1;
	dummy_format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	dummy_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	dummy_tex = rd->texture_create(dummy_format, RD::TextureView());
	dummy_buffer = rd->storage_buffer_create(16);
	return radiance.is_valid() && counter_buffer.is_valid() && dummy_tex.is_valid();
}

bool NextGIWRC::_ensure_shader() {
	if (shader != nullptr && shader->version.is_valid()) {
		return true;
	}
	NextGI *next_gi = NextGI::get_singleton();
	ERR_FAIL_NULL_V(next_gi, false);
	ERR_FAIL_COND_V(!next_gi->can_initialize_ray_query_shaders(), false);
	shader = memnew(ShaderState);
	Vector<String> defines;
	defines.push_back("");
	defines.push_back("#define MODE_SH\n");
	shader->shader.initialize(defines);
	shader->version = shader->shader.version_create(false);
	if (!shader->version.is_valid()) {
		memdelete(shader);
		shader = nullptr;
		ERR_FAIL_V_MSG(false, "The NextGI WRC shader failed to compile.");
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	shader->trace = rd->compute_pipeline_create(shader->shader.version_get_shader(shader->version, 0));
	shader->project = rd->compute_pipeline_create(shader->shader.version_get_shader(shader->version, 1));
	if (!shader->trace.is_valid() || !shader->project.is_valid()) {
		shader->shader.version_free(shader->version);
		memdelete(shader);
		shader = nullptr;
		ERR_FAIL_V_MSG(false, "The NextGI WRC pipeline failed to compile.");
	}
	next_gi->notify_ray_query_shader_initialized();
	return true;
}

void NextGIWRC::_read_counters() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr || !counter_buffer.is_valid()) {
		return;
	}
	const Vector<uint8_t> data = rd->buffer_get_data(counter_buffer);
	if (data.size() < int(sizeof(NextGIWRCCountersGPU))) {
		return;
	}
	NextGIWRCCountersGPU got;
	memcpy(&got, data.ptr(), sizeof(got));
	last_rays = got.rays;
	last_hits = got.hits;
	last_sky = got.sky_misses;
	last_energy_bits = got.energy_bits;
	fog_samples = got.fog_samples;
	fog_ambient_bits = got.fog_ambient_bits;
}

void NextGIWRC::update(const RenderDataRD *p_render_data) {
	if (p_render_data == nullptr || p_render_data->scene_data == nullptr) {
		return;
	}
	NextGI *next_gi = NextGI::get_singleton();
	if (next_gi == nullptr || !next_gi->can_initialize_ray_query_shaders() || next_gi->get_rt_scene() == nullptr || !next_gi->get_rt_scene()->is_tlas_traceable()) {
		return;
	}
	if (!_ensure_textures() || !_ensure_shader()) {
		return;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	const Vector3 camera = p_render_data->scene_data->cam_transform.origin;
	const Vector3 origin = nextgi_wrc_snap_origin(camera, 1.0f);
	NextGIWRCParamsGPU params;
	params.camera[0] = camera.x;
	params.camera[1] = camera.y;
	params.camera[2] = camera.z;
	params.camera[3] = 1.0f;
	params.origin_spacing[0] = origin.x;
	params.origin_spacing[1] = origin.y;
	params.origin_spacing[2] = origin.z;
	params.origin_spacing[3] = 1.0f;
	params.probe_base = cursor;
	params.probe_count = NEXTGI_WRC_PROBES_PER_FRAME;
	params.frame = frame;
	params.record_count = next_gi->get_rt_scene()->get_stats().instance_count;
	NextGIRelight *relight = NextGIRelight::get_singleton();
	params.page_rect_count = relight != nullptr ? relight->get_page_rect_count() : 0;
	RendererSceneRenderRD *scene = RendererSceneRenderRD::get_singleton();
	if (scene != nullptr && p_render_data->environment.is_valid()) {
		const Color bg = scene->environment_get_bg_color(p_render_data->environment);
		const float energy = scene->environment_get_bg_energy_multiplier(p_render_data->environment);
		params.sky[0] = bg.r * energy;
		params.sky[1] = bg.g * energy;
		params.sky[2] = bg.b * energy;
	}
	rd->buffer_update(params_buffer, 0, sizeof(params), &params);
	_read_counters();
	const uint32_t previous_fog_samples = fog_samples;
	const uint32_t previous_fog_bits = fog_ambient_bits;
	NextGIWRCCountersGPU zero;
	rd->buffer_update(counter_buffer, 0, sizeof(zero), &zero);

	NextGISRC *src = NextGISRC::get_singleton();
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	RID chunks[4] = { dummy_buffer, dummy_buffer, dummy_buffer, dummy_buffer };
	if (mesh_storage != nullptr) {
		for (uint32_t i = 0; i < MIN(mesh_storage->mesh_nextgi_rt_get_chunk_count(), 4u); i++) {
			const RID chunk = mesh_storage->mesh_nextgi_rt_get_chunk_buffer(i);
			if (chunk.is_valid()) {
				chunks[i] = chunk;
			}
		}
	}
	RID records = next_gi->get_rt_scene()->get_instance_record_buffer();
	RID surfaces = next_gi->get_rt_scene()->get_surface_record_buffer();
	RID cards = src != nullptr ? src->get_card_buffer() : RID();
	RID card_sets = src != nullptr ? src->get_card_set_buffer() : RID();
	RID page_rects = relight != nullptr ? relight->get_page_rect_buffer() : RID();
	RID src_radiance = relight != nullptr ? relight->get_radiance_texture() : RID();
	RID material_depth = src != nullptr ? src->get_atlas_texture(NextGISRC::ATLAS_DEPTH) : RID();
	auto valid_buffer = [&](RID p_buffer) {
		return p_buffer.is_valid() ? p_buffer : dummy_buffer;
	};
	auto valid_tex = [&](RID p_texture) {
		return p_texture.is_valid() ? p_texture : dummy_tex;
	};
	_free_uniform_set();
	RD::Uniform uniforms[22] = {
		RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, next_gi->get_rt_scene()->get_tlas()),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, radiance),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, visibility),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, sh_r),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, sh_g),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, sh_b),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 6, offsets),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, mip4),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, mip2),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, valid_buffer(records)),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, valid_buffer(surfaces)),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, chunks[0]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, chunks[1]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, chunks[2]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, chunks[3]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, valid_buffer(cards)),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, valid_buffer(card_sets)),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 17, valid_buffer(page_rects)),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 18, Vector<RID>({ sampler, valid_tex(src_radiance) })),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 19, Vector<RID>({ sampler, valid_tex(material_depth) })),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 20, params_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 21, counter_buffer),
	};
	uniform_set = rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 22), shader->shader.version_get_shader(shader->version, 0), 0);
	if (uniform_set.is_valid()) {
		const RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, shader->trace);
		rd->compute_list_bind_uniform_set(list, uniform_set, 0);
		rd->compute_list_dispatch(list, NEXTGI_WRC_PROBES_PER_FRAME, 1, 1);
		rd->compute_list_add_barrier(list);
		rd->compute_list_bind_compute_pipeline(list, shader->project);
		rd->compute_list_bind_uniform_set(list, uniform_set, 0);
		rd->compute_list_dispatch(list, NEXTGI_WRC_PROBES_PER_FRAME, 1, 1);
		rd->compute_list_end();
	}
	_read_counters();
	// Fog runs after this slot and adds into the same buffer. Keep the previous frame's fog totals.
	fog_samples = previous_fog_samples;
	fog_ambient_bits = previous_fog_bits;
	cursor = (cursor + NEXTGI_WRC_PROBES_PER_FRAME) % NEXTGI_WRC_PROBE_COUNT;
	frame++;
}

void NextGIWRC::fill_info(Dictionary &r_info) const {
	r_info["wrc_bytes"] = int64_t(nextgi_wrc_bytes());
	r_info["wrc_probes"] = int64_t(NEXTGI_WRC_PROBE_COUNT);
	r_info["wrc_rays"] = int64_t(last_rays);
	r_info["wrc_hits"] = int64_t(last_hits);
	r_info["wrc_sky_misses"] = int64_t(last_sky);
	r_info["wrc_energy_bits"] = int64_t(last_energy_bits);
	r_info["wrc_fog_samples"] = int64_t(fog_samples);
	r_info["wrc_fog_ambient_bits"] = int64_t(fog_ambient_bits);
}
