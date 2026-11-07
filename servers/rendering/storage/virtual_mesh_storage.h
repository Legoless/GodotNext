/**************************************************************************/
/*  virtual_mesh_storage.h                                                */
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

#include "core/math/vector2.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/safe_refcount.h"
#include "servers/rendering/rendering_server_types.h"

// GodotNext: nextgeo_instance_records (#75, plan §6.3, §9), nextgeo_residency (#78)
//
// The persistent GPU instance/mesh records and fixed-slot streaming page arena
// that NextGeo's cull and raster read. Record layout is a std430 contract:
// change it in the GLSL copy (shaders/nextgeo/nextgeo_records_inc.glsl) in the
// same commit.
class RendererVirtualMeshStorage {
public:
	// Per-instance record. 224 B, 256 B under REAL_T_IS_DOUBLE (plan §6.3).
	enum RecordFlags : uint32_t {
		RECORD_VALID = 1u << 0,
		RECORD_VISIBLE = 1u << 1,
		RECORD_CAST_SHADOW_OFF = 0u << 2,
		RECORD_CAST_SHADOW_ON = 1u << 2,
		RECORD_CAST_SHADOW_DOUBLE_SIDED = 2u << 2,
		RECORD_CAST_SHADOW_MASK = 3u << 2,
		RECORD_MOVED = 1u << 4,
		RECORD_TELEPORTED = 1u << 5,
		RECORD_HAS_SKELETON = 1u << 6,
		RECORD_HAS_INSTANCE_UNIFORMS = 1u << 7,
		RECORD_RT_STATIC = 1u << 8,
		RECORD_RT_SKINNED = 1u << 9,
		RECORD_RT_ALPHA_AS_OPAQUE = 1u << 10,
		RECORD_RT_FAR = 1u << 11,
	};

	// gi_flags.
	enum GIFlags : uint32_t {
		GI_MODE_NONE = 0u << 0,
		GI_MODE_STATIC = 1u << 0,
		GI_MODE_DYNAMIC = 2u << 0,
		GI_MODE_MASK = 3u << 0,
		GI_LIGHTMAPPED = 1u << 2,
	};

	struct Record {
		float transform[3][4] = { { 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f } };
		float prev_transform[3][4] = { { 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f } };
		float world_aabb_min[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // w = max scale.
		float world_aabb_max[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // w = lod_bias.
		float prev_world_aabb_min[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		float prev_world_aabb_max[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		uint32_t virtual_mesh = UINT32_MAX;
		uint32_t page_table_offset = 0;
		uint32_t surface_mask = 0;
		uint32_t bucket_base = 0;
		uint32_t layer_mask = 0xFFFFFFFFu;
		uint32_t flags = 0;
		uint32_t gi_flags = 0;
		uint32_t pad0 = 0;
		uint32_t tlas_instance_index = UINT32_MAX;
		uint32_t blas_handle = UINT32_MAX;
		uint32_t rt_arena_chunk = UINT32_MAX;
		uint32_t rt_arena_offset = 0;
		uint32_t src_page_table = UINT32_MAX;
		uint32_t card_set = UINT32_MAX;
		uint32_t pad1[2] = { 0, 0 };
#ifdef REAL_T_IS_DOUBLE
		float model_precision[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		float prev_model_precision[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
#endif
	};

	// Per-mesh record, 32 B. It exists only while the mesh has live instances.
	struct MeshRecord {
		uint32_t bvh_node_offset = 0;
		uint32_t bvh_level_count = 0;
		uint32_t cluster_offset = 0;
		uint32_t cluster_count = 0;
		uint32_t group_offset = 0;
		uint32_t page_table_offset = 0;
		uint32_t page_count = 0;
		uint32_t surface_count = 0;
	};

	static constexpr uint32_t RECORD_BYTES = sizeof(Record);
	static constexpr uint32_t MESH_RECORD_BYTES = sizeof(MeshRecord);
	static constexpr uint32_t INITIAL_RECORD_CAPACITY = 4096;
	static constexpr uint32_t INVALID_PAGE_SLOT = 0xFFFFFFFFu;
	static constexpr uint32_t DEFAULT_GEOMETRY_CACHE_MB = 512;
	static constexpr uint32_t DEFAULT_PAGE_UPLOAD_KIB_PER_FRAME = 4096;
	static constexpr uint64_t DEFAULT_IO_DELAY_SEED = 0x6E676D3738ULL;

	struct UploadRun {
		uint32_t first = 0;
		uint32_t count = 0;
	};

	// GodotNext: nextgeo_visibility_raster (#83). 16 B per-cluster GPU lookup
	// pointing into the global page arena for vertex-pulling hardware raster.
	struct ClusterGPUDesc {
		uint32_t pos_word_offset = 0;
		uint32_t tri_byte_offset = 0;
		uint32_t stride_and_counts = 0; // bits 0..7 = float stride, 8..15 = verts, 16..23 = tris, 24..31 = vertex_layout.
		uint32_t surface_and_page = 0; // bits 0..15 = surface_id, 16..31 = page_id.
	};

	// GodotNext: nextgeo_residency (#78). CPU mirror of resident cluster
	// refinement metadata so CPU_cut_hash matches cluster_cull.glsl bit-for-bit.
	struct ClusterRefineInfo {
		float group_lod[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		float group_error = 0.0f;
		int32_t refined_group = -1;
		bool valid = false;
	};

	// GodotNext: nextgeo_residency (#78). Main-view page request entry.
	struct PageRequest {
		uint32_t mesh_index = UINT32_MAX;
		uint32_t page_id = UINT32_MAX;
		bool visible = true;
		float distance = 0.0f;
		float px_error = 0.0f;
	};

	struct MeshInfo {
		RenderingServerTypes::VirtualGeometryDesc desc;
		MeshRecord record;
		RID mesh;
		uint32_t mesh_index = UINT32_MAX;
		uint64_t generation = 1;
		uint32_t live_instances = 0;
		bool record_dirty = false;
		// GodotNext: nextgeo_gpu_cull (#76). The mesh's DAG blob in the global
		// DAG buffer; the mesh record's offsets are relative to its base.
		uint32_t dag_offset = 0;
		uint32_t dag_size = 0;
		// GodotNext: nextgeo_visibility_raster (#83) / nextgeo_residency (#78).
		uint32_t cluster_table_offset = 0;
		uint32_t cluster_table_size = 0;
		Vector<uint8_t> cluster_blob;

		// Residency & streaming (#78).
		uint32_t total_page_count = 0;
		uint32_t root_page_count = 0;
		uint32_t group_count = 0;
		uint32_t layout_stride = 0;
		uint32_t vertex_layout = 0;
		LocalVector<uint32_t> group_page_ids;
		LocalVector<LocalVector<uint32_t>> group_pages; // Distinct pages holding clusters of group g.
		LocalVector<uint32_t> page_first_cluster;
		LocalVector<uint32_t> page_cluster_counts;
		LocalVector<uint32_t> page_to_slot; // Fixed arena slot, or INVALID_PAGE_SLOT (0xFFFFFFFFu).
		LocalVector<uint32_t> resident_bitset;
		LocalVector<uint32_t> refine_ok_bitset;
		LocalVector<LocalVector<uint32_t>> page_parents;
		LocalVector<LocalVector<uint32_t>> page_children;
		LocalVector<uint32_t> page_topo_depth;
		LocalVector<uint32_t> resident_child_count;
		LocalVector<ClusterRefineInfo> cluster_refine;
		uint32_t residency_refine_word_offset = 0;
		uint32_t residency_page_table_word_offset = 0;
		bool page_dag_ready = false;
		bool page_dag_scanning = false;
	};

	RendererVirtualMeshStorage();
	virtual ~RendererVirtualMeshStorage();

	// Mesh records.
	void mesh_set_virtual_geometry(RID p_mesh, const RenderingServerTypes::VirtualGeometryDesc &p_desc);
	void mesh_clear_virtual_geometry(RID p_mesh, uint32_t p_reason);
	bool mesh_has_virtual_geometry(RID p_mesh) const;
	// GodotNext: nextgeo_exclusive_dispatch (#79). The surface is inside the
	// mesh's eligible_surface_mask (index < 32).
	bool mesh_surface_is_virtual(RID p_mesh, uint32_t p_surface) const;
	bool mesh_is_stripped_to_coarse(RID p_mesh) const;
	// 0 while the mesh still has VGD or never had it; the clear reason after
	// a mesh_clear_virtual_geometry.
	uint32_t mesh_clear_reason(RID p_mesh) const;
	bool is_nextgeo_draw_ready() const { return nextgeo_draw_ready; }
	void set_nextgeo_draw_ready(bool p_ready) { nextgeo_draw_ready = p_ready; }
	void nextgeo_reset_counters() {
		nextgeo_double_dispatch = 0;
		nextgeo_missing = 0;
	}
	void nextgeo_count_double_dispatch() { nextgeo_double_dispatch++; }
	void nextgeo_count_missing() { nextgeo_missing++; }
	uint32_t nextgeo_get_double_dispatch() const { return nextgeo_double_dispatch; }
	uint32_t nextgeo_get_missing() const { return nextgeo_missing; }

	// Instance records.
	uint32_t instance_record_allocate(RID p_instance, RID p_mesh);
	void instance_record_free(uint32_t p_record);
	void instance_record_set_transform(uint32_t p_record, const Transform3D &p_transform, const AABB &p_world_aabb, bool p_visible, bool p_teleport = false, float p_lod_bias = 0.0f);
	void instance_record_set_state(uint32_t p_record, uint32_t p_flags);
	void instance_record_set_layer_mask(uint32_t p_record, uint32_t p_layer_mask);
	void instance_record_set_gi_flags(uint32_t p_record, uint32_t p_gi_flags);
	void instance_record_set_surface_mask(uint32_t p_record, uint32_t p_surface_mask);
	// tlas_instance_index, blas_handle, rt_arena_chunk, rt_arena_offset, src_page_table, card_set.
	void instance_record_set_rt(uint32_t p_record, const uint32_t p_rt_fields[6]);

	// Per frame: advance the free list and the temporal history, process page
	// streaming uploads under the per-frame KiB cap, then upload dirty records.
	void begin_frame();
	void flush();

	// --- Residency & Streaming (#78) ---
	void set_page_arena_bytes_override(uint64_t p_bytes);
	void set_page_upload_bytes_per_frame_override(uint64_t p_bytes);
	void set_io_delay_seed(uint64_t p_seed);
	uint64_t get_io_delay_seed() const { return io_delay_seed; }
	static uint64_t compute_debug_io_delay_usec(uint64_t p_seed, uint64_t p_request_index, Vector2 p_delay_ms);
	static Vector2 parse_debug_io_delay_setting(const Variant &p_value);

	// Set explicit page-parent edges and/or in-memory non-root page payloads (for unit tests).
	void mesh_set_page_parents(RID p_mesh, const LocalVector<LocalVector<uint32_t>> &p_page_parents);
	void mesh_set_in_memory_pages(RID p_mesh, const Vector<Vector<uint8_t>> &p_pages);

	// Submit page requests (only p_is_main_view == true schedules loads; non-main views are rejected and counted).
	void submit_page_requests(const PageRequest *p_requests, uint32_t p_count, bool p_is_main_view);
	// Mark resident page slots reported as drawn by the main-view GPU cluster cull.
	void touch_used_page_slots(const uint32_t *p_used_words, uint32_t p_word_count);
	// Wait for all currently in-flight worker IO tasks (p_cancel=true aborts sleeps/reads on shutdown).
	void wait_for_streaming_tasks(bool p_cancel = false);

	bool mesh_is_page_resident(RID p_mesh, uint32_t p_page_id) const;
	uint32_t mesh_get_page_slot(RID p_mesh, uint32_t p_page_id) const;
	bool mesh_is_group_refine_ok(uint32_t p_mesh_index, uint32_t p_group_index) const;
	bool mesh_is_group_refine_ok_rid(RID p_mesh, uint32_t p_group_index) const;

	// CPU mirror of refine_ok[g]: root groups are always ok; non-root group g is ok iff every
	// page holding a cluster whose parent_lod is group g is resident.
	static void compute_refine_ok_bits(uint32_t p_group_count, const LocalVector<LocalVector<uint32_t>> &p_group_pages, uint32_t p_root_page_count, const uint8_t *p_page_resident, uint32_t p_page_count, LocalVector<uint32_t> &r_refine_ok_bits);

	// Residency events (#78 step 7): record slots whose virtual mesh changed residency this frame (#82 consumes them).
	const LocalVector<uint32_t> &get_residency_changed_records() const { return residency_changed_records; }

	// Residency & streaming counters / diagnostics.
	uint64_t get_child_resident_without_parent_count() const { return child_resident_without_parent; }
	uint64_t get_evictions_with_resident_child_count() const { return evictions_with_resident_child; }
	uint64_t get_non_main_view_request_count() const { return non_main_view_requests; }
	uint64_t get_total_page_uploads() const { return total_page_uploads; }
	uint64_t get_total_page_evictions() const { return total_page_evictions; }
	uint64_t get_total_page_upload_bytes() const { return total_page_upload_bytes; }
	uint32_t get_last_frame_upload_bytes() const { return last_frame_upload_bytes; }
	uint32_t get_resident_page_count() const { return occupied_slot_count; }
	uint32_t get_pinned_page_count() const { return pinned_slot_count; }
	uint32_t get_page_slot_count() const { return uint32_t(page_slots.size()); }
	uint32_t get_page_slot_size_bytes() const { return slot_page_size; }
	uint64_t get_page_arena_allocation_bytes() const { return uint64_t(page_capacity); }
	uint64_t get_dag_allocation_bytes() const { return uint64_t(dag_capacity); }
	double get_last_page_upload_ms() const { return last_page_upload_ms; }
	Vector<uint64_t> get_recorded_io_delays_usec() const;
	void clear_recorded_io_delays();

	// Introspection (doctests and the debug overlay).
	const Record &get_record(uint32_t p_record) const;
	const MeshRecord &get_mesh_record(RID p_mesh) const;
	uint32_t get_record_capacity() const { return record_capacity; }
	uint32_t get_live_record_count() const { return live_records; }
	uint32_t get_frame() const { return frame; }
	const HashMap<RID, MeshInfo> &get_meshes() const { return meshes; }
	const MeshInfo *get_mesh_info(uint32_t p_mesh_index) const;
	virtual RID get_record_buffer() const { return RID(); }
	virtual RID get_mesh_record_buffer() const { return RID(); }
	virtual RID get_dag_buffer() const { return RID(); }
	virtual RID get_page_buffer() const { return RID(); }
	virtual RID get_cluster_buffer() const { return RID(); }
	virtual RID get_residency_buffer() const { return RID(); }
	uint32_t get_record_count() const { return used_slots; }
	uint32_t get_max_bvh_levels() const;

protected:
	virtual void _upload_records(uint32_t p_first, uint32_t p_count) = 0;
	virtual void _upload_mesh_records(uint32_t p_first, uint32_t p_count) = 0;
	// GodotNext: nextgeo_gpu_cull (#76). The DAG buffer lives in the renderer.
	virtual bool _resize_dag(uint32_t p_bytes) { return false; }
	virtual void _upload_dag(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) {}
	virtual void _reset_dag() {}
	// GodotNext: nextgeo_visibility_raster (#83) / nextgeo_residency (#78).
	virtual bool _resize_pages(uint32_t p_bytes) { return true; }
	virtual void _upload_pages(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) {}
	virtual void _reset_pages() {}
	virtual bool _resize_clusters(uint32_t p_bytes) { return true; }
	virtual void _upload_clusters(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) {}
	virtual void _reset_clusters() {}
	// GodotNext: nextgeo_residency (#78). GPU residency table (refine_ok + page_to_slot).
	virtual bool _resize_residency(uint32_t p_bytes) { return true; }
	virtual void _upload_residency(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) {}
	virtual void _reset_residency() {}
	// Renderer-specific bookkeeping after a record slot is reused.
	virtual void _record_reused(uint32_t p_record) {}
	// Renderer-specific refusal (for example an out-of-memory RD buffer).
	virtual bool _can_allocate_record() const { return true; }

	// One CPU-side slot. A freed slot is reusable on the next frame.
	struct SlotState {
		bool allocated = false;
		bool dirty = false;
		bool dirty_last = false;
		bool initialized = false;
		bool rewritten = false;
		uint32_t free_frame = UINT32_MAX;
	};

	struct PageSlot {
		RID mesh;
		uint32_t page_id = UINT32_MAX;
		uint64_t last_used_frame = 0;
		uint32_t lru_prev = UINT32_MAX;
		uint32_t lru_next = UINT32_MAX;
		bool occupied = false;
		bool pinned = false;
		bool in_lru = false;
	};

	struct QueuedPageKey {
		RID mesh;
		uint32_t page_id = 0;
		bool operator==(const QueuedPageKey &p_other) const {
			return mesh == p_other.mesh && page_id == p_other.page_id;
		}
	};

	struct QueuedPageKeyHasher {
		static _FORCE_INLINE_ uint32_t hash(const QueuedPageKey &p_key) {
			uint32_t h = hash_murmur3_one_64(p_key.mesh.get_id());
			return hash_fmix32(hash_murmur3_one_32(p_key.page_id, h));
		}
	};

	struct PrioritizedPageRequest {
		RID mesh;
		uint32_t mesh_index = UINT32_MAX;
		uint64_t generation = 0;
		uint32_t page_id = 0;
		uint32_t topo_depth = 0;
		bool visible = true;
		float distance = 0.0f;
		float px_error = 0.0f;
	};

	struct StagedPage {
		RID mesh;
		uint64_t generation = 0;
		uint32_t page_id = 0;
		uint32_t topo_depth = 0;
		bool visible = true;
		float distance = 0.0f;
		float px_error = 0.0f;
		Vector<uint8_t> raw;
	};

	struct IOBatchItem {
		RID mesh;
		uint64_t generation = 0;
		String ngm_path;
		uint32_t page_id = 0;
		uint32_t topo_depth = 0;
		bool visible = true;
		float distance = 0.0f;
		float px_error = 0.0f;
		uint64_t request_index = 0;
		uint64_t delay_seed = 0;
		Vector2 delay_ms;
		Vector<uint8_t> in_memory_raw;
		bool has_in_memory = false;
	};

	struct IODagScanItem {
		RID mesh;
		uint64_t generation = 0;
		String ngm_path;
		uint32_t total_page_count = 0;
		uint32_t group_count = 0;
		LocalVector<uint32_t> group_page_ids;
		LocalVector<LocalVector<uint32_t>> page_parents;
		bool ok = false;
	};

	struct IOWorkerBatch {
		RendererVirtualMeshStorage *owner = nullptr;
		LocalVector<IODagScanItem> dag_scans;
		LocalVector<IOBatchItem> items;
		LocalVector<StagedPage> results;
		LocalVector<uint64_t> delays_usec;
		WorkerThreadPool::TaskID task_id = WorkerThreadPool::INVALID_TASK_ID;
		SafeFlag done;
	};

	void _ensure_capacity(uint32_t p_records);
	void _mark_dirty(uint32_t p_record);
	uint32_t _alloc_dag(uint32_t p_size);
	void _free_dag(uint32_t p_offset, uint32_t p_size);
	uint32_t _alloc_clusters(uint32_t p_size);
	void _free_clusters(uint32_t p_offset, uint32_t p_size);
	MeshRecord _parse_dag(const Vector<uint8_t> &p_dag, uint32_t p_base);
	void _write_transform(Record &r_record, const Transform3D &p_transform, const AABB &p_world_aabb, float p_lod_bias);

	// Residency internal helpers (#78).
	uint64_t _desired_page_arena_bytes() const;
	uint64_t _desired_page_upload_budget_bytes() const;
	bool _ensure_page_arena();
	void _lru_unlink(uint32_t p_slot);
	void _lru_push_back(uint32_t p_slot);
	void _touch_page_slot(uint32_t p_slot);
	uint32_t _allocate_page_slot(bool p_pinned);
	void _free_mesh_page_slots(MeshInfo &r_info);
	bool _evict_slot(uint32_t p_slot, bool p_record_event);
	void _evict_all_non_pinned_leaf_first();
	bool _install_page_in_slot(MeshInfo &r_info, uint32_t p_page_id, uint32_t p_slot, const Vector<uint8_t> &p_raw, bool p_pinned, bool p_record_event);
	bool _all_parent_pages_resident(const MeshInfo &p_info, uint32_t p_page_id) const;
	void _rebuild_page_children_and_depths(MeshInfo &r_info);
	void _recompute_mesh_refine_ok(MeshInfo &r_info);
	void _rebuild_residency_gpu_table();
	void _sync_residency_gpu_mesh(const MeshInfo &p_info);
	void _process_streaming_frame();
	void _poll_worker_batches();
	void _dispatch_worker_batches();
	static void _io_worker_entry(void *p_userdata);

	LocalVector<Record> records;
	LocalVector<MeshRecord> mesh_records;
	LocalVector<SlotState> slot_states;
	LocalVector<uint32_t> free_now;
	LocalVector<uint32_t> free_next;

	HashMap<RID, MeshInfo> meshes;
	HashMap<RID, uint32_t> mesh_indices;
	LocalVector<RID> mesh_order; // mesh index -> RID
	HashMap<RID, uint32_t> mesh_clear_reasons;
	HashMap<RID, Vector<Vector<uint8_t>>> in_memory_mesh_pages;
	bool nextgeo_draw_ready = false;
	uint32_t nextgeo_double_dispatch = 0;
	uint32_t nextgeo_missing = 0;
	LocalVector<RID> mesh_rids;
	LocalVector<uint8_t> mesh_record_dirty;

	uint32_t record_capacity = 0;
	uint32_t used_slots = 0;
	uint32_t dag_used = 0;
	uint32_t dag_capacity = 0;
	uint32_t page_capacity = 0;
	uint32_t cluster_used = 0;
	uint32_t cluster_capacity = 0;
	struct DAGFree {
		uint32_t offset = 0;
		uint32_t size = 0;
	};
	LocalVector<DAGFree> dag_free;
	LocalVector<DAGFree> cluster_free;
	uint32_t live_records = 0;
	uint32_t frame = 0;
	uint32_t streamed_frame = UINT32_MAX;
	bool mesh_records_dirty = false;
	// Teleport threshold, in meters. A move larger than this sets prev = cur.
	float teleport_distance = 100.0f;

	// Residency & streaming state (#78).
	uint32_t slot_page_size = 64u * 1024u;
	uint64_t page_arena_override_bytes = 0;
	uint64_t page_upload_override_bytes = 0;
	LocalVector<PageSlot> page_slots;
	LocalVector<uint32_t> free_page_slots;
	LocalVector<uint32_t> pending_used_slots;
	uint32_t lru_head = UINT32_MAX;
	uint32_t lru_tail = UINT32_MAX;
	uint32_t occupied_slot_count = 0;
	uint32_t pinned_slot_count = 0;

	LocalVector<uint32_t> residency_words;
	uint32_t residency_capacity_bytes = 0;
	bool residency_layout_dirty = true;
	bool residency_data_dirty = false;

	HashMap<QueuedPageKey, PrioritizedPageRequest, QueuedPageKeyHasher> pending_requests;
	HashSet<QueuedPageKey, QueuedPageKeyHasher> in_flight_pages;
	LocalVector<StagedPage> staged_pages;
	LocalVector<IOWorkerBatch *> active_batches;
	SafeFlag shutting_down;

	HashSet<uint32_t> residency_changed_meshes;
	LocalVector<uint32_t> residency_changed_records;

	uint64_t io_delay_seed = DEFAULT_IO_DELAY_SEED;
	uint64_t next_io_request_index = 0;
	mutable Mutex delay_history_mutex;
	LocalVector<uint64_t> recorded_io_delays_usec;

	uint64_t child_resident_without_parent = 0;
	uint64_t evictions_with_resident_child = 0;
	uint64_t non_main_view_requests = 0;
	uint64_t total_page_uploads = 0;
	uint64_t total_page_evictions = 0;
	uint64_t total_page_upload_bytes = 0;
	uint32_t last_frame_upload_bytes = 0;
	double last_page_upload_ms = 0.0;
	double last_cpu_page_upload_ms = 0.0;
};
