/**************************************************************************/
/*  test_nextgeo_foliage_instancing.cpp                                   */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_nextgeo_foliage_instancing)

#include "core/config/project_settings.h"
#include "core/math/projection.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/virtual_mesh_storage.h"

#ifndef _3D_DISABLED
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/3d/virtual_geometry_assembly.h"
#include "scene/resources/3d/virtual_geometry_data.h"
#include "scene/resources/mesh.h"
#include "scene/resources/multimesh.h"
#endif

#ifdef TOOLS_ENABLED
#include "editor/import/3d/resource_importer_scene.h"
#endif

#include <cstring>

namespace {

using VMS = RendererVirtualMeshStorage;

class MockFoliageVirtualMeshStorage : public RendererVirtualMeshStorage {
public:
	LocalVector<UploadRun> uploads;
	LocalVector<UploadRun> clears;
	uint32_t gpu_capacity = 0;
	uint32_t bucket_mask_uploads = 0;
	uint32_t assembly_uploads = 0;

protected:
	void _upload_records(uint32_t p_first, uint32_t p_count) override {
		uploads.push_back({ p_first, p_count });
	}
	bool _ensure_record_gpu_capacity(uint32_t p_capacity) override {
		if (p_capacity <= gpu_capacity) {
			return false;
		}
		gpu_capacity = p_capacity;
		return true;
	}
	void _clear_record_range(uint32_t p_first, uint32_t p_count) override {
		clears.push_back({ p_first, p_count });
	}
	void _upload_bucket_surface_masks(const BucketSurfaceMaskGPU *p_data, uint32_t p_count) override {
		(void)p_data;
		if (p_count > 0) {
			bucket_mask_uploads++;
		}
	}
	void _upload_assembly_buffers(const AssemblyHeaderGPU *p_headers, uint32_t p_header_count, const AssemblyChildGPU *p_children, uint32_t p_child_count, const AssemblyPartGPU *p_parts, uint32_t p_part_count) override {
		(void)p_headers;
		(void)p_children;
		(void)p_parts;
		if (p_header_count > 0 || p_child_count > 0 || p_part_count > 0) {
			assembly_uploads++;
		}
	}
	void _upload_mesh_records(uint32_t p_first, uint32_t p_count) override {
		(void)p_first;
		(void)p_count;
	}
};

static RenderingServerTypes::VirtualGeometryDesc _make_minimal_vgd_desc() {
	RenderingServerTypes::VirtualGeometryDesc desc;
	desc.page_count = 1;
	desc.page_size = 65536;
	desc.eligible_surface_mask = 1u;
	return desc;
}

static void _extract_frustum_planes(const Projection &p_vp, float r_planes[6][4]) {
	Vector4 rows[4];
	for (int col = 0; col < 4; col++) {
		rows[0][col] = p_vp.columns[col][0];
		rows[1][col] = p_vp.columns[col][1];
		rows[2][col] = p_vp.columns[col][2];
		rows[3][col] = p_vp.columns[col][3];
	}
	const Vector4 planes[6] = { rows[3] + rows[0], rows[3] - rows[0], rows[3] + rows[1], rows[3] - rows[1], rows[2], rows[3] - rows[2] };
	for (int i = 0; i < 6; i++) {
		const float len = Vector3(planes[i].x, planes[i].y, planes[i].z).length();
		const float inv = len > 0.0f ? 1.0f / len : 0.0f;
		r_planes[i][0] = planes[i].x * inv;
		r_planes[i][1] = planes[i].y * inv;
		r_planes[i][2] = planes[i].z * inv;
		r_planes[i][3] = planes[i].w * inv;
	}
}

#ifndef _3D_DISABLED
static Ref<ArrayMesh> _make_box_vgd_mesh(const Vector3 &p_size = Vector3(1.0f, 1.0f, 1.0f)) {
	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size(p_size);
	Ref<ArrayMesh> am;
	am.instantiate();
	am->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, box->get_mesh_arrays());
	Ref<VirtualGeometryData> vgd;
	vgd.instantiate();
	vgd->set_cluster_count(1);
	vgd->set_group_count(1);
	vgd->set_page_count(1);
	vgd->set_dag_depth(1);
	vgd->set_page_size(65536);
	PackedByteArray elig;
	elig.push_back(1);
	vgd->set_surface_eligibility(elig);
	am->set_virtual_geometry(vgd);
	return am;
}
#endif

} // namespace

TEST_CASE("[NextGeo][FoliageInstancing] GPU struct layouts, default setting, and RASTER_BIN_TABLE parity (#151)") {
	// Record layout stays untouched (224 B float / 256 B double) and auxiliary structs match GLSL std430.
#ifdef REAL_T_IS_DOUBLE
	CHECK_EQ(sizeof(VMS::Record), size_t(256));
#else
	CHECK_EQ(sizeof(VMS::Record), size_t(224));
#endif
	CHECK_EQ(sizeof(VMS::InstanceAttributes), size_t(48));
	CHECK_EQ(sizeof(VMS::BucketSurfaceMaskGPU), size_t(16));
	CHECK_EQ(sizeof(VMS::AssemblyHeaderGPU), size_t(32));
	CHECK_EQ(sizeof(VMS::AssemblyChildGPU), size_t(64));
	CHECK_EQ(sizeof(VMS::AssemblyPartGPU), size_t(96));

	// Experimental assemblies setting exists and defaults to false.
	REQUIRE(ProjectSettings::get_singleton() != nullptr);
	CHECK(ProjectSettings::get_singleton()->has_setting("rendering/nextgeo/experimental/assemblies"));
	CHECK_FALSE(bool(ProjectSettings::get_singleton()->get_setting("rendering/nextgeo/experimental/assemblies")));
	CHECK_FALSE(VMS::are_assemblies_enabled());

	// RASTER_BIN_TABLE carries paired ASSEMBLY and VSM_ASSEMBLY entries.
	const int32_t vis_asm = VMS::find_raster_bin_entry(VMS::RASTER_FEATURE_ASSEMBLY, VMS::RASTER_PASS_VISIBILITY);
	const int32_t vsm_asm = VMS::find_raster_bin_entry(VMS::RASTER_FEATURE_ASSEMBLY, VMS::RASTER_PASS_VSM);
	REQUIRE(vis_asm >= 0);
	REQUIRE(vsm_asm >= 0);
	CHECK_EQ(String(VMS::RASTER_BIN_TABLE[vis_asm].name), String("ASSEMBLY"));
	CHECK_EQ(String(VMS::RASTER_BIN_TABLE[vsm_asm].name), String("VSM_ASSEMBLY"));
	CHECK_EQ(uint32_t(VMS::RASTER_BIN_TABLE[vis_asm].bin), uint32_t(VMS::RASTER_BIN_FIXED));
	CHECK_EQ(uint32_t(VMS::RASTER_BIN_TABLE[vsm_asm].bin), uint32_t(VMS::RASTER_BIN_FIXED));
}

TEST_CASE("[NextGeo][FoliageInstancing] MultiMesh contiguous GPU range allocation, dirty sync, and reuse (#151)") {
	MockFoliageVirtualMeshStorage storage;
	const RID mesh_rid = RID::from_uint64(1001);
	const RID mm_rid = RID::from_uint64(2001);
	const RID inst_rid = RID::from_uint64(3001);
	storage.mesh_set_virtual_geometry(mesh_rid, _make_minimal_vgd_desc());

	const uint32_t parent_rec = storage.instance_record_allocate(inst_rid, mesh_rid);
	REQUIRE(parent_rec != UINT32_MAX);
	const uint32_t bslot = storage.instance_record_get_bucket_slot(parent_rec);
	CHECK_EQ(bslot, 0u);
	CHECK_EQ(storage.get_record(parent_rec).bucket_base, 0u);

	// Attach a 128-element contiguous range.
	const uint32_t range_base = storage.multimesh_range_attach(parent_rec, mm_rid, mesh_rid, 128);
	REQUIRE(range_base != UINT32_MAX);
	CHECK(range_base > parent_rec);
	CHECK_EQ(storage.get_record_count(), range_base + 128u);
	CHECK((storage.get_record(parent_rec).flags & VMS::RECORD_MULTIMESH_PARENT) != 0u);
	REQUIRE_EQ(storage.get_multimesh_expand_entries().size(), 1u);
	CHECK(storage.get_multimesh_expand_entries()[0].active);
	CHECK(storage.get_multimesh_expand_entries()[0].dirty);
	CHECK_EQ(storage.get_multimesh_expand_entries()[0].range_count, 128u);

	// Initial flush allocates the GPU record buffer; subsequent dirty flush uploads only the parent record,
	// NOT the 128 GPU-managed element slots.
	storage.flush();
	storage.uploads.clear();
	storage.instance_record_set_state(parent_rec, VMS::RECORD_VALID | VMS::RECORD_VISIBLE | VMS::RECORD_MULTIMESH_PARENT);
	storage.flush();
	bool uploaded_parent = false;
	bool uploaded_gpu_elem = false;
	for (const VMS::UploadRun &run : storage.uploads) {
		if (parent_rec >= run.first && parent_rec < run.first + run.count) {
			uploaded_parent = true;
		}
		if (run.first + run.count > range_base && run.first < range_base + 128u) {
			uploaded_gpu_elem = true;
		}
	}
	CHECK(uploaded_parent);
	CHECK_FALSE(uploaded_gpu_elem);

	// Clear dirty flag (simulating multimesh_expand dispatch); moving parent marks expand dirty again.
	storage.get_multimesh_expand_entries()[0].dirty = false;
	storage.instance_record_set_transform(parent_rec, Transform3D(Basis(), Vector3(5, 0, 0)), AABB(Vector3(0, 0, 0), Vector3(10, 10, 10)), true);
	CHECK(storage.get_multimesh_expand_entries()[0].dirty);

	// Visibility range propagates into parent InstanceAttributes.meta[2..3] and marks expand dirty.
	storage.get_multimesh_expand_entries()[0].dirty = false;
	storage.instance_record_set_visibility_range(parent_rec, 2.5f, 45.0f);
	CHECK(storage.get_multimesh_expand_entries()[0].dirty);
	const VMS::InstanceAttributes &pattr = storage.get_instance_attributes(parent_rec);
	float got_begin = 0.0f;
	float got_end = 0.0f;
	memcpy(&got_begin, &pattr.meta[2], sizeof(float));
	memcpy(&got_end, &pattr.meta[3], sizeof(float));
	CHECK(got_begin == doctest::Approx(2.5f));
	CHECK(got_end == doctest::Approx(45.0f));

	// Detaching queues a GPU zero-clear and makes the range reusable on the next frame.
	storage.clears.clear();
	storage.multimesh_range_detach(parent_rec);
	storage.flush();
	CHECK_FALSE(storage.clears.is_empty());
	CHECK_EQ(storage.clears[0].first, range_base);
	CHECK_EQ(storage.clears[0].count, 128u);

	storage.begin_frame();
	const uint32_t reused_base = storage.multimesh_range_attach(parent_rec, mm_rid, mesh_rid, 64);
	CHECK_EQ(reused_base, range_base);

	storage.instance_record_free(parent_rec);
}

TEST_CASE("[NextGeo][FoliageInstancing] Per-instance cull vs CPU reference and INSTANCE_CUSTOM/COLOR packing (#151)") {
	VMS::Record parent = {};
	parent.flags = VMS::RECORD_VALID | VMS::RECORD_VISIBLE | VMS::RECORD_CAST_SHADOW_ON | VMS::RECORD_MULTIMESH_PARENT;
	parent.virtual_mesh = 0u;
	parent.layer_mask = 1u;
	parent.bucket_base = 3u * 32u;
	parent.surface_mask = 1u;
	parent.transform[0][0] = 1.0f;
	parent.transform[1][1] = 1.0f;
	parent.transform[2][2] = 1.0f;
	parent.prev_transform[0][0] = 1.0f;
	parent.prev_transform[1][1] = 1.0f;
	parent.prev_transform[2][2] = 1.0f;
	parent.world_aabb_min[0] = -25.0f;
	parent.world_aabb_min[1] = -2.0f;
	parent.world_aabb_min[2] = -25.0f;
	parent.world_aabb_min[3] = 1.0f;
	parent.world_aabb_max[0] = 25.0f;
	parent.world_aabb_max[1] = 2.0f;
	parent.world_aabb_max[2] = 5.0f;
	parent.world_aabb_max[3] = 0.0f;

	VMS::InstanceAttributes parent_attr = {};
	const float range_begin = 3.0f;
	const float range_end = 15.0f;
	memcpy(&parent_attr.meta[2], &range_begin, sizeof(float));
	memcpy(&parent_attr.meta[3], &range_end, sizeof(float));

	// Camera at (0, 0, 0) looking down -Z with 60 deg FOV.
	const Projection proj = Projection::create_perspective(60.0f, 1.0f, 0.1f, 100.0f);
	float frustum[6][4];
	_extract_frustum_planes(proj, frustum);
	const Vector3 cam_pos(0.0f, 0.0f, 0.0f);
	const AABB unit_aabb(Vector3(-0.5f, -0.5f, -0.5f), Vector3(1.0f, 1.0f, 1.0f));

	struct Case {
		Vector3 pos;
		Vector3 scale;
		Color color;
		Color custom;
		bool visible;
		bool expect_pass;
	};
	const Case cases[] = {
		// In frustum and within [3, 15] -> PASS.
		{ Vector3(0, 0, -8), Vector3(1, 1, 1), Color(0.25f, 0.5f, 0.75f, 1.0f), Color(1.0f, 2.0f, 3.0f, 4.0f), true, true },
		// Non-uniform scale in frustum and in range -> PASS + RECORD_NON_UNIFORM_SCALE set.
		{ Vector3(1, 0, -10), Vector3(1, 2.5f, 1), Color(0.9f, 0.1f, 0.2f, 1.0f), Color(0.5f, 0.25f, 0.125f, 0.75f), true, true },
		// Behind camera -> CULLED (frustum).
		{ Vector3(0, 0, 4), Vector3(1, 1, 1), Color(1, 1, 1, 1), Color(0, 0, 0, 0), true, false },
		// Outside right frustum plane -> CULLED (frustum).
		{ Vector3(20, 0, -8), Vector3(1, 1, 1), Color(1, 1, 1, 1), Color(0, 0, 0, 0), true, false },
		// Too close (< range_begin 3.0) -> CULLED (distance).
		{ Vector3(0, 0, -1.5f), Vector3(1, 1, 1), Color(1, 1, 1, 1), Color(0, 0, 0, 0), true, false },
		// Too far (> range_end 15.0) -> CULLED (distance).
		{ Vector3(0, 0, -22), Vector3(1, 1, 1), Color(1, 1, 1, 1), Color(0, 0, 0, 0), true, false },
		// Beyond visible_instance_count -> CULLED (RECORD_VISIBLE cleared).
		{ Vector3(0, 0, -8), Vector3(1, 1, 1), Color(1, 1, 1, 1), Color(0, 0, 0, 0), false, false },
	};

	// Parent itself is NEVER culled in (always skipped because RECORD_MULTIMESH_PARENT is set).
	CHECK_FALSE(VMS::cull_instance_cpu(parent, parent_attr, nullptr, frustum, cam_pos, 1u, false));

	for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const Case &c = cases[i];
		// Layout: 12 transform floats + 4 color floats + 4 custom floats = 20 floats.
		float elem_data[20] = {
			c.scale.x, 0.0f, 0.0f, c.pos.x,
			0.0f, c.scale.y, 0.0f, c.pos.y,
			0.0f, 0.0f, c.scale.z, c.pos.z,
			c.color.r, c.color.g, c.color.b, c.color.a,
			c.custom.r, c.custom.g, c.custom.b, c.custom.a,
		};
		VMS::Record out_rec = {};
		VMS::InstanceAttributes out_attr = {};
		VMS::build_multimesh_element_record_cpu(parent, parent_attr, 0u, i, elem_data, elem_data, 12u, 16u, true, true, c.visible, unit_aabb, out_rec, out_attr);

		// Verify flags, bucket_base inheritance, and packed COLOR / INSTANCE_CUSTOM / INSTANCE_ID.
		CHECK((out_rec.flags & VMS::RECORD_MULTIMESH_ELEMENT) != 0u);
		CHECK((out_rec.flags & VMS::RECORD_MULTIMESH_PARENT) == 0u);
		CHECK((out_rec.flags & VMS::RECORD_HAS_INSTANCE_COLOR) != 0u);
		CHECK((out_rec.flags & VMS::RECORD_HAS_INSTANCE_CUSTOM) != 0u);
		CHECK_EQ(out_rec.bucket_base, parent.bucket_base);
		CHECK_EQ(out_attr.meta[0], 0u);
		CHECK_EQ(out_attr.meta[1], i);
		CHECK(out_attr.color[0] == doctest::Approx(c.color.r));
		CHECK(out_attr.color[1] == doctest::Approx(c.color.g));
		CHECK(out_attr.color[2] == doctest::Approx(c.color.b));
		CHECK(out_attr.color[3] == doctest::Approx(c.color.a));
		CHECK(out_attr.custom_data[0] == doctest::Approx(c.custom.r));
		CHECK(out_attr.custom_data[1] == doctest::Approx(c.custom.g));
		CHECK(out_attr.custom_data[2] == doctest::Approx(c.custom.b));
		CHECK(out_attr.custom_data[3] == doctest::Approx(c.custom.a));
		if (i == 1) {
			CHECK((out_rec.flags & VMS::RECORD_NON_UNIFORM_SCALE) != 0u);
		} else {
			CHECK((out_rec.flags & VMS::RECORD_NON_UNIFORM_SCALE) == 0u);
		}

		const bool passed = VMS::cull_instance_cpu(out_rec, out_attr, &parent, frustum, cam_pos, 1u, false);
		CHECK_EQ(passed, c.expect_pass);
	}

	// Coarse parent AABB completely behind camera rejects all child elements early.
	VMS::Record behind_parent = parent;
	behind_parent.world_aabb_min[2] = 10.0f;
	behind_parent.world_aabb_max[2] = 20.0f;
	float in_front[20] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, -8, 1, 1, 1, 1, 0, 0, 0, 0 };
	VMS::Record elem_rec = {};
	VMS::InstanceAttributes elem_attr = {};
	VMS::build_multimesh_element_record_cpu(parent, parent_attr, 0u, 0u, in_front, in_front, 12u, 16u, true, true, true, unit_aabb, elem_rec, elem_attr);
	CHECK_FALSE(VMS::cull_instance_cpu(elem_rec, elem_attr, &behind_parent, frustum, cam_pos, 1u, false));
}

#ifndef _3D_DISABLED
TEST_CASE("[NextGeo][FoliageInstancing] Nanite Assembly 2-level hierarchy, depth clamp, and CPU expansion (#151)") {
	Ref<ArrayMesh> leaf_a = _make_box_vgd_mesh(Vector3(1.0f, 1.0f, 1.0f));
	Ref<ArrayMesh> leaf_b = _make_box_vgd_mesh(Vector3(2.0f, 1.0f, 1.0f));

	// Level 1 child assembly with 2 parts.
	Ref<VirtualGeometryAssembly> branch;
	branch.instantiate();
	branch->add_part(leaf_a, Transform3D(Basis(), Vector3(1.0f, 0.0f, 0.0f)));
	branch->add_part(leaf_b, Transform3D(Basis(), Vector3(-1.0f, 0.5f, 0.0f)));
	CHECK_EQ(branch->get_part_count(), 2);
	CHECK_EQ(branch->get_depth(), 1);
	CHECK_EQ(branch->get_total_leaf_part_count(), 2);

	// Level 2 root assembly with 1 direct part + 2 child assemblies (= 5 leaves total).
	Ref<VirtualGeometryAssembly> tree;
	tree.instantiate();
	tree->add_part(leaf_a, Transform3D(Basis(), Vector3(0.0f, 0.0f, 0.0f)));
	CHECK_EQ(tree->add_child_assembly(branch, Transform3D(Basis(), Vector3(0.0f, 2.0f, 0.0f))), OK);
	CHECK_EQ(tree->add_child_assembly(branch, Transform3D(Basis(), Vector3(0.0f, 4.0f, 0.0f))), OK);
	CHECK_EQ(tree->get_depth(), 2);
	CHECK_EQ(tree->get_total_leaf_part_count(), 5);

	// Level 3 nesting must be rejected (<= 2 levels deep).
	Ref<VirtualGeometryAssembly> too_deep;
	too_deep.instantiate();
	ERR_PRINT_OFF;
	CHECK_EQ(too_deep->add_child_assembly(tree, Transform3D()), ERR_INVALID_PARAMETER);
	// Cycle (self-child) must also be rejected.
	CHECK_EQ(branch->add_child_assembly(branch, Transform3D()), ERR_INVALID_PARAMETER);
	ERR_PRINT_ON;

	// Merged AABB covers direct + child leaves.
	const AABB merged = tree->get_merged_aabb();
	CHECK(merged.has_surface());
	CHECK(merged.has_point(Vector3(1.0f, 4.0f, 0.0f)));
	CHECK(merged.has_point(Vector3(-1.5f, 4.5f, 0.0f)));

	// Storage-level CPU expansion produces 5 leaves referencing the 2 unique part meshes.
	MockFoliageVirtualMeshStorage storage;
	const RID rid_a = leaf_a->get_rid();
	const RID rid_b = leaf_b->get_rid();
	const RID rid_tree = RID::from_uint64(9001);
	storage.mesh_set_virtual_geometry(rid_a, _make_minimal_vgd_desc());
	storage.mesh_set_virtual_geometry(rid_b, _make_minimal_vgd_desc());

	RenderingServerTypes::VirtualGeometryDesc tree_desc = _make_minimal_vgd_desc();
	const AABB box_aabb(Vector3(-0.5f, -0.5f, -0.5f), Vector3(1.0f, 1.0f, 1.0f));
	tree_desc.assembly_parts.push_back({ rid_a, Transform3D(Basis(), Vector3(0, 0, 0)), box_aabb });
	RenderingServerTypes::VirtualGeometryDesc::AssemblyChildDesc c0;
	c0.local_transform = Transform3D(Basis(), Vector3(0, 2, 0));
	c0.parts.push_back({ rid_a, Transform3D(Basis(), Vector3(1, 0, 0)), box_aabb });
	c0.parts.push_back({ rid_b, Transform3D(Basis(), Vector3(-1, 0.5f, 0)), box_aabb });
	RenderingServerTypes::VirtualGeometryDesc::AssemblyChildDesc c1 = c0;
	c1.local_transform = Transform3D(Basis(), Vector3(0, 4, 0));
	tree_desc.assembly_children.push_back(c0);
	tree_desc.assembly_children.push_back(c1);
	storage.mesh_set_virtual_geometry(rid_tree, tree_desc);

	CHECK(storage.mesh_has_assembly(rid_tree));
	CHECK_EQ(storage.mesh_get_assembly_leaf_count(rid_tree), 5u);
	const LocalVector<RID> *unique_parts = storage.mesh_get_assembly_unique_part_meshes(rid_tree);
	REQUIRE(unique_parts != nullptr);
	CHECK_EQ(unique_parts->size(), 2u);

	const Transform3D root_xf(Basis(), Vector3(10.0f, 0.0f, -5.0f));
	LocalVector<VMS::AssemblyLeafCPU> leaves;
	storage.mesh_get_assembly_expanded_parts_cpu(rid_tree, root_xf, leaves);
	REQUIRE_EQ(leaves.size(), 5u);
	CHECK(leaves[0].world_transform.origin.is_equal_approx(Vector3(10.0f, 0.0f, -5.0f)));
	CHECK(leaves[1].world_transform.origin.is_equal_approx(Vector3(11.0f, 2.0f, -5.0f)));
	CHECK(leaves[2].world_transform.origin.is_equal_approx(Vector3(9.0f, 2.5f, -5.0f)));
	CHECK(leaves[3].world_transform.origin.is_equal_approx(Vector3(11.0f, 4.0f, -5.0f)));
	CHECK(leaves[4].world_transform.origin.is_equal_approx(Vector3(9.0f, 4.5f, -5.0f)));

	// Enable experimental assemblies temporarily and verify get_effective_records_cpu expands the assembly.
	const bool prev_setting = bool(ProjectSettings::get_singleton()->get_setting("rendering/nextgeo/experimental/assemblies"));
	ProjectSettings::get_singleton()->set_setting("rendering/nextgeo/experimental/assemblies", true);

	const uint32_t tree_rec = storage.instance_record_allocate(RID::from_uint64(9002), rid_tree);
	storage.instance_record_resize_bucket_slots(tree_rec, 2u);
	storage.instance_record_set_state(tree_rec, VMS::RECORD_VALID | VMS::RECORD_VISIBLE | VMS::RECORD_HAS_ASSEMBLY);
	storage.instance_record_set_transform(tree_rec, root_xf, AABB(Vector3(5, -5, -10), Vector3(10, 15, 10)), true);
	storage.flush();
	CHECK(storage.assembly_uploads > 0u);
	CHECK(storage.has_active_assemblies());
	CHECK_EQ(storage.get_assembly_expanded_capacity(), 5u);

	LocalVector<VMS::Record> eff_recs;
	LocalVector<VMS::InstanceAttributes> eff_attrs;
	storage.get_effective_records_cpu(eff_recs, eff_attrs);
	uint32_t leaf_rec_count = 0u;
	for (uint32_t i = 0; i < eff_recs.size(); i++) {
		if ((eff_recs[i].flags & VMS::RECORD_ASSEMBLY_PART) != 0u) {
			leaf_rec_count++;
		}
	}
	CHECK_EQ(leaf_rec_count, 5u);

	storage.instance_record_free(tree_rec);
	ProjectSettings::get_singleton()->set_setting("rendering/nextgeo/experimental/assemblies", prev_setting);
}

#ifdef TOOLS_ENABLED
TEST_CASE("[NextGeo][FoliageInstancing] Scene importer builds Nanite Assembly from repeated glTF nodes (#151)") {
	Ref<ArrayMesh> leaf_mesh = _make_box_vgd_mesh(Vector3(0.5f, 0.5f, 0.5f));
	Ref<ArrayMesh> unique_mesh = _make_box_vgd_mesh(Vector3(1.0f, 1.0f, 1.0f));

	Node3D *root = memnew(Node3D);
	root->set_name("TreeScene");

	// Repeated virtualized leaf mesh across 3 sibling nodes -> becomes an assembly!
	for (int i = 0; i < 3; i++) {
		MeshInstance3D *mi = memnew(MeshInstance3D);
		mi->set_mesh(leaf_mesh);
		mi->set_position(Vector3(float(i) * 2.0f, 1.0f, 0.0f));
		root->add_child(mi);
	}
	// Unique non-repeated mesh -> stays untouched.
	MeshInstance3D *trunk = memnew(MeshInstance3D);
	trunk->set_mesh(unique_mesh);
	root->add_child(trunk);

	// Check build_virtual_geometry_assembly_from_scene produces an assembly with 3 leaves on a container node.
	const bool built = ResourceImporterScene::build_virtual_geometry_assembly_from_scene(root);
	CHECK(built);

	// Locate the generated Assembly container MeshInstance3D.
	MeshInstance3D *asm_node = nullptr;
	for (int i = 0; i < root->get_child_count(); i++) {
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(root->get_child(i));
		if (mi != nullptr && mi->get_mesh().is_valid()) {
			Ref<ArrayMesh> am = mi->get_mesh();
			if (am.is_valid() && am->get_virtual_geometry().is_valid() && am->get_virtual_geometry()->get_assembly().is_valid()) {
				asm_node = mi;
				break;
			}
		}
	}
	REQUIRE(asm_node != nullptr);
	Ref<VirtualGeometryAssembly> built_asm = Object::cast_to<ArrayMesh>(asm_node->get_mesh().ptr())->get_virtual_geometry()->get_assembly();
	REQUIRE(built_asm.is_valid());
	CHECK_EQ(built_asm->get_total_leaf_part_count(), 3);
	CHECK_LE(built_asm->get_depth(), VirtualGeometryAssembly::MAX_DEPTH);

	memdelete(root);
}
#endif

TEST_CASE("[SceneTree][NextGeo][FoliageInstancing] Eligible 3D MultiMeshInstance3D gets persistent NextGeo record while non-virtualized stays untouched (#151)") {
	REQUIRE(SceneTree::get_singleton() != nullptr);
	REQUIRE(SceneTree::get_singleton()->get_root() != nullptr);
	REQUIRE(RSG::virtual_mesh_storage != nullptr);
	Ref<ArrayMesh> vgd_mesh = _make_box_vgd_mesh();
	Ref<BoxMesh> plain_mesh;
	plain_mesh.instantiate();

	// 1. Virtualized 3D MultiMeshInstance3D -> gets a valid NextGeo record and routes virtual.
	Ref<MultiMesh> vmm;
	vmm.instantiate();
	vmm->set_transform_format(MultiMesh::TRANSFORM_3D);
	vmm->set_use_colors(true);
	vmm->set_use_custom_data(true);
	vmm->set_instance_count(4);
	vmm->set_mesh(vgd_mesh);
	for (int i = 0; i < 4; i++) {
		vmm->set_instance_transform(i, Transform3D(Basis(), Vector3(float(i) * 2.0f, 0.0f, 0.0f)));
		vmm->set_instance_color(i, Color(float(i) * 0.25f, 0.5f, 0.75f, 1.0f));
		vmm->set_instance_custom_data(i, Color(float(i), 1.0f, 2.0f, 3.0f));
	}
	MultiMeshInstance3D *vmmi = memnew(MultiMeshInstance3D);
	vmmi->set_multimesh(vmm);
	SceneTree::get_singleton()->get_root()->add_child(vmmi);

	RenderingServer *rs = RenderingServer::get_singleton();
	const Dictionary vst = rs->virtual_geometry_get_debug_stats(vmmi->get_instance());
	const int64_t vrec = int64_t(vst.get("record_index", -1));
	REQUIRE(vrec >= 0);
	CHECK((RSG::virtual_mesh_storage->get_record(uint32_t(vrec)).flags & VMS::RECORD_MULTIMESH_PARENT) != 0u);
	bool found_range = false;
	for (const VMS::MultiMeshExpandEntry &entry : RSG::virtual_mesh_storage->get_multimesh_expand_entries()) {
		if (entry.active && entry.parent_record == uint32_t(vrec) && entry.range_count == 4u) {
			found_range = true;
			break;
		}
	}
	CHECK(found_range);

	// 2. Non-virtualized MultiMeshInstance3D -> NO NextGeo record, legacy routing untouched.
	Ref<MultiMesh> pmm;
	pmm.instantiate();
	pmm->set_transform_format(MultiMesh::TRANSFORM_3D);
	pmm->set_instance_count(4);
	pmm->set_mesh(plain_mesh);
	MultiMeshInstance3D *pmmi = memnew(MultiMeshInstance3D);
	pmmi->set_multimesh(pmm);
	SceneTree::get_singleton()->get_root()->add_child(pmmi);

	const Dictionary pst = rs->virtual_geometry_get_debug_stats(pmmi->get_instance());
	CHECK_EQ(int64_t(pst.get("record_index", -1)), -1);
	CHECK_EQ(String(pst.get("route", "")), String("legacy"));

	SceneTree::get_singleton()->get_root()->remove_child(vmmi);
	SceneTree::get_singleton()->get_root()->remove_child(pmmi);
	memdelete(vmmi);
	memdelete(pmmi);
}
#endif
