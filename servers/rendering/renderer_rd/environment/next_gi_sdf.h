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
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/math/transform_3d.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/resources/3d/mesh_distance_field.h"

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

// GodotNext: ps_mesh_sdf_atlas (#185)
//
// The GPU side of the pool: one R8 brick buffer (slot * 512 B) plus the
// per-mesh mip page tables. The RD in this tree updates whole texture layers
// only, so the atlas is a linear brick buffer rather than a Texture3D; the
// logical brick layout is unchanged and shaders address it as
// slot * BRICK_VOXELS + voxel.
class NextGISDFAtlas {
public:
	struct MeshPages {
		Ref<MeshDistanceField> field;
		// One entry per brick slot of the source mip: a pool slot, or the
		// MeshSDFData::PAGE_FAR_OUTSIDE / PAGE_FAR_INSIDE sentinel.
		Vector<PackedInt32Array> page_tables;
		Vector<PackedInt32Array> slots;
		Vector<PackedInt32Array> source_tables; // CPU page tables, kept for reloads.
		Vector<uint8_t> mip_loaded; // Mip 0 is loaded at registration.
		Vector<uint64_t> mip_last_requested;
		uint32_t mip_count = 0;
		uint64_t resident_bytes = 0;
	};

	explicit NextGISDFAtlas(uint32_t p_size_mb = NextGISDFPool::DEFAULT_POOL_MB);

	// Allocates and uploads every brick of the field. Re-registering a mesh
	// releases the previous pages first. All-or-nothing.
	bool register_mesh(RID p_mesh, const Ref<MeshDistanceField> &p_field);
	void unregister_mesh(RID p_mesh);
	bool has_mesh(RID p_mesh) const;
	const MeshPages *get_pages(RID p_mesh) const;

	RID get_atlas_buffer() const { return atlas_buffer; }
	uint32_t get_pool_capacity() const { return pool.get_capacity(); }
	uint32_t get_pool_used() const { return pool.get_used_count(); }
	uint64_t get_resident_bytes() const { return resident_bytes; }
	uint64_t get_pool_size_bytes() const { return pool.get_size_bytes(); }
	uint64_t get_generation() const { return generation; }
	uint32_t get_mesh_id(RID p_mesh) const;
	uint32_t get_mip_count(uint32_t p_mesh_id) const;
	// Streams the requested mips in, up to p_budget_bytes (the caller passes
	// 4 MiB), and evicts mips idle for 60 frames. p_requests is one mip per
	// instance in the instance table; sdf_id UINT32_MAX entries are skipped.
	void stream_mips(const LocalVector<uint32_t> &p_sdf_ids, const LocalVector<uint32_t> &p_mips, uint64_t p_frame, uint64_t p_budget_bytes);
	void free_all();

private:
	RID _ensure_buffer();
	void _release_pages(MeshPages &r_pages);

	NextGISDFPool pool;
	RID atlas_buffer;
	HashMap<RID, MeshPages> meshes;
	HashMap<RID, uint32_t> mesh_ids;
	LocalVector<RID> mesh_order;
	uint64_t resident_bytes = 0;
	uint64_t generation = 1;
};

// GodotNext: ps_mesh_sdf_atlas (#185)
// The per-instance SWRT table, indexed by the frame's GI instance selection
// (the same list that feeds the NextGI records). skinned and
// vertex-displacing instances carry SDF_UNSUPPORTED_DEFORMATION.
enum NextGISDFInstanceReason {
	NEXTGI_SDF_INCLUDED = 0,
	NEXTGI_SDF_UNSUPPORTED_DEFORMATION,
	NEXTGI_SDF_NO_FIELD,
	NEXTGI_SDF_INSTANCE_CAP,
};

class NextGISDFInstances {
public:
	enum Flags {
		INSTANCE_VISIBLE = 1 << 0,
	};

	struct Record {
		float world_to_local[3][4] = { { 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f } };
		float world_position[4] = { 0.0f, 0.0f, 0.0f, 1.0f }; // w = scale bound.
		uint32_t sdf_id = UINT32_MAX;
		uint32_t reason = NEXTGI_SDF_INCLUDED;
		uint32_t flags = 0;
		uint32_t required_mip = 0; // Written by the streaming shader.
		uint32_t mip_count = 1;
		uint32_t pad[3] = { 0, 0, 0 };
	};
	static constexpr uint32_t RECORD_BYTES = sizeof(Record);
	static constexpr uint32_t MAX_INSTANCES = 100000;

	void begin_frame();
	// Returns the record index, or UINT32_MAX when the table is full.
	uint32_t add(const NextGISDFAtlas &p_atlas, RID p_mesh, const Transform3D &p_transform, bool p_skinned, bool p_has_instance_uniforms, float p_scale_bound, bool p_visible, bool p_dynamic = false);
	void upload();
	void clear();

	uint32_t get_count() const { return uint32_t(records.size()); }
	const Record &get_record(uint32_t p_index) const { return records[p_index]; }
	const Transform3D &get_transform(uint32_t p_index) const { return transforms[p_index]; }
	RID get_mesh(uint32_t p_index) const { return meshes[p_index]; }
	bool is_dynamic(uint32_t p_index) const { return dynamic[p_index] != 0; }
	uint32_t get_reason_count(uint32_t p_reason) const;
	RID get_buffer() const { return buffer; }

private:
	LocalVector<Record> records;
	LocalVector<Transform3D> transforms;
	LocalVector<RID> meshes;
	LocalVector<uint8_t> dynamic;
	RID buffer;
	uint32_t buffer_capacity = 0;
	uint32_t reason_counts[4] = { 0, 0, 0, 0 };
};
