/**************************************************************************/
/*  next_gi_world_cache.h                                                 */
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

#pragma once

#include "core/math/color.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "core/math/vector3i.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/renderer_rd/environment/next_gi_hit_materials.h"

class RenderDataRD;

namespace RendererRD {

class NextGIRTScene;

// GodotNext: nextgi_world_cache (#294, plan §21.2.1 / §26.1 / §26.2).
// Runtime world-space lighting cache (UE 5.9 Preview, off by default) that replaces the Surface Cache
// (SRC) on the HWRT tier on Vulkan and D3D12 when the hit-material path (#130) is available and
// Hardware Ray Tracing Mode (#286) is Full or Simplified Material.
// Everything above NextGIWorldCache is device-free so doctests exercise policy, hashing, cascades,
// temporal blending, eviction, and fallback logging without a GPU.

enum {
	NEXTGI_WC_BUCKETS = 16384, // 2^14 buckets.
	NEXTGI_WC_BUCKET_MASK = NEXTGI_WC_BUCKETS - 1,
	NEXTGI_WC_SLOTS_PER_BUCKET = 8, // 8-way set-associative buckets.
	NEXTGI_WC_CAPACITY = NEXTGI_WC_BUCKETS * NEXTGI_WC_SLOTS_PER_BUCKET, // 131,072 slots.
	NEXTGI_WC_LEVELS = 5, // 0.5m, 1m, 2m, 4m, 8m camera-distance cascades.
	NEXTGI_WC_EMPTY_KEY = 0u,
	NEXTGI_WC_MAX_AGE = 32, // Evict cells untouched for > 32 frames.
	NEXTGI_WC_MAX_SHADOWED_LIGHTS = 2,
};

inline constexpr float NEXTGI_WC_MAX_HISTORY = 16.0f;
inline constexpr float NEXTGI_WC_FIXED_SCALE = 1024.0f;
inline constexpr float NEXTGI_WC_MAX_RADIANCE = 64.0f;

// Why the World Cache is active or fell back to the Surface Cache (SRC).
enum NextGIWorldCacheReason {
	NEXTGI_WC_REASON_OFF = 0, // Project setting rendering/global_illumination/nextgi/world_cache is false (default).
	NEXTGI_WC_REASON_ACTIVE = 1, // Active on HWRT + Vulkan/D3D12 + Full/Simplified Material + hit materials ready.
	NEXTGI_WC_REASON_SOFTWARE_TIER = 2, // Unsupported case (a): software ray tracing tier (SWRT).
	NEXTGI_WC_REASON_NOT_HWRT = 3, // NextGI disabled or unsupported renderer.
	NEXTGI_WC_REASON_INLINE_HWRT = 4, // Unsupported case (b): Hardware Ray Tracing Mode is Inline.
	NEXTGI_WC_REASON_METAL_UNSUPPORTED = 5, // Unsupported case (c): Metal / macOS (§26.2).
	NEXTGI_WC_REASON_UNSUPPORTED_API = 6, // Graphics API other than Vulkan or D3D12.
	NEXTGI_WC_REASON_HIT_MATERIALS_UNAVAILABLE = 7, // Hit-material path (#130) unavailable (no RT pipeline / library / bindless / init failed).
	NEXTGI_WC_REASON_RAYGEN_UNAVAILABLE = 8, // World Cache raygen stage unavailable.
	NEXTGI_WC_REASON_NO_CURRENT_RECORDS = 9, // Current frame's TLAS hit records not built yet.
	NEXTGI_WC_REASON_MAX,
};

struct NextGIWorldCacheVerdict {
	bool requested = false;
	bool active = false;
	// True whenever World Cache replaces the Surface Cache for the view: SRC is not captured,
	// not lit, and not allocated.
	bool replaces_src = false;
	NextGIWorldCacheReason reason = NEXTGI_WC_REASON_OFF;
};

bool nextgi_world_cache_api_is_metal(const String &p_api_name);
bool nextgi_world_cache_api_supported(const String &p_api_name);
const char *nextgi_world_cache_reason_name(NextGIWorldCacheReason p_reason);
const char *nextgi_world_cache_reason_message(NextGIWorldCacheReason p_reason);

// Returns true ONCE per session for each distinct unsupported fallback reason (bitmask tracked in r_logged_mask).
bool nextgi_world_cache_should_log_reason(NextGIWorldCacheReason p_reason, uint32_t &r_logged_mask);

// Evaluates World Cache availability and SRC replacement policy.
// When p_require_current_records is false (before the TLAS phase), evaluates whether World Cache replaces
// SRC for the frame/view. When true (before dispatching GPU traces), also gates on has_current_records().
NextGIWorldCacheVerdict nextgi_world_cache_evaluate(
		bool p_requested,
		bool p_hwrt_tier,
		bool p_software_tier,
		const String &p_api_name,
		bool p_is_metal,
		NextGIHitMode p_hit_mode,
		NextGIHitAvailability p_consumer_availability,
		bool p_raygen_ready,
		bool p_has_current_records = true,
		bool p_require_current_records = false);

// Pure CPU spatial hash, quantization, cascade, and probe-grid helpers matching nextgi_world_cache_common_inc.glsl bit-for-bit.
uint32_t nextgi_wc_normal_face(const Vector3 &p_normal);
uint32_t nextgi_wc_level_for_distance(float p_distance);
float nextgi_wc_cell_size(uint32_t p_level);
Vector3i nextgi_wc_quantize(const Vector3 &p_world, uint32_t p_level);
uint32_t nextgi_wc_make_key(const Vector3i &p_cell, uint32_t p_level, uint32_t p_face);
uint32_t nextgi_wc_hash_bucket(const Vector3i &p_cell, uint32_t p_level, uint32_t p_face);
uint32_t nextgi_wc_probe_stride(const Size2i &p_screen_size);
Size2i nextgi_wc_probe_grid_size(const Size2i &p_screen_size, uint32_t p_stride);

// CPU reference mirror of the sparse 8-way set-associative World Cache hash table for unit doctests.
struct NextGIWorldCacheCPUTable {
	struct Accum {
		uint32_t r = 0;
		uint32_t g = 0;
		uint32_t b = 0;
		uint32_t count = 0;
	};
	struct Radiance {
		Vector3 rgb;
		float history = 0.0f;
	};
	LocalVector<uint32_t> keys;
	LocalVector<Radiance> radiance;
	LocalVector<Accum> accum;
	LocalVector<uint32_t> meta_frame;

	uint32_t cells_inserted = 0;
	uint32_t cells_updated = 0;
	uint32_t cells_active = 0;
	uint32_t cells_evicted = 0;
	uint32_t hash_collisions = 0;

	void init();
	void clear();
	bool insert_and_accum(const Vector3 &p_world, const Vector3 &p_normal, const Vector3 &p_camera_pos, const Vector3 &p_radiance);
	void resolve_and_evict(uint32_t p_frame, uint32_t p_max_age = NEXTGI_WC_MAX_AGE);
	bool lookup(const Vector3 &p_world, const Vector3 &p_normal, const Vector3 &p_camera_pos, Vector3 &r_radiance, uint32_t *r_level = nullptr) const;
};

// std430 World Cache params (192 bytes, matches nextgi_world_cache_common_inc.glsl).
struct NextGIWorldCacheParamsGPU {
	float inv_view_proj[16] = {};
	float camera_pos_radius[4] = {};
	float grid_min_inv_cell[4] = {};
	float ambient_color_sky[4] = {};
	float view_to_world[12] = {};
	uint32_t counts0[4] = {}; // screen_w, screen_h, probe_grid_w, probe_grid_h
	uint32_t counts1[4] = {}; // frame, light_count, probe_stride, flags
};
static_assert(sizeof(NextGIWorldCacheParamsGPU) == 192, "NextGIWorldCacheParamsGPU must be 192 bytes (nextgi_world_cache_common_inc.glsl).");

// std430 World Cache GPU counters (64 bytes, matches nextgi_world_cache_common_inc.glsl).
struct NextGIWorldCacheCountersGPU {
	uint32_t paths_traced = 0;
	uint32_t primary_hits = 0;
	uint32_t bounce_hits = 0;
	uint32_t cells_inserted = 0;
	uint32_t cells_updated = 0;
	uint32_t cells_active = 0;
	uint32_t cells_evicted = 0;
	uint32_t hash_collisions = 0;
	uint32_t shadow_rays = 0;
	uint32_t lookup_hits = 0;
	uint32_t lookup_misses = 0;
	uint32_t sink = 0;
	uint32_t pad0 = 0;
	uint32_t pad1 = 0;
	uint32_t pad2 = 0;
	uint32_t pad3 = 0;
};
static_assert(sizeof(NextGIWorldCacheCountersGPU) == 64, "NextGIWorldCacheCountersGPU must be 64 bytes.");

// Lazily allocated GPU manager for the sparse World Cache. Never constructed when world_cache is off
// or when World Cache falls back to the Surface Cache.
class NextGIWorldCache {
public:
	struct Counters {
		uint64_t dispatches = 0;
		uint32_t last_paths_traced = 0;
		uint32_t last_primary_hits = 0;
		uint32_t last_bounce_hits = 0;
		uint32_t last_cells_inserted = 0;
		uint32_t last_cells_updated = 0;
		uint32_t last_cells_active = 0;
		uint32_t last_cells_evicted = 0;
		uint32_t last_hash_collisions = 0;
		uint32_t last_shadow_rays = 0;
		uint32_t last_light_count = 0;
		uint32_t last_probe_stride = 8;
	};

private:
	struct ShaderState;
	ShaderState *shaders = nullptr;

	RID keys_buffer;
	RID radiance_buffer;
	RID accum_buffer;
	RID meta_buffer;
	RID params_buffer;
	RID counter_buffer;
	RID light_buffer;
	RID light_grid_buffer;
	RID light_grid_counter_buffer;
	RID blue_noise_texture;
	RID dummy_color;
	RID sampler_nearest;
	RID debug_texture;
	Size2i debug_texture_size;
	RID readback_buffer;

	uint32_t light_capacity = 0;
	uint32_t frame = 0;
	bool table_cleared = false;
	uint64_t counter_token = 0;
	bool counters_pending = false;
	Counters counters;

	bool _ensure_shaders();
	bool _ensure_buffers(uint32_t p_light_count);
	bool _ensure_debug_texture(const Size2i &p_size);
	uint32_t _upload_lights(RenderDataRD *p_render_data, NextGIRTScene *p_rt_scene);
	bool _dispatch_light_grid(const Vector3 &p_camera_pos, float p_rt_radius, uint32_t p_light_count);
	bool _dispatch_resolve(uint32_t p_frame, bool p_clear_all);
	void _dispatch_debug_vis(RID p_depth, RID p_normal_roughness, const Size2i &p_screen_size);
	void _request_counters();
	static void _counters_received(const Vector<uint8_t> &p_data, uint64_t p_token);

public:
	bool update(RenderDataRD *p_render_data, NextGIRTScene *p_rt_scene, NextGIHitMaterials *p_hit_materials, RID p_normal_roughness, bool p_debug_vis);
	void clear_table();
	void free_resources();

	bool is_ready() const { return keys_buffer.is_valid() && radiance_buffer.is_valid() && table_cleared; }
	RID get_keys_buffer() const { return keys_buffer; }
	RID get_radiance_buffer() const { return radiance_buffer; }
	RID get_debug_texture() const { return debug_texture; }
	uint64_t get_allocated_bytes() const;

	const Counters &get_counters() const { return counters; }
	void fill_info(Dictionary &r_info) const;
	void fill_counters(Dictionary &r_counters) const;

	NextGIWorldCache() = default;
	~NextGIWorldCache();
};

} // namespace RendererRD
