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

namespace {

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
	_reset_dag();
	_reset_pages();
	_reset_clusters();
	_reset_residency();
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
		_free_mesh_page_slots(*old_info);
		if (old_info->dag_size != 0) {
			_free_dag(old_info->dag_offset, old_info->dag_size);
		}
		if (old_info->cluster_table_size != 0) {
			_free_clusters(old_info->cluster_table_offset, old_info->cluster_table_size);
		}
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

	// Resolve vertex layout, page count and page slot size from VGD / .ngm header.
	info.vertex_layout = p_desc.vertex_layout;
	uint32_t ngm_page_count = p_desc.page_count;
	uint32_t ngm_page_size = p_desc.page_size;
	if ((ngm_page_count == 0 || info.vertex_layout == 0 || ngm_page_size == 0) && !p_desc.ngm_path.is_empty() && FileAccess::exists(p_desc.ngm_path)) {
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
				info.record.page_table_offset = info.cluster_table_offset / sizeof(ClusterGPUDesc);
				info.record.page_count = pinned_ok;
				if (info.record.cluster_count == 0) {
					info.record.cluster_count = max_local_cluster_end;
				}
			} else {
				_free_mesh_page_slots(info);
				info.cluster_blob.clear();
				info.cluster_refine.clear();
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
	if (RSG::mesh_storage != nullptr) {
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
	MeshInfo *info = meshes.getptr(p_mesh);
	if (info != nullptr) {
		_free_mesh_page_slots(*info);
		_free_dag(info->dag_offset, info->dag_size);
		_free_clusters(info->cluster_table_offset, info->cluster_table_size);
	}
	meshes.erase(p_mesh);

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
		if (!slot_states[slot].allocated || records[slot].virtual_mesh != *index) {
			continue;
		}
		records[slot].virtual_mesh = UINT32_MAX;
		records[slot].surface_mask = 0;
		_mark_dirty(slot);
	}
	// Keep the reason for the routing fallback overlay (#79); the record store
	// only needs to know the mesh is gone.
	if (p_reason != RenderingServerTypes::VIRTUAL_GEOMETRY_CLEAR_NONE) {
		mesh_clear_reasons[p_mesh] = p_reason;
	} else {
		mesh_clear_reasons.erase(p_mesh);
	}
	if (RSG::mesh_storage != nullptr) {
		RSG::mesh_storage->mesh_notify_dependency_changed(p_mesh);
	}
}

bool RendererVirtualMeshStorage::mesh_has_virtual_geometry(RID p_mesh) const {
	return meshes.has(p_mesh);
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

uint32_t RendererVirtualMeshStorage::mesh_clear_reason(RID p_mesh) const {
	const uint32_t *reason = mesh_clear_reasons.getptr(p_mesh);
	return reason != nullptr ? *reason : 0;
}

uint32_t RendererVirtualMeshStorage::instance_record_allocate(RID p_instance, RID p_mesh) {
	(void)p_instance;
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
	record.bucket_base = slot * 32u;
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

void RendererVirtualMeshStorage::instance_record_set_surface_mask(uint32_t p_record, uint32_t p_surface_mask) {
	if (p_record >= used_slots || !slot_states[p_record].allocated) {
		return;
	}
	if (records[p_record].surface_mask == p_surface_mask) {
		return;
	}
	records[p_record].surface_mask = p_surface_mask;
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
}

void RendererVirtualMeshStorage::flush() {
	if (streamed_frame != frame) {
		_process_streaming_frame();
		streamed_frame = frame;
	} else if (residency_layout_dirty) {
		_rebuild_residency_gpu_table();
	}

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

// ============================================================================
// Residency & Streaming (#78)
// ============================================================================

void RendererVirtualMeshStorage::set_page_arena_bytes_override(uint64_t p_bytes) {
	page_arena_override_bytes = p_bytes;
	if (!meshes.is_empty()) {
		_ensure_page_arena();
	}
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
	if (page_capacity == desired_bytes && !page_slots.is_empty() && uint64_t(page_slots.size()) * uint64_t(slot_page_size) == desired_bytes) {
		return true;
	}

	// Evict non-pinned streaming pages leaf-first before resizing the backing buffer.
	_evict_all_non_pinned_leaf_first();

	uint32_t required_pinned_slots = 0;
	for (const KeyValue<RID, MeshInfo> &E : meshes) {
		for (uint32_t p = 0; p < E.value.root_page_count; p++) {
			uint32_t c_count = 0;
			if (int(p) < E.value.desc.root_pages.size() && _is_valid_raw_page(E.value.desc.root_pages[int(p)], c_count)) {
				required_pinned_slots++;
			}
		}
	}

	const uint64_t min_bytes = uint64_t(MAX(required_pinned_slots, 1u)) * uint64_t(slot_page_size);
	const uint64_t target_bytes = MIN<uint64_t>(MAX(desired_bytes, min_bytes), 0x7FFF0000ull);
	const uint32_t total_slots = uint32_t(target_bytes / uint64_t(slot_page_size));
	const uint32_t actual_bytes = total_slots * slot_page_size;
	if (!_resize_pages(actual_bytes)) {
		return false;
	}

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
		// Pinned root pages are never dropped: grow the arena if every slot is already pinned.
		const uint64_t grown = uint64_t(page_slots.size() + 16u) * uint64_t(slot_page_size);
		if (grown <= 0x7FFF0000ull) {
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
				if (p_record_event && info->cluster_table_size > 0) {
					_upload_clusters(
							info->cluster_table_offset + base_cluster * sizeof(ClusterGPUDesc),
							info->cluster_blob.ptr() + base_cluster * sizeof(ClusterGPUDesc),
							count * sizeof(ClusterGPUDesc));
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
	_upload_pages(page_global, p_raw.ptr(), uint32_t(p_raw.size()));

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
		if (p_record_event && r_info.cluster_table_size > 0 && page_clusters > 0) {
			_upload_clusters(
					r_info.cluster_table_offset + base_cluster * sizeof(ClusterGPUDesc),
					r_info.cluster_blob.ptr() + base_cluster * sizeof(ClusterGPUDesc),
					page_clusters * sizeof(ClusterGPUDesc));
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
	residency_changed_meshes.clear();
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

	// Publish instance record slots whose virtual mesh changed its resident page set (#78 step 7).
	if (!residency_changed_meshes.is_empty()) {
		for (uint32_t slot = 0; slot < used_slots; slot++) {
			if (slot_states[slot].allocated && residency_changed_meshes.has(records[slot].virtual_mesh)) {
				residency_changed_records.push_back(slot);
			}
		}
	}
}
