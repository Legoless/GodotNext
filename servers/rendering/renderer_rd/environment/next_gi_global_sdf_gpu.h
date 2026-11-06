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

// GodotNext: ps_global_sdf (#182, plan §21.2 L4)
//
// The R16F brick pool and per-level 16^3 page tables. Bricks are 8^3 half
// floats (1 KiB); the CPU mirror keeps the packed bytes for tests and the
// RD buffers are mirrored on upload. A page table entry is a pool slot or
// PAGE_UNALLOCATED. The compositing compute writes through the same tables.
class NextGIGlobalSDFGPU {
public:
	static constexpr uint32_t BRICKS_PER_LEVEL = NextGIGlobalSDF::BRICKS_PER_LEVEL;
	static constexpr uint32_t BRICK_BYTES = NextGIGlobalSDF::BRICK_VOXELS * 2; // R16F.
	static constexpr uint32_t DEFAULT_POOL_BRICKS = 16384; // 16 MiB.
	static constexpr int32_t PAGE_UNALLOCATED = -1;

	explicit NextGIGlobalSDFGPU(uint32_t p_pool_bricks = DEFAULT_POOL_BRICKS);
	~NextGIGlobalSDFGPU();

	// Creates the RD buffers when a device is present. Safe to call repeatedly.
	void ensure_buffers();
	// Packs and uploads one brick, allocating a pool slot on first use.
	bool upload_brick(uint32_t p_level, const Vector3i &p_brick, const float *p_voxels);
	void release_level(uint32_t p_level);
	void free_all();

	int32_t get_slot(uint32_t p_level, const Vector3i &p_brick) const;
	bool has_brick(uint32_t p_level, const Vector3i &p_brick) const;
	uint32_t get_used_bricks() const { return used; }
	uint32_t get_capacity() const { return capacity; }
	const PackedInt32Array &get_page_table(uint32_t p_level) const { return page_tables[p_level]; }
	RID get_pool_buffer() const { return pool_buffer; }
	RID get_page_buffer(uint32_t p_level) const { return p_level < page_buffers.size() ? page_buffers[p_level] : RID(); }
	// Reads back the packed half of one voxel from the CPU mirror.
	float get_voxel(uint32_t p_level, const Vector3i &p_brick, const Vector3i &p_voxel) const;

private:
	static uint32_t _table_index(const Vector3i &p_brick);
	bool _allocate_slot(uint32_t &r_slot);
	void _free_slot(uint32_t p_slot);

	uint32_t capacity = 0;
	uint32_t used = 0;
	PackedByteArray pool_data;
	Vector<PackedInt32Array> page_tables;
	Vector<uint8_t> slot_used;
	Vector<int32_t> free_slots;
	RID pool_buffer;
	Vector<RID> page_buffers;
};
