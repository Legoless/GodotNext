/**************************************************************************/
/*  next_gi_reference.cpp                                                 */
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

#include "next_gi_reference.h"

#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/effects/path_tracer.h" // GodotNext: path_tracer (#167)
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_scene.h"
#include "servers/rendering/renderer_rd/environment/sky.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_reference.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

using namespace RendererRD;

struct NextGIReference::ShaderState {
	NextgiReferenceShaderRD shader;
	RID version;
	RID accumulate;
	RID composite;
};

struct NextGIRefGPUParams {
	// GodotNext: path_tracer (#167). The camera block is RendererRD::PathTracer's, which both estimators read.
	PathTracerGPUCamera camera;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t sample_index = 0;
	uint32_t direct_only = 0;
	uint32_t seed = 1;
	uint32_t light_count = 0;
	uint32_t surface_count = 0;
	uint32_t record_count = 0;
	float sky_color[4] = {};
	float exposure = 1.0f;
	float clear = 0.0f;
	float samples_per_pixel = 1.0f;
	float pad = 0.0f;
};

static_assert(sizeof(NextGIRefGPUParams) == 128, "Reference params must match the shader buffer.");

static uint32_t _nextgi_reference_samples = 0;

namespace RendererRD {
uint32_t nextgi_reference_sample_count() {
	return _nextgi_reference_samples;
}
} //namespace RendererRD

void NextGIReference::_free_uniform_set() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr && uniform_set.is_valid()) {
		rd->free_rid(uniform_set);
	}
	uniform_set = RID();
}

void NextGIReference::free() {
	_free_uniform_set();
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (shader != nullptr) {
		if (shader->version.is_valid()) {
			shader->shader.version_free(shader->version);
		}
		memdelete(shader);
		shader = nullptr;
	}
	if (rd != nullptr) {
		RID resources[7] = { accumulation, reflection_guides, reflection_distance, material_buffer, light_buffer, params_buffer, estimator_buffer };
		for (RID resource : resources) {
			if (resource.is_valid()) {
				rd->free_rid(resource);
			}
		}
		if (dummy_buffer.is_valid()) {
			rd->free_rid(dummy_buffer);
		}
		if (dummy_sky_texture.is_valid()) {
			rd->free_rid(dummy_sky_texture);
		}
		if (sky_sampler.is_valid()) {
			rd->free_rid(sky_sampler);
		}
	}
	accumulation = RID();
	reflection_guides = RID();
	reflection_distance = RID();
	material_buffer = RID();
	light_buffer = RID();
	params_buffer = RID();
	estimator_buffer = RID();
	dummy_buffer = RID();
	dummy_sky_texture = RID();
	sky_sampler = RID();
	tables_current = false;
	samples = 0;
	_nextgi_reference_samples = 0;
	read_accumulation.clear();
	read_reflection_guides.clear();
}

Vector<String> NextGIReference::get_shader_variant_defines() {
	Vector<String> defines;
	defines.push_back("");
	defines.push_back("#define MODE_COMPOSITE\n");
	return defines;
}

bool NextGIReference::_ensure_shader() {
	if (shader != nullptr && shader->version.is_valid()) {
		return true;
	}
	NextGI *next_gi = NextGI::get_singleton();
	ERR_FAIL_NULL_V(next_gi, false);
	ERR_FAIL_COND_V(!nextgi_reference_shader_allowed(next_gi->can_initialize_ray_query_shaders()), false);
	shader = memnew(ShaderState);
	shader->shader.initialize(get_shader_variant_defines());
	// GodotNext: shader_baker_ray_query
	// Embedded so the shader baker bakes or skips the ray-query version per target (#40).
	shader->version = shader->shader.version_create(true);
	if (!shader->version.is_valid()) {
		memdelete(shader);
		shader = nullptr;
		ERR_FAIL_V_MSG(false, "The NextGI reference shader failed to compile.");
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	shader->accumulate = rd->compute_pipeline_create(shader->shader.version_get_shader(shader->version, 0));
	shader->composite = rd->compute_pipeline_create(shader->shader.version_get_shader(shader->version, 1));
	if (!shader->accumulate.is_valid() || !shader->composite.is_valid()) {
		if (shader->version.is_valid()) {
			shader->shader.version_free(shader->version);
		}
		memdelete(shader);
		shader = nullptr;
		ERR_FAIL_V_MSG(false, "The NextGI reference pipeline failed to compile.");
	}
	next_gi->notify_ray_query_shader_initialized();
	return true;
}

bool NextGIReference::is_active() const {
	return int(GLOBAL_GET("rendering/global_illumination/nextgi/debug/reference_mode")) != 0;
}

bool NextGIReference::_refuse_without_hwrt() {
	NextGI *next_gi = NextGI::get_singleton();
	if (next_gi != nullptr && nextgi_reference_shader_allowed(next_gi->can_initialize_ray_query_shaders())) {
		return false;
	}
	if (!logged_inactive) {
		logged_inactive = true;
		ERR_PRINT("NextGI reference_mode requires the HWRT tier. The reference ShaderRD was not initialized.");
	}
	return true;
}

uint64_t NextGIReference::_scene_hash(const RenderDataRD *p_render_data) const {
	// GodotNext: nextgi_reference_capture (#263). See NextGIReferenceResetKey.
	NextGIReferenceResetKey key;
	key.seed = uint32_t(int(GLOBAL_GET("rendering/global_illumination/nextgi/debug/reference_seed")));
	key.mode = uint32_t(int(GLOBAL_GET("rendering/global_illumination/nextgi/debug/reference_mode")));
	key.epoch = uint32_t(int(GLOBAL_GET("rendering/global_illumination/nextgi/debug/reference_epoch")));
	NextGI *next_gi = NextGI::get_singleton();
	if (next_gi != nullptr && next_gi->get_rt_scene() != nullptr) {
		key.instance_count = next_gi->get_rt_scene()->get_stats().instance_count;
	}
	if (p_render_data != nullptr && p_render_data->scene_data != nullptr) {
		key.camera = p_render_data->scene_data->cam_transform;
		key.projection = p_render_data->scene_data->cam_projection;
	}
	RendererSceneRenderRD *scene = RendererSceneRenderRD::get_singleton();
	if (scene != nullptr && p_render_data != nullptr && p_render_data->environment.is_valid()) {
		const RID env = p_render_data->environment;
		const float bg_energy = scene->environment_get_bg_energy_multiplier(env);
		key.sky = scene->environment_get_bg_color(env) * bg_energy;
		key.sky.a = 0.0f;
		if (scene->environment_get_background(env) == RSE::ENV_BG_SKY) {
			const RID sky = scene->environment_get_sky(env);
			SkyRD *sky_rd = scene->get_sky();
			if (sky.is_valid() && sky_rd != nullptr && sky_rd->sky_get_radiance_texture_2d(sky).is_valid()) {
				key.sky.a = bg_energy;
			}
		}
	}
	key.tables = tables_hash;
	return nextgi_reference_reset_hash(key);
}

bool NextGIReference::_build_tables(const RenderDataRD *p_render_data, uint32_t &r_surface_count, uint32_t &r_light_count) {
	if (tables_current && material_buffer.is_valid() && light_buffer.is_valid()) {
		r_surface_count = cached_surface_count;
		r_light_count = cached_light_count;
		return true;
	}
	// GodotNext: path_tracer (#167). The D16 tables are RendererRD::PathTracer's, built as #27 and #263 built them.
	PathTracerTables tables;
	PathTracer::build_reference_tables(p_render_data, tables);
	scene_invalid = tables.textured_materials > 0;
	if (scene_invalid) {
		WARN_PRINT("NextGI reference path tracer ignores textured materials. This scene's reference is invalid.");
	}
	r_surface_count = tables.surface_count;
	r_light_count = tables.light_count;

	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	if (material_buffer.is_valid()) {
		rd->free_rid(material_buffer);
	}
	if (light_buffer.is_valid()) {
		rd->free_rid(light_buffer);
	}
	Vector<uint8_t> material_bytes;
	material_bytes.resize(tables.materials.size() * sizeof(PathTracerGPUMaterial));
	memcpy(material_bytes.ptrw(), tables.materials.ptr(), material_bytes.size());
	material_buffer = rd->storage_buffer_create(material_bytes.size(), material_bytes);
	Vector<uint8_t> light_bytes;
	light_bytes.resize(tables.lights.size() * sizeof(float));
	memcpy(light_bytes.ptrw(), tables.lights.ptr(), light_bytes.size());
	light_buffer = rd->storage_buffer_create(light_bytes.size(), light_bytes);
	tables_hash = tables.hash;
	tables_current = material_buffer.is_valid() && light_buffer.is_valid();
	cached_surface_count = r_surface_count;
	cached_light_count = r_light_count;
	return tables_current;
}

bool NextGIReference::prepare_estimator(const RenderDataRD *p_render_data, RID &r_materials, RID &r_lights, RID &r_params) {
	r_materials = RID();
	r_lights = RID();
	r_params = RID();
	if (p_render_data == nullptr) {
		return false;
	}
	uint32_t surface_count = 0;
	uint32_t light_count = 0;
	if (!_build_tables(p_render_data, surface_count, light_count) || scene_invalid) {
		return false;
	}
	NextGI *next_gi = NextGI::get_singleton();
	NextGIRefGPUParams params;
	params.direct_only = nextgi_reference_shader_mode(int(GLOBAL_GET("rendering/global_illumination/nextgi/debug/reference_mode")));
	params.seed = uint32_t(int(GLOBAL_GET("rendering/global_illumination/nextgi/debug/reference_seed")));
	params.light_count = light_count;
	params.surface_count = surface_count;
	params.record_count = next_gi != nullptr && next_gi->get_rt_scene() != nullptr ? next_gi->get_rt_scene()->get_stats().instance_count : 0;
	RendererSceneRenderRD *scene = RendererSceneRenderRD::get_singleton();
	if (scene != nullptr && p_render_data->environment.is_valid()) {
		const Color bg = scene->environment_get_bg_color(p_render_data->environment);
		const float energy = scene->environment_get_bg_energy_multiplier(p_render_data->environment);
		params.sky_color[0] = bg.r * energy;
		params.sky_color[1] = bg.g * energy;
		params.sky_color[2] = bg.b * energy;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	if (!estimator_buffer.is_valid()) {
		estimator_buffer = rd->storage_buffer_create(sizeof(NextGIRefGPUParams));
	}
	rd->buffer_update(estimator_buffer, 0, sizeof(params), &params);
	r_materials = material_buffer;
	r_lights = light_buffer;
	r_params = estimator_buffer;
	return r_materials.is_valid() && r_lights.is_valid() && r_params.is_valid();
}

void NextGIReference::render(const RenderDataRD *p_render_data) {
	if (!is_active() || p_render_data == nullptr || p_render_data->render_buffers.is_null() || p_render_data->scene_data == nullptr) {
		return;
	}
	NextGI *next_gi = NextGI::get_singleton();
	if (next_gi == nullptr || next_gi->get_rt_scene() == nullptr) {
		return;
	}
	if (_refuse_without_hwrt()) {
		return;
	}
	if (!next_gi->get_rt_scene()->is_tlas_traceable()) {
		return;
	}
	if (!_ensure_shader()) {
		return;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	const Size2i screen = rb->get_internal_size();
	if (screen.x <= 0 || screen.y <= 0) {
		return;
	}
	// GodotNext: nextgi_reference_capture (#263). The tables come first: their hash is part of the reset key.
	uint32_t surface_count = 0;
	uint32_t light_count = 0;
	if (!_build_tables(p_render_data, surface_count, light_count) || scene_invalid) {
		_nextgi_reference_samples = samples;
		return;
	}
	const uint64_t hash = _scene_hash(p_render_data);
	const bool reset = hash != scene_hash || uint32_t(screen.x) != width || uint32_t(screen.y) != height || !accumulation.is_valid();
	if (reset) {
		scene_hash = hash;
		samples = 0;
		resets++;
		width = screen.x;
		height = screen.y;
		if (accumulation.is_valid()) {
			rd->free_rid(accumulation);
		}
		RD::TextureFormat format;
		format.width = width;
		format.height = height;
		format.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
		format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		accumulation = rd->texture_create(format, RD::TextureView());
		rd->texture_clear(accumulation, Color(0, 0, 0, 0), 0, 1, 0, 1);
		if (reflection_guides.is_valid()) {
			rd->free_rid(reflection_guides);
		}
		if (reflection_distance.is_valid()) {
			rd->free_rid(reflection_distance);
		}
		RD::TextureFormat guide_format = format;
		guide_format.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
		reflection_guides = rd->texture_create(guide_format, RD::TextureView());
		rd->texture_clear(reflection_guides, Color(0, 0, 0, 0), 0, 1, 0, 1);
		RD::TextureFormat distance_format = format;
		distance_format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		reflection_distance = rd->texture_create(distance_format, RD::TextureView());
		rd->texture_clear(reflection_distance, Color(0, 0, 0, 0), 0, 1, 0, 1);
	}
	if (!dummy_buffer.is_valid()) {
		dummy_buffer = rd->storage_buffer_create(16);
	}
	if (!dummy_sky_texture.is_valid()) {
		RD::TextureFormat sky_fmt;
		sky_fmt.width = 1;
		sky_fmt.height = 1;
		sky_fmt.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		sky_fmt.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		dummy_sky_texture = rd->texture_create(sky_fmt, RD::TextureView());
		rd->texture_clear(dummy_sky_texture, Color(0, 0, 0, 0), 0, 1, 0, 1);
	}
	if (!sky_sampler.is_valid()) {
		RD::SamplerState sampler_state;
		sampler_state.mag_filter = RD::SAMPLER_FILTER_LINEAR;
		sampler_state.min_filter = RD::SAMPLER_FILTER_LINEAR;
		sampler_state.mip_filter = RD::SAMPLER_FILTER_LINEAR;
		sampler_state.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		sampler_state.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		sky_sampler = rd->sampler_create(sampler_state);
	}
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
	const Projection &projection = p_render_data->scene_data->cam_projection;
	const Transform3D &camera = p_render_data->scene_data->cam_transform;
	NextGIRefGPUParams params;
	params.camera = path_tracer_camera(camera, projection, screen);
	params.width = width;
	params.height = height;
	params.sample_index = samples;
	params.direct_only = nextgi_reference_shader_mode(int(GLOBAL_GET("rendering/global_illumination/nextgi/debug/reference_mode")));
	params.seed = uint32_t(int(GLOBAL_GET("rendering/global_illumination/nextgi/debug/reference_seed")));
	params.light_count = light_count;
	params.surface_count = surface_count;
	params.record_count = next_gi->get_rt_scene()->get_stats().instance_count;
	params.exposure = 1.0f;
	if (p_render_data->camera_attributes.is_valid() && RSG::camera_attributes != nullptr) {
		params.exposure = RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
	}
	exposure = params.exposure;
	params.clear = reset ? 1.0f : 0.0f;
	params.samples_per_pixel = 32.0f;
	RendererSceneRenderRD *scene = RendererSceneRenderRD::get_singleton();
	RID sky_tex = dummy_sky_texture;
	if (scene != nullptr && p_render_data->environment.is_valid()) {
		const RID env = p_render_data->environment;
		const Color bg = scene->environment_get_bg_color(env);
		const float energy = scene->environment_get_bg_energy_multiplier(env);
		params.sky_color[0] = bg.r * energy;
		params.sky_color[1] = bg.g * energy;
		params.sky_color[2] = bg.b * energy;
		params.sky_color[3] = 0.0f;
		if (scene->environment_get_background(env) == RSE::ENV_BG_SKY) {
			const RID sky = scene->environment_get_sky(env);
			SkyRD *sky_rd = scene->get_sky();
			if (sky.is_valid() && sky_rd != nullptr) {
				const RID radiance = sky_rd->sky_get_radiance_texture_2d(sky);
				if (radiance.is_valid()) {
					sky_tex = radiance;
					const float baked_exp = MAX(sky_rd->sky_get_baked_exposure(sky), 1e-4f);
					params.sky_color[3] = energy / MAX(baked_exp * params.exposure, 1e-6f);
				}
			}
		}
	}
	if (!params_buffer.is_valid()) {
		params_buffer = rd->storage_buffer_create(sizeof(NextGIRefGPUParams));
	}
	rd->buffer_update(params_buffer, 0, sizeof(params), &params);

	_free_uniform_set();
	RID records = next_gi->get_rt_scene()->get_instance_record_buffer();
	RID surfaces = next_gi->get_rt_scene()->get_surface_record_buffer();
	if (!records.is_valid()) {
		records = dummy_buffer;
	}
	if (!surfaces.is_valid()) {
		surfaces = dummy_buffer;
	}
	RD::Uniform sky_uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 14, Vector<RID>({ sky_sampler, sky_tex }));
	RD::Uniform uniforms[15] = {
		RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, next_gi->get_rt_scene()->get_tlas()),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, accumulation),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, accumulation),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, material_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, light_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, records),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, surfaces),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, chunks[0]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, chunks[1]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, chunks[2]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, chunks[3]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, params_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 12, reflection_guides),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 13, reflection_distance),
		sky_uniform,
	};
	uniform_set = rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 15), shader->shader.version_get_shader(shader->version, 0), 0);
	if (!uniform_set.is_valid()) {
		return;
	}
	const RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, shader->accumulate);
	rd->compute_list_bind_uniform_set(list, uniform_set, 0);
	rd->compute_list_dispatch(list, (width + 7) / 8, (height + 7) / 8, 1);
	rd->compute_list_end();
	samples += uint32_t(params.samples_per_pixel);
	_nextgi_reference_samples = samples;
	_readback();
}

void NextGIReference::_readback() {
	if (!bool(GLOBAL_GET("rendering/global_illumination/nextgi/debug/reference_readback"))) {
		return;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr || !accumulation.is_valid() || width == 0 || height == 0) {
		return;
	}
	const Vector<uint8_t> data = rd->texture_get_data(accumulation, 0);
	const int pixels = int(width) * int(height);
	if (data.size() < pixels * 16) {
		return;
	}
	// GodotNext: nextgi_reference_capture (#263). The harness reads the float32 sums for its batch statistics.
	read_accumulation = data;
	// GodotNext: rr_guides (#205). The RR guide check compares the specular hit distance with these.
	read_reflection_guides.clear();
	if (nextgi_reference_shader_mode(int(GLOBAL_GET("rendering/global_illumination/nextgi/debug/reference_mode"))) == 2 && reflection_guides.is_valid()) {
		read_reflection_guides = rd->texture_get_data(reflection_guides, 0);
	}
	const float *texels = reinterpret_cast<const float *>(data.ptr());
	double sum = 0.0;
	float lo = 1.0e30f;
	float hi = -1.0e30f;
	uint64_t hash = 1469598103934665603ull;
	int counted = 0;
	for (int i = 0; i < pixels; i++) {
		const float weight = texels[i * 4 + 3];
		if (weight <= 0.0f) {
			continue;
		}
		float channels[3] = { texels[i * 4] / weight, texels[i * 4 + 1] / weight, texels[i * 4 + 2] / weight };
		const float luminance = (channels[0] + channels[1] + channels[2]) / 3.0f;
		sum += luminance;
		lo = MIN(lo, luminance);
		hi = MAX(hi, luminance);
		counted++;
		for (int c = 0; c < 3; c++) {
			uint32_t bits = 0;
			memcpy(&bits, &channels[c], sizeof(bits));
			hash ^= bits;
			hash *= 1099511628211ull;
		}
	}
	read_mean = counted > 0 ? float(sum / counted) : 0.0f;
	read_min = counted > 0 ? lo : 0.0f;
	read_max = counted > 0 ? hi : 0.0f;
	read_hash = hash;
	read_luminance.clear();
	if (pixels <= 64 * 64) {
		read_luminance.resize(pixels);
		float *out = read_luminance.ptrw();
		for (int i = 0; i < pixels; i++) {
			const float weight = texels[i * 4 + 3];
			out[i] = weight > 0.0f ? (texels[i * 4] + texels[i * 4 + 1] + texels[i * 4 + 2]) / (3.0f * weight) : 0.0f;
		}
	}
}

void NextGIReference::fill_info(Dictionary &r_info) const {
	r_info["reference_samples"] = int64_t(samples);
	r_info["reference_mean"] = read_mean;
	r_info["reference_min"] = read_min;
	r_info["reference_max"] = read_max;
	r_info["reference_hash"] = String::num_uint64(read_hash, 16);
	r_info["reference_invalid"] = scene_invalid;
	r_info["reference_width"] = int64_t(width);
	r_info["reference_height"] = int64_t(height);
	if (!read_luminance.is_empty()) {
		r_info["reference_luminance"] = read_luminance;
	}
	// GodotNext: nextgi_reference_capture (#263).
	r_info["reference_resets"] = int64_t(resets);
	r_info["reference_exposure"] = exposure;
	if (!read_accumulation.is_empty()) {
		r_info["reference_accumulation"] = read_accumulation;
	}
	if (!read_reflection_guides.is_empty()) {
		r_info["reference_reflection_guides"] = read_reflection_guides;
	}
}

void NextGIReference::composite(RID p_color) {
	if (!is_active()) {
		return;
	}
	if (_refuse_without_hwrt()) {
		return;
	}
	if (scene_invalid || samples == 0 || !accumulation.is_valid() || !p_color.is_valid() || shader == nullptr || !uniform_set.is_valid()) {
		return;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr) {
		return;
	}
	const RD::TextureFormat format = rd->texture_get_format(p_color);
	if (format.format != RD::DATA_FORMAT_R16G16B16A16_SFLOAT) {
		return;
	}
	_free_uniform_set();
	NextGI *next_gi = NextGI::get_singleton();
	if (next_gi == nullptr || next_gi->get_rt_scene() == nullptr) {
		return;
	}
	RID records = next_gi->get_rt_scene()->get_instance_record_buffer();
	RID surfaces = next_gi->get_rt_scene()->get_surface_record_buffer();
	if (!records.is_valid()) {
		records = dummy_buffer;
	}
	if (!surfaces.is_valid()) {
		surfaces = dummy_buffer;
	}
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
	RD::Uniform sky_uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 14, Vector<RID>({ sky_sampler, dummy_sky_texture }));
	RD::Uniform uniforms[15] = {
		RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, next_gi->get_rt_scene()->get_tlas()),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, accumulation),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, p_color),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, material_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, light_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, records),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, surfaces),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, chunks[0]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, chunks[1]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, chunks[2]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, chunks[3]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, params_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 12, reflection_guides),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 13, reflection_distance),
		sky_uniform,
	};
	uniform_set = rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 15), shader->shader.version_get_shader(shader->version, 1), 0);
	if (!uniform_set.is_valid()) {
		return;
	}
	const RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, shader->composite);
	rd->compute_list_bind_uniform_set(list, uniform_set, 0);
	rd->compute_list_dispatch(list, (width + 7) / 8, (height + 7) / 8, 1);
	rd->compute_list_end();
	_free_uniform_set();
}
