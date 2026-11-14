/**************************************************************************/
/*  next_gi_global_sdf_gpu.h                                              */
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

#include "core/templates/rid.h"
#include "core/templates/vector.h"
#include "servers/rendering/renderer_rd/environment/next_gi_global_sdf.h"
#include "servers/rendering/renderer_rd/shader_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_global_sdf.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_global_sdf_coarse.glsl.gen.h"

// GodotNext: ps_global_sdf (#182, plan §21.2 L4)
//
// The R16F brick pool and per-level 16^3 page tables. Bricks are 8^3 half
// floats (1 KiB); the CPU mirror keeps the packed bytes for tests and the
// RD buffers are mirrored on upload. A page table entry is a pool slot,
// PAGE_UNALLOCATED (no instance's mesh SDF comes near the brick: empty space)
// or PAGE_MISSING (GodotNext: ps_swrt_trace (#186): the brick holds geometry
// but the pool had no slot for it; the trace reads the next level there). The
// compositing compute writes through the same tables.
class NextGIGlobalSDFGPU {
public:
	static constexpr uint32_t BRICKS_PER_LEVEL = NextGIGlobalSDF::BRICKS_PER_LEVEL;
	static constexpr uint32_t BRICK_BYTES = NextGIGlobalSDF::BRICK_VOXELS * 2; // R16F.
	static constexpr uint32_t COVERAGE_BYTES = NextGIGlobalSDF::BRICK_VOXELS / 8;
	static constexpr uint32_t DEFAULT_POOL_BRICKS = 16384; // 16 MiB.
	static constexpr int32_t PAGE_UNALLOCATED = -1;
	static constexpr int32_t PAGE_MISSING = -2; // GodotNext: ps_swrt_trace (#186)

	explicit NextGIGlobalSDFGPU(uint32_t p_pool_bricks = DEFAULT_POOL_BRICKS);
	~NextGIGlobalSDFGPU();

	// Creates the RD buffers when a device is present. Safe to call repeatedly.
	void ensure_buffers();
	// Packs and uploads one brick, allocating a pool slot on first use.
	bool upload_brick(uint32_t p_level, const Vector3i &p_brick, const float *p_voxels);
	// Composites the selected bricks through the compute pipeline.
	bool update_bricks(const Vector<NextGIGlobalSDF::Instance> &p_instances, uint32_t p_level, const Vector3i &p_origin, const Vector3i *p_bricks, uint32_t p_count);
	// Reads one static/dynamic/coverage brick back from the GPU.
	bool download_brick(uint32_t p_level, const Vector3i &p_brick, NextGIGlobalSDF::BrickSplit &r_split) const;
	bool download_coarse(uint32_t p_level, LocalVector<float> &r_coarse) const;
	// GodotNext: global_sdf_coarse_bound (#257). Checks a level's GPU coarse bound against its CPU
	// twin, built from the GPU page table and pools (r_twin_mismatches), and against the
	// distance to the nearest instance at p_count random points of the level, unallocated
	// cells and edges included (r_overestimates: points where the bound reads more than
	// p_tolerance past it).
	bool validate_coarse(const Vector<NextGIGlobalSDF::Instance> &p_instances, uint32_t p_level, const Vector3i &p_origin, uint32_t p_count, float p_tolerance, uint32_t &r_checked, uint32_t &r_overestimates, uint32_t &r_twin_mismatches) const;
	// Compares p_count random voxels of a level with the CPU composite
	// (NextGIGlobalSDF::composite_distance()). GodotNext: ps_swrt_trace (#186): r_overestimates
	// counts resident voxels that read more than a quarter voxel past the minimum over every
	// instance; for box-like meshes, whose CPU distance is exact, that is a step past a surface.
	bool validate_reference(const Vector<NextGIGlobalSDF::Instance> &p_instances, uint32_t p_level, const Vector3i &p_origin, uint32_t p_count, float &r_max_error, uint32_t &r_sign_mismatches, uint32_t &r_checked, uint32_t *r_overestimates = nullptr) const;
	// GodotNext: ps_swrt_trace (#186). Reads the coverage words of a level's resident
	// bricks and compares every bit with the CPU rule (a voxel is covered when a two-sided
	// instance wins its static/dynamic class, the static class's brick cap included).
	// Bricks without a two-sided instance must be empty; up to p_max_bricks bricks with one
	// are sampled. Voxels where the winner is within p_tie of another distance are skipped.
	bool validate_coverage(const Vector<NextGIGlobalSDF::Instance> &p_instances, uint32_t p_level, const Vector3i &p_origin, uint32_t p_max_bricks, float p_tie, uint32_t &r_checked, uint32_t &r_expected_set, uint32_t &r_mismatches) const;
	// The coverage word and bit of a voxel (brick-local index x + 8y + 64z) in a slot.
	static uint32_t coverage_word(uint32_t p_slot, uint32_t p_voxel_index) { return p_slot * (COVERAGE_BYTES / 4) + p_voxel_index / 32; }
	static uint32_t coverage_bit(uint32_t p_voxel_index) { return 1u << (p_voxel_index & 31u); }
	void release_level(uint32_t p_level);
	void scroll_level(uint32_t p_level, const Vector3i &p_delta_bricks);
	void free_all();

	int32_t get_slot(uint32_t p_level, const Vector3i &p_brick) const;
	bool has_brick(uint32_t p_level, const Vector3i &p_brick) const;
	uint32_t get_used_bricks() const { return used; }
	// GodotNext: ps_swrt_trace (#186). The last update_bricks() ran out of pool slots.
	bool is_allocation_failed() const { return last_allocation_failed; }
	// Marks a brick that holds geometry but got no pool slot (PAGE_MISSING). A brick
	// with a slot keeps it.
	void mark_missing(uint32_t p_level, const Vector3i &p_brick);
	// The PAGE_MISSING bricks of a level: a level with any is partial.
	uint32_t get_missing_bricks(uint32_t p_level) const { return p_level < NextGIGlobalSDF::MAX_LEVELS ? missing_bricks[p_level] : 0; }
	// The pool buffers exist (ensure_buffers() succeeded on a device).
	bool has_buffers() const { return pool_buffer.is_valid() && dynamic_pool_buffer.is_valid() && coverage_buffer.is_valid(); }
	uint32_t get_capacity() const { return capacity; }
	const PackedInt32Array &get_page_table(uint32_t p_level) const { return page_tables[p_level]; }
	RID get_pool_buffer() const { return pool_buffer; }
	RID get_dynamic_pool_buffer() const { return dynamic_pool_buffer; }
	RID get_coverage_buffer() const { return coverage_buffer; }
	RID get_page_buffer(uint32_t p_level) const { return p_level < page_buffers.size() ? page_buffers[p_level] : RID(); }
	// GodotNext: global_sdf_coarse_bound (#257). Every level's coarse bound, COARSE_VOXELS floats
	// per level (zeros, so no step, until a level's first update): the trace's binding 30.
	RID get_coarse_buffer() const { return coarse_bound_buffer; }
	// Reads back the packed half of one voxel from the CPU mirror.
	float get_voxel(uint32_t p_level, const Vector3i &p_brick, const Vector3i &p_voxel) const;

private:
	static uint32_t _table_index(const Vector3i &p_brick);
	bool _allocate_slot(uint32_t &r_slot);
	void _free_slot(uint32_t p_slot);
	bool _ensure_pipeline();
	bool _update_coarse(uint32_t p_level);
	uint32_t _allocate_brick(uint32_t p_level, const Vector3i &p_brick);
	void _release_brick(uint32_t p_level, const Vector3i &p_brick);
	void _recount_missing(uint32_t p_level);

	uint32_t capacity = 0;
	uint32_t used = 0;
	bool last_allocation_failed = false; // GodotNext: ps_swrt_trace (#186)
	uint32_t missing_bricks[NextGIGlobalSDF::MAX_LEVELS] = {}; // GodotNext: ps_swrt_trace (#186)
	mutable PackedByteArray pool_data;
	Vector<PackedInt32Array> page_tables;
	Vector<uint8_t> slot_used;
	Vector<int32_t> free_slots;
	RID pool_buffer;
	RID dynamic_pool_buffer;
	RID coverage_buffer;
	Vector<RID> page_buffers;
	// GodotNext: global_sdf_coarse_bound (#257). The bound of every level, and the seed and
	// flood scratch one level's update uses.
	RID coarse_bound_buffer;
	RID coarse_seed_buffer;
	RID coarse_flood_a;
	RID coarse_flood_b;
	Vector<RID> coarse_uniforms;
	// A level's page table changed since its bound was built (a scroll or a release), so an
	// update with no brick to build still rebuilds it.
	bool coarse_dirty[NextGIGlobalSDF::MAX_LEVELS] = { true, true, true, true, true, true, true, true, true, true };

	NextgiGlobalSdfShaderRD shader;
	RID version;
	RID pipeline;
	bool pipeline_failed = false;
	NextgiGlobalSdfCoarseShaderRD coarse_shader;
	RID coarse_version;
	RID coarse_pipeline;
	bool coarse_pipeline_failed = false;
};
