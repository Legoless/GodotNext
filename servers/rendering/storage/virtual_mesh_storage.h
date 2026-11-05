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
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,         */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "servers/rendering/rendering_server_types.h"

// GodotNext: nextgeo_instance_records (#75, plan §6.3, §9)
//
// The persistent GPU instance/mesh records that NextGeo's cull and raster
// read. Record layout is a std430 contract: change it in the GLSL copy
// (shaders/nextgeo/nextgeo_records_inc.glsl) in the same commit.
//
// The base class owns the CPU record store: allocation, free-list reuse,
// prev/cur temporal history and the dirty-run upload schedule. Renderer
// backends implement _upload_records() and _upload_mesh_records().
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

	struct UploadRun {
		uint32_t first = 0;
		uint32_t count = 0;
	};

	struct MeshInfo {
		RenderingServerTypes::VirtualGeometryDesc desc;
		MeshRecord record;
		RID mesh;
		uint32_t live_instances = 0;
		bool record_dirty = false;
		// GodotNext: nextgeo_gpu_cull (#76). The mesh's DAG blob in the global
		// DAG buffer; the mesh record's offsets are relative to its base.
		uint32_t dag_offset = 0;
		uint32_t dag_size = 0;
	};

	RendererVirtualMeshStorage();
	virtual ~RendererVirtualMeshStorage();

	// Mesh records.
	void mesh_set_virtual_geometry(RID p_mesh, const RenderingServerTypes::VirtualGeometryDesc &p_desc);
	void mesh_clear_virtual_geometry(RID p_mesh, uint32_t p_reason);
	bool mesh_has_virtual_geometry(RID p_mesh) const;

	// Instance records.
	uint32_t instance_record_allocate(RID p_instance, RID p_mesh);
	void instance_record_free(uint32_t p_record);
	void instance_record_set_transform(uint32_t p_record, const Transform3D &p_transform, const AABB &p_world_aabb, bool p_visible, bool p_teleport = false, float p_lod_bias = 0.0f);
	void instance_record_set_state(uint32_t p_record, uint32_t p_flags);
	void instance_record_set_layer_mask(uint32_t p_record, uint32_t p_layer_mask);
	void instance_record_set_gi_flags(uint32_t p_record, uint32_t p_gi_flags);
	// tlas_instance_index, blas_handle, rt_arena_chunk, rt_arena_offset, src_page_table, card_set.
	void instance_record_set_rt(uint32_t p_record, const uint32_t p_rt_fields[6]);

	// Per frame: advance the free list and the temporal history, then upload
	// only the records that changed this frame (plus one prev rewrite for a
	// record that changed last frame and not this one).
	void begin_frame();
	void flush();

	// Introspection (doctests and the debug overlay).
	const Record &get_record(uint32_t p_record) const;
	const MeshRecord &get_mesh_record(RID p_mesh) const;
	uint32_t get_record_capacity() const { return record_capacity; }
	uint32_t get_live_record_count() const { return live_records; }
	uint32_t get_frame() const { return frame; }
	const HashMap<RID, MeshInfo> &get_meshes() const { return meshes; }

protected:
	virtual void _upload_records(uint32_t p_first, uint32_t p_count) = 0;
	virtual void _upload_mesh_records(uint32_t p_first, uint32_t p_count) = 0;
	// GodotNext: nextgeo_gpu_cull (#76). The DAG buffer lives in the renderer.
	virtual bool _resize_dag(uint32_t p_bytes) { return false; }
	virtual void _upload_dag(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) {}
	virtual void _reset_dag() {}
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

	void _ensure_capacity(uint32_t p_records);
	void _mark_dirty(uint32_t p_record);
	uint32_t _alloc_dag(uint32_t p_size);
	void _free_dag(uint32_t p_offset, uint32_t p_size);
	MeshRecord _parse_dag(const Vector<uint8_t> &p_dag, uint32_t p_base);
	void _write_transform(Record &r_record, const Transform3D &p_transform, const AABB &p_world_aabb, float p_lod_bias);

	LocalVector<Record> records;
	LocalVector<MeshRecord> mesh_records;
	LocalVector<SlotState> slot_states;
	LocalVector<uint32_t> free_now;
	LocalVector<uint32_t> free_next;

	HashMap<RID, MeshInfo> meshes;
	HashMap<RID, uint32_t> mesh_indices;
	LocalVector<RID> mesh_rids;
	LocalVector<uint8_t> mesh_record_dirty;

	uint32_t record_capacity = 0;
	uint32_t used_slots = 0;
	uint32_t dag_used = 0;
	uint32_t dag_capacity = 0;
	struct DAGFree {
		uint32_t offset = 0;
		uint32_t size = 0;
	};
	LocalVector<DAGFree> dag_free;
	uint32_t live_records = 0;
	uint32_t frame = 0;
	bool mesh_records_dirty = false;
	// Teleport threshold, in metres. A move larger than this sets prev = cur.
	float teleport_distance = 100.0f;
};
