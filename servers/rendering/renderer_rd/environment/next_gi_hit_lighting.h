/**************************************************************************/
/*  next_gi_hit_lighting.h                                                */
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

#include "core/math/basis.h"
#include "core/math/color.h"
#include "core/math/projection.h"
#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/renderer_rd/environment/next_gi_hit_materials.h"

class RenderDataRD;

namespace RendererRD {

class NextGIRTScene;
class NextGIRelight;
class NextGISRC;
class NextGIWRC;

// GodotNext: nextgi_hit_lighting (#129, plan §21.2 contract L7, GL-1 to GL-3).
// Sorted-deferred hit lighting for reflections (and modularly structured so GI hit lighting #128
// and World Cache #294 reuse the hit records, 16-bit counting sort, 64^3 world light grid, and
// raygen shading pipeline). Everything above NextGIHitLighting is device-free for CPU doctests.

enum NextGIRayLightingMode {
	NEXTGI_RAY_LIGHTING_SURFACE_CACHE = 0,
	NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS = 1,
	NEXTGI_RAY_LIGHTING_MAX,
};

enum NextGIEnvRayLightingMode {
	NEXTGI_ENV_RAY_LIGHTING_PROJECT_DEFAULT = 0,
	NEXTGI_ENV_RAY_LIGHTING_SURFACE_CACHE = 1,
	NEXTGI_ENV_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS = 2,
};

enum {
	NEXTGI_LIGHT_GRID_DIM = 64,
	NEXTGI_LIGHT_GRID_CELLS = 64 * 64 * 64,
	NEXTGI_LIGHT_GRID_CELL_CAP = 32,
	NEXTGI_LIGHT_GRID_CELL_WORDS = 17, // 1 count + 16 words holding 32 uint16 light indices.
	NEXTGI_HIT_LIGHT_MAX_SHADOWED_LIGHTS = 4,
	NEXTGI_HIT_LIGHT_GI_MAX_SHADOWED_LIGHTS = 1,
	NEXTGI_HIT_LIGHT_MAX_TRANSLUCENT_SKIPS = 2,
	NEXTGI_HIT_LIGHT_MIN_BOUNCES = 1,
	NEXTGI_HIT_LIGHT_MAX_BOUNCES = 4,
	NEXTGI_HIT_LIGHT_SORT_BINS = 65536,
	// Disjoint 16-bit material key ranges (#128): reflections [0x0000..0x7FFF], diffuse GI [0x8000..0xFFFF].
	NEXTGI_HL_KEY_REFLECTION_BASE = 0x0000,
	NEXTGI_HL_KEY_REFLECTION_FALLBACK = 0x7FFF,
	NEXTGI_HL_KEY_GI_BASE = 0x8000,
	NEXTGI_HL_KEY_GI_FALLBACK = 0xFFFF,
};

inline constexpr float NEXTGI_HIT_LIGHT_MIRROR_ROUGHNESS = 0.1f;
inline constexpr float NEXTGI_HIT_LIGHT_GI_HANDOFF_SCALE = 1.5f;

enum : uint32_t {
	NEXTGI_HL_RECORD_FLAG_FRONT_FACE = 1u << 24,
	NEXTGI_HL_RECORD_FLAG_TRANSLUCENT = 1u << 25,
	NEXTGI_HL_RECORD_FLAG_MIRROR = 1u << 26,
	NEXTGI_HL_RECORD_FLAG_DIFFUSE_GI = 1u << 27,
	NEXTGI_HL_LIGHT_FLAG_SHADOW = 1u << 0,
	NEXTGI_HL_LIGHT_FLAG_STATIC = 1u << 1,
	NEXTGI_HL_PARAM_FLAG_TRANSLUCENT = 1u << 0,
	NEXTGI_HL_PARAM_FLAG_REFLECT_DEBUG = 1u << 1,
	NEXTGI_HL_PARAM_FLAG_BLUE_NOISE = 1u << 2,
	NEXTGI_HL_PARAM_FLAG_SRC_INDIRECT = 1u << 3,
	// GodotNext: nextgi_world_cache (#294). World Cache replaces SRC as indirect source.
	NEXTGI_HL_PARAM_FLAG_WORLD_CACHE = 1u << 4,
};

NextGIRayLightingMode nextgi_ray_lighting_mode_from_setting(int p_setting);
const char *nextgi_ray_lighting_mode_name(NextGIRayLightingMode p_mode);
NextGIRayLightingMode nextgi_resolve_ray_lighting_mode(int p_project_setting, int p_env_override);
uint32_t nextgi_clamp_max_reflection_bounces(int p_setting);

struct NextGIHitLightingVerdict {
	NextGIRayLightingMode requested_mode = NEXTGI_RAY_LIGHTING_SURFACE_CACHE;
	NextGIRayLightingMode effective_mode = NEXTGI_RAY_LIGHTING_SURFACE_CACHE;
	bool active = false;
	bool unsupported_fallback = false;
	const char *reason = "SURFACE_CACHE";
};

// Evaluates whether hit lighting for reflections can run on this tier/device/mode.
// Falls back cleanly to Surface Cache with reason "HIT_LIGHTING_UNSUPPORTED" when requested
// on SWRT, without RT pipelines / pipeline libraries / bindless, or outside Full HWRT mode.
NextGIHitLightingVerdict nextgi_hit_lighting_evaluate(
		NextGIRayLightingMode p_requested,
		bool p_hwrt_tier,
		bool p_raytracing_pipeline,
		bool p_pipeline_library,
		bool p_bindless,
		NextGIHitMode p_hit_mode = NEXTGI_HIT_MODE_FULL,
		bool p_raygen_ready = true);

// GodotNext: nextgi_hit_lighting_gi (#128, plan §21.2 contract L7, gate GL-4).
// Evaluates whether hit lighting for screen-probe diffuse GI can run.
// Active ONLY when hit_lighting_gi is true, reflection hit lighting is active
// (ray_lighting_mode == hit_lighting_reflections + HWRT + RT pipelines/libraries/bindless + Full + raygen),
// AND resolved quality == EPIC.
struct NextGIHitLightingGIVerdict {
	bool requested = false;
	bool active = false;
	const char *reason = "OFF";
};

NextGIHitLightingGIVerdict nextgi_hit_lighting_gi_evaluate(
		bool p_hit_lighting_gi_setting,
		const NextGIHitLightingVerdict &p_reflection_verdict,
		bool p_epic_quality);

// Cutoff distance for diffuse GI hit lighting: 1.5x WRC handoff distance, clamped to rt_scene_radius.
_FORCE_INLINE_ float nextgi_hit_light_gi_cutoff(float p_wrc_handoff_distance, float p_rt_radius) {
	return p_wrc_handoff_distance > 0.0f ? MIN(NEXTGI_HIT_LIGHT_GI_HANDOFF_SCALE * p_wrc_handoff_distance, p_rt_radius) : p_rt_radius;
}

// std430 compact per-ray hit record (48 bytes, matches nextgi_hit_lighting_common_inc.glsl).
struct NextGIHitLightRecord {
	float origin[3] = {};
	float hit_t = 0.0f;
	uint32_t instance_flags = 0; // Bits 0..23: 24-bit instance index; bits 24..31: flags.
	uint32_t geometry_material = 0; // Bits 0..15: geometry index; bits 16..31: 16-bit material key.
	uint32_t primitive = 0;
	uint32_t pixel_packed = 0; // x | (y << 16).
	uint32_t bary_f16 = 0; // 2x f16 barycentrics.
	uint32_t ray_dir_oct = 0; // 2x snorm16 octahedral ray direction.
	uint32_t cone_f16 = 0; // 2x f16 ray cone (width, spread).
	uint32_t resume_bits = 0; // float bits of screen-trace resume distance.
};
static_assert(sizeof(NextGIHitLightRecord) == 48, "NextGIHitLightRecord must be 48 bytes (nextgi_hit_lighting_common_inc.glsl).");

struct NextGIHitLightRecordUnpacked {
	Vector3 origin;
	float hit_t = 0.0f;
	uint32_t instance_index = 0;
	uint32_t flags = 0;
	uint16_t geometry_index = 0;
	uint16_t material_key = 0;
	uint32_t primitive = 0;
	Vector2i pixel;
	Vector2 bary;
	Vector3 ray_dir;
	Vector2 cone;
	float resume_distance = 0.0f;
};

// 16-bit material sort keys partitioned into two disjoint ranges (#128):
//   Reflections: [0x0000..0x7FFF] (7-bit shader slot + 8-bit material id, fallback 0x7FFF).
//   Diffuse GI:  [0x8000..0xFFFF] (0x8000 | 7-bit shader slot + 8-bit material id, fallback 0xFFFF).
uint16_t nextgi_hit_light_material_key(uint32_t p_shader_slot, uint32_t p_material_id);
uint16_t nextgi_hit_light_gi_material_key(uint32_t p_shader_slot, uint32_t p_material_id);
_FORCE_INLINE_ bool nextgi_hit_light_key_is_gi(uint16_t p_key) {
	return p_key >= uint16_t(NEXTGI_HL_KEY_GI_BASE);
}
NextGIHitLightRecord nextgi_hit_light_pack_record(const NextGIHitLightRecordUnpacked &p_in);
NextGIHitLightRecordUnpacked nextgi_hit_light_unpack_record(const NextGIHitLightRecord &p_rec);

// CPU reference matching nextgi_hit_sort.glsl's 16-bit counting sort.
void nextgi_hit_light_counting_sort(const NextGIHitLightRecord *p_records, uint32_t p_count, LocalVector<uint32_t> &r_sorted_indices);

// std430 direct light entry (80 bytes, matches nextgi_hit_lighting_common_inc.glsl).
struct NextGIHitLightGPU {
	float color_energy[4] = {};
	float position_inv_range[4] = {};
	float direction_type[4] = {};
	float spot_atten_radius[4] = {}; // attenuation, cos_spot, inv_spot_atten, source_radius.
	uint32_t shadow_flags[4] = {}; // flags, #145 shadow light_mask_mode, caster_mask, pad.
};
static_assert(sizeof(NextGIHitLightGPU) == 80, "NextGIHitLightGPU must be 80 bytes (nextgi_hit_lighting_common_inc.glsl).");

// std430 GPU counters (64 bytes, matches nextgi_hit_lighting_common_inc.glsl).
struct NextGIHitLightCountersGPU {
	uint32_t marked_hits = 0;
	uint32_t sorted_hits = 0;
	uint32_t shadow_rays = 0;
	uint32_t unshadowed_lights = 0;
	uint32_t bounce_rays = 0;
	uint32_t translucent_skips = 0;
	uint32_t light_grid_overflow = 0;
	uint32_t sink = 0;
	uint32_t refl_sorted_hits = 0;
	uint32_t gi_marked_hits = 0;
	uint32_t gi_sorted_hits = 0;
	uint32_t gi_shaded_hits = 0;
	uint32_t gi_shadow_rays = 0;
	uint32_t pad0 = 0;
	uint32_t pad1 = 0;
	uint32_t pad2 = 0;
};
static_assert(sizeof(NextGIHitLightCountersGPU) == 64, "NextGIHitLightCountersGPU must be 64 bytes.");

// Maps a world-space point into the 64^3 world light grid, or UINT32_MAX when outside.
uint32_t nextgi_light_grid_world_to_cell(const Vector3 &p_world, const Vector3 &p_grid_min, float p_inv_cell_size);
// CPU mirror of nextgi_light_grid.glsl's deterministic <= 32 lights/cell selection and overflow keeping.
uint32_t nextgi_light_grid_fill_cell_cpu(const NextGIHitLightGPU *p_lights, uint32_t p_light_count, const Vector3 &p_box_min, const Vector3 &p_box_max, uint16_t r_indices[NEXTGI_LIGHT_GRID_CELL_CAP], uint32_t &r_overflow);
// Shared D14 scenario light collector used by both Hit Lighting (#129) and World Cache (#294).
void nextgi_collect_direct_lights(RenderDataRD *p_render_data, NextGIRTScene *p_rt_scene, LocalVector<NextGIHitLightGPU> &r_lights);

// Multi-bounce mirror decision and translucent skip accumulation math.
bool nextgi_hit_light_should_bounce(float p_roughness, uint32_t p_bounce_index, uint32_t p_max_bounces);
Vector3 nextgi_hit_light_translucent_transmittance(const Color &p_albedo, float p_alpha);
bool nextgi_hit_light_accumulate_translucent(const Color &p_albedo, float p_alpha, const Vector3 &p_layer_radiance, Vector3 &r_accum_radiance, Vector3 &r_throughput, uint32_t &r_skips, uint32_t p_max_skips = NEXTGI_HIT_LIGHT_MAX_TRANSLUCENT_SKIPS);

// GPU pass manager for sorted-deferred hit lighting. Lazily allocated only when hit lighting runs;
// Surface Cache mode never constructs it or allocates any of its shaders/buffers.
class NextGIHitLighting {
public:
	struct ExecuteParams {
		RenderDataRD *render_data = nullptr;
		NextGIRTScene *rt_scene = nullptr;
		NextGIHitMaterials *hit_materials = nullptr;
		NextGISRC *src = nullptr;
		NextGIRelight *relight = nullptr;
		NextGIWRC *wrc = nullptr;
		// GodotNext: nextgi_world_cache (#294). Bound when World Cache replaces SRC on the view.
		RID wc_keys;
		RID wc_radiance;

		RID depth;
		RID normal_roughness;
		RID hiz;
		RID last_frame;
		RID sky_tex;
		RID blue_noise;
		RID sampler_linear;
		RID sampler_nearest;
		RID dummy_color;
		RID dummy_buffer;

		RID reflection_out;
		RID hit_distance_out;
		RID debug_out;
		RID reflect_counter_buffer;

		Size2i screen_size;
		Size2i trace_size;
		Projection inv_view_proj;
		Projection view_proj;
		Basis view_to_world;
		Vector3 camera_pos;
		float rt_radius = 200.0f;
		float max_origin_offset = 0.01f;
		float max_trace_roughness = 0.4f;
		uint32_t frame = 0;
		uint32_t max_reflection_bounces = 1;
		uint32_t hiz_max_mip = 0;
		bool reflect_enabled = true;
		bool reflect_debug = false;
		bool blue_noise_mode = true;
		bool front_layer = false;
		bool sky_enabled = false;
		float sky_energy = 1.0f;
		Color ambient_color;

		bool fog_enabled = false;
		bool fog_depth_mode = false;
		float fog_density = 0.0f;
		float fog_height = 0.0f;
		float fog_height_density = 0.0f;
		float fog_depth_begin = 0.0f;
		float fog_depth_end = 1.0f;
		float fog_depth_curve = 1.0f;
		Color fog_light_color;
		float fog_light_energy = 1.0f;
		float time = 0.0f;
	};

	struct Counters {
		uint64_t dispatches = 0;
		uint64_t gi_dispatches = 0;
		uint32_t last_marked_hits = 0;
		uint32_t last_sorted_hits = 0;
		uint32_t last_refl_sorted_hits = 0;
		uint32_t last_gi_marked_hits = 0;
		uint32_t last_gi_sorted_hits = 0;
		uint32_t last_gi_shaded_hits = 0;
		uint32_t last_gi_shadow_rays = 0;
		uint32_t last_shadow_rays = 0;
		uint32_t last_unshadowed_lights = 0;
		uint32_t last_bounce_rays = 0;
		uint32_t last_translucent_skips = 0;
		uint32_t last_light_grid_overflow = 0;
		uint32_t last_light_count = 0;
		uint32_t last_max_bounces = 1;
		bool indirect_dispatch = false;
	};

private:
	struct ShaderState;
	ShaderState *shaders = nullptr;

	RID record_buffer;
	RID sorted_index_buffer;
	RID histogram_buffer;
	RID block_sums_buffer;
	RID indirect_buffer;
	RID light_buffer;
	RID light_grid_buffer;
	RID counter_buffer;
	RID mark_params_buffer;
	RID hl_params_buffer;
	RID readback_buffer;

	uint32_t ray_capacity = 0;
	uint32_t light_capacity = 0;
	bool indirect_rt_supported = false;

	// State carried across begin_combined() -> mark_sort_and_shade_gi() -> shade_reflections_combined() (#128).
	uint32_t combined_max_rays = 0;
	uint32_t combined_gi_max_rays = 0;
	uint32_t combined_reflect_max_rays = 0;
	uint32_t combined_light_count = 0;

	uint64_t counter_token = 0;
	bool counters_pending = false;
	Counters counters;

	bool _ensure_shaders();
	bool _ensure_buffers(uint32_t p_max_rays, uint32_t p_light_count);
	uint32_t _upload_lights(RenderDataRD *p_render_data, NextGIRTScene *p_rt_scene);
	bool _dispatch_light_grid(const ExecuteParams &p_params, uint32_t p_light_count);
	bool _dispatch_mark(const ExecuteParams &p_params, uint32_t p_max_rays);
	bool _dispatch_sort(uint32_t p_max_rays);
	void _upload_hl_params(const ExecuteParams &p_params, uint32_t p_max_rays, uint32_t p_light_count, uint32_t p_max_bounces);
	bool _dispatch_raygen(const ExecuteParams &p_params, bool p_gi_pass, RID p_output_image, uint32_t p_max_rays, uint32_t p_indirect_offset);
	void _request_counters();
	static void _counters_received(const Vector<uint8_t> &p_data, uint64_t p_token);

public:
	bool execute(const ExecuteParams &p_params);
	// GodotNext: nextgi_hit_lighting_gi (#128). Combined GI + reflections pipeline sharing the world
	// light grid, hit record buffer, and 16-bit counting sort, while shading diffuse GI into probe_image
	// BEFORE probe spatial/temporal filtering and reflections into specular_raw after.
	bool begin_combined(const ExecuteParams &p_params, uint32_t p_gi_max_rays);
	bool mark_sort_and_shade_gi(const ExecuteParams &p_params, RID p_probe_image);
	bool shade_reflections_combined(const ExecuteParams &p_params);
	RID get_record_buffer() const { return record_buffer; }
	RID get_counter_buffer() const { return counter_buffer; }
	uint32_t get_combined_max_rays() const { return combined_max_rays; }
	void free_resources();

	const Counters &get_counters() const { return counters; }
	void fill_counters(Dictionary &r_counters, bool p_gi_active = true) const;
	static String build_mark_shader_source();

	NextGIHitLighting() = default;
	~NextGIHitLighting();
};

} // namespace RendererRD
