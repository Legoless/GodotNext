/**************************************************************************/
/*  next_gi_sdf.cpp                                                       */
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

#include "next_gi_sdf.h"

#include "core/math/mesh_sdf_builder.h"
#include "servers/rendering/rendering_device.h"

NextGISDFPool::NextGISDFPool(uint32_t p_size_mb) {
	size_mb = MAX(p_size_mb, 1u);
	const uint64_t total = get_size_bytes() / BRICK_BYTES;
	capacity = uint32_t(MIN(total, uint64_t(UINT32_MAX)));
	free_list.resize(capacity);
	for (uint32_t index = 0; index < capacity; index++) {
		free_list[index] = capacity - 1 - index; // Pop from the end.
	}
	slots.resize(capacity);
	memset(slots.ptr(), 0, capacity);
}

bool NextGISDFPool::allocate(uint32_t p_count, LocalVector<uint32_t> &r_slots) {
	r_slots.clear();
	if (p_count == 0) {
		return true;
	}
	if (p_count > get_free_count()) {
		return false;
	}
	r_slots.resize(p_count);
	for (uint32_t index = 0; index < p_count; index++) {
		const uint32_t slot = free_list[free_list.size() - 1];
		free_list.remove_at(free_list.size() - 1);
		slots[slot] = 1;
		r_slots[index] = slot;
	}
	used += p_count;
	return true;
}

void NextGISDFPool::free(const LocalVector<uint32_t> &p_slots) {
	for (const uint32_t &slot : p_slots) {
		free_slot(slot);
	}
}

void NextGISDFPool::free_slot(uint32_t p_slot) {
	if (p_slot >= capacity || slots[p_slot] == 0) {
		return;
	}
	slots[p_slot] = 0;
	free_list.push_back(p_slot);
	used--;
}

bool NextGISDFPool::is_allocated(uint32_t p_slot) const {
	return p_slot < capacity && slots[p_slot] != 0;
}

NextGISDFAtlas::NextGISDFAtlas(uint32_t p_size_mb) :
		pool(p_size_mb) {
}

RID NextGISDFAtlas::_ensure_buffer() {
	if (atlas_buffer.is_valid()) {
		return atlas_buffer;
	}
	RD *rd = RD::get_singleton();
	if (rd == nullptr) {
		return RID();
	}
	atlas_buffer = rd->storage_buffer_create(uint32_t(pool.get_size_bytes()));
	return atlas_buffer;
}

void NextGISDFAtlas::_release_pages(MeshPages &r_pages) {
	for (PackedInt32Array &mip_slots : r_pages.slots) {
		for (int index = 0; index < mip_slots.size(); index++) {
			pool.free_slot(uint32_t(mip_slots[index]));
		}
		mip_slots.clear();
	}
	resident_bytes -= MIN(resident_bytes, r_pages.resident_bytes);
	r_pages.resident_bytes = 0;
}

bool NextGISDFAtlas::register_mesh(RID p_mesh, const Ref<MeshDistanceField> &p_field) {
	ERR_FAIL_COND_V(p_mesh.is_null(), false);
	ERR_FAIL_COND_V(p_field.is_null(), false);

	unregister_mesh(p_mesh);

	const int mip_count = p_field->get_mip_count();
	if (mip_count <= 0) {
		return false;
	}
	RD *rd = RD::get_singleton();
	const RID buffer = _ensure_buffer();

	MeshPages pages;
	pages.field = p_field;
	pages.mip_count = uint32_t(mip_count);
	const TypedArray<PackedInt32Array> source_tables = p_field->get_page_tables();
	const TypedArray<PackedByteArray> source_bricks = p_field->get_brick_data();
	if (source_tables.size() != mip_count || source_bricks.size() != mip_count) {
		return false;
	}

	uint64_t reserved_bytes = 0;
	for (int mip = 0; mip < mip_count; mip++) {
		const PackedInt32Array table = source_tables[mip];
		const PackedByteArray bricks = source_bricks[mip];
		uint32_t brick_count = 0;
		for (int index = 0; index < table.size(); index++) {
			if (table[index] >= 0) {
				brick_count++;
			}
		}
		LocalVector<uint32_t> slots;
		if (!pool.allocate(brick_count, slots)) {
			// Roll back the mips already reserved.
			for (PackedInt32Array &allocated : pages.slots) {
				for (int index = 0; index < allocated.size(); index++) {
					pool.free_slot(uint32_t(allocated[index]));
				}
			}
			return false;
		}
		PackedInt32Array gpu_table;
		gpu_table.resize(table.size());
		uint32_t cursor = 0;
		for (int index = 0; index < table.size(); index++) {
			const int32_t brick = table[index];
			if (brick < 0) {
				gpu_table.set(index, brick);
				continue;
			}
			const uint32_t byte_offset = uint32_t(brick) * NextGISDFPool::BRICK_BYTES;
			if (byte_offset + NextGISDFPool::BRICK_BYTES > uint32_t(bricks.size())) {
				// A malformed table; release everything and refuse the mesh.
				for (PackedInt32Array &allocated : pages.slots) {
					for (int index = 0; index < allocated.size(); index++) {
						pool.free_slot(uint32_t(allocated[index]));
					}
				}
				pool.free(slots);
				return false;
			}
			const uint32_t slot = slots[cursor++];
			gpu_table.set(index, int32_t(slot));
			if (buffer.is_valid() && rd != nullptr) {
				rd->buffer_update(buffer, slot * NextGISDFPool::BRICK_BYTES, NextGISDFPool::BRICK_BYTES, bricks.ptr() + byte_offset);
			}
			reserved_bytes += NextGISDFPool::BRICK_BYTES;
		}
		pages.page_tables.push_back(gpu_table);
		PackedInt32Array packed_slots;
		packed_slots.resize(slots.size());
		for (uint32_t index = 0; index < slots.size(); index++) {
			packed_slots.set(index, int32_t(slots[index]));
		}
		pages.slots.push_back(packed_slots);
	}
	pages.resident_bytes = reserved_bytes;
	resident_bytes += reserved_bytes;
	meshes.insert(p_mesh, pages);
	return true;
}

void NextGISDFAtlas::unregister_mesh(RID p_mesh) {
	MeshPages *pages = meshes.getptr(p_mesh);
	if (pages == nullptr) {
		return;
	}
	_release_pages(*pages);
	meshes.erase(p_mesh);
}

bool NextGISDFAtlas::has_mesh(RID p_mesh) const {
	return meshes.has(p_mesh);
}

const NextGISDFAtlas::MeshPages *NextGISDFAtlas::get_pages(RID p_mesh) const {
	return meshes.getptr(p_mesh);
}
