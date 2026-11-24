/**************************************************************************/
/*  next_gi_rt_shadows.h                                                  */
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

#include "core/math/aabb.h"
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/safe_refcount.h"
#include "core/templates/vector.h"
#include "servers/rendering/renderer_rd/storage_rd/render_buffer_custom_data_rd.h"
#include "servers/rendering/rendering_server_enums.h"
#include "servers/rendering/storage/virtual_shadow_map_storage.h"

class RenderDataRD;
class RenderSceneBuffersRD;

namespace RendererRD {

class NextGI;
class NextGIDenoise;
class NextGIRTScene;

// GodotNext: rt_shadows (#141, plan §21.3 contract M9, GM-3, GM-4, budget M-7).
// Standalone HWRT ray-traced shadows: per-light override, no light cap, tile-sparse
// 16x16 R8 shadow-mask atlas, translucent-as-opaque casters (LQ6), alpha-tested casters
// via #145/#130, and per-light temporal + 2x spatial denoising via NextGIDenoise::shadow_denoise().
// Everything above NextGIRTShadowsView is pure CPU logic so doctests run without a GPU.

static constexpr uint32_t NEXTGI_RT_SHADOW_TILE_SIZE = 16u;
static constexpr uint32_t NEXTGI_RT_SHADOW_ATLAS_TILES_X = 256u;
static constexpr uint32_t NEXTGI_RT_SHADOW_TILE_NONE = 0xFFFFFFFFu;
static constexpr uint32_t NEXTGI_RT_SHADOW_FLAG_SAMPLE = 1u << 0u;
static constexpr float NEXTGI_RT_SHADOW_MAX_ORIGIN_OFFSET = 0.05f; // Plan §8.2: clamped <= 5 cm.
static constexpr float NEXTGI_RT_SHADOW_HIT_MISS = 65504.0f;

enum NextGIRTShadowLightType : uint32_t {
	NEXTGI_RT_SHADOW_TYPE_DIRECTIONAL = 0u,
	NEXTGI_RT_SHADOW_TYPE_OMNI = 1u,
	NEXTGI_RT_SHADOW_TYPE_SPOT = 2u,
	NEXTGI_RT_SHADOW_TYPE_AREA = 3u,
};

// Preset tile budget: High = 65,536 tiles (16 MiB R8), scalable by quality preset.
constexpr uint32_t nextgi_rt_shadow_preset_tile_budget(RendererVirtualShadowMapStorage::PoolQuality p_quality) {
	return p_quality == RendererVirtualShadowMapStorage::POOL_QUALITY_LOW ? 16384u :
			(p_quality == RendererVirtualShadowMapStorage::POOL_QUALITY_MEDIUM ? 32768u :
			(p_quality == RendererVirtualShadowMapStorage::POOL_QUALITY_HIGH ? 65536u : 131072u));
}

// 2D atlas dimensions in texels for p_tile_budget 16x16 tiles (width = 256 tiles = 4096 px).
inline Size2i nextgi_rt_shadow_atlas_size(uint32_t p_tile_budget) {
	const uint32_t clamped = p_tile_budget > 0u ? p_tile_budget : 256u;
	const uint32_t tiles_y = (clamped + NEXTGI_RT_SHADOW_ATLAS_TILES_X - 1u) / NEXTGI_RT_SHADOW_ATLAS_TILES_X;
	return Size2i(int32_t(NEXTGI_RT_SHADOW_ATLAS_TILES_X * NEXTGI_RT_SHADOW_TILE_SIZE), int32_t(tiles_y * NEXTGI_RT_SHADOW_TILE_SIZE));
}

// Single R8 shadow-mask atlas byte footprint (High = 65,536 tiles = 16 MiB).
inline uint64_t nextgi_rt_shadow_r8_atlas_bytes(uint32_t p_tile_budget) {
	const Size2i sz = nextgi_rt_shadow_atlas_size(p_tile_budget);
	return uint64_t(sz.x) * uint64_t(sz.y);
}

// Plan §8.2 ray origin offset: 4 * eps_rt * scale, clamped <= 5 cm.
float nextgi_rt_shadow_origin_offset(float p_eps_rt, float p_scale, float p_linear_depth = 0.0f);

// Screen-space 16x16 tile rectangle for one light.
struct NextGIRTShadowTileRect {
	uint32_t min_tx = 0;
	uint32_t min_ty = 0;
	uint32_t width = 0;
	uint32_t height = 0;

	uint32_t tile_count() const { return width * height; }
	bool is_empty() const { return width == 0 || height == 0; }
};

NextGIRTShadowTileRect nextgi_rt_shadow_project_rect(
		NextGIRTShadowLightType p_type,
		const Vector3 &p_world_pos,
		float p_cull_radius,
		const Projection &p_view_proj,
		const Vector3 &p_cam_pos,
		float p_z_near,
		uint32_t p_tiles_x,
		uint32_t p_tiles_y);

// Pure CPU tile-sparse atlas budget and overflow fallback tracker (used by doctests and CPU pre-checks).
struct NextGIRTShadowBudget {
	uint32_t tile_budget = 65536u;
	uint32_t tiles_used = 0u;
	uint32_t lights_accepted = 0u;
	uint32_t overflow_tiles = 0u;
	uint32_t overflow_lights = 0u;
	uint32_t fallback_lights = 0u;

	explicit NextGIRTShadowBudget(uint32_t p_budget = 65536u) :
			tile_budget(p_budget) {}

	void reset(uint32_t p_budget);
	// Attempts to reserve p_light_tiles for a light. Returns true if the light fits within tile_budget,
	// or false if it overflows (incrementing overflow_lights/fallback_lights so the light falls back to VSM/atlas).
	bool try_allocate_light(uint32_t p_light_tiles);
};

// CPU twin of nextgi_denoise_shadow.glsl's temporal history validity check:
// invalidates history when the tile's light ID or screen tile changes, or on depth/normal disocclusion.
bool nextgi_rt_shadow_history_valid(
		uint32_t p_cur_stable_id,
		uint32_t p_prev_stable_id,
		uint32_t p_cur_packed_st,
		uint32_t p_prev_packed_st,
		float p_expected_z,
		float p_prev_z,
		const Vector3 &p_cur_normal,
		const Vector3 &p_prev_normal);

// CPU twin of subsurface transmittance thickness from ray hit distance + LIGHT_PARAM_TRANSMITTANCE_BIAS.
// Returns >= 0 when an occluder hit was recorded, or -1.0f on a miss.
float nextgi_rt_shadow_transmittance_thickness(float p_hit_dist, float p_transmittance_bias);

// CPU twin of nextgi_denoise_shadow.glsl MODE_SPATIAL penumbra width in screen pixels.
float nextgi_rt_shadow_penumbra_pixels(
		bool p_directional,
		float p_hit_dist,
		float p_light_size,
		float p_dist_to_light,
		float p_pixel_footprint);

// Per-view persistent GPU state attached to RenderSceneBuffersRD (complete per-view isolation).
class NextGIRTShadowsView : public RenderBufferCustomDataRD {
	GDCLASS(NextGIRTShadowsView, RenderBufferCustomDataRD);

public:
	// Async readback bridge: outlives the view if a callback arrives after viewport destruction.
	struct ReadbackState {
		SafeRefCount refs;
		SafeFlag alive;
		SafeFlag pending;
		LocalVector<RID> submitted_lights;
		uint32_t tiles_allocated = 0;
		uint32_t overflow_tiles = 0;
		uint32_t overflow_lights = 0;
		HashSet<RID> overflow_set;
		bool has_result = false;
	};

	RID mask_atlas[3]; // Ping-pong twin history [0..1] + raw/spatial scratch [2] (R8_UNORM).
	RID history_len[2]; // Twin history length atlases (R8_UINT).
	RID hit_dist_atlas; // Per-pixel ray hit distance (R16_SFLOAT).
	RID prev_depth_normal; // Previous frame view-space normal + linear depth (RGBA16F).

	RID tile_depth_minmax_buffer;
	RID lookup_buffer[2];
	uint32_t lookup_capacity[2] = { 0, 0 };
	RID tile_list_buffer;
	RID tile_owner_buffer[2];
	RID lights_buffer;
	uint32_t lights_capacity = 0;
	RID params_buffer;
	RID alloc_state_buffer;
	RID indirect_buffer;
	RID light_overflow_buffer;
	uint32_t light_overflow_capacity = 0;

	RID trace_uniform_set;
	uint64_t trace_uniform_key = 0;

	Size2i screen_size;
	uint32_t tile_budget = 0;
	uint32_t history_index = 0; // Active prev index; cur writes to history_index ^ 1.
	bool has_history = false;
	uint32_t frame_counter = 0;
	uint64_t last_frame = UINT64_MAX;

	HashMap<RID, uint32_t> light_stable_ids;
	uint32_t next_stable_id = 1;
	HashMap<uint32_t, uint32_t> prev_lookup_bases;
	HashMap<uint32_t, uint32_t> cur_lookup_bases;
	HashSet<RID> overflow_fallback_lights;
	uint32_t overflow_retry_countdown = 0;

	uint32_t last_tiles_allocated = 0;
	uint32_t last_overflow_tiles = 0;
	uint32_t last_overflow_lights = 0;
	uint64_t vram_bytes = 0;

	ReadbackState *readback = nullptr;

	void ensure_resources(const Size2i &p_screen_size, uint32_t p_tile_budget);
	void ensure_dynamic_buffers(uint32_t p_light_count, uint32_t p_lookup_words, uint32_t p_cur_slot);
	void poll_readback();
	void invalidate_trace_set();
	void free_resources();

	void configure(RenderSceneBuffersRD *p_render_buffers) override {}
	void free_data() override { free_resources(); }

	NextGIRTShadowsView();
	~NextGIRTShadowsView() override;
};

// Standalone HWRT ray-traced shadow system owned lazily by NextGI.
// Allocates 0 VRAM and initializes 0 shaders until a view actually registers RT-shadowed lights.
class NextGIRTShadows {
public:
	// std430 layout matching RTShadowLight in nextgi_rt_shadows_inc.glsl (144 bytes).
	struct LightGPU {
		float pos_range[4] = {};
		float dir_cos[4] = {};
		float area_width_size[4] = {};
		float area_height_trans[4] = {};
		float view_pos_radius[4] = {};
		float view_dir_sin[4] = {};
		uint32_t policy_lookup[4] = {};
		uint32_t rect_id[4] = {};
		float area_cull[4] = {};
	};
	static_assert(sizeof(LightGPU) == 144, "LightGPU must match RTShadowLight in nextgi_rt_shadows_inc.glsl.");

	// std430 layout matching RTShadowParams in nextgi_rt_shadows_inc.glsl (336 bytes).
	struct ParamsGPU {
		float inv_view_proj[16] = {};
		float prev_view_proj[16] = {};
		float inv_projection[16] = {};
		float view_to_world[12] = {};
		float depth_coefficients[4] = {};
		float prev_depth_coeffs[4] = {};
		float camera_pos_offset[4] = {};
		float footprint_misc[4] = {};
		uint32_t screen_tiles[4] = {};
		uint32_t counts[4] = {};
	};
	static_assert(sizeof(ParamsGPU) == 336, "ParamsGPU must match RTShadowParams in nextgi_rt_shadows_inc.glsl.");

	struct LightRegistration {
		RID light_instance;
		NextGIRTShadowLightType type = NEXTGI_RT_SHADOW_TYPE_DIRECTIONAL;
		Vector3 world_pos;
		Vector3 world_dir; // Directional: toward light; spot/area: shine direction.
		Vector3 view_pos;
		Vector3 view_dir;
		Vector3 area_width_world;
		Vector3 area_height_world;
		Vector2 area_size;
		float range = 0.0f; // Local: range; directional: max trace distance.
		float cull_radius = 0.0f;
		float source_radius = 0.0f; // Directional: tan(half angular diameter); local: LIGHT_PARAM_SIZE.
		float cos_spot_angle = -1.0f;
		float sin_spot_angle = 0.0f;
		float transmittance_bias = 0.0f;
		uint32_t caster_mask = 0xFFFFFFFFu;
	};

	struct ViewStats {
		uint32_t lights_traced = 0;
		uint32_t directional_lights = 0;
		uint32_t local_lights = 0;
		uint32_t tiles_allocated = 0;
		uint32_t tile_budget = 0;
		uint32_t overflow_tiles = 0;
		uint32_t overflow_lights = 0;
		uint32_t fallback_lights = 0;
		uint64_t atlas_vram_bytes = 0;
		bool retrace_active = false;
	};

	static Vector<String> get_alloc_shader_variant_defines();
	static Vector<String> get_trace_shader_variant_defines();

	// Opens per-view light registration before LightStorage::update_light_buffers().
	void begin_view(RenderDataRD *p_render_data, NextGIRTScene *p_rt_scene, uint32_t p_tile_budget);
	// Registers one RT-shadowed light for the active view. Returns lookup base (and sets r_flags)
	// when accepted, or NEXTGI_RT_SHADOW_TILE_NONE if the light is in overflow fallback or unavailable.
	uint32_t register_light(const LightRegistration &p_light, uint32_t &r_flags);
	// Whether a light is currently in overflow fallback for p_render_buffers (so it keeps VSM/atlas).
	static bool is_light_in_overflow_fallback(const Ref<RenderSceneBuffersRD> &p_render_buffers, RID p_light_instance);
	// Runs tile allocation, ray-query trace, optional #130 material retrace, and NextGIDenoise::shadow_denoise().
	void render(RenderDataRD *p_render_data, NextGI *p_next_gi, RID p_normal_roughness, RID p_velocity, bool p_velocity_enabled);
	void end_view();

	bool is_view_active() const { return view_active; }
	RID get_bound_mask_atlas() const { return bound_mask_atlas; }
	RID get_bound_tile_lookup() const { return bound_tile_lookup; }
	RID get_bound_hit_dist_atlas() const { return bound_hit_dist_atlas; }
	const ViewStats &get_last_stats() const { return last_stats; }
	uint64_t get_total_vram_bytes() const { return last_stats.atlas_vram_bytes; }

	NextGIRTShadows();
	~NextGIRTShadows();

private:
	struct Shaders;
	Shaders *shaders = nullptr;
	NextGIDenoise *denoiser = nullptr;
	RID blue_noise_texture;
	RID retrace_buffer;
	RID shadow_texture_set;
	RID shadow_texture_set_table;
	bool alloc_shader_ready = false;
	bool alloc_shader_failed = false;
	bool trace_shader_ready = false;
	bool trace_shader_failed = false;

	bool view_active = false;
	bool view_tlas_ready = false;
	Ref<NextGIRTShadowsView> active_view;
	NextGIRTScene *active_rt_scene = nullptr;
	Size2i active_screen_size;
	uint32_t active_tiles_x = 0;
	uint32_t active_tiles_y = 0;
	uint32_t active_tile_budget = 65536u;
	Projection active_view_proj;
	Vector3 active_cam_pos;
	float active_z_near = 0.05f;
	uint32_t active_lookup_cursor = 0;
	uint32_t active_max_rect_w = 0;
	uint32_t active_max_rect_h = 0;
	uint32_t active_dir_count = 0;
	uint32_t active_local_count = 0;
	uint32_t active_fallback_count = 0;
	LocalVector<LightGPU> active_lights;
	LocalVector<RID> active_light_rids;

	RID bound_mask_atlas;
	RID bound_tile_lookup;
	RID bound_hit_dist_atlas;
	ViewStats last_stats;

	bool _ensure_alloc_shaders();
	bool _ensure_trace_shaders(NextGI *p_next_gi);
	void _ensure_blue_noise();
	void _free_resources();
};

} // namespace RendererRD
