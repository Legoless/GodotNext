/**************************************************************************/
/*  next_gi_hit_lighting.cpp                                              */
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

#include "next_gi_hit_lighting.h"

#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_relight.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_scene.h"
#include "servers/rendering/renderer_rd/environment/next_gi_src.h"
#include "servers/rendering/renderer_rd/environment/next_gi_src_relight.h"
#include "servers/rendering/renderer_rd/environment/next_gi_view.h"
#include "servers/rendering/renderer_rd/environment/next_gi_wrc.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_hit_sort.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_light_grid.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_reflect.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_globals.h"

#include <cstring>

namespace RendererRD {

// GodotNext: nextgi_hit_lighting (#129, plan §21.2 contract L7)

NextGIRayLightingMode nextgi_ray_lighting_mode_from_setting(int p_setting) {
	if (p_setting == int(NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS)) {
		return NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS;
	}
	return NEXTGI_RAY_LIGHTING_SURFACE_CACHE;
}

const char *nextgi_ray_lighting_mode_name(NextGIRayLightingMode p_mode) {
	switch (p_mode) {
		case NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS:
			return "hit_lighting_reflections";
		case NEXTGI_RAY_LIGHTING_SURFACE_CACHE:
		default:
			return "surface_cache";
	}
}

NextGIRayLightingMode nextgi_resolve_ray_lighting_mode(int p_project_setting, int p_env_override) {
	if (p_env_override == int(NEXTGI_ENV_RAY_LIGHTING_SURFACE_CACHE)) {
		return NEXTGI_RAY_LIGHTING_SURFACE_CACHE;
	}
	if (p_env_override == int(NEXTGI_ENV_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS)) {
		return NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS;
	}
	return nextgi_ray_lighting_mode_from_setting(p_project_setting);
}

uint32_t nextgi_clamp_max_reflection_bounces(int p_setting) {
	return uint32_t(CLAMP(p_setting, int(NEXTGI_HIT_LIGHT_MIN_BOUNCES), int(NEXTGI_HIT_LIGHT_MAX_BOUNCES)));
}

NextGIHitLightingVerdict nextgi_hit_lighting_evaluate(
		NextGIRayLightingMode p_requested,
		bool p_hwrt_tier,
		bool p_raytracing_pipeline,
		bool p_pipeline_library,
		bool p_bindless,
		NextGIHitMode p_hit_mode,
		bool p_raygen_ready) {
	NextGIHitLightingVerdict verdict;
	verdict.requested_mode = p_requested;
	if (p_requested == NEXTGI_RAY_LIGHTING_SURFACE_CACHE) {
		verdict.effective_mode = NEXTGI_RAY_LIGHTING_SURFACE_CACHE;
		verdict.active = false;
		verdict.unsupported_fallback = false;
		verdict.reason = "SURFACE_CACHE";
		return verdict;
	}
	const bool supported = p_hwrt_tier && p_raytracing_pipeline && p_pipeline_library && p_bindless &&
			nextgi_hit_mode_material_groups(p_hit_mode) && p_raygen_ready;
	if (supported) {
		verdict.effective_mode = NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS;
		verdict.active = true;
		verdict.unsupported_fallback = false;
		verdict.reason = "ACTIVE";
	} else {
		verdict.effective_mode = NEXTGI_RAY_LIGHTING_SURFACE_CACHE;
		verdict.active = false;
		verdict.unsupported_fallback = true;
		verdict.reason = "HIT_LIGHTING_UNSUPPORTED";
	}
	return verdict;
}

NextGIHitLightingGIVerdict nextgi_hit_lighting_gi_evaluate(
		bool p_hit_lighting_gi_setting,
		const NextGIHitLightingVerdict &p_reflection_verdict,
		bool p_epic_quality) {
	NextGIHitLightingGIVerdict verdict;
	verdict.requested = p_hit_lighting_gi_setting;
	if (!p_hit_lighting_gi_setting) {
		verdict.active = false;
		verdict.reason = "OFF";
		return verdict;
	}
	if (p_reflection_verdict.requested_mode != NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS) {
		verdict.active = false;
		verdict.reason = "RAY_LIGHTING_MODE_SURFACE_CACHE";
		return verdict;
	}
	if (!p_reflection_verdict.active) {
		verdict.active = false;
		verdict.reason = p_reflection_verdict.reason;
		return verdict;
	}
	if (!p_epic_quality) {
		verdict.active = false;
		verdict.reason = "QUALITY_NOT_EPIC";
		return verdict;
	}
	verdict.active = true;
	verdict.reason = "ACTIVE";
	return verdict;
}

uint16_t nextgi_hit_light_material_key(uint32_t p_shader_slot, uint32_t p_material_id) {
	if (p_material_id == UINT32_MAX || p_shader_slot == UINT32_MAX) {
		return uint16_t(NEXTGI_HL_KEY_REFLECTION_FALLBACK);
	}
	const uint16_t key = uint16_t(((p_shader_slot & 0x7Fu) << 8u) | (p_material_id & 0xFFu));
	return key == uint16_t(NEXTGI_HL_KEY_REFLECTION_FALLBACK) ? uint16_t(NEXTGI_HL_KEY_REFLECTION_FALLBACK - 1u) : key;
}

uint16_t nextgi_hit_light_gi_material_key(uint32_t p_shader_slot, uint32_t p_material_id) {
	if (p_material_id == UINT32_MAX || p_shader_slot == UINT32_MAX) {
		return uint16_t(NEXTGI_HL_KEY_GI_FALLBACK);
	}
	const uint16_t key = uint16_t(NEXTGI_HL_KEY_GI_BASE | ((p_shader_slot & 0x7Fu) << 8u) | (p_material_id & 0xFFu));
	return key == uint16_t(NEXTGI_HL_KEY_GI_FALLBACK) ? uint16_t(NEXTGI_HL_KEY_GI_FALLBACK - 1u) : key;
}

static Vector2 _oct_encode_hl(const Vector3 &p_dir) {
	Vector3 n = p_dir.normalized();
	const float l1 = Math::abs(n.x) + Math::abs(n.y) + Math::abs(n.z);
	if (l1 > 1e-6f) {
		n /= l1;
	}
	Vector2 e(n.x, n.y);
	if (n.z < 0.0f) {
		e = Vector2(
				(1.0f - Math::abs(n.y)) * (n.x >= 0.0f ? 1.0f : -1.0f),
				(1.0f - Math::abs(n.x)) * (n.y >= 0.0f ? 1.0f : -1.0f));
	}
	return e;
}

static Vector3 _oct_decode_hl(const Vector2 &p_e) {
	Vector3 v(p_e.x, p_e.y, 1.0f - Math::abs(p_e.x) - Math::abs(p_e.y));
	const float t = MAX(-v.z, 0.0f);
	v.x += t * (v.x >= 0.0f ? -1.0f : 1.0f);
	v.y += t * (v.y >= 0.0f ? -1.0f : 1.0f);
	return v.normalized();
}

static uint32_t _pack_half2x16(const Vector2 &p_v) {
	const uint32_t lo = Math::make_half_float(p_v.x);
	const uint32_t hi = Math::make_half_float(p_v.y);
	return (lo & 0xFFFFu) | ((hi & 0xFFFFu) << 16u);
}

static Vector2 _unpack_half2x16(uint32_t p_packed) {
	return Vector2(
			Math::half_to_float(uint16_t(p_packed & 0xFFFFu)),
			Math::half_to_float(uint16_t((p_packed >> 16u) & 0xFFFFu)));
}

static uint32_t _pack_snorm2x16(const Vector2 &p_v) {
	const uint16_t lo = uint16_t(nextgi_hit_snorm16(p_v.x));
	const uint16_t hi = uint16_t(nextgi_hit_snorm16(p_v.y));
	return uint32_t(lo) | (uint32_t(hi) << 16u);
}

static Vector2 _unpack_snorm2x16(uint32_t p_packed) {
	const int16_t lo = int16_t(p_packed & 0xFFFFu);
	const int16_t hi = int16_t((p_packed >> 16u) & 0xFFFFu);
	return Vector2(nextgi_hit_snorm16_to_float(lo), nextgi_hit_snorm16_to_float(hi));
}

NextGIHitLightRecord nextgi_hit_light_pack_record(const NextGIHitLightRecordUnpacked &p_in) {
	NextGIHitLightRecord rec;
	rec.origin[0] = p_in.origin.x;
	rec.origin[1] = p_in.origin.y;
	rec.origin[2] = p_in.origin.z;
	rec.hit_t = p_in.hit_t;
	rec.instance_flags = (p_in.instance_index & 0xFFFFFFu) | (p_in.flags & 0xFF000000u);
	rec.geometry_material = uint32_t(p_in.geometry_index) | (uint32_t(p_in.material_key) << 16u);
	rec.primitive = p_in.primitive;
	rec.pixel_packed = (uint32_t(MAX(p_in.pixel.x, 0)) & 0xFFFFu) | ((uint32_t(MAX(p_in.pixel.y, 0)) & 0xFFFFu) << 16u);
	rec.bary_f16 = _pack_half2x16(p_in.bary);
	rec.ray_dir_oct = _pack_snorm2x16(_oct_encode_hl(p_in.ray_dir));
	rec.cone_f16 = _pack_half2x16(p_in.cone);
	memcpy(&rec.resume_bits, &p_in.resume_distance, sizeof(float));
	return rec;
}

NextGIHitLightRecordUnpacked nextgi_hit_light_unpack_record(const NextGIHitLightRecord &p_rec) {
	NextGIHitLightRecordUnpacked out;
	out.origin = Vector3(p_rec.origin[0], p_rec.origin[1], p_rec.origin[2]);
	out.hit_t = p_rec.hit_t;
	out.instance_index = p_rec.instance_flags & 0xFFFFFFu;
	out.flags = p_rec.instance_flags & 0xFF000000u;
	out.geometry_index = uint16_t(p_rec.geometry_material & 0xFFFFu);
	out.material_key = uint16_t(p_rec.geometry_material >> 16u);
	out.primitive = p_rec.primitive;
	out.pixel = Vector2i(int(p_rec.pixel_packed & 0xFFFFu), int(p_rec.pixel_packed >> 16u));
	out.bary = _unpack_half2x16(p_rec.bary_f16);
	out.ray_dir = _oct_decode_hl(_unpack_snorm2x16(p_rec.ray_dir_oct));
	out.cone = _unpack_half2x16(p_rec.cone_f16);
	memcpy(&out.resume_distance, &p_rec.resume_bits, sizeof(float));
	return out;
}

void nextgi_hit_light_counting_sort(const NextGIHitLightRecord *p_records, uint32_t p_count, LocalVector<uint32_t> &r_sorted_indices) {
	r_sorted_indices.clear();
	if (p_records == nullptr || p_count == 0) {
		return;
	}
	LocalVector<uint32_t> histogram;
	histogram.resize_initialized(NEXTGI_HIT_LIGHT_SORT_BINS);
	for (uint32_t i = 0; i < p_count; i++) {
		const uint32_t key = p_records[i].geometry_material >> 16u;
		histogram[key]++;
	}
	uint32_t running = 0;
	for (uint32_t k = 0; k < NEXTGI_HIT_LIGHT_SORT_BINS; k++) {
		const uint32_t c = histogram[k];
		histogram[k] = running;
		running += c;
	}
	r_sorted_indices.resize(p_count);
	for (uint32_t i = 0; i < p_count; i++) {
		const uint32_t key = p_records[i].geometry_material >> 16u;
		const uint32_t dst = histogram[key]++;
		r_sorted_indices[dst] = i;
	}
}

uint32_t nextgi_light_grid_world_to_cell(const Vector3 &p_world, const Vector3 &p_grid_min, float p_inv_cell_size) {
	const Vector3 rel = (p_world - p_grid_min) * p_inv_cell_size;
	const int cx = int(Math::floor(rel.x));
	const int cy = int(Math::floor(rel.y));
	const int cz = int(Math::floor(rel.z));
	if (cx < 0 || cy < 0 || cz < 0 || cx >= NEXTGI_LIGHT_GRID_DIM || cy >= NEXTGI_LIGHT_GRID_DIM || cz >= NEXTGI_LIGHT_GRID_DIM) {
		return UINT32_MAX;
	}
	return uint32_t(cx) + uint32_t(cy) * NEXTGI_LIGHT_GRID_DIM + uint32_t(cz) * NEXTGI_LIGHT_GRID_DIM * NEXTGI_LIGHT_GRID_DIM;
}

static float _cell_light_priority_cpu(const NextGIHitLightGPU &p_light, const Vector3 &p_box_min, const Vector3 &p_box_max, bool &r_intersects) {
	const uint32_t ltype = uint32_t(p_light.direction_type[3] + 0.5f);
	const float e = p_light.color_energy[3];
	const float base_energy = MAX(p_light.color_energy[0] * e, MAX(p_light.color_energy[1] * e, p_light.color_energy[2] * e));
	if (!(base_energy > 0.0f)) {
		r_intersects = false;
		return 0.0f;
	}
	if (ltype == 0u) {
		r_intersects = true;
		return 1e12f + base_energy;
	}
	const float inv_range = p_light.position_inv_range[3];
	if (!(inv_range > 0.0f)) {
		r_intersects = false;
		return 0.0f;
	}
	const float range = 1.0f / inv_range;
	const Vector3 pos(p_light.position_inv_range[0], p_light.position_inv_range[1], p_light.position_inv_range[2]);
	const Vector3 closest(
			CLAMP(pos.x, p_box_min.x, p_box_max.x),
			CLAMP(pos.y, p_box_min.y, p_box_max.y),
			CLAMP(pos.z, p_box_min.z, p_box_max.z));
	const float dist_sq = (closest - pos).length_squared();
	if (dist_sq > range * range) {
		r_intersects = false;
		return 0.0f;
	}
	const float dist = Math::sqrt(dist_sq);
	if (ltype == 2u) {
		const Vector3 box_center = (p_box_min + p_box_max) * 0.5f;
		const float box_radius = (p_box_max - box_center).length();
		const Vector3 to_center = box_center - pos;
		const float center_dist = to_center.length();
		const Vector3 dir(p_light.direction_type[0], p_light.direction_type[1], p_light.direction_type[2]);
		if (center_dist > box_radius + 1e-4f) {
			const float cos_expand = p_light.spot_atten_radius[1];
			const float sin_half = Math::sqrt(MAX(1.0f - cos_expand * cos_expand, 0.0f));
			const float axial = to_center.dot(dir);
			if (axial < -box_radius) {
				r_intersects = false;
				return 0.0f;
			}
			const float perp_sq = MAX(center_dist * center_dist - axial * axial, 0.0f);
			const float cone_r = MAX(axial, 0.0f) * (sin_half / MAX(cos_expand, 1e-4f)) + box_radius / MAX(cos_expand, 1e-4f);
			if (cos_expand > 0.0f && perp_sq > cone_r * cone_r) {
				r_intersects = false;
				return 0.0f;
			}
		}
	}
	r_intersects = true;
	const float atten = nextgi_omni_attenuation(MAX(dist, 0.1f), inv_range, p_light.spot_atten_radius[0]);
	const float shadow_bias = (p_light.shadow_flags[0] & NEXTGI_HL_LIGHT_FLAG_SHADOW) != 0u ? 1.001f : 1.0f;
	return base_energy * MAX(atten, 1e-6f) * shadow_bias;
}

uint32_t nextgi_light_grid_fill_cell_cpu(const NextGIHitLightGPU *p_lights, uint32_t p_light_count, const Vector3 &p_box_min, const Vector3 &p_box_max, uint16_t r_indices[NEXTGI_LIGHT_GRID_CELL_CAP], uint32_t &r_overflow) {
	r_overflow = 0;
	for (uint32_t i = 0; i < NEXTGI_LIGHT_GRID_CELL_CAP; i++) {
		r_indices[i] = 0xFFFFu;
	}
	if (p_lights == nullptr || p_light_count == 0) {
		return 0;
	}
	float kept_prio[NEXTGI_LIGHT_GRID_CELL_CAP] = {};
	uint32_t count = 0;
	const uint32_t limit = MIN(p_light_count, 65535u);
	for (uint32_t i = 0; i < limit; i++) {
		bool intersects = false;
		const float prio = _cell_light_priority_cpu(p_lights[i], p_box_min, p_box_max, intersects);
		if (!intersects) {
			continue;
		}
		if (count < NEXTGI_LIGHT_GRID_CELL_CAP) {
			uint32_t pos = count;
			while (pos > 0 && prio > kept_prio[pos - 1]) {
				r_indices[pos] = r_indices[pos - 1];
				kept_prio[pos] = kept_prio[pos - 1];
				pos--;
			}
			r_indices[pos] = uint16_t(i);
			kept_prio[pos] = prio;
			count++;
		} else {
			r_overflow++;
			if (prio > kept_prio[NEXTGI_LIGHT_GRID_CELL_CAP - 1]) {
				uint32_t pos = NEXTGI_LIGHT_GRID_CELL_CAP - 1;
				while (pos > 0 && prio > kept_prio[pos - 1]) {
					r_indices[pos] = r_indices[pos - 1];
					kept_prio[pos] = kept_prio[pos - 1];
					pos--;
				}
				r_indices[pos] = uint16_t(i);
				kept_prio[pos] = prio;
			}
		}
	}
	return count;
}

bool nextgi_hit_light_should_bounce(float p_roughness, uint32_t p_bounce_index, uint32_t p_max_bounces) {
	const uint32_t cap = nextgi_clamp_max_reflection_bounces(int(p_max_bounces));
	return p_roughness < NEXTGI_HIT_LIGHT_MIRROR_ROUGHNESS && (p_bounce_index + 1u) < cap;
}

Vector3 nextgi_hit_light_translucent_transmittance(const Color &p_albedo, float p_alpha) {
	const float a = CLAMP(p_alpha, 0.0f, 1.0f);
	const Vector3 c(CLAMP(p_albedo.r, 0.0f, 1.0f), CLAMP(p_albedo.g, 0.0f, 1.0f), CLAMP(p_albedo.b, 0.0f, 1.0f));
	const Vector3 tint = Vector3(1.0f, 1.0f, 1.0f).lerp(c, a);
	return tint * (1.0f - a);
}

bool nextgi_hit_light_accumulate_translucent(const Color &p_albedo, float p_alpha, const Vector3 &p_layer_radiance, Vector3 &r_accum_radiance, Vector3 &r_throughput, uint32_t &r_skips, uint32_t p_max_skips) {
	if (r_skips >= p_max_skips) {
		return false;
	}
	const float a = CLAMP(p_alpha, 0.0f, 1.0f);
	r_accum_radiance += r_throughput * p_layer_radiance * a;
	r_throughput *= nextgi_hit_light_translucent_transmittance(p_albedo, a);
	r_skips++;
	return MAX(r_throughput.x, MAX(r_throughput.y, r_throughput.z)) >= 1e-3f;
}

/* NextGIHitLighting GPU manager */

static Mutex nextgi_hl_counter_mutex;
static HashMap<uint64_t, NextGIHitLighting *> nextgi_hl_counter_registry;
static uint64_t nextgi_hl_counter_next_token = 1;

enum {
	SORT_VARIANT_CLEAR = 0,
	SORT_VARIANT_HISTOGRAM = 1,
	SORT_VARIANT_SCAN_LOCAL = 2,
	SORT_VARIANT_SCAN_BLOCKS = 3,
	SORT_VARIANT_SCAN_ADD = 4,
	SORT_VARIANT_SCATTER = 5,
	SORT_VARIANT_MAX = 6,
};

struct NextGIHitLighting::ShaderState {
	NextgiReflectShaderRD mark_shader;
	RID mark_version;
	RID mark_shader_rid;
	RID mark_pipeline;

	NextgiHitSortShaderRD sort_shader;
	RID sort_version;
	RID sort_shader_rids[SORT_VARIANT_MAX];
	RID sort_pipelines[SORT_VARIANT_MAX];

	NextgiLightGridShaderRD grid_shader;
	RID grid_version;
	RID grid_shader_rid;
	RID grid_pipeline;
};

struct NextGIHitMarkParamsGPU {
	float inv_view_proj[16] = {};
	float view_proj[16] = {};
	float camera_pos_radius[4] = {};
	float screen_size_origin[4] = {};
	float ambient_color_sky[4] = {};
	float fog_params0[4] = {};
	float fog_params1[4] = {};
	float fog_light_color[4] = {};
	float view_to_world[12] = {};
	uint32_t counts0[4] = {};
	uint32_t counts1[4] = {};
};
static_assert(sizeof(NextGIHitMarkParamsGPU) == 304, "NextGIHitMarkParamsGPU must match nextgi_reflect.glsl.");

struct NextGIHitLightParamsGPU {
	float camera_pos_radius[4] = {};
	float grid_min_inv_cell[4] = {};
	float ambient_color_sky[4] = {};
	float fog_params0[4] = {};
	float fog_params1[4] = {};
	float fog_light_color[4] = {};
	float wrc_camera[4] = {};
	float wrc_origin_spacing[4] = {};
	uint32_t counts0[4] = {};
	uint32_t counts1[4] = {};
};
static_assert(sizeof(NextGIHitLightParamsGPU) == 160, "NextGIHitLightParamsGPU must match nextgi_hit_layout_inc.glsl.");

static void _store_projection_hl(const Projection &p_proj, float r_dst[16]) {
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			r_dst[c * 4 + r] = p_proj.columns[c][r];
		}
	}
}

NextGIHitLighting::~NextGIHitLighting() {
	free_resources();
}

void NextGIHitLighting::free_resources() {
	{
		MutexLock lock(nextgi_hl_counter_mutex);
		if (counter_token != 0) {
			nextgi_hl_counter_registry.erase(counter_token);
			counter_token = 0;
		}
		counters_pending = false;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (shaders != nullptr) {
		if (rd != nullptr) {
			if (shaders->mark_pipeline.is_valid()) {
				rd->free_rid(shaders->mark_pipeline);
			}
			for (int i = 0; i < SORT_VARIANT_MAX; i++) {
				if (shaders->sort_pipelines[i].is_valid()) {
					rd->free_rid(shaders->sort_pipelines[i]);
				}
			}
			if (shaders->grid_pipeline.is_valid()) {
				rd->free_rid(shaders->grid_pipeline);
			}
		}
		if (shaders->mark_version.is_valid()) {
			shaders->mark_shader.version_free(shaders->mark_version);
		}
		if (shaders->sort_version.is_valid()) {
			shaders->sort_shader.version_free(shaders->sort_version);
		}
		if (shaders->grid_version.is_valid()) {
			shaders->grid_shader.version_free(shaders->grid_version);
		}
		memdelete(shaders);
		shaders = nullptr;
	}
	if (rd != nullptr) {
		const RID buffers[] = {
			record_buffer,
			sorted_index_buffer,
			histogram_buffer,
			block_sums_buffer,
			indirect_buffer,
			light_buffer,
			light_grid_buffer,
			counter_buffer,
			mark_params_buffer,
			hl_params_buffer,
			readback_buffer,
		};
		for (const RID &b : buffers) {
			if (b.is_valid()) {
				rd->free_rid(b);
			}
		}
	}
	record_buffer = RID();
	sorted_index_buffer = RID();
	histogram_buffer = RID();
	block_sums_buffer = RID();
	indirect_buffer = RID();
	light_buffer = RID();
	light_grid_buffer = RID();
	counter_buffer = RID();
	mark_params_buffer = RID();
	hl_params_buffer = RID();
	readback_buffer = RID();
	ray_capacity = 0;
	light_capacity = 0;
}

bool NextGIHitLighting::_ensure_shaders() {
	if (shaders != nullptr && shaders->mark_pipeline.is_valid() && shaders->grid_pipeline.is_valid() && shaders->sort_pipelines[0].is_valid()) {
		return true;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	NextGI *next_gi = NextGI::get_singleton();
	if (rd == nullptr || next_gi == nullptr || !next_gi->can_initialize_ray_query_shaders()) {
		return false;
	}
	if (shaders == nullptr) {
		shaders = memnew(ShaderState);
		shaders->mark_shader.initialize(Vector<String>({ "" }));
		shaders->mark_version = shaders->mark_shader.version_create(false);
		if (!shaders->mark_version.is_valid()) {
			free_resources();
			ERR_FAIL_V_MSG(false, "NextGI hit lighting: failed to compile mark shader (nextgi_reflect.glsl).");
		}
		next_gi->notify_ray_query_shader_initialized();
		shaders->mark_shader_rid = shaders->mark_shader.version_get_shader(shaders->mark_version, 0);
		shaders->mark_pipeline = rd->compute_pipeline_create(shaders->mark_shader_rid);

		Vector<String> sort_defines;
		sort_defines.push_back("#define MODE_CLEAR\n");
		sort_defines.push_back("#define MODE_HISTOGRAM\n");
		sort_defines.push_back("#define MODE_SCAN_LOCAL\n");
		sort_defines.push_back("#define MODE_SCAN_BLOCKS\n");
		sort_defines.push_back("#define MODE_SCAN_ADD\n");
		sort_defines.push_back("#define MODE_SCATTER\n");
		shaders->sort_shader.initialize(sort_defines);
		shaders->sort_version = shaders->sort_shader.version_create(false);
		if (!shaders->sort_version.is_valid()) {
			free_resources();
			ERR_FAIL_V_MSG(false, "NextGI hit lighting: failed to compile sort shader (nextgi_hit_sort.glsl).");
		}
		for (int i = 0; i < SORT_VARIANT_MAX; i++) {
			shaders->sort_shader_rids[i] = shaders->sort_shader.version_get_shader(shaders->sort_version, i);
			shaders->sort_pipelines[i] = rd->compute_pipeline_create(shaders->sort_shader_rids[i]);
			if (shaders->sort_pipelines[i].is_null()) {
				free_resources();
				ERR_FAIL_V_MSG(false, "NextGI hit lighting: failed to create sort compute pipeline.");
			}
		}

		shaders->grid_shader.initialize(Vector<String>({ "" }));
		shaders->grid_version = shaders->grid_shader.version_create(false);
		if (!shaders->grid_version.is_valid()) {
			free_resources();
			ERR_FAIL_V_MSG(false, "NextGI hit lighting: failed to compile light grid shader (nextgi_light_grid.glsl).");
		}
		shaders->grid_shader_rid = shaders->grid_shader.version_get_shader(shaders->grid_version, 0);
		shaders->grid_pipeline = rd->compute_pipeline_create(shaders->grid_shader_rid);
		if (shaders->mark_pipeline.is_null() || shaders->grid_pipeline.is_null()) {
			free_resources();
			ERR_FAIL_V_MSG(false, "NextGI hit lighting: failed to create mark or light grid pipeline.");
		}
	}
	return true;
}

bool NextGIHitLighting::_ensure_buffers(uint32_t p_max_rays, uint32_t p_light_count) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	const uint32_t rays = MAX(p_max_rays, 1u);
	if (record_buffer.is_null() || ray_capacity < rays) {
		if (record_buffer.is_valid()) {
			rd->free_rid(record_buffer);
		}
		if (sorted_index_buffer.is_valid()) {
			rd->free_rid(sorted_index_buffer);
		}
		ray_capacity = MAX(rays, ray_capacity * 2u);
		record_buffer = rd->storage_buffer_create(ray_capacity * uint32_t(sizeof(NextGIHitLightRecord)));
		sorted_index_buffer = rd->storage_buffer_create(ray_capacity * uint32_t(sizeof(uint32_t)));
	}
	const uint32_t lights = MAX(p_light_count, 1u);
	if (light_buffer.is_null() || light_capacity < lights) {
		if (light_buffer.is_valid()) {
			rd->free_rid(light_buffer);
		}
		light_capacity = MAX(lights, light_capacity * 2u);
		light_buffer = rd->storage_buffer_create(light_capacity * uint32_t(sizeof(NextGIHitLightGPU)));
	}
	if (histogram_buffer.is_null()) {
		histogram_buffer = rd->storage_buffer_create(NEXTGI_HIT_LIGHT_SORT_BINS * uint32_t(sizeof(uint32_t)));
	}
	if (block_sums_buffer.is_null()) {
		block_sums_buffer = rd->storage_buffer_create(256u * uint32_t(sizeof(uint32_t)));
	}
	if (indirect_buffer.is_null()) {
		indirect_rt_supported = rd->has_feature(RD::SUPPORTS_RAYTRACING_TRACE_RAYS_INDIRECT);
		const BitField<RD::BufferCreationBits> creation_bits = indirect_rt_supported
				? BitField<RD::BufferCreationBits>(RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT)
				: BitField<RD::BufferCreationBits>(0);
		// 48 bytes (12 uints): [0..3] reflection RT indirect, [4..7] sort compute indirect, [8..11] GI RT indirect (#128).
		indirect_buffer = rd->storage_buffer_create(48u, {}, RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT, creation_bits);
	}
	if (light_grid_buffer.is_null()) {
		light_grid_buffer = rd->storage_buffer_create(uint32_t(NEXTGI_LIGHT_GRID_CELLS) * uint32_t(NEXTGI_LIGHT_GRID_CELL_WORDS) * uint32_t(sizeof(uint32_t)));
	}
	if (counter_buffer.is_null()) {
		counter_buffer = rd->storage_buffer_create(uint32_t(sizeof(NextGIHitLightCountersGPU)));
	}
	if (mark_params_buffer.is_null()) {
		mark_params_buffer = rd->storage_buffer_create(uint32_t(sizeof(NextGIHitMarkParamsGPU)));
	}
	if (hl_params_buffer.is_null()) {
		hl_params_buffer = rd->storage_buffer_create(uint32_t(sizeof(NextGIHitLightParamsGPU)));
	}
	return record_buffer.is_valid() && sorted_index_buffer.is_valid() && histogram_buffer.is_valid() &&
			block_sums_buffer.is_valid() && indirect_buffer.is_valid() && light_buffer.is_valid() &&
			light_grid_buffer.is_valid() && counter_buffer.is_valid() && mark_params_buffer.is_valid() &&
			hl_params_buffer.is_valid();
}

void nextgi_collect_direct_lights(RenderDataRD *p_render_data, NextGIRTScene *p_rt_scene, LocalVector<NextGIHitLightGPU> &r_lights) {
	r_lights.clear();
	LightStorage *light_storage = LightStorage::get_singleton();
	if (p_render_data == nullptr || p_render_data->sdfgi_update_data == nullptr || light_storage == nullptr || p_rt_scene == nullptr) {
		return;
	}
	float exposure = 1.0f;
	if (p_render_data->camera_attributes.is_valid() && RSG::camera_attributes != nullptr) {
		exposure = RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->camera_attributes);
	}
	const bool physical = RendererSceneRenderRD::get_singleton() != nullptr && RendererSceneRenderRD::get_singleton()->is_using_physical_light_units();

	auto add_light = [&](RID p_light_instance, bool p_directional, bool p_static) {
		if (!light_storage->owns_light_instance(p_light_instance)) {
			return;
		}
		const RID light = light_storage->light_instance_get_base_light(p_light_instance);
		const RSE::LightBakeMode bake = RSG::light_storage->light_get_bake_mode(light);
		if (bake == RSE::LIGHT_BAKE_DISABLED) {
			return;
		}
		if (p_directional && RSG::light_storage->light_directional_get_sky_mode(light) == RSE::LIGHT_DIRECTIONAL_SKY_MODE_SKY_ONLY) {
			return;
		}
		const Transform3D transform = light_storage->light_instance_get_base_transform(p_light_instance);
		const Vector3 travel = -transform.basis.get_column(Vector3::AXIS_Z).normalized();
		const Color color = RSG::light_storage->light_get_color(light).srgb_to_linear();
		const RSE::LightType type = RSG::light_storage->light_get_type(light);
		float energy = RSG::light_storage->light_get_param(light, RSE::LIGHT_PARAM_ENERGY);
		if (physical) {
			energy *= RSG::light_storage->light_get_param(light, RSE::LIGHT_PARAM_INTENSITY);
			if (type == RSE::LIGHT_OMNI) {
				energy *= 1.0f / (Math::PI * 4.0f);
			} else if (type == RSE::LIGHT_SPOT) {
				energy *= 1.0f / Math::PI;
			}
		} else {
			energy *= float(Math::PI);
		}
		energy *= exposure;
		const float range = RSG::light_storage->light_get_param(light, RSE::LIGHT_PARAM_RANGE);
		const float spot_atten = RSG::light_storage->light_get_param(light, RSE::LIGHT_PARAM_SPOT_ATTENUATION);
		const float size_param = RSG::light_storage->light_get_param(light, RSE::LIGHT_PARAM_SIZE);
		const float source_radius = p_directional ? Math::sin(Math::deg_to_rad(CLAMP(size_param, 0.0f, 30.0f))) : MAX(size_param, 0.0f);
		const bool has_shadow = RSG::light_storage->light_has_shadow(light);
		const uint32_t caster_mask = RSG::light_storage->light_get_shadow_caster_mask(light);

		NextGIHitLightGPU gpu;
		gpu.color_energy[0] = color.r;
		gpu.color_energy[1] = color.g;
		gpu.color_energy[2] = color.b;
		gpu.color_energy[3] = energy;
		gpu.position_inv_range[0] = transform.origin.x;
		gpu.position_inv_range[1] = transform.origin.y;
		gpu.position_inv_range[2] = transform.origin.z;
		gpu.position_inv_range[3] = (!p_directional && range > 0.0f) ? 1.0f / range : 0.0f;
		gpu.direction_type[0] = travel.x;
		gpu.direction_type[1] = travel.y;
		gpu.direction_type[2] = travel.z;
		gpu.direction_type[3] = float(type);
		gpu.spot_atten_radius[0] = RSG::light_storage->light_get_param(light, RSE::LIGHT_PARAM_ATTENUATION);
		gpu.spot_atten_radius[1] = Math::cos(Math::deg_to_rad(RSG::light_storage->light_get_param(light, RSE::LIGHT_PARAM_SPOT_ANGLE)));
		gpu.spot_atten_radius[2] = spot_atten > 0.0f ? 1.0f / spot_atten : 1.0f;
		gpu.spot_atten_radius[3] = source_radius;
		gpu.shadow_flags[0] = (has_shadow ? uint32_t(NEXTGI_HL_LIGHT_FLAG_SHADOW) : 0u) | (p_static ? uint32_t(NEXTGI_HL_LIGHT_FLAG_STATIC) : 0u);
		gpu.shadow_flags[1] = has_shadow ? p_rt_scene->get_shadow_light_mode(caster_mask) : 0u;
		gpu.shadow_flags[2] = caster_mask & NEXTGI_SHADOW_LAYER_MASK;
		gpu.shadow_flags[3] = 0u;
		r_lights.push_back(gpu);
	};

	const RendererSceneRender::RenderSDFGIUpdateData *feed = p_render_data->sdfgi_update_data;
	if (feed->directional_lights != nullptr) {
		for (int i = 0; i < feed->directional_lights->size(); i++) {
			add_light(feed->directional_lights->get(i), true, false);
		}
	}
	for (uint32_t i = 0; i < feed->nextgi_dynamic_light_count; i++) {
		add_light(feed->nextgi_dynamic_light_instances[i], false, false);
	}
	for (uint32_t i = 0; i < feed->nextgi_static_light_count; i++) {
		add_light(feed->nextgi_static_light_instances[i], false, true);
	}
}

uint32_t NextGIHitLighting::_upload_lights(RenderDataRD *p_render_data, NextGIRTScene *p_rt_scene) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr) {
		return 0;
	}
	LocalVector<NextGIHitLightGPU> gpu_lights;
	nextgi_collect_direct_lights(p_render_data, p_rt_scene, gpu_lights);

	const uint32_t count = gpu_lights.size();
	if (!_ensure_buffers(ray_capacity, MAX(count, 1u))) {
		return 0;
	}
	if (count > 0) {
		rd->buffer_update(light_buffer, 0, count * uint32_t(sizeof(NextGIHitLightGPU)), gpu_lights.ptr());
	} else {
		NextGIHitLightGPU empty;
		rd->buffer_update(light_buffer, 0, uint32_t(sizeof(empty)), &empty);
	}
	return count;
}

bool NextGIHitLighting::_dispatch_light_grid(const ExecuteParams &p_params, uint32_t p_light_count) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	const float radius = MAX(p_params.rt_radius, 1.0f);
	const Vector3 grid_min = p_params.camera_pos - Vector3(radius, radius, radius);
	const float cell_size = (2.0f * radius) / float(NEXTGI_LIGHT_GRID_DIM);

	struct LightGridPush {
		float grid_min[3];
		float cell_size;
		uint32_t light_count;
		uint32_t pad[3];
	};
	static_assert(sizeof(LightGridPush) == 32, "LightGridPush must be 32 bytes.");
	LightGridPush push = {};
	push.grid_min[0] = grid_min.x;
	push.grid_min[1] = grid_min.y;
	push.grid_min[2] = grid_min.z;
	push.cell_size = cell_size;
	push.light_count = p_light_count;

	RD::Uniform grid_uniforms[3] = {
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, light_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, light_grid_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, counter_buffer),
	};
	const RID grid_set = rd->uniform_set_create(VectorView<RD::Uniform>(grid_uniforms, 3), shaders->grid_shader_rid, 0);
	ERR_FAIL_COND_V(grid_set.is_null(), false);
	rd->draw_command_begin_label("NextGI World Light Grid");
	const RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, shaders->grid_pipeline);
	rd->compute_list_bind_uniform_set(list, grid_set, 0);
	rd->compute_list_set_push_constant(list, &push, sizeof(push));
	const uint32_t grid_groups = NEXTGI_LIGHT_GRID_DIM / 4u;
	rd->compute_list_dispatch(list, grid_groups, grid_groups, grid_groups);
	rd->compute_list_end();
	rd->draw_command_end_label();
	rd->free_rid(grid_set);
	return true;
}

bool NextGIHitLighting::_dispatch_mark(const ExecuteParams &p_params, uint32_t p_max_rays) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	const uint32_t trace_w = uint32_t(p_params.trace_size.width);
	const uint32_t trace_h = uint32_t(p_params.trace_size.height);
	const float radius = MAX(p_params.rt_radius, 1.0f);
	const bool has_translucent = p_params.rt_scene->get_translucent_instance_count() > 0;

	NextGIHitMarkParamsGPU mp;
	_store_projection_hl(p_params.inv_view_proj, mp.inv_view_proj);
	_store_projection_hl(p_params.view_proj, mp.view_proj);
	mp.camera_pos_radius[0] = p_params.camera_pos.x;
	mp.camera_pos_radius[1] = p_params.camera_pos.y;
	mp.camera_pos_radius[2] = p_params.camera_pos.z;
	mp.camera_pos_radius[3] = radius;
	mp.screen_size_origin[0] = float(MAX(p_params.screen_size.width, 1));
	mp.screen_size_origin[1] = float(MAX(p_params.screen_size.height, 1));
	mp.screen_size_origin[2] = p_params.max_origin_offset;
	mp.screen_size_origin[3] = p_params.max_trace_roughness;
	mp.ambient_color_sky[0] = p_params.ambient_color.r;
	mp.ambient_color_sky[1] = p_params.ambient_color.g;
	mp.ambient_color_sky[2] = p_params.ambient_color.b;
	mp.ambient_color_sky[3] = p_params.sky_enabled ? 1.0f : 0.0f;
	mp.fog_params0[0] = p_params.fog_enabled ? 1.0f : 0.0f;
	mp.fog_params0[1] = p_params.fog_depth_mode ? 1.0f : 0.0f;
	mp.fog_params0[2] = p_params.fog_density;
	mp.fog_params0[3] = p_params.fog_height;
	mp.fog_params1[0] = p_params.fog_height_density;
	mp.fog_params1[1] = p_params.fog_depth_begin;
	mp.fog_params1[2] = p_params.fog_depth_end;
	mp.fog_params1[3] = p_params.fog_depth_curve;
	mp.fog_light_color[0] = p_params.fog_light_color.r;
	mp.fog_light_color[1] = p_params.fog_light_color.g;
	mp.fog_light_color[2] = p_params.fog_light_color.b;
	mp.fog_light_color[3] = p_params.fog_light_energy;
	NextGIViewMath::store_basis(p_params.view_to_world, mp.view_to_world);
	mp.counts0[0] = p_params.frame;
	mp.counts0[1] = p_params.hit_materials->get_record_count();
	mp.counts0[2] = p_params.hit_materials->get_material_count();
	mp.counts0[3] = p_max_rays;
	mp.counts1[0] = p_params.reflect_enabled ? 1u : 0u;
	mp.counts1[1] = p_params.reflect_debug ? 1u : 0u;
	mp.counts1[2] = (p_params.blue_noise_mode ? 1u : 0u) | (p_params.front_layer ? 2u : 0u) | (has_translucent ? 4u : 0u);
	mp.counts1[3] = p_params.hiz_max_mip;
	rd->buffer_update(mark_params_buffer, 0, sizeof(mp), &mp);

	const RID wrc_rad = (p_params.wrc != nullptr && p_params.wrc->is_ready() && p_params.wrc->get_radiance().is_valid()) ? p_params.wrc->get_radiance() : p_params.dummy_color;
	const RID wrc_vis = (p_params.wrc != nullptr && p_params.wrc->is_ready() && p_params.wrc->get_visibility().is_valid()) ? p_params.wrc->get_visibility() : p_params.dummy_color;
	const RID wrc_params = (p_params.wrc != nullptr && p_params.wrc->is_ready() && p_params.wrc->get_params_buffer().is_valid()) ? p_params.wrc->get_params_buffer() : p_params.dummy_buffer;
	const RID records_buf = p_params.rt_scene->get_instance_record_buffer().is_valid() ? p_params.rt_scene->get_instance_record_buffer() : p_params.dummy_buffer;
	const RID hit_surf_buf = p_params.hit_materials->get_hit_surface_buffer().is_valid() ? p_params.hit_materials->get_hit_surface_buffer() : p_params.dummy_buffer;
	const RID hit_mat_buf = p_params.hit_materials->get_material_table_buffer().is_valid() ? p_params.hit_materials->get_material_table_buffer() : p_params.dummy_buffer;

	RD::Uniform mark_uniforms[20] = {
		RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, p_params.rt_scene->get_tlas()),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ p_params.sampler_nearest, p_params.depth })),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ p_params.sampler_nearest, p_params.normal_roughness })),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ p_params.sampler_nearest, p_params.hiz })),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ p_params.sampler_linear, p_params.last_frame })),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ p_params.sampler_linear, p_params.sky_tex })),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ p_params.sampler_nearest, p_params.blue_noise })),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ p_params.sampler_linear, wrc_rad })),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ p_params.sampler_linear, wrc_vis })),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, records_buf),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, hit_surf_buf),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, hit_mat_buf),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, wrc_params),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, mark_params_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, record_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, counter_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, p_params.reflect_counter_buffer.is_valid() ? p_params.reflect_counter_buffer : p_params.dummy_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 17, p_params.reflection_out),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 18, p_params.hit_distance_out.is_valid() ? p_params.hit_distance_out : p_params.reflection_out),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 19, p_params.debug_out.is_valid() ? p_params.debug_out : p_params.reflection_out),
	};
	const RID mark_set = rd->uniform_set_create(VectorView<RD::Uniform>(mark_uniforms, 20), shaders->mark_shader_rid, 0);
	ERR_FAIL_COND_V(mark_set.is_null(), false);
	rd->draw_command_begin_label("NextGI Hit Mark");
	const RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, shaders->mark_pipeline);
	rd->compute_list_bind_uniform_set(list, mark_set, 0);
	rd->compute_list_dispatch(list, MAX((trace_w + 7u) / 8u, 1u), MAX((trace_h + 7u) / 8u, 1u), 1);
	rd->compute_list_end();
	rd->draw_command_end_label();
	rd->free_rid(mark_set);
	return true;
}

bool NextGIHitLighting::_dispatch_sort(uint32_t p_max_rays) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	struct SortPush {
		uint32_t max_rays;
		uint32_t pad[3];
	};
	SortPush push = { p_max_rays, { 0, 0, 0 } };

	// Each sort stage uses its own uniform set matching its exact shader variant bindings so RDG
	// tracks STORAGE_BUFFER_READ_WRITE on indirect_buffer and block_sums_buffer accurately and emits
	// full COMPUTE_SHADER_WRITE -> DRAW_INDIRECT_READ barriers before indirect dispatches/RT traces.
	RD::Uniform clear_u[3] = {
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, histogram_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, counter_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, indirect_buffer),
	};
	RD::Uniform hist_u[3] = {
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, record_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, histogram_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, counter_buffer),
	};
	RD::Uniform scan_local_u[2] = {
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, histogram_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, block_sums_buffer),
	};
	RD::Uniform scan_blocks_u[1] = {
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, block_sums_buffer),
	};
	RD::Uniform scan_add_u[4] = {
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, histogram_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, block_sums_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, counter_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, indirect_buffer),
	};
	RD::Uniform scatter_u[4] = {
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, record_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, histogram_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, sorted_index_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, counter_buffer),
	};

	const RID sort_sets[SORT_VARIANT_MAX] = {
		rd->uniform_set_create(VectorView<RD::Uniform>(clear_u, 3), shaders->sort_shader_rids[SORT_VARIANT_CLEAR], 0),
		rd->uniform_set_create(VectorView<RD::Uniform>(hist_u, 3), shaders->sort_shader_rids[SORT_VARIANT_HISTOGRAM], 0),
		rd->uniform_set_create(VectorView<RD::Uniform>(scan_local_u, 2), shaders->sort_shader_rids[SORT_VARIANT_SCAN_LOCAL], 0),
		rd->uniform_set_create(VectorView<RD::Uniform>(scan_blocks_u, 1), shaders->sort_shader_rids[SORT_VARIANT_SCAN_BLOCKS], 0),
		rd->uniform_set_create(VectorView<RD::Uniform>(scan_add_u, 4), shaders->sort_shader_rids[SORT_VARIANT_SCAN_ADD], 0),
		rd->uniform_set_create(VectorView<RD::Uniform>(scatter_u, 4), shaders->sort_shader_rids[SORT_VARIANT_SCATTER], 0),
	};
	auto free_sets = [&]() {
		for (int i = 0; i < SORT_VARIANT_MAX; i++) {
			if (sort_sets[i].is_valid()) {
				rd->free_rid(sort_sets[i]);
			}
		}
	};
	for (int i = 0; i < SORT_VARIANT_MAX; i++) {
		if (sort_sets[i].is_null()) {
			free_sets();
			ERR_FAIL_V_MSG(false, "NextGI hit lighting: failed to create sort uniform set.");
		}
	}

	RENDER_TIMESTAMP("NextGI Hit Sort");
	rd->draw_command_begin_label("NextGI Hit Sort");

	// 3a. Clear histogram & write indirect dispatch arguments.
	{
		const RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, shaders->sort_pipelines[SORT_VARIANT_CLEAR]);
		rd->compute_list_bind_uniform_set(list, sort_sets[SORT_VARIANT_CLEAR], 0);
		rd->compute_list_set_push_constant(list, &push, sizeof(push));
		rd->compute_list_dispatch(list, 256u, 1u, 1u);
		rd->compute_list_end();
	}

	// 3b. Histogram active hits by 16-bit material key.
	{
		const RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, shaders->sort_pipelines[SORT_VARIANT_HISTOGRAM]);
		rd->compute_list_bind_uniform_set(list, sort_sets[SORT_VARIANT_HISTOGRAM], 0);
		rd->compute_list_set_push_constant(list, &push, sizeof(push));
		rd->compute_list_dispatch_indirect(list, indirect_buffer, 16u);
		rd->compute_list_end();
	}

	// 3c. Per-block exclusive prefix sum (256 blocks x 256 bins).
	{
		const RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, shaders->sort_pipelines[SORT_VARIANT_SCAN_LOCAL]);
		rd->compute_list_bind_uniform_set(list, sort_sets[SORT_VARIANT_SCAN_LOCAL], 0);
		rd->compute_list_dispatch(list, 256u, 1u, 1u);
		rd->compute_list_end();
	}

	// 3d. Exclusive prefix sum over the 256 block sums.
	{
		const RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, shaders->sort_pipelines[SORT_VARIANT_SCAN_BLOCKS]);
		rd->compute_list_bind_uniform_set(list, sort_sets[SORT_VARIANT_SCAN_BLOCKS], 0);
		rd->compute_list_dispatch(list, 1u, 1u, 1u);
		rd->compute_list_end();
	}

	// 3e. Add scanned block offsets to histogram bins (and emit reflection + GI indirect counts).
	{
		const RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, shaders->sort_pipelines[SORT_VARIANT_SCAN_ADD]);
		rd->compute_list_bind_uniform_set(list, sort_sets[SORT_VARIANT_SCAN_ADD], 0);
		rd->compute_list_set_push_constant(list, &push, sizeof(push));
		rd->compute_list_dispatch(list, 256u, 1u, 1u);
		rd->compute_list_end();
	}

	// 3f. Scatter active hit indices into sorted order.
	{
		const RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, shaders->sort_pipelines[SORT_VARIANT_SCATTER]);
		rd->compute_list_bind_uniform_set(list, sort_sets[SORT_VARIANT_SCATTER], 0);
		rd->compute_list_set_push_constant(list, &push, sizeof(push));
		rd->compute_list_dispatch_indirect(list, indirect_buffer, 16u);
		rd->compute_list_end();
	}

	rd->draw_command_end_label();
	free_sets();
	return true;
}

void NextGIHitLighting::_upload_hl_params(const ExecuteParams &p_params, uint32_t p_max_rays, uint32_t p_light_count, uint32_t p_max_bounces) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL(rd);
	const uint32_t trace_w = uint32_t(p_params.trace_size.width);
	const uint32_t trace_h = uint32_t(p_params.trace_size.height);
	const float radius = MAX(p_params.rt_radius, 1.0f);
	const Vector3 grid_min = p_params.camera_pos - Vector3(radius, radius, radius);
	const float cell_size = (2.0f * radius) / float(NEXTGI_LIGHT_GRID_DIM);
	const float inv_cell_size = 1.0f / MAX(cell_size, 1e-4f);
	const bool has_translucent = p_params.rt_scene->get_translucent_instance_count() > 0;

	NextGIHitLightParamsGPU hp;
	hp.camera_pos_radius[0] = p_params.camera_pos.x;
	hp.camera_pos_radius[1] = p_params.camera_pos.y;
	hp.camera_pos_radius[2] = p_params.camera_pos.z;
	hp.camera_pos_radius[3] = radius;
	hp.grid_min_inv_cell[0] = grid_min.x;
	hp.grid_min_inv_cell[1] = grid_min.y;
	hp.grid_min_inv_cell[2] = grid_min.z;
	hp.grid_min_inv_cell[3] = inv_cell_size;
	hp.ambient_color_sky[0] = p_params.ambient_color.r;
	hp.ambient_color_sky[1] = p_params.ambient_color.g;
	hp.ambient_color_sky[2] = p_params.ambient_color.b;
	hp.ambient_color_sky[3] = p_params.sky_enabled ? 1.0f : 0.0f;
	hp.fog_params0[0] = p_params.fog_enabled ? 1.0f : 0.0f;
	hp.fog_params0[1] = p_params.fog_depth_mode ? 1.0f : 0.0f;
	hp.fog_params0[2] = p_params.fog_density;
	hp.fog_params0[3] = p_params.fog_height;
	hp.fog_params1[0] = p_params.fog_height_density;
	hp.fog_params1[1] = p_params.fog_depth_begin;
	hp.fog_params1[2] = p_params.fog_depth_end;
	hp.fog_params1[3] = p_params.fog_depth_curve;
	hp.fog_light_color[0] = p_params.fog_light_color.r;
	hp.fog_light_color[1] = p_params.fog_light_color.g;
	hp.fog_light_color[2] = p_params.fog_light_color.b;
	hp.fog_light_color[3] = p_params.fog_light_energy;
	if (p_params.wrc != nullptr && p_params.wrc->is_ready()) {
		const Vector3 wrc_cam = p_params.camera_pos;
		const Vector3 wrc_origin = nextgi_wrc_snap_origin(wrc_cam, 1.0f);
		hp.wrc_camera[0] = wrc_cam.x;
		hp.wrc_camera[1] = wrc_cam.y;
		hp.wrc_camera[2] = wrc_cam.z;
		hp.wrc_camera[3] = 1.0f;
		hp.wrc_origin_spacing[0] = wrc_origin.x;
		hp.wrc_origin_spacing[1] = wrc_origin.y;
		hp.wrc_origin_spacing[2] = wrc_origin.z;
		hp.wrc_origin_spacing[3] = 1.0f;
	}
	const bool has_src_indirect = p_params.src != nullptr && p_params.relight != nullptr &&
			p_params.relight->get_indirect_texture().is_valid() && p_params.relight->get_page_rect_buffer().is_valid() &&
			p_params.src->get_card_buffer().is_valid() && p_params.src->get_card_set_buffer().is_valid();
	const bool has_world_cache = p_params.wc_keys.is_valid() && p_params.wc_radiance.is_valid();
	hp.counts0[0] = p_max_rays;
	hp.counts0[1] = p_light_count;
	hp.counts0[2] = p_max_bounces;
	hp.counts0[3] = p_params.frame;
	hp.counts1[0] = trace_w;
	hp.counts1[1] = trace_h;
	hp.counts1[2] = p_params.relight != nullptr ? p_params.relight->get_page_rect_count() : 0u;
	hp.counts1[3] = (has_translucent ? uint32_t(NEXTGI_HL_PARAM_FLAG_TRANSLUCENT) : 0u) |
			(p_params.reflect_debug ? uint32_t(NEXTGI_HL_PARAM_FLAG_REFLECT_DEBUG) : 0u) |
			(p_params.blue_noise_mode ? uint32_t(NEXTGI_HL_PARAM_FLAG_BLUE_NOISE) : 0u) |
			(has_src_indirect ? uint32_t(NEXTGI_HL_PARAM_FLAG_SRC_INDIRECT) : 0u) |
			(has_world_cache ? uint32_t(NEXTGI_HL_PARAM_FLAG_WORLD_CACHE) : 0u);
	rd->buffer_update(hl_params_buffer, 0, sizeof(hp), &hp);
}

bool NextGIHitLighting::_dispatch_raygen(const ExecuteParams &p_params, bool p_gi_pass, RID p_output_image, uint32_t p_max_rays, uint32_t p_indirect_offset) {
	NextGIHitLightTrace trace;
	trace.params = hl_params_buffer;
	trace.records = record_buffer;
	trace.sorted_indices = sorted_index_buffer;
	trace.lights = light_buffer;
	trace.light_grid = light_grid_buffer;
	trace.counters = counter_buffer;
	trace.indirect_buffer = indirect_rt_supported ? indirect_buffer : RID();
	trace.indirect_offset = p_indirect_offset;
	trace.src_material_depth = p_params.src != nullptr ? p_params.src->get_atlas_texture(NextGISRC::ATLAS_DEPTH) : RID();
	trace.src_cards = p_params.src != nullptr ? p_params.src->get_card_buffer() : RID();
	trace.src_card_sets = p_params.src != nullptr ? p_params.src->get_card_set_buffer() : RID();
	trace.src_page_rects = p_params.relight != nullptr ? p_params.relight->get_page_rect_buffer() : RID();
	trace.src_indirect = p_params.relight != nullptr ? p_params.relight->get_indirect_texture() : RID();
	trace.wc_keys = p_params.wc_keys;
	trace.wc_radiance = p_params.wc_radiance;
	trace.wrc_radiance = (p_params.wrc != nullptr && p_params.wrc->is_ready()) ? p_params.wrc->get_radiance() : RID();
	trace.wrc_visibility = (p_params.wrc != nullptr && p_params.wrc->is_ready()) ? p_params.wrc->get_visibility() : RID();
	trace.sky = p_params.sky_tex;
	trace.blue_noise = p_params.blue_noise;
	trace.reflection_out = p_output_image;
	trace.hit_distance_out = p_gi_pass ? RID() : p_params.hit_distance_out;
	trace.debug_out = p_gi_pass ? RID() : p_params.debug_out;
	trace.max_rays = p_max_rays;
	trace.time = p_params.time;
	trace.gi_pass = p_gi_pass;

	return p_params.hit_materials->trace_hit_lighting(p_params.rt_scene, trace);
}

bool NextGIHitLighting::execute(const ExecuteParams &p_params) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr || p_params.rt_scene == nullptr || p_params.hit_materials == nullptr || !p_params.rt_scene->is_tlas_traceable() || !p_params.hit_materials->can_trace_hit_lighting()) {
		return false;
	}
	if (p_params.trace_size.width <= 0 || p_params.trace_size.height <= 0 || !p_params.reflection_out.is_valid()) {
		return false;
	}
	if (!_ensure_shaders()) {
		return false;
	}
	const uint32_t trace_w = uint32_t(p_params.trace_size.width);
	const uint32_t trace_h = uint32_t(p_params.trace_size.height);
	const uint32_t max_rays = trace_w * trace_h;
	if (!_ensure_buffers(max_rays, MAX(light_capacity, 1u))) {
		return false;
	}

	const uint32_t light_count = _upload_lights(p_params.render_data, p_params.rt_scene);
	const uint32_t max_bounces = nextgi_clamp_max_reflection_bounces(int(p_params.max_reflection_bounces));

	// Clear hit lighting counters and reflect counters.
	NextGIHitLightCountersGPU zero_counters;
	rd->buffer_update(counter_buffer, 0, sizeof(zero_counters), &zero_counters);
	if (p_params.reflect_counter_buffer.is_valid()) {
		const uint32_t zero_reflect[4] = {};
		rd->buffer_update(p_params.reflect_counter_buffer, 0, sizeof(zero_reflect), zero_reflect);
	}

	if (!_dispatch_light_grid(p_params, light_count)) {
		return false;
	}
	if (!_dispatch_mark(p_params, max_rays)) {
		return false;
	}
	if (!_dispatch_sort(max_rays)) {
		return false;
	}
	_upload_hl_params(p_params, max_rays, light_count, max_bounces);

	RENDER_TIMESTAMP("NextGI Hit Lighting");
	if (!_dispatch_raygen(p_params, false, p_params.reflection_out, max_rays, 0u)) {
		return false;
	}

	counters.dispatches++;
	counters.last_gi_marked_hits = 0;
	counters.last_gi_sorted_hits = 0;
	counters.last_gi_shaded_hits = 0;
	counters.last_gi_shadow_rays = 0;
	counters.last_light_count = light_count;
	counters.last_max_bounces = max_bounces;
	counters.indirect_dispatch = indirect_rt_supported;
	_request_counters();
	return true;
}

// GodotNext: nextgi_hit_lighting_gi (#128). Step A of the combined GI + reflections pipeline:
// sizes the shared hit-record buffer for both GI and reflection rays, uploads scenario lights,
// zeroes shared counters, and builds the 64^3 world light grid once.
bool NextGIHitLighting::begin_combined(const ExecuteParams &p_params, uint32_t p_gi_max_rays) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr || p_params.rt_scene == nullptr || p_params.hit_materials == nullptr || !p_params.rt_scene->is_tlas_traceable() || !p_params.hit_materials->can_trace_hit_lighting()) {
		return false;
	}
	if (p_params.trace_size.width <= 0 || p_params.trace_size.height <= 0 || !p_params.reflection_out.is_valid() || p_gi_max_rays == 0u) {
		return false;
	}
	if (!_ensure_shaders()) {
		return false;
	}
	const uint32_t reflect_max_rays = uint32_t(p_params.trace_size.width) * uint32_t(p_params.trace_size.height);
	const uint32_t total_max_rays = p_gi_max_rays + reflect_max_rays;
	if (!_ensure_buffers(total_max_rays, MAX(light_capacity, 1u))) {
		return false;
	}

	const uint32_t light_count = _upload_lights(p_params.render_data, p_params.rt_scene);

	NextGIHitLightCountersGPU zero_counters;
	rd->buffer_update(counter_buffer, 0, sizeof(zero_counters), &zero_counters);
	if (p_params.reflect_counter_buffer.is_valid()) {
		const uint32_t zero_reflect[4] = {};
		rd->buffer_update(p_params.reflect_counter_buffer, 0, sizeof(zero_reflect), zero_reflect);
	}

	if (!_dispatch_light_grid(p_params, light_count)) {
		return false;
	}

	combined_max_rays = total_max_rays;
	combined_gi_max_rays = p_gi_max_rays;
	combined_reflect_max_rays = reflect_max_rays;
	combined_light_count = light_count;
	return true;
}

// Step D: after nextgi_gather.glsl (MODE_HIT_LIGHTING_GI) has appended diffuse GI hit records,
// append reflection hit records into the same buffer, run the single 16-bit counting sort (splitting
// reflections [0x0000..0x7FFF] from diffuse GI [0x8000..0xFFFF]), and shade diffuse GI into
// probe_image BEFORE probe spatial/temporal filtering.
bool NextGIHitLighting::mark_sort_and_shade_gi(const ExecuteParams &p_params, RID p_probe_image) {
	if (combined_max_rays == 0u || !p_probe_image.is_valid()) {
		return false;
	}
	if (!_dispatch_mark(p_params, combined_max_rays)) {
		return false;
	}
	if (!_dispatch_sort(combined_max_rays)) {
		return false;
	}
	const uint32_t max_bounces = nextgi_clamp_max_reflection_bounces(int(p_params.max_reflection_bounces));
	_upload_hl_params(p_params, combined_max_rays, combined_light_count, max_bounces);

	RENDER_TIMESTAMP("NextGI Hit Lighting GI");
	if (!_dispatch_raygen(p_params, true, p_probe_image, combined_gi_max_rays, 32u)) {
		return false;
	}
	counters.gi_dispatches++;
	return true;
}

// Step F: after _denoise_probes(), shade the sorted reflection hit slice using the already-sorted
// indices, world light grid, and hl_params_buffer.
bool NextGIHitLighting::shade_reflections_combined(const ExecuteParams &p_params) {
	if (combined_max_rays == 0u || !p_params.reflection_out.is_valid()) {
		return false;
	}
	const uint32_t max_bounces = nextgi_clamp_max_reflection_bounces(int(p_params.max_reflection_bounces));

	RENDER_TIMESTAMP("NextGI Hit Lighting");
	if (!_dispatch_raygen(p_params, false, p_params.reflection_out, combined_reflect_max_rays, 0u)) {
		return false;
	}

	counters.dispatches++;
	counters.last_light_count = combined_light_count;
	counters.last_max_bounces = max_bounces;
	counters.indirect_dispatch = indirect_rt_supported;
	_request_counters();
	return true;
}

void NextGIHitLighting::_request_counters() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr || !counter_buffer.is_valid()) {
		return;
	}
	uint64_t token = 0;
	{
		MutexLock lock(nextgi_hl_counter_mutex);
		if (counters_pending) {
			return;
		}
		counters_pending = true;
		token = nextgi_hl_counter_next_token++;
		counter_token = token;
		nextgi_hl_counter_registry.insert(token, this);
	}
	if (!readback_buffer.is_valid()) {
		readback_buffer = rd->storage_buffer_create(sizeof(NextGIHitLightCountersGPU));
	}
	Error err = readback_buffer.is_valid() ? OK : FAILED;
	if (err == OK) {
		rd->buffer_copy(counter_buffer, readback_buffer, 0, 0, sizeof(NextGIHitLightCountersGPU));
		err = rd->buffer_get_data_async(readback_buffer, callable_mp_static(&NextGIHitLighting::_counters_received).bind(token), 0, sizeof(NextGIHitLightCountersGPU));
	}
	if (err != OK) {
		MutexLock lock(nextgi_hl_counter_mutex);
		nextgi_hl_counter_registry.erase(token);
		counter_token = 0;
		counters_pending = false;
	}
}

void NextGIHitLighting::_counters_received(const Vector<uint8_t> &p_data, uint64_t p_token) {
	MutexLock lock(nextgi_hl_counter_mutex);
	NextGIHitLighting **owner = nextgi_hl_counter_registry.getptr(p_token);
	if (owner == nullptr) {
		return;
	}
	NextGIHitLighting *self = *owner;
	nextgi_hl_counter_registry.erase(p_token);
	self->counter_token = 0;
	self->counters_pending = false;
	if (p_data.size() < int(sizeof(NextGIHitLightCountersGPU))) {
		return;
	}
	NextGIHitLightCountersGPU got;
	memcpy(&got, p_data.ptr(), sizeof(got));
	self->counters.last_marked_hits = got.marked_hits;
	self->counters.last_sorted_hits = got.sorted_hits;
	self->counters.last_refl_sorted_hits = got.refl_sorted_hits;
	self->counters.last_gi_marked_hits = got.gi_marked_hits;
	self->counters.last_gi_sorted_hits = got.gi_sorted_hits;
	self->counters.last_gi_shaded_hits = got.gi_shaded_hits;
	self->counters.last_gi_shadow_rays = got.gi_shadow_rays;
	self->counters.last_shadow_rays = got.shadow_rays;
	self->counters.last_unshadowed_lights = got.unshadowed_lights;
	self->counters.last_bounce_rays = got.bounce_rays;
	self->counters.last_translucent_skips = got.translucent_skips;
	self->counters.last_light_grid_overflow = got.light_grid_overflow;
}

void NextGIHitLighting::fill_counters(Dictionary &r_counters, bool p_gi_active) const {
	r_counters["hit_lighting_dispatches"] = int64_t(counters.dispatches);
	r_counters["hit_lighting_gi_dispatches"] = int64_t(counters.gi_dispatches);
	r_counters["hit_lighting_marked_hits"] = int64_t(counters.last_marked_hits);
	r_counters["hit_lighting_sorted_hits"] = int64_t(counters.last_sorted_hits);
	r_counters["hit_lighting_refl_sorted_hits"] = int64_t(counters.last_refl_sorted_hits);
	r_counters["hit_lighting_gi_marked_hits"] = int64_t(p_gi_active ? counters.last_gi_marked_hits : 0u);
	r_counters["hit_lighting_gi_sorted_hits"] = int64_t(p_gi_active ? counters.last_gi_sorted_hits : 0u);
	r_counters["hit_lighting_gi_rays"] = int64_t(p_gi_active ? counters.last_gi_shaded_hits : 0u);
	r_counters["hit_lighting_gi_shadow_rays"] = int64_t(p_gi_active ? counters.last_gi_shadow_rays : 0u);
	r_counters["hit_lighting_shadow_rays"] = int64_t(counters.last_shadow_rays);
	r_counters["hit_lighting_unshadowed_lights"] = int64_t(counters.last_unshadowed_lights);
	r_counters["hit_lighting_bounce_rays"] = int64_t(counters.last_bounce_rays);
	r_counters["hit_lighting_translucent_skips"] = int64_t(counters.last_translucent_skips);
	r_counters["hit_lighting_light_grid_overflow"] = int64_t(counters.last_light_grid_overflow);
	r_counters["hit_lighting_lights"] = int64_t(counters.last_light_count);
	r_counters["hit_lighting_max_bounces"] = int64_t(counters.last_max_bounces);
	r_counters["hit_lighting_indirect_dispatch"] = counters.indirect_dispatch;
}

} // namespace RendererRD
