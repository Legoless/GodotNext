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
#include "core/math/transform_3d.h"
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
#include "servers/rendering/renderer_rd/environment/next_gi_rt_scene.h"
#include "servers/rendering/rendering_device.h"

class RenderDataRD;
class RenderGeometryInstance;
class NextGIVoxelLighting;
namespace RendererRD {
class NextGI;
class NextGIRelight;
struct NextGILight;
} // namespace RendererRD

// GodotNext: ps_swrt_trace (#186, plan §21.2 L5)
//
// The software ray tracer. NextGI drives it on the software tier:
// - update_scene() in the frame block's "AS updates" slot keeps the global SDF
//   clipmaps of the whole scene resident (NextGIGlobalSDFClipmaps; every fed
//   instance with a mesh SDF, no instance or brick cap) and uploads the instance
//   table; prepare_scene_params() then fills the params every later slot traces with.
//   The SRC relight slot rewrites them after the voxel shade (#188), so the WRC and the
//   gather read this frame's clipmaps.
// - The frame block's software siblings (nextgi_gather_swrt.glsl,
//   nextgi_wrc_update_swrt.glsl) bind the trace in set 1 through
//   create_trace_uniform_set(); the gather refills prepare_view_params() per view.
// - trace_view() is the verification kernel: with debug/swrt_trace_readback it
//   traces one gather and one reflection ray per cell of the ray grid after the
//   frame block's slots, from the current frame's depth.
// - run_microbenchmark() times the WRC update's software sibling over the fixture for
//   the auto quality (#259): the work Lumen Lite renders through each frame.
// - trace_shadow_rays() is the SDF shadow-ray kernel the verification checks run; the SRC
//   relight's own sibling (nextgi_src_relight_swrt.glsl) marches it for directional lights;
//   positional lights' rays march the mesh SDFs of the instances inside the light's range
//   (NextGIRelight::build_light_candidates, #186 step 5).
// - assign_src_pages() (#188) runs after update_scene() in the same slot: the record pass that
//   gives SWRT instance i its NextGIInstanceRecord i (card set, relight page table, HWRT's flag
//   rule) and uploads the instance and record buffers together.
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
// kernel), 28 Lite probes (the microbenchmark) or shadow results, 29 shadow
// requests out or lights.
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
		HIT_CARD = 1u << 10, // A two-sided instance resolved this coverage hit (#186 step 6).
		HIT_BACKFACE = 1u << 11, // The ray meets the back of that instance's card.
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
		uint32_t coarse_steps = 0; // GodotNext: global_sdf_coarse_bound (#257)
	};
	static_assert(sizeof(Counters) == 52, "SWRT counters must match SWRTCounters.");

	// Pool size for a level count: DEFAULT_POOL_BRICKS covers BASE_LEVELS.
	static uint32_t pool_bricks_for_levels(uint32_t p_levels);
	// GodotNext: global_sdf_coarse_bound (#257). The trace's fresh-level mask (its params' extra.w):
	// bit L (0-5) lets it step by level L's coarse bound, bit 8 + L lets it climb through level L's
	// empty bricks. A level is fresh while it is resident and has no dirty volume pending (it
	// updates on every 2^L-th frame). p_coarse_skip false clears bits 0-5 only.
	static uint32_t coarse_level_mask(const NextGIGlobalSDFClipmaps &p_clipmaps, uint32_t p_levels, bool p_coarse_skip);
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
	// After the depth prepass of a scenario view: gather and reflection rays
	// from this frame's depth. p_normal_roughness is optional; reflections need it.
	void trace_view(RenderDataRD *p_render_data, NextGIVoxelLighting *p_lighting, RID p_normal_roughness);
	// GodotNext: ps_swrt_trace (#186). The production siblings of the hardware shaders
	// (nextgi_gather_swrt.glsl, nextgi_wrc_update_swrt.glsl) bind the trace's resources in set 1
	// (bindings 1-12, 21-26). The scene params (levels, origins, step budget, pool, instance
	// counts, the voxel-lighting clipmap) are filled once per frame in the AS-updates slot right
	// after update_scene(); the view params add one view's matrices, screen and detail froxels and
	// are refilled before that view's gather.
	bool is_ready() const;
	void prepare_scene_params(NextGIVoxelLighting *p_lighting);
	void prepare_view_params(RenderDataRD *p_render_data, NextGIVoxelLighting *p_lighting);
	// A uniform set for p_set of p_shader with the trace's resources and the scene or view params.
	// The caller frees it after its dispatches.
	RID create_trace_uniform_set(RID p_shader, uint32_t p_set, bool p_view, NextGIVoxelLighting *p_lighting) const;
	// Detail mode: the froxel object grid of this view, marked from the gather's screen-probe
	// positions (one per 16 px tile) and culled with mesh-SDF samples. Needs prepare_view_params().
	void build_detail_froxels(RenderDataRD *p_render_data, NextGIVoxelLighting *p_lighting);
	bool is_detail_active() const;
	// The frame-block slot the last update_scene() ran in ("none" outside the block).
	const String &get_update_slot() const { return last_update_slot; }

	// GodotNext: ps_swrt_trace (#186, #188). The record pass: one NextGIInstanceRecord per SWRT
	// instance, in instance order, so a detail hit's instance index is its record index and the
	// relight's page record_index is the instance index (plan §6.3 records on the software tier).
	struct RecordSource {
		RenderGeometryInstance *geometry = nullptr;
		uint32_t element = UINT32_MAX;
		bool lightmapped = false;
		Transform3D transform;
		uint32_t instance_card_set = UINT32_MAX; // NextGISRC::get_instance_card_set().
		uint32_t mesh_card_set = UINT32_MAX; // NextGISRC::get_mesh_card_set(), the fallback.
	};
	// Assigns relight pages to r_record and returns its page table (NextGIRelight::assign_pages()).
	typedef uint32_t (*PageAssigner)(void *p_userdata, RenderGeometryInstance *p_geometry, RendererRD::NextGIInstanceRecord &r_record, uint32_t p_element);
	// One record pass of p_relight over the sources. Record i: the transform, STATIC, plus
	// LIGHTMAPPED for a lightmapped source (the RT scene's rule, so a STATIC light skips it), the
	// instance's card set or else its mesh's, tlas_index i, and the page table p_assign gives it.
	// p_relight null leaves every page table UINT32_MAX.
	static void build_records(const RecordSource *p_sources, uint32_t p_count, RendererRD::NextGIRelight *p_relight, PageAssigner p_assign, void *p_userdata, LocalVector<RendererRD::NextGIInstanceRecord> &r_records);
	// The instance and record buffers upload together, so GPU instance i, froxel list entry i and
	// record i always agree. They are due when the instances changed (bounds, meshes, membership),
	// their feed order changed, or a record differs from the uploaded one (a new card set or page
	// table).
	struct UploadState {
		bool uploaded = false;
		uint64_t order_hash = 0;
		LocalVector<RendererRD::NextGIInstanceRecord> records;
	};
	static uint64_t instance_order_hash(const RecordSource *p_sources, uint32_t p_count);
	static bool upload_due(const UploadState &p_state, uint64_t p_order_hash, bool p_instances_changed, const LocalVector<RendererRD::NextGIInstanceRecord> &p_records);
	// In the AS-updates slot after update_scene(): the record pass, then both uploads when due.
	void assign_src_pages(RendererRD::NextGIRelight *p_relight);
	// The records the gather, the SRC feedback and the relight read on the software tier.
	RID get_record_buffer() const;
	uint32_t get_record_count() const;
	// GodotNext: ps_swrt_trace (#186 step 5). The world bounds of each instance's mesh SDF, in the
	// order of the GPU instance buffer, and a counter that changes whenever that buffer is rebuilt.
	void get_instance_bounds(LocalVector<AABB> &r_bounds) const;
	uint64_t get_instance_generation() const { return instance_generation; }
	// The frame-block slot the last assign_src_pages() ran in ("none" outside the block).
	const String &get_record_slot() const { return last_record_slot; }
	// The verification trace (debug/swrt_trace_readback) runs after the frame block's slots.
	static bool readback_enabled();

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
	void _fill_params(RenderDataRD *p_render_data, NextGIVoxelLighting *p_lighting, RID p_normal_roughness, bool p_screen_trace, bool p_wrc, bool p_readback, RID p_target);
	// p_view_inputs false (the shadow kernel) binds dummies for the depth, normal-roughness,
	// screen, sky and WRC inputs.
	void _append_trace_uniforms(Vector<RD::Uniform> &r_uniforms, RID p_params, NextGIVoxelLighting *p_lighting, RID p_normal_roughness, bool p_view_inputs, bool p_screen_trace) const;
	// GodotNext: ps_swrt_trace (#186). The trace's own resources (0-12, 21-26) and the view inputs
	// (13-20) that _append_trace_uniforms() binds together.
	void _append_core_uniforms(Vector<RD::Uniform> &r_uniforms, RID p_params, NextGIVoxelLighting *p_lighting, bool p_counters) const;
	void _append_view_uniforms(Vector<RD::Uniform> &r_uniforms, RID p_normal_roughness, bool p_view_inputs, bool p_screen_trace) const;
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
	// GodotNext: ps_swrt_trace (#186, #188). Parallel to instances: the record pass's sources, this
	// frame's records, and the last upload of both buffers.
	LocalVector<RecordSource> record_sources;
	LocalVector<RendererRD::NextGIInstanceRecord> records;
	UploadState upload_state;
	uint32_t uploaded_record_count = 0;
	LocalVector<uint32_t> two_sided;
	HashMap<InstanceKey, FedInstance, InstanceKeyHasher> fed;
	bool instances_dirty = true;
	uint64_t instance_generation = 0;
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
	String last_update_slot = "none";
	String last_record_slot = "none";
	static uint64_t last_benchmark_usec;
	static uint32_t last_fixture_checked;
	static uint32_t last_fixture_agreed;
};
