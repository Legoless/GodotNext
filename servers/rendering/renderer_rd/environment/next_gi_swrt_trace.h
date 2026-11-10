/**************************************************************************/
/*  next_gi_swrt_trace.h                                                  */
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
#include "core/math/vector3.h"
#include "core/math/vector3i.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/renderer_rd/environment/next_gi_global_sdf.h"
#include "servers/rendering/renderer_rd/environment/next_gi_global_sdf_clipmaps.h"
#include "servers/rendering/renderer_rd/environment/next_gi_global_sdf_gpu.h"
#include "servers/rendering/rendering_device.h"

class RenderDataRD;
class NextGIVoxelLighting;
namespace RendererRD {
class NextGI;
struct NextGILight;
} // namespace RendererRD

// GodotNext: ps_swrt_trace (#186, plan §21.2 L5)
//
// The software ray tracer. NextGI drives it on the software tier:
// - update_scene() before the depth prepass keeps the global SDF clipmaps of
//   the whole scene resident (NextGIGlobalSDFClipmaps; every fed instance
//   with a mesh SDF, no instance or brick cap) and uploads the instance table.
// - trace_lite_probes() traces the Lumen Lite probes (#183) right after it.
// - trace_view() after the depth prepass traces one gather and one
//   reflection ray per cell of the ray grid from the current frame's depth.
// - trace_shadow_rays() is the SDF shadow-ray kernel for the SRC relight.
//
// Trace order (L5): Hi-Z screen trace, detail mesh SDFs for the first 2 m
// (software_ray_tracing_mode = detail), global SDF, then the WRC (at the
// gather handoff, 1.5x probe spacing, or on a miss), then the sky. A global
// hit is shaded from voxel lighting. The step limit reports a hit at the
// current t. Ray origins are offset 1.5 voxels of their level.
//
// Shader bindings (set 0, nextgi_sdf_trace_inc.glsl) that a consumer shader
// including the trace gets from append_trace_uniforms():
//   0 counters, 1 params, 2/3 static/dynamic pool, 4 coverage, 5-10 page
//   tables, 11/12 voxel lighting, 13 depth, 14 normal-roughness, 15 Hi-Z,
//   16 last frame, 17 sky, 18/19/20 WRC radiance/visibility/params,
//   21 instances, 22/23/24 detail mesh SDF info/pages/bricks, 25 froxel grid,
//   26 two-sided instance list.
// Kernel bindings: 27 rays (gather kernels) or shadow requests (shadow
// kernel), 28 Lite probes or shadow results, 29 shadow requests out or lights.
class NextGISWRTTrace {
public:
	enum Mode {
		MODE_GLOBAL = 0,
		MODE_DETAIL = 1,
	};

	enum RayKind {
		RAY_GATHER = 0,
		RAY_REFLECT = 1,
		RAY_KIND_MAX = 2,
	};

	// RayResult::radiance_flags[3] bits (SWRT_HIT_* in the shader).
	enum HitFlags : uint32_t {
		HIT_SURFACE = 1u << 0, // Stopped on a surface (or on the step limit).
		HIT_EXHAUSTED = 1u << 1, // The step limit ran out; reported as a hit at the current t.
		HIT_SCREEN = 1u << 2, // The Hi-Z screen trace hit first.
		HIT_DETAIL = 1u << 3, // A mesh-SDF hit.
		HIT_WRC = 1u << 4, // Shaded from the WRC.
		HIT_SKY = 1u << 5, // Shaded from the sky.
		HIT_COVERAGE = 1u << 6, // Two-sided coverage voxel.
		HIT_SKIPPED = 1u << 7, // No ray: background pixel or above the roughness gate.
		HIT_HANDOFF = 1u << 8, // A gather ray reached the WRC handoff distance.
		HIT_VOXEL = 1u << 9, // Shaded from voxel lighting.
	};

	static constexpr uint32_t MAX_STEPS = 64; // Start value (#186 step 2).
	static constexpr uint32_t MIN_LEVELS = 3; // SWRT_REDUCED floor (#187 step 2).
	static constexpr uint32_t DEFAULT_RAY_GRID = 64;
	static constexpr uint32_t FROXEL_TILE_PIXELS = 64;
	static constexpr uint32_t FROXEL_SLICES = 16;
	static constexpr uint32_t FROXEL_CAP = 8;

	// One traced ray, as the gather and reflection kernels write it.
	struct RayResult {
		float surface_offset[4]; // xyz: depth-reconstructed surface, w: origin offset along surface_normal.
		float surface_normal[4]; // xyz: the normal the ray left from, w: roughness.
		float direction_tmax[4];
		float hit_normal_t[4]; // xyz: hit normal, w: hit t (negative: no surface hit).
		float radiance_flags[4]; // rgb: radiance, w: HitFlags as uint bits.
	};
	static_assert(sizeof(RayResult) == 80, "SWRT ray results must match SWRTRayResult.");

	// One shadow ray: a receiver and the index of its light in the light buffer
	// (NextGILight layout). An offset <= 0 uses 1.5 voxels of the receiver's level.
	struct ShadowRequest {
		float position_offset[4];
		float normal_light[4];
	};
	static_assert(sizeof(ShadowRequest) == 32, "SWRT shadow requests must match SWRTShadowRequest.");
	// Shadow results, one uint per request.
	enum ShadowResult : uint32_t {
		SHADOW_OCCLUDED = 0,
		SHADOW_LIT = 1,
		SHADOW_NO_RAY = 2,
	};

	struct Counters {
		uint32_t steps = 0;
		uint32_t gather_rays = 0;
		uint32_t gather_hits = 0;
		uint32_t reflect_rays = 0;
		uint32_t reflect_hits = 0;
		uint32_t shadow_rays = 0;
		uint32_t shadow_occluded = 0;
		uint32_t exhausted = 0;
		uint32_t screen_hits = 0;
		uint32_t wrc_samples = 0;
		uint32_t detail_hits = 0;
		uint32_t skipped = 0;
	};
	static_assert(sizeof(Counters) == 48, "SWRT counters must match SWRTCounters.");

	// Pool size for a level count: DEFAULT_POOL_BRICKS covers BASE_LEVELS.
	static uint32_t pool_bricks_for_levels(uint32_t p_levels);
	// Cells of the ray grid for an internal size.
	static Vector2i ray_grid(const Vector2i &p_internal_size, uint32_t p_cells_per_axis = DEFAULT_RAY_GRID);
	// Cells of the detail froxel grid for an internal size.
	static Vector3i froxel_cells(const Vector2i &p_internal_size);

	NextGISWRTTrace();
	~NextGISWRTTrace();
	NextGISWRTTrace(const NextGISWRTTrace &) = delete;
	NextGISWRTTrace &operator=(const NextGISWRTTrace &) = delete;

	// Software tier, once per frame before the depth prepass.
	void update_scene(RenderDataRD *p_render_data);
	// Lumen Lite (#183): the WRC probes traced this frame into the irradiance field.
	void trace_lite_probes(RenderDataRD *p_render_data, NextGIVoxelLighting *p_lighting);
	// After the depth prepass of a scenario view: gather and reflection rays
	// from this frame's depth. p_normal_roughness is optional; reflections need it.
	void trace_view(RenderDataRD *p_render_data, NextGIVoxelLighting *p_lighting, RID p_normal_roughness);
	// The SRC relight's SDF shadow rays. Results get one ShadowResult per request.
	// Needs update_scene() of this frame, not trace_view(): the kernel reads the
	// current clipmaps through its own params and binds no view input.
	bool trace_shadow_rays(RID p_requests, uint32_t p_count, RID p_lights, uint32_t p_light_count, RID p_results, float p_rt_radius);

	RID get_ray_buffer(RayKind p_kind) const { return p_kind < RAY_KIND_MAX ? ray_buffers[p_kind] : RID(); }
	Vector2i get_ray_grid() const { return grid; }
	uint32_t get_active_levels() const { return active_levels; }
	uint32_t get_mode() const { return mode; }
	bool is_reduced() const { return reduced; }
	// One or more frames old (an asynchronous readback).
	Counters get_counters() const;
	void fill_info(Dictionary &r_info) const;

	// #187 auto quality: traces the Lite probe kernel over a fixture global SDF
	// for a High-size workload and returns the wall time of the dispatch. It fails
	// when the fixture's own segments (fixture_segments()) disagree with its boxes
	// on more than 1 - FIXTURE_MIN_AGREEMENT of them: a trace that misses its room
	// would time empty space.
	static bool run_microbenchmark(uint64_t &r_usec, String &r_failure);
	// The last microbenchmark's result in microseconds; 0 before it ran.
	static uint64_t get_last_benchmark_usec() { return last_benchmark_usec; }
	static constexpr uint32_t FIXTURE_SEGMENTS = 128; // Per class: lit and occluded.
	static constexpr float FIXTURE_CLEARANCE = 0.75f;
	static constexpr float FIXTURE_CORE_INSET = 0.05f;
	static constexpr float FIXTURE_MAX_LENGTH = 20.0f;
	static constexpr float FIXTURE_MIN_AGREEMENT = 0.95f;
	// Shadow requests between random free points around p_boxes, one positional light
	// per request at the segment's end. A segment is occluded when it crosses a box's
	// core; segments that graze a box are left out. Deterministic (fixed seed).
	static void fixture_segments(const LocalVector<AABB> &p_boxes, uint32_t p_per_class, LocalVector<ShadowRequest> &r_requests, LocalVector<RendererRD::NextGILight> &r_lights, LocalVector<uint8_t> &r_expected_occluded);

private:
	struct GpuState;
	struct InstanceKey {
		const void *geometry = nullptr;
		uint32_t element = 0;
		bool operator==(const InstanceKey &p_other) const { return geometry == p_other.geometry && element == p_other.element; }
	};
	struct InstanceKeyHasher {
		static uint32_t hash(const InstanceKey &p_key);
	};
	struct FedInstance {
		AABB bounds;
		RID mesh;
	};

	bool _ensure_gpu();
	bool _ensure_field();
	void _note_reduced(uint32_t p_levels_before);
	void _collect_instances(RenderDataRD *p_render_data);
	void _upload_instances();
	void _upload_detail_meshes();
	void _fill_params(RenderDataRD *p_render_data, NextGIVoxelLighting *p_lighting, RID p_normal_roughness, bool p_screen_trace, bool p_wrc, bool p_readback);
	// p_view_inputs false (the shadow kernel) binds dummies for the depth, normal-roughness,
	// screen, sky and WRC inputs.
	void _append_trace_uniforms(Vector<RD::Uniform> &r_uniforms, RID p_params, NextGIVoxelLighting *p_lighting, RID p_normal_roughness, bool p_view_inputs, bool p_screen_trace) const;
	void _bind_wrc(RendererRD::NextGI *p_next_gi);
	void _request_counters();
	static void _counters_received(const Vector<uint8_t> &p_data, uint64_t p_token);

	GpuState *gpu = nullptr;
	NextGIGlobalSDFGPU *field = nullptr;
	NextGIGlobalSDFClipmaps clipmaps;
	uint32_t active_levels = NextGIGlobalSDF::BASE_LEVELS;
	bool reduced = false;
	bool benchmark = false; // run_microbenchmark()'s tracer never reports SWRT_REDUCED.
	bool field_failed = false;
	uint64_t sdf_generation = UINT64_MAX;

	// The scene: every fed instance with a mesh SDF.
	HashMap<RID, MeshSDFData> field_cache;
	Vector<NextGIGlobalSDF::Instance> instances;
	LocalVector<uint32_t> instance_card_sets;
	LocalVector<uint32_t> two_sided;
	HashMap<InstanceKey, FedInstance, InstanceKeyHasher> fed;
	bool instances_dirty = true;
	bool detail_meshes_dirty = true;
	LocalVector<const MeshSDFData *> detail_mesh_order;

	uint32_t mode = MODE_GLOBAL;
	Vector2i grid;
	Vector2i screen_size;
	// This frame's inputs; dummies stand in for the RIDs that are not valid.
	RID screen_depth;
	RID screen_hiz;
	RID screen_last_frame;
	RID sky_radiance;
	RID wrc_radiance;
	RID wrc_visibility;
	RID wrc_params;
	bool pipelines_failed = false;
	RID ray_buffers[RAY_KIND_MAX];

	// Asynchronous counter readback.
	mutable Mutex counter_mutex;
	Counters counters;
	uint64_t counter_token = 0;
	bool counters_pending = false;

	// Verification readback (debug/swrt_trace_readback).
	Dictionary readback_meta;
	static uint64_t last_benchmark_usec;
	static uint32_t last_fixture_checked;
	static uint32_t last_fixture_agreed;
};
