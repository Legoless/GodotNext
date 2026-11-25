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
#include "core/math/quaternion.h"
#include "core/os/mutex.h"
#include "core/templates/sort_array.h"
#include "servers/rendering/renderer_geometry_instance.h"
#include "servers/rendering/renderer_rd/environment/fog.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_hit_materials.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_scene.h"
#include "servers/rendering/renderer_rd/environment/sky.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/shaders/effects/path_tracer_denoise.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/path_tracer_sky_cdf.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/path_tracer.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
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
	settings.denoiser = RenderingServerTypes::path_tracing_validate_denoiser(int(p_settings.denoiser));
	settings.denoise_requested = p_settings.denoise_requested;
	settings.reference_match = p_settings.reference_match;
	return settings;
}

const char *path_tracer_denoise_status_name(PathTracerDenoiseStatus p_status) {
	switch (p_status) {
		case PATH_TRACER_DENOISE_STATUS_OFF:
			return "OFF";
		case PATH_TRACER_DENOISE_STATUS_PENDING:
			return "PENDING";
		case PATH_TRACER_DENOISE_STATUS_DENOISED:
			return "DENOISED";
		case PATH_TRACER_DENOISE_STATUS_FALLBACK_BUILTIN:
			return "FALLBACK_BUILTIN";
		case PATH_TRACER_DENOISE_STATUS_MAX:
			break;
	}
	return "UNKNOWN";
}

const char *path_tracer_denoiser_name(RSE::ViewportPathTracingDenoiser p_denoiser) {
	switch (p_denoiser) {
		case RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF:
			return "OFF";
		case RSE::VIEWPORT_PATH_TRACING_DENOISER_BUILTIN:
			return "BUILTIN";
		case RSE::VIEWPORT_PATH_TRACING_DENOISER_OIDN:
			return "OIDN";
		case RSE::VIEWPORT_PATH_TRACING_DENOISER_MAX:
			break;
	}
	return "UNKNOWN";
}

static float _path_tracer_positive_finite(float p_value, float p_default, float p_min, float p_max) {
	if (!Math::is_finite(p_value) || p_value <= 0.0f) {
		return p_default;
	}
	return CLAMP(p_value, p_min, p_max);
}

PathTracerDenoiseParams path_tracer_validate_denoise_params(const PathTracerDenoiseParams &p_params) {
	PathTracerDenoiseParams valid;
	valid.spatial_bandwidth = _path_tracer_positive_finite(p_params.spatial_bandwidth, 5.0f, 0.25f, 32.0f);
	valid.light_bandwidth = _path_tracer_positive_finite(p_params.light_bandwidth, 0.1f, 0.001f, 4.0f);
	valid.albedo_bandwidth = _path_tracer_positive_finite(p_params.albedo_bandwidth, 0.2f, 0.01f, 4.0f);
	valid.normal_bandwidth = _path_tracer_positive_finite(p_params.normal_bandwidth, 0.1f, 0.01f, 4.0f);
	valid.filter_strength = _path_tracer_positive_finite(p_params.filter_strength, 10.0f, 0.1f, 64.0f);
	valid.half_search_window = CLAMP(p_params.half_search_window, 1, PATH_TRACER_DENOISE_MAX_HALF_SEARCH);
	return valid;
}

bool path_tracer_should_denoise(RSE::ViewportPathTracingDenoiser p_denoiser, uint32_t p_samples, uint32_t p_target, bool p_on_demand, bool p_denoised_valid, uint32_t p_denoised_samples, RSE::ViewportPathTracingDenoiser p_denoised_requested) {
	if (p_denoiser == RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF || p_samples == 0) {
		return false;
	}
	if (!p_on_demand && p_samples < p_target) {
		return false;
	}
	if (p_denoised_valid && p_denoised_samples == p_samples && p_denoised_requested == p_denoiser) {
		return false;
	}
	return true;
}

static float _path_tracer_sanitize_component(float p_value) {
	if (!Math::is_finite(p_value) || p_value < 0.0f) {
		return 0.0f;
	}
	return p_value;
}

bool path_tracer_prepare_oidn_buffers(uint32_t p_width, uint32_t p_height, const float *p_accum_rgba, const float *p_albedo_rgba, const float *p_normal_rgba, LocalVector<float> &r_color_rgb, LocalVector<float> &r_albedo_rgb, LocalVector<float> &r_normal_rgb) {
	if (p_width == 0 || p_height == 0 || p_accum_rgba == nullptr) {
		return false;
	}
	const uint64_t pixel_count = uint64_t(p_width) * uint64_t(p_height);
	if (pixel_count > (UINT32_MAX / 3u)) {
		return false;
	}
	const uint32_t rgb_floats = uint32_t(pixel_count * 3u);
	r_color_rgb.resize(rgb_floats);
	const bool has_albedo = p_albedo_rgba != nullptr;
	const bool has_normal = p_normal_rgba != nullptr;
	if (has_albedo) {
		r_albedo_rgb.resize(rgb_floats);
	} else {
		r_albedo_rgb.clear();
	}
	if (has_normal) {
		r_normal_rgb.resize(rgb_floats);
	} else {
		r_normal_rgb.clear();
	}
	for (uint64_t i = 0; i < pixel_count; i++) {
		const uint64_t s = i * 4u;
		const uint64_t d = i * 3u;
		const float weight = p_accum_rgba[s + 3u];
		if (Math::is_finite(weight) && weight > 0.0f) {
			const float inv_w = 1.0f / weight;
			r_color_rgb[d + 0u] = _path_tracer_sanitize_component(p_accum_rgba[s + 0u] * inv_w);
			r_color_rgb[d + 1u] = _path_tracer_sanitize_component(p_accum_rgba[s + 1u] * inv_w);
			r_color_rgb[d + 2u] = _path_tracer_sanitize_component(p_accum_rgba[s + 2u] * inv_w);
		} else {
			r_color_rgb[d + 0u] = 0.0f;
			r_color_rgb[d + 1u] = 0.0f;
			r_color_rgb[d + 2u] = 0.0f;
		}
		if (has_albedo) {
			const float aw = p_albedo_rgba[s + 3u];
			if (Math::is_finite(aw) && aw > 1e-6f) {
				const float inv_a = 1.0f / aw;
				r_albedo_rgb[d + 0u] = CLAMP(_path_tracer_sanitize_component(p_albedo_rgba[s + 0u] * inv_a), 0.0f, 1.0f);
				r_albedo_rgb[d + 1u] = CLAMP(_path_tracer_sanitize_component(p_albedo_rgba[s + 1u] * inv_a), 0.0f, 1.0f);
				r_albedo_rgb[d + 2u] = CLAMP(_path_tracer_sanitize_component(p_albedo_rgba[s + 2u] * inv_a), 0.0f, 1.0f);
			} else {
				r_albedo_rgb[d + 0u] = 0.0f;
				r_albedo_rgb[d + 1u] = 0.0f;
				r_albedo_rgb[d + 2u] = 0.0f;
			}
		}
		if (has_normal) {
			const float nw = p_normal_rgba[s + 3u];
			const float nx = p_normal_rgba[s + 0u];
			const float ny = p_normal_rgba[s + 1u];
			const float nz = p_normal_rgba[s + 2u];
			if (Math::is_finite(nw) && nw > 1e-6f && Math::is_finite(nx) && Math::is_finite(ny) && Math::is_finite(nz)) {
				const float len_sq = nx * nx + ny * ny + nz * nz;
				if (len_sq > 1e-12f && Math::is_finite(len_sq)) {
					const float inv_len = 1.0f / Math::sqrt(len_sq);
					r_normal_rgb[d + 0u] = nx * inv_len;
					r_normal_rgb[d + 1u] = ny * inv_len;
					r_normal_rgb[d + 2u] = nz * inv_len;
				} else {
					r_normal_rgb[d + 0u] = 0.0f;
					r_normal_rgb[d + 1u] = 0.0f;
					r_normal_rgb[d + 2u] = 0.0f;
				}
			} else {
				r_normal_rgb[d + 0u] = 0.0f;
				r_normal_rgb[d + 1u] = 0.0f;
				r_normal_rgb[d + 2u] = 0.0f;
			}
		}
	}
	return true;
}

void path_tracer_pack_denoised_rgba(uint32_t p_width, uint32_t p_height, const float *p_rgb, Vector<uint8_t> &r_rgba_bytes) {
	const uint64_t pixel_count = uint64_t(p_width) * uint64_t(p_height);
	if (pixel_count == 0 || p_rgb == nullptr) {
		r_rgba_bytes.clear();
		return;
	}
	r_rgba_bytes.resize(int64_t(pixel_count * 16u));
	float *dst = reinterpret_cast<float *>(r_rgba_bytes.ptrw());
	for (uint64_t i = 0; i < pixel_count; i++) {
		const uint64_t s = i * 3u;
		const uint64_t d = i * 4u;
		dst[d + 0u] = _path_tracer_sanitize_component(p_rgb[s + 0u]);
		dst[d + 1u] = _path_tracer_sanitize_component(p_rgb[s + 1u]);
		dst[d + 2u] = _path_tracer_sanitize_component(p_rgb[s + 2u]);
		dst[d + 3u] = 1.0f;
	}
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

// === PURE #169 HELPERS (LIGHTS, MIS, SKY CDF, VOLUMETRIC FOG, DOF, MOTION BLUR) ===

struct PathTracerLightEntryLess {
	_FORCE_INLINE_ bool operator()(const PathTracerLightEntry &p_a, const PathTracerLightEntry &p_b) const {
		if (p_a.type != p_b.type) {
			return p_a.type < p_b.type;
		}
		return p_a.rid_id < p_b.rid_id;
	}
};

void path_tracer_collect_and_sort_lights(const PathTracerLightEntry *p_entries, uint32_t p_count, LocalVector<PathTracerLightEntry> &r_sorted) {
	r_sorted.clear();
	if (p_entries == nullptr || p_count == 0) {
		return;
	}
	r_sorted.reserve(p_count);
	for (uint32_t i = 0; i < p_count; i++) {
		if (p_entries[i].rid_id == 0) {
			continue;
		}
		r_sorted.push_back(p_entries[i]);
	}
	if (r_sorted.is_empty()) {
		return;
	}
	SortArray<PathTracerLightEntry, PathTracerLightEntryLess> sorter;
	sorter.sort(r_sorted.ptr(), r_sorted.size());
	// Deduplicate by rid_id (same light may appear in multiple scenario/view feeds).
	uint32_t write = 0;
	for (uint32_t i = 0; i < r_sorted.size(); i++) {
		bool duplicate = false;
		for (uint32_t j = 0; j < write; j++) {
			if (r_sorted[j].rid_id == r_sorted[i].rid_id) {
				duplicate = true;
				break;
			}
		}
		if (!duplicate) {
			r_sorted[write++] = r_sorted[i];
		}
	}
	r_sorted.resize(write);
}

float path_tracer_mis_balance_weight(float p_pdf_a, float p_pdf_b) {
	if (!Math::is_finite(p_pdf_a) || p_pdf_a <= 0.0f) {
		return 0.0f;
	}
	if (!Math::is_finite(p_pdf_b) || p_pdf_b <= 0.0f) {
		return 1.0f;
	}
	return p_pdf_a / (p_pdf_a + p_pdf_b);
}

float path_tracer_cone_solid_angle(float p_cos_max) {
	return MAX(float(Math::TAU) * (1.0f - CLAMP(p_cos_max, -1.0f, 1.0f)), 0.0f);
}

float path_tracer_area_light_pdf_omega(float p_dist_sq, float p_clipped_area, float p_cos_light) {
	if (p_dist_sq <= 0.0f || p_clipped_area <= 0.0f || p_cos_light <= 0.0f) {
		return 0.0f;
	}
	return p_dist_sq / (p_clipped_area * p_cos_light);
}

Vector3 path_tracer_sky_oct_to_dir(const Vector2 &p_uv) {
	const Vector2 e = p_uv * 2.0f - Vector2(1.0f, 1.0f);
	Vector3 v(e.x, e.y, 1.0f - Math::abs(e.x) - Math::abs(e.y));
	const float t = MAX(-v.z, 0.0f);
	v.x += t * (v.x >= 0.0f ? -1.0f : 1.0f);
	v.y += t * (v.y >= 0.0f ? -1.0f : 1.0f);
	return v.normalized();
}

Vector2 path_tracer_sky_dir_to_oct(const Vector3 &p_dir) {
	Vector3 n = p_dir.normalized();
	const float l1 = Math::abs(n.x) + Math::abs(n.y) + Math::abs(n.z);
	if (l1 <= 1e-8f) {
		return Vector2(0.5f, 0.5f);
	}
	n /= l1;
	Vector2 e(n.x, n.y);
	if (n.z < 0.0f) {
		e = Vector2(
				(1.0f - Math::abs(n.y)) * (n.x >= 0.0f ? 1.0f : -1.0f),
				(1.0f - Math::abs(n.x)) * (n.y >= 0.0f ? 1.0f : -1.0f));
	}
	return e * 0.5f + Vector2(0.5f, 0.5f);
}

float path_tracer_sky_oct_jacobian(const Vector3 &p_dir) {
	const Vector3 d = p_dir.normalized();
	const float l1 = Math::abs(d.x) + Math::abs(d.y) + Math::abs(d.z);
	return 4.0f * l1 * l1 * l1;
}

void path_tracer_build_sky_cdf_cpu(const float *p_luminance, uint32_t p_dim, PathTracerSkyCdfCPU &r_cdf) {
	r_cdf.dim = p_dim;
	r_cdf.marginal.clear();
	r_cdf.conditional.clear();
	r_cdf.cell_pdf.clear();
	r_cdf.total_weight = 0.0f;
	if (p_luminance == nullptr || p_dim == 0) {
		return;
	}
	r_cdf.marginal.resize(p_dim);
	r_cdf.conditional.resize(p_dim * p_dim);
	r_cdf.cell_pdf.resize(p_dim * p_dim);

	double total = 0.0;
	for (uint32_t y = 0; y < p_dim; y++) {
		double row_sum = 0.0;
		const uint32_t row_base = y * p_dim;
		for (uint32_t x = 0; x < p_dim; x++) {
			const Vector2 uv((float(x) + 0.5f) / float(p_dim), (float(y) + 0.5f) / float(p_dim));
			const Vector3 dir = path_tracer_sky_oct_to_dir(uv);
			const float lum = (Math::is_finite(p_luminance[row_base + x]) && p_luminance[row_base + x] > 0.0f) ? p_luminance[row_base + x] : 0.0f;
			const float w = MAX(lum, 1e-4f) * path_tracer_sky_oct_jacobian(dir);
			r_cdf.cell_pdf[row_base + x] = w;
			row_sum += double(w);
			r_cdf.conditional[row_base + x] = float(row_sum);
		}
		const double safe_row = row_sum > 1e-12 ? row_sum : 1.0;
		for (uint32_t x = 0; x < p_dim; x++) {
			r_cdf.conditional[row_base + x] = (x + 1 == p_dim) ? 1.0f : float(double(r_cdf.conditional[row_base + x]) / safe_row);
		}
		total += row_sum;
		r_cdf.marginal[y] = float(total);
	}
	r_cdf.total_weight = float(total);
	const double safe_total = total > 1e-12 ? total : 1.0;
	for (uint32_t y = 0; y < p_dim; y++) {
		r_cdf.marginal[y] = (y + 1 == p_dim) ? 1.0f : float(double(r_cdf.marginal[y]) / safe_total);
	}
	const double inv_total_uv = total > 1e-12 ? (double(p_dim) * double(p_dim) / total) : 1.0;
	for (uint32_t i = 0; i < p_dim * p_dim; i++) {
		r_cdf.cell_pdf[i] = total > 1e-12 ? float(double(r_cdf.cell_pdf[i]) * inv_total_uv) : 1.0f;
	}
}

void path_tracer_sample_sky_cdf_cpu(const PathTracerSkyCdfCPU &p_cdf, const Vector2 &p_u, Vector3 &r_dir, float &r_pdf_omega) {
	if (p_cdf.dim == 0 || p_cdf.marginal.size() < p_cdf.dim || p_cdf.conditional.size() < p_cdf.dim * p_cdf.dim) {
		r_dir = Vector3(0.0f, 1.0f, 0.0f);
		r_pdf_omega = 0.0f;
		return;
	}
	const float uy = CLAMP(p_u.y, 0.0f, 0.9999999f);
	const float ux = CLAMP(p_u.x, 0.0f, 0.9999999f);
	uint32_t lo = 0;
	uint32_t hi = p_cdf.dim - 1;
	while (lo < hi) {
		const uint32_t mid = (lo + hi) >> 1u;
		if (uy > p_cdf.marginal[mid]) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	const uint32_t iy = lo;
	const float prev_y = iy > 0 ? p_cdf.marginal[iy - 1] : 0.0f;
	const float curr_y = p_cdf.marginal[iy];
	const float dy = CLAMP((uy - prev_y) / MAX(curr_y - prev_y, 1e-8f), 0.0f, 1.0f);

	const uint32_t row_base = iy * p_cdf.dim;
	lo = 0;
	hi = p_cdf.dim - 1;
	while (lo < hi) {
		const uint32_t mid = (lo + hi) >> 1u;
		if (ux > p_cdf.conditional[row_base + mid]) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	const uint32_t ix = lo;
	const float prev_x = ix > 0 ? p_cdf.conditional[row_base + ix - 1] : 0.0f;
	const float curr_x = p_cdf.conditional[row_base + ix];
	const float dx = CLAMP((ux - prev_x) / MAX(curr_x - prev_x, 1e-8f), 0.0f, 1.0f);

	const Vector2 uv((float(ix) + dx) / float(p_cdf.dim), (float(iy) + dy) / float(p_cdf.dim));
	r_dir = path_tracer_sky_oct_to_dir(uv);
	const float p_uv = p_cdf.cell_pdf[row_base + ix];
	r_pdf_omega = p_uv / MAX(path_tracer_sky_oct_jacobian(r_dir), 1e-6f);
}

float path_tracer_sky_cdf_pdf_cpu(const PathTracerSkyCdfCPU &p_cdf, const Vector3 &p_dir) {
	if (p_cdf.dim == 0 || p_cdf.cell_pdf.size() < p_cdf.dim * p_cdf.dim) {
		return 0.0f;
	}
	const Vector3 d = p_dir.normalized();
	const Vector2 uv = path_tracer_sky_dir_to_oct(d);
	const uint32_t ix = MIN(uint32_t(CLAMP(uv.x, 0.0f, 0.999999f) * float(p_cdf.dim)), p_cdf.dim - 1);
	const uint32_t iy = MIN(uint32_t(CLAMP(uv.y, 0.0f, 0.999999f) * float(p_cdf.dim)), p_cdf.dim - 1);
	const float p_uv = p_cdf.cell_pdf[iy * p_cdf.dim + ix];
	return p_uv / MAX(path_tracer_sky_oct_jacobian(d), 1e-6f);
}

float path_tracer_hg_phase(float p_cos_theta, float p_g) {
	const float g = CLAMP(p_g, -0.95f, 0.95f);
	const float g2 = g * g;
	const float denom = MAX(1.0f + g2 - 2.0f * g * CLAMP(p_cos_theta, -1.0f, 1.0f), 1e-4f);
	return (1.0f - g2) / (4.0f * float(Math::PI) * denom * Math::sqrt(denom));
}

Vector3 path_tracer_sample_hg(const Vector3 &p_dir, float p_g, const Vector2 &p_u, float &r_pdf) {
	const float g = CLAMP(p_g, -0.95f, 0.95f);
	float cos_theta = 0.0f;
	if (Math::abs(g) < 1e-3f) {
		cos_theta = 1.0f - 2.0f * CLAMP(p_u.x, 0.0f, 1.0f);
	} else {
		const float sqr = (1.0f - g * g) / (1.0f - g + 2.0f * g * CLAMP(p_u.x, 0.0f, 1.0f));
		cos_theta = CLAMP((1.0f + g * g - sqr * sqr) / (2.0f * g), -1.0f, 1.0f);
	}
	const float sin_theta = Math::sqrt(MAX(0.0f, 1.0f - cos_theta * cos_theta));
	const float phi = float(Math::TAU) * p_u.y;
	const Vector3 axis = p_dir.normalized();
	const Vector3 helper = Math::abs(axis.z) < 0.999f ? Vector3(0.0f, 0.0f, 1.0f) : Vector3(1.0f, 0.0f, 0.0f);
	const Vector3 t = helper.cross(axis).normalized();
	const Vector3 b = axis.cross(t);
	r_pdf = path_tracer_hg_phase(cos_theta, g);
	return (t * (sin_theta * Math::cos(phi)) + b * (sin_theta * Math::sin(phi)) + axis * cos_theta).normalized();
}

float path_tracer_fog_volume_sdf(const Vector3 &p_local_pos, const Vector3 &p_half_size, uint32_t p_shape) {
	if (p_shape == uint32_t(RSE::FOG_VOLUME_SHAPE_ELLIPSOID)) {
		const Vector3 hs(MAX(p_half_size.x, 1e-4f), MAX(p_half_size.y, 1e-4f), MAX(p_half_size.z, 1e-4f));
		const float k0 = Vector3(p_local_pos.x / hs.x, p_local_pos.y / hs.y, p_local_pos.z / hs.z).length();
		const float k1 = Vector3(p_local_pos.x / (hs.x * hs.x), p_local_pos.y / (hs.y * hs.y), p_local_pos.z / (hs.z * hs.z)).length();
		return k1 > 1e-6f ? k0 * (k0 - 1.0f) / k1 : -MIN(hs.x, MIN(hs.y, hs.z));
	}
	if (p_shape == uint32_t(RSE::FOG_VOLUME_SHAPE_CONE)) {
		const float inv_h = 1.0f / MAX(0.001f, p_half_size.y);
		const float r = 1.0f / MAX(0.001f, MIN(p_half_size.x, p_half_size.z) * 0.5f);
		const float hyp = Math::sqrt(r * r + inv_h * inv_h);
		const Vector2 c(r / hyp, inv_h / hyp);
		const float q = Vector2(p_local_pos.x, p_local_pos.z).length();
		return MAX(c.dot(Vector2(q, p_local_pos.y - p_half_size.y)), -p_half_size.y - p_local_pos.y);
	}
	if (p_shape == uint32_t(RSE::FOG_VOLUME_SHAPE_CYLINDER)) {
		const Vector2 d(
				Math::abs(Vector2(p_local_pos.x, p_local_pos.z).length()) - MIN(p_half_size.x, p_half_size.z),
				Math::abs(p_local_pos.y) - p_half_size.y);
		return MIN(MAX(d.x, d.y), 0.0f) + Vector2(MAX(d.x, 0.0f), MAX(d.y, 0.0f)).length();
	}
	if (p_shape == uint32_t(RSE::FOG_VOLUME_SHAPE_BOX)) {
		const Vector3 q(
				Math::abs(p_local_pos.x) - p_half_size.x,
				Math::abs(p_local_pos.y) - p_half_size.y,
				Math::abs(p_local_pos.z) - p_half_size.z);
		return Vector3(MAX(q.x, 0.0f), MAX(q.y, 0.0f), MAX(q.z, 0.0f)).length() + MIN(MAX(q.x, MAX(q.y, q.z)), 0.0f);
	}
	return -1.0f;
}

static uint32_t _pt_pcg_hash(uint32_t p_v) {
	const uint32_t state = p_v * 747796405u + 2891336453u;
	const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

static float _pt_pcg_rnd(uint32_t &r_rng) {
	r_rng = _pt_pcg_hash(r_rng);
	return float(r_rng & 0x00FFFFFFu) * (1.0f / 16777216.0f);
}

float path_tracer_delta_tracking_transmittance_cpu(float p_segment_length, float p_majorant, float (*p_sigma_fn)(float p_t, void *p_ctx), void *p_ctx, uint32_t &r_rng) {
	if (p_segment_length <= 0.0f || p_majorant <= 1e-6f || p_sigma_fn == nullptr) {
		return 1.0f;
	}
	float t = 0.0f;
	for (uint32_t step = 0; step < 512; step++) {
		const float u = MAX(1.0f - _pt_pcg_rnd(r_rng), 1e-7f);
		t += -Math::log(u) / p_majorant;
		if (t >= p_segment_length) {
			return 1.0f;
		}
		const float sigma = CLAMP(p_sigma_fn(t, p_ctx), 0.0f, p_majorant);
		if (_pt_pcg_rnd(r_rng) * p_majorant < sigma) {
			return 0.0f;
		}
	}
	return 1.0f;
}

float path_tracer_ratio_tracking_transmittance_cpu(float p_segment_length, float p_majorant, float (*p_sigma_fn)(float p_t, void *p_ctx), void *p_ctx, uint32_t &r_rng) {
	if (p_segment_length <= 0.0f || p_majorant <= 1e-6f || p_sigma_fn == nullptr) {
		return 1.0f;
	}
	float t = 0.0f;
	float tr = 1.0f;
	for (uint32_t step = 0; step < 512; step++) {
		const float u = MAX(1.0f - _pt_pcg_rnd(r_rng), 1e-7f);
		t += -Math::log(u) / p_majorant;
		if (t >= p_segment_length) {
			break;
		}
		const float sigma = CLAMP(p_sigma_fn(t, p_ctx), 0.0f, p_majorant);
		tr *= MAX(0.0f, 1.0f - sigma / p_majorant);
		if (tr < 1e-6f) {
			return 0.0f;
		}
	}
	return tr;
}

PathTracerDOFParams path_tracer_compute_dof_params(bool p_far_enabled, float p_far_distance, float p_far_transition, bool p_near_enabled, float p_near_distance, float p_near_transition, float p_blur_amount, float p_fov_y_rad, int p_viewport_height, bool p_orthogonal) {
	PathTracerDOFParams out;
	if (p_orthogonal || (!p_far_enabled && !p_near_enabled) || p_blur_amount <= 1e-6f || p_viewport_height <= 0) {
		return out;
	}
	const float far_dist = MAX(p_far_distance, 0.05f);
	const float near_dist = MAX(p_near_distance, 0.05f);
	float focus = far_dist;
	float transition_scale = 1.0f;
	if (p_far_enabled && p_near_enabled) {
		focus = 0.5f * (far_dist + near_dist);
	} else if (p_far_enabled) {
		focus = far_dist;
		if (p_far_transition > 0.0f) {
			transition_scale = CLAMP((focus + p_far_transition) / MAX(p_far_transition, 0.25f * focus), 0.5f, 4.0f);
		}
	} else {
		focus = near_dist;
		if (p_near_transition > 0.0f) {
			transition_scale = CLAMP(focus / MAX(p_near_transition, 0.25f * focus), 0.5f, 4.0f);
		}
	}
	const float fov = CLAMP(p_fov_y_rad, Math::deg_to_rad(1.0f), Math::deg_to_rad(175.0f));
	const float max_coc_pixels = CLAMP(p_blur_amount, 0.0f, 1.0f) * 32.0f * transition_scale;
	const float rad_per_pixel = (2.0f * Math::tan(fov * 0.5f)) / float(p_viewport_height);
	out.focus_distance = focus;
	out.aperture_radius = CLAMP(focus * max_coc_pixels * rad_per_pixel, 0.0f, focus * 0.45f);
	out.enabled = out.aperture_radius > 1e-6f;
	return out;
}

Vector2 path_tracer_sample_concentric_disk(const Vector2 &p_u) {
	const Vector2 offset = p_u * 2.0f - Vector2(1.0f, 1.0f);
	if (offset.x == 0.0f && offset.y == 0.0f) {
		return Vector2();
	}
	float r = 0.0f;
	float theta = 0.0f;
	if (Math::abs(offset.x) > Math::abs(offset.y)) {
		r = offset.x;
		theta = float(Math::PI * 0.25) * (offset.y / offset.x);
	} else {
		r = offset.y;
		theta = float(Math::PI * 0.5) - float(Math::PI * 0.25) * (offset.x / offset.y);
	}
	return Vector2(r * Math::cos(theta), r * Math::sin(theta));
}

void path_tracer_apply_thin_lens(const Vector3 &p_origin, const Vector3 &p_dir, const Vector3 &p_cam_forward, const Vector3 &p_cam_right, const Vector3 &p_cam_up, const PathTracerDOFParams &p_dof, const Vector2 &p_lens_u, Vector3 &r_origin, Vector3 &r_dir) {
	if (!p_dof.enabled || p_dof.aperture_radius <= 1e-6f) {
		r_origin = p_origin;
		r_dir = p_dir.normalized();
		return;
	}
	const Vector3 dir_n = p_dir.normalized();
	const Vector3 fwd_n = p_cam_forward.normalized();
	const float cos_view = MAX(dir_n.dot(fwd_n), 1e-4f);
	const Vector3 focus_point = p_origin + dir_n * (p_dof.focus_distance / cos_view);
	const Vector2 disk = path_tracer_sample_concentric_disk(p_lens_u) * p_dof.aperture_radius;
	r_origin = p_origin + p_cam_right * disk.x + p_cam_up * disk.y;
	r_dir = (focus_point - r_origin).normalized();
}

Transform3D path_tracer_interpolate_transform(const Transform3D &p_prev, const Transform3D &p_curr, float p_t) {
	const float t = CLAMP(p_t, 0.0f, 1.0f);
	if (t <= 0.0f) {
		return p_prev;
	}
	if (t >= 1.0f) {
		return p_curr;
	}
	const Vector3 origin = p_prev.origin.lerp(p_curr.origin, t);
	const float det_prev = p_prev.basis.determinant();
	const float det_curr = p_curr.basis.determinant();
	if (det_prev * det_curr <= 0.0f || Math::abs(det_prev) < 1e-8f || Math::abs(det_curr) < 1e-8f) {
		// Reflection or degenerate basis: component-wise basis lerp fallback.
		Basis b;
		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) {
				b.rows[i][j] = Math::lerp(p_prev.basis.rows[i][j], p_curr.basis.rows[i][j], t);
			}
		}
		return Transform3D(b, origin);
	}
	const Vector3 scale_prev = p_prev.basis.get_scale();
	const Vector3 scale_curr = p_curr.basis.get_scale();
	const Vector3 scale = scale_prev.lerp(scale_curr, t);
	const Quaternion q_prev = p_prev.basis.orthonormalized().get_rotation_quaternion();
	const Quaternion q_curr = p_curr.basis.orthonormalized().get_rotation_quaternion();
	const Quaternion q = q_prev.slerp(q_curr, t);
	Basis b(q);
	b.scale(scale);
	return Transform3D(b, origin);
}

float path_tracer_shutter_time(uint32_t p_sample_index, uint32_t p_slice, uint32_t p_slice_count, float p_shutter) {
	const float shutter = CLAMP(p_shutter, 0.0f, 1.0f);
	if (p_slice_count == 0 || shutter <= 0.0f) {
		return 1.0f;
	}
	const float jitter = float(Math::fposmod((double(p_sample_index) + 0.5) * 0.6180339887498949, 1.0));
	const float slice_u = (float(p_slice % p_slice_count) + jitter) / float(p_slice_count);
	return CLAMP(1.0f - shutter * (1.0f - slice_u), 0.0f, 1.0f);
}

} // namespace RendererRD

/* VIEW */

void PathTracerView::clear_history() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr) {
		const RID textures[4] = { accumulation, guide_albedo, guide_normal, denoised };
		for (const RID &tex : textures) {
			if (tex.is_valid()) {
				rd->free_rid(tex);
			}
		}
	}
	accumulation = RID();
	guide_albedo = RID();
	guide_normal = RID();
	denoised = RID();
	size = Size2i();
	samples = 0;
	has_state = false;
	denoised_valid = false;
	pending_denoise_request = false;
	denoised_samples = 0;
	denoised_requested = RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF;
	denoise_backend = RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF;
	denoise_status = PATH_TRACER_DENOISE_STATUS_OFF;
	read_denoised.clear();
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
	// GodotNext: path_tracer_denoise (#168).
	const bool denoised_active = settings.denoiser != RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF && denoised_valid && denoised_samples == samples && samples > 0;
	r_info["denoiser"] = path_tracer_denoiser_name(settings.denoiser);
	r_info["denoise_status"] = path_tracer_denoise_status_name(denoise_status);
	r_info["denoise_backend"] = path_tracer_denoiser_name(denoise_backend);
	r_info["denoised"] = denoised_active;
	r_info["denoised_samples"] = int64_t(denoised_samples);
	r_info["denoise_runs"] = int64_t(denoise_runs);
	r_info["oidn_available"] = PathTracer::is_oidn_available();
	r_info["oidn_backend"] = String(PathTracer::get_oidn_backend_name());
	// GodotNext: path_tracer_lights_camera (#169).
	r_info["light_count"] = int64_t(light_count);
	r_info["fog_volume_count"] = int64_t(fog_volume_count);
	r_info["sky_importance_active"] = sky_importance_active;
	r_info["volumetric_fog_active"] = volumetric_fog_active;
	r_info["dof_active"] = dof_active;
	r_info["motion_blur_active"] = motion_blur_active;
	r_info["motion_moving_instances"] = int64_t(motion_moving_instances);
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
	if (!read_denoised.is_empty()) {
		r_info["denoised_image"] = read_denoised;
		r_info["denoised_mean"] = read_denoised_mean;
		r_info["denoised_nonfinite"] = int64_t(read_denoised_nonfinite);
		r_info["denoised_hash"] = String::num_uint64(read_denoised_hash, 16);
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

// The D16 table entry of a material (#27): StandardMaterial3D's constant parameters, without textures.
// Converts sRGB color parameters to linear (p_linear_colors = true) so both the D16 reference and the
// product path tracer match the Forward+ material shaders and SRC material atlas.
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

// GodotNext: path_tracer_lights_camera (#169). Collects all scenario lights (directional, dynamic, static, and
// bake-disabled positional lights, plus any view-list lights) in deterministic (type, rid_id) order without
// camera culling (fixes #167 review finding R14).
static void _path_tracer_collect_scenario_lights(const RenderDataRD *p_render_data, LocalVector<PathTracerLightEntry> &r_entries) {
	r_entries.clear();
	LightStorage *light_storage = LightStorage::get_singleton();
	if (light_storage == nullptr || p_render_data == nullptr) {
		return;
	}
	LocalVector<PathTracerLightEntry> raw;
	auto append_instance = [&](RID p_instance, bool p_bake_disabled) {
		if (!p_instance.is_valid() || !light_storage->owns_light_instance(p_instance)) {
			return;
		}
		const RID light = light_storage->light_instance_get_base_light(p_instance);
		if (!light.is_valid() || !light_storage->owns_light(light)) {
			return;
		}
		const RSE::LightType type = light_storage->light_get_type(light);
		if (type == RSE::LIGHT_DIRECTIONAL && light_storage->light_directional_get_sky_mode(light) == RSE::LIGHT_DIRECTIONAL_SKY_MODE_SKY_ONLY) {
			return;
		}
		PathTracerLightEntry entry;
		entry.type = uint32_t(type);
		entry.rid_id = p_instance.get_id();
		entry.instance = p_instance;
		entry.bake_disabled = p_bake_disabled;
		raw.push_back(entry);
	};

	const RendererSceneRender::RenderSDFGIUpdateData *update = p_render_data->sdfgi_update_data;
	if (update != nullptr) {
		if (update->directional_lights != nullptr) {
			for (int i = 0; i < update->directional_lights->size(); i++) {
				append_instance((*update->directional_lights)[i], false);
			}
		}
		if (update->nextgi_dynamic_light_instances != nullptr) {
			for (uint32_t i = 0; i < update->nextgi_dynamic_light_count; i++) {
				append_instance(update->nextgi_dynamic_light_instances[i], false);
			}
		}
		if (update->nextgi_static_light_instances != nullptr) {
			for (uint32_t i = 0; i < update->nextgi_static_light_count; i++) {
				append_instance(update->nextgi_static_light_instances[i], false);
			}
		}
		if (update->nextgi_disabled_light_instances != nullptr) {
			for (uint32_t i = 0; i < update->nextgi_disabled_light_count; i++) {
				append_instance(update->nextgi_disabled_light_instances[i], true);
			}
		}
	}
	if (p_render_data->lights != nullptr) {
		for (uint32_t i = 0; i < p_render_data->lights->size(); i++) {
			append_instance((*p_render_data->lights)[i], false);
		}
	}
	path_tracer_collect_and_sort_lights(raw.ptr(), raw.size(), r_entries);
}

// GodotNext: path_tracer_lights_camera (#169). Builds the product light table (9 vec4s = 144 B per light, all
// Godot light types, IES profiles, projectors, barn doors, and area textures, in unexposed linear radiance).
static uint32_t _path_tracer_build_product_lights(const RenderDataRD *p_render_data, LocalVector<float> &r_lights) {
	r_lights.clear();
	LightStorage *light_storage = LightStorage::get_singleton();
	TextureStorage *texture_storage = TextureStorage::get_singleton();
	if (light_storage == nullptr || p_render_data == nullptr) {
		r_lights.resize_initialized(PATH_TRACER_PRODUCT_LIGHT_FLOATS);
		return 0;
	}
	const bool physical = RendererSceneRenderRD::get_singleton() != nullptr && RendererSceneRenderRD::get_singleton()->is_using_physical_light_units();

	LocalVector<PathTracerLightEntry> entries;
	_path_tracer_collect_scenario_lights(p_render_data, entries);
	if (entries.is_empty()) {
		r_lights.resize_initialized(PATH_TRACER_PRODUCT_LIGHT_FLOATS);
		return 0;
	}

	LocalVector<PathTracerGPULight> gpu_lights;
	gpu_lights.reserve(entries.size());

	for (const PathTracerLightEntry &entry : entries) {
		const RID instance = entry.instance;
		const RID light = light_storage->light_instance_get_base_light(instance);
		if (!light.is_valid()) {
			continue;
		}
		const RSE::LightType type = light_storage->light_get_type(light);
		const Transform3D xform = light_storage->light_instance_get_base_transform(instance);
		const Color linear_col = light_storage->light_get_color(light).srgb_to_linear();
		const bool negative = light_storage->light_is_negative(light);
		const bool has_shadow = light_storage->light_has_shadow(light);

		const Vector3 right = xform.basis.get_column(Vector3::AXIS_X).normalized();
		const Vector3 up = xform.basis.get_column(Vector3::AXIS_Y).normalized();
		const Vector3 back_z = xform.basis.get_column(Vector3::AXIS_Z).normalized(); // +Z (TO directional light)
		const Vector3 emit_dir = -back_z; // -Z (emission direction for spot / area / directional)

		PathTracerGPULight gl;
		gl.flags = 0u;
		if (has_shadow) {
			gl.flags |= PATH_TRACER_LIGHT_FLAG_CAST_SHADOW;
		}
		if (negative) {
			gl.flags |= PATH_TRACER_LIGHT_FLAG_NEGATIVE;
		}
		gl.shadow_extra[0] = has_shadow ? CLAMP(light_storage->light_get_param(light, RSE::LIGHT_PARAM_SHADOW_OPACITY), 0.0f, 1.0f) : 0.0f;
		gl.color_spec[3] = light_storage->light_get_param(light, RSE::LIGHT_PARAM_SPECULAR) * 2.0f;

		gl.dir_decay[0] = emit_dir.x;
		gl.dir_decay[1] = emit_dir.y;
		gl.dir_decay[2] = emit_dir.z;
		gl.dir_decay[3] = light_storage->light_get_param(light, RSE::LIGHT_PARAM_ATTENUATION);

		gl.right_size[0] = right.x;
		gl.right_size[1] = right.y;
		gl.right_size[2] = right.z;

		gl.up_param[0] = up.x;
		gl.up_param[1] = up.y;
		gl.up_param[2] = up.z;

		gl.ies_layer = 0xFFFFFFFFu;
		gl.spot_barn_ies[3] = 1.0f;

		if (type == RSE::LIGHT_DIRECTIONAL) {
			gl.type = PATH_TRACER_LIGHT_DIRECTIONAL;
			float energy = light_storage->light_get_param(light, RSE::LIGHT_PARAM_ENERGY);
			if (physical) {
				energy *= light_storage->light_get_param(light, RSE::LIGHT_PARAM_INTENSITY);
			} else {
				energy *= float(Math::PI);
			}
			gl.color_spec[0] = linear_col.r * energy;
			gl.color_spec[1] = linear_col.g * energy;
			gl.color_spec[2] = linear_col.b * energy;

			const float ang_deg = CLAMP(light_storage->light_get_param(light, RSE::LIGHT_PARAM_SIZE), 0.0f, 89.0f);
			const float ang_rad = Math::deg_to_rad(ang_deg);
			gl.pos_dir_range[0] = back_z.x;
			gl.pos_dir_range[1] = back_z.y;
			gl.pos_dir_range[2] = back_z.z;
			gl.pos_dir_range[3] = Math::cos(ang_rad);
			gl.right_size[3] = ang_rad;
			gl.shadow_extra[1] = Math::sin(ang_rad);
		} else {
			// Resolve #140 IES profile through the shared LightStorage atlas.
			const RID ies_profile = light_storage->light_get_ies_profile(light);
			uint32_t ies_layer = 0xFFFFFFFFu;
			float ies_peak_candela = 1.0f;
			if (ies_profile.is_valid()) {
				ies_layer = light_storage->ies_profile_get_atlas_layer(ies_profile);
				ies_peak_candela = light_storage->ies_profile_get_peak_candela(ies_profile);
			}
			const bool ies_active = ies_layer != 0xFFFFFFFFu;
			if (ies_active) {
				gl.flags |= PATH_TRACER_LIGHT_FLAG_IES;
				gl.ies_layer = ies_layer;
				gl.spot_barn_ies[3] = light_storage->light_get_ies_intensity_scale(light);
			}

			float energy = light_storage->light_get_param(light, RSE::LIGHT_PARAM_ENERGY);
			if (ies_active && light_storage->light_get_ies_use_intensity(light)) {
				energy *= physical ? ies_peak_candela : (float(Math::PI) * ies_peak_candela);
			} else if (physical) {
				energy *= light_storage->light_get_param(light, RSE::LIGHT_PARAM_INTENSITY);
				if (type == RSE::LIGHT_OMNI) {
					energy /= (4.0f * float(Math::PI));
				} else if (type == RSE::LIGHT_AREA) {
					energy /= (2.0f * float(Math::PI));
				} else {
					energy /= float(Math::PI);
				}
			} else {
				energy *= float(Math::PI);
			}

			const float range = MAX(light_storage->light_get_param(light, RSE::LIGHT_PARAM_RANGE), 0.001f);
			gl.pos_dir_range[0] = xform.origin.x;
			gl.pos_dir_range[1] = xform.origin.y;
			gl.pos_dir_range[2] = xform.origin.z;
			gl.pos_dir_range[3] = range;
			gl.inv_range = 1.0f / range;

			if (type == RSE::LIGHT_OMNI) {
				gl.type = PATH_TRACER_LIGHT_OMNI;
				gl.right_size[3] = MAX(light_storage->light_get_param(light, RSE::LIGHT_PARAM_SIZE), 0.0f);
				const RID projector = light_storage->light_get_projector(light);
				if (projector.is_valid() && texture_storage != nullptr) {
					const Rect2 rect = texture_storage->decal_atlas_get_texture_rect(projector);
					if (rect.size.width > 0.0f && rect.size.height > 0.0f) {
						gl.flags |= PATH_TRACER_LIGHT_FLAG_HAS_PROJECTOR;
						gl.tex_rect[0] = rect.position.x;
						gl.tex_rect[1] = rect.position.y;
						gl.tex_rect[2] = rect.size.width;
						gl.tex_rect[3] = rect.size.height * 0.5f; // Dual-paraboloid half-height per hemisphere.
					}
				}
			} else if (type == RSE::LIGHT_SPOT) {
				gl.type = PATH_TRACER_LIGHT_SPOT;
				gl.right_size[3] = MAX(light_storage->light_get_param(light, RSE::LIGHT_PARAM_SIZE), 0.0f);
				const float spot_deg = CLAMP(light_storage->light_get_param(light, RSE::LIGHT_PARAM_SPOT_ANGLE), 0.01f, 89.9f);
				const float spot_rad = Math::deg_to_rad(spot_deg);
				gl.up_param[3] = Math::cos(spot_rad);
				gl.spot_barn_ies[0] = 1.0f / MAX(light_storage->light_get_param(light, RSE::LIGHT_PARAM_SPOT_ATTENUATION), 1e-4f);
				gl.shadow_extra[2] = Math::tan(spot_rad);
				const RID projector = light_storage->light_get_projector(light);
				if (projector.is_valid() && texture_storage != nullptr) {
					const Rect2 rect = texture_storage->decal_atlas_get_texture_rect(projector);
					if (rect.size.width > 0.0f && rect.size.height > 0.0f) {
						gl.flags |= PATH_TRACER_LIGHT_FLAG_HAS_PROJECTOR;
						gl.tex_rect[0] = rect.position.x;
						gl.tex_rect[1] = rect.position.y;
						gl.tex_rect[2] = rect.size.width;
						gl.tex_rect[3] = rect.size.height;
					}
				}
			} else if (type == RSE::LIGHT_AREA) {
				gl.type = PATH_TRACER_LIGHT_AREA;
				const Vector2 area_size = light_storage->light_area_get_size(light);
				const float half_w = MAX(area_size.x * 0.5f, 1e-4f);
				const float half_h = MAX(area_size.y * 0.5f, 1e-4f);
				gl.right_size[3] = half_w;
				gl.up_param[3] = half_h;
				if (light_storage->light_area_get_normalize_energy(light)) {
					energy /= MAX(area_size.x * area_size.y, 1e-6f);
				}
				gl.spot_barn_ies[1] = float(Math::deg_to_rad(CLAMP(double(light_storage->light_get_param(light, RSE::LIGHT_PARAM_BARN_DOOR_ANGLE)), 0.0, 90.0)));
				gl.spot_barn_ies[2] = MAX(light_storage->light_get_param(light, RSE::LIGHT_PARAM_BARN_DOOR_LENGTH), 0.0f);
				const RID area_tex = light_storage->light_area_get_texture(light);
				if (area_tex.is_valid() && texture_storage != nullptr) {
					const Rect2 rect = texture_storage->area_light_atlas_get_texture_rect(area_tex);
					if (rect.size.width > 0.0f && rect.size.height > 0.0f) {
						gl.flags |= PATH_TRACER_LIGHT_FLAG_HAS_AREA_TEX;
						gl.tex_rect[0] = rect.position.x;
						gl.tex_rect[1] = rect.position.y;
						gl.tex_rect[2] = rect.size.width;
						gl.tex_rect[3] = rect.size.height;
					}
				}
			}

			gl.color_spec[0] = linear_col.r * energy;
			gl.color_spec[1] = linear_col.g * energy;
			gl.color_spec[2] = linear_col.b * energy;
		}
		gpu_lights.push_back(gl);
	}

	if (gpu_lights.is_empty()) {
		r_lights.resize_initialized(PATH_TRACER_PRODUCT_LIGHT_FLOATS);
		return 0;
	}
	r_lights.resize(gpu_lights.size() * PATH_TRACER_PRODUCT_LIGHT_FLOATS);
	memcpy(r_lights.ptr(), gpu_lights.ptr(), gpu_lights.size() * sizeof(PathTracerGPULight));
	return gpu_lights.size();
}

// Up to PATH_TRACER_MAX_LIGHTS omni, spot and directional lights, 16 floats each (the D16 reference light table).
static uint32_t _path_tracer_build_lights(const RenderDataRD *p_render_data, LocalVector<float> &r_lights) {
	r_lights.resize(PATH_TRACER_MAX_LIGHTS * PATH_TRACER_LIGHT_FLOATS);
	for (uint32_t i = 0; i < r_lights.size(); i++) {
		r_lights[i] = 0.0f;
	}
	uint32_t light_count = 0;
	LightStorage *light_storage = LightStorage::get_singleton();
	if (light_storage == nullptr) {
		return 0;
	}
	if (p_render_data->lights != nullptr) {
		for (uint32_t i = 0; i < p_render_data->lights->size() && light_count < PATH_TRACER_MAX_LIGHTS; i++) {
			const RID instance = (*p_render_data->lights)[i];
			const RID light = light_storage->light_instance_get_base_light(instance);
			if (!light.is_valid()) {
				continue;
			}
			const RSE::LightType type = light_storage->light_get_type(light);
			if (type == RSE::LIGHT_DIRECTIONAL) {
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
			if (type == RSE::LIGHT_SPOT) {
				const Vector3 spot_dir = -xform.basis.get_column(Vector3::AXIS_Z).normalized();
				const float spot_deg = CLAMP(light_storage->light_get_param(light, RSE::LIGHT_PARAM_SPOT_ANGLE), 0.01f, 89.9f);
				const float cos_spot = Math::cos(Math::deg_to_rad(spot_deg));
				const float inv_spot_atten = 1.0f / MAX(light_storage->light_get_param(light, RSE::LIGHT_PARAM_SPOT_ATTENUATION), 1e-4f);
				r_lights[base + 10] = 2.0f; // extra.z > 1.5 marks spotlight in nextgi_reference_pt_inc.glsl
				r_lights[base + 11] = cos_spot;
				r_lights[base + 12] = spot_dir.x;
				r_lights[base + 13] = spot_dir.y;
				r_lights[base + 14] = spot_dir.z;
				r_lights[base + 15] = inv_spot_atten;
			}
			light_count++;
		}
	}
	// Include DirectionalLight3D instances (e.g. Concave Courtyard sun) so D16 references light outdoor scenes.
	const RendererSceneRender::RenderSDFGIUpdateData *update = p_render_data->sdfgi_update_data;
	if (update != nullptr && update->directional_lights != nullptr) {
		for (int i = 0; i < update->directional_lights->size() && light_count < PATH_TRACER_MAX_LIGHTS; i++) {
			const RID instance = (*update->directional_lights)[i];
			const RID light = light_storage->light_instance_get_base_light(instance);
			if (!light.is_valid() || light_storage->light_get_type(light) != RSE::LIGHT_DIRECTIONAL) {
				continue;
			}
			if (light_storage->light_directional_get_sky_mode(light) == RSE::LIGHT_DIRECTIONAL_SKY_MODE_SKY_ONLY) {
				continue;
			}
			const Transform3D xform = light_storage->light_instance_get_base_transform(instance);
			const Vector3 to_light = xform.basis.get_column(Vector3::AXIS_Z).normalized();
			const Color color = light_storage->light_get_color(light);
			const float energy = light_storage->light_get_param(light, RSE::LIGHT_PARAM_ENERGY);
			const float specular = light_storage->light_get_param(light, RSE::LIGHT_PARAM_SPECULAR);
			const uint32_t base = light_count * PATH_TRACER_LIGHT_FLOATS;
			r_lights[base + 0] = to_light.x;
			r_lights[base + 1] = to_light.y;
			r_lights[base + 2] = to_light.z;
			r_lights[base + 3] = -1.0f;
			r_lights[base + 4] = color.r;
			r_lights[base + 5] = color.g;
			r_lights[base + 6] = color.b;
			r_lights[base + 7] = energy;
			r_lights[base + 8] = 0.0f;
			r_lights[base + 9] = specular;
			r_lights[base + 10] = 1.0f; // extra.z > 0.5 marks directional light in nextgi_reference_pt_inc.glsl
			light_count++;
		}
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

// Every light in the scenario (directional, dynamic, static, and bake-disabled positional lights, in deterministic
// order), including IES, projector, area texture, barn-door, shadow, and negative flags, so any light change
// restarts the path tracer's history within one frame.
static uint64_t _path_tracer_hash_lights(const RenderDataRD *p_render_data) {
	uint64_t hash = PATH_TRACER_HASH_BASIS;
	LightStorage *light_storage = LightStorage::get_singleton();
	TextureStorage *texture_storage = TextureStorage::get_singleton();
	if (light_storage == nullptr || p_render_data == nullptr) {
		return hash;
	}
	LocalVector<PathTracerLightEntry> entries;
	_path_tracer_collect_scenario_lights(p_render_data, entries);
	const uint32_t count = entries.size();
	hash = path_tracer_hash_bytes(hash, &count, sizeof(count));
	for (const PathTracerLightEntry &entry : entries) {
		const RID instance = entry.instance;
		const RID light = light_storage->light_instance_get_base_light(instance);
		if (!light.is_valid()) {
			continue;
		}
		float params[RSE::LIGHT_PARAM_MAX];
		for (int p = 0; p < RSE::LIGHT_PARAM_MAX; p++) {
			params[p] = light_storage->light_get_param(light, RSE::LightParam(p));
		}
		const RSE::LightType ltype = light_storage->light_get_type(light);
		hash = path_tracer_hash_light(hash, uint32_t(ltype), light_storage->light_instance_get_base_transform(instance), light_storage->light_get_color(light), params, RSE::LIGHT_PARAM_MAX);
		const RID ies = light_storage->light_get_ies_profile(light);
		const RID proj = light_storage->light_get_projector(light);
		const RID area_tex = light_storage->light_area_get_texture(light);
		const uint64_t extra_ids[5] = {
			uint64_t(light_storage->light_is_negative(light) ? 1u : 0u) |
					(uint64_t(light_storage->light_has_shadow(light) ? 1u : 0u) << 1u) |
					(uint64_t(light_storage->light_get_ies_use_intensity(light) ? 1u : 0u) << 2u) |
					(uint64_t(light_storage->light_area_get_normalize_energy(light) ? 1u : 0u) << 3u) |
					(uint64_t(light_storage->light_directional_get_sky_mode(light)) << 4u),
			ies.get_id(),
			ies.is_valid() ? uint64_t(light_storage->ies_profile_get_atlas_layer(ies)) : 0xFFFFFFFFull,
			proj.get_id(),
			area_tex.get_id(),
		};
		hash = path_tracer_hash_bytes(hash, extra_ids, sizeof(extra_ids));
		const Vector2 area_size = light_storage->light_area_get_size(light);
		const float extra_floats[4] = {
			area_size.x,
			area_size.y,
			light_storage->light_get_ies_intensity_scale(light),
			ies.is_valid() ? light_storage->ies_profile_get_peak_candela(ies) : 0.0f,
		};
		hash = path_tracer_hash_bytes(hash, extra_floats, sizeof(extra_floats));
		if (proj.is_valid() && texture_storage != nullptr) {
			const Rect2 rect = texture_storage->decal_atlas_get_texture_rect(proj);
			const float r[4] = { rect.position.x, rect.position.y, rect.size.width, rect.size.height };
			hash = path_tracer_hash_bytes(hash, r, sizeof(r));
		}
		if (area_tex.is_valid() && texture_storage != nullptr) {
			const Rect2 rect = texture_storage->area_light_atlas_get_texture_rect(area_tex);
			const float r[4] = { rect.position.x, rect.position.y, rect.size.width, rect.size.height };
			hash = path_tracer_hash_bytes(hash, r, sizeof(r));
		}
	}
	return hash;
}

// GodotNext: path_tracer_lights_camera (#169). Collects active FogVolume instances and hashes their parameters.
static uint64_t _path_tracer_collect_fog_volumes(const RenderDataRD *p_render_data, LocalVector<PathTracerGPUFogVolume> &r_volumes) {
	r_volumes.clear();
	uint64_t hash = PATH_TRACER_HASH_BASIS;
	Fog *fog = Fog::get_singleton();
	if (fog == nullptr || p_render_data == nullptr || p_render_data->fog_volumes == nullptr) {
		return hash;
	}
	for (uint32_t i = 0; i < p_render_data->fog_volumes->size(); i++) {
		const RID fvi_rid = (*p_render_data->fog_volumes)[i];
		if (!fog->owns_fog_volume_instance(fvi_rid) || !fog->fog_volume_instance_is_active(fvi_rid)) {
			continue;
		}
		const RID vol_rid = fog->fog_volume_instance_get_volume(fvi_rid);
		if (!fog->owns_fog_volume(vol_rid)) {
			continue;
		}
		const Transform3D xform = fog->fog_volume_instance_get_transform(fvi_rid);
		const Transform3D inv_xform = xform.affine_inverse();
		const Vector3 size = fog->fog_volume_get_size(vol_rid);
		const RSE::FogVolumeShape shape = fog->fog_volume_get_shape(vol_rid);
		const RID material = fog->fog_volume_get_material(vol_rid);

		const float density = _path_tracer_float_param(material, "density", 1.0f);
		const Color albedo = _path_tracer_color_param(material, "albedo", Color(1.0f, 1.0f, 1.0f)).srgb_to_linear();
		const Color emission = _path_tracer_color_param(material, "emission", Color(0.0f, 0.0f, 0.0f)).srgb_to_linear();
		const float height_falloff = MAX(_path_tracer_float_param(material, "height_falloff", 0.0f), 0.0f);
		const float edge_fade = MAX(_path_tracer_float_param(material, "edge_fade", 0.1f), 0.0f);

		PathTracerGPUFogVolume gpu;
		gpu.inv_xform_row0[0] = inv_xform.basis.rows[0][0];
		gpu.inv_xform_row0[1] = inv_xform.basis.rows[0][1];
		gpu.inv_xform_row0[2] = inv_xform.basis.rows[0][2];
		gpu.inv_xform_row0[3] = inv_xform.origin.x;
		gpu.inv_xform_row1[0] = inv_xform.basis.rows[1][0];
		gpu.inv_xform_row1[1] = inv_xform.basis.rows[1][1];
		gpu.inv_xform_row1[2] = inv_xform.basis.rows[1][2];
		gpu.inv_xform_row1[3] = inv_xform.origin.y;
		gpu.inv_xform_row2[0] = inv_xform.basis.rows[2][0];
		gpu.inv_xform_row2[1] = inv_xform.basis.rows[2][1];
		gpu.inv_xform_row2[2] = inv_xform.basis.rows[2][2];
		gpu.inv_xform_row2[3] = inv_xform.origin.z;

		gpu.half_size_shape[0] = MAX(size.x * 0.5f, 1e-4f);
		gpu.half_size_shape[1] = MAX(size.y * 0.5f, 1e-4f);
		gpu.half_size_shape[2] = MAX(size.z * 0.5f, 1e-4f);
		gpu.half_size_shape[3] = float(shape);

		gpu.albedo_density[0] = albedo.r;
		gpu.albedo_density[1] = albedo.g;
		gpu.albedo_density[2] = albedo.b;
		gpu.albedo_density[3] = density;

		gpu.emission_falloff[0] = emission.r;
		gpu.emission_falloff[1] = emission.g;
		gpu.emission_falloff[2] = emission.b;
		gpu.emission_falloff[3] = height_falloff;

		gpu.origin_edge[0] = xform.origin.x;
		gpu.origin_edge[1] = xform.origin.y;
		gpu.origin_edge[2] = xform.origin.z;
		gpu.origin_edge[3] = edge_fade;

		const float min_extent = MAX(MIN(size.x, MIN(size.y, size.z)), 1e-4f);
		gpu.bounds_scale[0] = MAX(density, 0.0f);
		gpu.bounds_scale[1] = 2.0f / min_extent;
		gpu.bounds_scale[2] = 0.0f;
		gpu.bounds_scale[3] = 0.0f;

		hash = path_tracer_hash_bytes(hash, &gpu, sizeof(gpu));
		r_volumes.push_back(gpu);
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
					r_tables.materials.push_back(_path_tracer_table_material(_path_tracer_surface_material(geometry, mesh, part_geometry.surface), r_tables.textured_materials, true));
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
			// Both the D16 reference (build_reference_tables) and the product path tracer convert sRGB
			// material color parameters to linear so reference_match compares identical material tables.
			r_tables.materials[i] = _path_tracer_table_material(material, r_tables.textured_materials, true);
		}
	}
	if (p_linear_colors) {
		r_tables.light_count = _path_tracer_build_product_lights(p_render_data, r_tables.lights);
	} else {
		r_tables.light_count = _path_tracer_build_lights(p_render_data, r_tables.lights);
	}
	_path_tracer_finish_tables(r_tables, hash);
}

/* PATH TRACER */

namespace {

// The product params (PathTracerParams in path_tracer.glsl, HitPTParams in nextgi_hit_layout_inc.glsl):
// the D16 reference's first 128 bytes, product word + camera rays (64 B), and #169 sky, volumetric fog,
// legacy fog, thin-lens DOF, and motion blur blocks (192 B) = 384 bytes total (fits NextGIHitMaterials' 512 B dummy).
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
	uint32_t fog_volume_count = 0;
	PathTracerGPURays rays;
	// GodotNext: path_tracer_lights_camera (#169).
	float sky_params[4] = {}; // x radiance_scale, y border_pad, z border_scale, w unused
	float sky_orient_row0[4] = { 1.0f, 0.0f, 0.0f, 0.0f }; // xyz inv sky orient row 0, w bg_sky_enabled
	float sky_orient_row1[4] = { 0.0f, 1.0f, 0.0f, float(PATH_TRACER_SKY_CDF_DIM) };
	float sky_orient_row2[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
	float vfog_params0[4] = {}; // x world_density, y anisotropy, z vfog_length, w vfog_sky_affect
	float vfog_scattering[4] = {}; // rgb scattering albedo, w majorant_world
	float vfog_emission[4] = {}; // rgb emission * energy, w fog_mode (0 off, 1 volumetric, 2 legacy)
	float fog_legacy0[4] = {}; // rgb fog_light_color * energy, w fog_density
	float fog_legacy1[4] = {}; // x fog_height, y fog_height_density, z fog_depth_begin, w fog_depth_end
	float fog_legacy2[4] = {}; // x fog_depth_curve, y fog_sky_affect, z fog_depth_mode, w fog_sun_scatter
	float dof_params[4] = {}; // x focus_distance, y aperture_radius, z near_plane_dist, w dof_enabled
	uint32_t motion_params[4] = {}; // x motion_slices, y shutter_bits, z sky_cdf_enabled, w pad
};
static_assert(sizeof(PathTracerGPUParams) == 384, "The path tracer params must match path_tracer.glsl and nextgi_hit_layout_inc.glsl.");

// GodotNext: path_tracer_lights_camera (#169). Push constants for path_tracer_sky_cdf.glsl.
struct PathTracerSkyCdfPush {
	float border_pad = 0.0f;
	float border_scale = 1.0f;
	float radiance_scale = 1.0f;
	float pad = 0.0f;
};
static_assert(sizeof(PathTracerSkyCdfPush) == 16, "Sky CDF push constants must match path_tracer_sky_cdf.glsl.");

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

// GodotNext: path_tracer_denoise (#168). Push constants matching path_tracer_denoise.glsl.
struct PathTracerDenoisePush {
	uint32_t width = 0;
	uint32_t height = 0;
	float exposure = 1.0f;
	float spatial_bandwidth = 5.0f;
	float light_bandwidth = 0.1f;
	float albedo_bandwidth = 0.2f;
	float normal_bandwidth = 0.1f;
	float filter_strength = 10.0f;
	int32_t half_search_window = 5;
	uint32_t pad0 = 0;
	uint32_t pad1 = 0;
	uint32_t pad2 = 0;
};
static_assert(sizeof(PathTracerDenoisePush) == 48, "Denoise push constants must match path_tracer_denoise.glsl.");

Mutex g_oidn_backend_mutex;
const PathTracerOIDNBackend *g_oidn_backend = nullptr;

} // namespace

void PathTracer::register_oidn_backend(const PathTracerOIDNBackend *p_backend) {
	MutexLock lock(g_oidn_backend_mutex);
	g_oidn_backend = p_backend;
}

void PathTracer::unregister_oidn_backend(const PathTracerOIDNBackend *p_backend) {
	MutexLock lock(g_oidn_backend_mutex);
	if (g_oidn_backend == p_backend) {
		g_oidn_backend = nullptr;
	}
}

bool PathTracer::is_oidn_available() {
	MutexLock lock(g_oidn_backend_mutex);
	return g_oidn_backend != nullptr && g_oidn_backend->denoise != nullptr;
}

const char *PathTracer::get_oidn_backend_name() {
	MutexLock lock(g_oidn_backend_mutex);
	if (g_oidn_backend != nullptr && g_oidn_backend->denoise != nullptr && g_oidn_backend->name != nullptr) {
		return g_oidn_backend->name;
	}
	return "none";
}

Error PathTracer::run_oidn_denoise(const PathTracerOIDNInput &p_input, String &r_error) {
	const PathTracerOIDNBackend *backend = nullptr;
	{
		MutexLock lock(g_oidn_backend_mutex);
		backend = g_oidn_backend;
	}
	if (backend == nullptr || backend->denoise == nullptr) {
		r_error = "OIDN denoiser module is not enabled in this build.";
		return ERR_UNAVAILABLE;
	}
	return backend->denoise(p_input, r_error);
}

struct PathTracer::ShaderState {
	PathTracerShaderRD shader;
	RID version;
	RID accumulate;
	RID composite;
	RID params_buffer;
	// GodotNext: path_tracer_denoise (#168). Plain compute shader (no ray query).
	PathTracerDenoiseShaderRD denoise_shader;
	RID denoise_version;
	RID denoise_pipeline;
	// GodotNext: path_tracer_lights_camera (#169). Sky 2D CDF compute shader (plain compute, no ray query).
	PathTracerSkyCdfShaderRD sky_cdf_shader;
	RID sky_cdf_version;
	RID sky_cdf_rows_pipeline;
	RID sky_cdf_marginal_pipeline;
};

Vector<String> PathTracer::get_shader_variant_defines() {
	Vector<String> defines;
	defines.push_back("");
	defines.push_back("#define MODE_COMPOSITE\n");
	return defines;
}

Vector<String> PathTracer::get_denoise_shader_variant_defines() {
	Vector<String> defines;
	defines.push_back("");
	return defines;
}

Vector<String> PathTracer::get_sky_cdf_shader_variant_defines() {
	Vector<String> defines;
	defines.push_back("");
	defines.push_back("#define MODE_MARGINAL\n");
	return defines;
}

bool PathTracer::is_compositing(RenderSceneBuffersRD *p_render_buffers) {
	PathTracerView *view = get_view(p_render_buffers, false);
	if (view == nullptr || view->settings.reference_match) {
		return false;
	}
	if (view->status != PATH_TRACER_STATUS_ACTIVE && view->status != PATH_TRACER_STATUS_CONVERGED && view->status != PATH_TRACER_STATUS_NO_TLAS) {
		return false;
	}
	return view->samples > 0 && view->accumulation.is_valid();
}

bool PathTracer::_ensure_sky_cdf_shader() {
	if (shader == nullptr) {
		return false;
	}
	if (shader->sky_cdf_version.is_valid() && shader->sky_cdf_rows_pipeline.is_valid() && shader->sky_cdf_marginal_pipeline.is_valid()) {
		return true;
	}
	if (logged_sky_cdf_compile_failure) {
		return false;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	shader->sky_cdf_shader.initialize(get_sky_cdf_shader_variant_defines());
	shader->sky_cdf_version = shader->sky_cdf_shader.version_create(false);
	if (shader->sky_cdf_version.is_valid()) {
		shader->sky_cdf_rows_pipeline = rd->compute_pipeline_create(shader->sky_cdf_shader.version_get_shader(shader->sky_cdf_version, 0));
		shader->sky_cdf_marginal_pipeline = rd->compute_pipeline_create(shader->sky_cdf_shader.version_get_shader(shader->sky_cdf_version, 1));
	}
	if (!shader->sky_cdf_version.is_valid() || !shader->sky_cdf_rows_pipeline.is_valid() || !shader->sky_cdf_marginal_pipeline.is_valid()) {
		if (shader->sky_cdf_version.is_valid()) {
			shader->sky_cdf_shader.version_free(shader->sky_cdf_version);
			shader->sky_cdf_version = RID();
		}
		shader->sky_cdf_rows_pipeline = RID();
		shader->sky_cdf_marginal_pipeline = RID();
		logged_sky_cdf_compile_failure = true;
		ERR_FAIL_V_MSG(false, "The path tracer sky CDF shader failed to compile.");
	}
	return true;
}

bool PathTracer::_ensure_denoise_shader() {
	if (shader == nullptr) {
		return false;
	}
	if (shader->denoise_version.is_valid() && shader->denoise_pipeline.is_valid()) {
		return true;
	}
	if (logged_denoise_compile_failure) {
		return false;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	shader->denoise_shader.initialize(get_denoise_shader_variant_defines());
	shader->denoise_version = shader->denoise_shader.version_create(false);
	if (shader->denoise_version.is_valid()) {
		shader->denoise_pipeline = rd->compute_pipeline_create(shader->denoise_shader.version_get_shader(shader->denoise_version, 0));
	}
	if (!shader->denoise_version.is_valid() || !shader->denoise_pipeline.is_valid()) {
		if (shader->denoise_version.is_valid()) {
			shader->denoise_shader.version_free(shader->denoise_version);
			shader->denoise_version = RID();
		}
		shader->denoise_pipeline = RID();
		logged_denoise_compile_failure = true;
		ERR_FAIL_V_MSG(false, "The path tracer denoiser shader failed to compile.");
	}
	return true;
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
		settings.denoiser = RenderingServerTypes::path_tracing_validate_denoiser(int(p_render_buffers->get_path_tracing_denoiser()));
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
	const uint32_t light_bytes = MAX(uint32_t(p_tables.lights.size() * sizeof(float)), uint32_t(sizeof(PathTracerGPULight)));
	if (!light_buffer.is_valid() || light_capacity < light_bytes) {
		if (light_buffer.is_valid()) {
			rd->free_rid(light_buffer);
		}
		light_capacity = Math::next_power_of_2(MAX(light_bytes, 256u));
		light_buffer = rd->storage_buffer_create(light_capacity);
	}
	if (!p_tables.lights.is_empty()) {
		rd->buffer_update(light_buffer, 0, p_tables.lights.size() * sizeof(float), p_tables.lights.ptr());
	}
}

void PathTracer::_upload_fog_volumes(const LocalVector<PathTracerGPUFogVolume> &p_volumes) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	const uint32_t vol_bytes = MAX(uint32_t(p_volumes.size() * sizeof(PathTracerGPUFogVolume)), uint32_t(sizeof(PathTracerGPUFogVolume)));
	if (!fog_volume_buffer.is_valid() || fog_volume_capacity < vol_bytes) {
		if (fog_volume_buffer.is_valid()) {
			rd->free_rid(fog_volume_buffer);
		}
		fog_volume_capacity = Math::next_power_of_2(MAX(vol_bytes, 256u));
		fog_volume_buffer = rd->storage_buffer_create(fog_volume_capacity);
	}
	if (!p_volumes.is_empty()) {
		rd->buffer_update(fog_volume_buffer, 0, p_volumes.size() * sizeof(PathTracerGPUFogVolume), p_volumes.ptr());
	}
}

bool PathTracer::_update_sky_cdf(RID p_sky_octmap, float p_border_pad, float p_border_scale, float p_radiance_scale, uint64_t p_key, bool p_force_rebuild) {
	if (!p_sky_octmap.is_valid() || !_ensure_sky_cdf_shader()) {
		return false;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	ERR_FAIL_NULL_V(material_storage, false);
	if (!sky_cdf_buffer.is_valid()) {
		sky_cdf_buffer = rd->storage_buffer_create(PATH_TRACER_SKY_CDF_FLOATS * sizeof(float));
		p_force_rebuild = true;
	}
	if (!sky_cdf_buffer.is_valid()) {
		return false;
	}
	if (!p_force_rebuild && sky_cdf_valid && sky_cdf_key == p_key) {
		return true;
	}
	const RID sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RD::Uniform uniforms[3] = {
		RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 0, p_sky_octmap),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 1, sampler),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, sky_cdf_buffer),
	};
	const RID set_rows = rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 3), shader->sky_cdf_shader.version_get_shader(shader->sky_cdf_version, 0), 0);
	const RID set_marginal = rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 3), shader->sky_cdf_shader.version_get_shader(shader->sky_cdf_version, 1), 0);
	if (!set_rows.is_valid() || !set_marginal.is_valid()) {
		if (set_rows.is_valid()) {
			rd->free_rid(set_rows);
		}
		if (set_marginal.is_valid()) {
			rd->free_rid(set_marginal);
		}
		return false;
	}
	PathTracerSkyCdfPush push;
	push.border_pad = p_border_pad;
	push.border_scale = p_border_scale;
	push.radiance_scale = p_radiance_scale;

	rd->draw_command_begin_label("Path Tracer Sky CDF");
	const RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, shader->sky_cdf_rows_pipeline);
	rd->compute_list_bind_uniform_set(list, set_rows, 0);
	rd->compute_list_set_push_constant(list, &push, sizeof(push));
	rd->compute_list_dispatch(list, PATH_TRACER_SKY_CDF_DIM, 1, 1);
	rd->compute_list_add_barrier(list);
	rd->compute_list_bind_compute_pipeline(list, shader->sky_cdf_marginal_pipeline);
	rd->compute_list_bind_uniform_set(list, set_marginal, 0);
	rd->compute_list_set_push_constant(list, &push, sizeof(push));
	rd->compute_list_dispatch(list, 1, 1, 1);
	rd->compute_list_end();
	rd->draw_command_end_label();
	rd->free_rid(set_rows);
	rd->free_rid(set_marginal);

	sky_cdf_key = p_key;
	sky_cdf_valid = true;
	return true;
}

uint32_t PathTracer::_update_motion_streaks(const NextGIRTScene *p_rt_scene, uint64_t p_frame, float p_shutter) {
	if (p_rt_scene == nullptr || p_shutter <= 1e-4f) {
		return 0;
	}
	const LocalVector<NextGIRTScene::MotionInstance> &instances = p_rt_scene->get_motion_instances();
	if (motion_streaks_frame != p_frame) {
		motion_streaks_frame = p_frame;
		for (const NextGIRTScene::MotionInstance &mi : instances) {
			if (!mi.has_prev_transform || mi.instance == nullptr) {
				continue;
			}
			const uint64_t key = uint64_t(uintptr_t(mi.instance));
			MotionStreak *ms = motion_streaks.getptr(key);
			if (ms == nullptr) {
				MotionStreak fresh;
				fresh.last_frame = p_frame;
				fresh.streak = 1;
				motion_streaks.insert(key, fresh);
			} else if (ms->last_frame != p_frame) {
				ms->streak = (ms->last_frame + 1 == p_frame) ? (ms->streak + 1) : 1;
				ms->last_frame = p_frame;
			}
		}
		LocalVector<uint64_t> stale;
		for (const KeyValue<uint64_t, MotionStreak> &E : motion_streaks) {
			if (E.value.last_frame + 4 < p_frame) {
				stale.push_back(E.key);
			}
		}
		for (uint64_t k : stale) {
			motion_streaks.erase(k);
		}
	}
	uint32_t active_moving = 0;
	for (const NextGIRTScene::MotionInstance &mi : instances) {
		if (!mi.has_prev_transform || mi.instance == nullptr) {
			continue;
		}
		const MotionStreak *ms = motion_streaks.getptr(uint64_t(uintptr_t(mi.instance)));
		if (ms != nullptr && ms->last_frame == p_frame && ms->streak >= 2) {
			active_moving++;
		}
	}
	return active_moving;
}

bool PathTracer::_build_motion_tlas(const NextGIRTScene *p_rt_scene, uint64_t p_frame, uint32_t p_sample_index, float p_shutter, uint32_t &r_record_count, uint32_t &r_moving_count) {
	r_record_count = 0;
	r_moving_count = 0;
	if (p_rt_scene == nullptr || p_shutter <= 1e-4f) {
		return false;
	}
	const LocalVector<NextGIRTScene::MotionInstance> &instances = p_rt_scene->get_motion_instances();
	if (instances.is_empty()) {
		return false;
	}
	const uint32_t active_moving = _update_motion_streaks(p_rt_scene, p_frame, p_shutter);
	if (active_moving == 0) {
		return false;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);

	LocalVector<RD::AccelerationStructureInstance> tlas_instances;
	LocalVector<NextGIInstanceRecord> motion_records;
	tlas_instances.reserve(instances.size() + active_moving * (PATH_TRACER_MOTION_SLICES - 1));
	motion_records.reserve(instances.size() + active_moving * (PATH_TRACER_MOTION_SLICES - 1));

	auto store_xform = [](NextGIInstanceRecord &r_rec, const Transform3D &p_xform) {
		for (int row = 0; row < 3; row++) {
			r_rec.object_to_world[row * 4 + 0] = p_xform.basis.rows[row][0];
			r_rec.object_to_world[row * 4 + 1] = p_xform.basis.rows[row][1];
			r_rec.object_to_world[row * 4 + 2] = p_xform.basis.rows[row][2];
		}
		r_rec.object_to_world[3] = p_xform.origin.x;
		r_rec.object_to_world[7] = p_xform.origin.y;
		r_rec.object_to_world[11] = p_xform.origin.z;
	};

	uint32_t moving_emitted = 0;
	for (const NextGIRTScene::MotionInstance &mi : instances) {
		if (!mi.blas.is_valid()) {
			continue;
		}
		bool blur_this = false;
		if (mi.has_prev_transform && mi.instance != nullptr) {
			const MotionStreak *ms = motion_streaks.getptr(uint64_t(uintptr_t(mi.instance)));
			blur_this = ms != nullptr && ms->last_frame == p_frame && ms->streak >= 2;
		}
		if (blur_this) {
			if (motion_records.size() + PATH_TRACER_MOTION_SLICES > NEXTGI_RT_TLAS_CAPACITY || motion_records.size() + PATH_TRACER_MOTION_SLICES > NEXTGI_RT_RECORD_INDEX_MAX) {
				blur_this = false;
			}
		}
		if (blur_this) {
			moving_emitted++;
			for (uint32_t s = 0; s < PATH_TRACER_MOTION_SLICES; s++) {
				const float t = path_tracer_shutter_time(p_sample_index, s, PATH_TRACER_MOTION_SLICES, p_shutter);
				const Transform3D xform = path_tracer_interpolate_transform(mi.prev_transform, mi.transform, t);
				const uint32_t rec_idx = motion_records.size();
				NextGIInstanceRecord rec = mi.record;
				store_xform(rec, xform);
				rec.tlas_index = rec_idx;
				motion_records.push_back(rec);

				RD::AccelerationStructureInstance ti;
				ti.transform = xform;
				ti.id = rec_idx;
				ti.mask = 1u << s;
				ti.hit_sbt_range = mi.hit_sbt_range;
				ti.flags = mi.tlas_flags;
				ti.blas = mi.blas;
				tlas_instances.push_back(ti);
			}
		} else {
			if (motion_records.size() >= NEXTGI_RT_TLAS_CAPACITY || motion_records.size() > NEXTGI_RT_RECORD_INDEX_MAX) {
				continue;
			}
			const uint32_t rec_idx = motion_records.size();
			NextGIInstanceRecord rec = mi.record;
			rec.tlas_index = rec_idx;
			motion_records.push_back(rec);

			RD::AccelerationStructureInstance ti;
			ti.transform = mi.transform;
			ti.id = rec_idx;
			ti.mask = 0xFFu; // Static instances visible to all 8 shutter slices.
			ti.hit_sbt_range = mi.hit_sbt_range;
			ti.flags = mi.tlas_flags;
			ti.blas = mi.blas;
			tlas_instances.push_back(ti);
		}
	}

	if (moving_emitted == 0 || tlas_instances.is_empty()) {
		return false;
	}
	if (!motion_tlas.is_valid()) {
		motion_tlas = rd->tlas_create(NEXTGI_RT_TLAS_CAPACITY, RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT);
	}
	if (!motion_tlas.is_valid()) {
		return false;
	}
	const uint32_t record_bytes = motion_records.size() * sizeof(NextGIInstanceRecord);
	if (!motion_record_buffer.is_valid() || motion_record_capacity < record_bytes) {
		if (motion_record_buffer.is_valid()) {
			rd->free_rid(motion_record_buffer);
		}
		motion_record_capacity = Math::next_power_of_2(MAX(record_bytes, uint32_t(64 * sizeof(NextGIInstanceRecord))));
		motion_record_buffer = rd->storage_buffer_create(motion_record_capacity);
	}
	if (!motion_record_buffer.is_valid()) {
		return false;
	}
	rd->buffer_update(motion_record_buffer, 0, record_bytes, motion_records.ptr());
	if (rd->tlas_build(motion_tlas, Span<RD::AccelerationStructureInstance>(tlas_instances.ptr(), tlas_instances.size())) != OK) {
		return false;
	}
	r_record_count = motion_records.size();
	r_moving_count = moving_emitted;
	return true;
}

void PathTracer::render(const RenderDataRD *p_render_data, NextGIRTScene *p_rt_scene, NextGIHitMaterials *p_hit_materials) {
	if (p_render_data == nullptr || p_render_data->render_buffers.is_null() || p_render_data->scene_data == nullptr) {
		return;
	}
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	PathTracerSettings settings = get_settings(rb.ptr());
	settings.denoise_requested = rb->consume_path_tracing_denoise_requested();
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
	const uint64_t frame = RSG::rasterizer != nullptr ? RSG::rasterizer->get_frame_number() : 0;

	// GodotNext: path_tracer_lights_camera (#169). Gather sky, fog, FogVolumes, DOF, and motion state.
	const PathTracerGPUCamera cam_block = path_tracer_camera(p_render_data->scene_data->cam_transform, p_render_data->scene_data->cam_projection, size);
	PathTracerDOFParams dof;
	if (!settings.reference_match && p_render_data->camera_attributes.is_valid() && RSG::camera_attributes != nullptr && RSG::camera_attributes->camera_attributes_uses_dof(p_render_data->camera_attributes)) {
		dof = path_tracer_compute_dof_params(
				RSG::camera_attributes->camera_attributes_get_dof_far_enabled(p_render_data->camera_attributes),
				RSG::camera_attributes->camera_attributes_get_dof_far_distance(p_render_data->camera_attributes),
				RSG::camera_attributes->camera_attributes_get_dof_far_transition(p_render_data->camera_attributes),
				RSG::camera_attributes->camera_attributes_get_dof_near_enabled(p_render_data->camera_attributes),
				RSG::camera_attributes->camera_attributes_get_dof_near_distance(p_render_data->camera_attributes),
				RSG::camera_attributes->camera_attributes_get_dof_near_transition(p_render_data->camera_attributes),
				RSG::camera_attributes->camera_attributes_get_dof_blur_amount(p_render_data->camera_attributes),
				cam_block.cam_origin_fov[3],
				size.y,
				p_render_data->scene_data->cam_orthogonal);
	}

	RendererSceneRenderRD *scene = RendererSceneRenderRD::get_singleton();
	Color sky;
	RID sky_octmap;
	float sky_radiance_scale = 1.0f;
	float sky_border_pad = 0.0f;
	float sky_border_scale = 1.0f;
	Basis sky_inv_orient;
	bool bg_sky_enabled = false;
	uint64_t sky_state_hash = PATH_TRACER_HASH_BASIS;

	uint32_t fog_mode = 0u;
	float vfog_density = 0.0f;
	float vfog_anisotropy = 0.0f;
	float vfog_length = 64.0f;
	float vfog_sky_affect = 1.0f;
	Color vfog_scattering;
	Color vfog_emission;
	Color fog_legacy_color;
	float fog_legacy_density = 0.0f;
	float fog_legacy_height = 0.0f;
	float fog_legacy_height_density = 0.0f;
	float fog_legacy_depth_begin = 10.0f;
	float fog_legacy_depth_end = 100.0f;
	float fog_legacy_depth_curve = 1.0f;
	float fog_legacy_sky_affect = 1.0f;
	uint32_t fog_legacy_depth_mode = 0u;
	float fog_legacy_sun_scatter = 0.0f;
	LocalVector<PathTracerGPUFogVolume> fog_volumes;
	uint64_t fog_state_hash = PATH_TRACER_HASH_BASIS;

	if (scene != nullptr && p_render_data->environment.is_valid()) {
		const RID env = p_render_data->environment;
		const RSE::EnvironmentBG bg_mode = scene->environment_get_background(env);
		const float bg_energy = scene->environment_get_bg_energy_multiplier(env);
		if (settings.reference_match) {
			sky = scene->environment_get_bg_color(env) * bg_energy;
		} else {
			if (bg_mode == RSE::ENV_BG_COLOR) {
				sky = scene->environment_get_bg_color(env).srgb_to_linear() * bg_energy;
			} else if (bg_mode == RSE::ENV_BG_CLEAR_COLOR) {
				sky = RSG::texture_storage->get_default_clear_color().srgb_to_linear() * bg_energy;
			} else if (bg_mode == RSE::ENV_BG_SKY) {
				const RID sky_rid = scene->environment_get_sky(env);
				SkyRD *sky_rd = scene->get_sky();
				if (sky_rid.is_valid() && sky_rd != nullptr) {
					const RID rad_2d = sky_rd->sky_get_radiance_texture_2d(sky_rid);
					if (rad_2d.is_valid()) {
						sky_octmap = rad_2d;
						bg_sky_enabled = true;
						const float baked_exp = MAX(sky_rd->sky_get_baked_exposure(sky_rid), 1e-4f);
						sky_radiance_scale = 1.0f / MAX(baked_exp * view->exposure, 1e-6f);
						sky_border_pad = sky_rd->sky_get_uv_border_size(sky_rid);
						sky_border_scale = MAX(1.0f - 2.0f * sky_border_pad, 1e-4f);
						sky_inv_orient = scene->environment_get_sky_orientation(env).inverse();
						const RID sky_mat = sky_rd->sky_get_material(sky_rid);
						const uint64_t sky_mat_serial = (sky_mat.is_valid() && MaterialStorage::get_singleton() != nullptr)
								? MaterialStorage::get_singleton()->material_get_change_serial(sky_mat)
								: 0;
						const uint64_t sky_ids[3] = { sky_rid.get_id(), rad_2d.get_id(), sky_mat_serial };
						sky_state_hash = path_tracer_hash_bytes(sky_state_hash, sky_ids, sizeof(sky_ids));
					}
				}
				if (!bg_sky_enabled) {
					sky = scene->environment_get_bg_color(env).srgb_to_linear() * bg_energy;
				}
			} else {
				sky = scene->environment_get_bg_color(env).srgb_to_linear() * bg_energy;
			}

			// Volumetric fog + FogVolumes, or fallback Environment.fog when volumetric fog is off.
			if (scene->environment_get_volumetric_fog_enabled(env)) {
				fog_mode = 1u;
				vfog_density = MAX(scene->environment_get_volumetric_fog_density(env), 0.0f);
				vfog_anisotropy = CLAMP(scene->environment_get_volumetric_fog_anisotropy(env), -0.95f, 0.95f);
				vfog_length = MAX(scene->environment_get_volumetric_fog_length(env), 0.1f);
				vfog_sky_affect = CLAMP(scene->environment_get_volumetric_fog_sky_affect(env), 0.0f, 1.0f);
				vfog_scattering = scene->environment_get_volumetric_fog_scattering(env).srgb_to_linear();
				vfog_emission = scene->environment_get_volumetric_fog_emission(env).srgb_to_linear() * MAX(scene->environment_get_volumetric_fog_emission_energy(env), 0.0f);
				fog_state_hash = _path_tracer_collect_fog_volumes(p_render_data, fog_volumes);
			} else if (scene->environment_get_fog_enabled(env)) {
				fog_mode = 2u;
				fog_legacy_color = scene->environment_get_fog_light_color(env).srgb_to_linear() * MAX(scene->environment_get_fog_light_energy(env), 0.0f);
				fog_legacy_density = MAX(scene->environment_get_fog_density(env), 0.0f);
				fog_legacy_height = scene->environment_get_fog_height(env);
				fog_legacy_height_density = scene->environment_get_fog_height_density(env);
				fog_legacy_depth_begin = scene->environment_get_fog_depth_begin(env);
				fog_legacy_depth_end = scene->environment_get_fog_depth_end(env);
				fog_legacy_depth_curve = scene->environment_get_fog_depth_curve(env);
				fog_legacy_sky_affect = CLAMP(scene->environment_get_fog_sky_affect(env), 0.0f, 1.0f);
				fog_legacy_depth_mode = scene->environment_get_fog_mode(env) == RSE::ENV_FOG_MODE_DEPTH ? 1u : 0u;
				fog_legacy_sun_scatter = scene->environment_get_fog_sun_scatter(env);
			}
		}
		const uint32_t bg_u = uint32_t(bg_mode);
		sky_state_hash = path_tracer_hash_bytes(sky_state_hash, &bg_u, sizeof(bg_u));
		sky_state_hash = path_tracer_hash_bytes(sky_state_hash, &sky, sizeof(sky));
		sky_state_hash = path_tracer_hash_bytes(sky_state_hash, &bg_energy, sizeof(bg_energy));
		sky_state_hash = path_tracer_hash_bytes(sky_state_hash, &sky_radiance_scale, sizeof(sky_radiance_scale));
		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) {
				sky_state_hash = _path_tracer_hash_real(sky_state_hash, sky_inv_orient.rows[i][j]);
			}
		}
		const float fog_scalars[18] = {
			float(fog_mode),
			vfog_density,
			vfog_anisotropy,
			vfog_length,
			vfog_sky_affect,
			vfog_scattering.r,
			vfog_scattering.g,
			vfog_scattering.b,
			vfog_emission.r,
			vfog_emission.g,
			vfog_emission.b,
			fog_legacy_color.r,
			fog_legacy_color.g,
			fog_legacy_color.b,
			fog_legacy_density,
			fog_legacy_height,
			fog_legacy_height_density,
			fog_legacy_depth_end,
		};
		fog_state_hash = path_tracer_hash_bytes(fog_state_hash, fog_scalars, sizeof(fog_scalars));
		const float fog_scalars2[4] = { fog_legacy_depth_begin, fog_legacy_depth_curve, fog_legacy_sky_affect, float(fog_legacy_depth_mode) };
		fog_state_hash = path_tracer_hash_bytes(fog_state_hash, fog_scalars2, sizeof(fog_scalars2));
	}

	const float motion_shutter = settings.reference_match ? 0.0f : CLAMP(float(GLOBAL_GET_CACHED(float, "rendering/global_illumination/nextgi/path_tracing_motion_blur_shutter")), 0.0f, 1.0f);
	const uint32_t moving_instances = settings.reference_match ? 0u : _update_motion_streaks(p_rt_scene, frame, motion_shutter);
	const bool motion_active_expected = moving_instances > 0u;

	// The reset keys are cheap; the tables are built only on frames that trace.
	PathTracerFrameState state;
	state.size = size;
	state.settings = path_tracer_hash_settings(settings, backend, view->exposure);
	uint64_t cam_hash = path_tracer_hash_camera(p_render_data->scene_data->cam_transform, p_render_data->scene_data->cam_projection);
	if (!settings.reference_match) {
		const float dof_words[3] = { dof.enabled ? 1.0f : 0.0f, dof.focus_distance, dof.aperture_radius };
		cam_hash = path_tracer_hash_bytes(cam_hash, dof_words, sizeof(dof_words));
	}
	state.camera = cam_hash;

	uint64_t materials_key = 0;
	uint64_t geometry_key = 0;
	_path_tracer_surface_keys(p_rt_scene, backend == PATH_TRACER_BACKEND_HIT_GROUPS, materials_key, geometry_key);
	const uint64_t records_hash = p_rt_scene->get_records_hash();
	const uint32_t instance_count = p_rt_scene->get_stats().instance_count;
	uint64_t scene_hash = path_tracer_hash_bytes(PATH_TRACER_HASH_BASIS, &records_hash, sizeof(records_hash));
	scene_hash = path_tracer_hash_bytes(scene_hash, &geometry_key, sizeof(geometry_key));
	scene_hash = path_tracer_hash_bytes(scene_hash, &sky_state_hash, sizeof(sky_state_hash));
	scene_hash = path_tracer_hash_bytes(scene_hash, &fog_state_hash, sizeof(fog_state_hash));
	const uint32_t motion_flag_u = motion_active_expected ? 1u : 0u;
	scene_hash = path_tracer_hash_bytes(scene_hash, &motion_flag_u, sizeof(motion_flag_u));
	scene_hash = path_tracer_hash_bytes(scene_hash, &motion_shutter, sizeof(motion_shutter));
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

	const uint32_t reasons = path_tracer_reset_reasons(view->has_state && view->accumulation.is_valid() && view->guide_albedo.is_valid() && view->guide_normal.is_valid(), view->state, state);
	if (reasons != 0) {
		if (!view->accumulation.is_valid() || !view->guide_albedo.is_valid() || !view->guide_normal.is_valid() || view->size != size) {
			const RID old_textures[4] = { view->accumulation, view->guide_albedo, view->guide_normal, view->denoised };
			for (const RID &tex : old_textures) {
				if (tex.is_valid()) {
					rd->free_rid(tex);
				}
			}
			view->denoised = RID();
			RD::TextureFormat format;
			format.width = size.x;
			format.height = size.y;
			format.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
			format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
			view->accumulation = rd->texture_create(format, RD::TextureView());
			view->guide_albedo = rd->texture_create(format, RD::TextureView());
			view->guide_normal = rd->texture_create(format, RD::TextureView());
			view->size = size;
		}
		rd->texture_clear(view->accumulation, Color(0, 0, 0, 0), 0, 1, 0, 1);
		rd->texture_clear(view->guide_albedo, Color(0, 0, 0, 0), 0, 1, 0, 1);
		rd->texture_clear(view->guide_normal, Color(0, 0, 0, 0), 0, 1, 0, 1);
		view->samples = 0;
		view->denoised_valid = false;
		view->denoised_samples = 0;
		view->denoised_requested = RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF;
		view->denoise_backend = RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF;
		view->denoise_status = settings.denoiser == RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF ? PATH_TRACER_DENOISE_STATUS_OFF : PATH_TRACER_DENOISE_STATUS_PENDING;
		view->resets++;
		view->last_reset_reasons = reasons;
		view->last_reset_frame = frame;
		for (uint32_t i = 0; i < PATH_TRACER_RESET_REASON_COUNT; i++) {
			if (reasons & (1u << i)) {
				view->reset_counts[i]++;
			}
		}
	}
	if (settings.denoise_requested) {
		view->pending_denoise_request = true;
	}
	view->state = state;
	view->has_state = true;
	view->backend = backend;
	if (!view->accumulation.is_valid() || !view->guide_albedo.is_valid() || !view->guide_normal.is_valid()) {
		return;
	}
	if (!p_rt_scene->is_tlas_traceable()) {
		view->status = PATH_TRACER_STATUS_NO_TLAS;
		RenderingServerDefault::redraw_request();
		return;
	}
	if (path_tracer_samples_this_frame(view->samples, view->target) == 0) {
		view->status = PATH_TRACER_STATUS_CONVERGED;
		_update_denoise(view);
		_readback(view);
		return;
	}

	PathTracerTables tables;
	build_tables(p_render_data, p_rt_scene, tables, !settings.reference_match);
	view->textured_materials = tables.textured_materials;
	_upload_tables(tables);
	_upload_fog_volumes(fog_volumes);

	// Update sky 2D importance CDF whenever sky or directional sun changes or history resets.
	bool sky_cdf_ready = false;
	if (bg_sky_enabled && sky_octmap.is_valid()) {
		const uint64_t cdf_key = path_tracer_hash_bytes(sky_state_hash, &state.lights, sizeof(state.lights));
		sky_cdf_ready = _update_sky_cdf(sky_octmap, sky_border_pad, sky_border_scale, sky_radiance_scale, cdf_key, view->samples == 0);
	}

	// Build 8-slice shutter-interpolated motion TLAS when any instance is continuously moving.
	uint32_t motion_record_count = 0;
	uint32_t motion_moving_emitted = 0;
	const bool use_motion_tlas = motion_active_expected && _build_motion_tlas(p_rt_scene, frame, view->samples, motion_shutter, motion_record_count, motion_moving_emitted);
	const RID trace_tlas = use_motion_tlas ? motion_tlas : p_rt_scene->get_tlas();
	const RID trace_records = use_motion_tlas ? motion_record_buffer : p_rt_scene->get_instance_record_buffer();
	const uint32_t trace_record_count = use_motion_tlas ? motion_record_count : p_rt_scene->get_stats().instance_count;

	PathTracerGPUParams params;
	params.camera = cam_block;
	params.rays = path_tracer_rays(p_render_data->scene_data->cam_transform, p_render_data->scene_data->cam_projection, p_render_data->scene_data->cam_orthogonal, size);
	params.width = size.x;
	params.height = size.y;
	params.sample_index = view->samples;
	params.seed = 1;
	params.light_count = tables.light_count;
	params.surface_count = tables.surface_count;
	params.record_count = trace_record_count;
	params.sky_color[0] = sky.r;
	params.sky_color[1] = sky.g;
	params.sky_color[2] = sky.b;
	params.sky_color[3] = 1.0f;
	params.exposure = view->exposure;
	params.clear = view->samples == 0 ? 1.0f : 0.0f;
	params.max_bounces = uint32_t(settings.max_bounces);
	params.flags = settings.reference_match ? 0u : (PATH_TRACER_FLAG_CULL | PATH_TRACER_FLAG_JITTER | PATH_TRACER_FLAG_PRODUCT);
	memcpy(&params.firefly_clamp_bits, &settings.firefly_clamp, sizeof(uint32_t));
	params.fog_volume_count = fog_mode == 1u ? fog_volumes.size() : 0u;

	params.sky_params[0] = sky_radiance_scale;
	params.sky_params[1] = sky_border_pad;
	params.sky_params[2] = sky_border_scale;
	params.sky_orient_row0[0] = sky_inv_orient.rows[0][0];
	params.sky_orient_row0[1] = sky_inv_orient.rows[0][1];
	params.sky_orient_row0[2] = sky_inv_orient.rows[0][2];
	params.sky_orient_row0[3] = bg_sky_enabled ? 1.0f : 0.0f;
	params.sky_orient_row1[0] = sky_inv_orient.rows[1][0];
	params.sky_orient_row1[1] = sky_inv_orient.rows[1][1];
	params.sky_orient_row1[2] = sky_inv_orient.rows[1][2];
	params.sky_orient_row1[3] = float(PATH_TRACER_SKY_CDF_DIM);
	params.sky_orient_row2[0] = sky_inv_orient.rows[2][0];
	params.sky_orient_row2[1] = sky_inv_orient.rows[2][1];
	params.sky_orient_row2[2] = sky_inv_orient.rows[2][2];
	params.sky_orient_row2[3] = 1.0f;

	params.vfog_params0[0] = vfog_density;
	params.vfog_params0[1] = vfog_anisotropy;
	params.vfog_params0[2] = vfog_length;
	params.vfog_params0[3] = vfog_sky_affect;
	params.vfog_scattering[0] = vfog_scattering.r;
	params.vfog_scattering[1] = vfog_scattering.g;
	params.vfog_scattering[2] = vfog_scattering.b;
	params.vfog_scattering[3] = vfog_density;
	params.vfog_emission[0] = vfog_emission.r;
	params.vfog_emission[1] = vfog_emission.g;
	params.vfog_emission[2] = vfog_emission.b;
	params.vfog_emission[3] = float(fog_mode);

	params.fog_legacy0[0] = fog_legacy_color.r;
	params.fog_legacy0[1] = fog_legacy_color.g;
	params.fog_legacy0[2] = fog_legacy_color.b;
	params.fog_legacy0[3] = fog_legacy_density;
	params.fog_legacy1[0] = fog_legacy_height;
	params.fog_legacy1[1] = fog_legacy_height_density;
	params.fog_legacy1[2] = fog_legacy_depth_begin;
	params.fog_legacy1[3] = fog_legacy_depth_end;
	params.fog_legacy2[0] = fog_legacy_depth_curve;
	params.fog_legacy2[1] = fog_legacy_sky_affect;
	params.fog_legacy2[2] = float(fog_legacy_depth_mode);
	params.fog_legacy2[3] = fog_legacy_sun_scatter;

	params.dof_params[0] = dof.focus_distance;
	params.dof_params[1] = dof.aperture_radius;
	params.dof_params[2] = dof.near_plane_dist;
	params.dof_params[3] = dof.enabled ? 1.0f : 0.0f;

	params.motion_params[0] = use_motion_tlas ? PATH_TRACER_MOTION_SLICES : 0u;
	memcpy(&params.motion_params[1], &motion_shutter, sizeof(uint32_t));
	params.motion_params[2] = sky_cdf_ready ? 1u : 0u;
	params.motion_params[3] = 0u;

	rd->buffer_update(shader->params_buffer, 0, sizeof(params), &params);

	view->light_count = tables.light_count;
	view->fog_volume_count = params.fog_volume_count;
	view->sky_importance_active = sky_cdf_ready;
	view->volumetric_fog_active = fog_mode == 1u;
	view->dof_active = dof.enabled;
	view->motion_blur_active = use_motion_tlas;
	view->motion_moving_instances = motion_moving_emitted;

	if (!dummy_buffer.is_valid()) {
		dummy_buffer = rd->storage_buffer_create(256);
	}
	TextureStorage *texture_storage = TextureStorage::get_singleton();
	LightStorage *light_storage = LightStorage::get_singleton();
	const RID black_2d = texture_storage != nullptr ? texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_BLACK) : RID();
	const RID white_2d_array = texture_storage != nullptr ? texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE) : RID();
	const RID ies_atlas = (light_storage != nullptr && light_storage->get_ies_atlas_texture().is_valid()) ? light_storage->get_ies_atlas_texture() : white_2d_array;
	const RID area_atlas = (texture_storage != nullptr && texture_storage->area_light_atlas_get_texture().is_valid()) ? texture_storage->area_light_atlas_get_texture() : black_2d;
	const RID projector_atlas = (texture_storage != nullptr && texture_storage->decal_atlas_get_texture_srgb().is_valid()) ? texture_storage->decal_atlas_get_texture_srgb() : black_2d;
	const RID bound_sky_octmap = sky_octmap.is_valid() ? sky_octmap : black_2d;

	bool traced = false;
	if (backend == PATH_TRACER_BACKEND_HIT_GROUPS) {
		NextGIHitPathTrace trace;
		trace.accumulation = view->accumulation;
		trace.guide_albedo = view->guide_albedo;
		trace.guide_normal = view->guide_normal;
		trace.params = shader->params_buffer;
		trace.lights = light_buffer;
		trace.materials = material_buffer;
		trace.sky_cdf = sky_cdf_buffer.is_valid() ? sky_cdf_buffer : dummy_buffer;
		trace.fog_volumes = fog_volume_buffer.is_valid() ? fog_volume_buffer : dummy_buffer;
		trace.ies_atlas = ies_atlas;
		trace.area_atlas = area_atlas;
		trace.projector_atlas = projector_atlas;
		trace.sky_octmap = bound_sky_octmap;
		if (use_motion_tlas) {
			trace.tlas_override = trace_tlas;
			trace.records_override = trace_records;
			trace.record_count_override = trace_record_count;
		}
		trace.width = size.x;
		trace.height = size.y;
		trace.time = float(p_render_data->scene_data->time);
		traced = p_hit_materials->trace_path_tracer(p_rt_scene, trace);
	}
	if (!traced) {
		_trace_inline(view, p_rt_scene, trace_tlas, trace_records, bound_sky_octmap, ies_atlas, area_atlas, projector_atlas);
		if (backend == PATH_TRACER_BACKEND_HIT_GROUPS) {
			view->backend = PATH_TRACER_BACKEND_INLINE;
		}
	}
	view->samples++;
	view->frames_traced++;
	view->last_traced_frame = frame;
	view->status = view->samples >= view->target ? PATH_TRACER_STATUS_CONVERGED : PATH_TRACER_STATUS_ACTIVE;
	_update_denoise(view);
	if (view->status == PATH_TRACER_STATUS_ACTIVE) {
		RenderingServerDefault::redraw_request();
	}
	_readback(view);
}

static void _path_tracer_summarize_rgba32f(const Vector<uint8_t> &p_data, int64_t p_pixels, float &r_mean, uint32_t &r_nonfinite, uint64_t &r_hash) {
	const float *texels = reinterpret_cast<const float *>(p_data.ptr());
	double sum = 0.0;
	int64_t counted = 0;
	uint32_t nonfinite = 0;
	for (int64_t i = 0; i < p_pixels * 4; i++) {
		if (!Math::is_finite(texels[i])) {
			nonfinite++;
		}
	}
	for (int64_t i = 0; i < p_pixels; i++) {
		const float weight = texels[i * 4 + 3];
		if (Math::is_finite(weight) && weight > 0.0f) {
			const float r = texels[i * 4 + 0];
			const float g = texels[i * 4 + 1];
			const float b = texels[i * 4 + 2];
			if (Math::is_finite(r) && Math::is_finite(g) && Math::is_finite(b)) {
				sum += double(r + g + b) / (3.0 * double(weight));
				counted++;
			}
		}
	}
	r_mean = counted > 0 ? float(sum / double(counted)) : 0.0f;
	r_nonfinite = nonfinite;
	r_hash = path_tracer_hash_bytes(PATH_TRACER_HASH_BASIS, p_data.ptr(), p_data.size());
}

void PathTracer::_update_denoise(PathTracerView *p_view) {
	if (p_view == nullptr) {
		return;
	}
	const RSE::ViewportPathTracingDenoiser requested = p_view->settings.denoiser;
	if (requested == RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF) {
		p_view->denoised_valid = false;
		p_view->pending_denoise_request = false;
		p_view->denoise_backend = RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF;
		p_view->denoise_status = PATH_TRACER_DENOISE_STATUS_OFF;
		return;
	}
	const bool should_run = path_tracer_should_denoise(requested, p_view->samples, p_view->target, p_view->pending_denoise_request, p_view->denoised_valid, p_view->denoised_samples, p_view->denoised_requested);
	if (!should_run) {
		if (!(p_view->denoised_valid && p_view->denoised_samples == p_view->samples && p_view->denoised_requested == requested)) {
			p_view->denoise_status = PATH_TRACER_DENOISE_STATUS_PENDING;
		}
		return;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL(rd);
	if (!p_view->denoised.is_valid()) {
		RD::TextureFormat format;
		format.width = p_view->size.x;
		format.height = p_view->size.y;
		format.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
		format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		p_view->denoised = rd->texture_create(format, RD::TextureView());
	}
	if (!p_view->denoised.is_valid()) {
		p_view->denoised_valid = false;
		p_view->pending_denoise_request = false;
		p_view->denoise_status = PATH_TRACER_DENOISE_STATUS_PENDING;
		return;
	}

	p_view->pending_denoise_request = false;
	if (requested == RSE::VIEWPORT_PATH_TRACING_DENOISER_OIDN) {
		if (_denoise_oidn(p_view)) {
			p_view->denoised_valid = true;
			p_view->denoised_samples = p_view->samples;
			p_view->denoised_requested = requested;
			p_view->denoise_backend = RSE::VIEWPORT_PATH_TRACING_DENOISER_OIDN;
			p_view->denoise_status = PATH_TRACER_DENOISE_STATUS_DENOISED;
			p_view->denoise_runs++;
			return;
		}
		if (_denoise_builtin(p_view)) {
			p_view->denoised_valid = true;
			p_view->denoised_samples = p_view->samples;
			p_view->denoised_requested = requested;
			p_view->denoise_backend = RSE::VIEWPORT_PATH_TRACING_DENOISER_BUILTIN;
			p_view->denoise_status = PATH_TRACER_DENOISE_STATUS_FALLBACK_BUILTIN;
			p_view->denoise_runs++;
			return;
		}
		p_view->denoised_valid = false;
		p_view->denoise_status = PATH_TRACER_DENOISE_STATUS_PENDING;
		return;
	}

	if (_denoise_builtin(p_view)) {
		p_view->denoised_valid = true;
		p_view->denoised_samples = p_view->samples;
		p_view->denoised_requested = requested;
		p_view->denoise_backend = RSE::VIEWPORT_PATH_TRACING_DENOISER_BUILTIN;
		p_view->denoise_status = PATH_TRACER_DENOISE_STATUS_DENOISED;
		p_view->denoise_runs++;
	} else {
		p_view->denoised_valid = false;
		p_view->denoise_status = PATH_TRACER_DENOISE_STATUS_PENDING;
	}
}

bool PathTracer::_denoise_builtin(PathTracerView *p_view) {
	if (!_ensure_denoise_shader() || p_view == nullptr || !p_view->denoised.is_valid()) {
		return false;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	RD::Uniform uniforms[4] = {
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 0, p_view->accumulation),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, p_view->guide_albedo),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, p_view->guide_normal),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, p_view->denoised),
	};
	const RID uniform_set = rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 4), shader->denoise_shader.version_get_shader(shader->denoise_version, 0), 0);
	ERR_FAIL_COND_V(!uniform_set.is_valid(), false);

	const PathTracerDenoiseParams params = path_tracer_validate_denoise_params(PathTracerDenoiseParams());
	PathTracerDenoisePush push;
	push.width = uint32_t(p_view->size.x);
	push.height = uint32_t(p_view->size.y);
	push.exposure = p_view->exposure > 0.0f ? p_view->exposure : 1.0f;
	push.spatial_bandwidth = params.spatial_bandwidth;
	push.light_bandwidth = params.light_bandwidth;
	push.albedo_bandwidth = params.albedo_bandwidth;
	push.normal_bandwidth = params.normal_bandwidth;
	push.filter_strength = params.filter_strength;
	push.half_search_window = params.half_search_window;

	rd->draw_command_begin_label("Path Tracer Denoise (JNLM)");
	const RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, shader->denoise_pipeline);
	rd->compute_list_bind_uniform_set(list, uniform_set, 0);
	rd->compute_list_set_push_constant(list, &push, sizeof(push));
	rd->compute_list_dispatch(list, (p_view->size.x + 7) / 8, (p_view->size.y + 7) / 8, 1);
	rd->compute_list_end();
	rd->draw_command_end_label();
	rd->free_rid(uniform_set);
	return true;
}

bool PathTracer::_denoise_oidn(PathTracerView *p_view) {
	if (p_view == nullptr || !p_view->denoised.is_valid() || p_view->size.x <= 0 || p_view->size.y <= 0) {
		return false;
	}
	if (!is_oidn_available()) {
		if (!logged_oidn_fallback) {
			WARN_PRINT("PathTracer: OIDN denoiser requested, but modules/oidn is not enabled in this build; falling back to the built-in screen-space denoiser.");
			logged_oidn_fallback = true;
		}
		return false;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	const uint32_t width = uint32_t(p_view->size.x);
	const uint32_t height = uint32_t(p_view->size.y);
	const int64_t expected_bytes = int64_t(width) * int64_t(height) * 16;

	const Vector<uint8_t> accum_bytes = rd->texture_get_data(p_view->accumulation, 0);
	const Vector<uint8_t> albedo_bytes = rd->texture_get_data(p_view->guide_albedo, 0);
	const Vector<uint8_t> normal_bytes = rd->texture_get_data(p_view->guide_normal, 0);
	if (accum_bytes.size() < expected_bytes || albedo_bytes.size() < expected_bytes || normal_bytes.size() < expected_bytes) {
		return false;
	}

	LocalVector<float> color_rgb;
	LocalVector<float> albedo_rgb;
	LocalVector<float> normal_rgb;
	if (!path_tracer_prepare_oidn_buffers(
				width, height,
				reinterpret_cast<const float *>(accum_bytes.ptr()),
				reinterpret_cast<const float *>(albedo_bytes.ptr()),
				reinterpret_cast<const float *>(normal_bytes.ptr()),
				color_rgb, albedo_rgb, normal_rgb)) {
		return false;
	}

	LocalVector<float> output_rgb;
	output_rgb.resize(color_rgb.size());
	PathTracerOIDNInput input;
	input.width = width;
	input.height = height;
	input.color = color_rgb.ptr();
	input.albedo = albedo_rgb.ptr();
	input.normal = normal_rgb.ptr();
	input.output = output_rgb.ptr();
	input.hdr = true;
	input.srgb = false;
	input.clean_aux = true;

	String error_msg;
	const Error err = run_oidn_denoise(input, error_msg);
	if (err != OK) {
		if (!logged_oidn_fallback) {
			WARN_PRINT(vformat("PathTracer: OIDN denoiser failed (%s); falling back to the built-in screen-space denoiser.", error_msg));
			logged_oidn_fallback = true;
		}
		return false;
	}

	Vector<uint8_t> rgba_bytes;
	path_tracer_pack_denoised_rgba(width, height, output_rgb.ptr(), rgba_bytes);
	if (rgba_bytes.size() < expected_bytes) {
		return false;
	}
	return rd->texture_update(p_view->denoised, 0, rgba_bytes) == OK;
}

void PathTracer::_readback(PathTracerView *p_view) {
	if (!bool(GLOBAL_GET_CACHED(bool, "rendering/global_illumination/nextgi/debug/path_tracer_readback"))) {
		p_view->read_accumulation.clear();
		p_view->read_denoised.clear();
		return;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	const Vector<uint8_t> data = rd->texture_get_data(p_view->accumulation, 0);
	const int64_t pixels = int64_t(p_view->size.x) * int64_t(p_view->size.y);
	if (data.size() < pixels * 16) {
		p_view->read_accumulation.clear();
		p_view->read_denoised.clear();
		return;
	}
	p_view->read_accumulation = data;
	_path_tracer_summarize_rgba32f(data, pixels, p_view->read_mean, p_view->read_nonfinite, p_view->read_hash);

	if (p_view->denoised_valid && p_view->denoised.is_valid() && p_view->denoised_samples == p_view->samples) {
		const Vector<uint8_t> denoised_data = rd->texture_get_data(p_view->denoised, 0);
		if (denoised_data.size() >= pixels * 16) {
			p_view->read_denoised = denoised_data;
			_path_tracer_summarize_rgba32f(denoised_data, pixels, p_view->read_denoised_mean, p_view->read_denoised_nonfinite, p_view->read_denoised_hash);
		} else {
			p_view->read_denoised.clear();
		}
	} else {
		p_view->read_denoised.clear();
	}
}

void PathTracer::_trace_inline(PathTracerView *p_view, NextGIRTScene *p_rt_scene, RID p_tlas, RID p_records, RID p_sky_octmap, RID p_ies_atlas, RID p_area_atlas, RID p_projector_atlas) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	RID chunks[4] = { dummy_buffer, dummy_buffer, dummy_buffer, dummy_buffer };
	if (mesh_storage != nullptr) {
		for (uint32_t i = 0; i < MIN(mesh_storage->mesh_nextgi_rt_get_chunk_count(), 4u); i++) {
			const RID chunk = mesh_storage->mesh_nextgi_rt_get_chunk_buffer(i);
			if (chunk.is_valid()) {
				chunks[i] = chunk;
			}
		}
	}
	const RID surfaces = p_rt_scene->get_surface_record_buffer();
	const RID linear_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RD::Uniform uniforms[20] = {
		RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, p_tlas),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, p_view->accumulation),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, material_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, light_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, p_records.is_valid() ? p_records : dummy_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, surfaces.is_valid() ? surfaces : dummy_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, chunks[0]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, chunks[1]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, chunks[2]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, chunks[3]),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, shader->params_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 12, p_view->guide_albedo),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 13, p_view->guide_normal),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, sky_cdf_buffer.is_valid() ? sky_cdf_buffer : dummy_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, fog_volume_buffer.is_valid() ? fog_volume_buffer : dummy_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 16, p_ies_atlas),
		RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 17, p_area_atlas),
		RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 18, p_projector_atlas),
		RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 19, p_sky_octmap),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 20, linear_sampler),
	};
	const RID uniform_set = rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 20), shader->shader.version_get_shader(shader->version, 0), 0);
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
	const bool use_denoised = view->settings.denoiser != RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF && view->denoised_valid && view->denoised.is_valid() && view->denoised_samples == view->samples;
	const RID source = use_denoised ? view->denoised : view->accumulation;
	RD::Uniform uniforms[2] = {
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, source),
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
		if (shader->sky_cdf_version.is_valid()) {
			shader->sky_cdf_shader.version_free(shader->sky_cdf_version);
		}
		if (shader->denoise_version.is_valid()) {
			shader->denoise_shader.version_free(shader->denoise_version);
		}
		if (shader->version.is_valid()) {
			shader->shader.version_free(shader->version);
		}
		memdelete(shader);
		shader = nullptr;
	}
	if (rd != nullptr) {
		const RID resources[7] = { motion_tlas, dummy_buffer, material_buffer, light_buffer, sky_cdf_buffer, fog_volume_buffer, motion_record_buffer };
		for (const RID &res : resources) {
			if (res.is_valid()) {
				rd->free_rid(res);
			}
		}
	}
	motion_tlas = RID();
	dummy_buffer = RID();
	material_buffer = RID();
	light_buffer = RID();
	sky_cdf_buffer = RID();
	fog_volume_buffer = RID();
	motion_record_buffer = RID();
	material_capacity = 0;
	light_capacity = 0;
	fog_volume_capacity = 0;
	motion_record_capacity = 0;
	sky_cdf_key = 0;
	sky_cdf_valid = false;
	motion_streaks.clear();
	motion_streaks_frame = UINT64_MAX;
}

PathTracer::~PathTracer() {
	free();
}
