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
#include "servers/rendering/renderer_rd/environment/next_gi_global_sdf_clipmaps.h"
#include "servers/rendering/renderer_rd/environment/next_gi_global_sdf_gpu.h"
#include "servers/rendering/renderer_rd/environment/next_gi_sdf.h"
#include "servers/rendering/renderer_rd/environment/next_gi_shadow_policy.h"
#include "servers/rendering/rendering_server_types.h"

class RenderDataRD;
class RenderGeometryInstance;
class RenderGeometryInstanceBase;
class NextGISDFStream;
class NextGIFarField;

namespace RendererRD {

// GodotNext: nextgeo_rt_native (#156)
class NextGIRTNative;

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
	// GodotNext: nextgi_hit_lighting (#129). Alpha-blended instance admitted with TLAS mask bit 1.
	NEXTGI_RT_FLAG_TRANSLUCENT = 1 << 5,
};

// NextGISurfaceRecord::flags (plan §6.3 hit record). Bits 8-31 carry the bind-pose vertex
// offset of a skinned slot in NEXTGI_RT_ALIGNMENT units, or bits 4-31 carry the 16-byte-aligned
// byte offset of a VGD Stage-2 triangle-group slice, so the record stays 16 bytes.
enum NextGISurfaceRTFlags {
	NEXTGI_RT_SURFACE_FLAG_SKINNED = 1 << 0,
	NEXTGI_RT_SURFACE_FLAG_STAGE2_CARDS = 1 << 1,
	NEXTGI_RT_SURFACE_FLAG_NO_CARDS = 1 << 2,
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

// GodotNext: ps_swrt_trace (#186, #188). The CPU copies of mesh triangles are kept with
// nextgi/enabled on any device: the SRC builds card sets from them on both tiers, and runtime
// mesh SDFs (the software tier's global SDF) are built from them. A device without ray query
// gets no RT arena (nextgi_rt_should_enable) but keeps the copies.
_FORCE_INLINE_ bool nextgi_rt_keep_cpu_geometry(bool p_project_enabled, bool p_has_ray_query) {
	return p_project_enabled;
}

// GodotNext: ps_swrt_trace (#186, #188). What mesh_surface_update_vertex_region() does to the
// NextGI copies. The CPU copy follows the vertex buffer wherever it is kept, on a device without
// ray query too (the card sets and runtime mesh SDFs read it), and the update is noted there, so
// a mesh updated every frame counts as dynamic on both kinds of device (the BLAS refit rule and
// the global SDF's dynamic split). The arena copy and the BLAS flag exist only with the RT arena.
// p_touches_cpu_copy: the update starts inside the copied positions. p_float_positions: the vertex
// buffer stores float positions, the copy's layout. A compressed buffer's 16-bit positions are not
// copied byte for byte, so neither copy is patched there (it stays stale until the surface is set
// again).
struct NextGIRTVertexUpdate {
	bool patch_cpu_copy = false;
	bool upload_arena = false;
	bool mark_blas_dirty = false;
	bool note_update = false;
};

_FORCE_INLINE_ NextGIRTVertexUpdate nextgi_rt_plan_vertex_update(bool p_rt_enabled, bool p_cpu_geometry_enabled, bool p_touches_cpu_copy, bool p_in_arena, bool p_float_positions) {
	NextGIRTVertexUpdate plan;
	plan.patch_cpu_copy = p_cpu_geometry_enabled && p_touches_cpu_copy && p_float_positions;
	plan.upload_arena = p_rt_enabled && p_touches_cpu_copy && p_in_arena && p_float_positions;
	plan.mark_blas_dirty = p_rt_enabled;
	plan.note_update = p_cpu_geometry_enabled;
	return plan;
}

// GodotNext: ps_swrt_trace (#186, #188). A surface material that keeps the surface out of the TLAS
// (alpha-tested without nextgi_rt_opaque, or blended; #61). The SRC's software-tier card rule
// leaves the same surfaces out.
bool nextgi_rt_material_is_alpha(RID p_material);

// GodotNext: ps_swrt_trace (#186, #188). debug/deny_ray_query simulates a device without ray query
// on one that has it, so the software tier's checks run as they would on such a GPU.
_FORCE_INLINE_ bool nextgi_device_has_ray_query(bool p_device_ray_query, bool p_deny_ray_query) {
	return p_device_ray_query && !p_deny_ray_query;
}

// GodotNext: nextgi_skinned_alpha_shadows (#302). Both static RT-arena surfaces and skinned surfaces
// (whose posed vertices live in per-instance slots, #59) keep a #130 attribute stream when streams are
// enabled, reusing the mesh's static UVs and attributes where skinning deforms positions/normals.
_FORCE_INLINE_ bool nextgi_rt_surface_wants_hit_attributes(bool p_rt_eligible, bool p_rt_skinned) {
	return p_rt_eligible || p_rt_skinned;
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
	uint64_t allocated_bytes = 0;
	LocalVector<FreeRange> free_ranges;

public:
	explicit NextGIArenaAllocator(uint32_t p_chunk_size = NEXTGI_RT_CHUNK_SIZE) :
			chunk_size(p_chunk_size) {}

	Allocation allocate(uint32_t p_size);
	void free_range(const Allocation &p_allocation);
	void reset();
	// GodotNext: nextgi_hit_groups (#130). Drops the last chunks while they are wholly free (the next allocation
	// past the rest opens a new one) and returns the chunk count left.
	uint32_t release_trailing_free_chunks();

	uint32_t get_chunk_size() const { return chunk_size; }
	uint32_t get_chunk_count() const { return chunk_count; }
	uint64_t get_allocated_bytes() const { return allocated_bytes; }
};

// One BLAS geometry: a whole surface, or a triangle slice of a surface over the BLAS cap.
struct NextGIRTPartGeometry {
	uint32_t surface = 0;
	uint32_t first_triangle = 0;
	uint32_t triangle_count = 0;
	// GodotNext: nextgeo_nextgi (#77). Self-contained arena offsets for VGD proxy parts and any
	// outside-mask legacy surfaces packed into the proxy part allocation.
	bool is_proxy_surface = false;
	// GodotNext: nextgi_nextgeo_alpha_shadows (#303). True for an outside-mask legacy surface packed
	// into a proxy part (its vertices come from the legacy surface, not the VGD proxy vertex stream).
	bool is_legacy_surface = false;
	bool has_stage2_cards = false;
	uint32_t vertex_offset = 0;
	uint32_t vertex_count = 0;
	uint32_t index_offset = 0;
	uint32_t group_offset = 0;
	// GodotNext: nextgi_nextgeo_alpha_shadows (#303). Optional per-geometry #130 attribute stream
	// (VGD proxy parts and #156 native NextGeo parts); UINT32_MAX when no stream is attached.
	uint32_t attribute_chunk = UINT32_MAX;
	uint32_t attribute_offset = 0;
	float eps = 0.0f;
};

// GodotNext: nextgeo_nextgi (#77). Pure CPU planner for VGD proxy arena upload and BLAS part
// splits, gated on HWRT tier active, SUPPORTS_RAY_QUERY, and live_instances > 0.
struct NextGIRTLegacySurfaceInput {
	const float *positions = nullptr;
	uint32_t vertex_count = 0;
	const uint32_t *indices = nullptr;
	uint32_t index_count = 0;
	float eps = 0.0f;
};

struct NextGIRTProxyPartUpload {
	NextGIArenaAllocator::Allocation allocation;
	uint32_t triangle_count = 0;
	uint32_t first_surface = 0;
	uint32_t surface_count = 0;
	// GodotNext: nextgi_nextgeo_alpha_shadows (#303). Source VGD proxy vertex/index slice for lazy
	// #130 attribute stream packing (UINT32_MAX when the part carries outside-mask legacy slices only).
	uint32_t proxy_vertex_offset = UINT32_MAX;
	uint32_t proxy_vertex_count = 0;
	uint32_t proxy_index_offset = 0;
	uint32_t proxy_index_count = 0;
	LocalVector<NextGIRTPartGeometry> geometries;
	Vector<uint8_t> payload;
};

bool nextgi_rt_plan_vgd_proxy(
		bool p_hwrt_tier_active,
		bool p_has_ray_query,
		uint32_t p_live_instances,
		const RenderingServerTypes::VirtualGeometryDesc &p_desc,
		uint32_t p_mesh_surface_count,
		const uint8_t *p_alpha_excluded_surfaces,
		const NextGIRTLegacySurfaceInput *p_legacy_surfaces,
		NextGIArenaAllocator &r_allocator,
		LocalVector<NextGIRTProxyPartUpload> &r_uploads,
		uint32_t &r_alpha_excluded_count,
		LocalVector<NextGIRTProxyPartUpload> *r_shadow_uploads = nullptr);

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
	// GodotNext: nextgi_alpha_policy (#61).
	NEXTGI_RT_ALPHA_TESTED_EXCLUDED,
};

enum {
	NEXTGI_RT_BUILD_TRIANGLE_CAP = 250000, // Provisional until G0-6 (#9) measures it in PQ.
	NEXTGI_RT_REFIT_TRIANGLE_CAP = 200000,
	NEXTGI_RT_ABSENT_FRAMES = 120,
};

static constexpr float NEXTGI_RT_ANGULAR_CULL_DISTANCE = 30.0f;
static constexpr uint64_t NEXTGI_RT_AS_BUDGET_BYTES = 1024ull * 1024ull * 1024ull;

_FORCE_INLINE_ bool nextgi_rt_instance_eligible(bool p_mesh, bool p_visible, bool p_gi_contributing, bool p_skinned) {
	// GodotNext: nextgi_skinned_actors (#59). Skinned actors enter the TLAS through their posed
	// slots (refit with a 32-frame rebuild), so they are no longer reception-only.
	(void)p_skinned;
	return p_mesh && p_visible && p_gi_contributing;
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
	// GodotNext: nextgi_alpha_policy (#61).
	uint32_t alpha_excluded = 0;
	uint32_t blas_pending = 0;
};

// Integer percent. An empty denominator is 0, not a divide.
int64_t nextgi_rt_coverage_percent(const NextGIRTCoverage &p_coverage);

// GodotNext: nextgi_skinned_actors (#59). Skinned BLAS work: nearest-first within the per-frame
// triangle budget, refit every frame with a rebuild every 32 frames.
enum NextGISkinnedBuild {
	NEXTGI_SKINNED_SKIP = 0,
	NEXTGI_SKINNED_REFIT,
	NEXTGI_SKINNED_REBUILD,
};

NextGISkinnedBuild nextgi_rt_skinned_build(uint32_t p_instance_id, uint32_t p_frame, uint32_t p_triangles, uint32_t p_triangles_so_far, uint32_t p_triangle_cap);
uint32_t nextgi_rt_skinned_triangle_budget(bool p_ada_class);

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
	// GodotNext: tlas_from_buffer
	enum InstanceLayout {
		INSTANCE_LAYOUT_VULKAN_64 = 0,
		INSTANCE_LAYOUT_METAL_72 = 1,
		INSTANCE_LAYOUT_MAX,
	};
	// Expanded source of the GPU instance-record encoder. Used by the doctests; no GPU compile.
	static String build_instances_shader_source(int p_layout = INSTANCE_LAYOUT_VULKAN_64);
	// Dynamic sources share the 100k TLAS capacity with the CPU-fed instances.
	_FORCE_INLINE_ uint32_t static nextgi_rt_dynamic_budget(uint32_t p_capacity, uint32_t p_cpu_count) {
		return p_cpu_count < p_capacity ? p_capacity - p_cpu_count : 0;
	}
	// Encodes p_count elements of p_source_buffer into the GPU instance-record buffer at p_dest_offset.
	// p_visible_count of them are active; the rest keep mask 0. Returns the records written.
	// GodotNext: nextgi_shadow_policy (#145). p_mask is the active elements' TLAS mask.
	uint32_t encode_gpu_instances(RID p_source_buffer, uint32_t p_source_stride_floats, uint32_t p_count, uint32_t p_visible_count, uint32_t p_dest_offset, RID p_blas, uint32_t p_id_base, uint32_t p_flags, const Vector3 &p_camera, float p_max_distance, float p_angle_limit, uint32_t p_source_element_offset = 0, RID p_count_buffer = RID(), uint32_t p_mask = 0xFFu);
	RID get_gpu_instance_buffer() const { return gpu_instance_buffer; }
	// GodotNext: ps_mesh_sdf_atlas (#185).
	NextGISDFInstances &get_sdf_instances() { return sdf_instances; }
	const NextGISDFInstances &get_sdf_instances() const { return sdf_instances; }
	void update_global_sdf(const ::RenderDataRD *p_render_data);
	Dictionary get_global_sdf_debug() const;

	struct Stats {
		uint32_t fed_instance_count = 0;
		uint32_t instance_count = 0; // TLAS instances (and records) of the last TLAS phase.
		NextGIRTCoverage coverage;
		uint32_t blas_build_count = 0; // BLAS builds of the last builds phase; 0 on a static scene once built.
		uint32_t blas_build_triangles = 0;
		uint32_t compaction_copies = 0;
		uint32_t refit_triangles = 0;
		// GodotNext: nextgi_skinned_actors (#59).
		uint32_t skinned_instances = 0;
		uint32_t skinned_refit_triangles = 0;
		uint32_t skinned_build_triangles = 0;
		// GodotNext: nextgeo_vertex_offset (#164). Opt-in RT proxy vertex-offset evaluation and BLAS refits.
		uint32_t vertex_offset_instances = 0;
		uint32_t vertex_offset_refit_triangles = 0;
		uint32_t vertex_offset_build_triangles = 0;
		uint64_t as_bytes = 0;
		double tlas_fill_ms = 0.0;
		double scene_update_ms = 0.0;
		uint64_t tlas_build_count = 0; // One TLAS rebuild per TLAS phase with instances.
		// GodotNext: nextgi_shadow_policy (#145). TLAS instances of the last TLAS phase that only shadow rays see
		// (alpha-tested and translucent surfaces), that are non-opaque (alpha-tested), and whose layers meet no
		// allocated caster mask.
		uint32_t shadow_only_instances = 0;
		uint32_t shadow_alpha_instances = 0;
		uint32_t shadow_unallocated_instances = 0;
		// Alpha-tested geometries the bindless-texture path samples, BaseMaterial3D ones without an albedo texture
		// (a constant alpha), those whose alpha comes from shader code and the retrace evaluates, and those neither
		// path can evaluate (they shadow as opaque: a shader the hit groups refuse, a surface without its stream).
		uint32_t shadow_texture_entries = 0;
		uint32_t shadow_cut_entries = 0;
		uint32_t shadow_retrace_entries = 0;
		uint32_t shadow_opaque_alpha_entries = 0;
		// TLAS instances the GPU wrote (dynamic MultiMesh and particle elements, #57), GI and shadow-only.
		uint32_t gpu_written_instances = 0;
	};

	// GodotNext: nextgi_shadow_policy (#145). One TLAS instance GM-4's check builds its rays from: its triangles
	// come from the mesh's CPU copy (MeshStorage::mesh_nextgi_get_cpu_geometry()).
	struct ShadowCheckSource {
		Transform3D transform;
		AABB world_bounds;
		uint32_t layers = 0;
		RID mesh;
		LocalVector<uint32_t> surfaces;
		bool opaque_caster = true; // False for alpha-tested or translucent surfaces, skinned slots and GPU-written instances.
		bool has_geometry = true; // False when its triangles are not on the CPU (rays near it are left out).
	};

private:
	struct PendingCompact {
		RID blas;
		RID mesh;
		uint32_t part_index = UINT32_MAX;
		uint32_t triangles = 0;
	};

	RID tlas;
	RID gpu_instance_buffer;
	RID gpu_count_dummy;
	uint32_t gpu_instance_capacity = 0;
	struct InstanceEncoder;
	InstanceEncoder *instance_encoder = nullptr;
	bool _ensure_gpu_instance_buffer();
	RID instance_record_buffer;
	RID surface_record_buffer;
	uint32_t instance_record_capacity = 0;
	uint32_t surface_record_capacity = 0;
	Stats stats;
	NextGISDFInstances sdf_instances;
	NextGISDFStream *sdf_stream = nullptr;
	NextGIFarField *far_field = nullptr;
	uint32_t total_record_count = 0;
	NextGIGlobalSDFGPU global_sdf;
	NextGIGlobalSDFClipmaps global_sdf_clipmaps; // GodotNext: ps_swrt_trace (#186)
	Vector3i global_sdf_origin;
	uint64_t global_sdf_generation = 0;
	// GodotNext: nextgi_skinned_actors (#59). Per-instance posed BLASes keyed by (instance, surface).
	struct SkinnedPart {
		RID blas;
		RID chunk_buffer; // Arena chunk buffer the BLAS depends on; RD cascade-frees the BLAS if the chunk goes away.
		uint32_t chunk = UINT32_MAX;
		uint32_t triangles = 0;
		uint64_t last_frame = 0;
	};
	HashMap<uint64_t, SkinnedPart> skinned_parts;
	bool _skinned_part_blas_alive(const SkinnedPart &p_part) const;
	// GodotNext: nextgeo_vertex_offset (#164). Per-instance opt-in RT proxy WPO parts keyed by (instance, element, part).
	struct VertexOffsetPart {
		RID mesh;
		RID blas;
		RID chunk_buffer;
		RID bind_pose_buffer;
		NextGIArenaAllocator::Allocation allocation;
		uint32_t src_chunk = UINT32_MAX;
		uint32_t src_allocation_offset = 0;
		uint32_t src_allocation_size = 0;
		uint32_t bind_pose_vertex_count = 0;
		uint32_t proxy_pos_rel = 0;
		uint32_t triangles = 0;
		uint64_t last_frame = 0;
		LocalVector<NextGIRTPartGeometry> geometries;
	};
	HashMap<uint64_t, VertexOffsetPart> vertex_offset_parts;
	HashMap<RID, RID> vertex_offset_pipelines;
	RID vertex_offset_dummy_instance_buffer;
	bool _vertex_offset_part_blas_alive(const VertexOffsetPart &p_part) const;
	void _free_vertex_offset_part(VertexOffsetPart &p_part);
	RID _get_vertex_offset_pipeline(RID p_shader);
	// GodotNext: spline_meshes (#161). Per-instance deformed RT proxy BLASes, rebuilt when spline params change.
	struct SplinePart {
		RID blas;
		RID chunk_buffer;
		NextGIArenaAllocator::Allocation allocation;
		uint32_t triangle_count = 0;
		LocalVector<NextGIRTPartGeometry> geometries;
	};
	struct SplineEntry {
		RID mesh;
		uint64_t spline_generation = 0;
		uint64_t last_frame = 0;
		LocalVector<SplinePart> parts;
	};
	HashMap<uint64_t, SplineEntry> spline_entries;
	bool _spline_entry_alive(const SplineEntry &p_entry) const;
	void _free_spline_entry(SplineEntry &p_entry);
	bool _rebuild_spline_entry(SplineEntry &r_entry, RID p_mesh, const RenderGeometryInstanceBase *p_instance, uint32_t &r_built_blas, uint32_t &r_built_triangles);
	LocalVector<PendingCompact> pending_compactions;
	HashMap<RID, uint64_t> mesh_last_tlas_frame;
	bool as_memory_exceeded = false;
	bool tlas_traceable = false;
	uint32_t compaction_triangles_this_frame = 0;
	uint64_t as_budget_bytes = NEXTGI_RT_AS_BUDGET_BYTES;
	// GodotNext: nextgeo_rt_native (#156). Exists only in native mode (EXPERIMENTAL); the fallback
	// mode, the default, never creates it.
	NextGIRTNative *native = nullptr;
	// GodotNext: nextgi_shadow_policy (#145). The caster-mask allocation, and per TLAS custom index the shadow
	// instance word (x, y) that nextgi_shadow_ray_inc.glsl reads, with the alpha table of the non-opaque
	// instances' geometries.
	NextGIShadowMaskAllocator shadow_allocator;
	LocalVector<uint32_t> shadow_words;
	LocalVector<NextGIShadowAlphaEntry> shadow_alpha;
	RID shadow_instance_buffer;
	RID shadow_alpha_buffer;
	uint32_t shadow_instance_capacity = 0;
	uint32_t shadow_alpha_capacity = 0;
	uint32_t shadow_alpha_bits = 0;
	bool shadow_check_sources_enabled = false;
	LocalVector<ShadowCheckSource> shadow_check_sources;
	// The bindless-texture alpha path (devices with bindless tables): the albedo textures of this frame's
	// alpha-tested BaseMaterial3D casters in a table of their own, and whether a caster waits for its surface's
	// attribute stream (NextGI then keeps the streams).
	struct ShadowTextureSlot {
		uint32_t slot = 0;
		uint64_t frame = 0;
	};
	// What the alpha path knows about an alpha-tested material's shader code (ShaderData::nextgi_hit_serial, unique
	// per code): whether BaseMaterial3D's texture alpha takes the bindless path, and the retrace verdict
	// (next_gi_shadow_policy.h). Kept while the material was in a TLAS phase of the last
	// NEXTGI_SHADOW_HYSTERESIS_FRAMES.
	struct ShadowMaterialAlpha {
		uint64_t serial = 0;
		uint64_t frame = 0; // The last TLAS phase (shadow_frame) that used it.
		bool texture_alpha = false;
		// GodotNext: nextgi_shadow_dual_route (#307). True when ShaderCompiler saw a discard statement in this
		// shader: BaseMaterial3D-pattern shaders that also discard take the dual route (retrace + texture alpha).
		bool uses_discard = false;
		bool hash = false;
		uint32_t sampler_flags = 0; // NEXTGI_SHADOW_ALPHA_NEAREST and NEXTGI_SHADOW_ALPHA_CLAMP.
		NextGIShadowRetraceVerdict verdict = NEXTGI_SHADOW_VERDICT_UNKNOWN;
	};
	RID shadow_texture_table;
	RID shadow_dummy_buffer;
	bool shadow_texture_table_failed = false;
	HashMap<RID, ShadowTextureSlot> shadow_texture_slots;
	HashMap<RID, ShadowMaterialAlpha> shadow_material_alpha;
	bool shadow_streams_wanted = false;
	uint64_t shadow_frame = 0;
	// The last TLAS phase (shadow_frame) with a caster that needed the attribute streams, the retrace (and so the
	// hit groups), or the relight's alpha variant. NextGI keeps each of them for NEXTGI_SHADOW_HYSTERESIS_FRAMES
	// TLAS phases after it, so a caster missing from a phase or two (a BLAS rebuild, a cap, a hidden frame) does not
	// free and rebuild them.
	uint64_t shadow_streams_frame = 0;
	uint64_t shadow_retrace_frame = 0;
	uint64_t shadow_variant_frame = 0;
	// Shadow instance words out of step with the TLAS custom indices (never expected; logged once).
	uint64_t shadow_word_misaligned = 0;
	bool _shadow_held(uint64_t p_frame) const { return p_frame != 0 && shadow_frame - p_frame < uint64_t(NEXTGI_SHADOW_HYSTERESIS_FRAMES); }
	void _check_shadow_word(uint32_t p_custom_index_end);
	uint32_t _shadow_texture_slot(RID p_rd_texture);
	void _release_shadow_texture_slots();
	// p_retrace: the instance has hit records, so shader-code alpha can go to the retrace (not a GPU-written one).
	// p_index_chunk != UINT32_MAX overrides the surface's static index location (posed skinned slots #302, NextGeo #303).
	// p_override_attributes uses p_attribute_chunk/p_attribute_offset instead of MeshStorage's per-surface stream (#303).
	void _fill_shadow_alpha_entry(RID p_material, RID p_mesh, uint32_t p_surface, uint32_t p_first_triangle, bool p_retrace, NextGIShadowAlphaEntry &r_entry, uint32_t p_index_chunk = UINT32_MAX, uint32_t p_index_offset = 0, bool p_override_attributes = false, uint32_t p_attribute_chunk = UINT32_MAX, uint32_t p_attribute_offset = 0, bool p_can_have_stream = true);
	void _update_shadow_allocation(const ::RenderDataRD *p_render_data);
	// One shadow instance word per TLAS instance, in TLAS order. p_alpha_base is the first alpha table entry of a
	// non-opaque instance, or UINT32_MAX.
	void _push_shadow_word(uint32_t p_layers, uint32_t p_alpha_base);
	// The alpha table entries of a part's geometries when one is alpha-tested (for the instance's effective
	// materials). Returns the first entry, or UINT32_MAX for an opaque instance (nothing is pushed).
	uint32_t _push_shadow_alpha(RenderGeometryInstance *p_instance, RID p_mesh, const LocalVector<NextGIRTPartGeometry> &p_geometries, uint32_t p_layers, bool p_retrace, uint32_t p_part_chunk = UINT32_MAX, bool p_native_parts = false);
	// The word of a part's TLAS instance, and its alpha table entries when a geometry is alpha-tested. Returns the
	// first entry, or UINT32_MAX for an opaque instance.
	uint32_t _push_shadow_part(RenderGeometryInstance *p_instance, RID p_mesh, const LocalVector<NextGIRTPartGeometry> &p_geometries, uint32_t p_layers, uint32_t p_part_chunk = UINT32_MAX, bool p_native_parts = false);
	// GodotNext: nextgi_skinned_alpha_shadows (#302). Shadow word and optional alpha entry for a posed skinned slot.
	uint32_t _push_shadow_skinned(RenderGeometryInstance *p_instance, RID p_mesh, uint32_t p_surface, uint32_t p_slot_chunk, uint32_t p_slot_index_offset, uint32_t p_layers);
	void _push_shadow_check_source(RenderGeometryInstance *p_instance, RID p_mesh, const LocalVector<NextGIRTPartGeometry> &p_geometries, const Transform3D &p_transform, uint32_t p_layers);
	void _upload_shadow_buffers();

public:
	// GodotNext: path_tracer (#167). What a surface record of the last TLAS phase draws: its geometry instance, mesh
	// and surface. Records without one (far proxies) keep the defaults.
	struct SurfaceSource {
		RenderGeometryInstance *instance = nullptr;
		RID mesh;
		uint32_t surface = UINT32_MAX;
	};
	// GodotNext: path_tracer_lights_camera (#169). Per-TLAS-instance data captured while a path tracer wants
	// surface sources so PathTracer can build a shutter-interpolated motion TLAS when any GI instance moves.
	struct MotionInstance {
		RenderGeometryInstance *instance = nullptr;
		RID blas;
		Transform3D transform;
		Transform3D prev_transform;
		NextGIInstanceRecord record;
		uint32_t tlas_mask = 0;
		uint32_t tlas_flags = 0;
		int64_t hit_sbt_range = 0;
		bool has_prev_transform = false;
	};

private:
	LocalVector<SurfaceSource> surface_sources;
	LocalVector<MotionInstance> motion_instances;
	uint64_t records_hash = 0;
	bool surface_sources_wanted = false;
	bool surface_sources_valid = false;
	// GodotNext: nextgi_hit_lighting (#129). When true, alpha-blended instances enter the TLAS with
	// mask bit 1 (NEXTGI_RT_MASK_TRANSLUCENT) and SRC cards so hit-lighting reflections skip and tint through them.
	bool hit_lighting_translucent_enabled = false;
	uint32_t translucent_instance_count = 0;
	void _finish_surface_sources(const LocalVector<NextGIInstanceRecord> &p_records, const LocalVector<NextGISurfaceRecord> &p_surfaces);

	void _build_dirty_mesh_parts(RID p_mesh);
	void _build_proxy_part(RID p_mesh, uint32_t p_part_index, bool p_is_shadow_part = false);
	void _ensure_record_buffers(uint32_t p_instance_count, uint32_t p_surface_count);
	uint32_t _mesh_triangle_count(RID p_mesh) const;
	void _apply_as_budget(uint64_t p_frame);
	void _update_far_records(const ::RenderDataRD *p_render_data, LocalVector<NextGIInstanceRecord> &r_records, LocalVector<NextGISurfaceRecord> &r_surfaces);
	// GodotNext: nextgi_src_page_levels (#247). Records waiting for relight pages: the pass assigns
	// them after its last record, in descending solid angle.
	struct PendingPages {
		RenderGeometryInstance *instance = nullptr;
		uint32_t record = 0;
		uint32_t element = UINT32_MAX;
		float solid_angle = 0.0f;
	};
	LocalVector<PendingPages> pending_pages;
	void _queue_pages(RenderGeometryInstance *p_instance, NextGIInstanceRecord &r_record, uint32_t p_element);
	void _assign_pending_pages(const ::RenderDataRD *p_render_data, LocalVector<NextGIInstanceRecord> &r_records);

public:
	void init();
	void free_far_field();
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
	RID build_far_proxy_mesh(RID p_mesh);
	bool append_far_proxy(RID p_mesh, RenderGeometryInstance *p_instance, const Transform3D &p_transform, LocalVector<NextGIInstanceRecord> &r_records, LocalVector<NextGISurfaceRecord> &r_surfaces, RD::AccelerationStructureInstance &r_tlas_instance);
	NextGIFarField *get_far_field() const { return far_field; }
	uint32_t get_total_record_count() const { return total_record_count; }
	RID get_instance_record_buffer() const { return instance_record_buffer; }
	RID get_surface_record_buffer() const { return surface_record_buffer; }
	const Stats &get_stats() const { return stats; }
	bool is_as_memory_exceeded() const { return as_memory_exceeded; }
	// GodotNext: nextgeo_rt_native (#156). nullptr in the fallback mode.
	const NextGIRTNative *get_native() const { return native; }
	// GodotNext: nextgi_shadow_policy (#145). Valid after this frame's TLAS phase.
	const NextGIShadowMaskAllocator::Allocation &get_shadow_allocation() const { return shadow_allocator.get_allocation(); }
	uint32_t get_shadow_alpha_bits() const { return shadow_alpha_bits; }
	// The light word trace_shadow() takes for a light with this caster mask, this frame.
	uint32_t get_shadow_light_mode(uint32_t p_caster_mask) const { return nextgi_shadow_light_mode(shadow_allocator.get_allocation(), p_caster_mask, shadow_alpha_bits); }
	RID get_shadow_instance_buffer() const { return shadow_instance_buffer; }
	RID get_shadow_alpha_buffer() const { return shadow_alpha_buffer; }
	// NextGI's bindless texture table, made on first use; invalid on a device without bindless tables. The shadow rays'
	// alpha path and the material hit groups (NextGIHitMaterials) allocate their texture slots from it: Direct3D 12
	// holds two texture or storage-buffer tables at a time, and the hit groups need a parameter table too.
	RID ensure_shadow_texture_table();
	// The other uniforms of the alpha path (nextgi_shadow_ray_inc.glsl with NEXTGI_SHADOW_ALPHA_TEXTURES): the first
	// NEXTGI_SHADOW_ALPHA_CHUNKS arena chunks (a dummy for a missing chunk) and a linear repeating sampler. Returns a
	// key of the resources, which changes when a set must be made again.
	uint64_t append_shadow_alpha_uniforms(uint32_t p_chunk_binding, uint32_t p_sampler_binding, LocalVector<RD::Uniform> &r_uniforms);
	// A 16-byte storage buffer for bindings a pass does not use.
	RID get_shadow_dummy_buffer();
	// An alpha-tested BaseMaterial3D caster with an albedo texture was in a recent TLAS phase (the last
	// NEXTGI_SHADOW_HYSTERESIS_FRAMES): the bindless path reads its UVs from the attribute streams.
	bool wants_shadow_streams() const { return _shadow_held(shadow_streams_frame); }
	// An alpha-tested caster whose alpha comes from shader code, with a retrace verdict that is not OPAQUE, was in a
	// recent TLAS phase: the retrace needs the material hit groups (to evaluate it, or to learn the verdict).
	bool wants_shadow_retrace() const { return _shadow_held(shadow_retrace_frame); }
	// A recent TLAS phase had a caster that only the relight's alpha variant evaluates (a sampled texture, a
	// retrace, or a texture waiting for its stream). Without one the relight runs its baked variant.
	bool wants_shadow_alpha_variant() const { return _shadow_held(shadow_variant_frame); }
	bool has_shadow_texture_table() const { return shadow_texture_table.is_valid(); }
	uint64_t get_shadow_word_misaligned() const { return shadow_word_misaligned; }
	// GM-4's check asks for the sources of its rays; the TLAS phase records them only then.
	void set_shadow_check_sources_enabled(bool p_enabled) { shadow_check_sources_enabled = p_enabled; }
	const LocalVector<ShadowCheckSource> &get_shadow_check_sources() const { return shadow_check_sources; }
	// True only after this frame's TLAS build, and only while line 1b has not rebuilt a BLAS the TLAS references.
	bool is_tlas_traceable() const { return tlas_traceable; }
	// GodotNext: path_tracer (#167). One SurfaceSource per surface record of the last TLAS phase, in record order,
	// and a hash of the records' transforms and geometry (not their relight pages or card sets, which change while
	// the surface cache fills). The TLAS phase collects them only while a path tracer wants them (set before the
	// phase); has_surface_sources() is true when this frame's phase collected them.
	void set_surface_sources_wanted(bool p_wanted) { surface_sources_wanted = p_wanted; }
	bool has_surface_sources() const { return surface_sources_valid; }
	const LocalVector<SurfaceSource> &get_surface_sources() const { return surface_sources; }
	const LocalVector<MotionInstance> &get_motion_instances() const { return motion_instances; }
	uint64_t get_records_hash() const { return records_hash; }
	void set_as_budget_bytes(uint64_t p_bytes) { as_budget_bytes = p_bytes; }
	// GodotNext: nextgi_hit_lighting (#129).
	void set_hit_lighting_translucent_enabled(bool p_enabled) { hit_lighting_translucent_enabled = p_enabled; }
	bool is_hit_lighting_translucent_enabled() const { return hit_lighting_translucent_enabled; }
	uint32_t get_translucent_instance_count() const { return translucent_instance_count; }

	NextGIRTScene() {}
	~NextGIRTScene() {}
};

} // namespace RendererRD
