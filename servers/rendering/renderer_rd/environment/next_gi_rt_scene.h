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

// The CPU-fed TLAS and the GPU instance/surface records. Owned by NextGI; created only under the
// HWRT tier. update() consumes the v0 feed in RenderSDFGIUpdateData::nextgi_rt_instances; the P1b
// registry (#39) replaces that feed.
class NextGIRTScene {
public:
	struct Stats {
		uint32_t fed_instance_count = 0;
		uint32_t instance_count = 0; // TLAS instances (and records) of the last update.
		uint32_t blas_build_count = 0; // BLAS builds of the last update; 0 on a static scene after frame 1.
		uint64_t tlas_build_count = 0; // One TLAS rebuild per update with instances.
	};

private:
	RID tlas;
	RID instance_record_buffer;
	RID surface_record_buffer;
	uint32_t instance_record_capacity = 0;
	uint32_t surface_record_capacity = 0;
	Stats stats;

	void _build_dirty_mesh_parts(RID p_mesh);
	void _ensure_record_buffers(uint32_t p_instance_count, uint32_t p_surface_count);

public:
	void init();
	void free();
	void update(const ::RenderDataRD *p_render_data);

	RID get_tlas() const { return tlas; }
	RID get_instance_record_buffer() const { return instance_record_buffer; }
	RID get_surface_record_buffer() const { return surface_record_buffer; }
	const Stats &get_stats() const { return stats; }

	NextGIRTScene() {}
	~NextGIRTScene() {}
};

} // namespace RendererRD
