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
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "virtual_mesh_storage.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/io/resource_format_ngm.h"
#include "core/math/math_funcs.h"
#include "core/math/vector2i.h"
#include "core/os/os.h"
#include "core/templates/sort_array.h"
#include "servers/rendering/rendering_server_globals.h"

#ifdef REAL_T_IS_DOUBLE
static_assert(sizeof(RendererVirtualMeshStorage::Record) == 256, "NextGeo instance record must stay 256 bytes under REAL_T_IS_DOUBLE (plan §6.3).");
#else
static_assert(sizeof(RendererVirtualMeshStorage::Record) == 224, "NextGeo instance record must stay 224 bytes (plan §6.3).");
#endif
static_assert(sizeof(RendererVirtualMeshStorage::MeshRecord) == 32, "NextGeo mesh record must stay 32 bytes (plan §6.3).");
static_assert(sizeof(RendererVirtualMeshStorage::ClusterGPUDesc) == 16, "NextGeo cluster GPU descriptor must stay 16 bytes.");
static_assert(sizeof(RendererVirtualMeshStorage::SkinInstanceInfo) == 16, "NextGeo skin instance side table entry must stay 16 bytes.");
static_assert(sizeof(RendererVirtualMeshStorage::InstanceAttributes) == 48, "NextGeo InstanceAttributes must stay 48 bytes.");
static_assert(sizeof(RendererVirtualMeshStorage::BucketSurfaceMaskGPU) == 16, "NextGeo BucketSurfaceMaskGPU must stay 16 bytes.");
static_assert(sizeof(RendererVirtualMeshStorage::AssemblyHeaderGPU) == 32, "NextGeo AssemblyHeaderGPU must stay 32 bytes.");
static_assert(sizeof(RendererVirtualMeshStorage::AssemblyChildGPU) == 64, "NextGeo AssemblyChildGPU must stay 64 bytes.");
static_assert(sizeof(RendererVirtualMeshStorage::AssemblyPartGPU) == 96, "NextGeo AssemblyPartGPU must stay 96 bytes.");

namespace {

// Gershgorin upper bound on the spectral norm of a 3x4 bone matrix (3 row-major vec4s).
// Matches nextgeo_bone_matrix_scale() and shaders/nextgeo/skinning_inc.glsl.
float _bone_matrix_scale(const float p_rows[12]) {
	const float c0x = p_rows[0], c0y = p_rows[4], c0z = p_rows[8];
	const float c1x = p_rows[1], c1y = p_rows[5], c1z = p_rows[9];
	const float c2x = p_rows[2], c2y = p_rows[6], c2z = p_rows[10];
	const float d00 = c0x * c0x + c0y * c0y + c0z * c0z;
	const float d11 = c1x * c1x + c1y * c1y + c1z * c1z;
	const float d22 = c2x * c2x + c2y * c2y + c2z * c2z;
	const float d01 = Math::abs(c0x * c1x + c0y * c1y + c0z * c1z);
	const float d02 = Math::abs(c0x * c2x + c0y * c2y + c0z * c2z);
	const float d12 = Math::abs(c1x * c2x + c1y * c2y + c1z * c2z);
	const float m = MAX(d00 + d01 + d02, MAX(d11 + d01 + d12, d22 + d02 + d12));
	return (m > 0.0f && Math::is_finite(m)) ? Math::sqrt(m) : 0.0f;
}

float _palette_max_bone_scale(const float *p_bones, uint32_t p_bone_count) {
	if (p_bones == nullptr || p_bone_count == 0u) {
		return 0.0f;
	}
	float max_scale = 0.0f;
	for (uint32_t b = 0; b < p_bone_count; b++) {
		max_scale = MAX(max_scale, _bone_matrix_scale(p_bones + b * 12u));
	}
	return max_scale;
}

uint32_t _ngm_vertex_float_stride(uint32_t p_vertex_layout) {
	uint32_t stride = 0;
	if ((p_vertex_layout & (1u << 0)) != 0) {
		stride += 3; // POSITION
	}
	if ((p_vertex_layout & (1u << 1)) != 0) {
		stride += 3; // NORMAL
	}
	if ((p_vertex_layout & (1u << 2)) != 0) {
		stride += 4; // TANGENT
	}
	if ((p_vertex_layout & (1u << 3)) != 0) {
		stride += 2; // UV0
	}
	if ((p_vertex_layout & (1u << 4)) != 0) {
		stride += 2; // UV2
	}
	if ((p_vertex_layout & (1u << 5)) != 0) {
		stride += 4; // COLOR
	}
	return stride;
}

uint32_t _infer_vertex_layout_from_stride(uint32_t p_stride) {
	switch (p_stride) {
		case 3:
			return 0x01u;
		case 5:
			return 0x09u;
		case 6:
			return 0x03u;
		case 7:
			return 0x21u;
		case 8:
			return 0x0Bu;
		case 10:
			return 0x07u;
		case 12:
			return 0x0Fu;
		case 14:
			return 0x1Fu;
		case 16:
			return 0x2Fu;
		default:
			return p_stride >= 18 ? 0x3Fu : 0x01u;
	}
}

bool _is_valid_raw_page(const Vector<uint8_t> &p_raw, uint32_t &r_clusters) {
	r_clusters = 0;
	if (p_raw.size() < 16) {
		return false;
	}
	uint32_t header[4] = { 0, 0, 0, 0 };
	memcpy(header, p_raw.ptr(), sizeof(header));
	const uint64_t min_bytes = 16ull + uint64_t(header[0]) * 80ull + uint64_t(header[2]) * 3ull + uint64_t(header[3]);
	if (min_bytes > uint64_t(p_raw.size())) {
		return false;
	}
	r_clusters = header[0];
	return true;
}

struct StagedPagePriorityComparator {
	template <typename T>
	_FORCE_INLINE_ bool operator()(const T &p_a, const T &p_b) const {
		if (p_a.visible != p_b.visible) {
			return p_a.visible; // Visible first.
		}
		if (p_a.distance != p_b.distance) {
			return p_a.distance < p_b.distance; // Front-to-back.
		}
		if (p_a.px_error != p_b.px_error) {
			return p_a.px_error > p_b.px_error; // Highest projected error first.
		}
		if (p_a.topo_depth != p_b.topo_depth) {
			return p_a.topo_depth < p_b.topo_depth; // Coarser parents before finer children.
		}
		if (p_a.mesh != p_b.mesh) {
			return p_a.mesh < p_b.mesh;
		}
		return p_a.page_id < p_b.page_id;
	}
};

static constexpr uint32_t MAX_ACTIVE_IO_BATCHES = 2;
static constexpr uint32_t MAX_ITEMS_PER_IO_BATCH = 8;

} // namespace

RendererVirtualMeshStorage::RendererVirtualMeshStorage() {
	_ensure_capacity(INITIAL_RECORD_CAPACITY);
}

RendererVirtualMeshStorage::~RendererVirtualMeshStorage() {
	wait_for_streaming_tasks(true);
	_reset_auxiliary_buffers();
	_reset_dag();
	_reset_pages();
	_reset_clusters();
	_reset_residency();
	_reset_splines();
	_reset_skin_palette();
	_reset_skin_instances();
	_reset_cluster_skin();
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
	skin_instances.resize(capacity);
	instance_attributes.resize(capacity);
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
		// A resize replaces the GPU buffer; restore every mesh blob already
		// resident so their record offsets stay valid.
		for (const KeyValue<RID, MeshInfo> &entry : meshes) {
			if (entry.value.dag_size != 0 && !entry.value.desc.dag_header.is_empty()) {
				_upload_dag(entry.value.dag_offset, entry.value.desc.dag_header.ptr(), entry.value.dag_size);
			}
		}
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

uint32_t RendererVirtualMeshStorage::_alloc_clusters(uint32_t p_size) {
	if (p_size == 0) {
		return UINT32_MAX;
	}
	const uint32_t aligned = (p_size + 15u) & ~15u;
	for (uint32_t index = 0; index < cluster_free.size(); index++) {
		if (cluster_free[index].size < aligned) {
			continue;
		}
		const uint32_t offset = cluster_free[index].offset;
		cluster_free[index].offset += aligned;
		cluster_free[index].size -= aligned;
		if (cluster_free[index].size == 0) {
			cluster_free.remove_at(index);
		}
		return offset;
	}
	if (cluster_used + aligned > cluster_capacity) {
		uint32_t capacity = cluster_capacity == 0 ? 256u * 1024u : cluster_capacity;
		while (capacity < cluster_used + aligned) {
			capacity *= 2;
		}
		if (!_resize_clusters(capacity)) {
			return UINT32_MAX;
		}
		cluster_capacity = capacity;
		for (const KeyValue<RID, MeshInfo> &entry : meshes) {
			if (entry.value.cluster_table_size != 0 && !entry.value.cluster_blob.is_empty()) {
				_upload_clusters(entry.value.cluster_table_offset, entry.value.cluster_blob.ptr(), entry.value.cluster_table_size);
			}
		}
		if (cluster_skin_active) {
			_ensure_cluster_skin_buffer();
		}
	}
	const uint32_t offset = cluster_used;
	cluster_used += aligned;
	return offset;
}

void RendererVirtualMeshStorage::_free_clusters(uint32_t p_offset, uint32_t p_size) {
	if (p_size == 0) {
		return;
	}
	DAGFree range;
	range.offset = p_offset;
	range.size = (p_size + 15u) & ~15u;
	cluster_free.push_back(range);
}

bool RendererVirtualMeshStorage::_ensure_cluster_skin_buffer() {
	const uint32_t needed_bytes = MAX(cluster_capacity / 4u, 64u);
	if (cluster_skin_active && cluster_skin_capacity >= needed_bytes) {
		return true;
	}
	uint32_t target = cluster_skin_capacity == 0 ? 64u * 1024u : cluster_skin_capacity;
	while (target < needed_bytes) {
		target *= 2u;
	}
	if (!_resize_cluster_skin(target)) {
		return false;
	}
	cluster_skin_capacity = target;
	cluster_skin_active = true;
	// Initialize every slot to UINT32_MAX (unskinned) before restoring resident skinned meshes.
	LocalVector<uint32_t> sentinel;
	sentinel.resize(target / sizeof(uint32_t));
	for (uint32_t i = 0; i < sentinel.size(); i++) {
		sentinel[i] = UINT32_MAX;
	}
	_upload_cluster_skin(0u, reinterpret_cast<const uint8_t *>(sentinel.ptr()), target);
	for (const KeyValue<RID, MeshInfo> &entry : meshes) {
		if (entry.value.cluster_table_size != 0 && !entry.value.cluster_skin_offsets.is_empty()) {
			const uint32_t dst_off = entry.value.cluster_table_offset / 4u;
			const uint32_t upload_bytes = uint32_t(entry.value.cluster_skin_offsets.size()) * sizeof(uint32_t);
			if (dst_off + upload_bytes <= cluster_skin_capacity) {
				_upload_cluster_skin(dst_off, reinterpret_cast<const uint8_t *>(entry.value.cluster_skin_offsets.ptr()), upload_bytes);
			}
		}
	}
	return true;
}

uint32_t RendererVirtualMeshStorage::_alloc_palette_bones(uint32_t p_bones) {
	if (p_bones == 0u) {
		return UINT32_MAX;
	}
	for (uint32_t index = 0; index < palette_free.size(); index++) {
		if (palette_free[index].size < p_bones) {
			continue;
		}
		const uint32_t offset = palette_free[index].offset;
		palette_free[index].offset += p_bones;
		palette_free[index].size -= p_bones;
		if (palette_free[index].size == 0u) {
			palette_free.remove_at(index);
		}
		return offset;
	}
	if (palette_used_bones + p_bones > palette_capacity_bones) {
		uint32_t capacity = palette_capacity_bones == 0u ? 256u : palette_capacity_bones;
		while (capacity < palette_used_bones + p_bones) {
			capacity *= 2u;
		}
		const uint64_t bytes64 = uint64_t(capacity) * 48ull; // 3 x vec4 = 48 B per bone.
		if (bytes64 > UINT32_MAX || !_resize_skin_palette(uint32_t(bytes64))) {
			return UINT32_MAX;
		}
		palette_capacity_bones = capacity;
		// Restore all resident skeleton palettes into the resized GPU buffer.
		for (const KeyValue<RID, SkeletonPaletteSlot> &entry : skeleton_palettes) {
			const SkeletonPaletteSlot &slot = entry.value;
			if (slot.bone_offset != UINT32_MAX && slot.bone_count > 0u) {
				const uint32_t half_bytes = slot.bone_count * 48u;
				if (!slot.cur_data.is_empty()) {
					_upload_skin_palette(slot.bone_offset * 48u, reinterpret_cast<const uint8_t *>(slot.cur_data.ptr()), half_bytes);
				}
				if (!slot.prev_data.is_empty()) {
					_upload_skin_palette((slot.bone_offset + slot.bone_count) * 48u, reinterpret_cast<const uint8_t *>(slot.prev_data.ptr()), half_bytes);
				}
			}
		}
	}
	const uint32_t offset = palette_used_bones;
	palette_used_bones += p_bones;
	return offset;
}

void RendererVirtualMeshStorage::_free_palette_bones(uint32_t p_offset, uint32_t p_bones) {
	if (p_offset == UINT32_MAX || p_bones == 0u) {
		return;
	}
	DAGFree range;
	range.offset = p_offset;
	range.size = p_bones;
	uint32_t insert_at = palette_free.size();
	for (uint32_t i = 0; i < palette_free.size(); i++) {
		if (range.offset < palette_free[i].offset) {
			insert_at = i;
			break;
		}
	}
	palette_free.insert(insert_at, range);
	// Coalesce with next block.
	if (insert_at + 1u < palette_free.size() &&
			palette_free[insert_at].offset + palette_free[insert_at].size == palette_free[insert_at + 1u].offset) {
		palette_free[insert_at].size += palette_free[insert_at + 1u].size;
		palette_free.remove_at(insert_at + 1u);
	}
	// Coalesce with previous block.
	if (insert_at > 0u &&
			palette_free[insert_at - 1u].offset + palette_free[insert_at - 1u].size == palette_free[insert_at].offset) {
		palette_free[insert_at - 1u].size += palette_free[insert_at].size;
		palette_free.remove_at(insert_at);
	}
}

void RendererVirtualMeshStorage::_sync_record_skin_info(uint32_t p_record) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	Record &record = records[p_record];
	SlotState &state = slot_states[p_record];
	SkinInstanceInfo next_info;
	bool valid_palette = false;
	if (state.skeleton.is_valid() && record.virtual_mesh < mesh_order.size()) {
		const MeshInfo *mesh = meshes.getptr(mesh_order[record.virtual_mesh]);
		if (mesh != nullptr && (mesh->skin_influences == 4u || mesh->skin_influences == 8u)) {
			const SkeletonPaletteSlot *sk = skeleton_palettes.getptr(state.skeleton);
			if (sk != nullptr && sk->bone_count > 0u && sk->bone_offset != UINT32_MAX) {
				next_info.current_palette_offset = sk->bone_offset * 3u;
				next_info.prev_palette_offset = (sk->bone_offset + sk->bone_count) * 3u;
				next_info.bone_count = sk->bone_count;
				const float scale = sk->max_bone_scale > 0.0f ? sk->max_bone_scale : 1.0f;
				memcpy(&next_info.max_bone_scale_bits, &scale, sizeof(uint32_t));
				valid_palette = true;
				skin_instances_active = true;
			}
		}
	}
	const SkinInstanceInfo &prev_info = skin_instances[p_record];
	if (prev_info.current_palette_offset != next_info.current_palette_offset ||
			prev_info.prev_palette_offset != next_info.prev_palette_offset ||
			prev_info.bone_count != next_info.bone_count ||
			prev_info.max_bone_scale_bits != next_info.max_bone_scale_bits) {
		skin_instances[p_record] = next_info;
		state.skin_dirty = true;
	}
	const uint32_t next_flags = valid_palette ? (record.flags | RECORD_SKINNED_PALETTE) : (record.flags & ~RECORD_SKINNED_PALETTE);
	if (record.flags != next_flags) {
		record.flags = next_flags;
		_mark_dirty(p_record);
	}
}

void RendererVirtualMeshStorage::_sync_skeleton_records(RID p_skeleton) {
	if (p_skeleton.is_null()) {
		return;
	}
	for (uint32_t slot = 0; slot < used_slots; slot++) {
		if (slot_states[slot].allocated && slot_states[slot].skeleton == p_skeleton) {
			_sync_record_skin_info(slot);
		}
	}
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

const RendererVirtualMeshStorage::MeshInfo *RendererVirtualMeshStorage::get_mesh_info(uint32_t p_mesh_index) const {
	return p_mesh_index < mesh_order.size() ? meshes.getptr(mesh_order[p_mesh_index]) : nullptr;
}

uint32_t RendererVirtualMeshStorage::get_max_bvh_levels() const {
	uint32_t levels = 0;
	for (const MeshRecord &record : mesh_records) {
		levels = MAX(levels, record.bvh_level_count);
	}
	return MIN(levels, 8u);
}

void RendererVirtualMeshStorage::_mark_dirty(uint32_t p_record) {
	if (p_record < slot_states.size()) {
		slot_states[p_record].dirty = true;
		if ((records[p_record].flags & RECORD_MULTIMESH_PARENT) != 0u) {
			multimesh_mark_parent_dirty(p_record);
		}
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
	mesh_clear_reasons.erase(p_mesh);
	uint32_t index = 0;
	const uint32_t *existing = mesh_indices.getptr(p_mesh);
	if (existing != nullptr) {
		index = *existing;
	} else {
		index = mesh_rids.size();
		mesh_indices.insert(p_mesh, index);
		mesh_rids.push_back(p_mesh);
		mesh_order.push_back(p_mesh);
		mesh_records.push_back(MeshRecord());
		mesh_record_dirty.push_back(0);
		wind_meshes.push_back(WindMeshGPU());
		wind_mesh_dirty.push_back(0);
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
	info.mesh_index = index;

	MeshInfo *old_info = meshes.getptr(p_mesh);
	if (old_info != nullptr) {
		info.generation = old_info->generation + 1;
		info.live_instances = old_info->live_instances;
		_release_assembly_part_refs(*old_info);
		_free_mesh_page_slots(*old_info);
		if (old_info->dag_size != 0) {
			_free_dag(old_info->dag_offset, old_info->dag_size);
		}
		if (old_info->cluster_table_size != 0) {
			_free_clusters(old_info->cluster_table_offset, old_info->cluster_table_size);
		}
	}

	// GodotNext: nextgeo_foliage_instancing (#151, plan §21.4 GN-7). Extract deduplicated assembly part meshes.
	auto collect_assembly_part_mesh = [&](RID p_part_mesh) {
		if (p_part_mesh.is_null() || p_part_mesh == p_mesh) {
			return;
		}
		if (!info.assembly_unique_part_meshes.has(p_part_mesh)) {
			info.assembly_unique_part_meshes.push_back(p_part_mesh);
		}
		info.assembly_leaf_count++;
	};
	for (int i = 0; i < p_desc.assembly_parts.size(); i++) {
		collect_assembly_part_mesh(p_desc.assembly_parts[i].mesh);
	}
	for (int c = 0; c < p_desc.assembly_children.size(); c++) {
		const RenderingServerTypes::VirtualGeometryDesc::AssemblyChildDesc &child = p_desc.assembly_children[c];
		for (int i = 0; i < child.parts.size(); i++) {
			collect_assembly_part_mesh(child.parts[i].mesh);
		}
	}
	info.has_assembly = info.assembly_leaf_count > 0 && !info.assembly_unique_part_meshes.is_empty();

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

	// Resolve vertex layout, page count, page slot size, and skin influences from VGD / .ngm header.
	info.vertex_layout = p_desc.vertex_layout;
	info.skin_influences = p_desc.skin_influences;
	uint32_t ngm_page_count = p_desc.page_count;
	uint32_t ngm_page_size = p_desc.page_size;
	if ((ngm_page_count == 0 || info.vertex_layout == 0 || ngm_page_size == 0 || info.skin_influences == 0) && !p_desc.ngm_path.is_empty() && FileAccess::exists(p_desc.ngm_path)) {
		Ref<NGMFile> ngm;
		ngm.instantiate();
		if (ngm->open(p_desc.ngm_path) == OK) {
			const NGMHeader &hdr = ngm->get_header();
			if (info.vertex_layout == 0) {
				info.vertex_layout = hdr.vertex_layout;
			}
			if (ngm_page_count == 0) {
				ngm_page_count = hdr.page_count;
			}
			if (ngm_page_size == 0) {
				ngm_page_size = hdr.page_size;
			}
			if (info.skin_influences == 0 && (hdr.flags & NGM_FLAG_SKINNED) != 0u) {
				info.skin_influences = hdr.skin_influences;
			}
		}
	}
	if (info.skin_influences == 0) {
		for (int p = 0; p < p_desc.root_pages.size(); p++) {
			const Vector<uint8_t> &raw = p_desc.root_pages[p];
			if (raw.size() >= 20) {
				uint32_t header[4] = {};
				memcpy(header, raw.ptr(), sizeof(header));
				if (header[3] >= 4u && uint64_t(header[3]) <= uint64_t(raw.size()) - 16ull) {
					const uint32_t skin_start = uint32_t(raw.size()) - header[3];
					const uint8_t inf = raw[int(skin_start + 1u)];
					if (inf == 4u || inf == 8u) {
						info.skin_influences = inf;
						break;
					}
				}
			}
		}
	}
	if (info.vertex_layout != 0) {
		info.layout_stride = _ngm_vertex_float_stride(info.vertex_layout);
	}

	// Parse group -> page mapping and cluster spans from the DAG header.
	uint32_t max_dag_page_id = 0;
	bool has_dag_groups = false;
	uint32_t max_local_cluster_end = info.record.cluster_count;
	if (p_desc.dag_header.size() >= 8) {
		uint32_t group_count = 0;
		uint32_t bvh_count = 0;
		memcpy(&group_count, p_desc.dag_header.ptr(), sizeof(uint32_t));
		memcpy(&bvh_count, p_desc.dag_header.ptr() + 4, sizeof(uint32_t));
		if (8ull + uint64_t(group_count) * 60ull + uint64_t(bvh_count) * 80ull <= uint64_t(p_desc.dag_header.size())) {
			info.group_count = group_count;
			info.group_page_ids.resize(group_count);
			info.group_pages.resize(group_count);
			for (uint32_t g = 0; g < group_count; g++) {
				uint32_t g_cluster_offset = 0;
				uint32_t g_cluster_count = 0;
				uint32_t g_page_id = 0;
				memcpy(&g_cluster_offset, p_desc.dag_header.ptr() + 8 + g * 60, sizeof(uint32_t));
				memcpy(&g_cluster_count, p_desc.dag_header.ptr() + 8 + g * 60 + 4, sizeof(uint32_t));
				memcpy(&g_page_id, p_desc.dag_header.ptr() + 8 + g * 60 + 8, sizeof(uint32_t));
				info.group_page_ids[g] = g_page_id;
				info.group_pages[g].push_back(g_page_id);
				max_dag_page_id = MAX(max_dag_page_id, g_page_id);
				has_dag_groups = true;
				if (g_cluster_offset >= info.record.cluster_offset) {
					const uint32_t end = (g_cluster_offset - info.record.cluster_offset) + g_cluster_count;
					max_local_cluster_end = MAX(max_local_cluster_end, end);
				}
			}
		}
	}

	uint32_t valid_root_pages = 0;
	uint32_t root_clusters_sum = 0;
	uint32_t max_root_raw_bytes = 0;
	for (int p = 0; p < p_desc.root_pages.size(); p++) {
		uint32_t c_count = 0;
		if (_is_valid_raw_page(p_desc.root_pages[p], c_count)) {
			valid_root_pages++;
			root_clusters_sum += c_count;
			max_root_raw_bytes = MAX(max_root_raw_bytes, uint32_t(p_desc.root_pages[p].size()));
		}
	}

	const Vector<Vector<uint8_t>> *mem_pages = in_memory_mesh_pages.getptr(p_mesh);
	const uint32_t mem_page_count = mem_pages != nullptr ? uint32_t(mem_pages->size()) : 0u;

	info.total_page_count = MAX(ngm_page_count, MAX(mem_page_count, MAX(uint32_t(p_desc.root_pages.size()), has_dag_groups ? max_dag_page_id + 1u : 0u)));
	info.root_page_count = MIN(uint32_t(p_desc.root_pages.size()), info.total_page_count);
	info.page_first_cluster.resize(info.total_page_count);
	info.page_cluster_counts.resize(info.total_page_count);
	for (uint32_t p = 0; p < info.total_page_count; p++) {
		info.page_first_cluster[p] = UINT32_MAX;
		info.page_cluster_counts[p] = 0;
	}

	if (has_dag_groups) {
		for (uint32_t g = 0; g < info.group_count; g++) {
			uint32_t g_cluster_offset = 0;
			memcpy(&g_cluster_offset, p_desc.dag_header.ptr() + 8 + g * 60, sizeof(uint32_t));
			const uint32_t pid = info.group_page_ids[g];
			if (pid < info.total_page_count && g_cluster_offset >= info.record.cluster_offset) {
				info.page_first_cluster[pid] = MIN(info.page_first_cluster[pid], g_cluster_offset - info.record.cluster_offset);
			}
		}
		for (uint32_t g = 0; g < info.group_count; g++) {
			uint32_t g_cluster_offset = 0;
			uint32_t g_cluster_count = 0;
			memcpy(&g_cluster_offset, p_desc.dag_header.ptr() + 8 + g * 60, sizeof(uint32_t));
			memcpy(&g_cluster_count, p_desc.dag_header.ptr() + 8 + g * 60 + 4, sizeof(uint32_t));
			const uint32_t pid = info.group_page_ids[g];
			if (pid < info.total_page_count && info.page_first_cluster[pid] != UINT32_MAX && g_cluster_offset >= info.record.cluster_offset) {
				const uint32_t local_end = (g_cluster_offset - info.record.cluster_offset) + g_cluster_count;
				info.page_cluster_counts[pid] = MAX(info.page_cluster_counts[pid], local_end - info.page_first_cluster[pid]);
			}
		}
	} else {
		uint32_t seq_cluster = 0;
		for (uint32_t p = 0; p < info.root_page_count; p++) {
			uint32_t c_count = 0;
			if (_is_valid_raw_page(p_desc.root_pages[int(p)], c_count)) {
				info.page_first_cluster[p] = seq_cluster;
				info.page_cluster_counts[p] = c_count;
				seq_cluster += c_count;
			}
		}
		max_local_cluster_end = MAX(max_local_cluster_end, root_clusters_sum);
	}

	info.page_to_slot.resize(info.total_page_count);
	for (uint32_t p = 0; p < info.total_page_count; p++) {
		info.page_to_slot[p] = INVALID_PAGE_SLOT;
	}
	info.resident_bitset.resize((info.total_page_count + 31u) / 32u);
	for (uint32_t i = 0; i < info.resident_bitset.size(); i++) {
		info.resident_bitset[i] = 0u;
	}
	info.refine_ok_bitset.resize((info.group_count + 31u) / 32u);
	for (uint32_t i = 0; i < info.refine_ok_bitset.size(); i++) {
		info.refine_ok_bitset[i] = 0u;
	}
	info.page_parents.resize(info.total_page_count);
	info.page_children.resize(info.total_page_count);
	info.page_topo_depth.resize(info.total_page_count);
	info.resident_child_count.resize(info.total_page_count);
	for (uint32_t p = 0; p < info.total_page_count; p++) {
		info.page_topo_depth[p] = 0u;
		info.resident_child_count[p] = 0u;
	}

	// Adapt fixed slot size from VGD header when initializing an empty arena.
	const uint32_t needed_slot_bytes = (MAX(ngm_page_size, max_root_raw_bytes) + 15u) & ~15u;
	if (occupied_slot_count == 0 && needed_slot_bytes > 0) {
		if (page_arena_override_bytes > 0 && ngm_page_size > 0) {
			slot_page_size = MAX(needed_slot_bytes, 256u);
		} else {
			slot_page_size = MAX(needed_slot_bytes, 64u * 1024u);
		}
	} else if (needed_slot_bytes > slot_page_size) {
		slot_page_size = needed_slot_bytes;
	}

	if (valid_root_pages > 0 && max_local_cluster_end > 0) {
		const uint32_t cluster_bytes = max_local_cluster_end * sizeof(ClusterGPUDesc);
		info.cluster_blob.resize(int(cluster_bytes));
		memset(info.cluster_blob.ptrw(), 0, cluster_bytes);
		info.cluster_refine.resize(max_local_cluster_end);
		if (info.skin_influences == 4u || info.skin_influences == 8u) {
			info.cluster_skin_offsets.resize(max_local_cluster_end);
			for (uint32_t c = 0; c < max_local_cluster_end; c++) {
				info.cluster_skin_offsets[c] = UINT32_MAX;
			}
			info.cluster_skin_bones.resize(max_local_cluster_end);
		}

		if (_ensure_page_arena()) {
			uint32_t pinned_ok = 0;
			for (uint32_t p = 0; p < info.root_page_count; p++) {
				uint32_t c_count = 0;
				if (!_is_valid_raw_page(p_desc.root_pages[int(p)], c_count)) {
					continue;
				}
				const uint32_t slot = _allocate_page_slot(true);
				if (slot != INVALID_PAGE_SLOT && _install_page_in_slot(info, p, slot, p_desc.root_pages[int(p)], true, false)) {
					pinned_ok++;
				}
			}

			const uint32_t cluster_offset = _alloc_clusters(cluster_bytes);
			if (cluster_offset != UINT32_MAX && pinned_ok > 0) {
				info.cluster_table_offset = cluster_offset;
				info.cluster_table_size = cluster_bytes;
				_upload_clusters(info.cluster_table_offset, info.cluster_blob.ptr(), info.cluster_table_size);
				if (!info.cluster_skin_offsets.is_empty() && _ensure_cluster_skin_buffer()) {
					const uint32_t dst_off = info.cluster_table_offset / 4u;
					const uint32_t upload_bytes = uint32_t(info.cluster_skin_offsets.size()) * sizeof(uint32_t);
					if (dst_off + upload_bytes <= cluster_skin_capacity) {
						_upload_cluster_skin(dst_off, reinterpret_cast<const uint8_t *>(info.cluster_skin_offsets.ptr()), upload_bytes);
					}
				}
				info.record.page_table_offset = info.cluster_table_offset / sizeof(ClusterGPUDesc);
				info.record.page_count = pinned_ok;
				if (info.record.cluster_count == 0) {
					info.record.cluster_count = max_local_cluster_end;
				}
			} else {
				_free_mesh_page_slots(info);
				info.cluster_blob.clear();
				info.cluster_refine.clear();
				info.cluster_skin_offsets.clear();
				info.cluster_skin_bones.clear();
			}
		}
	}

	if (info.total_page_count <= info.root_page_count) {
		info.page_dag_ready = true;
	} else if (mem_pages != nullptr) {
		// Derive page_parents directly from in-memory pages if already registered.
		for (uint32_t p = 0; p < MIN(info.total_page_count, uint32_t(mem_pages->size())); p++) {
			const Vector<uint8_t> &raw = (*mem_pages)[int(p)];
			uint32_t c_count = 0;
			if (!_is_valid_raw_page(raw, c_count)) {
				continue;
			}
			for (uint32_t i = 0; i < c_count; i++) {
				int32_t refined_group = -1;
				memcpy(&refined_group, raw.ptr() + 16u + i * 80u + 60u, sizeof(int32_t));
				if (refined_group >= 0 && uint32_t(refined_group) < info.group_page_ids.size()) {
					const uint32_t child_page = info.group_page_ids[uint32_t(refined_group)];
					if (child_page != p && child_page < info.total_page_count && !info.page_parents[child_page].has(p)) {
						info.page_parents[child_page].push_back(p);
					}
				}
			}
		}
		_rebuild_page_children_and_depths(info);
		info.page_dag_ready = true;
	}

	_recompute_mesh_refine_ok(info);
	residency_layout_dirty = true;

	meshes[p_mesh] = info;
	if (info.live_instances > 0 && info.has_assembly) {
		_acquire_assembly_part_refs(meshes[p_mesh]);
	}
	assemblies_gpu_dirty = true;
	mesh_records[index] = info.record;
	mesh_record_dirty[index] = 1;
	mesh_records_dirty = true;

	// GodotNext: nextgeo_foliage_wind (#152). Populate compact per-mesh wind bone table (0 palette bytes per instance).
	WindMeshGPU wm;
	if (!p_desc.wind_bones.is_empty() && (info.skin_influences == 4u || info.skin_influences == 8u)) {
		const uint32_t bcount = MIN(uint32_t(p_desc.wind_bones.size()), WIND_MAX_BONES);
		wm.bone_count = bcount;
		wm.max_sway = MAX(p_desc.wind_max_sway, 0.0f);
		wm.base_height = p_desc.wind_base_height;
		wm.top_height = p_desc.wind_top_height;
		for (uint32_t b = 0; b < bcount; b++) {
			const RenderingServerTypes::VirtualGeometryDesc::WindBoneDesc &src = p_desc.wind_bones[int(b)];
			WindBoneGPU &dst = wm.bones[b];
			dst.rest_pos[0] = float(src.rest_position.x);
			dst.rest_pos[1] = float(src.rest_position.y);
			dst.rest_pos[2] = float(src.rest_position.z);
			dst.lever_length = MAX(src.lever_length, 0.25f);
			Vector3 ax = src.axis;
			if (ax.length_squared() > real_t(1e-10)) {
				ax.normalize();
			} else {
				ax = Vector3(0.0f, 1.0f, 0.0f);
			}
			dst.axis[0] = float(ax.x);
			dst.axis[1] = float(ax.y);
			dst.axis[2] = float(ax.z);
			dst.stiffness = CLAMP(src.stiffness, 0.0f, 1.0f);
			dst.max_sway = MAX(src.max_sway, 0.0f);
			dst.sway_phase = src.sway_phase;
			dst.branch_weight = (b == 0u) ? 0.0f : 1.0f;
			wm.max_sway = MAX(wm.max_sway, dst.max_sway);
		}
	}
	if (index < wind_meshes.size()) {
		wind_meshes[index] = wm;
		wind_mesh_dirty[index] = 1;
		wind_meshes_dirty = true;
		wind_params.mesh_count = wind_meshes.size();
		wind_params_dirty = true;
	}

	// Live records of this mesh pick up the new mask and virtual-mesh index.
	for (uint32_t slot = 0; slot < used_slots; slot++) {
		if (!slot_states[slot].allocated || slot_states[slot].gpu_managed || records[slot].virtual_mesh != index) {
			continue;
		}
		records[slot].surface_mask = p_desc.eligible_surface_mask;
		records[slot].pad0 = p_desc.eligible_surface_mask;
		_mark_dirty(slot);
		_sync_record_skin_info(slot);
	}
	if (RSG::mesh_storage != nullptr) {
		RSG::mesh_storage->mesh_on_virtual_geometry_changed(p_mesh);
		RSG::mesh_storage->mesh_notify_dependency_changed(p_mesh);
	}
}

void RendererVirtualMeshStorage::mesh_clear_virtual_geometry(RID p_mesh, uint32_t p_reason) {
	ERR_FAIL_COND(p_mesh.is_null());
	in_memory_mesh_pages.erase(p_mesh);
	const uint32_t *index = mesh_indices.getptr(p_mesh);
	if (index == nullptr) {
		if (p_reason != RenderingServerTypes::VIRTUAL_GEOMETRY_CLEAR_NONE) {
			mesh_clear_reasons[p_mesh] = p_reason;
		}
		return;
	}
	mesh_records[*index] = MeshRecord();
	mesh_record_dirty[*index] = 1;
	mesh_records_dirty = true;
	if (*index < wind_meshes.size()) {
		wind_meshes[*index] = WindMeshGPU();
		wind_mesh_dirty[*index] = 1;
		wind_meshes_dirty = true;
	}
	MeshInfo *info = meshes.getptr(p_mesh);
	if (info != nullptr) {
		_release_assembly_part_refs(*info);
		_free_mesh_page_slots(*info);
		_free_dag(info->dag_offset, info->dag_size);
		_free_clusters(info->cluster_table_offset, info->cluster_table_size);
	}
	meshes.erase(p_mesh);
	assemblies_gpu_dirty = true;

	// Purge pending / staged requests for the cleared mesh.
	LocalVector<QueuedPageKey> dead_keys;
	for (const KeyValue<QueuedPageKey, PrioritizedPageRequest> &E : pending_requests) {
		if (E.key.mesh == p_mesh) {
			dead_keys.push_back(E.key);
		}
	}
	for (const QueuedPageKey &key : dead_keys) {
		pending_requests.erase(key);
	}
	for (int32_t i = int32_t(staged_pages.size()) - 1; i >= 0; i--) {
		if (staged_pages[uint32_t(i)].mesh == p_mesh) {
			staged_pages.remove_at(uint32_t(i));
		}
	}

	if (meshes.is_empty()) {
		_reset_pages();
		page_capacity = 0;
		page_slots.clear();
		free_page_slots.clear();
		lru_head = UINT32_MAX;
		lru_tail = UINT32_MAX;
		occupied_slot_count = 0;
		pinned_slot_count = 0;
	}
	residency_layout_dirty = true;

	for (uint32_t slot = 0; slot < used_slots; slot++) {
		if (!slot_states[slot].allocated || slot_states[slot].gpu_managed || records[slot].virtual_mesh != *index) {
			continue;
		}
		records[slot].virtual_mesh = UINT32_MAX;
		records[slot].surface_mask = 0;
		records[slot].pad0 = 0;
		_mark_dirty(slot);
		_sync_record_skin_info(slot);
	}
	// Keep the reason for the routing fallback overlay (#79); the record store
	// only needs to know the mesh is gone.
	if (p_reason != RenderingServerTypes::VIRTUAL_GEOMETRY_CLEAR_NONE) {
		mesh_clear_reasons[p_mesh] = p_reason;
	} else {
		mesh_clear_reasons.erase(p_mesh);
	}
	if (RSG::mesh_storage != nullptr) {
		RSG::mesh_storage->mesh_on_virtual_geometry_changed(p_mesh);
		RSG::mesh_storage->mesh_notify_dependency_changed(p_mesh);
	}
}

bool RendererVirtualMeshStorage::mesh_has_virtual_geometry(RID p_mesh) const {
	return meshes.has(p_mesh);
}

uint32_t RendererVirtualMeshStorage::mesh_get_live_instance_count(RID p_mesh) const {
	const MeshInfo *info = meshes.getptr(p_mesh);
	return info != nullptr ? info->live_instances : 0u;
}

const RenderingServerTypes::VirtualGeometryDesc *RendererVirtualMeshStorage::mesh_get_virtual_geometry_desc(RID p_mesh) const {
	const MeshInfo *info = meshes.getptr(p_mesh);
	return info != nullptr ? &info->desc : nullptr;
}

bool RendererVirtualMeshStorage::mesh_surface_is_virtual(RID p_mesh, uint32_t p_surface) const {
	const MeshInfo *info = meshes.getptr(p_mesh);
	if (info == nullptr || p_surface >= 32) {
		return false;
	}
	return (info->desc.eligible_surface_mask & (1u << p_surface)) != 0;
}

bool RendererVirtualMeshStorage::mesh_is_stripped_to_coarse(RID p_mesh) const {
	const MeshInfo *info = meshes.getptr(p_mesh);
	return info != nullptr && info->desc.stripped_to_coarse;
}

bool RendererVirtualMeshStorage::mesh_is_skinned_virtual_geometry(RID p_mesh) const {
	const MeshInfo *info = meshes.getptr(p_mesh);
	return info != nullptr && (info->skin_influences == 4u || info->skin_influences == 8u);
}

uint32_t RendererVirtualMeshStorage::mesh_clear_reason(RID p_mesh) const {
	const uint32_t *reason = mesh_clear_reasons.getptr(p_mesh);
	return reason != nullptr ? *reason : 0;
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

	const uint32_t bslot = _bucket_slot_allocate(1u);

	Record &record = records[slot];
	record = Record();
	record.bucket_base = bslot * 32u;
	instance_attributes[slot] = InstanceAttributes();
	instance_attributes[slot].meta[1] = slot;
	SlotState &state = slot_states[slot];
	state.instance = p_instance;
	state.allocated = true;
	state.dirty = true;
	state.dirty_last = false;
	state.initialized = false;
	state.gpu_managed = false;
	state.free_frame = UINT32_MAX;
	state.bucket_slot = bslot;
	state.bucket_slot_count = 1u;
	live_records++;
	_record_reused(slot);

	const uint32_t *index = mesh_indices.getptr(p_mesh);
	MeshInfo *info = meshes.getptr(p_mesh);
	bool notify_first_instance = false;
	if (index != nullptr && info != nullptr) {
		record.virtual_mesh = *index;
		record.surface_mask = info->desc.eligible_surface_mask;
		record.pad0 = info->desc.eligible_surface_mask;
		info->live_instances++;
		notify_first_instance = info->live_instances == 1u;
		if (notify_first_instance && info->has_assembly) {
			_acquire_assembly_part_refs(*info);
		}
	}
	bucket_slot_set_surface_masks(bslot, record.surface_mask, record.pad0, 0u, 0u);
	record.flags = RECORD_VALID | RECORD_VISIBLE;
	if (notify_first_instance && RSG::mesh_storage != nullptr) {
		RSG::mesh_storage->mesh_on_virtual_geometry_changed(p_mesh);
	}
	return slot;
}

void RendererVirtualMeshStorage::instance_record_free(uint32_t p_record) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	// GodotNext: nextgeo_skinned_runtime (#160). Release skeleton ref and recycle palette bones before freeing slot.
	instance_record_set_skeleton(p_record, RID());
	multimesh_range_detach(p_record);
	SlotState &state = slot_states[p_record];
	if (state.bucket_slot != UINT32_MAX && state.bucket_slot_count > 0u) {
		_bucket_slot_free(state.bucket_slot, state.bucket_slot_count);
		state.bucket_slot = UINT32_MAX;
		state.bucket_slot_count = 0u;
	}
	state.instance = RID();
	state.allocated = false;
	state.dirty = true;
	state.dirty_last = false;
	state.gpu_managed = false;
	state.free_frame = frame + 1;
	// GodotNext: spline_meshes (#161). Release any spline slot owned by the freed record.
	if ((records[p_record].flags & RECORD_HAS_SPLINE) != 0u) {
		const uint32_t spline_slot = records[p_record].flags >> RECORD_SPLINE_INDEX_SHIFT;
		if (spline_slot < spline_allocated.size() && spline_allocated[spline_slot]) {
			spline_allocated[spline_slot] = 0;
			spline_dirty[spline_slot] = 0;
			spline_dirty_last[spline_slot] = 0;
			spline_rewritten[spline_slot] = 0;
			if (live_splines > 0) {
				live_splines--;
			}
			if (!spline_free_next.has(spline_slot)) {
				spline_free_next.push_back(spline_slot);
			}
		}
	}
	records[p_record].flags = 0;
	instance_attributes[p_record] = InstanceAttributes();
	live_records--;
	const uint32_t index = records[p_record].virtual_mesh;
	RID notify_last_mesh;
	if (index < mesh_rids.size()) {
		const RID mesh_rid = mesh_rids[index];
		MeshInfo *info = meshes.getptr(mesh_rid);
		if (info != nullptr && info->live_instances > 0) {
			info->live_instances--;
			if (info->live_instances == 0u) {
				if (info->assembly_part_refs_held) {
					_release_assembly_part_refs(*info);
				}
				notify_last_mesh = mesh_rid;
			}
		}
	}
	// Reusable on the next frame, not this one.
	if (!free_next.has(p_record)) {
		free_next.push_back(p_record);
	}
	if (notify_last_mesh.is_valid() && RSG::mesh_storage != nullptr) {
		RSG::mesh_storage->mesh_on_virtual_geometry_changed(notify_last_mesh);
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
		// GodotNext: nextgeo_masked_two_sided (#153) & nextgeo_displacement (#150). Preserve
		// prev_world_aabb_min[3] (programmable_raster_distance) and prev_world_aabb_max[3] (displacement_bound).
		for (int index = 0; index < 3; index++) {
			record.prev_world_aabb_min[index] = record.world_aabb_min[index];
		}
		for (int index = 0; index < 3; index++) {
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
		// GodotNext: nextgeo_masked_two_sided (#153) & nextgeo_displacement (#150). Preserve
		// prev_world_aabb_min[3] (programmable_raster_distance) and prev_world_aabb_max[3] (displacement_bound).
		for (int index = 0; index < 3; index++) {
			record.prev_world_aabb_min[index] = record.world_aabb_min[index];
		}
		for (int index = 0; index < 3; index++) {
			record.prev_world_aabb_max[index] = record.world_aabb_max[index];
		}
		_write_transform(record, p_transform, p_world_aabb, p_lod_bias);
		record.flags |= RECORD_MOVED;
		record.flags &= ~RECORD_TELEPORTED;
	}
	if (basis_has_non_uniform_scale(p_transform.basis)) {
		record.flags |= RECORD_NON_UNIFORM_SCALE;
	} else {
		record.flags &= ~RECORD_NON_UNIFORM_SCALE;
	}
	record.flags |= RECORD_VALID | RECORD_VISIBLE;
	_mark_dirty(p_record);
}

void RendererVirtualMeshStorage::instance_record_set_state(uint32_t p_record, uint32_t p_flags) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	Record &record = records[p_record];
	constexpr uint32_t preserved_mask = RECORD_VALID | RECORD_VISIBLE | RECORD_MOVED | RECORD_TELEPORTED | RECORD_RT_STATIC | RECORD_RT_SKINNED | RECORD_RT_ALPHA_AS_OPAQUE | RECORD_RT_FAR | RECORD_VSM_DYNAMIC | RECORD_RT_EVALUATE_VERTEX_OFFSET | RECORD_SKINNED_PALETTE | RECORD_HAS_SPLINE | RECORD_SPLINE_INDEX_MASK | RECORD_MULTIMESH_ELEMENT | RECORD_NON_UNIFORM_SCALE | RECORD_HAS_INSTANCE_COLOR | RECORD_HAS_INSTANCE_CUSTOM | RECORD_ASSEMBLY_PART;
	uint32_t keep = record.flags & preserved_mask;
	if ((p_flags & RECORD_VISIBLE) == 0u) {
		keep &= ~RECORD_VISIBLE;
	}
	record.flags = keep | (p_flags & ~preserved_mask);
	_mark_dirty(p_record);
}

void RendererVirtualMeshStorage::instance_record_set_layer_mask(uint32_t p_record, uint32_t p_layer_mask) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	records[p_record].layer_mask = p_layer_mask;
	_mark_dirty(p_record);
}

// GodotNext: nextgeo_displacement (#150, plan §21.4 N11). Stores max displacement magnitude (meters)
// in prev_world_aabb_max[3] so cluster cull and CPU_cut_hash inflate BVH/group/cluster sphere bounds.
void RendererVirtualMeshStorage::instance_record_set_displacement_bound(uint32_t p_record, float p_magnitude) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	const float bound = (!Math::is_finite(p_magnitude) || p_magnitude < 0.0f) ? 0.0f : p_magnitude;
	if (records[p_record].prev_world_aabb_max[3] == bound) {
		return;
	}
	records[p_record].prev_world_aabb_max[3] = bound;
	_mark_dirty(p_record);
}

// GodotNext: nextgeo_masked_two_sided (#153, plan §21.4.1). Stores programmable raster cutoff distance
// (meters; 0 = always programmable) in prev_world_aabb_min[3] so cluster cull routes clusters past the
// distance to the FIXED bin.
void RendererVirtualMeshStorage::instance_record_set_programmable_raster_distance(uint32_t p_record, float p_distance) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	const float dist = (!Math::is_finite(p_distance) || p_distance < 0.0f) ? 0.0f : p_distance;
	if (records[p_record].prev_world_aabb_min[3] == dist) {
		return;
	}
	records[p_record].prev_world_aabb_min[3] = dist;
	_mark_dirty(p_record);
}

// GodotNext: nextgeo_vertex_offset (#164, plan §21.4 N5).
void RendererVirtualMeshStorage::instance_record_set_vertex_offset_bound(uint32_t p_record, float p_bound) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	const uint32_t packed = uint32_t(pack_vertex_offset_bound_half(p_bound)) << 16u;
	const uint32_t merged = (records[p_record].gi_flags & 0xFFFFu) | packed;
	if (records[p_record].gi_flags == merged) {
		return;
	}
	records[p_record].gi_flags = merged;
	_mark_dirty(p_record);
}

void RendererVirtualMeshStorage::instance_record_set_vertex_offset_disable_distance(uint32_t p_record, float p_distance) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	const float dist = (!Math::is_finite(p_distance) || p_distance < 0.0f) ? 0.0f : p_distance;
	uint32_t bits = 0u;
	memcpy(&bits, &dist, sizeof(uint32_t));
	if (records[p_record].page_table_offset == bits) {
		return;
	}
	records[p_record].page_table_offset = bits;
	_mark_dirty(p_record);
}

void RendererVirtualMeshStorage::instance_record_set_rt_evaluate_vertex_offset(uint32_t p_record, bool p_enable) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	const uint32_t next_flags = p_enable ? (records[p_record].flags | RECORD_RT_EVALUATE_VERTEX_OFFSET) : (records[p_record].flags & ~RECORD_RT_EVALUATE_VERTEX_OFFSET);
	if (records[p_record].flags == next_flags) {
		return;
	}
	records[p_record].flags = next_flags;
	_mark_dirty(p_record);
}

void RendererVirtualMeshStorage::instance_record_set_gi_flags(uint32_t p_record, uint32_t p_gi_flags) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	const uint32_t merged = (records[p_record].gi_flags & 0xFFFF0000u) | (p_gi_flags & 0xFFFFu);
	if (records[p_record].gi_flags == merged) {
		return;
	}
	records[p_record].gi_flags = merged;
	_mark_dirty(p_record);
}

void RendererVirtualMeshStorage::instance_record_set_surface_mask(uint32_t p_record, uint32_t p_surface_mask, uint32_t p_shadow_surface_mask) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	const uint32_t shadow_mask = p_shadow_surface_mask == UINT32_MAX ? p_surface_mask : p_shadow_surface_mask;
	if (slot_states[p_record].bucket_slot != UINT32_MAX && slot_states[p_record].bucket_slot_count == 1u) {
		const uint32_t bslot = slot_states[p_record].bucket_slot;
		if (bslot < bucket_surface_masks.size()) {
			bucket_slot_set_surface_masks(bslot, p_surface_mask, shadow_mask, bucket_surface_masks[bslot].cull_disabled_mask, bucket_surface_masks[bslot].cull_front_mask);
		}
	}
	if (records[p_record].surface_mask == p_surface_mask && records[p_record].pad0 == shadow_mask) {
		return;
	}
	records[p_record].surface_mask = p_surface_mask;
	records[p_record].pad0 = shadow_mask;
	_mark_dirty(p_record);
}

void RendererVirtualMeshStorage::instance_record_set_surface_cull_masks(uint32_t p_record, uint32_t p_cull_disabled_mask, uint32_t p_cull_front_mask) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	if (slot_states[p_record].bucket_slot != UINT32_MAX && slot_states[p_record].bucket_slot_count == 1u) {
		const uint32_t bslot = slot_states[p_record].bucket_slot;
		if (bslot < bucket_surface_masks.size()) {
			bucket_slot_set_surface_masks(bslot, bucket_surface_masks[bslot].surface_mask, bucket_surface_masks[bslot].shadow_surface_mask, p_cull_disabled_mask, p_cull_front_mask);
		}
	}
	Record &record = records[p_record];
	if (record.pad1[0] == p_cull_disabled_mask && record.pad1[1] == p_cull_front_mask) {
		return;
	}
	record.pad1[0] = p_cull_disabled_mask;
	record.pad1[1] = p_cull_front_mask;
	_mark_dirty(p_record);
}

RID RendererVirtualMeshStorage::instance_record_get_instance(uint32_t p_record) const {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return RID();
	}
	return slot_states[p_record].instance;
}

void RendererVirtualMeshStorage::instance_record_set_rt(uint32_t p_record, const uint32_t p_rt_fields[6]) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	Record &record = records[p_record];
	if (record.tlas_instance_index == p_rt_fields[0] &&
			record.blas_handle == p_rt_fields[1] &&
			record.rt_arena_chunk == p_rt_fields[2] &&
			record.rt_arena_offset == p_rt_fields[3] &&
			record.src_page_table == p_rt_fields[4] &&
			record.card_set == p_rt_fields[5]) {
		return;
	}
	record.tlas_instance_index = p_rt_fields[0];
	record.blas_handle = p_rt_fields[1];
	record.rt_arena_chunk = p_rt_fields[2];
	record.rt_arena_offset = p_rt_fields[3];
	record.src_page_table = p_rt_fields[4];
	record.card_set = p_rt_fields[5];
	_mark_dirty(p_record);
}

void RendererVirtualMeshStorage::instance_record_set_rt_flags(uint32_t p_record, uint32_t p_rt_flags) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	Record &record = records[p_record];
	constexpr uint32_t rt_mask = RECORD_RT_STATIC | RECORD_RT_SKINNED | RECORD_RT_ALPHA_AS_OPAQUE | RECORD_RT_FAR;
	const uint32_t next_flags = (record.flags & ~rt_mask) | (p_rt_flags & rt_mask);
	if (record.flags == next_flags) {
		return;
	}
	record.flags = next_flags;
	_mark_dirty(p_record);
}

// GodotNext: vsm_caching (#212).
void RendererVirtualMeshStorage::instance_record_set_vsm_dynamic(uint32_t p_record, bool p_dynamic) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	Record &record = records[p_record];
	const uint32_t next_flags = p_dynamic ? (record.flags | RECORD_VSM_DYNAMIC) : (record.flags & ~RECORD_VSM_DYNAMIC);
	if (record.flags == next_flags) {
		return;
	}
	record.flags = next_flags;
	_mark_dirty(p_record);
}

// GodotNext: spline_meshes (#161, plan §21.4 contract N12).
void RendererVirtualMeshStorage::instance_record_set_spline_deform(uint32_t p_record, bool p_active, const SplineDeformPose &p_pose, bool p_teleport) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	Record &record = records[p_record];
	const bool had_spline = (record.flags & RECORD_HAS_SPLINE) != 0u;
	if (!p_active) {
		if (!had_spline) {
			return;
		}
		const uint32_t slot = record.flags >> RECORD_SPLINE_INDEX_SHIFT;
		if (slot < spline_allocated.size() && spline_allocated[slot]) {
			spline_allocated[slot] = 0;
			spline_dirty[slot] = 0;
			spline_dirty_last[slot] = 0;
			spline_rewritten[slot] = 0;
			if (live_splines > 0) {
				live_splines--;
			}
			if (!spline_free_next.has(slot)) {
				spline_free_next.push_back(slot);
			}
		}
		record.flags &= ~(RECORD_HAS_SPLINE | RECORD_SPLINE_INDEX_MASK);
		_mark_dirty(p_record);
		return;
	}

	uint32_t slot = UINT32_MAX;
	if (had_spline) {
		slot = record.flags >> RECORD_SPLINE_INDEX_SHIFT;
		if (slot >= spline_entries.size() || !spline_allocated[slot]) {
			slot = UINT32_MAX;
		}
	}
	const bool newly_allocated = (slot == UINT32_MAX);
	if (newly_allocated) {
		if (!spline_free_now.is_empty()) {
			slot = spline_free_now[spline_free_now.size() - 1];
			spline_free_now.remove_at(spline_free_now.size() - 1);
		} else {
			ERR_FAIL_COND_MSG(spline_entries.size() >= 0x7Fu, "NextGeo: spline deformation slot limit (127) reached.");
			slot = spline_entries.size();
			spline_entries.push_back(SplineDeformEntry());
			spline_allocated.push_back(0);
			spline_dirty.push_back(0);
			spline_dirty_last.push_back(0);
			spline_rewritten.push_back(0);
		}
		spline_allocated[slot] = 1;
		spline_dirty_last[slot] = 0;
		spline_rewritten[slot] = 0;
		live_splines++;
		record.flags = (record.flags & ~RECORD_SPLINE_INDEX_MASK) | RECORD_HAS_SPLINE | (slot << RECORD_SPLINE_INDEX_SHIFT);
		_mark_dirty(p_record);
	}

	SplineDeformEntry &entry = spline_entries[slot];
	if (newly_allocated || p_teleport) {
		entry.current = p_pose;
		entry.previous = p_pose;
	} else {
		if (!spline_dirty[slot]) {
			entry.previous = entry.current;
		}
		entry.current = p_pose;
	}
	spline_dirty[slot] = 1;
}

const SplineDeformEntry &RendererVirtualMeshStorage::get_spline_entry(uint32_t p_index) const {
	static const SplineDeformEntry fallback;
	ERR_FAIL_UNSIGNED_INDEX_V(p_index, spline_entries.size(), fallback);
	return spline_entries[p_index];
}

// GodotNext: nextgeo_skinned_runtime (#160, plan §21.4 contract N8).
void RendererVirtualMeshStorage::instance_record_set_skeleton(uint32_t p_record, RID p_skeleton) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	// GodotNext: nextgeo_foliage_wind (#152). Procedural wind meshes evaluate bones in-shader with
	// 0 bytes of per-instance skin palette arena memory, even when an imported glTF retains a Skeleton3D.
	if (p_skeleton.is_valid() && mesh_index_has_wind(records[p_record].virtual_mesh)) {
		p_skeleton = RID();
	}
	SlotState &state = slot_states[p_record];
	if (state.skeleton == p_skeleton) {
		_sync_record_skin_info(p_record);
		return;
	}
	if (state.skeleton.is_valid()) {
		SkeletonPaletteSlot *old_sk = skeleton_palettes.getptr(state.skeleton);
		if (old_sk != nullptr && old_sk->ref_count > 0u) {
			old_sk->ref_count--;
			if (old_sk->ref_count == 0u) {
				if (old_sk->bone_offset != UINT32_MAX && old_sk->bone_count > 0u) {
					_free_palette_bones(old_sk->bone_offset, old_sk->bone_count * 2u);
				}
				skeleton_palettes.erase(state.skeleton);
			}
		}
	}
	state.skeleton = p_skeleton;
	if (p_skeleton.is_valid()) {
		SkeletonPaletteSlot *sk = skeleton_palettes.getptr(p_skeleton);
		if (sk != nullptr) {
			sk->ref_count++;
		} else {
			SkeletonPaletteSlot slot;
			slot.ref_count = 1u;
			skeleton_palettes[p_skeleton] = slot;
		}
		if (RSG::mesh_storage != nullptr && !RSG::mesh_storage->skeleton_is_2d(p_skeleton)) {
			uint32_t bone_count = 0u;
			const float *data = RSG::mesh_storage->skeleton_get_3d_data(p_skeleton, bone_count);
			if (data != nullptr && bone_count > 0u) {
				skeleton_update_palette(p_skeleton, data, bone_count);
			}
		}
	}
	_sync_record_skin_info(p_record);
}

void RendererVirtualMeshStorage::skeleton_update_palette(RID p_skeleton, const float *p_bone_data, uint32_t p_bone_count) {
	if (p_skeleton.is_null()) {
		return;
	}
	SkeletonPaletteSlot *sk = skeleton_palettes.getptr(p_skeleton);
	if (sk == nullptr) {
		return;
	}
	if (p_bone_data == nullptr || p_bone_count == 0u) {
		if (sk->bone_offset != UINT32_MAX && sk->bone_count > 0u) {
			_free_palette_bones(sk->bone_offset, sk->bone_count * 2u);
		}
		sk->bone_offset = UINT32_MAX;
		sk->bone_count = 0u;
		sk->cur_data.clear();
		sk->prev_data.clear();
		sk->max_bone_scale = 1.0f;
		sk->has_nonzero_pose = false;
		sk->dirty_this_frame = false;
		sk->dirty_last_frame = false;
		_sync_skeleton_records(p_skeleton);
		return;
	}

	const uint32_t float_count = p_bone_count * 12u;
	const uint32_t half_bytes = p_bone_count * 48u;
	bool incoming_nonzero = false;
	for (uint32_t i = 0; i < float_count; i++) {
		if (p_bone_data[i] != 0.0f) {
			incoming_nonzero = true;
			break;
		}
	}

	const bool reallocated = (sk->bone_count != p_bone_count || sk->bone_offset == UINT32_MAX);
	if (reallocated) {
		if (sk->bone_offset != UINT32_MAX && sk->bone_count > 0u) {
			_free_palette_bones(sk->bone_offset, sk->bone_count * 2u);
		}
		const uint32_t new_offset = _alloc_palette_bones(p_bone_count * 2u);
		// Re-fetch pointer in case skeleton_palettes was iterated/touched (defensive).
		sk = skeleton_palettes.getptr(p_skeleton);
		if (sk == nullptr) {
			return;
		}
		if (new_offset == UINT32_MAX) {
			sk->bone_offset = UINT32_MAX;
			sk->bone_count = 0u;
			sk->cur_data.clear();
			sk->prev_data.clear();
			sk->max_bone_scale = 1.0f;
			sk->has_nonzero_pose = false;
			sk->dirty_this_frame = false;
			sk->dirty_last_frame = false;
			_sync_skeleton_records(p_skeleton);
			return;
		}
		sk->bone_offset = new_offset;
		sk->bone_count = p_bone_count;
		sk->cur_data.resize(float_count);
		sk->prev_data.resize(float_count);
		sk->has_nonzero_pose = false;
		sk->dirty_this_frame = false;
		sk->dirty_last_frame = false;
	}

	if (reallocated || !sk->has_nonzero_pose) {
		// Initial pose upload (or bone count resize): seed both current and previous palettes identically
		// so the first rendered pose produces zero spawn velocity spike.
		memcpy(sk->cur_data.ptr(), p_bone_data, half_bytes);
		memcpy(sk->prev_data.ptr(), p_bone_data, half_bytes);
		_upload_skin_palette(sk->bone_offset * 48u, reinterpret_cast<const uint8_t *>(sk->cur_data.ptr()), half_bytes);
		_upload_skin_palette((sk->bone_offset + sk->bone_count) * 48u, reinterpret_cast<const uint8_t *>(sk->prev_data.ptr()), half_bytes);
		sk->has_nonzero_pose = incoming_nonzero;
		sk->dirty_this_frame = false;
		sk->dirty_last_frame = false;
	} else {
		if (!sk->dirty_this_frame) {
			// Snapshot previous frame's current palette into the previous-frame slot BEFORE overwriting current.
			memcpy(sk->prev_data.ptr(), sk->cur_data.ptr(), half_bytes);
			_upload_skin_palette((sk->bone_offset + sk->bone_count) * 48u, reinterpret_cast<const uint8_t *>(sk->prev_data.ptr()), half_bytes);
			sk->dirty_this_frame = true;
		}
		memcpy(sk->cur_data.ptr(), p_bone_data, half_bytes);
		_upload_skin_palette(sk->bone_offset * 48u, reinterpret_cast<const uint8_t *>(sk->cur_data.ptr()), half_bytes);
	}

	sk->max_bone_scale = incoming_nonzero ? _palette_max_bone_scale(sk->cur_data.ptr(), sk->bone_count) : 1.0f;
	_sync_skeleton_records(p_skeleton);
}

void RendererVirtualMeshStorage::skeleton_data_changed(RID p_skeleton) {
	if (p_skeleton.is_null() || !skeleton_palettes.has(p_skeleton)) {
		return;
	}
	if (RSG::mesh_storage != nullptr) {
		if (RSG::mesh_storage->skeleton_is_2d(p_skeleton)) {
			skeleton_update_palette(p_skeleton, nullptr, 0u);
			return;
		}
		uint32_t bone_count = 0u;
		const float *data = RSG::mesh_storage->skeleton_get_3d_data(p_skeleton, bone_count);
		skeleton_update_palette(p_skeleton, data, bone_count);
	}
}

void RendererVirtualMeshStorage::skeleton_free_notify(RID p_skeleton) {
	if (p_skeleton.is_null()) {
		return;
	}
	for (uint32_t slot = 0; slot < used_slots; slot++) {
		if (slot_states[slot].allocated && slot_states[slot].skeleton == p_skeleton) {
			instance_record_set_skeleton(slot, RID());
		}
	}
	SkeletonPaletteSlot *sk = skeleton_palettes.getptr(p_skeleton);
	if (sk != nullptr) {
		if (sk->bone_offset != UINT32_MAX && sk->bone_count > 0u) {
			_free_palette_bones(sk->bone_offset, sk->bone_count * 2u);
		}
		skeleton_palettes.erase(p_skeleton);
	}
}

RendererVirtualMeshStorage::SkinInstanceInfo RendererVirtualMeshStorage::get_skin_instance_info(uint32_t p_record) const {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return SkinInstanceInfo();
	}
	return skin_instances[p_record];
}

RID RendererVirtualMeshStorage::instance_record_get_skeleton(uint32_t p_record) const {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return RID();
	}
	return slot_states[p_record].skeleton;
}

const float *RendererVirtualMeshStorage::get_skeleton_palette_data(RID p_skeleton, bool p_previous, uint32_t &r_bone_count) const {
	const SkeletonPaletteSlot *sk = skeleton_palettes.getptr(p_skeleton);
	if (sk == nullptr || sk->bone_count == 0u) {
		r_bone_count = 0u;
		return nullptr;
	}
	const LocalVector<float> &vec = p_previous ? sk->prev_data : sk->cur_data;
	if (vec.size() < sk->bone_count * 12u) {
		r_bone_count = 0u;
		return nullptr;
	}
	r_bone_count = sk->bone_count;
	return vec.ptr();
}

void RendererVirtualMeshStorage::begin_frame() {
	// Harvest the completed "NextGeo Page Upload" GPU timestamp delta before RD resets timestamps.
	if (RSG::utilities != nullptr && RSG::virtual_mesh_storage == this) {
		const uint32_t ts_count = RSG::utilities->get_captured_timestamps_count();
		bool found_ts = false;
		for (uint32_t i = 0; i + 1 < ts_count; i++) {
			if (RSG::utilities->get_captured_timestamp_name(i) == "NextGeo Page Upload") {
				const uint64_t t0_gpu = RSG::utilities->get_captured_timestamp_gpu_time(i);
				const uint64_t t1_gpu = RSG::utilities->get_captured_timestamp_gpu_time(i + 1);
				if (t1_gpu >= t0_gpu && t1_gpu > 0) {
					last_page_upload_ms = double(t1_gpu - t0_gpu) / 1000000.0;
				} else {
					const uint64_t t0_cpu = RSG::utilities->get_captured_timestamp_cpu_time(i);
					const uint64_t t1_cpu = RSG::utilities->get_captured_timestamp_cpu_time(i + 1);
					last_page_upload_ms = t1_cpu >= t0_cpu ? double(t1_cpu - t0_cpu) / 1000.0 : last_cpu_page_upload_ms;
				}
				found_ts = true;
				break;
			}
		}
		if (!found_ts) {
			last_page_upload_ms = last_cpu_page_upload_ms;
		}
	} else {
		last_page_upload_ms = last_cpu_page_upload_ms;
	}

	frame++;
	free_now = free_next;
	free_next.clear();
	for (uint32_t slot : spline_free_next) {
		spline_free_now.push_back(slot);
	}
	spline_free_next.clear();

	// GodotNext: nextgeo_skinned_runtime (#160). Advance skeleton palette frame tracking once per frame.
	for (KeyValue<RID, SkeletonPaletteSlot> &entry : skeleton_palettes) {
		if (entry.value.dirty_this_frame) {
			entry.value.dirty_last_frame = true;
			entry.value.dirty_this_frame = false;
		}
	}

	for (const UploadRun &r : range_free_next) {
		range_free_now.push_back(r);
	}
	range_free_next.clear();
	if (range_free_now.size() > 1u) {
		for (uint32_t i = 0; i < range_free_now.size(); i++) {
			for (uint32_t j = i + 1; j < range_free_now.size(); j++) {
				if (range_free_now[j].first < range_free_now[i].first) {
					SWAP(range_free_now[i], range_free_now[j]);
				}
			}
		}
		uint32_t write_idx = 0u;
		for (uint32_t i = 0; i < range_free_now.size(); i++) {
			if (write_idx > 0u && range_free_now[write_idx - 1u].first + range_free_now[write_idx - 1u].count == range_free_now[i].first) {
				range_free_now[write_idx - 1u].count += range_free_now[i].count;
			} else {
				range_free_now[write_idx++] = range_free_now[i];
			}
		}
		range_free_now.resize(write_idx);
	}
}

void RendererVirtualMeshStorage::process_streaming() {
	if (streamed_frame != frame) {
		_process_streaming_frame();
		streamed_frame = frame;
		return;
	}
	if (residency_layout_dirty) {
		_rebuild_residency_gpu_table();
	} else if (residency_data_dirty) {
		for (const uint32_t &mesh_idx : residency_changed_meshes) {
			if (mesh_idx < mesh_order.size()) {
				const MeshInfo *info = meshes.getptr(mesh_order[mesh_idx]);
				if (info != nullptr) {
					_sync_residency_gpu_mesh(*info);
				}
			}
		}
		residency_data_dirty = false;
	}
	if (!residency_changed_meshes.is_empty()) {
		bool added = false;
		for (uint32_t slot = 0; slot < used_slots; slot++) {
			if (slot_states[slot].allocated && !slot_states[slot].gpu_managed && residency_changed_meshes.has(records[slot].virtual_mesh)) {
				if (!residency_changed_records.has(slot)) {
					residency_changed_records.push_back(slot);
					added = true;
				}
			}
		}
		residency_changed_meshes.clear();
		if (added) {
			residency_event_epoch++;
		}
	}
}

void RendererVirtualMeshStorage::flush() {
	process_streaming();

	// GodotNext: nextgeo_foliage_instancing (#151). Compute required GPU tail capacity for active Nanite Assemblies.
	uint32_t wanted_assembly_leaves = 0u;
	if (are_assemblies_enabled()) {
		for (uint32_t slot = 0; slot < used_slots; slot++) {
			const SlotState &state = slot_states[slot];
			if (!state.allocated || state.gpu_managed) {
				continue;
			}
			const Record &rec = records[slot];
			if ((rec.flags & (RECORD_VALID | RECORD_VISIBLE | RECORD_HAS_ASSEMBLY)) != (RECORD_VALID | RECORD_VISIBLE | RECORD_HAS_ASSEMBLY)) {
				continue;
			}
			const MeshInfo *info = get_mesh_info(rec.virtual_mesh);
			if (info == nullptr || !info->has_assembly || info->assembly_leaf_count == 0u) {
				continue;
			}
			if ((rec.flags & RECORD_MULTIMESH_PARENT) != 0u) {
				const uint32_t *idx_ptr = multimesh_parent_to_expand.getptr(slot);
				if (idx_ptr != nullptr && *idx_ptr < multimesh_expands.size() && multimesh_expands[*idx_ptr].active) {
					wanted_assembly_leaves += multimesh_expands[*idx_ptr].range_count * info->assembly_leaf_count;
				}
			} else {
				wanted_assembly_leaves += info->assembly_leaf_count;
			}
		}
	}
	active_assembly_leaf_capacity = wanted_assembly_leaves;
	if (used_slots + active_assembly_leaf_capacity > record_capacity) {
		_ensure_capacity(used_slots + active_assembly_leaf_capacity);
	}

	// A record that changed last frame and not this one needs one rewrite so
	// prev holds last frame's cur, not the frame before the last change.
	for (uint32_t slot = 0; slot < used_slots; slot++) {
		SlotState &state = slot_states[slot];
		if (state.gpu_managed) {
			continue;
		}
		if (state.dirty_last && !state.dirty && state.allocated) {
			Record &record = records[slot];
			for (int row = 0; row < 3; row++) {
				for (int column = 0; column < 4; column++) {
					record.prev_transform[row][column] = record.transform[row][column];
				}
			}
			// GodotNext: nextgeo_masked_two_sided (#153) & nextgeo_displacement (#150). Preserve
			// prev_world_aabb_min[3] (programmable_raster_distance) and prev_world_aabb_max[3] (displacement_bound).
			for (int index = 0; index < 3; index++) {
				record.prev_world_aabb_min[index] = record.world_aabb_min[index];
			}
			for (int index = 0; index < 3; index++) {
				record.prev_world_aabb_max[index] = record.world_aabb_max[index];
			}
			state.dirty = true;
			state.rewritten = true;
			if ((record.flags & RECORD_MULTIMESH_PARENT) != 0u) {
				multimesh_mark_parent_dirty(slot);
			}
		}
	}

	const bool gpu_reallocated = _ensure_record_gpu_capacity(record_capacity);
	if (gpu_reallocated) {
		pending_zero_ranges.clear();
		for (uint32_t i = 0; i < multimesh_expands.size(); i++) {
			if (multimesh_expands[i].active) {
				multimesh_expands[i].dirty = true;
			}
		}
	} else {
		for (const UploadRun &r : pending_zero_ranges) {
			_clear_record_range(r.first, r.count);
		}
		pending_zero_ranges.clear();

		uint32_t run_first = UINT32_MAX;
		uint32_t run_count = 0;
		for (uint32_t slot = 0; slot < used_slots; slot++) {
			SlotState &state = slot_states[slot];
			if (state.dirty && !state.gpu_managed) {
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
	}
	for (uint32_t slot = 0; slot < used_slots; slot++) {
		SlotState &state = slot_states[slot];
		if (state.gpu_managed) {
			state.dirty = false;
			state.dirty_last = false;
			state.rewritten = false;
			continue;
		}
		state.dirty_last = state.dirty && !state.rewritten;
		state.dirty = false;
		state.rewritten = false;
	}

	if (bucket_surface_masks_dirty && !bucket_surface_masks.is_empty()) {
		_upload_bucket_surface_masks(bucket_surface_masks.ptr(), bucket_surface_masks.size());
		bucket_surface_masks_dirty = false;
	}

	if (assemblies_gpu_dirty) {
		_rebuild_assemblies_gpu();
		assemblies_gpu_dirty = false;
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

	// GodotNext: nextgeo_foliage_wind (#152). Upload per-mesh wind bone skeletons & per-scenario wind params.
	if (wind_meshes_dirty) {
		uint32_t first = UINT32_MAX;
		uint32_t count = 0;
		for (uint32_t index = 0; index < wind_mesh_dirty.size(); index++) {
			if (wind_mesh_dirty[index]) {
				if (first == UINT32_MAX) {
					first = index;
				}
				count++;
				wind_mesh_dirty[index] = 0;
			} else if (first != UINT32_MAX) {
				_upload_wind_meshes(first, count);
				first = UINT32_MAX;
				count = 0;
			}
		}
		if (first != UINT32_MAX) {
			_upload_wind_meshes(first, count);
		}
		wind_meshes_dirty = false;
	}
	if (wind_params_dirty) {
		wind_params.mesh_count = wind_meshes.size();
		_upload_wind_params(wind_params);
		wind_params_dirty = false;
	}

	// GodotNext: spline_meshes (#161). Advance spline temporal history and upload dirty entries.
	if (!spline_entries.is_empty()) {
		const uint32_t spline_count = spline_entries.size();
		for (uint32_t s = 0; s < spline_count; s++) {
			if (spline_dirty_last[s] && !spline_dirty[s] && spline_allocated[s]) {
				spline_entries[s].previous = spline_entries[s].current;
				spline_dirty[s] = 1;
				spline_rewritten[s] = 1;
			}
		}
		uint32_t s_first = UINT32_MAX;
		uint32_t s_count = 0;
		for (uint32_t s = 0; s < spline_count; s++) {
			if (spline_dirty[s]) {
				if (s_first == UINT32_MAX) {
					s_first = s;
				}
				s_count++;
			} else if (s_first != UINT32_MAX) {
				_upload_splines(s_first, s_count);
				s_first = UINT32_MAX;
				s_count = 0;
			}
		}
		if (s_first != UINT32_MAX) {
			_upload_splines(s_first, s_count);
		}
		for (uint32_t s = 0; s < spline_count; s++) {
			spline_dirty_last[s] = spline_dirty[s] && !spline_rewritten[s];
			spline_dirty[s] = 0;
			spline_rewritten[s] = 0;
		}
	}

	// GodotNext: nextgeo_skinned_runtime (#160). Steady-state previous-frame palette sync: a skeleton that
	// animated last frame and is static this frame copies current -> previous so motion vectors return to zero.
	for (KeyValue<RID, SkeletonPaletteSlot> &entry : skeleton_palettes) {
		SkeletonPaletteSlot &sk = entry.value;
		if (sk.dirty_last_frame && !sk.dirty_this_frame && sk.bone_offset != UINT32_MAX && sk.bone_count > 0u) {
			const uint32_t half_bytes = sk.bone_count * 48u;
			memcpy(sk.prev_data.ptr(), sk.cur_data.ptr(), half_bytes);
			_upload_skin_palette((sk.bone_offset + sk.bone_count) * 48u, reinterpret_cast<const uint8_t *>(sk.prev_data.ptr()), half_bytes);
			sk.dirty_last_frame = false;
		}
	}

	// Upload the per-record skin instance side table lazily once any skinned virtual instance exists.
	if (skin_instances_active && record_capacity > 0u) {
		const uint32_t needed_bytes = record_capacity * uint32_t(sizeof(SkinInstanceInfo));
		bool uploaded_all = false;
		if (skin_instance_capacity < needed_bytes) {
			if (_resize_skin_instances(needed_bytes)) {
				skin_instance_capacity = needed_bytes;
				if (used_slots > 0u) {
					_upload_skin_instances(0u, reinterpret_cast<const uint8_t *>(skin_instances.ptr()), used_slots * uint32_t(sizeof(SkinInstanceInfo)));
				}
				for (uint32_t slot = 0; slot < used_slots; slot++) {
					slot_states[slot].skin_dirty = false;
				}
				uploaded_all = true;
			}
		}
		if (!uploaded_all && skin_instance_capacity >= needed_bytes) {
			uint32_t s_first = UINT32_MAX;
			uint32_t s_count = 0u;
			for (uint32_t slot = 0; slot < used_slots; slot++) {
				if (slot_states[slot].skin_dirty) {
					if (s_first == UINT32_MAX) {
						s_first = slot;
					}
					s_count++;
					slot_states[slot].skin_dirty = false;
				} else if (s_first != UINT32_MAX) {
					_upload_skin_instances(s_first * uint32_t(sizeof(SkinInstanceInfo)), reinterpret_cast<const uint8_t *>(skin_instances.ptr() + s_first), s_count * uint32_t(sizeof(SkinInstanceInfo)));
					s_first = UINT32_MAX;
					s_count = 0u;
				}
			}
			if (s_first != UINT32_MAX) {
				_upload_skin_instances(s_first * uint32_t(sizeof(SkinInstanceInfo)), reinterpret_cast<const uint8_t *>(skin_instances.ptr() + s_first), s_count * uint32_t(sizeof(SkinInstanceInfo)));
			}
		}
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

// ============================================================================
// Residency & Streaming (#78)
// ============================================================================

void RendererVirtualMeshStorage::set_page_arena_bytes_override(uint64_t p_bytes) {
	page_arena_override_bytes = p_bytes;
	if (!meshes.is_empty()) {
		_ensure_page_arena();
	}
}

void RendererVirtualMeshStorage::set_max_storage_buffer_range_override(uint64_t p_bytes) {
	max_storage_buffer_range_override = p_bytes;
	if (!meshes.is_empty()) {
		_ensure_page_arena();
	}
}

uint64_t RendererVirtualMeshStorage::get_effective_max_storage_buffer_range() const {
	if (max_storage_buffer_range_override > 0) {
		return max_storage_buffer_range_override;
	}
	return _get_max_storage_buffer_range();
}

RendererVirtualMeshStorage::PageArenaLayout RendererVirtualMeshStorage::compute_page_arena_layout(uint64_t p_desired_bytes, uint32_t p_slot_page_size, uint32_t p_required_pinned_slots, uint64_t p_max_storage_buffer_range) {
	PageArenaLayout layout;
	const uint64_t slot_sz = MAX<uint64_t>(p_slot_page_size, 256u);
	constexpr uint64_t HARD_CAP = 0x7FFF0000ull;
	const uint64_t raw_cap = (p_max_storage_buffer_range > 0) ? MIN<uint64_t>(HARD_CAP, p_max_storage_buffer_range) : HARD_CAP;
	const uint64_t aligned_cap = (raw_cap / slot_sz) * slot_sz;

	const uint64_t desired_clamped_hard = MIN<uint64_t>(p_desired_bytes, HARD_CAP);
	const uint64_t desired_slots = MAX<uint64_t>(desired_clamped_hard / slot_sz, 1ull);
	const uint64_t requested_aligned = desired_slots * slot_sz;
	layout.requested_bytes = requested_aligned;
	layout.effective_limit_bytes = raw_cap;

	const uint64_t min_bytes = uint64_t(MAX(p_required_pinned_slots, 1u)) * slot_sz;
	if (aligned_cap == 0 || min_bytes > aligned_cap) {
		layout.clamped_to_device_limit = (p_max_storage_buffer_range > 0 && requested_aligned > aligned_cap);
		layout.valid = false;
		return layout;
	}

	layout.clamped_to_device_limit = (p_max_storage_buffer_range > 0 && requested_aligned > aligned_cap);
	const uint64_t capped_desired = MIN<uint64_t>(requested_aligned, aligned_cap);
	const uint64_t target_bytes = MAX<uint64_t>(capped_desired, min_bytes);
	const uint64_t slots = target_bytes / slot_sz;
	const uint64_t actual_bytes = slots * slot_sz;
	if (slots == 0 || actual_bytes > raw_cap || actual_bytes > UINT32_MAX) {
		layout.valid = false;
		return layout;
	}

	layout.slot_count = uint32_t(slots);
	layout.arena_bytes = uint32_t(actual_bytes);
	layout.valid = true;
	return layout;
}

RendererVirtualMeshStorage::PageArenaLayout RendererVirtualMeshStorage::get_page_arena_layout() const {
	if (last_page_arena_layout.valid) {
		return last_page_arena_layout;
	}
	return compute_page_arena_layout(_desired_page_arena_bytes(), slot_page_size, 1u, get_effective_max_storage_buffer_range());
}

void RendererVirtualMeshStorage::set_page_upload_bytes_per_frame_override(uint64_t p_bytes) {
	page_upload_override_bytes = p_bytes;
}

void RendererVirtualMeshStorage::set_io_delay_seed(uint64_t p_seed) {
	wait_for_streaming_tasks();
	io_delay_seed = p_seed;
	next_io_request_index = 0;
	clear_recorded_io_delays();
}

Vector2 RendererVirtualMeshStorage::parse_debug_io_delay_setting(const Variant &p_value) {
	switch (p_value.get_type()) {
		case Variant::VECTOR2: {
			const Vector2 v = p_value;
			const float lo = MAX(0.0f, MIN(v.x, v.y));
			const float hi = MAX(0.0f, MAX(v.x, v.y));
			return Vector2(lo, hi);
		}
		case Variant::VECTOR2I: {
			const Vector2i v = p_value;
			const float lo = MAX(0.0f, float(MIN(v.x, v.y)));
			const float hi = MAX(0.0f, float(MAX(v.x, v.y)));
			return Vector2(lo, hi);
		}
		case Variant::INT:
		case Variant::FLOAT: {
			const float m = MAX(0.0f, float(p_value));
			return Vector2(m, m);
		}
		default:
			return Vector2();
	}
}

uint64_t RendererVirtualMeshStorage::compute_debug_io_delay_usec(uint64_t p_seed, uint64_t p_request_index, Vector2 p_delay_ms) {
	const float min_ms = MAX(0.0f, MIN(p_delay_ms.x, p_delay_ms.y));
	const float max_ms = MAX(0.0f, MAX(p_delay_ms.x, p_delay_ms.y));
	if (max_ms <= 0.0f) {
		return 0;
	}
	const uint64_t min_usec = uint64_t(Math::round(double(min_ms) * 1000.0));
	const uint64_t max_usec = uint64_t(Math::round(double(max_ms) * 1000.0));
	if (max_usec <= min_usec) {
		return min_usec;
	}
	uint64_t z = p_seed + (p_request_index + 1ULL) * 0x9E3779B97F4A7C15ULL;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	z = z ^ (z >> 31);
	return min_usec + (z % (max_usec - min_usec + 1ULL));
}

Vector<uint64_t> RendererVirtualMeshStorage::get_recorded_io_delays_usec() const {
	MutexLock lock(delay_history_mutex);
	Vector<uint64_t> copy;
	copy.resize(int(recorded_io_delays_usec.size()));
	for (uint32_t i = 0; i < recorded_io_delays_usec.size(); i++) {
		copy.write[int(i)] = recorded_io_delays_usec[i];
	}
	return copy;
}

void RendererVirtualMeshStorage::clear_recorded_io_delays() {
	MutexLock lock(delay_history_mutex);
	recorded_io_delays_usec.clear();
}

uint64_t RendererVirtualMeshStorage::_desired_page_arena_bytes() const {
	uint64_t bytes = page_arena_override_bytes;
	if (bytes == 0) {
		int64_t mb = DEFAULT_GEOMETRY_CACHE_MB;
		if (ProjectSettings::get_singleton() != nullptr && ProjectSettings::get_singleton()->has_setting("rendering/limits/nextgeo/geometry_cache_mb")) {
			mb = int64_t(ProjectSettings::get_singleton()->get_setting("rendering/limits/nextgeo/geometry_cache_mb"));
		}
		mb = CLAMP<int64_t>(mb, 1, 2047);
		bytes = uint64_t(mb) * 1024ull * 1024ull;
	}
	const uint64_t slot_sz = MAX<uint64_t>(slot_page_size, 256u);
	bytes = MIN<uint64_t>(bytes, 0x7FFF0000ull);
	const uint64_t slots = MAX<uint64_t>(bytes / slot_sz, 1ull);
	return slots * slot_sz;
}

uint64_t RendererVirtualMeshStorage::_desired_page_upload_budget_bytes() const {
	if (page_upload_override_bytes > 0) {
		return page_upload_override_bytes;
	}
	int64_t kib = DEFAULT_PAGE_UPLOAD_KIB_PER_FRAME;
	if (ProjectSettings::get_singleton() != nullptr && ProjectSettings::get_singleton()->has_setting("rendering/limits/nextgeo/page_upload_kib_per_frame")) {
		kib = int64_t(ProjectSettings::get_singleton()->get_setting("rendering/limits/nextgeo/page_upload_kib_per_frame"));
	}
	kib = MAX<int64_t>(kib, 64);
	return uint64_t(kib) * 1024ull;
}

void RendererVirtualMeshStorage::_lru_unlink(uint32_t p_slot) {
	if (p_slot >= page_slots.size() || !page_slots[p_slot].in_lru) {
		return;
	}
	PageSlot &ps = page_slots[p_slot];
	if (ps.lru_prev != UINT32_MAX) {
		page_slots[ps.lru_prev].lru_next = ps.lru_next;
	} else {
		lru_head = ps.lru_next;
	}
	if (ps.lru_next != UINT32_MAX) {
		page_slots[ps.lru_next].lru_prev = ps.lru_prev;
	} else {
		lru_tail = ps.lru_prev;
	}
	ps.lru_prev = UINT32_MAX;
	ps.lru_next = UINT32_MAX;
	ps.in_lru = false;
}

void RendererVirtualMeshStorage::_lru_push_back(uint32_t p_slot) {
	if (p_slot >= page_slots.size()) {
		return;
	}
	PageSlot &ps = page_slots[p_slot];
	_lru_unlink(p_slot);
	ps.lru_prev = lru_tail;
	ps.lru_next = UINT32_MAX;
	if (lru_tail != UINT32_MAX) {
		page_slots[lru_tail].lru_next = p_slot;
	} else {
		lru_head = p_slot;
	}
	lru_tail = p_slot;
	ps.in_lru = true;
}

void RendererVirtualMeshStorage::_touch_page_slot(uint32_t p_slot) {
	if (p_slot >= page_slots.size() || !page_slots[p_slot].occupied) {
		return;
	}
	page_slots[p_slot].last_used_frame = frame;
	if (!page_slots[p_slot].pinned) {
		_lru_push_back(p_slot);
	}
}

void RendererVirtualMeshStorage::_evict_all_non_pinned_leaf_first() {
	bool progressed = true;
	while (progressed) {
		progressed = false;
		uint32_t cur = lru_head;
		while (cur != UINT32_MAX) {
			const uint32_t next = page_slots[cur].lru_next;
			const PageSlot &ps = page_slots[cur];
			if (!ps.pinned && ps.occupied) {
				const MeshInfo *info = meshes.getptr(ps.mesh);
				const bool has_child = info != nullptr && ps.page_id < info->resident_child_count.size() && info->resident_child_count[ps.page_id] > 0;
				if (!has_child && _evict_slot(cur, true)) {
					progressed = true;
				}
			}
			cur = next;
		}
	}
}

bool RendererVirtualMeshStorage::_ensure_page_arena() {
	const uint64_t desired_bytes = _desired_page_arena_bytes();
	const uint64_t max_storage_range = get_effective_max_storage_buffer_range();
	uint32_t required_pinned_slots = 0;
	for (const KeyValue<RID, MeshInfo> &E : meshes) {
		for (uint32_t p = 0; p < E.value.root_page_count; p++) {
			uint32_t c_count = 0;
			if (int(p) < E.value.desc.root_pages.size() && _is_valid_raw_page(E.value.desc.root_pages[int(p)], c_count)) {
				required_pinned_slots++;
			}
		}
	}

	const PageArenaLayout layout = compute_page_arena_layout(desired_bytes, slot_page_size, required_pinned_slots, max_storage_range);
	if (!layout.valid) {
		ERR_PRINT(vformat("NextGeo: page arena needs at least %d pinned slots (%d B), which exceeds device maxStorageBufferRange (%d B).",
				MAX(required_pinned_slots, 1u), uint64_t(MAX(required_pinned_slots, 1u)) * uint64_t(slot_page_size), max_storage_range));
		return false;
	}
	if (page_capacity == layout.arena_bytes && !page_slots.is_empty() && uint32_t(page_slots.size()) == layout.slot_count && uint64_t(page_slots.size()) * uint64_t(slot_page_size) == layout.arena_bytes) {
		last_page_arena_layout = layout;
		return true;
	}

	// Evict non-pinned streaming pages leaf-first before resizing the backing buffer.
	_evict_all_non_pinned_leaf_first();

	const uint32_t total_slots = layout.slot_count;
	const uint32_t actual_bytes = layout.arena_bytes;
	if (!_resize_pages(actual_bytes)) {
		return false;
	}
	last_page_arena_layout = layout;

	page_capacity = actual_bytes;
	page_slots.clear();
	page_slots.resize(total_slots);
	free_page_slots.clear();
	free_page_slots.resize(total_slots);
	for (uint32_t i = 0; i < total_slots; i++) {
		// Reverse order so popping from the back allocates slot 0 first.
		free_page_slots[i] = total_slots - 1u - i;
	}
	lru_head = UINT32_MAX;
	lru_tail = UINT32_MAX;
	occupied_slot_count = 0;
	pinned_slot_count = 0;

	// Re-pin and re-upload root pages of already-registered meshes.
	for (KeyValue<RID, MeshInfo> &E : meshes) {
		MeshInfo &info = E.value;
		for (uint32_t p = 0; p < info.page_to_slot.size(); p++) {
			info.page_to_slot[p] = INVALID_PAGE_SLOT;
			info.resident_child_count[p] = 0;
		}
		for (uint32_t w = 0; w < info.resident_bitset.size(); w++) {
			info.resident_bitset[w] = 0;
		}
		if (!info.cluster_blob.is_empty()) {
			memset(info.cluster_blob.ptrw(), 0, info.cluster_blob.size());
		}
		for (uint32_t c = 0; c < info.cluster_refine.size(); c++) {
			info.cluster_refine[c] = ClusterRefineInfo();
		}
		for (uint32_t c = 0; c < info.cluster_skin_offsets.size(); c++) {
			info.cluster_skin_offsets[c] = UINT32_MAX;
		}
		for (uint32_t c = 0; c < info.cluster_skin_bones.size(); c++) {
			info.cluster_skin_bones[c].clear();
		}
		for (uint32_t p = 0; p < info.root_page_count; p++) {
			uint32_t c_count = 0;
			if (int(p) < info.desc.root_pages.size() && _is_valid_raw_page(info.desc.root_pages[int(p)], c_count)) {
				const uint32_t slot = _allocate_page_slot(true);
				if (slot != INVALID_PAGE_SLOT) {
					_install_page_in_slot(info, p, slot, info.desc.root_pages[int(p)], true, false);
				}
			}
		}
		if (info.cluster_table_size > 0 && !info.cluster_blob.is_empty()) {
			_upload_clusters(info.cluster_table_offset, info.cluster_blob.ptr(), info.cluster_table_size);
			if (!info.cluster_skin_offsets.is_empty() && _ensure_cluster_skin_buffer()) {
				const uint32_t dst_off = info.cluster_table_offset / 4u;
				const uint32_t upload_bytes = uint32_t(info.cluster_skin_offsets.size()) * sizeof(uint32_t);
				if (dst_off + upload_bytes <= cluster_skin_capacity) {
					_upload_cluster_skin(dst_off, reinterpret_cast<const uint8_t *>(info.cluster_skin_offsets.ptr()), upload_bytes);
				}
			}
		}
		_recompute_mesh_refine_ok(info);
	}
	residency_layout_dirty = true;
	return true;
}

uint32_t RendererVirtualMeshStorage::_allocate_page_slot(bool p_pinned) {
	if (!free_page_slots.is_empty()) {
		const uint32_t slot = free_page_slots[free_page_slots.size() - 1];
		free_page_slots.remove_at(free_page_slots.size() - 1);
		return slot;
	}

	// Walk LRU oldest-first for a leaf page (no resident child).
	for (int pass = 0; pass < (p_pinned ? 2 : 1); pass++) {
		uint32_t cur = lru_head;
		while (cur != UINT32_MAX) {
			const uint32_t next = page_slots[cur].lru_next;
			const PageSlot &ps = page_slots[cur];
			if (!ps.pinned && ps.occupied) {
				const bool unused_this_frame = ps.last_used_frame < frame;
				if (pass == 1 || unused_this_frame) {
					const MeshInfo *info = meshes.getptr(ps.mesh);
					const bool has_resident_child = info != nullptr && ps.page_id < info->resident_child_count.size() && info->resident_child_count[ps.page_id] > 0;
					if (!has_resident_child && _evict_slot(cur, true) && !free_page_slots.is_empty()) {
						const uint32_t slot = free_page_slots[free_page_slots.size() - 1];
						free_page_slots.remove_at(free_page_slots.size() - 1);
						return slot;
					}
				}
			}
			cur = next;
		}
	}

	if (p_pinned) {
		// Pinned root pages are never dropped: grow the arena if every slot is already pinned,
		// staying within the device's maxStorageBufferRange (#306).
		const uint64_t max_storage_range = get_effective_max_storage_buffer_range();
		const uint64_t hard_cap = (max_storage_range > 0) ? MIN<uint64_t>(0x7FFF0000ull, max_storage_range) : 0x7FFF0000ull;
		const uint64_t grown = uint64_t(page_slots.size() + 16u) * uint64_t(slot_page_size);
		if (grown <= hard_cap) {
			const uint64_t saved_override = page_arena_override_bytes;
			page_arena_override_bytes = grown;
			const bool ok = _ensure_page_arena();
			page_arena_override_bytes = saved_override;
			if (ok && !free_page_slots.is_empty()) {
				const uint32_t slot = free_page_slots[free_page_slots.size() - 1];
				free_page_slots.remove_at(free_page_slots.size() - 1);
				return slot;
			}
		}
	}
	return INVALID_PAGE_SLOT;
}

void RendererVirtualMeshStorage::_free_mesh_page_slots(MeshInfo &r_info) {
	for (uint32_t p = 0; p < r_info.page_to_slot.size(); p++) {
		const uint32_t slot = r_info.page_to_slot[p];
		if (slot == INVALID_PAGE_SLOT || slot >= page_slots.size()) {
			continue;
		}
		PageSlot &ps = page_slots[slot];
		if (ps.in_lru) {
			_lru_unlink(slot);
		}
		if (ps.occupied) {
			if (ps.pinned && pinned_slot_count > 0) {
				pinned_slot_count--;
			}
			if (occupied_slot_count > 0) {
				occupied_slot_count--;
			}
		}
		ps = PageSlot();
		free_page_slots.push_back(slot);
		r_info.page_to_slot[p] = INVALID_PAGE_SLOT;
	}
}

bool RendererVirtualMeshStorage::_evict_slot(uint32_t p_slot, bool p_record_event) {
	if (p_slot >= page_slots.size()) {
		return false;
	}
	PageSlot &ps = page_slots[p_slot];
	if (!ps.occupied || ps.pinned) {
		return false;
	}
	MeshInfo *info = meshes.getptr(ps.mesh);
	const uint32_t page_id = ps.page_id;
	if (info != nullptr && page_id < info->total_page_count) {
		if (page_id < info->resident_child_count.size() && info->resident_child_count[page_id] > 0) {
			evictions_with_resident_child++;
			return false;
		}
		info->page_to_slot[page_id] = INVALID_PAGE_SLOT;
		info->resident_bitset[page_id >> 5u] &= ~(1u << (page_id & 31u));
		if (page_id < info->page_parents.size()) {
			for (uint32_t parent_page : info->page_parents[page_id]) {
				if (parent_page < info->resident_child_count.size() && info->resident_child_count[parent_page] > 0) {
					info->resident_child_count[parent_page]--;
				}
			}
		}

		if (page_id < info->page_first_cluster.size() && info->page_first_cluster[page_id] != UINT32_MAX) {
			const uint32_t base_cluster = info->page_first_cluster[page_id];
			const uint32_t count = info->page_cluster_counts[page_id];
			if (count > 0 && uint64_t(base_cluster + count) * sizeof(ClusterGPUDesc) <= uint64_t(info->cluster_blob.size())) {
				memset(info->cluster_blob.ptrw() + base_cluster * sizeof(ClusterGPUDesc), 0, count * sizeof(ClusterGPUDesc));
				for (uint32_t i = 0; i < count && (base_cluster + i) < info->cluster_refine.size(); i++) {
					info->cluster_refine[base_cluster + i] = ClusterRefineInfo();
				}
				for (uint32_t i = 0; i < count && (base_cluster + i) < info->cluster_skin_offsets.size(); i++) {
					info->cluster_skin_offsets[base_cluster + i] = UINT32_MAX;
				}
				for (uint32_t i = 0; i < count && (base_cluster + i) < info->cluster_skin_bones.size(); i++) {
					info->cluster_skin_bones[base_cluster + i].clear();
				}
				if (p_record_event && info->cluster_table_size > 0) {
					_upload_clusters(
							info->cluster_table_offset + base_cluster * sizeof(ClusterGPUDesc),
							info->cluster_blob.ptr() + base_cluster * sizeof(ClusterGPUDesc),
							count * sizeof(ClusterGPUDesc));
					if (cluster_skin_active && !info->cluster_skin_offsets.is_empty() && (base_cluster + count) <= info->cluster_skin_offsets.size()) {
						const uint32_t dst_off = info->cluster_table_offset / 4u + base_cluster * sizeof(uint32_t);
						const uint32_t upload_bytes = count * sizeof(uint32_t);
						if (dst_off + upload_bytes <= cluster_skin_capacity) {
							_upload_cluster_skin(dst_off, reinterpret_cast<const uint8_t *>(info->cluster_skin_offsets.ptr() + base_cluster), upload_bytes);
						}
					}
				}
			}
		}

		if (p_record_event) {
			_recompute_mesh_refine_ok(*info);
			residency_data_dirty = true;
			residency_changed_meshes.insert(info->mesh_index);
			total_page_evictions++;
		}
	}

	_lru_unlink(p_slot);
	ps = PageSlot();
	if (occupied_slot_count > 0) {
		occupied_slot_count--;
	}
	free_page_slots.push_back(p_slot);
	return true;
}

bool RendererVirtualMeshStorage::_install_page_in_slot(MeshInfo &r_info, uint32_t p_page_id, uint32_t p_slot, const Vector<uint8_t> &p_raw, bool p_pinned, bool p_record_event) {
	if (p_page_id >= r_info.total_page_count || p_slot >= page_slots.size() || p_raw.size() < 16 || uint32_t(p_raw.size()) > slot_page_size) {
		return false;
	}
	uint32_t header[4] = { 0, 0, 0, 0 };
	memcpy(header, p_raw.ptr(), sizeof(header));
	const uint32_t page_clusters = header[0];
	const uint32_t page_vertices = header[1];
	const uint32_t page_triangles = header[2];
	const uint32_t skin_bytes = header[3];
	const uint64_t fixed_bytes = 16ull + uint64_t(page_clusters) * 80ull + uint64_t(page_triangles) * 3ull + uint64_t(skin_bytes);
	if (fixed_bytes > uint64_t(p_raw.size())) {
		return false;
	}
	const uint64_t vertex_bytes = uint64_t(p_raw.size()) - fixed_bytes;
	uint32_t stride = r_info.layout_stride;
	if (stride == 0) {
		if (page_vertices > 0 && (vertex_bytes % (uint64_t(page_vertices) * 4ull)) == 0) {
			stride = uint32_t(vertex_bytes / (uint64_t(page_vertices) * 4ull));
		} else {
			stride = 3u;
		}
	}
	if (uint64_t(page_vertices) * uint64_t(stride) * 4ull != vertex_bytes) {
		return false;
	}
	const uint32_t vertex_layout = r_info.vertex_layout != 0 ? r_info.vertex_layout : _infer_vertex_layout_from_stride(stride);

	// Verify parents-first invariant on non-root uploads.
	if (!p_pinned && p_page_id >= r_info.root_page_count && p_page_id < r_info.page_parents.size()) {
		for (uint32_t parent_page : r_info.page_parents[p_page_id]) {
			if (parent_page < r_info.total_page_count) {
				const bool parent_res = (r_info.resident_bitset[parent_page >> 5u] & (1u << (parent_page & 31u))) != 0u;
				if (!parent_res) {
					child_resident_without_parent++;
				}
			}
		}
	}

	const uint32_t page_global = p_slot * slot_page_size;
	// GodotNext: nextgeo_sw_raster (#162). A v1 page gets each cluster's max_edge, which .ngm v2 stores,
	// computed from its own vertices, so the triangle-size switch reads the same record field.
	Vector<uint8_t> patched_page;
	if (!ngm_page_has_max_edges(p_raw.ptr(), uint64_t(p_raw.size()))) {
		patched_page = p_raw;
		if (!ngm_page_fill_max_edges(patched_page.ptrw(), uint64_t(patched_page.size()), stride)) {
			patched_page.clear();
		}
	}
	const Vector<uint8_t> &uploaded_page = patched_page.is_empty() ? p_raw : patched_page;
	_upload_pages(page_global, uploaded_page.ptr(), uint32_t(uploaded_page.size()));

	const uint32_t page_vert_bytes = page_global + 16u + page_clusters * 80u;
	const uint32_t page_tri_bytes = page_vert_bytes + page_vertices * stride * 4u;
	const uint32_t base_cluster = (p_page_id < r_info.page_first_cluster.size() && r_info.page_first_cluster[p_page_id] != UINT32_MAX) ? r_info.page_first_cluster[p_page_id] : 0u;
	if (p_page_id < r_info.page_cluster_counts.size()) {
		r_info.page_cluster_counts[p_page_id] = MAX(r_info.page_cluster_counts[p_page_id], page_clusters);
	}

	const uint32_t needed_clusters = base_cluster + page_clusters;
	if (uint64_t(needed_clusters) * sizeof(ClusterGPUDesc) <= uint64_t(r_info.cluster_blob.size())) {
		ClusterGPUDesc *descs = reinterpret_cast<ClusterGPUDesc *>(r_info.cluster_blob.ptrw());
		for (uint32_t i = 0; i < page_clusters; i++) {
			const uint8_t *rec = p_raw.ptr() + 16u + i * 80u;
			uint32_t vertex_offset = 0;
			uint32_t triangle_offset = 0;
			uint16_t surface_id = 0;
			memcpy(&vertex_offset, rec + 68, sizeof(uint32_t));
			memcpy(&triangle_offset, rec + 72, sizeof(uint32_t));
			const uint8_t vertex_count = rec[76];
			const uint8_t triangle_count = rec[77];
			memcpy(&surface_id, rec + 78, sizeof(uint16_t));

			ClusterGPUDesc desc;
			desc.pos_word_offset = (page_vert_bytes >> 2u) + vertex_offset * stride;
			desc.tri_byte_offset = page_tri_bytes + triangle_offset * 3u;
			desc.stride_and_counts = (stride & 0xFFu) | (uint32_t(vertex_count) << 8u) | (uint32_t(triangle_count) << 16u) | ((vertex_layout & 0xFFu) << 24u);
			desc.surface_and_page = uint32_t(surface_id) | (p_page_id << 16u);
			descs[base_cluster + i] = desc;

			if (base_cluster + i < r_info.cluster_refine.size()) {
				ClusterRefineInfo ref_info;
				memcpy(ref_info.group_lod, rec + 20, sizeof(float) * 4);
				memcpy(&ref_info.group_error, rec + 36, sizeof(float));
				memcpy(&ref_info.refined_group, rec + 60, sizeof(int32_t));
				ref_info.valid = true;
				r_info.cluster_refine[base_cluster + i] = ref_info;
			}
		}
		// GodotNext: nextgeo_skinned_runtime (#160). Index per-cluster #159 skin blocks inside the page arena.
		if (skin_bytes > 0u && needed_clusters <= r_info.cluster_skin_offsets.size()) {
			uint32_t skin_cursor = 16u + page_clusters * 80u + page_vertices * stride * 4u + page_triangles * 3u;
			const uint32_t skin_end = MIN(skin_cursor + skin_bytes, uint32_t(p_raw.size()));
			for (uint32_t i = 0; i < page_clusters; i++) {
				const uint8_t *rec = p_raw.ptr() + 16u + i * 80u;
				const uint8_t vertex_count = rec[76];
				r_info.cluster_skin_offsets[base_cluster + i] = UINT32_MAX;
				if (base_cluster + i < r_info.cluster_skin_bones.size()) {
					r_info.cluster_skin_bones[base_cluster + i].clear();
				}
				if (skin_cursor + 4u > skin_end) {
					break;
				}
				const uint8_t bone_count = p_raw[int(skin_cursor + 0u)];
				const uint8_t inf = p_raw[int(skin_cursor + 1u)];
				if (inf != 4u && inf != 8u) {
					break;
				}
				const uint32_t block_bytes = 4u + uint32_t(bone_count) * 26u + uint32_t(vertex_count) * uint32_t(inf) * 3u;
				if (skin_cursor + block_bytes > skin_end) {
					break;
				}
				r_info.cluster_skin_offsets[base_cluster + i] = page_global + skin_cursor;
				if (base_cluster + i < r_info.cluster_skin_bones.size() && bone_count > 0u) {
					LocalVector<ClusterSkinBone> &bones = r_info.cluster_skin_bones[base_cluster + i];
					bones.resize(bone_count);
					for (uint32_t b = 0; b < bone_count; b++) {
						const uint8_t *be = p_raw.ptr() + skin_cursor + 4u + b * 26u;
						uint16_t bone_id = 0u;
						memcpy(&bone_id, be, sizeof(uint16_t));
						bones[b].global_bone = bone_id;
						memcpy(bones[b].aabb_min, be + 2u, sizeof(float) * 3u);
						memcpy(bones[b].aabb_max, be + 14u, sizeof(float) * 3u);
					}
				}
				skin_cursor += block_bytes;
			}
		}
		if (p_record_event && r_info.cluster_table_size > 0 && page_clusters > 0) {
			_upload_clusters(
					r_info.cluster_table_offset + base_cluster * sizeof(ClusterGPUDesc),
					r_info.cluster_blob.ptr() + base_cluster * sizeof(ClusterGPUDesc),
					page_clusters * sizeof(ClusterGPUDesc));
			if (skin_bytes > 0u && needed_clusters <= r_info.cluster_skin_offsets.size() && _ensure_cluster_skin_buffer()) {
				const uint32_t dst_off = r_info.cluster_table_offset / 4u + base_cluster * sizeof(uint32_t);
				const uint32_t upload_bytes = page_clusters * sizeof(uint32_t);
				if (dst_off + upload_bytes <= cluster_skin_capacity) {
					_upload_cluster_skin(dst_off, reinterpret_cast<const uint8_t *>(r_info.cluster_skin_offsets.ptr() + base_cluster), upload_bytes);
				}
			}
		}
	}

	PageSlot &ps = page_slots[p_slot];
	ps.mesh = r_info.mesh;
	ps.page_id = p_page_id;
	ps.last_used_frame = frame;
	ps.occupied = true;
	ps.pinned = p_pinned;
	occupied_slot_count++;
	if (p_pinned) {
		pinned_slot_count++;
	} else {
		_lru_push_back(p_slot);
	}

	r_info.page_to_slot[p_page_id] = p_slot;
	r_info.resident_bitset[p_page_id >> 5u] |= (1u << (p_page_id & 31u));
	if (p_page_id < r_info.page_parents.size()) {
		for (uint32_t parent_page : r_info.page_parents[p_page_id]) {
			if (parent_page < r_info.resident_child_count.size()) {
				r_info.resident_child_count[parent_page]++;
			}
		}
	}

	if (p_record_event) {
		_recompute_mesh_refine_ok(r_info);
		residency_data_dirty = true;
		residency_changed_meshes.insert(r_info.mesh_index);
		total_page_uploads++;
		total_page_upload_bytes += uint32_t(p_raw.size());
	}
	return true;
}

bool RendererVirtualMeshStorage::_all_parent_pages_resident(const MeshInfo &p_info, uint32_t p_page_id) const {
	if (p_page_id < p_info.root_page_count) {
		return true;
	}
	if (!p_info.page_dag_ready || p_page_id >= p_info.page_parents.size()) {
		return false;
	}
	for (uint32_t parent_page : p_info.page_parents[p_page_id]) {
		if (parent_page >= p_info.total_page_count) {
			continue;
		}
		if ((p_info.resident_bitset[parent_page >> 5u] & (1u << (parent_page & 31u))) == 0u) {
			return false;
		}
	}
	return true;
}

void RendererVirtualMeshStorage::_rebuild_page_children_and_depths(MeshInfo &r_info) {
	const uint32_t n = r_info.total_page_count;
	r_info.page_children.clear();
	r_info.page_children.resize(n);
	r_info.page_topo_depth.clear();
	r_info.page_topo_depth.resize(n);
	r_info.resident_child_count.clear();
	r_info.resident_child_count.resize(n);

	LocalVector<uint32_t> indegree;
	indegree.resize(n);
	for (uint32_t p = 0; p < n; p++) {
		r_info.page_topo_depth[p] = 0;
		r_info.resident_child_count[p] = 0;
		indegree[p] = 0;
	}

	for (uint32_t child = 0; child < MIN(n, r_info.page_parents.size()); child++) {
		for (uint32_t parent : r_info.page_parents[child]) {
			if (parent < n && parent != child) {
				if (!r_info.page_children[parent].has(child)) {
					r_info.page_children[parent].push_back(child);
					indegree[child]++;
				}
				const bool child_res = (r_info.resident_bitset[child >> 5u] & (1u << (child & 31u))) != 0u;
				if (child_res) {
					r_info.resident_child_count[parent]++;
				}
			}
		}
	}

	// Kahn BFS to assign topological depth (parents strictly < children).
	LocalVector<uint32_t> queue;
	for (uint32_t p = 0; p < n; p++) {
		if (indegree[p] == 0) {
			queue.push_back(p);
		}
	}
	uint32_t head = 0;
	while (head < queue.size()) {
		const uint32_t u = queue[head++];
		for (uint32_t v : r_info.page_children[u]) {
			r_info.page_topo_depth[v] = MAX(r_info.page_topo_depth[v], r_info.page_topo_depth[u] + 1u);
			if (indegree[v] > 0) {
				indegree[v]--;
				if (indegree[v] == 0) {
					queue.push_back(v);
				}
			}
		}
	}
}

void RendererVirtualMeshStorage::compute_refine_ok_bits(uint32_t p_group_count, const LocalVector<LocalVector<uint32_t>> &p_group_pages, uint32_t p_root_page_count, const uint8_t *p_page_resident, uint32_t p_page_count, LocalVector<uint32_t> &r_refine_ok_bits) {
	const uint32_t words = (p_group_count + 31u) / 32u;
	r_refine_ok_bits.resize(words);
	for (uint32_t w = 0; w < words; w++) {
		r_refine_ok_bits[w] = 0u;
	}
	for (uint32_t g = 0; g < p_group_count; g++) {
		bool ok = true;
		if (g < p_group_pages.size() && !p_group_pages[g].is_empty()) {
			for (uint32_t pid : p_group_pages[g]) {
				if (pid < p_root_page_count) {
					continue; // Root pages stay resident; root groups are always ok.
				}
				if (pid >= p_page_count || (p_page_resident != nullptr && p_page_resident[pid] == 0)) {
					ok = false;
					break;
				}
			}
		} else if (p_page_count > 0 && p_root_page_count == 0) {
			ok = false;
		}
		if (ok) {
			r_refine_ok_bits[g >> 5u] |= (1u << (g & 31u));
		}
	}
}

void RendererVirtualMeshStorage::_recompute_mesh_refine_ok(MeshInfo &r_info) {
	// GodotNext: nextgeo_rt_native (#156)
	r_info.residency_generation++;
	LocalVector<uint8_t> resident_bytes;
	resident_bytes.resize(r_info.total_page_count);
	for (uint32_t p = 0; p < r_info.total_page_count; p++) {
		const bool res = (r_info.resident_bitset[p >> 5u] & (1u << (p & 31u))) != 0u;
		resident_bytes[p] = res ? 1u : 0u;
	}
	compute_refine_ok_bits(
			r_info.group_count,
			r_info.group_pages,
			r_info.root_page_count,
			resident_bytes.ptr(),
			r_info.total_page_count,
			r_info.refine_ok_bitset);
}

void RendererVirtualMeshStorage::mesh_set_page_parents(RID p_mesh, const LocalVector<LocalVector<uint32_t>> &p_page_parents) {
	MeshInfo *info = meshes.getptr(p_mesh);
	ERR_FAIL_NULL(info);
	if (p_page_parents.size() > info->total_page_count) {
		info->total_page_count = p_page_parents.size();
		const uint32_t old_pages = info->page_to_slot.size();
		info->page_to_slot.resize(info->total_page_count);
		for (uint32_t p = old_pages; p < info->total_page_count; p++) {
			info->page_to_slot[p] = INVALID_PAGE_SLOT;
		}
		info->resident_bitset.resize((info->total_page_count + 31u) / 32u);
		residency_layout_dirty = true;
	}
	info->page_parents.clear();
	info->page_parents.resize(info->total_page_count);
	for (uint32_t p = 0; p < MIN(info->total_page_count, p_page_parents.size()); p++) {
		info->page_parents[p] = p_page_parents[p];
	}
	_rebuild_page_children_and_depths(*info);
	info->page_dag_ready = true;
}

void RendererVirtualMeshStorage::mesh_set_in_memory_pages(RID p_mesh, const Vector<Vector<uint8_t>> &p_pages) {
	ERR_FAIL_COND(p_mesh.is_null());
	in_memory_mesh_pages[p_mesh] = p_pages;
	MeshInfo *info = meshes.getptr(p_mesh);
	if (info == nullptr) {
		return;
	}
	if (uint32_t(p_pages.size()) > info->total_page_count) {
		const uint32_t old_pages = info->total_page_count;
		info->total_page_count = uint32_t(p_pages.size());
		info->page_to_slot.resize(info->total_page_count);
		info->page_first_cluster.resize(info->total_page_count);
		info->page_cluster_counts.resize(info->total_page_count);
		for (uint32_t p = old_pages; p < info->total_page_count; p++) {
			info->page_to_slot[p] = INVALID_PAGE_SLOT;
			info->page_first_cluster[p] = UINT32_MAX;
			info->page_cluster_counts[p] = 0;
		}
		const uint32_t old_words = info->resident_bitset.size();
		const uint32_t new_words = (info->total_page_count + 31u) / 32u;
		info->resident_bitset.resize(new_words);
		for (uint32_t w = old_words; w < new_words; w++) {
			info->resident_bitset[w] = 0;
		}
		info->page_parents.resize(info->total_page_count);
		residency_layout_dirty = true;
	}

	// If no explicit page_parents were set yet, scan cluster headers of p_pages.
	bool any_parents = false;
	for (uint32_t p = 0; p < info->page_parents.size(); p++) {
		if (!info->page_parents[p].is_empty()) {
			any_parents = true;
			break;
		}
	}
	if (!any_parents && !info->group_page_ids.is_empty()) {
		for (uint32_t p = 0; p < MIN(info->total_page_count, uint32_t(p_pages.size())); p++) {
			uint32_t c_count = 0;
			if (!_is_valid_raw_page(p_pages[int(p)], c_count)) {
				continue;
			}
			for (uint32_t i = 0; i < c_count; i++) {
				int32_t refined_group = -1;
				memcpy(&refined_group, p_pages[int(p)].ptr() + 16u + i * 80u + 60u, sizeof(int32_t));
				if (refined_group >= 0 && uint32_t(refined_group) < info->group_page_ids.size()) {
					const uint32_t child_page = info->group_page_ids[uint32_t(refined_group)];
					if (child_page != p && child_page < info->total_page_count && !info->page_parents[child_page].has(p)) {
						info->page_parents[child_page].push_back(p);
					}
				}
			}
		}
	}
	_rebuild_page_children_and_depths(*info);
	info->page_dag_ready = true;
}

bool RendererVirtualMeshStorage::mesh_is_page_resident(RID p_mesh, uint32_t p_page_id) const {
	const MeshInfo *info = meshes.getptr(p_mesh);
	if (info == nullptr || p_page_id >= info->total_page_count) {
		return false;
	}
	return (info->resident_bitset[p_page_id >> 5u] & (1u << (p_page_id & 31u))) != 0u;
}

uint32_t RendererVirtualMeshStorage::mesh_get_page_slot(RID p_mesh, uint32_t p_page_id) const {
	const MeshInfo *info = meshes.getptr(p_mesh);
	if (info == nullptr || p_page_id >= info->page_to_slot.size()) {
		return INVALID_PAGE_SLOT;
	}
	return info->page_to_slot[p_page_id];
}

bool RendererVirtualMeshStorage::mesh_is_group_refine_ok(uint32_t p_mesh_index, uint32_t p_group_index) const {
	const MeshInfo *info = get_mesh_info(p_mesh_index);
	if (info == nullptr || p_group_index >= info->group_count) {
		return false;
	}
	return (info->refine_ok_bitset[p_group_index >> 5u] & (1u << (p_group_index & 31u))) != 0u;
}

bool RendererVirtualMeshStorage::mesh_is_group_refine_ok_rid(RID p_mesh, uint32_t p_group_index) const {
	const MeshInfo *info = meshes.getptr(p_mesh);
	if (info == nullptr || p_group_index >= info->group_count) {
		return false;
	}
	return (info->refine_ok_bitset[p_group_index >> 5u] & (1u << (p_group_index & 31u))) != 0u;
}

void RendererVirtualMeshStorage::submit_page_requests(const PageRequest *p_requests, uint32_t p_count, bool p_is_main_view) {
	if (p_requests == nullptr || p_count == 0) {
		return;
	}
	if (!p_is_main_view) {
		non_main_view_requests += p_count;
		return;
	}

	auto queue_or_merge = [&](MeshInfo &r_info, uint32_t p_page_id, bool p_visible, float p_distance, float p_px_error) {
		if (p_page_id >= r_info.total_page_count) {
			return;
		}
		const uint32_t slot = r_info.page_to_slot[p_page_id];
		if (slot != INVALID_PAGE_SLOT) {
			_touch_page_slot(slot);
			return;
		}
		const QueuedPageKey key = { r_info.mesh, p_page_id };
		if (in_flight_pages.has(key)) {
			return;
		}
		for (StagedPage &staged : staged_pages) {
			if (staged.mesh == r_info.mesh && staged.page_id == p_page_id) {
				staged.visible = staged.visible || p_visible;
				staged.distance = MIN(staged.distance, p_distance);
				staged.px_error = MAX(staged.px_error, p_px_error);
				return;
			}
		}
		PrioritizedPageRequest *existing = pending_requests.getptr(key);
		if (existing != nullptr) {
			existing->visible = existing->visible || p_visible;
			existing->distance = MIN(existing->distance, p_distance);
			existing->px_error = MAX(existing->px_error, p_px_error);
		} else {
			PrioritizedPageRequest pr;
			pr.mesh = r_info.mesh;
			pr.mesh_index = r_info.mesh_index;
			pr.generation = r_info.generation;
			pr.page_id = p_page_id;
			pr.topo_depth = p_page_id < r_info.page_topo_depth.size() ? r_info.page_topo_depth[p_page_id] : 0u;
			pr.visible = p_visible;
			pr.distance = p_distance;
			pr.px_error = p_px_error;
			pending_requests.insert(key, pr);
		}
	};

	for (uint32_t i = 0; i < p_count; i++) {
		const PageRequest &req = p_requests[i];
		if (req.mesh_index >= mesh_order.size()) {
			continue;
		}
		MeshInfo *info = meshes.getptr(mesh_order[req.mesh_index]);
		if (info == nullptr || req.page_id >= info->total_page_count) {
			continue;
		}

		// Touch resident ancestors and expand any missing ancestor pages so parents load first.
		if (info->page_dag_ready) {
			LocalVector<uint32_t> stack;
			HashSet<uint32_t> visited;
			stack.push_back(req.page_id);
			visited.insert(req.page_id);
			while (!stack.is_empty()) {
				const uint32_t cur = stack[stack.size() - 1];
				stack.remove_at(stack.size() - 1);
				queue_or_merge(*info, cur, req.visible, req.distance, req.px_error);
				if (cur < info->page_parents.size()) {
					for (uint32_t parent_page : info->page_parents[cur]) {
						if (parent_page < info->total_page_count && !visited.has(parent_page)) {
							visited.insert(parent_page);
							stack.push_back(parent_page);
						}
					}
				}
			}
		} else {
			queue_or_merge(*info, req.page_id, req.visible, req.distance, req.px_error);
		}
	}
}

void RendererVirtualMeshStorage::touch_used_page_slots(const uint32_t *p_used_words, uint32_t p_word_count) {
	if (p_used_words == nullptr || p_word_count == 0) {
		return;
	}
	for (uint32_t w = 0; w < p_word_count; w++) {
		uint32_t word = p_used_words[w];
		while (word != 0u) {
			uint32_t bit = 0u;
			while ((word & (1u << bit)) == 0u) {
				bit++;
			}
			word &= ~(1u << bit);
			const uint32_t slot = w * 32u + bit;
			if (slot < page_slots.size() && page_slots[slot].occupied) {
				_touch_page_slot(slot);
				pending_used_slots.push_back(slot);
			}
		}
	}
}

void RendererVirtualMeshStorage::_io_worker_entry(void *p_userdata) {
	IOWorkerBatch *batch = static_cast<IOWorkerBatch *>(p_userdata);
	if (batch == nullptr || batch->owner == nullptr) {
		return;
	}

	// 1. Complete lightweight .ngm cluster-header DAG scans (no debug delay injection).
	for (uint32_t s = 0; s < batch->dag_scans.size(); s++) {
		if (batch->owner->shutting_down.is_set()) {
			break;
		}
		IODagScanItem &scan = batch->dag_scans[s];
		scan.page_parents.resize(scan.total_page_count);
		Ref<NGMFile> ngm;
		ngm.instantiate();
		if (ngm->open(scan.ngm_path) != OK) {
			continue;
		}
		const uint32_t count = MIN(scan.total_page_count, ngm->get_page_count());
		Vector<uint8_t> records;
		bool all_ok = true;
		for (uint32_t p = 0; p < count; p++) {
			uint32_t c_count = 0;
			if (ngm->read_page_cluster_records(p, c_count, records) != OK) {
				all_ok = false;
				break;
			}
			for (uint32_t i = 0; i < c_count; i++) {
				int32_t refined_group = -1;
				memcpy(&refined_group, records.ptr() + i * 80u + 60u, sizeof(int32_t));
				if (refined_group >= 0 && uint32_t(refined_group) < scan.group_page_ids.size()) {
					const uint32_t child_page = scan.group_page_ids[uint32_t(refined_group)];
					if (child_page != p && child_page < scan.total_page_count && !scan.page_parents[child_page].has(p)) {
						scan.page_parents[child_page].push_back(p);
					}
				}
			}
		}
		scan.ok = all_ok;
	}

	// 2. Read requested page payloads (with deterministic wall-time debug IO delay per request).
	for (uint32_t i = 0; i < batch->items.size(); i++) {
		if (batch->owner->shutting_down.is_set()) {
			break;
		}
		const IOBatchItem &item = batch->items[i];
		const uint64_t delay_usec = compute_debug_io_delay_usec(item.delay_seed, item.request_index, item.delay_ms);
		batch->delays_usec.push_back(delay_usec);
		if (delay_usec > 0 && OS::get_singleton() != nullptr) {
			uint64_t remaining = delay_usec;
			while (remaining > 0 && !batch->owner->shutting_down.is_set()) {
				const uint32_t slice = uint32_t(MIN<uint64_t>(remaining, 5000ULL));
				OS::get_singleton()->delay_usec(slice);
				remaining -= slice;
			}
			if (batch->owner->shutting_down.is_set()) {
				break;
			}
		}

		StagedPage out;
		out.mesh = item.mesh;
		out.generation = item.generation;
		out.page_id = item.page_id;
		out.topo_depth = item.topo_depth;
		out.visible = item.visible;
		out.distance = item.distance;
		out.px_error = item.px_error;
		if (item.has_in_memory) {
			out.raw = item.in_memory_raw;
		} else if (!item.ngm_path.is_empty()) {
			Ref<NGMFile> ngm;
			ngm.instantiate();
			if (ngm->open(item.ngm_path) == OK) {
				ngm->read_page(item.page_id, out.raw);
			}
		}
		if (!out.raw.is_empty()) {
			batch->results.push_back(out);
		}
	}

	if (!batch->delays_usec.is_empty()) {
		MutexLock lock(batch->owner->delay_history_mutex);
		for (uint32_t i = 0; i < batch->delays_usec.size(); i++) {
			batch->owner->recorded_io_delays_usec.push_back(batch->delays_usec[i]);
		}
	}

	batch->done.set();
}

void RendererVirtualMeshStorage::wait_for_streaming_tasks(bool p_cancel) {
	if (p_cancel) {
		shutting_down.set();
	}
	for (uint32_t i = 0; i < active_batches.size(); i++) {
		IOWorkerBatch *batch = active_batches[i];
		if (batch->task_id != WorkerThreadPool::INVALID_TASK_ID && WorkerThreadPool::get_singleton() != nullptr) {
			WorkerThreadPool::get_singleton()->wait_for_task_completion(batch->task_id);
			batch->task_id = WorkerThreadPool::INVALID_TASK_ID;
		}
	}
	if (p_cancel) {
		shutting_down.clear();
	}
	_poll_worker_batches();
}

void RendererVirtualMeshStorage::_poll_worker_batches() {
	LocalVector<RID> newly_ready_meshes;
	for (int32_t i = int32_t(active_batches.size()) - 1; i >= 0; i--) {
		IOWorkerBatch *batch = active_batches[uint32_t(i)];
		if (!batch->done.is_set()) {
			continue;
		}
		if (batch->task_id != WorkerThreadPool::INVALID_TASK_ID && WorkerThreadPool::get_singleton() != nullptr) {
			WorkerThreadPool::get_singleton()->wait_for_task_completion(batch->task_id);
			batch->task_id = WorkerThreadPool::INVALID_TASK_ID;
		}

		for (uint32_t s = 0; s < batch->dag_scans.size(); s++) {
			const IODagScanItem &scan = batch->dag_scans[s];
			MeshInfo *info = meshes.getptr(scan.mesh);
			if (info == nullptr || info->generation != scan.generation) {
				continue;
			}
			info->page_dag_scanning = false;
			if (scan.ok) {
				info->page_parents = scan.page_parents;
				_rebuild_page_children_and_depths(*info);
				info->page_dag_ready = true;
				newly_ready_meshes.push_back(scan.mesh);
			}
		}

		for (uint32_t r = 0; r < batch->items.size(); r++) {
			in_flight_pages.erase({ batch->items[r].mesh, batch->items[r].page_id });
		}
		for (uint32_t r = 0; r < batch->results.size(); r++) {
			const StagedPage &res = batch->results[r];
			const MeshInfo *info = meshes.getptr(res.mesh);
			if (info == nullptr || info->generation != res.generation || res.page_id >= info->total_page_count) {
				continue;
			}
			if (info->page_to_slot[res.page_id] != INVALID_PAGE_SLOT) {
				continue;
			}
			staged_pages.push_back(res);
		}

		memdelete(batch);
		active_batches.remove_at(uint32_t(i));
	}

	// Expand missing ancestor pages for any mesh whose .ngm page_parents scan just completed.
	for (const RID &mesh_rid : newly_ready_meshes) {
		MeshInfo *info = meshes.getptr(mesh_rid);
		if (info == nullptr || !info->page_dag_ready) {
			continue;
		}
		LocalVector<PrioritizedPageRequest> snapshot;
		for (KeyValue<QueuedPageKey, PrioritizedPageRequest> &E : pending_requests) {
			if (E.key.mesh == mesh_rid) {
				E.value.topo_depth = E.key.page_id < info->page_topo_depth.size() ? info->page_topo_depth[E.key.page_id] : 0u;
				snapshot.push_back(E.value);
			}
		}
		for (const PrioritizedPageRequest &req : snapshot) {
			PageRequest pr;
			pr.mesh_index = info->mesh_index;
			pr.page_id = req.page_id;
			pr.visible = req.visible;
			pr.distance = req.distance;
			pr.px_error = req.px_error;
			submit_page_requests(&pr, 1, true);
		}
	}
}

void RendererVirtualMeshStorage::_dispatch_worker_batches() {
	if (pending_requests.is_empty() || active_batches.size() >= MAX_ACTIVE_IO_BATCHES) {
		return;
	}

	// Collect any meshes needing an initial .ngm cluster-header scan.
	LocalVector<IODagScanItem> dag_scans;
	HashSet<RID> scan_queued;
	LocalVector<PrioritizedPageRequest> ready_requests;
	for (KeyValue<QueuedPageKey, PrioritizedPageRequest> &E : pending_requests) {
		MeshInfo *info = meshes.getptr(E.key.mesh);
		if (info == nullptr || E.value.generation != info->generation || E.key.page_id >= info->total_page_count) {
			continue;
		}
		if (info->page_to_slot[E.key.page_id] != INVALID_PAGE_SLOT) {
			continue;
		}
		if (!info->page_dag_ready) {
			if (!info->page_dag_scanning && !scan_queued.has(info->mesh) && !info->desc.ngm_path.is_empty()) {
				info->page_dag_scanning = true;
				scan_queued.insert(info->mesh);
				IODagScanItem scan;
				scan.mesh = info->mesh;
				scan.generation = info->generation;
				scan.ngm_path = info->desc.ngm_path;
				scan.total_page_count = info->total_page_count;
				scan.group_count = info->group_count;
				scan.group_page_ids = info->group_page_ids;
				dag_scans.push_back(scan);
			}
			continue;
		}
		E.value.topo_depth = E.key.page_id < info->page_topo_depth.size() ? info->page_topo_depth[E.key.page_id] : 0u;
		ready_requests.push_back(E.value);
	}

	if (dag_scans.is_empty() && ready_requests.is_empty()) {
		return;
	}

	SortArray<PrioritizedPageRequest, StagedPagePriorityComparator> sorter;
	sorter.sort(ready_requests.ptr(), ready_requests.size());

	Vector2 delay_ms;
	if (ProjectSettings::get_singleton() != nullptr && ProjectSettings::get_singleton()->has_setting("rendering/nextgeo/streaming/debug_io_delay_ms")) {
		delay_ms = parse_debug_io_delay_setting(ProjectSettings::get_singleton()->get_setting("rendering/nextgeo/streaming/debug_io_delay_ms"));
	}

	while ((active_batches.size() < MAX_ACTIVE_IO_BATCHES) && (!dag_scans.is_empty() || !ready_requests.is_empty())) {
		IOWorkerBatch *batch = memnew(IOWorkerBatch);
		batch->owner = this;
		if (!dag_scans.is_empty()) {
			batch->dag_scans = dag_scans;
			dag_scans.clear();
		}

		uint32_t cursor = 0;
		while (cursor < ready_requests.size() && batch->items.size() < MAX_ITEMS_PER_IO_BATCH) {
			const PrioritizedPageRequest &req = ready_requests[cursor++];
			const QueuedPageKey key = { req.mesh, req.page_id };
			pending_requests.erase(key);
			MeshInfo *info = meshes.getptr(req.mesh);
			if (info == nullptr || info->page_to_slot[req.page_id] != INVALID_PAGE_SLOT || in_flight_pages.has(key)) {
				continue;
			}

			IOBatchItem item;
			item.mesh = req.mesh;
			item.generation = req.generation;
			item.ngm_path = info->desc.ngm_path;
			item.page_id = req.page_id;
			item.topo_depth = req.topo_depth;
			item.visible = req.visible;
			item.distance = req.distance;
			item.px_error = req.px_error;
			item.request_index = next_io_request_index++;
			item.delay_seed = io_delay_seed;
			item.delay_ms = delay_ms;
			const Vector<Vector<uint8_t>> *mem_pages = in_memory_mesh_pages.getptr(req.mesh);
			if (mem_pages != nullptr && int(req.page_id) < mem_pages->size()) {
				item.in_memory_raw = (*mem_pages)[int(req.page_id)];
				item.has_in_memory = true;
			}
			in_flight_pages.insert(key);
			batch->items.push_back(item);
		}
		if (cursor > 0) {
			for (uint32_t r = cursor; r < ready_requests.size(); r++) {
				ready_requests[r - cursor] = ready_requests[r];
			}
			ready_requests.resize(ready_requests.size() - cursor);
		}

		if (batch->dag_scans.is_empty() && batch->items.is_empty()) {
			memdelete(batch);
			break;
		}

		active_batches.push_back(batch);
		if (WorkerThreadPool::get_singleton() != nullptr) {
			batch->task_id = WorkerThreadPool::get_singleton()->add_native_task(&RendererVirtualMeshStorage::_io_worker_entry, batch, false, "NextGeo Page Streaming IO");
		} else {
			_io_worker_entry(batch);
		}
	}
}

void RendererVirtualMeshStorage::_rebuild_residency_gpu_table() {
	const uint32_t mesh_count = mesh_order.size();
	uint32_t cursor_words = 2u + mesh_count * 4u;
	for (uint32_t m = 0; m < mesh_count; m++) {
		MeshInfo *info = meshes.getptr(mesh_order[m]);
		if (info == nullptr) {
			continue;
		}
		info->residency_refine_word_offset = cursor_words;
		cursor_words += info->refine_ok_bitset.size();
		info->residency_page_table_word_offset = cursor_words;
		cursor_words += info->total_page_count * 2u;
	}
	cursor_words = MAX(cursor_words, 4u);

	residency_words.resize(cursor_words);
	for (uint32_t i = 0; i < cursor_words; i++) {
		residency_words[i] = 0u;
	}
	residency_words[0] = slot_page_size;
	residency_words[1] = mesh_count;

	for (uint32_t m = 0; m < mesh_count; m++) {
		const MeshInfo *info = meshes.getptr(mesh_order[m]);
		const uint32_t hdr = 2u + m * 4u;
		if (info == nullptr) {
			continue;
		}
		residency_words[hdr + 0u] = info->residency_refine_word_offset;
		residency_words[hdr + 1u] = info->residency_page_table_word_offset;
		residency_words[hdr + 2u] = info->group_count;
		residency_words[hdr + 3u] = info->total_page_count;
		for (uint32_t w = 0; w < info->refine_ok_bitset.size(); w++) {
			residency_words[info->residency_refine_word_offset + w] = info->refine_ok_bitset[w];
		}
		for (uint32_t p = 0; p < info->total_page_count; p++) {
			residency_words[info->residency_page_table_word_offset + p * 2u + 0u] = info->page_to_slot[p];
			residency_words[info->residency_page_table_word_offset + p * 2u + 1u] = p < info->page_first_cluster.size() ? info->page_first_cluster[p] : 0u;
		}
	}

	const uint32_t required_bytes = cursor_words * sizeof(uint32_t);
	if (required_bytes > residency_capacity_bytes) {
		uint32_t cap = residency_capacity_bytes == 0 ? 4096u : residency_capacity_bytes;
		while (cap < required_bytes) {
			cap *= 2u;
		}
		if (_resize_residency(cap)) {
			residency_capacity_bytes = cap;
		}
	}
	if (residency_capacity_bytes >= required_bytes) {
		_upload_residency(0, reinterpret_cast<const uint8_t *>(residency_words.ptr()), required_bytes);
	}
	residency_layout_dirty = false;
	residency_data_dirty = false;
}

void RendererVirtualMeshStorage::_sync_residency_gpu_mesh(const MeshInfo &p_info) {
	if (residency_layout_dirty) {
		return;
	}
	const uint32_t refine_words = p_info.refine_ok_bitset.size();
	const uint32_t table_words = p_info.total_page_count * 2u;
	if (p_info.residency_refine_word_offset + refine_words > residency_words.size() ||
			p_info.residency_page_table_word_offset + table_words > residency_words.size()) {
		residency_layout_dirty = true;
		return;
	}
	for (uint32_t w = 0; w < refine_words; w++) {
		residency_words[p_info.residency_refine_word_offset + w] = p_info.refine_ok_bitset[w];
	}
	for (uint32_t p = 0; p < p_info.total_page_count; p++) {
		residency_words[p_info.residency_page_table_word_offset + p * 2u + 0u] = p_info.page_to_slot[p];
		residency_words[p_info.residency_page_table_word_offset + p * 2u + 1u] = p < p_info.page_first_cluster.size() ? p_info.page_first_cluster[p] : 0u;
	}
	const uint32_t start_word = p_info.residency_refine_word_offset;
	const uint32_t total_words = refine_words + table_words;
	if (total_words > 0 && (start_word + total_words) * sizeof(uint32_t) <= residency_capacity_bytes) {
		_upload_residency(
				start_word * sizeof(uint32_t),
				reinterpret_cast<const uint8_t *>(residency_words.ptr() + start_word),
				total_words * sizeof(uint32_t));
	}
}

void RendererVirtualMeshStorage::_process_streaming_frame() {
	residency_changed_records.clear();
	last_frame_upload_bytes = 0;

	if (!meshes.is_empty()) {
		_ensure_page_arena();
	}

	_poll_worker_batches();

	// Mark resident ancestors of staged / pending / in-flight pages as used this frame
	// so LRU eviction never drops an ancestor needed by an incoming child.
	auto touch_ancestors = [&](const RID &p_mesh_rid, uint32_t p_page_id) {
		const MeshInfo *info = meshes.getptr(p_mesh_rid);
		if (info == nullptr || !info->page_dag_ready || p_page_id >= info->total_page_count) {
			return;
		}
		LocalVector<uint32_t> stack;
		HashSet<uint32_t> visited;
		stack.push_back(p_page_id);
		visited.insert(p_page_id);
		while (!stack.is_empty()) {
			const uint32_t cur = stack[stack.size() - 1];
			stack.remove_at(stack.size() - 1);
			if (cur < info->page_to_slot.size() && info->page_to_slot[cur] != INVALID_PAGE_SLOT) {
				_touch_page_slot(info->page_to_slot[cur]);
			}
			if (cur < info->page_parents.size()) {
				for (uint32_t parent_page : info->page_parents[cur]) {
					if (parent_page < info->total_page_count && !visited.has(parent_page)) {
						visited.insert(parent_page);
						stack.push_back(parent_page);
					}
				}
			}
		}
	};
	for (StagedPage &sp : staged_pages) {
		const MeshInfo *info = meshes.getptr(sp.mesh);
		if (info != nullptr && sp.page_id < info->page_topo_depth.size()) {
			sp.topo_depth = info->page_topo_depth[sp.page_id];
		}
		touch_ancestors(sp.mesh, sp.page_id);
	}
	for (const KeyValue<QueuedPageKey, PrioritizedPageRequest> &E : pending_requests) {
		touch_ancestors(E.key.mesh, E.key.page_id);
	}
	for (const QueuedPageKey &key : in_flight_pages) {
		touch_ancestors(key.mesh, key.page_id);
	}
	for (const uint32_t &used_slot : pending_used_slots) {
		if (used_slot < page_slots.size() && page_slots[used_slot].occupied) {
			_touch_page_slot(used_slot);
			touch_ancestors(page_slots[used_slot].mesh, page_slots[used_slot].page_id);
		}
	}
	pending_used_slots.clear();

	if (!staged_pages.is_empty()) {
		SortArray<StagedPage, StagedPagePriorityComparator> sorter;
		sorter.sort(staged_pages.ptr(), staged_pages.size());
	}

	const bool capture_gpu_ts = RSG::utilities != nullptr && RSG::virtual_mesh_storage == this;
	if (capture_gpu_ts) {
		RSG::utilities->capture_timestamp("NextGeo Page Upload");
	}
	const uint64_t cpu_t0 = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;

	const uint64_t budget_bytes = _desired_page_upload_budget_bytes();
	uint64_t uploaded_bytes = 0;

	if (!staged_pages.is_empty()) {
		LocalVector<uint8_t> consumed;
		consumed.resize(staged_pages.size());
		for (uint32_t i = 0; i < consumed.size(); i++) {
			consumed[i] = 0u;
		}

		// Multi-pass scan so a newly uploaded parent unblocks a staged child in the same frame
		// (as long as the per-frame KiB cap has budget remaining).
		bool uploaded_any = true;
		while (uploaded_any && uploaded_bytes < budget_bytes) {
			uploaded_any = false;
			for (uint32_t i = 0; i < staged_pages.size(); i++) {
				if (consumed[i]) {
					continue;
				}
				const StagedPage &sp = staged_pages[i];
				MeshInfo *info = meshes.getptr(sp.mesh);
				if (info == nullptr || info->generation != sp.generation || sp.page_id >= info->total_page_count || sp.raw.is_empty()) {
					consumed[i] = 1u;
					continue;
				}
				if (info->page_to_slot[sp.page_id] != INVALID_PAGE_SLOT) {
					_touch_page_slot(info->page_to_slot[sp.page_id]);
					consumed[i] = 1u;
					continue;
				}
				if (!_all_parent_pages_resident(*info, sp.page_id)) {
					// Ensure missing parents are queued if not already in flight or staged.
					PageRequest req;
					req.mesh_index = info->mesh_index;
					req.page_id = sp.page_id;
					req.visible = sp.visible;
					req.distance = sp.distance;
					req.px_error = sp.px_error;
					submit_page_requests(&req, 1, true);
					continue;
				}

				const uint64_t page_bytes = uint64_t(sp.raw.size());
				if (uploaded_bytes > 0 && uploaded_bytes + page_bytes > budget_bytes) {
					continue;
				}
				if (uploaded_bytes == 0 && page_bytes > budget_bytes && budget_bytes < uint64_t(slot_page_size)) {
					// Strict sub-page budget set by caller/test: respect cap.
					break;
				}

				const uint32_t slot = _allocate_page_slot(false);
				if (slot == INVALID_PAGE_SLOT) {
					break; // Cache full of pinned or actively used pages.
				}
				if (_install_page_in_slot(*info, sp.page_id, slot, sp.raw, false, true)) {
					uploaded_bytes += page_bytes;
					consumed[i] = 1u;
					uploaded_any = true;
				} else {
					free_page_slots.push_back(slot);
					consumed[i] = 1u;
				}
			}
		}

		uint32_t write_idx = 0;
		for (uint32_t i = 0; i < staged_pages.size(); i++) {
			if (!consumed[i]) {
				if (write_idx != i) {
					staged_pages[write_idx] = staged_pages[i];
				}
				write_idx++;
			}
		}
		staged_pages.resize(write_idx);
	}

	const uint64_t cpu_t1 = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : cpu_t0;
	last_cpu_page_upload_ms = cpu_t1 >= cpu_t0 ? double(cpu_t1 - cpu_t0) / 1000.0 : 0.0;
	if (capture_gpu_ts) {
		RSG::utilities->capture_timestamp("<NextGeo Page Upload");
	}
	last_frame_upload_bytes = uint32_t(uploaded_bytes);

	_dispatch_worker_batches();

	if (residency_layout_dirty) {
		_rebuild_residency_gpu_table();
	} else if (residency_data_dirty) {
		for (const uint32_t &mesh_idx : residency_changed_meshes) {
			if (mesh_idx < mesh_order.size()) {
				const MeshInfo *info = meshes.getptr(mesh_order[mesh_idx]);
				if (info != nullptr) {
					_sync_residency_gpu_mesh(*info);
				}
			}
		}
		residency_data_dirty = false;
	}

	// Publish instance record slots whose virtual mesh changed its resident page set (#78 step 7, #82).
	if (!residency_changed_meshes.is_empty()) {
		for (uint32_t slot = 0; slot < used_slots; slot++) {
			if (slot_states[slot].allocated && !slot_states[slot].gpu_managed && residency_changed_meshes.has(records[slot].virtual_mesh)) {
				residency_changed_records.push_back(slot);
			}
		}
		residency_changed_meshes.clear();
		if (!residency_changed_records.is_empty()) {
			residency_event_epoch++;
		}
	}
}

// ============================================================================
// Foliage Instancing, MultiMesh Ranges & Nanite Assemblies (#151)
// ============================================================================

bool RendererVirtualMeshStorage::are_assemblies_enabled() {
	if (ProjectSettings::get_singleton() == nullptr) {
		return false;
	}
	return bool(ProjectSettings::get_singleton()->get_setting("rendering/nextgeo/experimental/assemblies", false));
}

bool RendererVirtualMeshStorage::is_foliage_wind_enabled() {
	if (ProjectSettings::get_singleton() == nullptr) {
		return false;
	}
	return bool(ProjectSettings::get_singleton()->get_setting("rendering/nextgeo/experimental/foliage_wind", false));
}

bool RendererVirtualMeshStorage::mesh_has_wind(RID p_mesh) const {
	const uint32_t *idx = mesh_indices.getptr(p_mesh);
	if (idx == nullptr) {
		return false;
	}
	return mesh_index_has_wind(*idx);
}

bool RendererVirtualMeshStorage::mesh_index_has_wind(uint32_t p_mesh_index) const {
	if (p_mesh_index >= wind_meshes.size()) {
		return false;
	}
	return wind_meshes[p_mesh_index].bone_count > 0u;
}

float RendererVirtualMeshStorage::mesh_get_wind_max_sway(RID p_mesh) const {
	const uint32_t *idx = mesh_indices.getptr(p_mesh);
	if (idx == nullptr) {
		return 0.0f;
	}
	return mesh_index_get_wind_max_sway(*idx);
}

float RendererVirtualMeshStorage::mesh_index_get_wind_max_sway(uint32_t p_mesh_index) const {
	if (p_mesh_index >= wind_meshes.size() || wind_meshes[p_mesh_index].bone_count == 0u) {
		return 0.0f;
	}
	return wind_meshes[p_mesh_index].max_sway;
}

const RendererVirtualMeshStorage::WindMeshGPU *RendererVirtualMeshStorage::get_wind_mesh(uint32_t p_mesh_index) const {
	if (p_mesh_index >= wind_meshes.size() || wind_meshes[p_mesh_index].bone_count == 0u) {
		return nullptr;
	}
	return &wind_meshes[p_mesh_index];
}

void RendererVirtualMeshStorage::set_wind_params(const Vector3 &p_direction, float p_strength, float p_gust_frequency, float p_time, float p_prev_time, bool p_enabled) {
	Vector3 dir = p_direction;
	if (dir.length_squared() > real_t(1e-10)) {
		dir.normalize();
	} else {
		dir = Vector3(1.0f, 0.0f, 0.0f);
	}
	WindParamsGPU next;
	next.direction[0] = float(dir.x);
	next.direction[1] = float(dir.y);
	next.direction[2] = float(dir.z);
	next.strength = MAX(Math::is_finite(p_strength) ? p_strength : 0.0f, 0.0f);
	next.time = Math::is_finite(p_time) ? p_time : 0.0f;
	next.prev_time = Math::is_finite(p_prev_time) ? p_prev_time : next.time;
	next.gust_frequency = MAX(Math::is_finite(p_gust_frequency) ? p_gust_frequency : 1.0f, 0.0f);
	next.flags = p_enabled ? WIND_PARAM_FLAG_ENABLED : 0u;
	next.mesh_count = wind_meshes.size();
	if (memcmp(&wind_params, &next, sizeof(WindParamsGPU)) != 0) {
		wind_params = next;
		wind_params_dirty = true;
	}
}

bool RendererVirtualMeshStorage::mesh_has_assembly(RID p_mesh) const {
	const MeshInfo *info = meshes.getptr(p_mesh);
	return info != nullptr && info->has_assembly;
}

uint32_t RendererVirtualMeshStorage::mesh_get_assembly_leaf_count(RID p_mesh) const {
	const MeshInfo *info = meshes.getptr(p_mesh);
	return info != nullptr && info->has_assembly ? info->assembly_leaf_count : 0u;
}

const LocalVector<RID> *RendererVirtualMeshStorage::mesh_get_assembly_unique_part_meshes(RID p_mesh) const {
	const MeshInfo *info = meshes.getptr(p_mesh);
	return (info != nullptr && info->has_assembly) ? &info->assembly_unique_part_meshes : nullptr;
}

void RendererVirtualMeshStorage::mesh_get_assembly_expanded_parts_cpu(RID p_mesh, const Transform3D &p_root_transform, LocalVector<AssemblyLeafCPU> &r_out) const {
	r_out.clear();
	const MeshInfo *info = meshes.getptr(p_mesh);
	if (info == nullptr || !info->has_assembly) {
		return;
	}
	auto append_part = [&](const RenderingServerTypes::VirtualGeometryDesc::AssemblyPartDesc &p_part, const Transform3D &p_parent_world) {
		if (p_part.mesh.is_null() || p_part.mesh == p_mesh) {
			return;
		}
		int64_t unique_idx = info->assembly_unique_part_meshes.find(p_part.mesh);
		if (unique_idx < 0) {
			return;
		}
		AssemblyLeafCPU leaf;
		leaf.mesh = p_part.mesh;
		const uint32_t *m_idx = mesh_indices.getptr(p_part.mesh);
		leaf.part_mesh_index = (m_idx != nullptr && meshes.has(p_part.mesh)) ? *m_idx : UINT32_MAX;
		leaf.unique_part_index = uint32_t(unique_idx);
		leaf.world_transform = p_parent_world * p_part.local_transform;
		if (p_part.local_aabb.has_surface()) {
			leaf.local_aabb = p_part.local_aabb;
		} else if (RSG::mesh_storage != nullptr && p_part.mesh.is_valid()) {
			leaf.local_aabb = RSG::mesh_storage->mesh_get_aabb(p_part.mesh);
		}
		r_out.push_back(leaf);
	};
	for (int i = 0; i < info->desc.assembly_parts.size(); i++) {
		append_part(info->desc.assembly_parts[i], p_root_transform);
	}
	for (int c = 0; c < info->desc.assembly_children.size(); c++) {
		const RenderingServerTypes::VirtualGeometryDesc::AssemblyChildDesc &child = info->desc.assembly_children[c];
		const Transform3D child_world = p_root_transform * child.local_transform;
		for (int i = 0; i < child.parts.size(); i++) {
			append_part(child.parts[i], child_world);
		}
	}
}

AABB RendererVirtualMeshStorage::mesh_get_assembly_local_aabb(RID p_mesh) const {
	LocalVector<AssemblyLeafCPU> leaves;
	mesh_get_assembly_expanded_parts_cpu(p_mesh, Transform3D(), leaves);
	AABB out;
	bool has = false;
	for (uint32_t i = 0; i < leaves.size(); i++) {
		if (!leaves[i].local_aabb.has_surface()) {
			continue;
		}
		const AABB part_aabb = leaves[i].world_transform.xform(leaves[i].local_aabb);
		out = has ? out.merge(part_aabb) : part_aabb;
		has = true;
	}
	return out;
}

uint32_t RendererVirtualMeshStorage::_bucket_slot_allocate(uint32_t p_count) {
	const uint32_t count = MAX(p_count, 1u);
	for (uint32_t i = 0; i < bucket_free.size(); i++) {
		if (bucket_free[i].count < count) {
			continue;
		}
		const uint32_t base = bucket_free[i].first;
		bucket_free[i].first += count;
		bucket_free[i].count -= count;
		if (bucket_free[i].count == 0u) {
			bucket_free.remove_at(i);
		}
		return base;
	}
	const uint32_t base = bucket_slots_used;
	bucket_slots_used += count;
	if (bucket_surface_masks.size() < bucket_slots_used) {
		const uint32_t old_size = bucket_surface_masks.size();
		bucket_surface_masks.resize(bucket_slots_used);
		for (uint32_t i = old_size; i < bucket_slots_used; i++) {
			bucket_surface_masks[i] = BucketSurfaceMaskGPU();
		}
		bucket_surface_masks_dirty = true;
	}
	return base;
}

void RendererVirtualMeshStorage::_bucket_slot_free(uint32_t p_base, uint32_t p_count) {
	if (p_base == UINT32_MAX || p_count == 0u) {
		return;
	}
	for (uint32_t i = 0; i < p_count && p_base + i < bucket_surface_masks.size(); i++) {
		bucket_surface_masks[p_base + i] = BucketSurfaceMaskGPU();
	}
	bucket_surface_masks_dirty = true;
	UploadRun run;
	run.first = p_base;
	run.count = p_count;
	bucket_free.push_back(run);
	if (bucket_free.size() > 1u) {
		for (uint32_t i = 0; i < bucket_free.size(); i++) {
			for (uint32_t j = i + 1; j < bucket_free.size(); j++) {
				if (bucket_free[j].first < bucket_free[i].first) {
					SWAP(bucket_free[i], bucket_free[j]);
				}
			}
		}
		uint32_t write_idx = 0u;
		for (uint32_t i = 0; i < bucket_free.size(); i++) {
			if (write_idx > 0u && bucket_free[write_idx - 1u].first + bucket_free[write_idx - 1u].count == bucket_free[i].first) {
				bucket_free[write_idx - 1u].count += bucket_free[i].count;
			} else {
				bucket_free[write_idx++] = bucket_free[i];
			}
		}
		bucket_free.resize(write_idx);
	}
}

uint32_t RendererVirtualMeshStorage::instance_record_get_bucket_slot(uint32_t p_record) const {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return UINT32_MAX;
	}
	return slot_states[p_record].bucket_slot;
}

uint32_t RendererVirtualMeshStorage::instance_record_get_bucket_slot_count(uint32_t p_record) const {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return 0u;
	}
	return slot_states[p_record].bucket_slot_count;
}

void RendererVirtualMeshStorage::instance_record_resize_bucket_slots(uint32_t p_record, uint32_t p_count) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	const uint32_t count = MAX(p_count, 1u);
	SlotState &state = slot_states[p_record];
	if (state.bucket_slot_count == count && state.bucket_slot != UINT32_MAX) {
		return;
	}
	if (state.bucket_slot != UINT32_MAX && state.bucket_slot_count > 0u) {
		_bucket_slot_free(state.bucket_slot, state.bucket_slot_count);
	}
	state.bucket_slot = _bucket_slot_allocate(count);
	state.bucket_slot_count = count;
	records[p_record].bucket_base = state.bucket_slot * 32u;
	_mark_dirty(p_record);
}

void RendererVirtualMeshStorage::bucket_slot_set_surface_masks(uint32_t p_bucket_slot, uint32_t p_surface_mask, uint32_t p_shadow_surface_mask, uint32_t p_cull_disabled_mask, uint32_t p_cull_front_mask) {
	if (p_bucket_slot == UINT32_MAX) {
		return;
	}
	if (p_bucket_slot >= bucket_surface_masks.size()) {
		const uint32_t old_size = bucket_surface_masks.size();
		bucket_surface_masks.resize(p_bucket_slot + 1u);
		for (uint32_t i = old_size; i < bucket_surface_masks.size(); i++) {
			bucket_surface_masks[i] = BucketSurfaceMaskGPU();
		}
	}
	BucketSurfaceMaskGPU &entry = bucket_surface_masks[p_bucket_slot];
	if (entry.surface_mask == p_surface_mask && entry.shadow_surface_mask == p_shadow_surface_mask && entry.cull_disabled_mask == p_cull_disabled_mask && entry.cull_front_mask == p_cull_front_mask) {
		return;
	}
	entry.surface_mask = p_surface_mask;
	entry.shadow_surface_mask = p_shadow_surface_mask;
	entry.cull_disabled_mask = p_cull_disabled_mask;
	entry.cull_front_mask = p_cull_front_mask;
	bucket_surface_masks_dirty = true;
}

void RendererVirtualMeshStorage::instance_record_set_visibility_range(uint32_t p_record, float p_begin, float p_end) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	const float begin = (!Math::is_finite(p_begin) || p_begin < 0.0f) ? 0.0f : p_begin;
	const float end = (!Math::is_finite(p_end) || p_end < 0.0f) ? 0.0f : p_end;
	uint32_t begin_bits = 0u;
	uint32_t end_bits = 0u;
	memcpy(&begin_bits, &begin, sizeof(uint32_t));
	memcpy(&end_bits, &end, sizeof(uint32_t));
	InstanceAttributes &attr = instance_attributes[p_record];
	if (attr.meta[2] == begin_bits && attr.meta[3] == end_bits) {
		return;
	}
	attr.meta[2] = begin_bits;
	attr.meta[3] = end_bits;
	_mark_dirty(p_record);
}

void RendererVirtualMeshStorage::instance_record_set_attributes(uint32_t p_record, const Color &p_color, const Color &p_custom_data, uint32_t p_parent_record, uint32_t p_element_index, bool p_has_color, bool p_has_custom) {
	if (p_record >= used_slots) {
		return;
	}
	InstanceAttributes &attr = instance_attributes[p_record];
	attr.color[0] = p_color.r;
	attr.color[1] = p_color.g;
	attr.color[2] = p_color.b;
	attr.color[3] = p_color.a;
	attr.custom_data[0] = p_custom_data.r;
	attr.custom_data[1] = p_custom_data.g;
	attr.custom_data[2] = p_custom_data.b;
	attr.custom_data[3] = p_custom_data.a;
	attr.meta[0] = p_parent_record;
	attr.meta[1] = p_element_index;
	if (p_has_color) {
		records[p_record].flags |= RECORD_HAS_INSTANCE_COLOR;
	} else {
		records[p_record].flags &= ~RECORD_HAS_INSTANCE_COLOR;
	}
	if (p_has_custom) {
		records[p_record].flags |= RECORD_HAS_INSTANCE_CUSTOM;
	} else {
		records[p_record].flags &= ~RECORD_HAS_INSTANCE_CUSTOM;
	}
	_mark_dirty(p_record);
}

const RendererVirtualMeshStorage::InstanceAttributes &RendererVirtualMeshStorage::get_instance_attributes(uint32_t p_record) const {
	static const InstanceAttributes fallback;
	if (p_record >= instance_attributes.size()) {
		return fallback;
	}
	return instance_attributes[p_record];
}

uint32_t RendererVirtualMeshStorage::multimesh_range_attach(uint32_t p_parent_record, RID p_multimesh, RID p_mesh, uint32_t p_count) {
	if (p_parent_record >= used_slots || !slot_states[p_parent_record].allocated) {
		return UINT32_MAX;
	}
	if (p_count == 0u || p_multimesh.is_null()) {
		multimesh_range_detach(p_parent_record);
		return UINT32_MAX;
	}
	const uint32_t *existing_idx = multimesh_parent_to_expand.getptr(p_parent_record);
	if (existing_idx != nullptr && *existing_idx < multimesh_expands.size()) {
		MultiMeshExpandEntry &entry = multimesh_expands[*existing_idx];
		if (entry.active && entry.range_count == p_count && entry.multimesh == p_multimesh && entry.mesh == p_mesh) {
			entry.dirty = true;
			return entry.range_base;
		}
		multimesh_range_detach(p_parent_record);
	}

	uint32_t range_base = UINT32_MAX;
	for (uint32_t i = 0; i < range_free_now.size(); i++) {
		if (range_free_now[i].count < p_count) {
			continue;
		}
		range_base = range_free_now[i].first;
		range_free_now[i].first += p_count;
		range_free_now[i].count -= p_count;
		if (range_free_now[i].count == 0u) {
			range_free_now.remove_at(i);
		}
		break;
	}
	if (range_base == UINT32_MAX) {
		range_base = used_slots;
		used_slots += p_count;
		_ensure_capacity(used_slots);
	}
	ERR_FAIL_COND_V(range_base + p_count > record_capacity, UINT32_MAX);

	for (uint32_t i = 0; i < p_count; i++) {
		const uint32_t slot = range_base + i;
		records[slot] = Record();
		instance_attributes[slot] = InstanceAttributes();
		SlotState &st = slot_states[slot];
		st.instance = RID();
		st.allocated = true;
		st.dirty = false;
		st.dirty_last = false;
		st.initialized = false;
		st.gpu_managed = true;
		st.free_frame = UINT32_MAX;
		st.bucket_slot = UINT32_MAX;
		st.bucket_slot_count = 0u;
	}

	uint32_t expand_idx = UINT32_MAX;
	for (uint32_t i = 0; i < multimesh_expands.size(); i++) {
		if (!multimesh_expands[i].active) {
			expand_idx = i;
			break;
		}
	}
	if (expand_idx == UINT32_MAX) {
		expand_idx = multimesh_expands.size();
		multimesh_expands.push_back(MultiMeshExpandEntry());
	}
	MultiMeshExpandEntry &entry = multimesh_expands[expand_idx];
	entry.parent_record = p_parent_record;
	entry.range_base = range_base;
	entry.range_count = p_count;
	entry.multimesh = p_multimesh;
	entry.mesh = p_mesh;
	entry.last_serial = 0u;
	entry.active = true;
	entry.dirty = true;
	entry.dirty_last = false;
	multimesh_parent_to_expand[p_parent_record] = expand_idx;

	records[p_parent_record].flags |= RECORD_MULTIMESH_PARENT;
	_mark_dirty(p_parent_record);
	return range_base;
}

void RendererVirtualMeshStorage::multimesh_range_detach(uint32_t p_parent_record) {
	const uint32_t *existing_idx = multimesh_parent_to_expand.getptr(p_parent_record);
	if (existing_idx == nullptr) {
		return;
	}
	const uint32_t idx = *existing_idx;
	multimesh_parent_to_expand.erase(p_parent_record);
	if (idx >= multimesh_expands.size()) {
		return;
	}
	MultiMeshExpandEntry &entry = multimesh_expands[idx];
	if (entry.active && entry.range_count > 0u) {
		const uint32_t end = MIN(entry.range_base + entry.range_count, used_slots);
		for (uint32_t slot = entry.range_base; slot < end; slot++) {
			records[slot].flags = 0u;
			instance_attributes[slot] = InstanceAttributes();
			SlotState &st = slot_states[slot];
			st.allocated = false;
			st.dirty = false;
			st.dirty_last = false;
			st.gpu_managed = false;
			st.free_frame = frame + 1u;
		}
		UploadRun run;
		run.first = entry.range_base;
		run.count = entry.range_count;
		range_free_next.push_back(run);
		pending_zero_ranges.push_back(run);
	}
	entry = MultiMeshExpandEntry();
	if (p_parent_record < used_slots && slot_states[p_parent_record].allocated) {
		records[p_parent_record].flags &= ~RECORD_MULTIMESH_PARENT;
		_mark_dirty(p_parent_record);
	}
}

void RendererVirtualMeshStorage::multimesh_mark_parent_dirty(uint32_t p_parent_record) {
	const uint32_t *idx_ptr = multimesh_parent_to_expand.getptr(p_parent_record);
	if (idx_ptr != nullptr && *idx_ptr < multimesh_expands.size() && multimesh_expands[*idx_ptr].active) {
		multimesh_expands[*idx_ptr].dirty = true;
	}
}

void RendererVirtualMeshStorage::_acquire_assembly_part_refs(MeshInfo &r_info) {
	if (r_info.assembly_part_refs_held || !r_info.has_assembly) {
		return;
	}
	r_info.assembly_part_refs_held = true;
	for (uint32_t i = 0; i < r_info.assembly_unique_part_meshes.size(); i++) {
		const RID part_rid = r_info.assembly_unique_part_meshes[i];
		MeshInfo *part_info = meshes.getptr(part_rid);
		if (part_info != nullptr) {
			part_info->live_instances++;
			if (part_info->live_instances == 1u && RSG::mesh_storage != nullptr) {
				RSG::mesh_storage->mesh_on_virtual_geometry_changed(part_rid);
			}
		}
	}
}

void RendererVirtualMeshStorage::_release_assembly_part_refs(MeshInfo &r_info) {
	if (!r_info.assembly_part_refs_held) {
		return;
	}
	r_info.assembly_part_refs_held = false;
	for (uint32_t i = 0; i < r_info.assembly_unique_part_meshes.size(); i++) {
		const RID part_rid = r_info.assembly_unique_part_meshes[i];
		MeshInfo *part_info = meshes.getptr(part_rid);
		if (part_info != nullptr && part_info->live_instances > 0u) {
			part_info->live_instances--;
			if (part_info->live_instances == 0u && RSG::mesh_storage != nullptr) {
				RSG::mesh_storage->mesh_on_virtual_geometry_changed(part_rid);
			}
		}
	}
}

void RendererVirtualMeshStorage::_rebuild_assemblies_gpu() {
	const uint32_t mesh_count = mesh_records.size();
	assembly_headers_gpu.resize(mesh_count);
	for (uint32_t i = 0; i < mesh_count; i++) {
		assembly_headers_gpu[i] = AssemblyHeaderGPU();
	}
	assembly_children_gpu.clear();
	assembly_parts_gpu.clear();

	auto store_xform = [](const Transform3D &p_t, float r_out[3][4]) {
		for (int row = 0; row < 3; row++) {
			r_out[row][0] = p_t.basis.rows[row][0];
			r_out[row][1] = p_t.basis.rows[row][1];
			r_out[row][2] = p_t.basis.rows[row][2];
			r_out[row][3] = p_t.origin[row];
		}
	};

	for (const KeyValue<RID, MeshInfo> &E : meshes) {
		const MeshInfo &info = E.value;
		if (!info.has_assembly || info.mesh_index >= mesh_count) {
			continue;
		}
		AssemblyHeaderGPU hdr;
		hdr.direct_part_offset = assembly_parts_gpu.size();

		auto push_part = [&](const RenderingServerTypes::VirtualGeometryDesc::AssemblyPartDesc &p_part) -> bool {
			if (p_part.mesh.is_null() || p_part.mesh == info.mesh) {
				return false;
			}
			const uint32_t *part_m_idx = mesh_indices.getptr(p_part.mesh);
			const MeshInfo *part_info = meshes.getptr(p_part.mesh);
			const int64_t unique_idx = info.assembly_unique_part_meshes.find(p_part.mesh);
			if (part_m_idx == nullptr || part_info == nullptr || unique_idx < 0) {
				return false;
			}
			AssemblyPartGPU gp;
			store_xform(p_part.local_transform, gp.transform);
			AABB local_aabb = p_part.local_aabb;
			if (!local_aabb.has_surface() && RSG::mesh_storage != nullptr) {
				local_aabb = RSG::mesh_storage->mesh_get_aabb(p_part.mesh);
			}
			gp.local_aabb_min[0] = local_aabb.position.x;
			gp.local_aabb_min[1] = local_aabb.position.y;
			gp.local_aabb_min[2] = local_aabb.position.z;
			gp.local_aabb_min[3] = 0.0f;
			gp.local_aabb_max[0] = local_aabb.position.x + local_aabb.size.x;
			gp.local_aabb_max[1] = local_aabb.position.y + local_aabb.size.y;
			gp.local_aabb_max[2] = local_aabb.position.z + local_aabb.size.z;
			gp.local_aabb_max[3] = 0.0f;
			gp.part_virtual_mesh = *part_m_idx;
			gp.unique_part_index = uint32_t(unique_idx);
			gp.surface_mask = part_info->desc.eligible_surface_mask;
			assembly_parts_gpu.push_back(gp);
			return true;
		};

		for (int i = 0; i < info.desc.assembly_parts.size(); i++) {
			if (push_part(info.desc.assembly_parts[i])) {
				hdr.direct_part_count++;
			}
		}

		hdr.child_offset = assembly_children_gpu.size();
		for (int c = 0; c < info.desc.assembly_children.size(); c++) {
			const RenderingServerTypes::VirtualGeometryDesc::AssemblyChildDesc &child = info.desc.assembly_children[c];
			AssemblyChildGPU gc;
			store_xform(child.local_transform, gc.transform);
			gc.part_offset = assembly_parts_gpu.size();
			for (int i = 0; i < child.parts.size(); i++) {
				if (push_part(child.parts[i])) {
					gc.part_count++;
				}
			}
			if (gc.part_count > 0u) {
				assembly_children_gpu.push_back(gc);
				hdr.child_count++;
				hdr.leaf_count += gc.part_count;
			}
		}
		hdr.leaf_count += hdr.direct_part_count;
		assembly_headers_gpu[info.mesh_index] = hdr;
	}

	_upload_assembly_buffers(
			assembly_headers_gpu.ptr(), assembly_headers_gpu.size(),
			assembly_children_gpu.ptr(), assembly_children_gpu.size(),
			assembly_parts_gpu.ptr(), assembly_parts_gpu.size());
}

void RendererVirtualMeshStorage::build_multimesh_element_record_cpu(const Record &p_parent, const InstanceAttributes &p_parent_attr, uint32_t p_parent_record_idx, uint32_t p_element_idx, const float *p_elem_cur, const float *p_elem_prev, uint32_t p_color_offset, uint32_t p_custom_offset, bool p_has_color, bool p_has_custom, bool p_visible, const AABB &p_mesh_local_aabb, Record &r_out_record, InstanceAttributes &r_out_attr) {
	r_out_record = p_parent;
	r_out_record.flags &= ~RECORD_MULTIMESH_PARENT;
	r_out_record.flags |= RECORD_MULTIMESH_ELEMENT;
	if (!p_visible || p_elem_cur == nullptr) {
		r_out_record.flags &= ~RECORD_VISIBLE;
	}
	if (p_has_color) {
		r_out_record.flags |= RECORD_HAS_INSTANCE_COLOR;
	} else {
		r_out_record.flags &= ~RECORD_HAS_INSTANCE_COLOR;
	}
	if (p_has_custom) {
		r_out_record.flags |= RECORD_HAS_INSTANCE_CUSTOM;
	} else {
		r_out_record.flags &= ~RECORD_HAS_INSTANCE_CUSTOM;
	}

	auto read_xform = [](const float p_rows[3][4]) -> Transform3D {
		Transform3D t;
		for (int r = 0; r < 3; r++) {
			t.basis.rows[r][0] = p_rows[r][0];
			t.basis.rows[r][1] = p_rows[r][1];
			t.basis.rows[r][2] = p_rows[r][2];
			t.origin[r] = p_rows[r][3];
		}
		return t;
	};
	auto read_mm_xform = [](const float *p_f) -> Transform3D {
		Transform3D t;
		for (int r = 0; r < 3; r++) {
			t.basis.rows[r][0] = p_f[r * 4 + 0];
			t.basis.rows[r][1] = p_f[r * 4 + 1];
			t.basis.rows[r][2] = p_f[r * 4 + 2];
			t.origin[r] = p_f[r * 4 + 3];
		}
		return t;
	};
	auto write_xform = [](const Transform3D &p_t, float r_rows[3][4]) {
		for (int r = 0; r < 3; r++) {
			r_rows[r][0] = p_t.basis.rows[r][0];
			r_rows[r][1] = p_t.basis.rows[r][1];
			r_rows[r][2] = p_t.basis.rows[r][2];
			r_rows[r][3] = p_t.origin[r];
		}
	};

	const Transform3D parent_cur = read_xform(p_parent.transform);
	const Transform3D parent_prev = read_xform(p_parent.prev_transform);
	const Transform3D elem_cur = p_elem_cur != nullptr ? read_mm_xform(p_elem_cur) : Transform3D();
	const Transform3D elem_prev = p_elem_prev != nullptr ? read_mm_xform(p_elem_prev) : elem_cur;
	const Transform3D world_cur = parent_cur * elem_cur;
	const Transform3D world_prev = parent_prev * elem_prev;

	write_xform(world_cur, r_out_record.transform);
	write_xform(world_prev, r_out_record.prev_transform);

	const Vector3 scale = world_cur.basis.get_scale();
	const float max_scale = MAX(scale.x, MAX(scale.y, scale.z));
	if (max_scale <= 1e-12f) {
		r_out_record.flags &= ~RECORD_VISIBLE;
	}
	if (basis_has_non_uniform_scale(world_cur.basis)) {
		r_out_record.flags |= RECORD_NON_UNIFORM_SCALE;
	} else {
		r_out_record.flags &= ~RECORD_NON_UNIFORM_SCALE;
	}

	const AABB world_aabb = world_cur.xform(p_mesh_local_aabb);
	const AABB prev_world_aabb = world_prev.xform(p_mesh_local_aabb);
	r_out_record.world_aabb_min[0] = world_aabb.position.x;
	r_out_record.world_aabb_min[1] = world_aabb.position.y;
	r_out_record.world_aabb_min[2] = world_aabb.position.z;
	r_out_record.world_aabb_min[3] = max_scale;
	r_out_record.world_aabb_max[0] = world_aabb.position.x + world_aabb.size.x;
	r_out_record.world_aabb_max[1] = world_aabb.position.y + world_aabb.size.y;
	r_out_record.world_aabb_max[2] = world_aabb.position.z + world_aabb.size.z;
	r_out_record.prev_world_aabb_min[0] = prev_world_aabb.position.x;
	r_out_record.prev_world_aabb_min[1] = prev_world_aabb.position.y;
	r_out_record.prev_world_aabb_min[2] = prev_world_aabb.position.z;
	r_out_record.prev_world_aabb_max[0] = prev_world_aabb.position.x + prev_world_aabb.size.x;
	r_out_record.prev_world_aabb_max[1] = prev_world_aabb.position.y + prev_world_aabb.size.y;
	r_out_record.prev_world_aabb_max[2] = prev_world_aabb.position.z + prev_world_aabb.size.z;

	r_out_attr = InstanceAttributes();
	if (p_has_color && p_elem_cur != nullptr) {
		for (int k = 0; k < 4; k++) {
			r_out_attr.color[k] = p_elem_cur[p_color_offset + k];
		}
	}
	if (p_has_custom && p_elem_cur != nullptr) {
		for (int k = 0; k < 4; k++) {
			r_out_attr.custom_data[k] = p_elem_cur[p_custom_offset + k];
		}
	}
	r_out_attr.meta[0] = p_parent_record_idx;
	r_out_attr.meta[1] = p_element_idx;
	r_out_attr.meta[2] = p_parent_attr.meta[2];
	r_out_attr.meta[3] = p_parent_attr.meta[3];
}

bool RendererVirtualMeshStorage::cull_instance_cpu(const Record &p_record, const InstanceAttributes &p_attr, const Record *p_parent_or_null, const float p_frustum[6][4], const Vector3 &p_camera_pos, uint32_t p_cull_mask, bool p_is_shadow_view, float p_wind_max_sway) {
	const bool cast_ok = p_is_shadow_view
			? ((p_record.flags & RECORD_CAST_SHADOW_MASK) != RECORD_CAST_SHADOW_OFF)
			: ((p_record.flags & RECORD_CAST_SHADOW_MASK) != RECORD_CAST_SHADOW_SHADOWS_ONLY);
	if ((p_record.flags & (RECORD_VALID | RECORD_VISIBLE)) != (RECORD_VALID | RECORD_VISIBLE) ||
			!cast_ok ||
			(p_record.flags & (RECORD_HAS_INSTANCE_UNIFORMS | RECORD_MULTIMESH_PARENT | RECORD_HAS_ASSEMBLY)) != 0u ||
			(p_record.flags & (RECORD_HAS_SKELETON | RECORD_SKINNED_PALETTE)) == RECORD_HAS_SKELETON ||
			(p_record.gi_flags & GI_LIGHTMAPPED) != 0u ||
			p_record.virtual_mesh == UINT32_MAX ||
			(p_is_shadow_view ? p_record.pad0 : p_record.surface_mask) == 0u ||
			(p_record.layer_mask & p_cull_mask) == 0u) {
		return false;
	}
	auto aabb_visible = [&](const Vector3 &p_lo, const Vector3 &p_hi) -> bool {
		for (int plane = 0; plane < 6; plane++) {
			const float nx = p_frustum[plane][0];
			const float ny = p_frustum[plane][1];
			const float nz = p_frustum[plane][2];
			const float fx = nx >= 0.0f ? p_hi.x : p_lo.x;
			const float fy = ny >= 0.0f ? p_hi.y : p_lo.y;
			const float fz = nz >= 0.0f ? p_hi.z : p_lo.z;
			if (nx * fx + ny * fy + nz * fz + p_frustum[plane][3] < 0.0f) {
				return false;
			}
		}
		return true;
	};
	// Coarse MultiMesh parent AABB pre-test.
	if ((p_record.flags & RECORD_MULTIMESH_ELEMENT) != 0u && p_parent_or_null != nullptr) {
		if ((p_parent_or_null->flags & (RECORD_VALID | RECORD_VISIBLE)) != (RECORD_VALID | RECORD_VISIBLE)) {
			return false;
		}
		const float parent_margin = (get_record_vertex_offset_bound(*p_parent_or_null) + MAX(p_wind_max_sway, 0.0f)) * Math::abs(p_parent_or_null->world_aabb_min[3]);
		const Vector3 plo(p_parent_or_null->world_aabb_min[0] - parent_margin, p_parent_or_null->world_aabb_min[1] - parent_margin, p_parent_or_null->world_aabb_min[2] - parent_margin);
		const Vector3 phi(p_parent_or_null->world_aabb_max[0] + parent_margin, p_parent_or_null->world_aabb_max[1] + parent_margin, p_parent_or_null->world_aabb_max[2] + parent_margin);
		if (!aabb_visible(plo, phi)) {
			return false;
		}
	}
	// Per-instance distance culling (main view).
	if (!p_is_shadow_view) {
		float vis_begin = 0.0f;
		float vis_end = 0.0f;
		memcpy(&vis_begin, &p_attr.meta[2], sizeof(float));
		memcpy(&vis_end, &p_attr.meta[3], sizeof(float));
		if (vis_begin > 0.0f || vis_end > 0.0f) {
			const Vector3 center(
					0.5f * (p_record.world_aabb_min[0] + p_record.world_aabb_max[0]),
					0.5f * (p_record.world_aabb_min[1] + p_record.world_aabb_max[1]),
					0.5f * (p_record.world_aabb_min[2] + p_record.world_aabb_max[2]));
			const float dist = center.distance_to(p_camera_pos);
			if ((vis_end > 0.0f && dist > vis_end) || (vis_begin > 0.0f && dist < vis_begin)) {
				return false;
			}
		}
	}
	// Per-instance frustum test with WPO and procedural wind sway AABB inflation.
	const float margin = (get_record_vertex_offset_bound(p_record) + MAX(p_wind_max_sway, 0.0f)) * Math::abs(p_record.world_aabb_min[3]);
	const Vector3 lo(p_record.world_aabb_min[0] - margin, p_record.world_aabb_min[1] - margin, p_record.world_aabb_min[2] - margin);
	const Vector3 hi(p_record.world_aabb_max[0] + margin, p_record.world_aabb_max[1] + margin, p_record.world_aabb_max[2] + margin);
	return aabb_visible(lo, hi);
}

void RendererVirtualMeshStorage::get_effective_records_cpu(LocalVector<Record> &r_records, LocalVector<InstanceAttributes> &r_attrs) const {
	r_records.resize(used_slots);
	r_attrs.resize(used_slots);
	for (uint32_t i = 0; i < used_slots; i++) {
		r_records[i] = records[i];
		r_attrs[i] = i < instance_attributes.size() ? instance_attributes[i] : InstanceAttributes();
	}

	// Expand MultiMesh element records on CPU matching multimesh_expand.glsl.
	if (RSG::mesh_storage != nullptr) {
		for (uint32_t i = 0; i < multimesh_expands.size(); i++) {
			const MultiMeshExpandEntry &entry = multimesh_expands[i];
			if (!entry.active || entry.parent_record >= used_slots || entry.range_base + entry.range_count > used_slots) {
				continue;
			}
			RendererMeshStorage::MultiMeshNextGeoInfo info;
			if (!RSG::mesh_storage->multimesh_get_nextgeo_info(entry.multimesh, info) || !info.eligible) {
				continue;
			}
			const Vector<float> buf = RSG::mesh_storage->multimesh_get_buffer(entry.multimesh);
			const Record &parent = r_records[entry.parent_record];
			const InstanceAttributes &parent_attr = r_attrs[entry.parent_record];
			for (uint32_t e = 0; e < entry.range_count; e++) {
				const uint32_t slot = entry.range_base + e;
				const float *elem_ptr = (uint64_t(e + 1) * uint64_t(info.stride) <= uint64_t(buf.size())) ? (buf.ptr() + e * info.stride) : nullptr;
				build_multimesh_element_record_cpu(
						parent, parent_attr, entry.parent_record, e,
						elem_ptr, elem_ptr,
						info.color_offset, info.custom_offset,
						info.uses_colors, info.uses_custom_data,
						e < info.visible_count, info.mesh_local_aabb,
						r_records[slot], r_attrs[slot]);
			}
		}
	}

	// Expand 2-level Nanite Assemblies matching instance_cull.glsl ASSEMBLY_EXPAND.
	if (are_assemblies_enabled() && active_assembly_leaf_capacity > 0u) {
		const uint32_t source_count = used_slots;
		for (uint32_t idx = 0; idx < source_count; idx++) {
			const Record &src = r_records[idx];
			if ((src.flags & (RECORD_VALID | RECORD_VISIBLE | RECORD_HAS_ASSEMBLY)) != (RECORD_VALID | RECORD_VISIBLE | RECORD_HAS_ASSEMBLY) ||
					(src.flags & RECORD_MULTIMESH_PARENT) != 0u) {
				continue;
			}
			const MeshInfo *info = get_mesh_info(src.virtual_mesh);
			if (info == nullptr || !info->has_assembly) {
				continue;
			}
			Transform3D src_xform;
			for (int r = 0; r < 3; r++) {
				src_xform.basis.rows[r][0] = src.transform[r][0];
				src_xform.basis.rows[r][1] = src.transform[r][1];
				src_xform.basis.rows[r][2] = src.transform[r][2];
				src_xform.origin[r] = src.transform[r][3];
			}
			LocalVector<AssemblyLeafCPU> leaves;
			mesh_get_assembly_expanded_parts_cpu(info->mesh, src_xform, leaves);
			const InstanceAttributes src_attr = r_attrs[idx];
			const uint32_t base_bucket_slot = src.bucket_base >> 5u;
			for (uint32_t l = 0; l < leaves.size(); l++) {
				const AssemblyLeafCPU &leaf = leaves[l];
				if (leaf.part_mesh_index == UINT32_MAX) {
					continue;
				}
				Record out_rec = src;
				out_rec.flags &= ~RECORD_HAS_ASSEMBLY;
				out_rec.flags |= RECORD_ASSEMBLY_PART;
				out_rec.virtual_mesh = leaf.part_mesh_index;
				const uint32_t bslot = base_bucket_slot + leaf.unique_part_index;
				out_rec.bucket_base = bslot * 32u;
				if (bslot < bucket_surface_masks.size()) {
					out_rec.surface_mask = bucket_surface_masks[bslot].surface_mask;
					out_rec.pad0 = bucket_surface_masks[bslot].shadow_surface_mask;
					out_rec.pad1[0] = bucket_surface_masks[bslot].cull_disabled_mask;
					out_rec.pad1[1] = bucket_surface_masks[bslot].cull_front_mask;
				}
				for (int r = 0; r < 3; r++) {
					out_rec.transform[r][0] = leaf.world_transform.basis.rows[r][0];
					out_rec.transform[r][1] = leaf.world_transform.basis.rows[r][1];
					out_rec.transform[r][2] = leaf.world_transform.basis.rows[r][2];
					out_rec.transform[r][3] = leaf.world_transform.origin[r];
					out_rec.prev_transform[r][0] = out_rec.transform[r][0];
					out_rec.prev_transform[r][1] = out_rec.transform[r][1];
					out_rec.prev_transform[r][2] = out_rec.transform[r][2];
					out_rec.prev_transform[r][3] = out_rec.transform[r][3];
				}
				const Vector3 scale = leaf.world_transform.basis.get_scale();
				const float max_scale = MAX(scale.x, MAX(scale.y, scale.z));
				if (basis_has_non_uniform_scale(leaf.world_transform.basis)) {
					out_rec.flags |= RECORD_NON_UNIFORM_SCALE;
				} else {
					out_rec.flags &= ~RECORD_NON_UNIFORM_SCALE;
				}
				const AABB world_aabb = leaf.world_transform.xform(leaf.local_aabb);
				out_rec.world_aabb_min[0] = world_aabb.position.x;
				out_rec.world_aabb_min[1] = world_aabb.position.y;
				out_rec.world_aabb_min[2] = world_aabb.position.z;
				out_rec.world_aabb_min[3] = max_scale;
				out_rec.world_aabb_max[0] = world_aabb.position.x + world_aabb.size.x;
				out_rec.world_aabb_max[1] = world_aabb.position.y + world_aabb.size.y;
				out_rec.world_aabb_max[2] = world_aabb.position.z + world_aabb.size.z;
				for (int k = 0; k < 3; k++) {
					out_rec.prev_world_aabb_min[k] = out_rec.world_aabb_min[k];
					out_rec.prev_world_aabb_max[k] = out_rec.world_aabb_max[k];
				}
				r_records.push_back(out_rec);
				r_attrs.push_back(src_attr);
			}
		}
	}
}
