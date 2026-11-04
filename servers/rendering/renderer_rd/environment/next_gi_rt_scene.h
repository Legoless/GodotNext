/**************************************************************************/
/*  next_gi_rt_scene.h                                                    */
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
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/vector.h"

class RenderDataRD;

namespace RendererRD {

// NextGI RT scene v0 (#28, plan D13 and §6.3/§8.2): the contracts and pure helpers for the
// chunked RT geometry arena, static BLAS parts and the CPU-fed TLAS. Everything above
// NextGIRTScene is device-free so doctests can run it without a GPU.

enum {
	NEXTGI_RT_CHUNK_SIZE = 256 * 1024 * 1024, // 256 MiB; chunks never move (D13).
	NEXTGI_RT_MAX_TRIANGLES_PER_BLAS = 128 * 1024,
	NEXTGI_RT_TLAS_CAPACITY = 100000,
	NEXTGI_RT_RECORD_INDEX_MAX = (1 << 24) - 1, // The instance custom index is 24 bits.
	NEXTGI_RT_ALIGNMENT = 16,
};

// NextGIInstanceRecord::rt_flags bits (§6.3 RT hit record).
enum NextGIInstanceRTFlags {
	NEXTGI_RT_FLAG_STATIC = 1 << 0,
	NEXTGI_RT_FLAG_SKINNED = 1 << 1,
	NEXTGI_RT_FLAG_ALPHA_AS_OPAQUE = 1 << 2,
	NEXTGI_RT_FLAG_FAR = 1 << 3,
	NEXTGI_RT_FLAG_LIGHTMAPPED = 1 << 4,
};

// std430 GPU records. The sizes are part of the §6.3 contract; do not repack them.
struct NextGIInstanceRecord {
	float object_to_world[12]; // 3x4 rows, packed like the drivers' TLAS instance transform.
	uint32_t surface_base = 0; // First NextGISurfaceRecord of this part.
	uint32_t surface_count = 0;
	uint32_t card_set = UINT32_MAX; // Set by the SRC card work; invalid until then.
	uint32_t page_table = UINT32_MAX;
	uint32_t rt_flags = 0; // NextGIInstanceRTFlags.
	float origin_offset = 0.0f; // min(4 * eps_rt * max_scale, 0.05).
	uint32_t tlas_index = 0;
	uint32_t arena_chunk = 0;
};
static_assert(sizeof(NextGIInstanceRecord) == 80, "NextGIInstanceRecord must stay 80 bytes (plan §6.3).");

struct NextGISurfaceRecord {
	uint32_t vertex_offset = 0; // Bytes in the instance record's arena_chunk.
	uint32_t index_offset = 0;
	uint32_t triangle_count = 0;
	uint32_t flags = 0;
};
static_assert(sizeof(NextGISurfaceRecord) == 16, "NextGISurfaceRecord must stay 16 bytes (plan §6.3).");

// The RT arena and BLAS builds exist only with nextgi/enabled and SUPPORTS_RAY_QUERY (D13).
_FORCE_INLINE_ bool nextgi_rt_should_enable(bool p_project_enabled, bool p_has_ray_query) {
	return p_project_enabled && p_has_ray_query;
}

// GodotNext: nextgi_as_slots
// An instance enters the TLAS only once its BLAS exists (plan §8.2). In the frame block
// "AS updates" (line 1a) runs before "AS builds" (line 1b), so the TLAS phase admits only
// parts whose BLAS was already built by an earlier frame's builds phase. A dirty mesh is
// skipped this frame, built later in the same frame, and joins the TLAS on the next frame:
// a one-frame lag, never a stale or missing BLAS in the TLAS.
_FORCE_INLINE_ bool nextgi_rt_part_enters_tlas(bool p_blas_dirty, bool p_blas_valid) {
	return !p_blas_dirty && p_blas_valid;
}

// eps_rt = min(max_extent / 256, rt_max_error) (D13).
_FORCE_INLINE_ float nextgi_rt_compute_eps(float p_longest_axis_size, float p_rt_max_error) {
	return MIN(p_longest_axis_size / 256.0f, p_rt_max_error);
}

// LODs ascend in edge_length. Returns the last LOD with edge_length <= eps_rt, or -1 for the base indices.
int nextgi_rt_select_lod(const float *p_edge_lengths, uint32_t p_lod_count, float p_eps_rt);

_FORCE_INLINE_ float nextgi_rt_origin_offset(float p_eps_rt, float p_max_scale) {
	return MIN(4.0f * p_eps_rt * p_max_scale, 0.05f);
}

// Decodes float32 positions from SurfaceData vertex_data (positions come first in the planar layout).
// Compressed positions (ARRAY_FLAG_COMPRESS_ATTRIBUTES) are R16G16B16A16_UNORM over the surface AABB.
// The caller has excluded 2D vertices. Returns false on malformed input.
bool nextgi_rt_decode_positions(uint64_t p_format, const Vector<uint8_t> &p_vertex_data, uint32_t p_vertex_count, const AABB &p_aabb, Vector<float> &r_positions);

// Widens 16-bit indices to uint32 (or copies uint32). An empty index array generates sequential indices.
void nextgi_rt_widen_indices(const Vector<uint8_t> &p_index_data, uint32_t p_index_count, bool p_is_16_bit, uint32_t p_vertex_count, Vector<uint32_t> &r_indices);

// First-fit free list over fixed-size chunks (16 B aligned, coalescing). Allocations never move;
// a range that fits no chunk opens a new one. Pure CPU logic; MeshStorage owns the GPU buffers.
class NextGIArenaAllocator {
public:
	struct Allocation {
		uint32_t chunk = UINT32_MAX;
		uint32_t offset = 0;
		uint32_t size = 0; // Requested size rounded up to the alignment.
		bool new_chunk = false;
		bool is_valid() const { return chunk != UINT32_MAX; }
	};

private:
	struct FreeRange {
		uint32_t chunk = 0;
		uint32_t offset = 0;
		uint32_t size = 0;
	};

	uint32_t chunk_size = NEXTGI_RT_CHUNK_SIZE;
	uint32_t chunk_count = 0;
	LocalVector<FreeRange> free_ranges;

public:
	explicit NextGIArenaAllocator(uint32_t p_chunk_size = NEXTGI_RT_CHUNK_SIZE) :
			chunk_size(p_chunk_size) {}

	Allocation allocate(uint32_t p_size);
	void free_range(const Allocation &p_allocation);
	void reset();

	uint32_t get_chunk_size() const { return chunk_size; }
	uint32_t get_chunk_count() const { return chunk_count; }
};

// One BLAS geometry: a whole surface, or a triangle slice of a surface over the BLAS cap.
struct NextGIRTPartGeometry {
	uint32_t surface = 0;
	uint32_t first_triangle = 0;
	uint32_t triangle_count = 0;
};

// A planned BLAS part. Whole surfaces are packed first; a surface alone over the cap is split by
// triangles, never coarsened (D13). A part never spans arena chunks, so its instance record can
// name one arena_chunk.
struct NextGIRTPartPlan {
	LocalVector<NextGIRTPartGeometry> geometries;
	uint32_t chunk = UINT32_MAX;
	uint32_t triangle_count = 0;
	uint32_t first_surface = 0;
	uint32_t surface_count = 0;
};

// Surfaces with a triangle count of 0 (ineligible or alpha-excluded) are skipped.
void nextgi_rt_split_parts(const uint32_t *p_triangle_counts, const uint32_t *p_chunks, uint32_t p_surface_count, uint32_t p_max_triangles_per_part, LocalVector<NextGIRTPartPlan> &r_parts);

// GodotNext: nextgi_rt_registry
// Scenario-wide selection (#39). These reasons are the per-instance Fallback enum, not tier reasons.
enum NextGIRTExclusion {
	NEXTGI_RT_INCLUDED = 0,
	NEXTGI_RT_NOT_IN_SCENE,
	NEXTGI_RT_INSTANCE_CAP,
	NEXTGI_RT_DYNAMIC_EXCLUDED,
};

enum {
	NEXTGI_RT_BUILD_TRIANGLE_CAP = 250000, // Provisional until G0-6 (#9) measures it in PQ.
	NEXTGI_RT_REFIT_TRIANGLE_CAP = 200000,
	NEXTGI_RT_ABSENT_FRAMES = 120,
};

static constexpr float NEXTGI_RT_ANGULAR_CULL_DISTANCE = 30.0f;
static constexpr uint64_t NEXTGI_RT_AS_BUDGET_BYTES = 1024ull * 1024ull * 1024ull;

_FORCE_INLINE_ bool nextgi_rt_instance_eligible(bool p_mesh, bool p_visible, bool p_gi_contributing, bool p_skinned) {
	return p_mesh && p_visible && p_gi_contributing && !p_skinned;
}

// A MultiMesh rewritten on each of the last 8 frames is dynamic and stays out of the TLAS.
enum { NEXTGI_RT_DYNAMIC_CHANGE_STREAK = 8 };

struct NextGIMultiMeshChange {
	uint64_t last_change_frame = 0;
	uint8_t change_streak = 0;
};

// One change per frame. A same-frame repeat does not advance the streak. A gap resets it to 1.
void nextgi_rt_note_multimesh_change(NextGIMultiMeshChange &r_state, uint64_t p_frame);
_FORCE_INLINE_ bool nextgi_rt_multimesh_is_dynamic(const NextGIMultiMeshChange &p_state) {
	return p_state.change_streak >= NEXTGI_RT_DYNAMIC_CHANGE_STREAK;
}

// Element 3x4 is row-major, matching MeshStorage's MultiMesh cache (basis rows, then origin component).
Transform3D nextgi_rt_element_transform(const float *p_rows_3x4);
Transform3D nextgi_rt_element_world(const Transform3D &p_instance, const float *p_rows_3x4);

struct NextGIRTCoverage {
	uint32_t denominator = 0;
	uint32_t numerator = 0;
	uint32_t not_in_scene = 0;
	uint32_t instance_cap = 0;
	uint32_t dynamic_excluded = 0;
	uint32_t blas_pending = 0;
};

// Integer percent. An empty denominator is 0, not a divide.
int64_t nextgi_rt_coverage_percent(const NextGIRTCoverage &p_coverage);

// asin(min(1, r/d)). A distance of 0 (the camera is inside the bounds) is a full hemisphere.
float nextgi_rt_angular_radius(float p_distance, float p_bounding_radius);

// Outside rt_scene_radius, or beyond 30 m and under 0.5°, is NOT_IN_RT_SCENE. No frustum test.
NextGIRTExclusion nextgi_rt_cull_instance(float p_distance, float p_bounding_radius, float p_scene_radius);

// Keeps the p_cap largest angular radii. The rest become RT_INSTANCE_CAP. p_angular is reordered.
void nextgi_rt_apply_instance_cap(float *p_angular, NextGIRTExclusion *p_reasons, uint32_t p_count, uint32_t p_cap);

enum NextGIDirtyKind {
	NEXTGI_DIRTY_MOVED,
	NEXTGI_DIRTY_FREED,
	NEXTGI_DIRTY_SCENARIO,
	NEXTGI_DIRTY_HIDDEN,
};

struct NextGIDirtyRegion {
	bool has_old = false;
	bool has_new = false;
};

NextGIDirtyRegion nextgi_rt_dirty_region(NextGIDirtyKind p_kind);

enum NextGILightBake {
	NEXTGI_LIGHT_DISABLED,
	NEXTGI_LIGHT_STATIC,
	NEXTGI_LIGHT_DYNAMIC,
};

enum NextGILightChange {
	NEXTGI_LIGHT_ADD,
	NEXTGI_LIGHT_REMOVE,
	NEXTGI_LIGHT_MOVE,
	NEXTGI_LIGHT_ENERGY,
	NEXTGI_LIGHT_COLOR,
	NEXTGI_LIGHT_RANGE,
	NEXTGI_LIGHT_SHADOW,
	NEXTGI_LIGHT_CHANGE_MAX,
};

// DISABLED lights emit nothing. STATIC and DYNAMIC emit every change.
_FORCE_INLINE_ bool nextgi_rt_light_event_emits(NextGILightBake p_bake) {
	return p_bake != NEXTGI_LIGHT_DISABLED;
}

// A surface updated on each of the last 4 frames is dynamic and refits instead of rebuilding.
bool nextgi_rt_surface_is_dynamic(uint32_t p_frame, const uint32_t p_update_frames[4]);

// Past the refit triangle cap, the instance is RT_DYNAMIC_EXCLUDED. Otherwise it refits.
NextGIRTExclusion nextgi_rt_refit_or_exclude(uint32_t p_refit_triangles_so_far, uint32_t p_triangles, uint32_t p_refit_cap);

struct NextGIBuildItem {
	uint32_t id = 0;
	uint32_t triangles = 0;
	bool is_new = false;
	float angular_radius = 0.0f;
};

// New instances first, then largest angular radius. Builds and compaction copies both spend the cap.
// Returns how many items were accepted. The walk stops at the first item that does not fit.
uint32_t nextgi_rt_schedule_builds(const NextGIBuildItem *p_items, uint32_t p_count, uint32_t p_triangle_cap, uint32_t *r_accepted, uint32_t p_accepted_capacity, uint32_t &r_triangles);

// A chunk with more than a quarter of its bytes free is fragmented and its BLASes are rebuilt into fresh ranges.
_FORCE_INLINE_ bool nextgi_rt_chunk_needs_defrag(uint64_t p_free_bytes, uint64_t p_chunk_bytes) {
	return p_chunk_bytes > 0 && p_free_bytes * 4 > p_chunk_bytes;
}

struct NextGIASMesh {
	uint32_t id = 0;
	uint64_t bytes = 0;
	uint32_t frames_absent = 0; // Frames since this mesh was in the TLAS.
};

// Evicts meshes absent for at least p_absent_frames, oldest first. Returns true when the
// remainder still exceeds the budget: the tier falls back with AS_MEMORY.
bool nextgi_rt_evict_as_memory(const NextGIASMesh *p_meshes, uint32_t p_count, uint64_t p_budget, uint32_t p_absent_frames, uint32_t *r_evict, uint32_t &r_evict_count);

// Patches the CPU-decoded position copy. A range past the positions is ignored.
void nextgi_rt_patch_positions(Vector<float> &r_positions, uint32_t p_byte_offset, const uint8_t *p_data, uint32_t p_size);

// One published list per frame. Two views read it; the next swap drops it.
template <typename T>
class NextGIRTEventBuffer {
	LocalVector<T> sides[2];
	int published = -1;
	int writing = 0;
	uint64_t frame = 0;

public:
	void push(const T &p_event) { sides[writing].push_back(p_event); }
	void swap(uint64_t p_frame) {
		published = writing;
		writing = 1 - writing;
		sides[writing].clear();
		frame = p_frame;
	}
	uint64_t get_frame() const { return frame; }
	const LocalVector<T> &read() const {
		static const LocalVector<T> empty;
		return published < 0 ? empty : sides[published];
	}
};

// The CPU-fed TLAS and the GPU instance/surface records. Owned by NextGI; created only under the
// HWRT tier. The two phases consume the v0 feed in RenderSDFGIUpdateData::nextgi_rt_instances;
// the P1b registry (#39) replaces that feed. NextGI::render_frame calls rebuild_tlas() from the
// "AS updates" slot (line 1a) and build_dirty_blas() from the "AS builds" slot (line 1b), in
// that order, so a BLAS built this frame enters the TLAS next frame (see
// nextgi_rt_part_enters_tlas).
class NextGIRTScene {
public:
	struct Stats {
		uint32_t fed_instance_count = 0;
		uint32_t instance_count = 0; // TLAS instances (and records) of the last TLAS phase.
		NextGIRTCoverage coverage;
		uint32_t blas_build_count = 0; // BLAS builds of the last builds phase; 0 on a static scene once built.
		uint32_t blas_build_triangles = 0;
		uint32_t compaction_copies = 0;
		uint32_t refit_triangles = 0;
		uint64_t as_bytes = 0;
		double tlas_fill_ms = 0.0;
		double scene_update_ms = 0.0;
		uint64_t tlas_build_count = 0; // One TLAS rebuild per TLAS phase with instances.
	};

private:
	struct PendingCompact {
		RID blas;
		uint32_t triangles = 0;
	};

	RID tlas;
	RID instance_record_buffer;
	RID surface_record_buffer;
	uint32_t instance_record_capacity = 0;
	uint32_t surface_record_capacity = 0;
	Stats stats;
	LocalVector<PendingCompact> pending_compactions;
	HashMap<RID, uint64_t> mesh_last_tlas_frame;
	bool as_memory_exceeded = false;
	bool tlas_traceable = false;
	uint32_t compaction_triangles_this_frame = 0;
	uint64_t as_budget_bytes = NEXTGI_RT_AS_BUDGET_BYTES;

	void _build_dirty_mesh_parts(RID p_mesh);
	void _ensure_record_buffers(uint32_t p_instance_count, uint32_t p_surface_count);
	uint32_t _mesh_triangle_count(RID p_mesh) const;
	void _apply_as_budget(uint64_t p_frame);

public:
	void init();
	void free();
	// GodotNext: nextgi_as_slots
	// The "AS updates" phase (line 1a): rebuilds the TLAS and the records from the parts
	// whose BLAS already exists. Runs before build_dirty_blas() in the frame block.
	void rebuild_tlas(const ::RenderDataRD *p_render_data);
	// The "AS builds" phase (line 1b): builds the BLAS parts of every dirty fed mesh.
	// The new parts enter the TLAS in the next frame's rebuild_tlas().
	void build_dirty_blas(const ::RenderDataRD *p_render_data);
	// Queues compaction after this frame's traces. A readback during card capture flushes the
	// device, and completing a compaction invalidates the TLAS those traces still bind.
	void compact_after_traces();

	RID get_tlas() const { return tlas; }
	RID get_instance_record_buffer() const { return instance_record_buffer; }
	RID get_surface_record_buffer() const { return surface_record_buffer; }
	const Stats &get_stats() const { return stats; }
	bool is_as_memory_exceeded() const { return as_memory_exceeded; }
	// True only after this frame's TLAS build, and only while line 1b has not rebuilt a BLAS the TLAS references.
	bool is_tlas_traceable() const { return tlas_traceable; }
	void set_as_budget_bytes(uint64_t p_bytes) { as_budget_bytes = p_bytes; }

	NextGIRTScene() {}
	~NextGIRTScene() {}
};

} // namespace RendererRD
