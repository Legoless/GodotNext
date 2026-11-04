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
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_scene.h"
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

struct NextGIRefGPUMaterial {
	float albedo_metal[4] = { 0.8f, 0.8f, 0.8f, 0.0f };
	float emission_roughness[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	float specular_mode[4] = { 0.5f, 1.0f, 0.0f, 0.0f };
};

struct NextGIRefGPUParams {
	float cam_origin_fov[4] = {};
	float cam_forward_aspect[4] = {};
	float cam_up[4] = {};
	float cam_right[4] = {};
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

static float _nextgi_reference_float_param(RID p_material, const char *p_name, float p_default) {
	MaterialStorage *storage = MaterialStorage::get_singleton();
	if (storage == nullptr || !storage->owns_material(p_material)) {
		return p_default;
	}
	Variant value = storage->material_get_param(p_material, p_name);
	if (value.get_type() == Variant::FLOAT || value.get_type() == Variant::INT) {
		return float(value);
	}
	return p_default;
}

static Color _nextgi_reference_color_param(RID p_material, const char *p_name, const Color &p_default) {
	MaterialStorage *storage = MaterialStorage::get_singleton();
	if (storage == nullptr || !storage->owns_material(p_material)) {
		return p_default;
	}
	Variant value = storage->material_get_param(p_material, p_name);
	if (value.get_type() == Variant::COLOR) {
		return value;
	}
	return p_default;
}

static bool _nextgi_reference_has_texture(RID p_material, const char *p_name) {
	MaterialStorage *storage = MaterialStorage::get_singleton();
	if (storage == nullptr || !storage->owns_material(p_material)) {
		return false;
	}
	Variant value = storage->material_get_param(p_material, p_name);
	return value.get_type() == Variant::RID && RID(value).is_valid();
}

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
		RID resources[5] = { accumulation, material_buffer, light_buffer, params_buffer, estimator_buffer };
		for (RID resource : resources) {
			if (resource.is_valid()) {
				rd->free_rid(resource);
			}
		}
		if (dummy_buffer.is_valid()) {
			rd->free_rid(dummy_buffer);
		}
	}
	accumulation = RID();
	material_buffer = RID();
	light_buffer = RID();
	params_buffer = RID();
	estimator_buffer = RID();
	dummy_buffer = RID();
	tables_current = false;
	samples = 0;
	_nextgi_reference_samples = 0;
}

bool NextGIReference::_ensure_shader() {
	if (shader != nullptr && shader->version.is_valid()) {
		return true;
	}
	NextGI *next_gi = NextGI::get_singleton();
	ERR_FAIL_NULL_V(next_gi, false);
	ERR_FAIL_COND_V(!nextgi_reference_shader_allowed(next_gi->can_initialize_ray_query_shaders()), false);
	shader = memnew(ShaderState);
	Vector<String> defines;
	defines.push_back("");
	defines.push_back("#define MODE_COMPOSITE\n");
	shader->shader.initialize(defines);
	shader->version = shader->shader.version_create(false);
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
	uint64_t hash = uint64_t(GLOBAL_GET("rendering/global_illumination/nextgi/debug/reference_seed"));
	hash = hash * 16777619u ^ uint64_t(GLOBAL_GET("rendering/global_illumination/nextgi/debug/reference_mode"));
	NextGI *next_gi = NextGI::get_singleton();
	if (next_gi != nullptr && next_gi->get_rt_scene() != nullptr) {
		hash = hash * 16777619u ^ next_gi->get_rt_scene()->get_stats().instance_count;
	}
	if (p_render_data != nullptr && p_render_data->scene_data != nullptr) {
		const Vector3 origin = p_render_data->scene_data->cam_transform.origin;
		hash = hash * 16777619u ^ uint64_t(origin.x * 100.0);
		hash = hash * 16777619u ^ uint64_t(origin.y * 100.0);
		hash = hash * 16777619u ^ uint64_t(origin.z * 100.0);
	}
	return hash;
}

bool NextGIReference::_build_tables(const RenderDataRD *p_render_data, uint32_t &r_surface_count, uint32_t &r_light_count) {
	if (tables_current && material_buffer.is_valid() && light_buffer.is_valid()) {
		r_surface_count = cached_surface_count;
		r_light_count = cached_light_count;
		return true;
	}
	r_surface_count = 0;
	r_light_count = 0;
	scene_invalid = false;
	LocalVector<NextGIRefGPUMaterial> materials;
	const RendererSceneRender::RenderSDFGIUpdateData *update = p_render_data->sdfgi_update_data;
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (update != nullptr && update->nextgi_rt_instances != nullptr && mesh_storage != nullptr) {
		for (const RendererSceneRender::NextGIRTInstance &fed : *update->nextgi_rt_instances) {
			RenderGeometryInstanceBase *geometry = static_cast<RenderGeometryInstanceBase *>(fed.geometry_instance);
			if (geometry == nullptr || geometry->data == nullptr) {
				continue;
			}
			const RID mesh = geometry->data->base;
			if (!mesh_storage->owns_mesh(mesh)) {
				continue;
			}
			const bool dirty = mesh_storage->mesh_nextgi_rt_is_blas_dirty(mesh);
			const LocalVector<MeshStorage::NextGIRTPart> *parts = mesh_storage->mesh_nextgi_rt_get_parts(mesh);
			if (parts == nullptr) {
				continue;
			}
			for (const MeshStorage::NextGIRTPart &part : *parts) {
				if (!nextgi_rt_part_enters_tlas(dirty, part.blas.is_valid())) {
					continue;
				}
				for (const NextGIRTPartGeometry &part_geometry : part.geometries) {
					RID material = geometry->data->material_override;
					if (!material.is_valid() && part_geometry.surface < uint32_t(geometry->data->surface_materials.size())) {
						material = geometry->data->surface_materials[part_geometry.surface];
					}
					if (!material.is_valid()) {
						material = mesh_storage->mesh_surface_get_material(mesh, int(part_geometry.surface));
					}
					NextGIRefGPUMaterial gpu;
					if (material.is_valid() && (_nextgi_reference_has_texture(material, "texture_albedo") || _nextgi_reference_has_texture(material, "texture_emission"))) {
						if (!scene_invalid) {
							WARN_PRINT("NextGI reference path tracer ignores textured materials. This scene's reference is invalid.");
						}
						scene_invalid = true;
					}
					if (material.is_valid()) {
						const Color albedo = _nextgi_reference_color_param(material, "albedo", Color(0.8, 0.8, 0.8));
						const Color emission = _nextgi_reference_color_param(material, "emission", Color(0, 0, 0));
						const float energy = _nextgi_reference_float_param(material, "emission_energy", 1.0f);
						gpu.albedo_metal[0] = albedo.r;
						gpu.albedo_metal[1] = albedo.g;
						gpu.albedo_metal[2] = albedo.b;
						gpu.albedo_metal[3] = _nextgi_reference_float_param(material, "metallic", 0.0f);
						gpu.emission_roughness[0] = emission.r * energy;
						gpu.emission_roughness[1] = emission.g * energy;
						gpu.emission_roughness[2] = emission.b * energy;
						gpu.emission_roughness[3] = _nextgi_reference_float_param(material, "roughness", 1.0f);
						gpu.specular_mode[0] = _nextgi_reference_float_param(material, "specular", 0.5f);
						// Specular 0 and roughness 1 is the Lambertian furnace. Burley is the StandardMaterial3D default otherwise.
						gpu.specular_mode[1] = (gpu.specular_mode[0] <= 0.0f && gpu.emission_roughness[3] >= 1.0f) ? 0.0f : 1.0f;
					}
					materials.push_back(gpu);
				}
			}
		}
	}
	r_surface_count = materials.size();
	if (r_surface_count == 0) {
		NextGIRefGPUMaterial fallback;
		materials.push_back(fallback);
		r_surface_count = 1;
	}

	LocalVector<float> lights;
	lights.resize(8 * 12);
	for (uint32_t i = 0; i < lights.size(); i++) {
		lights[i] = 0.0f;
	}
	LightStorage *light_storage = LightStorage::get_singleton();
	if (light_storage != nullptr && p_render_data->lights != nullptr) {
		for (uint32_t i = 0; i < p_render_data->lights->size() && r_light_count < 8; i++) {
			const RID instance = (*p_render_data->lights)[i];
			const RID light = light_storage->light_instance_get_base_light(instance);
			if (!light.is_valid() || light_storage->light_get_type(light) == RSE::LIGHT_DIRECTIONAL) {
				continue;
			}
			const Transform3D xform = light_storage->light_instance_get_base_transform(instance);
			const Color color = light_storage->light_get_color(light);
			const float energy = light_storage->light_get_param(light, RSE::LIGHT_PARAM_ENERGY);
			const float range = MAX(light_storage->light_get_param(light, RSE::LIGHT_PARAM_RANGE), 0.001f);
			const float decay = light_storage->light_get_param(light, RSE::LIGHT_PARAM_ATTENUATION);
			const float specular = light_storage->light_get_param(light, RSE::LIGHT_PARAM_SPECULAR);
			const uint32_t base = r_light_count * 12;
			lights[base + 0] = xform.origin.x;
			lights[base + 1] = xform.origin.y;
			lights[base + 2] = xform.origin.z;
			lights[base + 3] = range;
			lights[base + 4] = color.r;
			lights[base + 5] = color.g;
			lights[base + 6] = color.b;
			lights[base + 7] = energy;
			lights[base + 8] = decay;
			lights[base + 9] = specular;
			r_light_count++;
		}
	}

	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	if (material_buffer.is_valid()) {
		rd->free_rid(material_buffer);
	}
	if (light_buffer.is_valid()) {
		rd->free_rid(light_buffer);
	}
	Vector<uint8_t> material_bytes;
	material_bytes.resize(materials.size() * sizeof(NextGIRefGPUMaterial));
	memcpy(material_bytes.ptrw(), materials.ptr(), material_bytes.size());
	material_buffer = rd->storage_buffer_create(material_bytes.size(), material_bytes);
	Vector<uint8_t> light_bytes;
	light_bytes.resize(lights.size() * sizeof(float));
	memcpy(light_bytes.ptrw(), lights.ptr(), light_bytes.size());
	light_buffer = rd->storage_buffer_create(light_bytes.size(), light_bytes);
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
	params.direct_only = int(GLOBAL_GET("rendering/global_illumination/nextgi/debug/reference_mode")) == 2 ? 1u : 0u;
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
	const uint64_t hash = _scene_hash(p_render_data);
	const bool reset = hash != scene_hash || uint32_t(screen.x) != width || uint32_t(screen.y) != height || !accumulation.is_valid();
	if (reset) {
		scene_hash = hash;
		samples = 0;
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
	}
	uint32_t surface_count = 0;
	uint32_t light_count = 0;
	if (!_build_tables(p_render_data, surface_count, light_count) || scene_invalid) {
		_nextgi_reference_samples = samples;
		return;
	}
	if (!dummy_buffer.is_valid()) {
		dummy_buffer = rd->storage_buffer_create(16);
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
	params.cam_origin_fov[0] = camera.origin.x;
	params.cam_origin_fov[1] = camera.origin.y;
	params.cam_origin_fov[2] = camera.origin.z;
	params.cam_origin_fov[3] = Math::abs(projection.columns[1][1]) > 0.0f ? 2.0f * Math::atan(1.0f / Math::abs(projection.columns[1][1])) : Math::deg_to_rad(75.0f);
	const Vector3 forward = -camera.basis.get_column(Vector3::AXIS_Z);
	const Vector3 up = camera.basis.get_column(Vector3::AXIS_Y);
	const Vector3 right = camera.basis.get_column(Vector3::AXIS_X);
	params.cam_forward_aspect[0] = forward.x;
	params.cam_forward_aspect[1] = forward.y;
	params.cam_forward_aspect[2] = forward.z;
	params.cam_forward_aspect[3] = float(screen.x) / float(screen.y);
	params.cam_up[0] = up.x;
	params.cam_up[1] = up.y;
	params.cam_up[2] = up.z;
	params.cam_right[0] = right.x;
	params.cam_right[1] = right.y;
	params.cam_right[2] = right.z;
	params.width = width;
	params.height = height;
	params.sample_index = samples;
	params.direct_only = int(GLOBAL_GET("rendering/global_illumination/nextgi/debug/reference_mode")) == 2 ? 1u : 0u;
	params.seed = uint32_t(int(GLOBAL_GET("rendering/global_illumination/nextgi/debug/reference_seed")));
	params.light_count = light_count;
	params.surface_count = surface_count;
	params.record_count = next_gi->get_rt_scene()->get_stats().instance_count;
	params.exposure = 1.0f;
	if (p_render_data->camera_attributes.is_valid() && RSG::camera_attributes != nullptr) {
		params.exposure = RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
	}
	params.clear = reset ? 1.0f : 0.0f;
	params.samples_per_pixel = 32.0f;
	RendererSceneRenderRD *scene = RendererSceneRenderRD::get_singleton();
	if (scene != nullptr && p_render_data->environment.is_valid()) {
		const Color bg = scene->environment_get_bg_color(p_render_data->environment);
		const float energy = scene->environment_get_bg_energy_multiplier(p_render_data->environment);
		params.sky_color[0] = bg.r * energy;
		params.sky_color[1] = bg.g * energy;
		params.sky_color[2] = bg.b * energy;
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
	RD::Uniform uniforms[12] = {
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
	};
	uniform_set = rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 12), shader->shader.version_get_shader(shader->version, 0), 0);
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
	RD::Uniform uniforms[12] = {
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
	};
	uniform_set = rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 12), shader->shader.version_get_shader(shader->version, 1), 0);
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
