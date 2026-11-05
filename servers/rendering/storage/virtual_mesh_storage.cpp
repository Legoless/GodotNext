/**************************************************************************/
/*  virtual_mesh_storage.cpp                                              */
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

#include "virtual_mesh_storage.h"

#include "core/math/math_funcs.h"

#ifdef REAL_T_IS_DOUBLE
static_assert(sizeof(RendererVirtualMeshStorage::Record) == 256, "NextGeo instance record must stay 256 bytes under REAL_T_IS_DOUBLE (plan §6.3).");
#else
static_assert(sizeof(RendererVirtualMeshStorage::Record) == 224, "NextGeo instance record must stay 224 bytes (plan §6.3).");
#endif
static_assert(sizeof(RendererVirtualMeshStorage::MeshRecord) == 32, "NextGeo mesh record must stay 32 bytes (plan §6.3).");

RendererVirtualMeshStorage::RendererVirtualMeshStorage() {
	_ensure_capacity(INITIAL_RECORD_CAPACITY);
}

RendererVirtualMeshStorage::~RendererVirtualMeshStorage() {
	_reset_dag();
}

void RendererVirtualMeshStorage::_ensure_capacity(uint32_t p_records) {
	if (p_records <= record_capacity) {
		return;
	}
	uint32_t capacity = record_capacity == 0 ? INITIAL_RECORD_CAPACITY : record_capacity;
	while (capacity < p_records) {
		capacity *= 2;
	}
	records.resize(capacity);
	slot_states.resize(capacity);
	record_capacity = capacity;
}

uint32_t RendererVirtualMeshStorage::_alloc_dag(uint32_t p_size) {
	if (p_size == 0) {
		return UINT32_MAX;
	}
	const uint32_t aligned = (p_size + 15u) & ~15u;
	for (uint32_t index = 0; index < dag_free.size(); index++) {
		if (dag_free[index].size < aligned) {
			continue;
		}
		const uint32_t offset = dag_free[index].offset;
		dag_free[index].offset += aligned;
		dag_free[index].size -= aligned;
		if (dag_free[index].size == 0) {
			dag_free.remove_at(index);
		}
		return offset;
	}
	if (dag_used + aligned > dag_capacity) {
		uint32_t capacity = dag_capacity == 0 ? 1024u * 1024u : dag_capacity;
		while (capacity < dag_used + aligned) {
			capacity *= 2;
		}
		if (!_resize_dag(capacity)) {
			return UINT32_MAX;
		}
		dag_capacity = capacity;
	}
	const uint32_t offset = dag_used;
	dag_used += aligned;
	return offset;
}

void RendererVirtualMeshStorage::_free_dag(uint32_t p_offset, uint32_t p_size) {
	if (p_size == 0) {
		return;
	}
	DAGFree range;
	range.offset = p_offset;
	range.size = (p_size + 15u) & ~15u;
	dag_free.push_back(range);
}

RendererVirtualMeshStorage::MeshRecord RendererVirtualMeshStorage::_parse_dag(const Vector<uint8_t> &p_dag, uint32_t p_base) {
	MeshRecord record;
	const uint32_t size = uint32_t(p_dag.size());
	if (size < 8) {
		return record;
	}
	uint32_t group_count = 0;
	uint32_t bvh_count = 0;
	memcpy(&group_count, p_dag.ptr(), sizeof(uint32_t));
	memcpy(&bvh_count, p_dag.ptr() + 4, sizeof(uint32_t));
	const uint64_t expected = 8ull + uint64_t(group_count) * 60ull + uint64_t(bvh_count) * 80ull;
	if (expected > size) {
		return record;
	}
	record.group_offset = p_base + 8;
	record.bvh_node_offset = p_base + 8 + group_count * 60;
	record.bvh_level_count = 0;
	if (group_count > 0) {
		uint32_t first_cluster = 0;
		uint32_t total_clusters = 0;
		memcpy(&first_cluster, p_dag.ptr() + 8, sizeof(uint32_t));
		for (uint32_t group = 0; group < group_count; group++) {
			uint32_t cluster_count = 0;
			memcpy(&cluster_count, p_dag.ptr() + 8 + group * 60 + 4, sizeof(uint32_t));
			total_clusters += cluster_count;
		}
		record.cluster_offset = first_cluster;
		record.cluster_count = total_clusters;
	}
	if (bvh_count > 0) {
		// Level-synchronous dispatches follow the tree depth from the root (node 0).
		LocalVector<uint32_t> queue;
		LocalVector<uint8_t> visited;
		visited.resize(bvh_count);
		memset(visited.ptr(), 0, bvh_count);
		queue.push_back(0);
		visited[0] = 1;
		uint32_t head = 0;
		uint32_t max_level = 0;
		LocalVector<uint32_t> levels;
		levels.push_back(0);
		while (head < queue.size()) {
			const uint32_t node = queue[head];
			const uint32_t level = levels[head];
			head++;
			max_level = MAX(max_level, level);
			uint32_t child_count = 0;
			memcpy(&child_count, p_dag.ptr() + record.bvh_node_offset - p_base + node * 80 + 44, sizeof(uint32_t));
			if (child_count > 8) {
				child_count = 8;
			}
			for (uint32_t child = 0; child < child_count; child++) {
				uint32_t value = 0;
				memcpy(&value, p_dag.ptr() + record.bvh_node_offset - p_base + node * 80 + 48 + child * 4, sizeof(uint32_t));
				if ((value & 0x80000000u) != 0) {
					continue;
				}
				if (value >= bvh_count || visited[value]) {
					continue;
				}
				visited[value] = 1;
				queue.push_back(value);
				levels.push_back(level + 1);
			}
		}
		record.bvh_level_count = max_level + 1;
	}
	return record;
}

void RendererVirtualMeshStorage::_mark_dirty(uint32_t p_record) {
	if (p_record < slot_states.size()) {
		slot_states[p_record].dirty = true;
	}
}

void RendererVirtualMeshStorage::_write_transform(Record &r_record, const Transform3D &p_transform, const AABB &p_world_aabb, float p_lod_bias) {
	for (int row = 0; row < 3; row++) {
		r_record.transform[row][0] = p_transform.basis.rows[row][0];
		r_record.transform[row][1] = p_transform.basis.rows[row][1];
		r_record.transform[row][2] = p_transform.basis.rows[row][2];
		r_record.transform[row][3] = p_transform.origin[row];
	}
	const Vector3 scale = p_transform.basis.get_scale();
	r_record.world_aabb_min[0] = p_world_aabb.position.x;
	r_record.world_aabb_min[1] = p_world_aabb.position.y;
	r_record.world_aabb_min[2] = p_world_aabb.position.z;
	r_record.world_aabb_min[3] = MAX(scale.x, MAX(scale.y, scale.z));
	r_record.world_aabb_max[0] = p_world_aabb.position.x + p_world_aabb.size.x;
	r_record.world_aabb_max[1] = p_world_aabb.position.y + p_world_aabb.size.y;
	r_record.world_aabb_max[2] = p_world_aabb.position.z + p_world_aabb.size.z;
	r_record.world_aabb_max[3] = p_lod_bias;
}

void RendererVirtualMeshStorage::mesh_set_virtual_geometry(RID p_mesh, const RenderingServerTypes::VirtualGeometryDesc &p_desc) {
	ERR_FAIL_COND(p_mesh.is_null());
	uint32_t index = 0;
	const uint32_t *existing = mesh_indices.getptr(p_mesh);
	if (existing != nullptr) {
		index = *existing;
	} else {
		index = mesh_rids.size();
		mesh_indices.insert(p_mesh, index);
		mesh_rids.push_back(p_mesh);
		mesh_records.push_back(MeshRecord());
		mesh_record_dirty.push_back(0);
	}

	MeshRecord &record = mesh_records[index];
	uint32_t surface_count = 0;
	for (uint32_t bit = 0; bit < 32; bit++) {
		if (p_desc.eligible_surface_mask & (1u << bit)) {
			surface_count++;
		}
	}
	record.surface_count = surface_count;
	record.page_table_offset = 0;
	record.bvh_level_count = 0;

	MeshInfo info;
	info.desc = p_desc;
	info.record = record;
	info.mesh = p_mesh;
	// GodotNext: nextgeo_gpu_cull (#76). Upload the DAG blob and fill the
	// mesh record's offsets for the instance/BVH cull.
	MeshInfo *old_info = meshes.getptr(p_mesh);
	if (old_info != nullptr && old_info->dag_size != 0) {
		_free_dag(old_info->dag_offset, old_info->dag_size);
	}
	if (!p_desc.dag_header.is_empty()) {
		const uint32_t dag_offset = _alloc_dag(uint32_t(p_desc.dag_header.size()));
		if (dag_offset != UINT32_MAX) {
			info.dag_offset = dag_offset;
			info.dag_size = uint32_t(p_desc.dag_header.size());
			_upload_dag(dag_offset, p_desc.dag_header.ptr(), info.dag_size);
			info.record = _parse_dag(p_desc.dag_header, dag_offset);
			info.record.surface_count = surface_count;
		}
	}
	if (old_info != nullptr) {
		info.live_instances = old_info->live_instances;
	}
	meshes[p_mesh] = info;
	mesh_records[index] = info.record;
	mesh_record_dirty[index] = 1;
	mesh_records_dirty = true;

	// Live records of this mesh pick up the new mask and virtual-mesh index.
	for (uint32_t slot = 0; slot < used_slots; slot++) {
		if (!slot_states[slot].allocated || records[slot].virtual_mesh != index) {
			continue;
		}
		records[slot].surface_mask = p_desc.eligible_surface_mask;
		_mark_dirty(slot);
	}
}

void RendererVirtualMeshStorage::mesh_clear_virtual_geometry(RID p_mesh, uint32_t p_reason) {
	ERR_FAIL_COND(p_mesh.is_null());
	const uint32_t *index = mesh_indices.getptr(p_mesh);
	if (index == nullptr) {
		return;
	}
	mesh_record_dirty[*index] = 1;
	mesh_records_dirty = true;
	MeshInfo *info = meshes.getptr(p_mesh);
	if (info != nullptr) {
		_free_dag(info->dag_offset, info->dag_size);
	}
	meshes.erase(p_mesh);
	for (uint32_t slot = 0; slot < used_slots; slot++) {
		if (!slot_states[slot].allocated || records[slot].virtual_mesh != *index) {
			continue;
		}
		records[slot].virtual_mesh = UINT32_MAX;
		records[slot].surface_mask = 0;
		_mark_dirty(slot);
	}
	// p_reason is propagated by the NextGeo fallback overlay (#80); the record
	// store only needs to know the mesh is gone.
	(void)p_reason;
}

bool RendererVirtualMeshStorage::mesh_has_virtual_geometry(RID p_mesh) const {
	return meshes.has(p_mesh);
}

uint32_t RendererVirtualMeshStorage::instance_record_allocate(RID p_instance, RID p_mesh) {
	if (!_can_allocate_record()) {
		return UINT32_MAX;
	}
	uint32_t slot = UINT32_MAX;
	if (!free_now.is_empty()) {
		slot = free_now[free_now.size() - 1];
		free_now.remove_at(free_now.size() - 1);
	} else {
		slot = used_slots;
		used_slots++;
		_ensure_capacity(used_slots);
	}
	ERR_FAIL_UNSIGNED_INDEX_V(slot, record_capacity, UINT32_MAX);

	Record &record = records[slot];
	record = Record();
	SlotState &state = slot_states[slot];
	state.allocated = true;
	state.dirty = true;
	state.dirty_last = false;
	state.initialized = false;
	state.free_frame = UINT32_MAX;
	live_records++;
	_record_reused(slot);

	const uint32_t *index = mesh_indices.getptr(p_mesh);
	MeshInfo *info = meshes.getptr(p_mesh);
	if (index != nullptr && info != nullptr) {
		record.virtual_mesh = *index;
		record.surface_mask = info->desc.eligible_surface_mask;
		info->live_instances++;
	}
	record.flags = RECORD_VALID | RECORD_VISIBLE;
	return slot;
}

void RendererVirtualMeshStorage::instance_record_free(uint32_t p_record) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	SlotState &state = slot_states[p_record];
	state.allocated = false;
	state.dirty = true;
	state.dirty_last = false;
	state.free_frame = frame + 1;
	records[p_record].flags = 0;
	live_records--;
	const uint32_t index = records[p_record].virtual_mesh;
	if (index < mesh_rids.size()) {
		MeshInfo *info = meshes.getptr(mesh_rids[index]);
		if (info != nullptr && info->live_instances > 0) {
			info->live_instances--;
		}
	}
	// Reusable on the next frame, not this one.
	if (!free_next.has(p_record)) {
		free_next.push_back(p_record);
	}
}

void RendererVirtualMeshStorage::instance_record_set_transform(uint32_t p_record, const Transform3D &p_transform, const AABB &p_world_aabb, bool p_visible, bool p_teleport, float p_lod_bias) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	Record &record = records[p_record];
	SlotState &state = slot_states[p_record];
	if (!p_visible) {
		record.flags &= ~RECORD_VISIBLE;
		_mark_dirty(p_record);
		return;
	}

	const float distance = record.transform[0][3] * record.transform[0][3] + record.transform[1][3] * record.transform[1][3] + record.transform[2][3] * record.transform[2][3];
	const float new_distance = p_transform.origin.length_squared();
	const bool first = !state.initialized;
	const bool jumped = !first && Math::abs(Math::sqrt(new_distance) - Math::sqrt(distance)) > teleport_distance;

	if (first || p_teleport || jumped) {
		_write_transform(record, p_transform, p_world_aabb, p_lod_bias);
		for (int row = 0; row < 3; row++) {
			for (int column = 0; column < 4; column++) {
				record.prev_transform[row][column] = record.transform[row][column];
			}
		}
		for (int index = 0; index < 4; index++) {
			record.prev_world_aabb_min[index] = record.world_aabb_min[index];
			record.prev_world_aabb_max[index] = record.world_aabb_max[index];
		}
		if (!first) {
			record.flags |= RECORD_TELEPORTED;
		}
		state.initialized = true;
	} else {
		for (int row = 0; row < 3; row++) {
			for (int column = 0; column < 4; column++) {
				record.prev_transform[row][column] = record.transform[row][column];
			}
		}
		for (int index = 0; index < 4; index++) {
			record.prev_world_aabb_min[index] = record.world_aabb_min[index];
			record.prev_world_aabb_max[index] = record.world_aabb_max[index];
		}
		_write_transform(record, p_transform, p_world_aabb, p_lod_bias);
		record.flags |= RECORD_MOVED;
		record.flags &= ~RECORD_TELEPORTED;
	}
	record.flags |= RECORD_VALID | RECORD_VISIBLE;
	_mark_dirty(p_record);
}

void RendererVirtualMeshStorage::instance_record_set_state(uint32_t p_record, uint32_t p_flags) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	Record &record = records[p_record];
	const uint32_t keep = record.flags & (RECORD_VALID | RECORD_VISIBLE | RECORD_MOVED | RECORD_TELEPORTED);
	record.flags = keep | (p_flags & ~(RECORD_VALID | RECORD_VISIBLE | RECORD_MOVED | RECORD_TELEPORTED));
	_mark_dirty(p_record);
}

void RendererVirtualMeshStorage::instance_record_set_layer_mask(uint32_t p_record, uint32_t p_layer_mask) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	records[p_record].layer_mask = p_layer_mask;
	_mark_dirty(p_record);
}

void RendererVirtualMeshStorage::instance_record_set_gi_flags(uint32_t p_record, uint32_t p_gi_flags) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	records[p_record].gi_flags = p_gi_flags;
	_mark_dirty(p_record);
}

void RendererVirtualMeshStorage::instance_record_set_rt(uint32_t p_record, const uint32_t p_rt_fields[6]) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	Record &record = records[p_record];
	record.tlas_instance_index = p_rt_fields[0];
	record.blas_handle = p_rt_fields[1];
	record.rt_arena_chunk = p_rt_fields[2];
	record.rt_arena_offset = p_rt_fields[3];
	record.src_page_table = p_rt_fields[4];
	record.card_set = p_rt_fields[5];
	_mark_dirty(p_record);
}

void RendererVirtualMeshStorage::begin_frame() {
	frame++;
	free_now = free_next;
	free_next.clear();
}

void RendererVirtualMeshStorage::flush() {
	// A record that changed last frame and not this one needs one rewrite so
	// prev holds last frame's cur, not the frame before the last change.
	for (uint32_t slot = 0; slot < used_slots; slot++) {
		SlotState &state = slot_states[slot];
		if (state.dirty_last && !state.dirty && state.allocated) {
			Record &record = records[slot];
			for (int row = 0; row < 3; row++) {
				for (int column = 0; column < 4; column++) {
					record.prev_transform[row][column] = record.transform[row][column];
				}
			}
			for (int index = 0; index < 4; index++) {
				record.prev_world_aabb_min[index] = record.world_aabb_min[index];
				record.prev_world_aabb_max[index] = record.world_aabb_max[index];
			}
			state.dirty = true;
			state.rewritten = true;
		}
	}

	uint32_t run_first = UINT32_MAX;
	uint32_t run_count = 0;
	for (uint32_t slot = 0; slot < used_slots; slot++) {
		SlotState &state = slot_states[slot];
		if (state.dirty) {
			if (run_first == UINT32_MAX) {
				run_first = slot;
			}
			run_count++;
		} else if (run_first != UINT32_MAX) {
			_upload_records(run_first, run_count);
			run_first = UINT32_MAX;
			run_count = 0;
		}
	}
	if (run_first != UINT32_MAX) {
		_upload_records(run_first, run_count);
	}
	for (uint32_t slot = 0; slot < used_slots; slot++) {
		SlotState &state = slot_states[slot];
		state.dirty_last = state.dirty && !state.rewritten;
		state.dirty = false;
		state.rewritten = false;
	}

	if (mesh_records_dirty) {
		uint32_t first = UINT32_MAX;
		uint32_t count = 0;
		for (uint32_t index = 0; index < mesh_record_dirty.size(); index++) {
			if (mesh_record_dirty[index]) {
				if (first == UINT32_MAX) {
					first = index;
				}
				count++;
				mesh_record_dirty[index] = 0;
			} else if (first != UINT32_MAX) {
				_upload_mesh_records(first, count);
				first = UINT32_MAX;
				count = 0;
			}
		}
		if (first != UINT32_MAX) {
			_upload_mesh_records(first, count);
		}
		mesh_records_dirty = false;
	}
}

const RendererVirtualMeshStorage::Record &RendererVirtualMeshStorage::get_record(uint32_t p_record) const {
	static const Record fallback;
	ERR_FAIL_COND_V_MSG(p_record >= used_slots, fallback, "NextGeo record index out of range.");
	return records[p_record];
}

const RendererVirtualMeshStorage::MeshRecord &RendererVirtualMeshStorage::get_mesh_record(RID p_mesh) const {
	static const MeshRecord fallback;
	const uint32_t *index = mesh_indices.getptr(p_mesh);
	ERR_FAIL_NULL_V(index, fallback);
	ERR_FAIL_UNSIGNED_INDEX_V(*index, mesh_records.size(), fallback);
	return mesh_records[*index];
}
