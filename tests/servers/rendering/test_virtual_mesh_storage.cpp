/**************************************************************************/
/*  test_virtual_mesh_storage.cpp                                         */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_virtual_mesh_storage)

#include "servers/rendering/storage/virtual_mesh_storage.h"

#include <cstddef>

namespace {

class TestVirtualMeshStorage : public RendererVirtualMeshStorage {
public:
	int record_upload_calls = 0;
	uint32_t uploaded_records = 0;
	uint32_t last_first = 0;
	uint32_t last_count = 0;
	int mesh_upload_calls = 0;
	uint32_t uploaded_mesh_records = 0;
	uint32_t dag_capacity = 0;
	uint32_t uploaded_dag_bytes = 0;

protected:
	void _upload_records(uint32_t p_first, uint32_t p_count) override {
		record_upload_calls++;
		uploaded_records += p_count;
		last_first = p_first;
		last_count = p_count;
	}
	void _upload_mesh_records(uint32_t p_first, uint32_t p_count) override {
		mesh_upload_calls++;
		uploaded_mesh_records += p_count;
		(void)p_first;
		(void)p_count;
	}
	bool _resize_dag(uint32_t p_bytes) override {
		dag_capacity = p_bytes;
		return true;
	}
	void _upload_dag(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) override {
		(void)p_offset;
		(void)p_data;
		uploaded_dag_bytes += p_size;
	}
};

static RenderingServerTypes::VirtualGeometryDesc _desc(uint32_t p_mask) {
	RenderingServerTypes::VirtualGeometryDesc desc;
	desc.eligible_surface_mask = p_mask;
	desc.ngm_path = "res://test.ngm";
	desc.dag_header.resize(8);
	desc.root_pages.push_back(Vector<uint8_t>());
	return desc;
}

static void _append_u32(Vector<uint8_t> &r_bytes, uint32_t p_value) {
	const uint8_t *raw = reinterpret_cast<const uint8_t *>(&p_value);
	for (int index = 0; index < 4; index++) {
		r_bytes.push_back(raw[index]);
	}
}

static void _append_f32(Vector<uint8_t> &r_bytes, float p_value) {
	uint32_t bits = 0;
	memcpy(&bits, &p_value, sizeof(uint32_t));
	_append_u32(r_bytes, bits);
}

// Two groups and a two-level BVH8: root (node 0) -> node 1 -> two leaf groups.
static Vector<uint8_t> _test_dag() {
	Vector<uint8_t> dag;
	_append_u32(dag, 2); // group count
	_append_u32(dag, 2); // BVH node count
	for (int group = 0; group < 2; group++) {
		_append_u32(dag, group == 0 ? 5 : 10); // cluster offset
		_append_u32(dag, group == 0 ? 3 : 4); // cluster count
		_append_u32(dag, 0); // page id
		_append_u32(dag, 0); // depth
		for (int index = 0; index < 4; index++) {
			_append_f32(dag, 0.0f); // parent LOD sphere
		}
		_append_f32(dag, 1.0f); // parent error
		for (int index = 0; index < 3; index++) {
			_append_f32(dag, -1.0f); // aabb min
		}
		for (int index = 0; index < 3; index++) {
			_append_f32(dag, 1.0f); // aabb max
		}
	}
	for (int node = 0; node < 2; node++) {
		for (int index = 0; index < 4; index++) {
			_append_f32(dag, 0.0f); // lod sphere
		}
		_append_f32(dag, 1.0f); // max parent error
		for (int index = 0; index < 3; index++) {
			_append_f32(dag, -1.0f); // aabb min
		}
		for (int index = 0; index < 3; index++) {
			_append_f32(dag, 1.0f); // aabb max
		}
		_append_u32(dag, node == 0 ? 1 : 2); // child count
		for (int child = 0; child < 8; child++) {
			uint32_t value = 0xFFFFFFFFu;
			if (node == 0 && child == 0) {
				value = 1; // node 1
			} else if (node == 1 && child < 2) {
				value = 0x80000000u | uint32_t(child); // leaf group
			}
			_append_u32(dag, value);
		}
	}
	return dag;
}

static Transform3D _transform(float p_x) {
	return Transform3D(Basis(), Vector3(p_x, 0.0f, 0.0f));
}

static AABB _aabb(float p_x) {
	return AABB(Vector3(p_x - 0.5f, -0.5f, -0.5f), Vector3(1.0f, 1.0f, 1.0f));
}

} // namespace

TEST_CASE("[NextGeo][Records] Record layout and offsets") {
	using S = RendererVirtualMeshStorage;
#ifdef REAL_T_IS_DOUBLE
	CHECK_EQ(sizeof(S::Record), size_t(256));
#else
	CHECK_EQ(sizeof(S::Record), size_t(224));
#endif
	CHECK_EQ(sizeof(S::MeshRecord), size_t(32));
	CHECK_EQ(S::RECORD_BYTES, uint32_t(sizeof(S::Record)));
	CHECK_EQ(S::MESH_RECORD_BYTES, uint32_t(32));

	// std430 offsets, plan §6.3.
	CHECK_EQ(offsetof(S::Record, transform), size_t(0));
	CHECK_EQ(offsetof(S::Record, prev_transform), size_t(48));
	CHECK_EQ(offsetof(S::Record, world_aabb_min), size_t(96));
	CHECK_EQ(offsetof(S::Record, world_aabb_max), size_t(112));
	CHECK_EQ(offsetof(S::Record, prev_world_aabb_min), size_t(128));
	CHECK_EQ(offsetof(S::Record, prev_world_aabb_max), size_t(144));
	CHECK_EQ(offsetof(S::Record, virtual_mesh), size_t(160));
	CHECK_EQ(offsetof(S::Record, page_table_offset), size_t(164));
	CHECK_EQ(offsetof(S::Record, surface_mask), size_t(168));
	CHECK_EQ(offsetof(S::Record, bucket_base), size_t(172));
	CHECK_EQ(offsetof(S::Record, layer_mask), size_t(176));
	CHECK_EQ(offsetof(S::Record, flags), size_t(180));
	CHECK_EQ(offsetof(S::Record, gi_flags), size_t(184));
	CHECK_EQ(offsetof(S::Record, tlas_instance_index), size_t(192));
	CHECK_EQ(offsetof(S::Record, blas_handle), size_t(196));
	CHECK_EQ(offsetof(S::Record, rt_arena_chunk), size_t(200));
	CHECK_EQ(offsetof(S::Record, rt_arena_offset), size_t(204));
	CHECK_EQ(offsetof(S::Record, src_page_table), size_t(208));
	CHECK_EQ(offsetof(S::Record, card_set), size_t(212));
#ifdef REAL_T_IS_DOUBLE
	CHECK_EQ(offsetof(S::Record, model_precision), size_t(224));
	CHECK_EQ(offsetof(S::Record, prev_model_precision), size_t(240));
#endif
	CHECK_EQ(offsetof(S::MeshRecord, bvh_node_offset), size_t(0));
	CHECK_EQ(offsetof(S::MeshRecord, bvh_level_count), size_t(4));
	CHECK_EQ(offsetof(S::MeshRecord, cluster_offset), size_t(8));
	CHECK_EQ(offsetof(S::MeshRecord, cluster_count), size_t(12));
	CHECK_EQ(offsetof(S::MeshRecord, group_offset), size_t(16));
	CHECK_EQ(offsetof(S::MeshRecord, page_table_offset), size_t(20));
	CHECK_EQ(offsetof(S::MeshRecord, page_count), size_t(24));
	CHECK_EQ(offsetof(S::MeshRecord, surface_count), size_t(28));
}

TEST_CASE("[NextGeo][Records] Allocate, free and one-frame reuse") {
	TestVirtualMeshStorage storage;
	const RID mesh = RID::from_uint64(1);
	const RID instance = RID::from_uint64(2);
	storage.mesh_set_virtual_geometry(mesh, _desc(0x3));
	CHECK(storage.mesh_has_virtual_geometry(mesh));

	const uint32_t first = storage.instance_record_allocate(instance, mesh);
	CHECK_EQ(first, uint32_t(0));
	CHECK_EQ(storage.get_live_record_count(), uint32_t(1));
	CHECK_EQ(storage.get_record(first).virtual_mesh, uint32_t(0));
	CHECK_EQ(storage.get_record(first).surface_mask, uint32_t(0x3));
	CHECK_EQ((storage.get_record(first).flags & RendererVirtualMeshStorage::RECORD_VALID), uint32_t(RendererVirtualMeshStorage::RECORD_VALID));

	storage.instance_record_free(first);
	CHECK_EQ(storage.get_live_record_count(), uint32_t(0));
	// A freed slot is not reusable in the same frame.
	const uint32_t second = storage.instance_record_allocate(instance, mesh);
	CHECK_EQ(second, uint32_t(1));
	storage.begin_frame();
	const uint32_t reused = storage.instance_record_allocate(instance, mesh);
	CHECK_EQ(reused, uint32_t(0));
	CHECK_EQ(storage.get_live_record_count(), uint32_t(2));
	CHECK_EQ(storage.get_record_capacity(), uint32_t(RendererVirtualMeshStorage::INITIAL_RECORD_CAPACITY));
}

TEST_CASE("[NextGeo][Records] Previous-frame transform history") {
	TestVirtualMeshStorage storage;
	const RID mesh = RID::from_uint64(11);
	const RID instance = RID::from_uint64(12);
	storage.mesh_set_virtual_geometry(mesh, _desc(0x1));
	const uint32_t record = storage.instance_record_allocate(instance, mesh);

	storage.instance_record_set_transform(record, _transform(1.0f), _aabb(1.0f), true);
	storage.flush();
	CHECK_EQ(storage.get_record(record).transform[0][3], 1.0f);
	CHECK_EQ(storage.get_record(record).prev_transform[0][3], 1.0f);

	storage.begin_frame();
	storage.instance_record_set_transform(record, _transform(2.0f), _aabb(2.0f), true);
	storage.flush();
	CHECK_EQ(storage.get_record(record).transform[0][3], 2.0f);
	CHECK_EQ(storage.get_record(record).prev_transform[0][3], 1.0f);

	// No update this frame: the first clean frame rewrites prev = cur once.
	storage.begin_frame();
	storage.flush();
	CHECK_EQ(storage.get_record(record).prev_transform[0][3], 2.0f);

	// And then the record is static again: no further upload.
	const int uploads = storage.record_upload_calls;
	storage.begin_frame();
	storage.flush();
	CHECK_EQ(storage.record_upload_calls, uploads);
}

TEST_CASE("[NextGeo][Records] Teleport and invisibility") {
	TestVirtualMeshStorage storage;
	const RID mesh = RID::from_uint64(21);
	const RID instance = RID::from_uint64(22);
	storage.mesh_set_virtual_geometry(mesh, _desc(0x1));
	const uint32_t record = storage.instance_record_allocate(instance, mesh);

	storage.instance_record_set_transform(record, _transform(1.0f), _aabb(1.0f), true);
	storage.flush();
	storage.begin_frame();
	storage.instance_record_set_transform(record, _transform(200.0f), _aabb(200.0f), true, true);
	storage.flush();
	CHECK_EQ(storage.get_record(record).transform[0][3], 200.0f);
	CHECK_EQ(storage.get_record(record).prev_transform[0][3], 200.0f);
	CHECK((storage.get_record(record).flags & RendererVirtualMeshStorage::RECORD_TELEPORTED) != 0u);

	storage.begin_frame();
	storage.instance_record_set_transform(record, _transform(200.0f), _aabb(200.0f), false);
	storage.flush();
	CHECK_EQ((storage.get_record(record).flags & RendererVirtualMeshStorage::RECORD_VISIBLE), uint32_t(0));
}

TEST_CASE("[NextGeo][Records] Dirty runs and RT fields") {
	TestVirtualMeshStorage storage;
	const RID mesh = RID::from_uint64(31);
	storage.mesh_set_virtual_geometry(mesh, _desc(0x7));
	CHECK_EQ(storage.get_mesh_record(mesh).surface_count, uint32_t(3));
	CHECK(storage.mesh_upload_calls == 0); // Flushed below.

	const uint32_t record = storage.instance_record_allocate(RID::from_uint64(32), mesh);
	const uint32_t gap = storage.instance_record_allocate(RID::from_uint64(33), mesh);
	const uint32_t record_b = storage.instance_record_allocate(RID::from_uint64(34), mesh);
	storage.flush();
	CHECK(storage.mesh_upload_calls > 0);
	CHECK_EQ(storage.uploaded_records, uint32_t(3));
	CHECK_EQ(storage.last_first, uint32_t(0));
	CHECK_EQ(storage.last_count, uint32_t(3));

	// Consume the first clean frame's prev rewrite so the gap is quiet below.
	storage.begin_frame();
	storage.flush();
	const int calls_before = storage.record_upload_calls;
	storage.begin_frame();
	storage.instance_record_set_transform(record, _transform(3.0f), _aabb(3.0f), true);
	storage.instance_record_set_transform(record_b, _transform(5.0f), _aabb(5.0f), true);
	const uint32_t rt_fields[6] = { 7, 8, 9, 10, 11, 12 };
	storage.instance_record_set_rt(record, rt_fields);
	storage.flush();
	// Two separate dirty slots give two contiguous runs, with the gap in between.
	CHECK_EQ(storage.record_upload_calls - calls_before, 2);
	CHECK_EQ(storage.get_record(record).tlas_instance_index, uint32_t(7));
	CHECK_EQ(storage.get_record(record).blas_handle, uint32_t(8));
	CHECK_EQ(storage.get_record(record).rt_arena_chunk, uint32_t(9));
	CHECK_EQ(storage.get_record(record).rt_arena_offset, uint32_t(10));
	CHECK_EQ(storage.get_record(record).src_page_table, uint32_t(11));
	CHECK_EQ(storage.get_record(record).card_set, uint32_t(12));
	CHECK_EQ(storage.get_record(gap).transform[0][3], 0.0f);

	// Clearing removes the mesh but keeps its record slot stable for a re-set.
	storage.mesh_clear_virtual_geometry(mesh, 0);
	CHECK_FALSE(storage.mesh_has_virtual_geometry(mesh));
	storage.mesh_set_virtual_geometry(mesh, _desc(0x1));
	CHECK(storage.mesh_has_virtual_geometry(mesh));
	CHECK_EQ(storage.get_mesh_record(mesh).surface_count, uint32_t(1));
}

TEST_CASE("[NextGeo][Records] DAG offsets and BVH level count") {
	TestVirtualMeshStorage storage;
	const RID mesh = RID::from_uint64(5001);
	RenderingServerTypes::VirtualGeometryDesc desc = _desc(0x3);
	desc.dag_header = _test_dag();
	CHECK_EQ(desc.dag_header.size(), size_t(8 + 2 * 60 + 2 * 80));
	storage.mesh_set_virtual_geometry(mesh, desc);

	const RendererVirtualMeshStorage::MeshRecord &record = storage.get_mesh_record(mesh);
	CHECK_EQ(record.group_offset, uint32_t(8));
	CHECK_EQ(record.bvh_node_offset, uint32_t(8 + 2 * 60));
	CHECK_EQ(record.bvh_level_count, uint32_t(2));
	CHECK_EQ(record.cluster_offset, uint32_t(5));
	CHECK_EQ(record.cluster_count, uint32_t(7));
	CHECK_EQ(record.surface_count, uint32_t(2));
	CHECK_EQ(storage.uploaded_dag_bytes, uint32_t(desc.dag_header.size()));
	CHECK(storage.dag_capacity >= desc.dag_header.size());

	// A second mesh lands after the first, 16-byte aligned.
	const RID second = RID::from_uint64(5002);
	storage.mesh_set_virtual_geometry(second, desc);
	const RendererVirtualMeshStorage::MeshRecord &second_record = storage.get_mesh_record(second);
	CHECK(second_record.group_offset >= record.group_offset + record.bvh_node_offset - 8 + 2 * 80);
	CHECK_EQ((second_record.group_offset - 8) % 16, uint32_t(0));
	CHECK_EQ(second_record.bvh_level_count, uint32_t(2));

	// Clearing releases the DAG range, so a re-set reuses the first range.
	storage.mesh_clear_virtual_geometry(mesh, 0);
	storage.mesh_set_virtual_geometry(mesh, desc);
	const RendererVirtualMeshStorage::MeshRecord &reused = storage.get_mesh_record(mesh);
	CHECK_EQ(reused.group_offset, uint32_t(8));
	CHECK_EQ(reused.cluster_count, uint32_t(7));
}

TEST_CASE("[NextGeo][Records] Capacity doubles from 4096") {
	TestVirtualMeshStorage storage;
	CHECK_EQ(storage.get_record_capacity(), uint32_t(4096));
	const RID mesh = RID::from_uint64(41);
	storage.mesh_set_virtual_geometry(mesh, _desc(0x1));
	for (uint32_t index = 0; index < 4096; index++) {
		CHECK_EQ(storage.instance_record_allocate(RID::from_uint64(1000 + index), mesh), index);
	}
	CHECK_EQ(storage.get_record_capacity(), uint32_t(4096));
	CHECK_EQ(storage.instance_record_allocate(RID::from_uint64(9999), mesh), uint32_t(4096));
	CHECK_EQ(storage.get_record_capacity(), uint32_t(8192));
}
