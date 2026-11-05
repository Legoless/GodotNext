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
	const uint32_t mesh_id = mesh_ids.has(p_mesh) ? mesh_ids[p_mesh] : uint32_t(mesh_order.size());
	if (!mesh_ids.has(p_mesh)) {
		mesh_ids.insert(p_mesh, mesh_id);
		mesh_order.push_back(p_mesh);
	}

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

	// All mips get their pool slots up front (bricks never move). Mip 0 is
	// uploaded now; higher mips stream in on request.
	for (int mip = 0; mip < mip_count; mip++) {
		const PackedInt32Array table = source_tables[mip];
		const PackedByteArray bricks = source_bricks[mip];
		if (mip > 0) {
			// Higher mips stream in on request and allocate their slots then.
			pages.slots.push_back(PackedInt32Array());
			pages.source_tables.push_back(table);
			pages.mip_loaded.push_back(0);
			pages.mip_last_requested.push_back(0);
			pages.page_tables.push_back(table);
			continue;
		}
		uint32_t brick_count = 0;
		for (int index = 0; index < table.size(); index++) {
			if (table[index] >= 0) {
				brick_count++;
			}
		}
		LocalVector<uint32_t> slots;
		if (!pool.allocate(brick_count, slots)) {
			for (PackedInt32Array &allocated : pages.slots) {
				for (int index = 0; index < allocated.size(); index++) {
					pool.free_slot(uint32_t(allocated[index]));
				}
			}
			return false;
		}
		PackedInt32Array packed_slots;
		packed_slots.resize(slots.size());
		for (uint32_t index = 0; index < slots.size(); index++) {
			packed_slots.set(index, int32_t(slots[index]));
		}
		pages.slots.push_back(packed_slots);
		pages.source_tables.push_back(table);
		pages.mip_loaded.push_back(mip == 0 ? 1 : 0);
		pages.mip_last_requested.push_back(0);

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
				for (PackedInt32Array &allocated : pages.slots) {
					for (int slot_index = 0; slot_index < allocated.size(); slot_index++) {
						pool.free_slot(uint32_t(allocated[slot_index]));
					}
				}
				return false;
			}
			const uint32_t slot = slots[cursor++];
			gpu_table.set(index, int32_t(slot));
			if (mip == 0 && buffer.is_valid() && rd != nullptr) {
				rd->buffer_update(buffer, slot * NextGISDFPool::BRICK_BYTES, NextGISDFPool::BRICK_BYTES, bricks.ptr() + byte_offset);
			}
		}
		pages.page_tables.push_back(gpu_table);
		if (mip == 0) {
			pages.resident_bytes += uint64_t(brick_count) * NextGISDFPool::BRICK_BYTES;
		}
	}
	resident_bytes += pages.resident_bytes;
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

uint32_t NextGISDFAtlas::get_mesh_id(RID p_mesh) const {
	const uint32_t *id = mesh_ids.getptr(p_mesh);
	return id != nullptr && meshes.has(p_mesh) ? *id : UINT32_MAX;
}

uint32_t NextGISDFAtlas::get_mip_count(uint32_t p_mesh_id) const {
	if (p_mesh_id >= mesh_order.size()) {
		return 1;
	}
	const MeshPages *pages = meshes.getptr(mesh_order[p_mesh_id]);
	return pages != nullptr ? MAX(pages->mip_count, 1u) : 1;
}

void NextGISDFAtlas::stream_mips(const LocalVector<uint32_t> &p_sdf_ids, const LocalVector<uint32_t> &p_mips, uint64_t p_frame, uint64_t p_budget_bytes) {
	RD *rd = RD::get_singleton();
	const RID buffer = _ensure_buffer();
	const uint32_t count = MIN(p_sdf_ids.size(), p_mips.size());
	uint64_t budget = p_budget_bytes;

	// Requests are applied mip order first (nearest detail first).
	for (uint32_t mip = 1; mip < 8 && budget > 0; mip++) {
		for (uint32_t index = 0; index < count && budget > 0; index++) {
			const uint32_t mesh_id = p_sdf_ids[index];
			if (mesh_id == UINT32_MAX || p_mips[index] < mip) {
				continue;
			}
			if (mesh_id >= mesh_order.size()) {
				continue;
			}
			MeshPages *pages = meshes.getptr(mesh_order[mesh_id]);
			if (pages == nullptr || mip >= pages->mip_count) {
				continue;
			}
			if (pages->mip_loaded[mip]) {
				pages->mip_last_requested.write[mip] = p_frame;
				continue;
			}
			const PackedInt32Array &source = pages->source_tables[mip];
			uint32_t brick_count = 0;
			for (int brick_index = 0; brick_index < source.size(); brick_index++) {
				if (source[brick_index] >= 0) {
					brick_count++;
				}
			}
			const uint64_t bytes = uint64_t(brick_count) * NextGISDFPool::BRICK_BYTES;
			if (bytes > budget) {
				continue;
			}
			LocalVector<uint32_t> slots;
			if (!pool.allocate(brick_count, slots)) {
				continue;
			}
			const PackedByteArray bricks = pages->field->get_brick_data()[mip];
			PackedInt32Array gpu_table;
			gpu_table.resize(source.size());
			uint32_t cursor = 0;
			for (int brick_index = 0; brick_index < source.size(); brick_index++) {
				const int32_t brick = source[brick_index];
				if (brick < 0) {
					gpu_table.set(brick_index, brick);
					continue;
				}
				const uint32_t slot = slots[cursor++];
				gpu_table.set(brick_index, int32_t(slot));
				if (buffer.is_valid() && rd != nullptr) {
					rd->buffer_update(buffer, slot * NextGISDFPool::BRICK_BYTES, NextGISDFPool::BRICK_BYTES, bricks.ptr() + uint32_t(brick) * NextGISDFPool::BRICK_BYTES);
				}
			}
			PackedInt32Array packed_slots;
			packed_slots.resize(slots.size());
			for (uint32_t slot_index = 0; slot_index < slots.size(); slot_index++) {
				packed_slots.set(slot_index, int32_t(slots[slot_index]));
			}
			pages->slots.write[mip] = packed_slots;
			pages->page_tables.write[mip] = gpu_table;
			pages->mip_loaded.write[mip] = 1;
			pages->mip_last_requested.write[mip] = p_frame;
			pages->resident_bytes += bytes;
			resident_bytes += bytes;
			budget -= bytes;
		}
	}

	// Evict higher mips idle for 60 frames.
	for (const KeyValue<RID, MeshPages> &entry : meshes) {
		MeshPages &pages = const_cast<MeshPages &>(entry.value);
		for (uint32_t mip = 1; mip < pages.mip_count; mip++) {
			if (!pages.mip_loaded[mip] || p_frame - pages.mip_last_requested[mip] <= 60) {
				continue;
			}
			const uint64_t bytes = uint64_t(pages.slots[mip].size()) * NextGISDFPool::BRICK_BYTES;
			for (int slot_index = 0; slot_index < pages.slots[mip].size(); slot_index++) {
				pool.free_slot(uint32_t(pages.slots[mip][slot_index]));
			}
			pages.slots.write[mip].clear();
			pages.mip_loaded.write[mip] = 0;
			pages.resident_bytes -= MIN(pages.resident_bytes, bytes);
			resident_bytes -= MIN(resident_bytes, bytes);
		}
	}
}

void NextGISDFAtlas::free_all() {
	LocalVector<RID> registered;
	for (const KeyValue<RID, MeshPages> &entry : meshes) {
		registered.push_back(entry.key);
	}
	for (const RID &mesh : registered) {
		unregister_mesh(mesh);
	}
	RD *rd = RD::get_singleton();
	if (rd != nullptr && atlas_buffer.is_valid()) {
		rd->free_rid(atlas_buffer);
	}
	atlas_buffer = RID();
	mesh_ids.clear();
	mesh_order.clear();
}

void NextGISDFInstances::begin_frame() {
	records.clear();
	for (uint32_t &count : reason_counts) {
		count = 0;
	}
}

uint32_t NextGISDFInstances::add(const NextGISDFAtlas &p_atlas, RID p_mesh, const Transform3D &p_transform, bool p_skinned, bool p_has_instance_uniforms, float p_scale_bound, bool p_visible) {
	if (records.size() >= MAX_INSTANCES) {
		reason_counts[NEXTGI_SDF_INSTANCE_CAP]++;
		return UINT32_MAX;
	}
	Record record;
	const Transform3D world_to_local = p_transform.affine_inverse();
	for (int row = 0; row < 3; row++) {
		record.world_to_local[row][0] = world_to_local.basis.rows[row][0];
		record.world_to_local[row][1] = world_to_local.basis.rows[row][1];
		record.world_to_local[row][2] = world_to_local.basis.rows[row][2];
		record.world_to_local[row][3] = world_to_local.origin[row];
	}
	record.world_position[0] = p_transform.origin.x;
	record.world_position[1] = p_transform.origin.y;
	record.world_position[2] = p_transform.origin.z;
	record.world_position[3] = MAX(p_scale_bound, 0.001f);
	if (p_visible) {
		record.flags |= INSTANCE_VISIBLE;
	}
	if (p_skinned || p_has_instance_uniforms) {
		record.reason = NEXTGI_SDF_UNSUPPORTED_DEFORMATION;
	} else {
		const uint32_t sdf_id = p_atlas.get_mesh_id(p_mesh);
		if (sdf_id == UINT32_MAX) {
			record.reason = NEXTGI_SDF_NO_FIELD;
		} else {
			record.sdf_id = sdf_id;
			record.mip_count = p_atlas.get_mip_count(sdf_id);
		}
	}
	reason_counts[record.reason]++;
	records.push_back(record);
	return uint32_t(records.size() - 1);
}

void NextGISDFInstances::upload() {
	RD *rd = RD::get_singleton();
	if (rd == nullptr || records.is_empty()) {
		return;
	}
	const uint32_t required = uint32_t(records.size());
	if (buffer.is_null() || buffer_capacity < required) {
		if (buffer.is_valid()) {
			rd->free_rid(buffer);
		}
		buffer_capacity = MAX(required, MAX_INSTANCES / 4);
		buffer = rd->storage_buffer_create(buffer_capacity * RECORD_BYTES);
	}
	ERR_FAIL_COND(buffer.is_null());
	rd->buffer_update(buffer, 0, required * RECORD_BYTES, records.ptr());
}

void NextGISDFInstances::clear() {
	records.clear();
	for (uint32_t &count : reason_counts) {
		count = 0;
	}
	RD *rd = RD::get_singleton();
	if (rd != nullptr && buffer.is_valid()) {
		rd->free_rid(buffer);
	}
	buffer = RID();
	buffer_capacity = 0;
}

uint32_t NextGISDFInstances::get_reason_count(uint32_t p_reason) const {
	return p_reason < 4 ? reason_counts[p_reason] : 0;
}

bool NextGISDFAtlas::has_mesh(RID p_mesh) const {
	return meshes.has(p_mesh);
}

const NextGISDFAtlas::MeshPages *NextGISDFAtlas::get_pages(RID p_mesh) const {
	return meshes.getptr(p_mesh);
}
