/**************************************************************************/
/*  next_gi_global_sdf_gpu.cpp                                            */
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

#include "next_gi_global_sdf_gpu.h"

#include "core/math/math_funcs.h"
#include "servers/rendering/rendering_device.h"

NextGIGlobalSDFGPU::NextGIGlobalSDFGPU(uint32_t p_pool_bricks) {
	capacity = MAX(p_pool_bricks, 1u);
	pool_data.resize(capacity * BRICK_BYTES);
	pool_data.fill(0);
	slot_used.resize(capacity);
	free_slots.resize(capacity);
	for (uint32_t index = 0; index < capacity; index++) {
		slot_used.set(index, 0);
		free_slots.set(index, int32_t(capacity - 1 - index));
	}
	page_tables.resize(NextGIGlobalSDF::MAX_LEVELS);
	for (uint32_t level = 0; level < NextGIGlobalSDF::MAX_LEVELS; level++) {
		PackedInt32Array &table = page_tables.write[level];
		table.resize(BRICKS_PER_LEVEL);
		table.fill(PAGE_UNALLOCATED);
	}
	page_buffers.resize(NextGIGlobalSDF::MAX_LEVELS);
}

NextGIGlobalSDFGPU::~NextGIGlobalSDFGPU() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr) {
		if (pool_buffer.is_valid()) {
			rd->free_rid(pool_buffer);
		}
		for (const RID &buffer : page_buffers) {
			if (buffer.is_valid()) {
				rd->free_rid(buffer);
			}
		}
	}
	pool_buffer = RID();
	page_buffers.clear();
}

uint32_t NextGIGlobalSDFGPU::_table_index(const Vector3i &p_brick) {
	return uint32_t(p_brick.x) + uint32_t(p_brick.y) * NextGIGlobalSDF::PAGE_SIZE + uint32_t(p_brick.z) * NextGIGlobalSDF::PAGE_SIZE * NextGIGlobalSDF::PAGE_SIZE;
}

void NextGIGlobalSDFGPU::ensure_buffers() {
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr) {
		return;
	}
	if (!pool_buffer.is_valid()) {
		pool_buffer = rd->storage_buffer_create(capacity * BRICK_BYTES);
		if (pool_buffer.is_valid()) {
			rd->buffer_update(pool_buffer, 0, pool_data.size(), pool_data.ptr());
		}
	}
	for (uint32_t level = 0; level < NextGIGlobalSDF::MAX_LEVELS; level++) {
		if (page_buffers[level].is_valid()) {
			continue;
		}
		page_buffers.write[level] = rd->storage_buffer_create(BRICKS_PER_LEVEL * sizeof(int32_t));
		if (page_buffers[level].is_valid()) {
			rd->buffer_update(page_buffers[level], 0, page_tables[level].size() * sizeof(int32_t), page_tables[level].ptr());
		}
	}
}

bool NextGIGlobalSDFGPU::_allocate_slot(uint32_t &r_slot) {
	if (free_slots.is_empty()) {
		return false;
	}
	r_slot = uint32_t(free_slots[free_slots.size() - 1]);
	free_slots.remove_at(free_slots.size() - 1);
	slot_used.set(r_slot, 1);
	used++;
	return true;
}

void NextGIGlobalSDFGPU::_free_slot(uint32_t p_slot) {
	if (p_slot >= capacity || slot_used[p_slot] == 0) {
		return;
	}
	slot_used.set(p_slot, 0);
	free_slots.push_back(int32_t(p_slot));
	used--;
}

bool NextGIGlobalSDFGPU::upload_brick(uint32_t p_level, const Vector3i &p_brick, const float *p_voxels) {
	if (p_level >= NextGIGlobalSDF::MAX_LEVELS || p_voxels == nullptr) {
		return false;
	}
	if (p_brick.x < 0 || p_brick.y < 0 || p_brick.z < 0 ||
			p_brick.x >= int(NextGIGlobalSDF::PAGE_SIZE) ||
			p_brick.y >= int(NextGIGlobalSDF::PAGE_SIZE) ||
			p_brick.z >= int(NextGIGlobalSDF::PAGE_SIZE)) {
		return false;
	}
	const uint32_t index = _table_index(p_brick);
	PackedInt32Array &table = page_tables.write[p_level];
	uint32_t slot = 0;
	if (table[index] >= 0) {
		slot = uint32_t(table[index]);
	} else if (!_allocate_slot(slot)) {
		return false;
	}

	const uint32_t byte_offset = slot * BRICK_BYTES;
	uint16_t *packed = reinterpret_cast<uint16_t *>(pool_data.ptrw() + byte_offset);
	for (uint32_t voxel = 0; voxel < NextGIGlobalSDF::BRICK_VOXELS; voxel++) {
		packed[voxel] = Math::make_half_float(p_voxels[voxel]);
	}
	table.set(index, int32_t(slot));

	ensure_buffers();
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr && pool_buffer.is_valid()) {
		rd->buffer_update(pool_buffer, byte_offset, BRICK_BYTES, pool_data.ptr() + byte_offset);
		if (page_buffers[p_level].is_valid()) {
			rd->buffer_update(page_buffers[p_level], index * sizeof(int32_t), sizeof(int32_t), table.ptr() + index);
		}
	}
	return true;
}

void NextGIGlobalSDFGPU::release_level(uint32_t p_level) {
	if (p_level >= NextGIGlobalSDF::MAX_LEVELS) {
		return;
	}
	PackedInt32Array &table = page_tables.write[p_level];
	for (int index = 0; index < table.size(); index++) {
		if (table[index] >= 0) {
			_free_slot(uint32_t(table[index]));
			table.set(index, PAGE_UNALLOCATED);
		}
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr && page_buffers[p_level].is_valid()) {
		rd->buffer_update(page_buffers[p_level], 0, table.size() * sizeof(int32_t), table.ptr());
	}
}

void NextGIGlobalSDFGPU::free_all() {
	for (uint32_t level = 0; level < NextGIGlobalSDF::MAX_LEVELS; level++) {
		release_level(level);
	}
}

int32_t NextGIGlobalSDFGPU::get_slot(uint32_t p_level, const Vector3i &p_brick) const {
	if (p_level >= NextGIGlobalSDF::MAX_LEVELS || p_brick.x < 0 || p_brick.y < 0 || p_brick.z < 0 ||
			p_brick.x >= int(NextGIGlobalSDF::PAGE_SIZE) || p_brick.y >= int(NextGIGlobalSDF::PAGE_SIZE) || p_brick.z >= int(NextGIGlobalSDF::PAGE_SIZE)) {
		return PAGE_UNALLOCATED;
	}
	return page_tables[p_level][_table_index(p_brick)];
}

bool NextGIGlobalSDFGPU::has_brick(uint32_t p_level, const Vector3i &p_brick) const {
	return get_slot(p_level, p_brick) >= 0;
}

float NextGIGlobalSDFGPU::get_voxel(uint32_t p_level, const Vector3i &p_brick, const Vector3i &p_voxel) const {
	const int32_t slot = get_slot(p_level, p_brick);
	if (slot < 0 || p_voxel.x < 0 || p_voxel.y < 0 || p_voxel.z < 0 ||
			p_voxel.x >= int(NextGIGlobalSDF::BRICK_SIZE) || p_voxel.y >= int(NextGIGlobalSDF::BRICK_SIZE) || p_voxel.z >= int(NextGIGlobalSDF::BRICK_SIZE)) {
		return 1e30f;
	}
	const uint32_t index = uint32_t(p_voxel.x) + uint32_t(p_voxel.y) * NextGIGlobalSDF::BRICK_SIZE + uint32_t(p_voxel.z) * NextGIGlobalSDF::BRICK_SIZE * NextGIGlobalSDF::BRICK_SIZE;
	const uint16_t *packed = reinterpret_cast<const uint16_t *>(pool_data.ptr() + uint32_t(slot) * BRICK_BYTES);
	return Math::half_to_float(packed[index]);
}
