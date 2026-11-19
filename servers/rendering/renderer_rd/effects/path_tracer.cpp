/**************************************************************************/
/*  path_tracer.cpp                                                       */
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

#include "path_tracer.h"

#include "core/config/project_settings.h"
#include "servers/rendering/renderer_geometry_instance.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_hit_materials.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_scene.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/path_tracer.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/rendering_server_default.h"
#include "servers/rendering/rendering_server_globals.h"

using namespace RendererRD;

#define RB_SCOPE_PATH_TRACER SNAME("path_tracer")

namespace RendererRD {

/* RULES */

PathTracerSettings path_tracer_effective_settings(const PathTracerSettings &p_settings, RSE::ViewportDebugDraw p_debug_draw) {
	PathTracerSettings settings = p_settings;
	settings.enabled = p_settings.enabled || p_debug_draw == RSE::VIEWPORT_DEBUG_DRAW_PATH_TRACER;
	settings.samples = RenderingServerTypes::path_tracing_validate_samples(p_settings.samples);
	settings.max_bounces = RenderingServerTypes::path_tracing_validate_max_bounces(p_settings.max_bounces);
	settings.firefly_clamp = RenderingServerTypes::path_tracing_validate_firefly_clamp(p_settings.firefly_clamp);
	settings.reference_match = p_settings.reference_match;
	return settings;
}

const char *path_tracer_status_name(PathTracerStatus p_status) {
	switch (p_status) {
		case PATH_TRACER_STATUS_OFF:
			return "OFF";
		case PATH_TRACER_STATUS_ACTIVE:
			return "ACTIVE";
		case PATH_TRACER_STATUS_CONVERGED:
			return "CONVERGED";
		case PATH_TRACER_STATUS_NO_NEXTGI:
			return "NO_NEXTGI";
		case PATH_TRACER_STATUS_NOT_HWRT:
			return "NOT_HWRT";
		case PATH_TRACER_STATUS_NO_TLAS:
			return "NO_TLAS";
		case PATH_TRACER_STATUS_MULTIVIEW:
			return "MULTIVIEW";
		case PATH_TRACER_STATUS_REFERENCE_MODE:
			return "REFERENCE_MODE";
		case PATH_TRACER_STATUS_UNSUPPORTED_DEVICE:
			return "UNSUPPORTED_DEVICE";
		case PATH_TRACER_STATUS_MAX:
			break;
	}
	return "UNKNOWN";
}

const char *path_tracer_backend_name(PathTracerBackend p_backend) {
	switch (p_backend) {
		case PATH_TRACER_BACKEND_NONE:
			return "NONE";
		case PATH_TRACER_BACKEND_HIT_GROUPS:
			return "HIT_GROUPS";
		case PATH_TRACER_BACKEND_INLINE:
			return "INLINE";
		case PATH_TRACER_BACKEND_MAX:
			break;
	}
	return "UNKNOWN";
}

PathTracerBackend path_tracer_backend(bool p_hit_groups_traceable, bool p_hit_raygen) {
	return p_hit_groups_traceable && p_hit_raygen ? PATH_TRACER_BACKEND_HIT_GROUPS : PATH_TRACER_BACKEND_INLINE;
}

PathTracerDeviceSupport path_tracer_device_support(bool p_metal, int p_apple_family, bool p_ray_query) {
	if (p_metal && p_apple_family > 0 && p_apple_family < 8) {
		return PATH_TRACER_DEVICE_APPLE_FAMILY;
	}
	if (!p_ray_query) {
		return PATH_TRACER_DEVICE_NO_RAY_QUERY;
	}
	return p_metal ? PATH_TRACER_DEVICE_EXPERIMENTAL : PATH_TRACER_DEVICE_SUPPORTED;
}

const char *path_tracer_device_support_name(PathTracerDeviceSupport p_support) {
	switch (p_support) {
		case PATH_TRACER_DEVICE_SUPPORTED:
			return "SUPPORTED";
		case PATH_TRACER_DEVICE_EXPERIMENTAL:
			return "EXPERIMENTAL";
		case PATH_TRACER_DEVICE_NO_RAY_QUERY:
			return "NO_RAY_QUERY";
		case PATH_TRACER_DEVICE_APPLE_FAMILY:
			return "APPLE_FAMILY";
	}
	return "UNKNOWN";
}

bool path_tracer_hit_is_front(const Vector3 &p_p0, const Vector3 &p_p1, const Vector3 &p_p2, const Basis &p_object_to_world, const Vector3 &p_ray_dir) {
	const Vector3 outward_object = (p_p2 - p_p0).cross(p_p1 - p_p0);
	// The inverse transpose keeps the outward side through a mirror, where the corners' own winding flips.
	const Vector3 outward_world = p_object_to_world.inverse().transposed().xform(outward_object);
	return outward_world.dot(p_ray_dir) < 0.0f;
}

const char *path_tracer_reset_reason_name(uint32_t p_reason_index) {
	static const char *names[PATH_TRACER_RESET_REASON_COUNT] = {
		"history",
		"settings",
		"camera",
		"instances",
		"lights",
		"materials",
		"scene",
		"hit_groups",
	};
	return p_reason_index < PATH_TRACER_RESET_REASON_COUNT ? names[p_reason_index] : "unknown";
}

uint32_t path_tracer_reset_reasons(bool p_has_history, const PathTracerFrameState &p_previous, const PathTracerFrameState &p_current) {
	uint32_t reasons = 0;
	if (!p_has_history || p_previous.size != p_current.size) {
		reasons |= PATH_TRACER_RESET_HISTORY;
	}
	if (p_previous.settings != p_current.settings) {
		reasons |= PATH_TRACER_RESET_SETTINGS;
	}
	if (p_previous.camera != p_current.camera) {
		reasons |= PATH_TRACER_RESET_CAMERA;
	}
	if (p_current.instance_events > 0) {
		reasons |= PATH_TRACER_RESET_INSTANCES;
	}
	if (p_previous.lights != p_current.lights || p_current.light_events > 0 || p_current.directional_light_event) {
		reasons |= PATH_TRACER_RESET_LIGHTS;
	}
	if (p_previous.material_serial != p_current.material_serial) {
		reasons |= PATH_TRACER_RESET_MATERIALS;
	}
	if (p_previous.scene != p_current.scene) {
		reasons |= PATH_TRACER_RESET_SCENE;
	}
	if (p_previous.hit_groups_serial != p_current.hit_groups_serial) {
		reasons |= PATH_TRACER_RESET_HIT_GROUPS;
	}
	return reasons;
}

// FNV-1a, one byte at a time, as the #263 reset key.
uint64_t path_tracer_hash_bytes(uint64_t p_hash, const void *p_data, size_t p_size) {
	const uint8_t *bytes = static_cast<const uint8_t *>(p_data);
	for (size_t i = 0; i < p_size; i++) {
		p_hash = (p_hash ^ bytes[i]) * 1099511628211ull;
	}
	return p_hash;
}

static constexpr uint64_t PATH_TRACER_HASH_BASIS = 1469598103934665603ull;

static uint64_t _path_tracer_hash_real(uint64_t p_hash, double p_value) {
	return path_tracer_hash_bytes(p_hash, &p_value, sizeof(p_value));
}

uint64_t path_tracer_hash_camera(const Transform3D &p_camera, const Projection &p_projection) {
	uint64_t hash = PATH_TRACER_HASH_BASIS;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			hash = _path_tracer_hash_real(hash, p_camera.basis.rows[i][j]);
		}
		hash = _path_tracer_hash_real(hash, p_camera.origin[i]);
	}
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			hash = _path_tracer_hash_real(hash, p_projection.columns[i][j]);
		}
	}
	return hash;
}

uint64_t path_tracer_hash_settings(const PathTracerSettings &p_settings, PathTracerBackend p_backend, float p_exposure) {
	const uint32_t words[3] = { uint32_t(p_settings.max_bounces), uint32_t(p_backend), p_settings.reference_match ? 1u : 0u };
	uint64_t hash = path_tracer_hash_bytes(PATH_TRACER_HASH_BASIS, words, sizeof(words));
	hash = path_tracer_hash_bytes(hash, &p_settings.firefly_clamp, sizeof(p_settings.firefly_clamp));
	// The clamp limits the exposed radiance (path_tracer_clamp()): in radiance units it moves with the exposure.
	if (p_settings.firefly_clamp > 0.0f) {
		hash = path_tracer_hash_bytes(hash, &p_exposure, sizeof(p_exposure));
	}
	return hash;
}

uint64_t path_tracer_hash_light(uint64_t p_hash, uint32_t p_type, const Transform3D &p_transform, const Color &p_color, const float *p_params, uint32_t p_param_count) {
	uint64_t hash = path_tracer_hash_bytes(p_hash, &p_type, sizeof(p_type));
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			hash = _path_tracer_hash_real(hash, p_transform.basis.rows[i][j]);
		}
		hash = _path_tracer_hash_real(hash, p_transform.origin[i]);
	}
	const float color[4] = { p_color.r, p_color.g, p_color.b, p_color.a };
	hash = path_tracer_hash_bytes(hash, color, sizeof(color));
	return path_tracer_hash_bytes(hash, p_params, p_param_count * sizeof(float));
}

// The camera block of the D16 reference (#27), for both shaders: the vertical field of view from the projection,
// and the basis.
PathTracerGPUCamera path_tracer_camera(const Transform3D &p_camera, const Projection &p_projection, const Size2i &p_size) {
	PathTracerGPUCamera camera;
	camera.cam_origin_fov[0] = p_camera.origin.x;
	camera.cam_origin_fov[1] = p_camera.origin.y;
	camera.cam_origin_fov[2] = p_camera.origin.z;
	camera.cam_origin_fov[3] = Math::abs(p_projection.columns[1][1]) > 0.0f ? 2.0f * Math::atan(1.0f / Math::abs(p_projection.columns[1][1])) : Math::deg_to_rad(75.0f);
	const Vector3 forward = -p_camera.basis.get_column(Vector3::AXIS_Z);
	const Vector3 up = p_camera.basis.get_column(Vector3::AXIS_Y);
	const Vector3 right = p_camera.basis.get_column(Vector3::AXIS_X);
	camera.cam_forward_aspect[0] = forward.x;
	camera.cam_forward_aspect[1] = forward.y;
	camera.cam_forward_aspect[2] = forward.z;
	camera.cam_forward_aspect[3] = float(p_size.x) / float(p_size.y);
	camera.cam_up[0] = up.x;
	camera.cam_up[1] = up.y;
	camera.cam_up[2] = up.z;
	camera.cam_right[0] = right.x;
	camera.cam_right[1] = right.y;
	camera.cam_right[2] = right.z;
	return camera;
}

PathTracerGPURays path_tracer_rays(const Transform3D &p_camera, const Projection &p_projection, bool p_orthogonal, const Size2i &p_size) {
	PathTracerGPURays rays;
	// Godot's projections map the near plane to a normalized device depth of -1. View space first, for the cone.
	const Projection inverse = p_projection.inverse();
	const Vector3 center_view = inverse.xform(Vector3(0.0, 0.0, -1.0));
	const Vector3 dy_view = inverse.xform(Vector3(0.0, 1.0, -1.0)) - center_view;
	const Vector3 center = p_camera.xform(center_view);
	const Vector3 dx = p_camera.xform(inverse.xform(Vector3(1.0, 0.0, -1.0))) - center;
	const Vector3 dy = p_camera.xform(center_view + dy_view) - center;
	const float pixel_height = 2.0f * dy_view.length() / float(MAX(p_size.y, 1));
	rays.center[0] = center.x;
	rays.center[1] = center.y;
	rays.center[2] = center.z;
	rays.center[3] = p_orthogonal ? 1.0f : 0.0f;
	rays.dx[0] = dx.x;
	rays.dx[1] = dx.y;
	rays.dx[2] = dx.z;
	rays.dy[0] = dy.x;
	rays.dy[1] = dy.y;
	rays.dy[2] = dy.z;
	if (p_orthogonal) {
		// One pixel wide at every distance.
		rays.dx[3] = pixel_height;
		rays.dy[3] = 0.0f;
	} else {
		// One pixel's angle: the pixel's height on the near plane over the near plane's distance.
		const float near_distance = MAX(-center_view.z, 1e-6f);
		rays.dx[3] = 0.0f;
		rays.dy[3] = pixel_height / near_distance;
	}
	return rays;
}

} // namespace RendererRD

/* VIEW */

void PathTracerView::clear_history() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr && accumulation.is_valid()) {
		rd->free_rid(accumulation);
	}
	accumulation = RID();
	size = Size2i();
	samples = 0;
	has_state = false;
}

void PathTracerView::configure(RenderSceneBuffersRD *p_render_buffers) {
	// A new size or a new configuration: the next traced frame starts a new history.
	clear_history();
}

void PathTracerView::free_data() {
	clear_history();
}

PathTracerView::~PathTracerView() {
	clear_history();
}

void PathTracerView::fill_info(Dictionary &r_info) const {
	r_info["status"] = path_tracer_status_name(status);
	r_info["backend"] = path_tracer_backend_name(backend);
	r_info["samples"] = int64_t(samples);
	r_info["target_samples"] = int64_t(target);
	r_info["resets"] = int64_t(resets);
	r_info["width"] = int64_t(size.x);
	r_info["height"] = int64_t(size.y);
	r_info["max_bounces"] = int64_t(settings.max_bounces);
	r_info["firefly_clamp"] = settings.firefly_clamp;
	r_info["exposure"] = exposure;
	r_info["last_reset_frame"] = int64_t(last_reset_frame);
	r_info["last_traced_frame"] = int64_t(last_traced_frame);
	r_info["frames_traced"] = int64_t(frames_traced);
	r_info["textured_materials"] = int64_t(textured_materials);
	r_info["reference_match"] = settings.reference_match;
	r_info["device_support"] = path_tracer_device_support_name(device_support);
	PackedStringArray last;
	Dictionary counts;
	for (uint32_t i = 0; i < PATH_TRACER_RESET_REASON_COUNT; i++) {
		if (last_reset_reasons & (1u << i)) {
			last.push_back(path_tracer_reset_reason_name(i));
		}
		counts[path_tracer_reset_reason_name(i)] = int64_t(reset_counts[i]);
	}
	r_info["last_reset_reasons"] = last;
	r_info["reset_counts"] = counts;
	if (!read_accumulation.is_empty()) {
		r_info["accumulation"] = read_accumulation;
		r_info["mean"] = read_mean;
		r_info["nonfinite"] = int64_t(read_nonfinite);
		r_info["hash"] = String::num_uint64(read_hash, 16);
	}
}

/* TABLES */

static float _path_tracer_float_param(RID p_material, const char *p_name, float p_default) {
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

static Color _path_tracer_color_param(RID p_material, const char *p_name, const Color &p_default) {
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

static bool _path_tracer_has_texture(RID p_material, const char *p_name) {
	MaterialStorage *storage = MaterialStorage::get_singleton();
	if (storage == nullptr || !storage->owns_material(p_material)) {
		return false;
	}
	Variant value = storage->material_get_param(p_material, p_name);
	return value.get_type() == Variant::RID && RID(value).is_valid();
}

static RSE::CullMode _path_tracer_cull_mode(RID p_material) {
	MaterialStorage *storage = MaterialStorage::get_singleton();
	if (storage == nullptr || !p_material.is_valid() || !storage->owns_material(p_material)) {
		return RSE::CULL_MODE_BACK; // The default material is one-sided.
	}
	const MaterialStorage::ShaderData *shader_data = storage->material_get_shader_data(p_material);
	return shader_data != nullptr ? shader_data->get_cull_mode() : RSE::CULL_MODE_BACK;
}

// The D16 table entry of a material (#27): StandardMaterial3D's constant parameters, without textures. The D16
// reference takes the color parameters' values as they are; the product (p_linear_colors) converts them to linear,
// as the renderer does for the material shaders (and so for the hit groups).
static PathTracerGPUMaterial _path_tracer_table_material(RID p_material, uint32_t &r_textured, bool p_linear_colors) {
	PathTracerGPUMaterial gpu;
	if (p_material.is_valid() && (_path_tracer_has_texture(p_material, "texture_albedo") || _path_tracer_has_texture(p_material, "texture_emission"))) {
		r_textured++;
	}
	if (p_material.is_valid()) {
		Color albedo = _path_tracer_color_param(p_material, "albedo", Color(0.8, 0.8, 0.8));
		Color emission = _path_tracer_color_param(p_material, "emission", Color(0, 0, 0));
		if (p_linear_colors) {
			albedo = albedo.srgb_to_linear();
			emission = emission.srgb_to_linear();
		}
		const float energy = _path_tracer_float_param(p_material, "emission_energy", 1.0f);
		gpu.albedo_metal[0] = albedo.r;
		gpu.albedo_metal[1] = albedo.g;
		gpu.albedo_metal[2] = albedo.b;
		gpu.albedo_metal[3] = _path_tracer_float_param(p_material, "metallic", 0.0f);
		gpu.emission_roughness[0] = emission.r * energy;
		gpu.emission_roughness[1] = emission.g * energy;
		gpu.emission_roughness[2] = emission.b * energy;
		gpu.emission_roughness[3] = _path_tracer_float_param(p_material, "roughness", 1.0f);
		gpu.specular_mode[0] = _path_tracer_float_param(p_material, "specular", 0.5f);
		// Specular 0 and roughness 1 is the Lambertian furnace. Burley is the StandardMaterial3D default otherwise.
		gpu.specular_mode[1] = (gpu.specular_mode[0] <= 0.0f && gpu.emission_roughness[3] >= 1.0f) ? 0.0f : 1.0f;
	}
	gpu.specular_mode[2] = float(_path_tracer_cull_mode(p_material));
	return gpu;
}

static RID _path_tracer_surface_material(RenderGeometryInstanceBase *p_geometry, RID p_mesh, uint32_t p_surface) {
	RID material = p_geometry->data->material_override;
	if (!material.is_valid() && p_surface < uint32_t(p_geometry->data->surface_materials.size())) {
		material = p_geometry->data->surface_materials[p_surface];
	}
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (!material.is_valid() && mesh_storage != nullptr && mesh_storage->owns_mesh(p_mesh)) {
		material = mesh_storage->mesh_surface_get_material(p_mesh, int(p_surface));
	}
	return material;
}

// Up to PATH_TRACER_MAX_LIGHTS omni and spot lights, 12 floats each (the D16 light table; spot cones are #169's).
static uint32_t _path_tracer_build_lights(const RenderDataRD *p_render_data, LocalVector<float> &r_lights) {
	r_lights.resize(PATH_TRACER_MAX_LIGHTS * PATH_TRACER_LIGHT_FLOATS);
	for (uint32_t i = 0; i < r_lights.size(); i++) {
		r_lights[i] = 0.0f;
	}
	uint32_t light_count = 0;
	LightStorage *light_storage = LightStorage::get_singleton();
	if (light_storage == nullptr || p_render_data->lights == nullptr) {
		return 0;
	}
	for (uint32_t i = 0; i < p_render_data->lights->size() && light_count < PATH_TRACER_MAX_LIGHTS; i++) {
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
		const uint32_t base = light_count * PATH_TRACER_LIGHT_FLOATS;
		r_lights[base + 0] = xform.origin.x;
		r_lights[base + 1] = xform.origin.y;
		r_lights[base + 2] = xform.origin.z;
		r_lights[base + 3] = range;
		r_lights[base + 4] = color.r;
		r_lights[base + 5] = color.g;
		r_lights[base + 6] = color.b;
		r_lights[base + 7] = energy;
		r_lights[base + 8] = decay;
		r_lights[base + 9] = specular;
		light_count++;
	}
	return light_count;
}

static void _path_tracer_finish_tables(PathTracerTables &r_tables, uint64_t p_hash) {
	r_tables.surface_count = r_tables.materials.size();
	if (r_tables.surface_count == 0) {
		r_tables.materials.push_back(PathTracerGPUMaterial());
		r_tables.surface_count = 1;
	}
	const uint64_t materials_hash = path_tracer_hash_bytes(p_hash, r_tables.materials.ptr(), r_tables.materials.size() * sizeof(PathTracerGPUMaterial));
	r_tables.hash = path_tracer_hash_bytes(materials_hash, r_tables.lights.ptr(), r_tables.lights.size() * sizeof(float));
}

// Every light the view has, of any type (the D16 table holds omni and spot lights only; #169 traces the rest), so a
// change to any of them restarts the history even where NextGI raises no light event for it.
static uint64_t _path_tracer_hash_lights(const RenderDataRD *p_render_data) {
	uint64_t hash = PATH_TRACER_HASH_BASIS;
	LightStorage *light_storage = LightStorage::get_singleton();
	if (light_storage == nullptr || p_render_data->lights == nullptr) {
		return hash;
	}
	for (uint32_t i = 0; i < p_render_data->lights->size(); i++) {
		const RID instance = (*p_render_data->lights)[i];
		const RID light = light_storage->light_instance_get_base_light(instance);
		if (!light.is_valid()) {
			continue;
		}
		float params[RSE::LIGHT_PARAM_MAX];
		for (int p = 0; p < RSE::LIGHT_PARAM_MAX; p++) {
			params[p] = light_storage->light_get_param(light, RSE::LightParam(p));
		}
		hash = path_tracer_hash_light(hash, uint32_t(light_storage->light_get_type(light)), light_storage->light_instance_get_base_transform(instance), light_storage->light_get_color(light), params, RSE::LIGHT_PARAM_MAX);
	}
	return hash;
}

void PathTracer::build_reference_tables(const RenderDataRD *p_render_data, PathTracerTables &r_tables) {
	// The D16 reference's walk (#27, #263), unchanged: the fed instances' parts that enter the TLAS, in order.
	r_tables = PathTracerTables();
	uint64_t hash = PATH_TRACER_HASH_BASIS;
	const RendererSceneRender::RenderSDFGIUpdateData *update = p_render_data->sdfgi_update_data;
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (update != nullptr && update->nextgi_rt_instances != nullptr && mesh_storage != nullptr) {
		for (const RendererSceneRender::NextGIRTInstance &fed : *update->nextgi_rt_instances) {
			RenderGeometryInstanceBase *geometry = static_cast<RenderGeometryInstanceBase *>(fed.geometry_instance);
			if (geometry == nullptr || geometry->data == nullptr) {
				continue;
			}
			const Transform3D transform = fed.has_rt_transform ? fed.rt_transform : geometry->transform;
			for (int i = 0; i < 3; i++) {
				for (int j = 0; j < 3; j++) {
					hash = _path_tracer_hash_real(hash, transform.basis.rows[i][j]);
				}
				hash = _path_tracer_hash_real(hash, transform.origin[i]);
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
					r_tables.materials.push_back(_path_tracer_table_material(_path_tracer_surface_material(geometry, mesh, part_geometry.surface), r_tables.textured_materials, false));
				}
			}
		}
	}
	r_tables.light_count = _path_tracer_build_lights(p_render_data, r_tables.lights);
	_path_tracer_finish_tables(r_tables, hash);
}

void PathTracer::build_tables(const RenderDataRD *p_render_data, const NextGIRTScene *p_rt_scene, PathTracerTables &r_tables, bool p_linear_colors) {
	// One entry per surface record of this frame's TLAS phase, from what the record draws, so the table lines
	// up with every record kind (MultiMesh elements, skinned slots, native NextGeo sets, far proxies).
	r_tables = PathTracerTables();
	uint64_t hash = PATH_TRACER_HASH_BASIS;
	if (p_rt_scene != nullptr) {
		const uint64_t records_hash = p_rt_scene->get_records_hash();
		hash = path_tracer_hash_bytes(hash, &records_hash, sizeof(records_hash));
		const LocalVector<NextGIRTScene::SurfaceSource> &sources = p_rt_scene->get_surface_sources();
		r_tables.materials.resize(sources.size());
		for (uint32_t i = 0; i < sources.size(); i++) {
			const NextGIRTScene::SurfaceSource &source = sources[i];
			RenderGeometryInstanceBase *geometry = static_cast<RenderGeometryInstanceBase *>(source.instance);
			RID material;
			if (geometry != nullptr && geometry->data != nullptr && source.surface != UINT32_MAX) {
				material = _path_tracer_surface_material(geometry, source.mesh, source.surface);
			}
			r_tables.materials[i] = _path_tracer_table_material(material, r_tables.textured_materials, p_linear_colors);
		}
	}
	r_tables.light_count = _path_tracer_build_lights(p_render_data, r_tables.lights);
	_path_tracer_finish_tables(r_tables, hash);
}

/* PATH TRACER */

namespace {

// The product params (PathTracerParams in path_tracer.glsl, HitPTParams in nextgi_hit_layout_inc.glsl): the D16
// reference's first 128 bytes, then the product word and the product's camera rays.
struct PathTracerGPUParams {
	PathTracerGPUCamera camera;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t sample_index = 0;
	uint32_t mode = 0; // The D16 mode word: 0 full.
	uint32_t seed = 1;
	uint32_t light_count = 0;
	uint32_t surface_count = 0;
	uint32_t record_count = 0;
	float sky_color[4] = {};
	float exposure = 1.0f;
	float clear = 0.0f;
	float samples_per_pixel = 1.0f;
	float pad = 0.0f;
	uint32_t max_bounces = 0;
	uint32_t flags = 0;
	uint32_t firefly_clamp_bits = 0;
	uint32_t pad1 = 0;
	PathTracerGPURays rays;
};
// NextGIHitMaterials binds a 256-byte stand-in for it in the #130 check.
static_assert(sizeof(PathTracerGPUParams) == 192, "The path tracer params must match path_tracer.glsl and nextgi_hit_layout_inc.glsl.");

// path_tracer_inc.glsl's product flags. PRODUCT: camera rays from the projection (PathTracerGPURays), random
// numbers decorrelated across pixels and samples, and black where a camera ray hits an instance without records.
// The reference match sets none, so it traces exactly as the D16 reference.
constexpr uint32_t PATH_TRACER_FLAG_CULL = 1u;
constexpr uint32_t PATH_TRACER_FLAG_JITTER = 2u;
constexpr uint32_t PATH_TRACER_FLAG_PRODUCT = 4u;

struct PathTracerCompositePush {
	uint32_t width = 0;
	uint32_t height = 0;
	float exposure = 1.0f;
	float pad = 0.0f;
};

} // namespace

struct PathTracer::ShaderState {
	PathTracerShaderRD shader;
	RID version;
	RID accumulate;
	RID composite;
	RID params_buffer;
};

Vector<String> PathTracer::get_shader_variant_defines() {
	Vector<String> defines;
	defines.push_back("");
	defines.push_back("#define MODE_COMPOSITE\n");
	return defines;
}

bool PathTracer::_ensure_shader() {
	if (shader != nullptr && shader->version.is_valid()) {
		return true;
	}
	if (logged_compile_failure) {
		return false;
	}
	NextGI *next_gi = NextGI::get_singleton();
	ERR_FAIL_NULL_V(next_gi, false);
	// A ray-query ShaderRD, created only under the hardware tier (D5).
	if (!next_gi->can_initialize_ray_query_shaders()) {
		return false;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	shader = memnew(ShaderState);
	shader->shader.initialize(get_shader_variant_defines());
	// Embedded so the shader baker bakes or skips the ray-query version per target (#40).
	shader->version = shader->shader.version_create(true);
	if (shader->version.is_valid()) {
		shader->accumulate = rd->compute_pipeline_create(shader->shader.version_get_shader(shader->version, 0));
		shader->composite = rd->compute_pipeline_create(shader->shader.version_get_shader(shader->version, 1));
	}
	if (!shader->version.is_valid() || !shader->accumulate.is_valid() || !shader->composite.is_valid()) {
		if (shader->version.is_valid()) {
			shader->shader.version_free(shader->version);
		}
		memdelete(shader);
		shader = nullptr;
		logged_compile_failure = true;
		ERR_FAIL_V_MSG(false, "The path tracer shader failed to compile.");
	}
	shader->params_buffer = rd->storage_buffer_create(sizeof(PathTracerGPUParams));
	next_gi->notify_ray_query_shader_initialized();
	return true;
}

PathTracerView *PathTracer::get_view(RenderSceneBuffersRD *p_render_buffers, bool p_create) {
	if (p_render_buffers == nullptr) {
		return nullptr;
	}
	if (!p_render_buffers->has_custom_data(RB_SCOPE_PATH_TRACER)) {
		if (!p_create) {
			return nullptr;
		}
		Ref<PathTracerView> created;
		created.instantiate();
		p_render_buffers->set_custom_data(RB_SCOPE_PATH_TRACER, created);
	}
	Ref<PathTracerView> view = p_render_buffers->get_custom_data(RB_SCOPE_PATH_TRACER);
	return view.ptr();
}

PathTracerSettings PathTracer::get_settings(const RenderSceneBuffersRD *p_render_buffers) {
	PathTracerSettings settings;
	if (p_render_buffers != nullptr) {
		settings.enabled = p_render_buffers->get_path_tracing_enabled();
		settings.samples = RenderingServerTypes::path_tracing_validate_samples(p_render_buffers->get_path_tracing_samples());
		settings.max_bounces = RenderingServerTypes::path_tracing_validate_max_bounces(p_render_buffers->get_path_tracing_max_bounces());
		settings.firefly_clamp = RenderingServerTypes::path_tracing_validate_firefly_clamp(p_render_buffers->get_path_tracing_firefly_clamp());
	}
	return settings;
}

void PathTracer::set_status(RenderSceneBuffersRD *p_render_buffers, PathTracerStatus p_status) {
	const PathTracerSettings settings = get_settings(p_render_buffers);
	PathTracerView *view = get_view(p_render_buffers, settings.enabled);
	if (view == nullptr) {
		return;
	}
	view->settings = settings;
	view->target = uint32_t(settings.samples);
	view->status = settings.enabled ? p_status : PATH_TRACER_STATUS_OFF;
	view->backend = PATH_TRACER_BACKEND_NONE;
	view->device_support = get_device_support();
	// Frames that are not traced see no events, so the history cannot continue past them.
	view->clear_history();
	// A view that waits for its TLAS asks for the next frame, also where frames are drawn only on changes (the
	// editor, low-processor mode).
	if (view->status == PATH_TRACER_STATUS_NO_TLAS) {
		RenderingServerDefault::redraw_request();
	}
}

PathTracerDeviceSupport PathTracer::get_device_support() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr) {
		return PATH_TRACER_DEVICE_NO_RAY_QUERY;
	}
	const RDD::Capabilities &capabilities = rd->get_device_capabilities();
	return path_tracer_device_support(capabilities.device_family == RDD::DEVICE_METAL, int(capabilities.apple_gpu_family), rd->has_feature(RD::SUPPORTS_RAY_QUERY));
}

// The reset keys of the traced surfaces (cheap: no table): which material each surface record draws and its
// change serial, plus the global shader parameters for materials whose hit groups read them; and the in-place
// content of each traced mesh (region updates) and posed slot (skinning).
static void _path_tracer_surface_keys(const NextGIRTScene *p_rt_scene, bool p_hit_groups, uint64_t &r_materials, uint64_t &r_geometry) {
	r_materials = PATH_TRACER_HASH_BASIS;
	r_geometry = PATH_TRACER_HASH_BASIS;
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (p_rt_scene == nullptr || material_storage == nullptr || mesh_storage == nullptr) {
		return;
	}
	const uint64_t global_serial = material_storage->get_global_shader_uniform_serial();
	const LocalVector<NextGIRTScene::SurfaceSource> &sources = p_rt_scene->get_surface_sources();
	for (const NextGIRTScene::SurfaceSource &source : sources) {
		RenderGeometryInstanceBase *geometry = static_cast<RenderGeometryInstanceBase *>(source.instance);
		if (geometry == nullptr || geometry->data == nullptr || source.surface == UINT32_MAX) {
			continue;
		}
		const RID material = _path_tracer_surface_material(geometry, source.mesh, source.surface);
		uint64_t material_key[3] = { material.get_id(), material_storage->material_get_change_serial(material), 0 };
		if (p_hit_groups && material_storage->material_uses_global_shader_uniforms(material)) {
			material_key[2] = global_serial;
		}
		r_materials = path_tracer_hash_bytes(r_materials, material_key, sizeof(material_key));
		const uint64_t geometry_key[2] = { mesh_storage->mesh_get_content_serial(source.mesh), geometry->mesh_instance.is_valid() ? mesh_storage->mesh_instance_get_rt_pose_serial(geometry->mesh_instance) : 0 };
		r_geometry = path_tracer_hash_bytes(r_geometry, geometry_key, sizeof(geometry_key));
	}
}

void PathTracer::_upload_tables(const PathTracerTables &p_tables) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	const uint32_t material_bytes = p_tables.materials.size() * sizeof(PathTracerGPUMaterial);
	if (!material_buffer.is_valid() || material_capacity < material_bytes) {
		if (material_buffer.is_valid()) {
			rd->free_rid(material_buffer);
		}
		material_capacity = MAX(material_bytes, uint32_t(64 * sizeof(PathTracerGPUMaterial)));
		material_capacity = Math::next_power_of_2(material_capacity);
		material_buffer = rd->storage_buffer_create(material_capacity);
	}
	rd->buffer_update(material_buffer, 0, material_bytes, p_tables.materials.ptr());
	const uint32_t light_bytes = p_tables.lights.size() * sizeof(float);
	if (!light_buffer.is_valid() || light_capacity < light_bytes) {
		if (light_buffer.is_valid()) {
			rd->free_rid(light_buffer);
		}
		light_capacity = light_bytes;
		light_buffer = rd->storage_buffer_create(light_capacity);
	}
	rd->buffer_update(light_buffer, 0, light_bytes, p_tables.lights.ptr());
}

void PathTracer::render(const RenderDataRD *p_render_data, NextGIRTScene *p_rt_scene, NextGIHitMaterials *p_hit_materials) {
	if (p_render_data == nullptr || p_render_data->render_buffers.is_null() || p_render_data->scene_data == nullptr) {
		return;
	}
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	PathTracerSettings settings = get_settings(rb.ptr());
	settings.reference_match = bool(GLOBAL_GET_CACHED(bool, "rendering/global_illumination/nextgi/debug/path_tracer_reference_match"));
	if (!settings.enabled) {
		PathTracerView *idle = get_view(rb.ptr(), false);
		if (idle != nullptr && idle->status != PATH_TRACER_STATUS_OFF) {
			set_status(rb.ptr(), PATH_TRACER_STATUS_OFF);
		}
		return;
	}
	if (int(GLOBAL_GET_CACHED(int, "rendering/global_illumination/nextgi/debug/reference_mode")) != 0) {
		set_status(rb.ptr(), PATH_TRACER_STATUS_REFERENCE_MODE);
		return;
	}
	if (rb->get_view_count() > 1) {
		set_status(rb.ptr(), PATH_TRACER_STATUS_MULTIVIEW);
		return;
	}
	if (p_rt_scene == nullptr) {
		set_status(rb.ptr(), PATH_TRACER_STATUS_NOT_HWRT);
		return;
	}
	const PathTracerDeviceSupport device_support = get_device_support();
	if (!path_tracer_device_runs(device_support)) {
		set_status(rb.ptr(), PATH_TRACER_STATUS_UNSUPPORTED_DEVICE);
		return;
	}
	if (!p_rt_scene->get_tlas().is_valid()) {
		set_status(rb.ptr(), PATH_TRACER_STATUS_NO_TLAS);
		return;
	}
	if (!_ensure_shader()) {
		set_status(rb.ptr(), PATH_TRACER_STATUS_NOT_HWRT);
		return;
	}
	if (!p_rt_scene->has_surface_sources()) {
		// This frame's TLAS phase ran before any view asked for path tracing (another view rendered first), so it
		// did not list what the records draw. The next frame's phase does.
		set_status(rb.ptr(), PATH_TRACER_STATUS_NO_TLAS);
		return;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	PathTracerView *view = get_view(rb.ptr(), true);
	const Size2i size = rb->get_internal_size();
	if (size.x <= 0 || size.y <= 0) {
		return;
	}
	view->settings = settings;
	view->target = uint32_t(settings.samples);
	view->device_support = device_support;
	view->exposure = 1.0f;
	if (p_render_data->camera_attributes.is_valid() && RSG::camera_attributes != nullptr) {
		view->exposure = RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
	}

	const bool hit_groups = !settings.reference_match && p_hit_materials != nullptr && p_hit_materials->can_trace_path_tracer();
	const PathTracerBackend backend = path_tracer_backend(hit_groups, p_hit_materials != nullptr && p_hit_materials->has_path_tracer_raygen());

	// The reset keys are cheap; the tables are built only on frames that trace.
	PathTracerFrameState state;
	state.size = size;
	state.settings = path_tracer_hash_settings(settings, backend, view->exposure);
	state.camera = path_tracer_hash_camera(p_render_data->scene_data->cam_transform, p_render_data->scene_data->cam_projection);
	Color sky;
	RendererSceneRenderRD *scene = RendererSceneRenderRD::get_singleton();
	if (scene != nullptr && p_render_data->environment.is_valid()) {
		sky = scene->environment_get_bg_color(p_render_data->environment) * scene->environment_get_bg_energy_multiplier(p_render_data->environment);
	}
	uint64_t materials_key = 0;
	uint64_t geometry_key = 0;
	_path_tracer_surface_keys(p_rt_scene, backend == PATH_TRACER_BACKEND_HIT_GROUPS, materials_key, geometry_key);
	const uint64_t records_hash = p_rt_scene->get_records_hash();
	const uint32_t instance_count = p_rt_scene->get_stats().instance_count;
	uint64_t scene_hash = path_tracer_hash_bytes(PATH_TRACER_HASH_BASIS, &records_hash, sizeof(records_hash));
	scene_hash = path_tracer_hash_bytes(scene_hash, &geometry_key, sizeof(geometry_key));
	scene_hash = path_tracer_hash_bytes(scene_hash, &sky, sizeof(sky));
	state.scene = path_tracer_hash_bytes(scene_hash, &instance_count, sizeof(instance_count));
	state.lights = _path_tracer_hash_lights(p_render_data);
	state.material_serial = materials_key;
	state.hit_groups_serial = backend == PATH_TRACER_BACKEND_HIT_GROUPS ? p_hit_materials->get_pipeline_serial() : 0;
	const RendererSceneRender::RenderSDFGIUpdateData *update = p_render_data->sdfgi_update_data;
	if (update != nullptr) {
		state.instance_events = update->nextgi_dirty_volume_count;
		state.light_events = update->nextgi_light_volume_count;
		state.directional_light_event = update->nextgi_directional_light_dirty;
	}

	const uint64_t frame = RSG::rasterizer != nullptr ? RSG::rasterizer->get_frame_number() : 0;
	const uint32_t reasons = path_tracer_reset_reasons(view->has_state && view->accumulation.is_valid(), view->state, state);
	if (reasons != 0) {
		if (!view->accumulation.is_valid() || view->size != size) {
			if (view->accumulation.is_valid()) {
				rd->free_rid(view->accumulation);
			}
			RD::TextureFormat format;
			format.width = size.x;
			format.height = size.y;
			format.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
			format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
			view->accumulation = rd->texture_create(format, RD::TextureView());
			view->size = size;
		}
		rd->texture_clear(view->accumulation, Color(0, 0, 0, 0), 0, 1, 0, 1);
		view->samples = 0;
		view->resets++;
		view->last_reset_reasons = reasons;
		view->last_reset_frame = frame;
		for (uint32_t i = 0; i < PATH_TRACER_RESET_REASON_COUNT; i++) {
			if (reasons & (1u << i)) {
				view->reset_counts[i]++;
			}
		}
	}
	view->state = state;
	view->has_state = true;
	view->backend = backend;
	if (!view->accumulation.is_valid()) {
		return;
	}
	if (!p_rt_scene->is_tlas_traceable()) {
		// A BLAS the TLAS references was rebuilt after it this frame (a new or changed mesh). The history restarted
		// above when the frame brought a change; its next sample comes on the next frame.
		view->status = PATH_TRACER_STATUS_NO_TLAS;
		RenderingServerDefault::redraw_request();
		return;
	}
	if (path_tracer_samples_this_frame(view->samples, view->target) == 0) {
		// Converged: no more frames are asked for.
		view->status = PATH_TRACER_STATUS_CONVERGED;
		return;
	}

	PathTracerTables tables;
	build_tables(p_render_data, p_rt_scene, tables, !settings.reference_match);
	view->textured_materials = tables.textured_materials;
	_upload_tables(tables);
	PathTracerGPUParams params;
	params.camera = path_tracer_camera(p_render_data->scene_data->cam_transform, p_render_data->scene_data->cam_projection, size);
	params.rays = path_tracer_rays(p_render_data->scene_data->cam_transform, p_render_data->scene_data->cam_projection, p_render_data->scene_data->cam_orthogonal, size);
	params.width = size.x;
	params.height = size.y;
	params.sample_index = view->samples;
	params.seed = 1;
	params.light_count = tables.light_count;
	params.surface_count = tables.surface_count;
	params.record_count = p_rt_scene->get_stats().instance_count;
	params.sky_color[0] = sky.r;
	params.sky_color[1] = sky.g;
	params.sky_color[2] = sky.b;
	params.exposure = view->exposure;
	params.clear = view->samples == 0 ? 1.0f : 0.0f;
	params.max_bounces = uint32_t(settings.max_bounces);
	params.flags = settings.reference_match ? 0u : (PATH_TRACER_FLAG_CULL | PATH_TRACER_FLAG_JITTER | PATH_TRACER_FLAG_PRODUCT);
	memcpy(&params.firefly_clamp_bits, &settings.firefly_clamp, sizeof(uint32_t));
	rd->buffer_update(shader->params_buffer, 0, sizeof(params), &params);

	if (!dummy_buffer.is_valid()) {
		dummy_buffer = rd->storage_buffer_create(256);
	}
	bool traced = false;
	if (backend == PATH_TRACER_BACKEND_HIT_GROUPS) {
		NextGIHitPathTrace trace;
		trace.accumulation = view->accumulation;
		trace.params = shader->params_buffer;
		trace.lights = light_buffer;
		trace.materials = material_buffer;
		trace.width = size.x;
		trace.height = size.y;
		trace.time = float(p_render_data->scene_data->time);
		traced = p_hit_materials->trace_path_tracer(p_rt_scene, trace);
	}
	if (!traced) {
		_trace_inline(view, p_rt_scene);
		if (backend == PATH_TRACER_BACKEND_HIT_GROUPS) {
			view->backend = PATH_TRACER_BACKEND_INLINE;
		}
	}
	view->samples++;
	view->frames_traced++;
	view->last_traced_frame = frame;
	view->status = view->samples >= view->target ? PATH_TRACER_STATUS_CONVERGED : PATH_TRACER_STATUS_ACTIVE;
	// While it accumulates, the view asks for the next frame, also where frames are drawn only on changes (the
	// editor's Path Tracer display mode, low-processor mode).
	if (view->status == PATH_TRACER_STATUS_ACTIVE) {
		RenderingServerDefault::redraw_request();
	}
	_readback(view);
}

void PathTracer::_readback(PathTracerView *p_view) {
	if (!bool(GLOBAL_GET_CACHED(bool, "rendering/global_illumination/nextgi/debug/path_tracer_readback"))) {
		p_view->read_accumulation.clear();
		return;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	const Vector<uint8_t> data = rd->texture_get_data(p_view->accumulation, 0);
	const int64_t pixels = int64_t(p_view->size.x) * int64_t(p_view->size.y);
	if (data.size() < pixels * 16) {
		p_view->read_accumulation.clear();
		return;
	}
	p_view->read_accumulation = data;
	const float *texels = reinterpret_cast<const float *>(data.ptr());
	double sum = 0.0;
	int64_t counted = 0;
	uint32_t nonfinite = 0;
	for (int64_t i = 0; i < pixels * 4; i++) {
		if (!Math::is_finite(texels[i])) {
			nonfinite++;
		}
	}
	for (int64_t i = 0; i < pixels; i++) {
		const float weight = texels[i * 4 + 3];
		if (weight > 0.0f) {
			sum += double(texels[i * 4] + texels[i * 4 + 1] + texels[i * 4 + 2]) / (3.0 * double(weight));
			counted++;
		}
	}
	p_view->read_mean = counted > 0 ? float(sum / double(counted)) : 0.0f;
	p_view->read_nonfinite = nonfinite;
	p_view->read_hash = path_tracer_hash_bytes(PATH_TRACER_HASH_BASIS, data.ptr(), data.size());
}

void PathTracer::_trace_inline(PathTracerView *p_view, NextGIRTScene *p_rt_scene) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
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
	RID records = p_rt_scene->get_instance_record_buffer();
	RID surfaces = p_rt_scene->get_surface_record_buffer();
	RD::Uniform uniforms[11] = {
		RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, p_rt_scene->get_tlas()),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, p_view->accumulation),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, material_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, light_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, records.is_valid() ? records : dummy_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, surfaces.is_valid() ? surfaces : dummy_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, chunks[0]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, chunks[1]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, chunks[2]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, chunks[3]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, shader->params_buffer),
	};
	const RID uniform_set = rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 11), shader->shader.version_get_shader(shader->version, 0), 0);
	ERR_FAIL_COND(!uniform_set.is_valid());
	rd->draw_command_begin_label("Path Tracer");
	const RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, shader->accumulate);
	rd->compute_list_bind_uniform_set(list, uniform_set, 0);
	rd->compute_list_dispatch(list, (p_view->size.x + 7) / 8, (p_view->size.y + 7) / 8, 1);
	rd->compute_list_end();
	rd->draw_command_end_label();
	rd->free_rid(uniform_set);
}

void PathTracer::composite(RenderSceneBuffersRD *p_render_buffers, RID p_color) {
	PathTracerView *view = get_view(p_render_buffers, false);
	// A frame without a traceable TLAS shows the history it kept (none after a restart: the normal image).
	if (view == nullptr || (view->status != PATH_TRACER_STATUS_ACTIVE && view->status != PATH_TRACER_STATUS_CONVERGED && view->status != PATH_TRACER_STATUS_NO_TLAS)) {
		return;
	}
	if (view->samples == 0 || !view->accumulation.is_valid() || !p_color.is_valid() || shader == nullptr) {
		return;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	const RD::TextureFormat format = rd->texture_get_format(p_color);
	if (format.format != RD::DATA_FORMAT_R16G16B16A16_SFLOAT || int(format.width) != view->size.x || int(format.height) != view->size.y) {
		return;
	}
	RD::Uniform uniforms[2] = {
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, view->accumulation),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, p_color),
	};
	const RID uniform_set = rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 2), shader->shader.version_get_shader(shader->version, 1), 0);
	ERR_FAIL_COND(!uniform_set.is_valid());
	PathTracerCompositePush push;
	push.width = view->size.x;
	push.height = view->size.y;
	push.exposure = view->exposure;
	rd->draw_command_begin_label("Path Tracer Composite");
	const RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, shader->composite);
	rd->compute_list_bind_uniform_set(list, uniform_set, 0);
	rd->compute_list_set_push_constant(list, &push, sizeof(push));
	rd->compute_list_dispatch(list, (view->size.x + 7) / 8, (view->size.y + 7) / 8, 1);
	rd->compute_list_end();
	rd->draw_command_end_label();
	rd->free_rid(uniform_set);
}

void PathTracer::free() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (shader != nullptr) {
		if (rd != nullptr && shader->params_buffer.is_valid()) {
			rd->free_rid(shader->params_buffer);
		}
		if (shader->version.is_valid()) {
			shader->shader.version_free(shader->version);
		}
		memdelete(shader);
		shader = nullptr;
	}
	if (rd != nullptr) {
		const RID buffers[3] = { dummy_buffer, material_buffer, light_buffer };
		for (const RID &buffer : buffers) {
			if (buffer.is_valid()) {
				rd->free_rid(buffer);
			}
		}
	}
	dummy_buffer = RID();
	material_buffer = RID();
	light_buffer = RID();
	material_capacity = 0;
	light_capacity = 0;
}

PathTracer::~PathTracer() {
	free();
}
