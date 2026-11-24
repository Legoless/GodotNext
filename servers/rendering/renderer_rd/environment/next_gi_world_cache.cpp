/**************************************************************************/
/*  next_gi_world_cache.cpp                                               */
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

#include "next_gi_world_cache.h"

#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "core/math/projection.h"
#include "core/object/callable_mp.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "servers/rendering/renderer_rd/environment/next_gi_hit_lighting.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_scene.h"
#include "servers/rendering/renderer_rd/environment/next_gi_view.h"
#include "servers/rendering/renderer_rd/environment/nextgi_blue_noise.h"
#include "servers/rendering/renderer_rd/environment/sky.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_light_grid.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_world_cache_resolve.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/rendering_device.h"

#include <cstring>

namespace RendererRD {

// GodotNext: nextgi_world_cache (#294, plan §21.2.1 / §26.1 / §26.2).

/* Device-free policy & CPU reference helpers. */

bool nextgi_world_cache_api_is_metal(const String &p_api_name) {
	return p_api_name.nocasecmp_to("Metal") == 0;
}

bool nextgi_world_cache_api_supported(const String &p_api_name) {
	return p_api_name.nocasecmp_to("Vulkan") == 0 ||
			p_api_name.nocasecmp_to("D3D12") == 0 ||
			p_api_name.nocasecmp_to("Direct3D 12") == 0;
}

const char *nextgi_world_cache_reason_name(NextGIWorldCacheReason p_reason) {
	switch (p_reason) {
		case NEXTGI_WC_REASON_OFF:
			return "OFF";
		case NEXTGI_WC_REASON_ACTIVE:
			return "ACTIVE";
		case NEXTGI_WC_REASON_SOFTWARE_TIER:
			return "SOFTWARE_TIER";
		case NEXTGI_WC_REASON_NOT_HWRT:
			return "NOT_HWRT";
		case NEXTGI_WC_REASON_INLINE_HWRT:
			return "INLINE_HWRT";
		case NEXTGI_WC_REASON_METAL_UNSUPPORTED:
			return "METAL_UNSUPPORTED";
		case NEXTGI_WC_REASON_UNSUPPORTED_API:
			return "UNSUPPORTED_API";
		case NEXTGI_WC_REASON_HIT_MATERIALS_UNAVAILABLE:
			return "HIT_MATERIALS_UNAVAILABLE";
		case NEXTGI_WC_REASON_RAYGEN_UNAVAILABLE:
			return "RAYGEN_UNAVAILABLE";
		case NEXTGI_WC_REASON_NO_CURRENT_RECORDS:
			return "NO_CURRENT_RECORDS";
		case NEXTGI_WC_REASON_MAX:
			break;
	}
	return "UNKNOWN";
}

const char *nextgi_world_cache_reason_message(NextGIWorldCacheReason p_reason) {
	switch (p_reason) {
		case NEXTGI_WC_REASON_SOFTWARE_TIER:
			return "NextGI world cache: keeping surface cache (SOFTWARE_TIER: World Cache applies only on the HWRT tier, not software ray tracing).";
		case NEXTGI_WC_REASON_NOT_HWRT:
			return "NextGI world cache: keeping surface cache (NOT_HWRT: World Cache applies only on the HWRT tier).";
		case NEXTGI_WC_REASON_INLINE_HWRT:
			return "NextGI world cache: keeping surface cache (INLINE_HWRT: World Cache requires Full or Simplified Material hardware ray tracing mode, not Inline).";
		case NEXTGI_WC_REASON_METAL_UNSUPPORTED:
			return "NextGI world cache: keeping surface cache (METAL_UNSUPPORTED: World Cache is supported on Vulkan and D3D12 only, not Metal/macOS).";
		case NEXTGI_WC_REASON_UNSUPPORTED_API:
			return "NextGI world cache: keeping surface cache (UNSUPPORTED_API: World Cache is supported on Vulkan and D3D12 only).";
		case NEXTGI_WC_REASON_HIT_MATERIALS_UNAVAILABLE:
			return "NextGI world cache: keeping surface cache (HIT_MATERIALS_UNAVAILABLE: hit-material ray tracing pipeline is unavailable on this device).";
		case NEXTGI_WC_REASON_RAYGEN_UNAVAILABLE:
			return "NextGI world cache: keeping surface cache (RAYGEN_UNAVAILABLE: World Cache ray generation shader did not initialize).";
		case NEXTGI_WC_REASON_NO_CURRENT_RECORDS:
			return "NextGI world cache: waiting for current TLAS hit records.";
		case NEXTGI_WC_REASON_OFF:
		case NEXTGI_WC_REASON_ACTIVE:
		case NEXTGI_WC_REASON_MAX:
			break;
	}
	return "";
}

bool nextgi_world_cache_should_log_reason(NextGIWorldCacheReason p_reason, uint32_t &r_logged_mask) {
	if (p_reason == NEXTGI_WC_REASON_OFF || p_reason == NEXTGI_WC_REASON_ACTIVE || p_reason == NEXTGI_WC_REASON_NO_CURRENT_RECORDS || p_reason >= NEXTGI_WC_REASON_MAX) {
		return false;
	}
	const uint32_t bit = 1u << uint32_t(p_reason);
	if ((r_logged_mask & bit) != 0u) {
		return false;
	}
	r_logged_mask |= bit;
	return true;
}

NextGIWorldCacheVerdict nextgi_world_cache_evaluate(
		bool p_requested,
		bool p_hwrt_tier,
		bool p_software_tier,
		const String &p_api_name,
		bool p_is_metal,
		NextGIHitMode p_hit_mode,
		NextGIHitAvailability p_consumer_availability,
		bool p_raygen_ready,
		bool p_has_current_records,
		bool p_require_current_records) {
	NextGIWorldCacheVerdict verdict;
	verdict.requested = p_requested;
	if (!p_requested) {
		verdict.reason = NEXTGI_WC_REASON_OFF;
		return verdict;
	}
	// Unsupported case (c): Metal / macOS is not supported for World Cache (§26.2).
	if (p_is_metal || nextgi_world_cache_api_is_metal(p_api_name)) {
		verdict.reason = NEXTGI_WC_REASON_METAL_UNSUPPORTED;
		return verdict;
	}
	// Unsupported case (a): Software tier (SWRT) or non-HWRT tier keeps the Surface Cache.
	if (!p_hwrt_tier) {
		verdict.reason = p_software_tier ? NEXTGI_WC_REASON_SOFTWARE_TIER : NEXTGI_WC_REASON_NOT_HWRT;
		return verdict;
	}
	// Only Vulkan and D3D12 are supported graphics APIs for World Cache (§26.2).
	if (!nextgi_world_cache_api_supported(p_api_name)) {
		verdict.reason = NEXTGI_WC_REASON_UNSUPPORTED_API;
		return verdict;
	}
	// Unsupported case (b): Inline HWRT mode builds no hit pipeline.
	if (p_hit_mode == NEXTGI_HIT_MODE_INLINE) {
		verdict.reason = NEXTGI_WC_REASON_INLINE_HWRT;
		return verdict;
	}
	// Gate on NextGI::get_hit_availability_for(NEXTGI_HIT_CONSUMER_WORLD_CACHE).
	if (p_consumer_availability != NEXTGI_HIT_AVAILABLE) {
		if (p_consumer_availability == NEXTGI_HIT_HWRT_MODE_INLINE) {
			verdict.reason = NEXTGI_WC_REASON_INLINE_HWRT;
		} else {
			verdict.reason = NEXTGI_WC_REASON_HIT_MATERIALS_UNAVAILABLE;
		}
		return verdict;
	}
	if (!p_raygen_ready) {
		verdict.reason = NEXTGI_WC_REASON_RAYGEN_UNAVAILABLE;
		return verdict;
	}
	// World Cache replaces the Surface Cache whenever the configuration and hit pipeline support it.
	verdict.replaces_src = true;
	if (p_require_current_records && !p_has_current_records) {
		verdict.active = false;
		verdict.reason = NEXTGI_WC_REASON_NO_CURRENT_RECORDS;
		return verdict;
	}
	verdict.active = true;
	verdict.reason = NEXTGI_WC_REASON_ACTIVE;
	return verdict;
}

uint32_t nextgi_wc_normal_face(const Vector3 &p_normal) {
	const float ax = Math::abs(p_normal.x);
	const float ay = Math::abs(p_normal.y);
	const float az = Math::abs(p_normal.z);
	if (ax >= ay && ax >= az) {
		return p_normal.x >= 0.0f ? 0u : 1u;
	}
	if (ay >= az) {
		return p_normal.y >= 0.0f ? 2u : 3u;
	}
	return p_normal.z >= 0.0f ? 4u : 5u;
}

uint32_t nextgi_wc_level_for_distance(float p_distance) {
	if (p_distance < 12.0f) {
		return 0u;
	}
	if (p_distance < 28.0f) {
		return 1u;
	}
	if (p_distance < 64.0f) {
		return 2u;
	}
	if (p_distance < 140.0f) {
		return 3u;
	}
	return 4u;
}

float nextgi_wc_cell_size(uint32_t p_level) {
	return 0.5f * float(1u << MIN(p_level, uint32_t(NEXTGI_WC_LEVELS - 1)));
}

Vector3i nextgi_wc_quantize(const Vector3 &p_world, uint32_t p_level) {
	const float sz = nextgi_wc_cell_size(p_level);
	return Vector3i(
			int32_t(Math::floor(p_world.x / sz)),
			int32_t(Math::floor(p_world.y / sz)),
			int32_t(Math::floor(p_world.z / sz)));
}

uint32_t nextgi_wc_make_key(const Vector3i &p_cell, uint32_t p_level, uint32_t p_face) {
	const uint32_t lvl = MIN(p_level, uint32_t(NEXTGI_WC_LEVELS - 1));
	const uint32_t face = MIN(p_face, 5u);
	const uint32_t tag = 1u + lvl * 6u + face;
	const uint32_t cx = uint32_t(p_cell.x) & 0xFFu;
	const uint32_t cy = uint32_t(p_cell.y) & 0xFFu;
	const uint32_t cz = uint32_t(p_cell.z) & 0xFFu;
	return cx | (cy << 8u) | (cz << 16u) | (tag << 24u);
}

uint32_t nextgi_wc_hash_bucket(const Vector3i &p_cell, uint32_t p_level, uint32_t p_face) {
	uint32_t h = (uint32_t(p_cell.x) * 73856093u) ^
			(uint32_t(p_cell.y) * 19349663u) ^
			(uint32_t(p_cell.z) * 83492791u) ^
			(p_level * 2654435761u) ^
			(p_face * 2246822519u);
	h ^= h >> 16u;
	h *= 0x7feb352du;
	h ^= h >> 15u;
	h *= 0x846ca68bu;
	h ^= h >> 16u;
	return h & uint32_t(NEXTGI_WC_BUCKET_MASK);
}

uint32_t nextgi_wc_probe_stride(const Size2i &p_screen_size) {
	const int32_t max_dim = MAX(p_screen_size.x, p_screen_size.y);
	if (max_dim <= 512) {
		return 4u;
	}
	return 8u;
}

Size2i nextgi_wc_probe_grid_size(const Size2i &p_screen_size, uint32_t p_stride) {
	const uint32_t stride = MAX(p_stride, 1u);
	const uint32_t w = MAX((uint32_t(MAX(p_screen_size.x, 1)) + stride - 1u) / stride, 1u);
	const uint32_t h = MAX((uint32_t(MAX(p_screen_size.y, 1)) + stride - 1u) / stride, 1u);
	return Size2i(int32_t(w), int32_t(h));
}

/* NextGIWorldCacheCPUTable */

void NextGIWorldCacheCPUTable::init() {
	keys.resize_initialized(NEXTGI_WC_CAPACITY);
	radiance.resize_initialized(NEXTGI_WC_CAPACITY);
	accum.resize_initialized(NEXTGI_WC_CAPACITY);
	meta_frame.resize_initialized(NEXTGI_WC_CAPACITY);
	clear();
}

void NextGIWorldCacheCPUTable::clear() {
	if (keys.size() != NEXTGI_WC_CAPACITY) {
		init();
		return;
	}
	memset(keys.ptr(), 0, keys.size() * sizeof(uint32_t));
	memset(meta_frame.ptr(), 0, meta_frame.size() * sizeof(uint32_t));
	for (uint32_t i = 0; i < NEXTGI_WC_CAPACITY; i++) {
		radiance[i] = Radiance();
		accum[i] = Accum();
	}
	cells_inserted = 0;
	cells_updated = 0;
	cells_active = 0;
	cells_evicted = 0;
	hash_collisions = 0;
}

bool NextGIWorldCacheCPUTable::insert_and_accum(const Vector3 &p_world, const Vector3 &p_normal, const Vector3 &p_camera_pos, const Vector3 &p_radiance) {
	if (keys.size() != NEXTGI_WC_CAPACITY) {
		init();
	}
	if (!p_radiance.is_finite()) {
		return false;
	}
	const Vector3 clamped(
			CLAMP(p_radiance.x, 0.0f, NEXTGI_WC_MAX_RADIANCE),
			CLAMP(p_radiance.y, 0.0f, NEXTGI_WC_MAX_RADIANCE),
			CLAMP(p_radiance.z, 0.0f, NEXTGI_WC_MAX_RADIANCE));
	const float dist = p_world.distance_to(p_camera_pos);
	const uint32_t level = nextgi_wc_level_for_distance(dist);
	const uint32_t face = nextgi_wc_normal_face(p_normal);
	const Vector3i cell = nextgi_wc_quantize(p_world, level);
	const uint32_t key = nextgi_wc_make_key(cell, level, face);
	const uint32_t base_slot = nextgi_wc_hash_bucket(cell, level, face) * NEXTGI_WC_SLOTS_PER_BUCKET;

	uint32_t target_slot = UINT32_MAX;
	uint32_t first_empty = UINT32_MAX;
	for (uint32_t w = 0; w < NEXTGI_WC_SLOTS_PER_BUCKET; w++) {
		const uint32_t slot = base_slot + w;
		if (keys[slot] == key) {
			target_slot = slot;
			break;
		}
		if (keys[slot] == NEXTGI_WC_EMPTY_KEY && first_empty == UINT32_MAX) {
			first_empty = slot;
		}
	}
	if (target_slot == UINT32_MAX && first_empty != UINT32_MAX) {
		keys[first_empty] = key;
		target_slot = first_empty;
		cells_inserted++;
	}
	if (target_slot == UINT32_MAX) {
		hash_collisions++;
		return false;
	}
	accum[target_slot].r += uint32_t(Math::round(clamped.x * NEXTGI_WC_FIXED_SCALE));
	accum[target_slot].g += uint32_t(Math::round(clamped.y * NEXTGI_WC_FIXED_SCALE));
	accum[target_slot].b += uint32_t(Math::round(clamped.z * NEXTGI_WC_FIXED_SCALE));
	accum[target_slot].count++;
	return true;
}

void NextGIWorldCacheCPUTable::resolve_and_evict(uint32_t p_frame, uint32_t p_max_age) {
	if (keys.size() != NEXTGI_WC_CAPACITY) {
		init();
		return;
	}
	cells_updated = 0;
	cells_active = 0;
	cells_evicted = 0;
	for (uint32_t slot = 0; slot < NEXTGI_WC_CAPACITY; slot++) {
		Accum &acc = accum[slot];
		if (acc.count > 0u && keys[slot] != NEXTGI_WC_EMPTY_KEY) {
			const float inv = 1.0f / (NEXTGI_WC_FIXED_SCALE * float(acc.count));
			const Vector3 sample_rad(float(acc.r) * inv, float(acc.g) * inv, float(acc.b) * inv);
			Radiance &rad = radiance[slot];
			const float new_hist = MIN(rad.history + 1.0f, NEXTGI_WC_MAX_HISTORY);
			const float alpha = 1.0f / new_hist;
			rad.rgb = rad.history > 0.0f ? rad.rgb.lerp(sample_rad, alpha) : sample_rad;
			rad.history = new_hist;
			meta_frame[slot] = p_frame;
			acc = Accum();
			cells_updated++;
			cells_active++;
			continue;
		}
		if (acc.count > 0u) {
			acc = Accum();
		}
		if (keys[slot] == NEXTGI_WC_EMPTY_KEY) {
			continue;
		}
		const uint32_t last_frame = meta_frame[slot];
		const uint32_t age = p_frame >= last_frame ? (p_frame - last_frame) : 0u;
		if (age > p_max_age || (radiance[slot].history <= 0.0f && age > 2u)) {
			keys[slot] = NEXTGI_WC_EMPTY_KEY;
			radiance[slot] = Radiance();
			meta_frame[slot] = 0u;
			cells_evicted++;
		} else {
			cells_active++;
		}
	}
}

bool NextGIWorldCacheCPUTable::lookup(const Vector3 &p_world, const Vector3 &p_normal, const Vector3 &p_camera_pos, Vector3 &r_radiance, uint32_t *r_level) const {
	r_radiance = Vector3();
	if (keys.size() != NEXTGI_WC_CAPACITY) {
		return false;
	}
	const float dist = p_world.distance_to(p_camera_pos);
	const uint32_t base_level = nextgi_wc_level_for_distance(dist);
	const uint32_t face = nextgi_wc_normal_face(p_normal);
	for (uint32_t l = 0; l < 2u; l++) {
		const uint32_t level = base_level + l;
		if (level >= NEXTGI_WC_LEVELS) {
			break;
		}
		const Vector3i cell = nextgi_wc_quantize(p_world, level);
		const uint32_t key = nextgi_wc_make_key(cell, level, face);
		const uint32_t base_slot = nextgi_wc_hash_bucket(cell, level, face) * NEXTGI_WC_SLOTS_PER_BUCKET;
		for (uint32_t w = 0; w < NEXTGI_WC_SLOTS_PER_BUCKET; w++) {
			if (keys[base_slot + w] == key) {
				if (radiance[base_slot + w].history > 0.0f) {
					r_radiance = radiance[base_slot + w].rgb;
					if (r_level != nullptr) {
						*r_level = level;
					}
					return true;
				}
				return false;
			}
		}
	}
	return false;
}

/* NextGIWorldCache GPU manager */

static Mutex nextgi_wc_counter_mutex;
static HashMap<uint64_t, NextGIWorldCache *> nextgi_wc_counter_registry;
static uint64_t nextgi_wc_counter_next_token = 1;

struct NextGIWorldCache::ShaderState {
	NextgiLightGridShaderRD grid_shader;
	RID grid_version;
	RID grid_shader_rid;
	RID grid_pipeline;

	NextgiWorldCacheResolveShaderRD resolve_shader;
	RID resolve_version;
	RID resolve_shader_rid;
	RID resolve_pipeline;
	RID debug_vis_shader_rid;
	RID debug_vis_pipeline;
};

static void _store_projection_wc(const Projection &p_proj, float r_dst[16]) {
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			r_dst[c * 4 + r] = p_proj.columns[c][r];
		}
	}
}

NextGIWorldCache::~NextGIWorldCache() {
	free_resources();
}

void NextGIWorldCache::free_resources() {
	{
		MutexLock lock(nextgi_wc_counter_mutex);
		if (counter_token != 0) {
			nextgi_wc_counter_registry.erase(counter_token);
			counter_token = 0;
		}
		counters_pending = false;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (shaders != nullptr) {
		if (rd != nullptr) {
			if (shaders->grid_pipeline.is_valid()) {
				rd->free_rid(shaders->grid_pipeline);
			}
			if (shaders->resolve_pipeline.is_valid()) {
				rd->free_rid(shaders->resolve_pipeline);
			}
			if (shaders->debug_vis_pipeline.is_valid()) {
				rd->free_rid(shaders->debug_vis_pipeline);
			}
		}
		if (shaders->grid_version.is_valid()) {
			shaders->grid_shader.version_free(shaders->grid_version);
		}
		if (shaders->resolve_version.is_valid()) {
			shaders->resolve_shader.version_free(shaders->resolve_version);
		}
		memdelete(shaders);
		shaders = nullptr;
	}
	if (rd != nullptr) {
		const RID rids[] = {
			keys_buffer,
			radiance_buffer,
			accum_buffer,
			meta_buffer,
			params_buffer,
			counter_buffer,
			light_buffer,
			light_grid_buffer,
			light_grid_counter_buffer,
			blue_noise_texture,
			dummy_color,
			sampler_nearest,
			debug_texture,
			readback_buffer,
		};
		for (const RID &r : rids) {
			if (r.is_valid()) {
				rd->free_rid(r);
			}
		}
	}
	keys_buffer = RID();
	radiance_buffer = RID();
	accum_buffer = RID();
	meta_buffer = RID();
	params_buffer = RID();
	counter_buffer = RID();
	light_buffer = RID();
	light_grid_buffer = RID();
	light_grid_counter_buffer = RID();
	blue_noise_texture = RID();
	dummy_color = RID();
	sampler_nearest = RID();
	debug_texture = RID();
	debug_texture_size = Size2i();
	readback_buffer = RID();
	light_capacity = 0;
	table_cleared = false;
}

bool NextGIWorldCache::_ensure_shaders() {
	if (shaders != nullptr && shaders->grid_pipeline.is_valid() && shaders->resolve_pipeline.is_valid() && shaders->debug_vis_pipeline.is_valid()) {
		return true;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	if (shaders == nullptr) {
		shaders = memnew(ShaderState);
		shaders->grid_shader.initialize(Vector<String>({ "" }));
		shaders->grid_version = shaders->grid_shader.version_create(false);
		if (!shaders->grid_version.is_valid()) {
			free_resources();
			ERR_FAIL_V_MSG(false, "NextGI world cache: failed to compile light grid shader (nextgi_light_grid.glsl).");
		}
		shaders->grid_shader_rid = shaders->grid_shader.version_get_shader(shaders->grid_version, 0);
		shaders->grid_pipeline = rd->compute_pipeline_create(shaders->grid_shader_rid);

		Vector<String> resolve_defines;
		resolve_defines.push_back("");
		resolve_defines.push_back("#define MODE_DEBUG_VIS\n");
		shaders->resolve_shader.initialize(resolve_defines);
		shaders->resolve_version = shaders->resolve_shader.version_create(false);
		if (!shaders->resolve_version.is_valid()) {
			free_resources();
			ERR_FAIL_V_MSG(false, "NextGI world cache: failed to compile resolve shader (nextgi_world_cache_resolve.glsl).");
		}
		shaders->resolve_shader_rid = shaders->resolve_shader.version_get_shader(shaders->resolve_version, 0);
		shaders->resolve_pipeline = rd->compute_pipeline_create(shaders->resolve_shader_rid);
		shaders->debug_vis_shader_rid = shaders->resolve_shader.version_get_shader(shaders->resolve_version, 1);
		shaders->debug_vis_pipeline = rd->compute_pipeline_create(shaders->debug_vis_shader_rid);
		if (shaders->grid_pipeline.is_null() || shaders->resolve_pipeline.is_null() || shaders->debug_vis_pipeline.is_null()) {
			free_resources();
			ERR_FAIL_V_MSG(false, "NextGI world cache: failed to create compute pipelines.");
		}
	}
	return true;
}

bool NextGIWorldCache::_ensure_buffers(uint32_t p_light_count) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	if (keys_buffer.is_null()) {
		keys_buffer = rd->storage_buffer_create(uint32_t(NEXTGI_WC_CAPACITY) * uint32_t(sizeof(uint32_t)));
		radiance_buffer = rd->storage_buffer_create(uint32_t(NEXTGI_WC_CAPACITY) * uint32_t(sizeof(float) * 4));
		accum_buffer = rd->storage_buffer_create(uint32_t(NEXTGI_WC_CAPACITY) * uint32_t(sizeof(uint32_t) * 4));
		meta_buffer = rd->storage_buffer_create(uint32_t(NEXTGI_WC_CAPACITY) * uint32_t(sizeof(uint32_t)));
		table_cleared = false;
	}
	if (params_buffer.is_null()) {
		params_buffer = rd->storage_buffer_create(uint32_t(sizeof(NextGIWorldCacheParamsGPU)));
	}
	if (counter_buffer.is_null()) {
		counter_buffer = rd->storage_buffer_create(uint32_t(sizeof(NextGIWorldCacheCountersGPU)));
	}
	const uint32_t lights = MAX(p_light_count, 1u);
	if (light_buffer.is_null() || light_capacity < lights) {
		if (light_buffer.is_valid()) {
			rd->free_rid(light_buffer);
		}
		light_capacity = MAX(lights, light_capacity * 2u);
		light_buffer = rd->storage_buffer_create(light_capacity * uint32_t(sizeof(NextGIHitLightGPU)));
	}
	if (light_grid_buffer.is_null()) {
		light_grid_buffer = rd->storage_buffer_create(uint32_t(NEXTGI_LIGHT_GRID_CELLS) * uint32_t(NEXTGI_LIGHT_GRID_CELL_WORDS) * uint32_t(sizeof(uint32_t)));
	}
	if (light_grid_counter_buffer.is_null()) {
		light_grid_counter_buffer = rd->storage_buffer_create(uint32_t(sizeof(NextGIHitLightCountersGPU)));
	}
	if (blue_noise_texture.is_null()) {
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
	if (dummy_color.is_null()) {
		RD::TextureFormat df;
		df.width = 1;
		df.height = 1;
		df.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		df.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		dummy_color = rd->texture_create(df, RD::TextureView());
		if (dummy_color.is_valid()) {
			rd->texture_clear(dummy_color, Color(0, 0, 0, 1), 0, 1, 0, 1);
		}
	}
	if (sampler_nearest.is_null()) {
		RD::SamplerState state;
		state.mag_filter = RD::SAMPLER_FILTER_NEAREST;
		state.min_filter = RD::SAMPLER_FILTER_NEAREST;
		sampler_nearest = rd->sampler_create(state);
	}
	return keys_buffer.is_valid() && radiance_buffer.is_valid() && accum_buffer.is_valid() && meta_buffer.is_valid() &&
			params_buffer.is_valid() && counter_buffer.is_valid() && light_buffer.is_valid() &&
			light_grid_buffer.is_valid() && light_grid_counter_buffer.is_valid() && blue_noise_texture.is_valid() &&
			dummy_color.is_valid() && sampler_nearest.is_valid();
}

bool NextGIWorldCache::_ensure_debug_texture(const Size2i &p_size) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	if (p_size.x <= 0 || p_size.y <= 0) {
		return false;
	}
	if (debug_texture.is_valid() && debug_texture_size == p_size) {
		return true;
	}
	if (debug_texture.is_valid()) {
		rd->free_rid(debug_texture);
		debug_texture = RID();
	}
	RD::TextureFormat tf;
	tf.width = uint32_t(p_size.x);
	tf.height = uint32_t(p_size.y);
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	debug_texture = rd->texture_create(tf, RD::TextureView());
	if (debug_texture.is_null()) {
		debug_texture_size = Size2i();
		return false;
	}
	rd->texture_clear(debug_texture, Color(0, 0, 0, 1), 0, 1, 0, 1);
	debug_texture_size = p_size;
	return true;
}

uint32_t NextGIWorldCache::_upload_lights(RenderDataRD *p_render_data, NextGIRTScene *p_rt_scene) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr) {
		return 0;
	}
	LocalVector<NextGIHitLightGPU> gpu_lights;
	nextgi_collect_direct_lights(p_render_data, p_rt_scene, gpu_lights);
	const uint32_t count = gpu_lights.size();
	if (!_ensure_buffers(MAX(count, 1u))) {
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

bool NextGIWorldCache::_dispatch_light_grid(const Vector3 &p_camera_pos, float p_rt_radius, uint32_t p_light_count) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	const float radius = MAX(p_rt_radius, 1.0f);
	const Vector3 grid_min = p_camera_pos - Vector3(radius, radius, radius);
	const float cell_size = (2.0f * radius) / float(NEXTGI_LIGHT_GRID_DIM);

	NextGIHitLightCountersGPU zero_grid_counters;
	rd->buffer_update(light_grid_counter_buffer, 0, sizeof(zero_grid_counters), &zero_grid_counters);

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
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, light_grid_counter_buffer),
	};
	const RID grid_set = rd->uniform_set_create(VectorView<RD::Uniform>(grid_uniforms, 3), shaders->grid_shader_rid, 0);
	ERR_FAIL_COND_V(grid_set.is_null(), false);
	rd->draw_command_begin_label("NextGI World Cache Light Grid");
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

bool NextGIWorldCache::_dispatch_resolve(uint32_t p_frame, bool p_clear_all) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	struct ResolvePush {
		uint32_t frame;
		uint32_t max_age;
		uint32_t clear_all;
		uint32_t pad;
	};
	static_assert(sizeof(ResolvePush) == 16, "ResolvePush must be 16 bytes.");
	ResolvePush push = { p_frame, uint32_t(NEXTGI_WC_MAX_AGE), p_clear_all ? 1u : 0u, 0u };

	RD::Uniform resolve_uniforms[5] = {
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, keys_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, radiance_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, accum_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, meta_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, counter_buffer),
	};
	const RID resolve_set = rd->uniform_set_create(VectorView<RD::Uniform>(resolve_uniforms, 5), shaders->resolve_shader_rid, 0);
	ERR_FAIL_COND_V(resolve_set.is_null(), false);
	if (p_clear_all) {
		rd->draw_command_begin_label("NextGI World Cache Clear");
	} else {
		rd->draw_command_begin_label("NextGI World Cache Resolve");
	}
	const RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, shaders->resolve_pipeline);
	rd->compute_list_bind_uniform_set(list, resolve_set, 0);
	rd->compute_list_set_push_constant(list, &push, sizeof(push));
	rd->compute_list_dispatch(list, uint32_t(NEXTGI_WC_CAPACITY) / 64u, 1u, 1u);
	rd->compute_list_end();
	rd->draw_command_end_label();
	rd->free_rid(resolve_set);
	return true;
}

void NextGIWorldCache::_dispatch_debug_vis(RID p_depth, RID p_normal_roughness, const Size2i &p_screen_size) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr || !_ensure_debug_texture(p_screen_size)) {
		return;
	}
	RD::Uniform debug_uniforms[6] = {
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, params_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, keys_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, radiance_buffer),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ sampler_nearest, p_depth })),
		RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ sampler_nearest, p_normal_roughness })),
		RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, debug_texture),
	};
	const RID debug_set = rd->uniform_set_create(VectorView<RD::Uniform>(debug_uniforms, 6), shaders->debug_vis_shader_rid, 0);
	if (debug_set.is_null()) {
		return;
	}
	rd->draw_command_begin_label("NextGI World Cache Debug");
	const RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, shaders->debug_vis_pipeline);
	rd->compute_list_bind_uniform_set(list, debug_set, 0);
	rd->compute_list_dispatch(list, MAX((uint32_t(p_screen_size.x) + 7u) / 8u, 1u), MAX((uint32_t(p_screen_size.y) + 7u) / 8u, 1u), 1u);
	rd->compute_list_end();
	rd->draw_command_end_label();
	rd->free_rid(debug_set);
}

void NextGIWorldCache::clear_table() {
	table_cleared = false;
}

bool NextGIWorldCache::update(RenderDataRD *p_render_data, NextGIRTScene *p_rt_scene, NextGIHitMaterials *p_hit_materials, RID p_normal_roughness, bool p_debug_vis) {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	RendererSceneRenderRD *scene = RendererSceneRenderRD::get_singleton();
	if (rd == nullptr || scene == nullptr || p_render_data == nullptr || p_render_data->scene_data == nullptr ||
			p_rt_scene == nullptr || p_hit_materials == nullptr || !p_rt_scene->is_tlas_traceable() ||
			!p_hit_materials->can_trace_world_cache()) {
		return false;
	}
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	if (rb.is_null()) {
		return false;
	}
	const RID depth = rb->get_depth_texture();
	if (depth.is_null() || p_normal_roughness.is_null()) {
		return false;
	}
	const Size2i screen = rb->get_internal_size();
	if (screen.x <= 0 || screen.y <= 0) {
		return false;
	}
	if (!_ensure_shaders() || !_ensure_buffers(MAX(light_capacity, 1u))) {
		return false;
	}

	if (!table_cleared) {
		if (!_dispatch_resolve(frame, true)) {
			return false;
		}
		table_cleared = true;
	}

	const uint32_t light_count = _upload_lights(p_render_data, p_rt_scene);
	const RenderSceneDataRD *scene_data = p_render_data->scene_data;
	const Vector3 cam_pos = scene_data->cam_transform.origin;
	const float rt_radius_setting = float(GLOBAL_GET("rendering/global_illumination/nextgi/rt_scene_radius"));
	const float rt_radius = rt_radius_setting > 0.0f ? rt_radius_setting : 200.0f;

	if (!_dispatch_light_grid(cam_pos, rt_radius, light_count)) {
		return false;
	}

	RID sky_tex = dummy_color;
	bool sky_enabled = false;
	Color ambient(0, 0, 0, 1);
	if (p_render_data->environment.is_valid()) {
		ambient = scene->environment_get_ambient_light(p_render_data->environment);
		ambient *= scene->environment_get_ambient_light_energy(p_render_data->environment);
		const RID sky = scene->environment_get_sky(p_render_data->environment);
		SkyRD *sky_rd = scene->get_sky();
		if (sky.is_valid() && sky_rd != nullptr && scene->environment_get_nextgi_read_sky_light(p_render_data->environment)) {
			const RID radiance = sky_rd->sky_get_radiance_texture_2d(sky);
			if (radiance.is_valid()) {
				sky_tex = radiance;
				sky_enabled = true;
			}
		}
	}

	const uint32_t probe_stride = nextgi_wc_probe_stride(screen);
	const Size2i probe_grid = nextgi_wc_probe_grid_size(screen, probe_stride);
	const Vector3 grid_min = cam_pos - Vector3(rt_radius, rt_radius, rt_radius);
	const float cell_size = (2.0f * rt_radius) / float(NEXTGI_LIGHT_GRID_DIM);
	const float inv_cell_size = 1.0f / MAX(cell_size, 1e-4f);

	NextGIWorldCacheParamsGPU params;
	_store_projection_wc(NextGIView::inv_view_projection(*scene_data), params.inv_view_proj);
	params.camera_pos_radius[0] = cam_pos.x;
	params.camera_pos_radius[1] = cam_pos.y;
	params.camera_pos_radius[2] = cam_pos.z;
	params.camera_pos_radius[3] = rt_radius;
	params.grid_min_inv_cell[0] = grid_min.x;
	params.grid_min_inv_cell[1] = grid_min.y;
	params.grid_min_inv_cell[2] = grid_min.z;
	params.grid_min_inv_cell[3] = inv_cell_size;
	params.ambient_color_sky[0] = ambient.r;
	params.ambient_color_sky[1] = ambient.g;
	params.ambient_color_sky[2] = ambient.b;
	params.ambient_color_sky[3] = sky_enabled ? 1.0f : 0.0f;
	NextGIViewMath::store_basis(scene_data->cam_transform.basis, params.view_to_world);
	params.counts0[0] = uint32_t(screen.x);
	params.counts0[1] = uint32_t(screen.y);
	params.counts0[2] = uint32_t(probe_grid.x);
	params.counts0[3] = uint32_t(probe_grid.y);
	params.counts1[0] = frame;
	params.counts1[1] = light_count;
	params.counts1[2] = probe_stride;
	params.counts1[3] = 0u;
	rd->buffer_update(params_buffer, 0, sizeof(params), &params);

	NextGIWorldCacheCountersGPU zero_counters;
	rd->buffer_update(counter_buffer, 0, sizeof(zero_counters), &zero_counters);

	NextGIHitWorldCacheTrace trace;
	trace.params = params_buffer;
	trace.keys = keys_buffer;
	trace.radiance = radiance_buffer;
	trace.accum = accum_buffer;
	trace.counters = counter_buffer;
	trace.lights = light_buffer;
	trace.light_grid = light_grid_buffer;
	trace.sky = sky_tex;
	trace.blue_noise = blue_noise_texture;
	trace.depth = depth;
	trace.normal_roughness = p_normal_roughness;
	trace.grid_w = uint32_t(probe_grid.x);
	trace.grid_h = uint32_t(probe_grid.y);
	trace.time = float(scene_data->time);

	if (!p_hit_materials->trace_world_cache(p_rt_scene, trace)) {
		return false;
	}
	if (!_dispatch_resolve(frame, false)) {
		return false;
	}
	if (p_debug_vis) {
		_dispatch_debug_vis(depth, p_normal_roughness, screen);
	}

	counters.dispatches++;
	counters.last_light_count = light_count;
	counters.last_probe_stride = probe_stride;
	_request_counters();
	frame++;
	return true;
}

void NextGIWorldCache::_request_counters() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr || counter_buffer.is_null()) {
		return;
	}
	uint64_t token = 0;
	{
		MutexLock lock(nextgi_wc_counter_mutex);
		if (counters_pending) {
			return;
		}
		counters_pending = true;
		token = nextgi_wc_counter_next_token++;
		counter_token = token;
		nextgi_wc_counter_registry.insert(token, this);
	}
	if (readback_buffer.is_null()) {
		readback_buffer = rd->storage_buffer_create(uint32_t(sizeof(NextGIWorldCacheCountersGPU)));
	}
	Error err = readback_buffer.is_valid() ? OK : FAILED;
	if (err == OK) {
		rd->buffer_copy(counter_buffer, readback_buffer, 0, 0, uint32_t(sizeof(NextGIWorldCacheCountersGPU)));
		err = rd->buffer_get_data_async(readback_buffer, callable_mp_static(&NextGIWorldCache::_counters_received).bind(token), 0, uint32_t(sizeof(NextGIWorldCacheCountersGPU)));
	}
	if (err != OK) {
		MutexLock lock(nextgi_wc_counter_mutex);
		nextgi_wc_counter_registry.erase(token);
		counter_token = 0;
		counters_pending = false;
	}
}

void NextGIWorldCache::_counters_received(const Vector<uint8_t> &p_data, uint64_t p_token) {
	MutexLock lock(nextgi_wc_counter_mutex);
	NextGIWorldCache **owner = nextgi_wc_counter_registry.getptr(p_token);
	if (owner == nullptr) {
		return;
	}
	NextGIWorldCache *self = *owner;
	nextgi_wc_counter_registry.erase(p_token);
	self->counter_token = 0;
	self->counters_pending = false;
	if (p_data.size() < int(sizeof(NextGIWorldCacheCountersGPU))) {
		return;
	}
	NextGIWorldCacheCountersGPU got;
	memcpy(&got, p_data.ptr(), sizeof(got));
	self->counters.last_paths_traced = got.paths_traced;
	self->counters.last_primary_hits = got.primary_hits;
	self->counters.last_bounce_hits = got.bounce_hits;
	self->counters.last_cells_inserted = got.cells_inserted;
	self->counters.last_cells_updated = got.cells_updated;
	self->counters.last_cells_active = got.cells_active;
	self->counters.last_cells_evicted = got.cells_evicted;
	self->counters.last_hash_collisions = got.hash_collisions;
	self->counters.last_shadow_rays = got.shadow_rays;
}

uint64_t NextGIWorldCache::get_allocated_bytes() const {
	if (!is_ready()) {
		return 0;
	}
	// keys (4B) + radiance (16B) + accum (16B) + meta (4B) = 40B/slot + light grid + light buffer + debug texture.
	uint64_t bytes = uint64_t(NEXTGI_WC_CAPACITY) * 40ull;
	bytes += uint64_t(NEXTGI_LIGHT_GRID_CELLS) * uint64_t(NEXTGI_LIGHT_GRID_CELL_WORDS) * sizeof(uint32_t);
	bytes += uint64_t(light_capacity) * sizeof(NextGIHitLightGPU);
	if (debug_texture.is_valid()) {
		bytes += uint64_t(MAX(debug_texture_size.x, 0)) * uint64_t(MAX(debug_texture_size.y, 0)) * 8ull;
	}
	return bytes;
}

void NextGIWorldCache::fill_info(Dictionary &r_info) const {
	MutexLock lock(nextgi_wc_counter_mutex);
	r_info["world_cache_bytes"] = int64_t(get_allocated_bytes());
	r_info["world_cache_capacity"] = int64_t(NEXTGI_WC_CAPACITY);
	r_info["world_cache_dispatches"] = int64_t(counters.dispatches);
	r_info["world_cache_paths_traced"] = int64_t(counters.last_paths_traced);
	r_info["world_cache_primary_hits"] = int64_t(counters.last_primary_hits);
	r_info["world_cache_bounce_hits"] = int64_t(counters.last_bounce_hits);
	r_info["world_cache_cells_inserted"] = int64_t(counters.last_cells_inserted);
	r_info["world_cache_cells_updated"] = int64_t(counters.last_cells_updated);
	r_info["world_cache_cells_active"] = int64_t(counters.last_cells_active);
	r_info["world_cache_cells_evicted"] = int64_t(counters.last_cells_evicted);
	r_info["world_cache_hash_collisions"] = int64_t(counters.last_hash_collisions);
	r_info["world_cache_shadow_rays"] = int64_t(counters.last_shadow_rays);
	r_info["world_cache_light_count"] = int64_t(counters.last_light_count);
	r_info["world_cache_probe_stride"] = int64_t(counters.last_probe_stride);
}

void NextGIWorldCache::fill_counters(Dictionary &r_counters) const {
	fill_info(r_counters);
}

} // namespace RendererRD
