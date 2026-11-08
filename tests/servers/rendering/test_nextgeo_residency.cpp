/**************************************************************************/
/*  test_nextgeo_residency.cpp                                            */
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

TEST_FORCE_LINK(test_nextgeo_residency)

#ifndef _3D_DISABLED

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_format_ngm.h"
#include "core/math/random_pcg.h"
#include "core/os/os.h"
#include "scene/resources/3d/importer_mesh.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/3d/virtual_geometry_data.h"
#include "scene/resources/mesh.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/virtual_mesh_storage.h"
#include "tests/test_utils.h"

#include "modules/modules_enabled.gen.h"
#ifdef MODULE_NEXTGEO_ENABLED
#include "modules/nextgeo/nextgeo_cut.h"
#include "modules/nextgeo/nextgeo_dag_builder.h"
#endif

namespace TestNextGeoResidency {

class MockVirtualMeshStorage : public RendererVirtualMeshStorage {
public:
	uint32_t page_uploads = 0;
	uint32_t residency_uploads = 0;
	uint32_t cluster_uploads = 0;

protected:
	void _upload_records(uint32_t p_first, uint32_t p_count) override {
		(void)p_first;
		(void)p_count;
	}
	void _upload_mesh_records(uint32_t p_first, uint32_t p_count) override {
		(void)p_first;
		(void)p_count;
	}
	bool _resize_dag(uint32_t p_bytes) override {
		(void)p_bytes;
		return true;
	}
	bool _resize_pages(uint32_t p_bytes) override {
		(void)p_bytes;
		return true;
	}
	void _upload_pages(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) override {
		(void)p_offset;
		(void)p_data;
		if (p_size > 0) {
			page_uploads++;
		}
	}
	bool _resize_clusters(uint32_t p_bytes) override {
		(void)p_bytes;
		return true;
	}
	void _upload_clusters(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) override {
		(void)p_offset;
		(void)p_data;
		if (p_size > 0) {
			cluster_uploads++;
		}
	}
	bool _resize_residency(uint32_t p_bytes) override {
		(void)p_bytes;
		return true;
	}
	void _upload_residency(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) override {
		(void)p_offset;
		(void)p_data;
		if (p_size > 0) {
			residency_uploads++;
		}
	}
};

static RID make_test_rid(uint64_t p_id) {
	return RID::from_uint64(p_id);
}

static Vector<uint8_t> make_synthetic_raw_page(int32_t p_refined_group, float p_group_error = 8.0f) {
	// 1 cluster, 3 vertices (stride 3 floats = 36 bytes), 1 triangle (3 bytes), 0 skin bytes.
	// Total raw size = 16 + 80 + 36 + 3 = 135 bytes.
	Vector<uint8_t> raw;
	raw.resize(135);
	memset(raw.ptrw(), 0, raw.size());
	uint32_t hdr[4] = { 1u, 3u, 1u, 0u };
	memcpy(raw.ptrw(), hdr, sizeof(hdr));

	uint8_t *rec = raw.ptrw() + 16;
	const float sphere[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	const float parent_err = 1.0e30f;
	memcpy(rec + 0, sphere, sizeof(sphere));
	memcpy(rec + 20, sphere, sizeof(sphere));
	memcpy(rec + 36, &p_group_error, sizeof(float));
	memcpy(rec + 40, sphere, sizeof(sphere));
	memcpy(rec + 56, &parent_err, sizeof(float));
	memcpy(rec + 60, &p_refined_group, sizeof(int32_t));
	rec[76] = 3u; // vertex_count
	rec[77] = 1u; // triangle_count

	uint8_t *tris = raw.ptrw() + 16 + 80 + 36;
	tris[0] = 0u;
	tris[1] = 1u;
	tris[2] = 2u;
	return raw;
}

static Vector<uint8_t> make_synthetic_dag_header(const LocalVector<uint32_t> &p_group_pages) {
	const uint32_t group_count = p_group_pages.size();
	const uint32_t bvh_count = 1u;
	Vector<uint8_t> dag;
	dag.resize(int(8u + group_count * 60u + bvh_count * 80u));
	memset(dag.ptrw(), 0, dag.size());
	memcpy(dag.ptrw() + 0, &group_count, sizeof(uint32_t));
	memcpy(dag.ptrw() + 4, &bvh_count, sizeof(uint32_t));

	for (uint32_t g = 0; g < group_count; g++) {
		uint8_t *grp = dag.ptrw() + 8u + g * 60u;
		const uint32_t cluster_offset = g;
		const uint32_t cluster_count = 1u;
		const uint32_t page_id = p_group_pages[g];
		const uint32_t depth = g == 0 ? (group_count - 1u) : (group_count - 1u - g);
		const float sphere[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		const float parent_err = g == 0 ? 3.402823466e+38f : 16.0f;
		memcpy(grp + 0, &cluster_offset, sizeof(uint32_t));
		memcpy(grp + 4, &cluster_count, sizeof(uint32_t));
		memcpy(grp + 8, &page_id, sizeof(uint32_t));
		memcpy(grp + 12, &depth, sizeof(uint32_t));
		memcpy(grp + 16, sphere, sizeof(sphere));
		memcpy(grp + 32, &parent_err, sizeof(float));
	}

	uint8_t *node = dag.ptrw() + 8u + group_count * 60u;
	const float root_sphere[4] = { 0.0f, 0.0f, 0.0f, 2.0f };
	const float root_err = 3.402823466e+38f;
	const uint32_t child_count = MIN(group_count, 8u);
	memcpy(node + 0, root_sphere, sizeof(root_sphere));
	memcpy(node + 16, &root_err, sizeof(float));
	memcpy(node + 44, &child_count, sizeof(uint32_t));
	for (uint32_t c = 0; c < child_count; c++) {
		const uint32_t leaf = 0x80000000u | c;
		memcpy(node + 48u + c * 4u, &leaf, sizeof(uint32_t));
	}
	return dag;
}

#ifdef MODULE_NEXTGEO_ENABLED
static NextGeoBuildOutput build_multipage_wavy_dag() {
	const int nx = 72;
	const int nz = 72;
	NextGeoSurfaceInput surface;
	surface.eligible = true;
	surface.positions.resize((nx + 1) * (nz + 1));
	surface.normals.resize((nx + 1) * (nz + 1));
	surface.uv0.resize((nx + 1) * (nz + 1));
	for (int z = 0; z <= nz; z++) {
		for (int x = 0; x <= nx; x++) {
			const int idx = z * (nx + 1) + x;
			const float fx = (float(x) / float(nx) - 0.5f) * 12.0f;
			const float fz = (float(z) / float(nz) - 0.5f) * 12.0f;
			const float fy = 0.45f * Math::sin(fx * 1.1f) * Math::cos(fz * 0.9f);
			surface.positions.write[idx] = Vector3(fx, fy, fz);
			surface.normals.write[idx] = Vector3(0.0f, 1.0f, 0.0f);
			surface.uv0.write[idx] = Vector2(float(x) / float(nx), float(z) / float(nz));
		}
	}
	surface.indices.resize(nx * nz * 6);
	int w = 0;
	for (int z = 0; z < nz; z++) {
		for (int x = 0; x < nx; x++) {
			const int i0 = z * (nx + 1) + x;
			const int i1 = i0 + 1;
			const int i2 = i0 + (nx + 1);
			const int i3 = i2 + 1;
			surface.indices.write[w++] = i0;
			surface.indices.write[w++] = i1;
			surface.indices.write[w++] = i2;
			surface.indices.write[w++] = i1;
			surface.indices.write[w++] = i3;
			surface.indices.write[w++] = i2;
		}
	}

	NextGeoBuildInput input;
	input.surfaces.push_back(surface);
	// 16 KiB pages hold ~2 full clusters, guaranteeing a deep multi-page DAG.
	input.page_size = 16u * 1024u;
	input.use_threads = true;

	NextGeoBuildOutput dag;
	const Error err = nextgeo_build_dag(input, dag);
	REQUIRE_EQ(err, OK);
	return dag;
}

static float shader_projected_error_f32(const float p_sphere[4], float p_error, const Vector3 &p_eye, float p_scale, float p_znear, float p_height, float p_p11, float p_lod_bias, float p_tau) {
	if (p_error >= 3.4e38f) {
		return p_tau + 1.0f;
	}
	const Vector3 center(p_sphere[0] * p_scale, p_sphere[1] * p_scale, p_sphere[2] * p_scale);
	const float radius = p_sphere[3] * p_scale;
	const float dist = MAX(center.distance_to(p_eye) - radius, p_znear);
	return p_error * p_scale * 0.5f * p_height * p_p11 / dist * p_lod_bias;
}
#endif // MODULE_NEXTGEO_ENABLED

} // namespace TestNextGeoResidency

#ifdef MODULE_NEXTGEO_ENABLED
TEST_CASE("[NextGeo][Residency] 10k random parent-closed resident page sets produce valid partitions") {
	using namespace TestNextGeoResidency;

	const NextGeoBuildOutput dag = build_multipage_wavy_dag();
	REQUIRE(dag.pages.size() >= 16);
	REQUIRE(dag.pages.size() > dag.root_page_count);
	REQUIRE(dag.dag_depth >= 3);

	const uint32_t page_count = dag.pages.size();
	const uint32_t group_count = dag.groups.size();

	LocalVector<LocalVector<uint32_t>> group_pages;
	group_pages.resize(group_count);
	for (uint32_t g = 0; g < group_count; g++) {
		group_pages[g].push_back(dag.groups[g].page_id);
	}

	LocalVector<LocalVector<uint32_t>> page_parents;
	page_parents.resize(page_count);
	for (uint32_t c = 0; c < dag.clusters.size(); c++) {
		const NextGeoCluster &cl = dag.clusters[c];
		if (cl.refined_group >= 0) {
			const uint32_t child_page = dag.groups[uint32_t(cl.refined_group)].page_id;
			if (child_page != cl.page_id && !page_parents[child_page].has(cl.page_id)) {
				page_parents[child_page].push_back(cl.page_id);
			}
		}
	}

	RandomPCG rng(0x78A110CAULL);
	LocalVector<uint8_t> resident;
	resident.resize(page_count);
	LocalVector<uint32_t> refine_ok_bits;
	LocalVector<uint32_t> cut_clusters;

	uint32_t invalid_cuts = 0;
	uint32_t partial_cuts = 0;
	for (uint32_t iter = 0; iter < 10000; iter++) {
		const double keep_prob = rng.randd();
		bool all_resident = true;
		for (uint32_t p = 0; p < page_count; p++) {
			bool res = (p < dag.root_page_count) || (rng.randd() < keep_prob);
			for (uint32_t parent_p : page_parents[p]) {
				res = res && (resident[parent_p] != 0u);
			}
			resident[p] = res ? 1u : 0u;
			if (!res) {
				all_resident = false;
			}
		}
		if (!all_resident) {
			partial_cuts++;
		}

		RendererVirtualMeshStorage::compute_refine_ok_bits(
				group_count,
				group_pages,
				dag.root_page_count,
				resident.ptr(),
				page_count,
				refine_ok_bits);

		auto is_refine_ok = [&](uint32_t p_g) -> bool {
			return p_g < group_count && (refine_ok_bits[p_g >> 5u] & (1u << (p_g & 31u))) != 0u;
		};

		const Vector3 eye(
				rng.random(-14.0f, 14.0f),
				rng.random(0.6f, 16.0f),
				rng.random(-14.0f, 14.0f));
		const float scale = rng.random(0.5f, 2.5f);
		const float height = 1080.0f;
		const float fov_y = rng.random(0.7f, 1.4f);
		const float p11 = 1.0f / Math::tan(fov_y * 0.5f);
		const float tau = rng.random(0.25f, 6.0f);
		const float znear = 0.01f;
		const float lod_bias = 1.0f;

		// Traverse BVH + evaluate cluster_cull.glsl's exact float32 refine_ok predicate.
		cut_clusters.clear();
		LocalVector<uint32_t> stack;
		if (!dag.bvh.is_empty()) {
			stack.push_back(0u);
		}
		LocalVector<uint32_t> leaf_groups;
		while (!stack.is_empty()) {
			const uint32_t n_idx = stack[stack.size() - 1];
			stack.remove_at(stack.size() - 1);
			const NextGeoBVHNode &node = dag.bvh[n_idx];
			if (shader_projected_error_f32(node.lod_sphere, node.max_parent_error, eye, scale, znear, height, p11, lod_bias, tau) <= tau) {
				continue;
			}
			for (uint32_t ch = 0; ch < MIN(node.child_count, 8u); ch++) {
				const uint32_t v = node.children[ch];
				if ((v & 0x80000000u) != 0u) {
					leaf_groups.push_back(v & ~0x80000000u);
				} else if (v < dag.bvh.size()) {
					stack.push_back(v);
				}
			}
		}

		for (uint32_t g_idx : leaf_groups) {
			if (g_idx >= group_count || !is_refine_ok(g_idx)) {
				continue;
			}
			const NextGeoGroup &grp = dag.groups[g_idx];
			if (shader_projected_error_f32(grp.parent_lod, grp.parent_error, eye, scale, znear, height, p11, lod_bias, tau) <= tau) {
				continue;
			}
			for (uint32_t c = 0; c < grp.cluster_count; c++) {
				const uint32_t cl_idx = grp.cluster_offset + c;
				const NextGeoCluster &cl = dag.clusters[cl_idx];
				if (cl.refined_group >= 0 && uint32_t(cl.refined_group) < group_count) {
					const float child_px = shader_projected_error_f32(cl.group_lod, cl.group_error, eye, scale, znear, height, p11, lod_bias, tau);
					if (child_px > tau && is_refine_ok(uint32_t(cl.refined_group))) {
						continue;
					}
				}
				cut_clusters.push_back(cl_idx);
			}
		}

		const NextGeoCutCheck check = nextgeo_check_cut(dag, cut_clusters);
		if (!check.is_valid_partition()) {
			invalid_cuts++;
		}
	}

	CHECK_GT(partial_cuts, 8000u);
	CHECK_EQ(invalid_cuts, 0u);
}
#endif // MODULE_NEXTGEO_ENABLED

TEST_CASE("[NextGeo][Residency] Parents-first upload ordering, main-view gating, and residency events") {
	using namespace TestNextGeoResidency;

	// Build a 4-level chain: page 0 (pinned root) -> page 1 -> page 2 -> page 3 (leaf).
	LocalVector<uint32_t> group_pages;
	group_pages.push_back(0u);
	group_pages.push_back(1u);
	group_pages.push_back(2u);
	group_pages.push_back(3u);

	Vector<Vector<uint8_t>> all_pages;
	all_pages.resize(4);
	all_pages.write[0] = make_synthetic_raw_page(1);
	all_pages.write[1] = make_synthetic_raw_page(2);
	all_pages.write[2] = make_synthetic_raw_page(3);
	all_pages.write[3] = make_synthetic_raw_page(-1, 0.0f);

	RenderingServerTypes::VirtualGeometryDesc desc;
	desc.dag_header = make_synthetic_dag_header(group_pages);
	desc.root_pages.push_back(all_pages[0]);
	desc.eligible_surface_mask = 1u;
	desc.vertex_layout = 1u; // position (3 floats)
	desc.page_size = 256u;
	desc.page_count = 4u;

	MockVirtualMeshStorage storage;
	// Allow 16 slots of 256 B, and cap uploads to 1 page (135 B <= 200 B < 270 B) per frame.
	storage.set_page_arena_bytes_override(16u * 256u);
	storage.set_page_upload_bytes_per_frame_override(200u);

	const RID mesh = make_test_rid(101);
	storage.mesh_set_in_memory_pages(mesh, all_pages);
	storage.mesh_set_virtual_geometry(mesh, desc);
	const uint32_t rec_slot = storage.instance_record_allocate(make_test_rid(201), mesh);
	REQUIRE_NE(rec_slot, UINT32_MAX);

	CHECK(storage.mesh_is_page_resident(mesh, 0u));
	CHECK_FALSE(storage.mesh_is_page_resident(mesh, 1u));
	CHECK_FALSE(storage.mesh_is_page_resident(mesh, 2u));
	CHECK_FALSE(storage.mesh_is_page_resident(mesh, 3u));
	CHECK_EQ(storage.get_pinned_page_count(), 1u);

	// Auxiliary / non-main view requests are rejected and counted.
	RendererVirtualMeshStorage::PageRequest aux_req;
	aux_req.mesh_index = 0u;
	aux_req.page_id = 3u;
	aux_req.visible = true;
	aux_req.distance = 2.0f;
	aux_req.px_error = 10.0f;
	storage.submit_page_requests(&aux_req, 1u, false);
	CHECK_EQ(storage.get_non_main_view_request_count(), 1u);

	storage.begin_frame();
	storage.flush();
	storage.wait_for_streaming_tasks();
	CHECK_FALSE(storage.mesh_is_page_resident(mesh, 1u));
	CHECK_EQ(storage.get_residency_changed_records().size(), 0u);

	// Main-view request for ONLY leaf page 3 must automatically expand & upload 1 -> 2 -> 3 in order.
	storage.submit_page_requests(&aux_req, 1u, true);
	CHECK_EQ(storage.get_non_main_view_request_count(), 1u); // Unchanged on main-view request.

	// Kick worker batch on frame N, then upload 1 page per frame over the next 3 frames.
	storage.begin_frame();
	storage.flush();
	storage.wait_for_streaming_tasks();

	// Frame +1: parent page 1 becomes resident first; pages 2 and 3 wait.
	storage.begin_frame();
	storage.flush();
	CHECK(storage.mesh_is_page_resident(mesh, 1u));
	CHECK_FALSE(storage.mesh_is_page_resident(mesh, 2u));
	CHECK_FALSE(storage.mesh_is_page_resident(mesh, 3u));
	CHECK_EQ(storage.get_child_resident_without_parent_count(), 0u);
	REQUIRE_EQ(storage.get_residency_changed_records().size(), 1u);
	CHECK_EQ(storage.get_residency_changed_records()[0], rec_slot);

	// Frame +2: page 2 becomes resident; leaf page 3 waits.
	storage.begin_frame();
	storage.flush();
	CHECK(storage.mesh_is_page_resident(mesh, 1u));
	CHECK(storage.mesh_is_page_resident(mesh, 2u));
	CHECK_FALSE(storage.mesh_is_page_resident(mesh, 3u));
	CHECK_EQ(storage.get_child_resident_without_parent_count(), 0u);
	CHECK_EQ(storage.get_residency_changed_records().size(), 1u);

	// Frame +3: leaf page 3 becomes resident.
	storage.begin_frame();
	storage.flush();
	CHECK(storage.mesh_is_page_resident(mesh, 3u));
	CHECK(storage.mesh_is_group_refine_ok_rid(mesh, 3u));
	CHECK_EQ(storage.get_child_resident_without_parent_count(), 0u);
	CHECK_EQ(storage.get_residency_changed_records().size(), 1u);

	// Frame +4: idle frame emits zero residency change events.
	storage.begin_frame();
	storage.flush();
	CHECK_EQ(storage.get_residency_changed_records().size(), 0u);
}

TEST_CASE("[NextGeo][Residency] Child-first LRU eviction over unused pages and pinned roots") {
	using namespace TestNextGeoResidency;

	// Mesh A: root 0 -> parent 1 -> leaf 2.
	LocalVector<uint32_t> groups_a;
	groups_a.push_back(0u);
	groups_a.push_back(1u);
	groups_a.push_back(2u);
	Vector<Vector<uint8_t>> pages_a;
	pages_a.resize(3);
	pages_a.write[0] = make_synthetic_raw_page(1);
	pages_a.write[1] = make_synthetic_raw_page(2);
	pages_a.write[2] = make_synthetic_raw_page(-1, 0.0f);

	RenderingServerTypes::VirtualGeometryDesc desc_a;
	desc_a.dag_header = make_synthetic_dag_header(groups_a);
	desc_a.root_pages.push_back(pages_a[0]);
	desc_a.eligible_surface_mask = 1u;
	desc_a.vertex_layout = 1u;
	desc_a.page_size = 256u;
	desc_a.page_count = 3u;

	// Mesh B: root 0 -> leaf 1, leaf 2 (two independent children of root 0).
	LocalVector<uint32_t> groups_b;
	groups_b.push_back(0u);
	groups_b.push_back(1u);
	groups_b.push_back(2u);
	Vector<Vector<uint8_t>> pages_b;
	pages_b.resize(3);
	pages_b.write[0] = make_synthetic_raw_page(1);
	pages_b.write[1] = make_synthetic_raw_page(-1, 0.0f);
	pages_b.write[2] = make_synthetic_raw_page(-1, 0.0f);

	LocalVector<LocalVector<uint32_t>> parents_b;
	parents_b.resize(3);
	parents_b[1].push_back(0u);
	parents_b[2].push_back(0u);

	RenderingServerTypes::VirtualGeometryDesc desc_b;
	desc_b.dag_header = make_synthetic_dag_header(groups_b);
	desc_b.root_pages.push_back(pages_b[0]);
	desc_b.eligible_surface_mask = 1u;
	desc_b.vertex_layout = 1u;
	desc_b.page_size = 256u;
	desc_b.page_count = 3u;

	MockVirtualMeshStorage storage;
	// Exactly 4 slots total: 2 pinned roots (mesh A root 0 + mesh B root 0) + 2 evictable slots!
	storage.set_page_arena_bytes_override(4u * 256u);
	storage.set_page_upload_bytes_per_frame_override(4096u);

	const RID mesh_a = make_test_rid(301);
	const RID mesh_b = make_test_rid(302);
	storage.mesh_set_in_memory_pages(mesh_a, pages_a);
	storage.mesh_set_virtual_geometry(mesh_a, desc_a);
	storage.mesh_set_in_memory_pages(mesh_b, pages_b);
	storage.mesh_set_virtual_geometry(mesh_b, desc_b);
	storage.mesh_set_page_parents(mesh_b, parents_b);

	CHECK_EQ(storage.get_page_slot_count(), 4u);
	CHECK_EQ(storage.get_pinned_page_count(), 2u);

	// Stream Mesh A's parent 1 + leaf 2 into the 2 free slots.
	RendererVirtualMeshStorage::PageRequest req_a2 = { 0u, 2u, true, 5.0f, 8.0f };
	storage.submit_page_requests(&req_a2, 1u, true);
	storage.begin_frame();
	storage.flush();
	storage.wait_for_streaming_tasks();
	storage.begin_frame();
	storage.flush();
	CHECK(storage.mesh_is_page_resident(mesh_a, 1u));
	CHECK(storage.mesh_is_page_resident(mesh_a, 2u));

	// Now request Mesh B's leaf 1 (1 page upload cap) on a new frame:
	// Mesh A's parent 1 has resident child 2, so ONLY leaf 2 may be evicted first!
	storage.set_page_upload_bytes_per_frame_override(200u);
	RendererVirtualMeshStorage::PageRequest req_b[2] = {
		{ 1u, 1u, true, 2.0f, 20.0f },
		{ 1u, 2u, true, 3.0f, 15.0f },
	};
	storage.submit_page_requests(req_b, 2u, true);
	storage.begin_frame();
	storage.flush();
	storage.wait_for_streaming_tasks();

	// Upload B.1: must evict A.2 (leaf), keeping A.1 resident!
	storage.begin_frame();
	storage.flush();
	CHECK_FALSE(storage.mesh_is_page_resident(mesh_a, 2u));
	CHECK(storage.mesh_is_page_resident(mesh_a, 1u));
	CHECK(storage.mesh_is_page_resident(mesh_b, 1u));
	CHECK_EQ(storage.get_evictions_with_resident_child_count(), 0u);

	// Next frame: A.1 now has 0 resident children, so B.2 can evict A.1 cleanly.
	storage.begin_frame();
	storage.flush();
	CHECK_FALSE(storage.mesh_is_page_resident(mesh_a, 1u));
	CHECK(storage.mesh_is_page_resident(mesh_b, 1u));
	CHECK(storage.mesh_is_page_resident(mesh_b, 2u));
	// Pinned roots never evict.
	CHECK(storage.mesh_is_page_resident(mesh_a, 0u));
	CHECK(storage.mesh_is_page_resident(mesh_b, 0u));
	CHECK_EQ(storage.get_evictions_with_resident_child_count(), 0u);
	CHECK_EQ(storage.get_child_resident_without_parent_count(), 0u);
	CHECK_EQ(storage.get_total_page_evictions(), 2u);
}

TEST_CASE("[NextGeo][Residency] Priority ordering: visible > front-to-back distance > pixel error") {
	using namespace TestNextGeoResidency;

	LocalVector<uint32_t> groups;
	for (uint32_t g = 0; g < 5u; g++) {
		groups.push_back(g);
	}
	Vector<Vector<uint8_t>> pages;
	pages.resize(5);
	pages.write[0] = make_synthetic_raw_page(1);
	for (int p = 1; p < 5; p++) {
		pages.write[p] = make_synthetic_raw_page(-1, 0.0f);
	}
	LocalVector<LocalVector<uint32_t>> parents;
	parents.resize(5);
	for (uint32_t p = 1; p < 5; p++) {
		parents[p].push_back(0u);
	}

	RenderingServerTypes::VirtualGeometryDesc desc;
	desc.dag_header = make_synthetic_dag_header(groups);
	desc.root_pages.push_back(pages[0]);
	desc.eligible_surface_mask = 1u;
	desc.vertex_layout = 1u;
	desc.page_size = 256u;
	desc.page_count = 5u;

	MockVirtualMeshStorage storage;
	storage.set_page_arena_bytes_override(16u * 256u);
	storage.set_page_upload_bytes_per_frame_override(200u); // 1 page per frame.

	const RID mesh = make_test_rid(401);
	storage.mesh_set_in_memory_pages(mesh, pages);
	storage.mesh_set_virtual_geometry(mesh, desc);
	storage.mesh_set_page_parents(mesh, parents);

	const RendererVirtualMeshStorage::PageRequest reqs[4] = {
		{ 0u, 1u, false, 1.0f, 500.0f }, // Invisible despite closest distance & largest error -> 4th.
		{ 0u, 2u, true, 40.0f, 100.0f }, // Visible, far distance -> 3rd.
		{ 0u, 3u, true, 10.0f, 3.0f }, // Visible, near distance, lower error -> 2nd.
		{ 0u, 4u, true, 10.0f, 12.0f }, // Visible, near distance, higher error -> 1st.
	};
	storage.submit_page_requests(reqs, 4u, true);

	storage.begin_frame();
	storage.flush();
	storage.wait_for_streaming_tasks();

	// Frame 1 -> page 4.
	storage.begin_frame();
	storage.flush();
	CHECK(storage.mesh_is_page_resident(mesh, 4u));
	CHECK_FALSE(storage.mesh_is_page_resident(mesh, 3u));
	CHECK_FALSE(storage.mesh_is_page_resident(mesh, 2u));
	CHECK_FALSE(storage.mesh_is_page_resident(mesh, 1u));

	// Frame 2 -> page 3.
	storage.begin_frame();
	storage.flush();
	CHECK(storage.mesh_is_page_resident(mesh, 3u));
	CHECK_FALSE(storage.mesh_is_page_resident(mesh, 2u));
	CHECK_FALSE(storage.mesh_is_page_resident(mesh, 1u));

	// Frame 3 -> page 2.
	storage.begin_frame();
	storage.flush();
	CHECK(storage.mesh_is_page_resident(mesh, 2u));
	CHECK_FALSE(storage.mesh_is_page_resident(mesh, 1u));

	// Frame 4 -> page 1 (non-visible).
	storage.begin_frame();
	storage.flush();
	CHECK(storage.mesh_is_page_resident(mesh, 1u));
}

#ifdef MODULE_NEXTGEO_ENABLED
TEST_CASE("[NextGeo][Residency] .ngm streaming and deterministic seeded wall-time IO delay") {
	using namespace TestNextGeoResidency;

	const String ngm_path = TestUtils::get_temp_path("nextgeo_residency_delay.ngm");
	Ref<SphereMesh> sphere;
	sphere.instantiate();
	sphere->set_radius(2.0f);
	sphere->set_height(4.0f);
	sphere->set_radial_segments(96);
	sphere->set_rings(48);

	Ref<ImporterMesh> imp = ImporterMesh::from_mesh(sphere);
	REQUIRE(imp.is_valid());
	REQUIRE_EQ(imp->build_virtual_geometry(ngm_path), OK);
	Ref<VirtualGeometryData> vgd = imp->get_virtual_geometry();
	REQUIRE(vgd.is_valid());

	Ref<NGMFile> check_ngm;
	check_ngm.instantiate();
	REQUIRE_EQ(check_ngm->open(ngm_path), OK);
	const uint32_t page_count = check_ngm->get_page_count();
	const uint32_t root_count = check_ngm->get_header().root_page_count;
	REQUIRE_GT(page_count, root_count + 2u);

	RenderingServerTypes::VirtualGeometryDesc desc;
	desc.dag_header = vgd->get_dag_header();
	const TypedArray<PackedByteArray> roots = vgd->get_root_pages();
	for (int i = 0; i < roots.size(); i++) {
		desc.root_pages.push_back(roots[i]);
	}
	desc.ngm_path = ngm_path;
	desc.eligible_surface_mask = 1u;
	desc.vertex_layout = vgd->get_ngm_vertex_layout();
	desc.page_size = uint32_t(vgd->get_page_size());
	desc.page_count = page_count;
	desc.stripped_to_coarse = true;

	const Variant prev_delay = ProjectSettings::get_singleton()->get_setting("rendering/nextgeo/streaming/debug_io_delay_ms");
	ProjectSettings::get_singleton()->set_setting("rendering/nextgeo/streaming/debug_io_delay_ms", Vector2(2.0f, 7.0f));

	auto run_seeded_stream = [&](uint64_t p_seed, uint64_t &r_elapsed_usec) -> Vector<uint64_t> {
		MockVirtualMeshStorage storage;
		storage.set_io_delay_seed(p_seed);
		const RID mesh = make_test_rid(501);
		storage.mesh_set_virtual_geometry(mesh, desc);

		RendererVirtualMeshStorage::PageRequest req;
		req.mesh_index = 0u;
		req.page_id = page_count - 1u;
		req.visible = true;
		req.distance = 3.0f;
		req.px_error = 24.0f;
		storage.submit_page_requests(&req, 1u, true);

		const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
		// Step 1 scans .ngm cluster headers; step 2 dispatches delayed page reads; step 3 uploads.
		for (int step = 0; step < 6 && !storage.mesh_is_page_resident(mesh, page_count - 1u); step++) {
			storage.begin_frame();
			storage.flush();
			storage.wait_for_streaming_tasks();
		}
		r_elapsed_usec = OS::get_singleton()->get_ticks_usec() - t0;
		CHECK(storage.mesh_is_page_resident(mesh, page_count - 1u));
		CHECK_EQ(storage.get_child_resident_without_parent_count(), 0u);
		return storage.get_recorded_io_delays_usec();
	};

	uint64_t elapsed_a = 0;
	uint64_t elapsed_b = 0;
	const Vector<uint64_t> delays_a = run_seeded_stream(0xC0FFEE78ULL, elapsed_a);
	const Vector<uint64_t> delays_b = run_seeded_stream(0xC0FFEE78ULL, elapsed_b);
	REQUIRE_GE(delays_a.size(), 3);
	CHECK_EQ(delays_a, delays_b); // Same seed, same delays.

	uint64_t sum_delays = 0;
	for (int i = 0; i < delays_a.size(); i++) {
		CHECK_GE(delays_a[i], uint64_t(2000));
		CHECK_LE(delays_a[i], uint64_t(7000));
		sum_delays += delays_a[i];
	}
	CHECK_GE(elapsed_a + 1000ULL, delays_a[0]);
	CHECK_GT(sum_delays, uint64_t(0));

	uint64_t elapsed_c = 0;
	const Vector<uint64_t> delays_other_seed = run_seeded_stream(0x9988776655ULL, elapsed_c);
	CHECK_NE(delays_a, delays_other_seed);

	// Zero delay setting injects 0 usec delays.
	ProjectSettings::get_singleton()->set_setting("rendering/nextgeo/streaming/debug_io_delay_ms", Vector2(0.0f, 0.0f));
	uint64_t elapsed_zero = 0;
	const Vector<uint64_t> delays_zero = run_seeded_stream(0xC0FFEE78ULL, elapsed_zero);
	for (int i = 0; i < delays_zero.size(); i++) {
		CHECK_EQ(delays_zero[i], uint64_t(0));
	}

	ProjectSettings::get_singleton()->set_setting("rendering/nextgeo/streaming/debug_io_delay_ms", prev_delay);
	if (FileAccess::exists(ngm_path)) {
		DirAccess::remove_absolute(ngm_path);
	}
}
#endif // MODULE_NEXTGEO_ENABLED

TEST_CASE("[NextGeo][Residency] Q15 memory report and routing counters surface") {
	RenderingServer *rs = RenderingServer::get_singleton();
	REQUIRE(rs != nullptr);

	const Dictionary mem = rs->virtual_geometry_get_memory_report();
	const char *expected_mem_keys[10] = {
		"authored_remainder",
		"lods",
		"shadow_mesh",
		"coarse_fallback",
		"rt_arena",
		"blas",
		"tlas",
		"as_scratch",
		"dag_headers",
		"page_cache",
	};
	for (const char *key : expected_mem_keys) {
		CHECK_MESSAGE(mem.has(key), "Missing memory report key: ", key);
		CHECK_GE(int64_t(mem[key]), 0);
	}
	if (RSG::virtual_mesh_storage != nullptr) {
		CHECK_EQ(int64_t(mem["page_cache"]), int64_t(RSG::virtual_mesh_storage->get_page_arena_allocation_bytes()));
		CHECK_EQ(int64_t(mem["dag_headers"]), int64_t(RSG::virtual_mesh_storage->get_dag_allocation_bytes()));
	}

	const Dictionary routing = rs->virtual_geometry_get_routing_counters();
	const char *expected_routing_keys[10] = {
		"page_upload_ms",
		"double_dispatch",
		"missing",
		"child_resident_without_parent",
		"evictions_with_resident_child",
		"non_main_view_requests",
		"page_uploads",
		"page_evictions",
		"resident_pages",
		"pinned_pages",
	};
	for (const char *key : expected_routing_keys) {
		CHECK_MESSAGE(routing.has(key), "Missing routing counter key: ", key);
	}
	CHECK_GE(double(routing["page_upload_ms"]), 0.0);
	CHECK_EQ(int64_t(routing["child_resident_without_parent"]), 0);
	CHECK_EQ(int64_t(routing["evictions_with_resident_child"]), 0);
}

TEST_CASE("[NextGeo][Residency][Shadow] Residency epoch, instance lookup, and surface cull masks") {
	using namespace TestNextGeoResidency;

	LocalVector<uint32_t> groups;
	groups.push_back(0u);
	groups.push_back(1u);
	Vector<Vector<uint8_t>> pages;
	pages.resize(2);
	pages.write[0] = make_synthetic_raw_page(1);
	pages.write[1] = make_synthetic_raw_page(-1, 0.0f);

	RenderingServerTypes::VirtualGeometryDesc desc;
	desc.dag_header = make_synthetic_dag_header(groups);
	desc.root_pages.push_back(pages[0]);
	desc.eligible_surface_mask = 1u;
	desc.vertex_layout = 1u;
	desc.page_size = 256u;
	desc.page_count = 2u;

	MockVirtualMeshStorage storage;
	storage.set_page_arena_bytes_override(16u * 256u);
	storage.set_page_upload_bytes_per_frame_override(200u);

	const RID mesh = make_test_rid(8201);
	const RID inst = make_test_rid(8202);
	storage.mesh_set_in_memory_pages(mesh, pages);
	storage.mesh_set_virtual_geometry(mesh, desc);

	const uint32_t slot = storage.instance_record_allocate(inst, mesh);
	REQUIRE_NE(slot, UINT32_MAX);
	CHECK_EQ(storage.instance_record_get_instance(slot), inst);
	CHECK_EQ(storage.instance_record_get_instance(9999u), RID());

	storage.instance_record_set_surface_cull_masks(slot, 0x5u, 0x2u);
	CHECK_EQ(storage.get_record(slot).pad1[0], 0x5u);
	CHECK_EQ(storage.get_record(slot).pad1[1], 0x2u);

	// instance_record_set_state clears RECORD_VISIBLE when hidden and preserves transform visibility when re-shown.
	storage.instance_record_set_state(slot, RendererVirtualMeshStorage::RECORD_VALID | RendererVirtualMeshStorage::RECORD_CAST_SHADOW_SHADOWS_ONLY);
	CHECK_EQ(storage.get_record(slot).flags & RendererVirtualMeshStorage::RECORD_VISIBLE, 0u);
	CHECK_EQ(storage.get_record(slot).flags & RendererVirtualMeshStorage::RECORD_CAST_SHADOW_MASK, RendererVirtualMeshStorage::RECORD_CAST_SHADOW_SHADOWS_ONLY);
	storage.instance_record_set_transform(slot, Transform3D(), AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2)), true);
	CHECK_NE(storage.get_record(slot).flags & RendererVirtualMeshStorage::RECORD_VISIBLE, 0u);
	storage.instance_record_set_state(slot, RendererVirtualMeshStorage::RECORD_VALID | RendererVirtualMeshStorage::RECORD_CAST_SHADOW_OFF);
	CHECK_EQ(storage.get_record(slot).flags & RendererVirtualMeshStorage::RECORD_VISIBLE, 0u);
	CHECK_EQ(storage.get_record(slot).flags & RendererVirtualMeshStorage::RECORD_CAST_SHADOW_MASK, RendererVirtualMeshStorage::RECORD_CAST_SHADOW_OFF);
	storage.instance_record_set_transform(slot, Transform3D(), AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2)), true);

	const uint64_t epoch0 = storage.get_residency_event_epoch();

	// Clean frame: process_streaming() does not bump epoch.
	storage.begin_frame();
	storage.process_streaming();
	CHECK_EQ(storage.get_residency_event_epoch(), epoch0);
	CHECK_EQ(storage.get_residency_changed_records().size(), 0u);

	// Request page 1: kick worker on frame N, upload on frame N+1.
	RendererVirtualMeshStorage::PageRequest req;
	req.mesh_index = 0u;
	req.page_id = 1u;
	req.visible = true;
	req.distance = 4.0f;
	req.px_error = 12.0f;
	storage.submit_page_requests(&req, 1u, true);
	storage.begin_frame();
	storage.process_streaming();
	storage.wait_for_streaming_tasks();
	CHECK_EQ(storage.get_residency_event_epoch(), epoch0);

	// Upload frame: process_streaming() bumps epoch once and flush() within the same frame is idempotent.
	storage.begin_frame();
	storage.process_streaming();
	const uint64_t epoch1 = storage.get_residency_event_epoch();
	CHECK_EQ(epoch1, epoch0 + 1u);
	REQUIRE_EQ(storage.get_residency_changed_records().size(), 1u);
	CHECK_EQ(storage.get_residency_changed_records()[0], slot);
	storage.flush();
	CHECK_EQ(storage.get_residency_event_epoch(), epoch1);

	// Subsequent clean frame: epoch stays steady and changed_records clears.
	storage.begin_frame();
	storage.process_streaming();
	CHECK_EQ(storage.get_residency_event_epoch(), epoch1);
	CHECK_EQ(storage.get_residency_changed_records().size(), 0u);
}

#endif // _3D_DISABLED
