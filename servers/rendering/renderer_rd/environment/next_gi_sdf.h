/**************************************************************************/
/*  next_gi_sdf.h                                                         */
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
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,         */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/templates/local_vector.h"

// GodotNext: ps_mesh_sdf_atlas (#185, plan §21.2 L3)
//
// The mesh-SDF brick pool. Mesh SDFs are stored as sparse 8^3 R8 bricks
// (core/math/mesh_sdf_builder.h) and streamed into one atlas. Bricks never
// move once allocated, so GPU page tables stay valid until a mesh's SDF is
// replaced or freed. This class is the CPU allocator; the renderer owns the
// R8 3D texture that mirrors the slots.
class NextGISDFPool {
public:
	static constexpr uint32_t BRICK_SIZE = 8;
	static constexpr uint32_t BRICK_VOXELS = BRICK_SIZE * BRICK_SIZE * BRICK_SIZE;
	static constexpr uint32_t BRICK_BYTES = BRICK_VOXELS;
	static constexpr uint32_t DEFAULT_POOL_MB = 256;

	explicit NextGISDFPool(uint32_t p_size_mb = DEFAULT_POOL_MB);

	// Allocates p_count bricks and writes their pool slots. All-or-nothing:
	// a failed allocation leaves the pool unchanged.
	bool allocate(uint32_t p_count, LocalVector<uint32_t> &r_slots);
	void free(const LocalVector<uint32_t> &p_slots);
	void free_slot(uint32_t p_slot);

	bool is_allocated(uint32_t p_slot) const;
	uint32_t get_capacity() const { return capacity; }
	uint32_t get_free_count() const { return capacity - used; }
	uint32_t get_used_count() const { return used; }
	uint32_t get_size_mb() const { return size_mb; }
	uint64_t get_size_bytes() const { return uint64_t(size_mb) * 1024ull * 1024ull; }
	// One mip's page table. Entries are pool slots or MeshSDFData::PAGE_* sentinels.
	static constexpr uint32_t PAGE_UNALLOCATED = UINT32_MAX;

private:
	uint32_t size_mb = DEFAULT_POOL_MB;
	uint32_t capacity = 0;
	uint32_t used = 0;
	LocalVector<uint32_t> free_list;
	LocalVector<uint8_t> slots; // 1 = allocated, for validation and tests.
};
