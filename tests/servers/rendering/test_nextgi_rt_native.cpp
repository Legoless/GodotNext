/**************************************************************************/
/*  test_nextgi_rt_native.cpp                                             */
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

TEST_FORCE_LINK(test_nextgi_rt_native)

// GodotNext: nextgeo_rt_native (#156). Native Nanite ray tracing (EXPERIMENTAL): the device-free
// parts of the bucket cut, part split, arena layout, caps and Stage-2 card mapping.

#if !defined(_3D_DISABLED) && defined(RD_ENABLED)

#include "core/config/project_settings.h"
#include "core/templates/sort_array.h"
#include "scene/resources/3d/virtual_geometry_data.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_native.h"
#include "servers/rendering/storage/virtual_mesh_storage.h"

#include "modules/modules_enabled.gen.h"
#ifdef MODULE_NEXTGEO_ENABLED
#include "modules/nextgeo/nextgeo_cut.h"
#include "modules/nextgeo/nextgeo_dag_builder.h"
#include "modules/nextgeo/nextgeo_rt_proxy.h"
#include "modules/nextgeo/nextgeo_serialize.h"
#endif

#include <cstring>
#include <limits>

namespace TestNextGIRTNative {

using namespace RendererRD;

class MockVirtualMeshStorage : public RendererVirtualMeshStorage {
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
};

// A DAG header (plan §6.3): 8 bytes, then 60-byte groups (clusters, page, depth, parent LOD sphere,
// parent error, bounds). No BVH nodes: the native helpers read the groups only.
struct SyntheticGroup {
	float parent_error = 0.0f;
	Vector3 lo;
	Vector3 hi;
};

static Vector<uint8_t> make_dag_header(const LocalVector<SyntheticGroup> &p_groups) {
	Vector<uint8_t> dag;
	dag.resize(int64_t(8 + p_groups.size() * 60));
	memset(dag.ptrw(), 0, dag.size());
	const uint32_t group_count = p_groups.size();
	memcpy(dag.ptrw(), &group_count, sizeof(uint32_t));
	for (uint32_t g = 0; g < group_count; g++) {
		uint8_t *group = dag.ptrw() + 8 + g * 60;
		const uint32_t cluster_offset = g;
		const uint32_t cluster_count = 1;
		memcpy(group + 0, &cluster_offset, sizeof(uint32_t));
		memcpy(group + 4, &cluster_count, sizeof(uint32_t));
		memcpy(group + 32, &p_groups[g].parent_error, sizeof(float));
		const float lo[3] = { p_groups[g].lo.x, p_groups[g].lo.y, p_groups[g].lo.z };
		const float hi[3] = { p_groups[g].hi.x, p_groups[g].hi.y, p_groups[g].hi.z };
		memcpy(group + 36, lo, sizeof(lo));
		memcpy(group + 48, hi, sizeof(hi));
	}
	return dag;
}

static void append_card(PackedByteArray &r_cards, uint16_t p_group, uint8_t p_axis, const Vector3 &p_center, const Vector3 &p_half) {
	const int64_t at = r_cards.size();
	r_cards.resize(at + 32);
	uint8_t *card = r_cards.ptrw() + at;
	memset(card, 0, 32);
	memcpy(card + 0, &p_group, sizeof(uint16_t));
	card[2] = p_axis;
	const float center[3] = { p_center.x, p_center.y, p_center.z };
	const float half[3] = { p_half.x, p_half.y, p_half.z };
	memcpy(card + 4, center, sizeof(center));
	memcpy(card + 16, half, sizeof(half));
}

#ifdef MODULE_NEXTGEO_ENABLED
// A multi-page wavy grid: a deep DAG with many pages (the residency doctests use the same shape).
static NextGeoBuildInput make_wavy_input() {
	const int nx = 72;
	const int nz = 72;
	NextGeoSurfaceInput surface;
	surface.eligible = true;
	surface.positions.resize((nx + 1) * (nz + 1));
	surface.normals.resize((nx + 1) * (nz + 1));
	for (int z = 0; z <= nz; z++) {
		for (int x = 0; x <= nx; x++) {
			const int idx = z * (nx + 1) + x;
			const float fx = (float(x) / float(nx) - 0.5f) * 12.0f;
			const float fz = (float(z) / float(nz) - 0.5f) * 12.0f;
			const float fy = 0.45f * Math::sin(fx * 1.1f) * Math::cos(fz * 0.9f);
			surface.positions.write[idx] = Vector3(fx, fy, fz);
			surface.normals.write[idx] = Vector3(0.0f, 1.0f, 0.0f);
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
	input.page_size = 16u * 1024u;
	input.use_threads = false;
	return input;
}

struct StreamedMesh {
	NextGeoBuildInput input;
	NextGeoBuildOutput dag;
	Vector<Vector<uint8_t>> pages;
	RenderingServerTypes::VirtualGeometryDesc desc;
};

static void make_streamed_mesh(StreamedMesh &r_mesh) {
	r_mesh.input = make_wavy_input();
	REQUIRE(nextgeo_build_dag(r_mesh.input, r_mesh.dag) == OK);
	REQUIRE(r_mesh.dag.pages.size() > r_mesh.dag.root_page_count + 2);
	REQUIRE(nextgeo_write_pages(r_mesh.dag, r_mesh.pages) == OK);
	Ref<VirtualGeometryData> vgd;
	vgd.instantiate();
	REQUIRE(nextgeo_fill_vgd(r_mesh.input, r_mesh.dag, r_mesh.pages, vgd) == OK);
	r_mesh.desc.dag_header = vgd->get_dag_header();
	const TypedArray<PackedByteArray> roots = vgd->get_root_pages();
	for (int i = 0; i < roots.size(); i++) {
		r_mesh.desc.root_pages.push_back(roots[i]);
	}
	r_mesh.desc.eligible_surface_mask = 1u;
	r_mesh.desc.vertex_layout = vgd->get_ngm_vertex_layout();
	r_mesh.desc.page_size = uint32_t(vgd->get_page_size());
	r_mesh.desc.page_count = uint32_t(r_mesh.pages.size());
}

// Makes the requested pages resident (their parents come first) in a mock storage.
static void stream_pages(MockVirtualMeshStorage &r_storage, RID p_rid, const StreamedMesh &p_data, const LocalVector<uint32_t> &p_pages) {
	r_storage.set_page_arena_bytes_override(uint64_t(p_data.pages.size() + 8) * 64ull * 1024ull);
	r_storage.set_page_upload_bytes_per_frame_override(256ull * 1024ull * 1024ull);
	r_storage.mesh_set_in_memory_pages(p_rid, p_data.pages);
	r_storage.mesh_set_virtual_geometry(p_rid, p_data.desc);
	REQUIRE(r_storage.instance_record_allocate(RID::from_uint64(p_rid.get_id() + 1), p_rid) != UINT32_MAX);
	LocalVector<RendererVirtualMeshStorage::PageRequest> requests;
	for (uint32_t page : p_pages) {
		RendererVirtualMeshStorage::PageRequest request;
		request.mesh_index = 0;
		request.page_id = page;
		request.visible = true;
		request.distance = 1.0f;
		request.px_error = 100.0f;
		requests.push_back(request);
	}
	for (int step = 0; step < 64; step++) {
		bool done = true;
		for (uint32_t page : p_pages) {
			done = done && r_storage.mesh_is_page_resident(p_rid, page);
		}
		if (done) {
			break;
		}
		if (!requests.is_empty()) {
			r_storage.submit_page_requests(requests.ptr(), requests.size(), true);
		}
		r_storage.begin_frame();
		r_storage.flush();
		r_storage.wait_for_streaming_tasks();
	}
	for (uint32_t page : p_pages) {
		REQUIRE(r_storage.mesh_is_page_resident(p_rid, page));
	}
}

static bool same_clusters(const LocalVector<uint32_t> &p_a, const LocalVector<uint32_t> &p_b) {
	if (p_a.size() != p_b.size()) {
		return false;
	}
	for (uint32_t i = 0; i < p_a.size(); i++) {
		if (p_a[i] != p_b[i]) {
			return false;
		}
	}
	return true;
}

static LocalVector<uint32_t> global_clusters(const NextGeoRTNativeCut &p_cut, uint32_t p_first) {
	LocalVector<uint32_t> out;
	for (uint32_t cluster : p_cut.clusters) {
		out.push_back(cluster + p_first);
	}
	out.sort();
	return out;
}
#endif // MODULE_NEXTGEO_ENABLED

// A device-free backend for the bucket state machine (NextGeoRTNativeSets): each mesh's cuts,
// proxy bytes and liveness are scripted; parts split at part_max_triangles, a BLAS takes
// bytes_per_triangle per triangle, and every BLAS and RT-arena range is counted in and out.
class FakeNativeSets : public NextGeoRTNativeSets {
public:
	struct Level {
		float error = 0.0f; // The level is the cut for targets from this error up to the next level's.
		uint32_t triangles = 0;
		float max_error = 0.0f; // The cut's own error (above the target when pages are missing).
	};
	struct FakeMesh {
		bool alive = true;
		bool proxy_built = true;
		float eps = 0.02f;
		uint64_t generation = 1;
		uint64_t salt = 0; // Part of every cut hash: a residency change that changes the cuts.
		uint64_t proxy_bytes = 1000;
		uint32_t proxy_triangles = 800;
		LocalVector<Level> levels; // Sorted by error.
	};

	HashMap<RID, FakeMesh> scripted;
	uint32_t part_max_triangles = 1000;
	uint64_t bytes_per_triangle = 1;
	uint64_t frame_number = 100;
	uint64_t next_blas = 1;
	uint32_t extracts = 0;
	uint32_t creates = 0;
	uint32_t builds = 0;
	int64_t live_blases = 0;
	int64_t live_ranges = 0;

	FakeMesh &add_mesh(RID p_mesh) {
		FakeMesh mesh;
		Level leaves;
		leaves.triangles = 4000;
		mesh.levels.push_back(leaves);
		Level fine;
		fine.error = 0.004f;
		fine.triangles = 1500;
		fine.max_error = 0.004f;
		mesh.levels.push_back(fine);
		Level mid;
		mid.error = 0.016f;
		mid.triangles = 900;
		mid.max_error = 0.016f;
		mesh.levels.push_back(mid);
		Level proxy;
		proxy.error = 0.02f;
		proxy.triangles = 800;
		proxy.max_error = 0.02f;
		mesh.levels.push_back(proxy);
		scripted.insert(p_mesh, mesh);
		return scripted[p_mesh];
	}

	// One frame (step): the TLAS phase (error per meter 0.001: band b targets min(0.02, 0.002 * 2^b)), each
	// (mesh, key, distance) instance's choice, then the builds phase.
	struct Trace {
		RID mesh;
		uint64_t key = 0;
		float distance = 0.0f;
		bool native = false;
		uint32_t triangles = 0;
		float eps = 0.0f;
	};
	void step(LocalVector<Trace> &r_traces, uint32_t p_budget = 250000) {
		frame_number++;
		begin_frame_sets(frame_number, 0.001f, false);
		for (Trace &trace : r_traces) {
			const AABB bounds(Vector3(-0.5f, -0.5f, -0.5f), Vector3(1.0f, 1.0f, 1.0f));
			const Vector3 camera(0.5f + trace.distance, 0.0f, 0.0f);
			float eps = -1.0f;
			const LocalVector<Part> *parts = select(trace.mesh, trace.key, bounds, Transform3D(), camera, NEXTGEO_RT_NATIVE_INCLUDED, eps);
			trace.native = parts != nullptr;
			trace.triangles = 0;
			trace.eps = eps;
			if (parts != nullptr) {
				for (const Part &part : *parts) {
					trace.triangles += part.triangle_count;
				}
			}
		}
		build(p_budget);
		uint32_t compacted = 0;
		compact_after_traces(compacted, 250000);
	}
	uint32_t set_count(RID p_mesh) const {
		const MeshState *state = meshes.getptr(p_mesh);
		return state != nullptr ? state->sets.size() : 0u;
	}

protected:
	bool _mesh_alive(RID p_mesh) override {
		const FakeMesh *mesh = scripted.getptr(p_mesh);
		return mesh != nullptr && mesh->alive;
	}
	bool _mesh_cut_inputs(RID p_mesh, float &r_proxy_eps, uint64_t &r_generation) override {
		const FakeMesh *mesh = scripted.getptr(p_mesh);
		if (mesh == nullptr) {
			return false;
		}
		r_proxy_eps = mesh->eps;
		r_generation = mesh->generation;
		return true;
	}
	bool _mesh_cut(RID p_mesh, float p_error, NextGeoRTNativeCut &r_cut) override {
		const FakeMesh *mesh = scripted.getptr(p_mesh);
		if (mesh == nullptr || mesh->levels.is_empty()) {
			return false;
		}
		uint32_t level = 0;
		for (uint32_t i = 0; i < mesh->levels.size(); i++) {
			if (mesh->levels[i].error <= p_error) {
				level = i;
			}
		}
		r_cut = NextGeoRTNativeCut();
		r_cut.triangle_count = mesh->levels[level].triangles;
		r_cut.vertex_count = mesh->levels[level].triangles;
		r_cut.max_error = mesh->levels[level].max_error;
		r_cut.hash = (p_mesh.get_id() << 40u) ^ (mesh->salt << 8u) ^ uint64_t(level + 1u);
		return true;
	}
	bool _set_extract(MeshState &r_state, const NextGeoRTNativeCut &p_cut, Set &r_set) override {
		extracts++;
		for (uint32_t left = p_cut.triangle_count; left > 0;) {
			Part part;
			part.triangle_count = MIN(left, part_max_triangles);
			part.chunk = 0;
			part.allocation_size = 16;
			left -= part.triangle_count;
			r_set.parts.push_back(part);
			live_ranges++;
		}
		r_set.proxy_triangles = scripted[r_state.mesh].proxy_triangles;
		return !r_set.parts.is_empty();
	}
	bool _part_create(Part &r_part, uint64_t &r_bytes) override {
		creates++;
		live_blases++;
		r_part.blas = RID::from_uint64(next_blas++);
		r_bytes = uint64_t(r_part.triangle_count) * bytes_per_triangle;
		return true;
	}
	void _part_build(Part &r_part) override {
		(void)r_part;
		builds++;
	}
	bool _part_compact(Part &r_part) override {
		(void)r_part;
		return true;
	}
	uint64_t _part_bytes(const Part &p_part) const override {
		return uint64_t(p_part.triangle_count) * bytes_per_triangle;
	}
	void _part_free_blas(Part &r_part) override {
		live_blases--;
		r_part.blas = RID();
	}
	void _part_free_range(RID p_mesh, Part &r_part, bool p_now) override {
		(void)p_mesh;
		(void)p_now;
		if (r_part.allocation_size > 0) {
			live_ranges--;
			r_part.allocation_size = 0;
		}
	}
	void _mesh_proxy_bytes(RID p_mesh, uint64_t &r_build_bytes, uint64_t &r_current_bytes) override {
		const FakeMesh *mesh = scripted.getptr(p_mesh);
		if (mesh != nullptr && mesh->proxy_built) {
			r_build_bytes += mesh->proxy_bytes;
			r_current_bytes += mesh->proxy_bytes;
		}
	}

public:
	~FakeNativeSets() {
		release_all();
	}
};

static FakeNativeSets::Trace fake_trace(RID p_mesh, uint64_t p_key, float p_distance) {
	FakeNativeSets::Trace trace;
	trace.mesh = p_mesh;
	trace.key = p_key;
	trace.distance = p_distance;
	return trace;
}

} // namespace TestNextGIRTNative

TEST_CASE("[NextGI][NativeRT] The mode setting defaults to fallback; only 1 selects native") {
	using namespace RendererRD;
	CHECK(nextgeo_rt_mode_from_setting(0) == NEXTGEO_RT_MODE_FALLBACK);
	CHECK(nextgeo_rt_mode_from_setting(1) == NEXTGEO_RT_MODE_NATIVE);
	CHECK(nextgeo_rt_mode_from_setting(2) == NEXTGEO_RT_MODE_FALLBACK);
	CHECK(nextgeo_rt_mode_from_setting(-1) == NEXTGEO_RT_MODE_FALLBACK);
	ProjectSettings *settings = ProjectSettings::get_singleton();
	REQUIRE(settings->has_setting("rendering/nextgeo/ray_tracing_mode"));
	CHECK(int(settings->get_setting_with_override("rendering/nextgeo/ray_tracing_mode")) == 0);
	CHECK(int(settings->property_get_revert("rendering/nextgeo/ray_tracing_mode")) == 0);
}

TEST_CASE("[NextGI][NativeRT] Camera-distance bands double from the base distance") {
	using namespace RendererRD;
	const float base = NEXTGEO_RT_NATIVE_BAND_BASE_DISTANCE;
	CHECK(nextgeo_rt_native_band(0.0f, 1.0f) == 0);
	CHECK(nextgeo_rt_native_band(base * 0.999f, 1.0f) == 0);
	CHECK(nextgeo_rt_native_band(base, 1.0f) == 1);
	CHECK(nextgeo_rt_native_band(base * 1.999f, 1.0f) == 1);
	CHECK(nextgeo_rt_native_band(base * 2.0f, 1.0f) == 2);
	CHECK(nextgeo_rt_native_band(base * 4.0f, 1.0f) == 3);
	CHECK(nextgeo_rt_native_band(base * 8.0f, 1.0f) == 4);
	// The effective distance divides by the instance's scale: a 2x instance at 2d is a 1x one at d.
	CHECK(nextgeo_rt_native_band(base * 2.0f, 2.0f) == 1);
	CHECK(nextgeo_rt_native_band(base * 3.0f, 0.5f) == 3);
	CHECK(nextgeo_rt_native_band(1.0e9f, 1.0f) == NEXTGEO_RT_NATIVE_BAND_COUNT - 1);
	CHECK(nextgeo_rt_native_band(std::numeric_limits<float>::quiet_NaN(), 1.0f) == 0);
	// An unusable scale counts as 1.
	CHECK(nextgeo_rt_native_band(base * 2.0f, 0.0f) == 2);
	CHECK(nextgeo_rt_native_band(base * 2.0f, -3.0f) == 2);
}

TEST_CASE("[NextGI][NativeRT] Band error targets follow the pixel error, quantized, capped at the proxy eps") {
	using namespace RendererRD;
	// 1080 rows at a 60 degree vertical field of view: |P11| = 1 / tan(30 deg).
	const float p11 = 1.0f / Math::tan(Math::deg_to_rad(30.0f));
	const float exact = 2.0f / (1080.0f * p11);
	const float per_meter = nextgeo_rt_native_error_per_meter(1.0f, 1080.0f, p11);
	CHECK(per_meter == 1.0f / 1024.0f);
	CHECK(per_meter <= exact);
	CHECK(per_meter > exact * 0.5f);
	// A small resize keeps the quantized value, so no bucket is rebuilt.
	CHECK(nextgeo_rt_native_error_per_meter(1.0f, 1070.0f, p11) == per_meter);
	CHECK(nextgeo_rt_native_error_per_meter(2.0f, 1080.0f, p11) == per_meter * 2.0f);
	CHECK(nextgeo_rt_native_error_per_meter(0.0f, 1080.0f, p11) == 0.0f);
	CHECK(nextgeo_rt_native_error_per_meter(1.0f, 0.0f, p11) == 0.0f);
	CHECK(nextgeo_rt_native_error_per_meter(1.0f, 1080.0f, 0.0f) == 0.0f);

	const float eps = 0.02f;
	const float half_base = NEXTGEO_RT_NATIVE_BAND_BASE_DISTANCE * 0.5f;
	CHECK(nextgeo_rt_native_band_error(0, per_meter, eps, false) == doctest::Approx(per_meter * half_base));
	CHECK(nextgeo_rt_native_band_error(1, per_meter, eps, false) == doctest::Approx(per_meter * half_base * 2.0f));
	CHECK(nextgeo_rt_native_band_error(3, per_meter, eps, false) == doctest::Approx(per_meter * half_base * 8.0f));
	for (uint32_t band = 0; band < NEXTGEO_RT_NATIVE_BAND_COUNT; band++) {
		const float error = nextgeo_rt_native_band_error(band, per_meter, eps, false);
		CHECK(error <= eps);
		if (band > 0) {
			CHECK(error >= nextgeo_rt_native_band_error(band - 1, per_meter, eps, false));
		}
	}
	// Far bands stop at the proxy's error: native is never coarser than the fallback.
	CHECK(nextgeo_rt_native_band_error(NEXTGEO_RT_NATIVE_BAND_COUNT - 1, per_meter, eps, false) == eps);
	// Without a usable view every band takes the proxy's error; orthographic views have one band.
	CHECK(nextgeo_rt_native_band_error(0, 0.0f, eps, false) == eps);
	CHECK(nextgeo_rt_native_band_error(0, per_meter, eps, true) == per_meter);
	CHECK(nextgeo_rt_native_band_error(0, 1.0f, eps, true) == eps);
}

TEST_CASE("[NextGI][NativeRT] Exclusions, BLAS choice, the N7 budget and the MN4 cap") {
	using namespace RendererRD;
	CHECK(nextgeo_rt_native_exclusion(false, false, false) == NEXTGEO_RT_NATIVE_INCLUDED);
	CHECK(nextgeo_rt_native_exclusion(true, false, false) == NEXTGEO_RT_NATIVE_EXCLUDED_SKINNED);
	CHECK(nextgeo_rt_native_exclusion(true, true, true) == NEXTGEO_RT_NATIVE_EXCLUDED_SKINNED);
	CHECK(nextgeo_rt_native_exclusion(false, true, true) == NEXTGEO_RT_NATIVE_EXCLUDED_VERTEX_OFFSET);
	CHECK(nextgeo_rt_native_exclusion(false, false, true) == NEXTGEO_RT_NATIVE_EXCLUDED_LEGACY_SURFACES);

	// Over the cap an instance keeps its previous native BLAS, or the proxy when it has none.
	CHECK(nextgeo_rt_native_choose(true, true) == NEXTGEO_RT_NATIVE_USE_BAND);
	CHECK(nextgeo_rt_native_choose(true, false) == NEXTGEO_RT_NATIVE_USE_BAND);
	CHECK(nextgeo_rt_native_choose(false, true) == NEXTGEO_RT_NATIVE_USE_PREVIOUS);
	CHECK(nextgeo_rt_native_choose(false, false) == NEXTGEO_RT_NATIVE_USE_PROXY);

	CHECK(nextgeo_rt_native_build_budget(0) == 0);
	CHECK(nextgeo_rt_native_build_budget(1000) == 1000);
	CHECK(nextgeo_rt_native_build_budget(NEXTGI_RT_BUILD_TRIANGLE_CAP) == uint32_t(NEXTGEO_RT_NATIVE_REBUILD_TRIANGLE_CAP));
	CHECK(nextgeo_rt_native_build_budget(UINT32_MAX) == uint32_t(NEXTGEO_RT_NATIVE_REBUILD_TRIANGLE_CAP));

	// MN4: twice the fallback BLAS bytes, and never past what the AS memory line has left.
	CHECK(nextgeo_rt_native_memory_cap(100, UINT64_MAX, 0) == 200);
	CHECK(nextgeo_rt_native_memory_cap(100, 1000, 900) == 100);
	CHECK(nextgeo_rt_native_memory_cap(100, 1000, 700) == 200);
	CHECK(nextgeo_rt_native_memory_cap(100, 1000, 1200) == 0); // The proxies alone fill the line.
	CHECK(nextgeo_rt_native_memory_cap(0, UINT64_MAX, 0) == 0); // No fallback BLAS yet: nothing fits.

	CHECK(nextgeo_rt_native_fits_memory(0, 200, 200));
	CHECK_FALSE(nextgeo_rt_native_fits_memory(0, 201, 200));
	CHECK(nextgeo_rt_native_fits_memory(150, 50, 200));
	CHECK_FALSE(nextgeo_rt_native_fits_memory(150, 51, 200));
	CHECK_FALSE(nextgeo_rt_native_fits_memory(0, 1, 0));

	// MN4 reclaim: the set an instance is leaving (138 of 276 cap bytes, as on Concave Courtyard)
	// leaves first when the new set (160) fits only without it; never when it would not fit anyway.
	CHECK_FALSE(nextgeo_rt_native_fits_memory(138, 160, 276));
	CHECK(nextgeo_rt_native_reclaim_makes_room(138, 138, 160, 276));
	CHECK_FALSE(nextgeo_rt_native_reclaim_makes_room(138, 138, 420, 276)); // Too big on its own.
	CHECK_FALSE(nextgeo_rt_native_reclaim_makes_room(138, 0, 160, 276)); // Nothing to reclaim.
	CHECK(nextgeo_rt_native_reclaim_makes_room(300, 100, 76, 276)); // Other sets stay: 200 + 76 <= 276.
	CHECK_FALSE(nextgeo_rt_native_reclaim_makes_room(300, 100, 77, 276));
	CHECK_FALSE(nextgeo_rt_native_reclaim_makes_room(100, 101, 10, 276)); // Inconsistent totals.
}

TEST_CASE("[NextGI][NativeRT] Parts keep whole clusters under the cap and run per surface") {
	using namespace RendererRD;
	const uint32_t triangles[5] = { 100, 100, 50, 30, 400 };
	const uint32_t vertices[5] = { 60, 62, 40, 20, 255 };
	const uint32_t surfaces[5] = { 0, 0, 1, 1, 3 };
	LocalVector<NextGeoRTNativePartPlan> parts;

	nextgeo_rt_native_split_parts(triangles, vertices, surfaces, 5, 250, parts);
	REQUIRE(parts.size() == 3);
	CHECK(parts[0].first_cluster == 0);
	CHECK(parts[0].cluster_count == 3);
	CHECK(parts[0].triangle_count == 250);
	CHECK(parts[0].vertex_count == 162);
	REQUIRE(parts[0].runs.size() == 2);
	CHECK(parts[0].runs[0].surface == 0);
	CHECK(parts[0].runs[0].first_triangle == 0);
	CHECK(parts[0].runs[0].triangle_count == 200);
	CHECK(parts[0].runs[1].surface == 1);
	CHECK(parts[0].runs[1].first_triangle == 200);
	CHECK(parts[0].runs[1].triangle_count == 50);
	CHECK(parts[1].first_cluster == 3);
	CHECK(parts[1].cluster_count == 1);
	REQUIRE(parts[1].runs.size() == 1);
	CHECK(parts[1].runs[0].surface == 1);
	// A cluster over the cap alone is its own part; it is never split.
	CHECK(parts[2].first_cluster == 4);
	CHECK(parts[2].triangle_count == 400);

	nextgeo_rt_native_split_parts(triangles, vertices, surfaces, 5, NEXTGI_RT_MAX_TRIANGLES_PER_BLAS, parts);
	REQUIRE(parts.size() == 1);
	CHECK(parts[0].triangle_count == 680);
	CHECK(parts[0].runs.size() == 3);

	nextgeo_rt_native_split_parts(triangles, vertices, surfaces, 0, 250, parts);
	CHECK(parts.is_empty());

	NextGeoRTNativePartPlan plan;
	plan.vertex_count = 10;
	plan.triangle_count = 5;
	NextGeoRTNativeRun run;
	run.surface = 0;
	run.first_triangle = 0;
	run.triangle_count = 3;
	plan.runs.push_back(run);
	run.surface = 2;
	run.first_triangle = 3;
	run.triangle_count = 2;
	plan.runs.push_back(run);
	NextGeoRTNativePartLayout layout;
	nextgeo_rt_native_part_layout(plan, layout);
	CHECK(layout.index_offset == 128); // 10 * 12 = 120, aligned to 16.
	REQUIRE(layout.run_group_offsets.size() == 2);
	CHECK(layout.run_group_offsets[0] == 192); // 128 + 5 * 12 = 188, aligned.
	CHECK(layout.run_group_offsets[1] == 208); // 192 + 3 * 2 = 198, aligned.
	CHECK(layout.size == 224); // 208 + 2 * 2 = 212, aligned.
	for (uint32_t offset : layout.run_group_offsets) {
		CHECK((offset % 16) == 0); // The surface record keeps it in bits 4-31.
	}
}

TEST_CASE("[NextGI][NativeRT] Stage-2 cards: DAG-keyed cards map directly, others by bounds, walks stop at the proxy level") {
	using namespace RendererRD;
	using namespace TestNextGIRTNative;
	const float eps = 0.02f;
	LocalVector<SyntheticGroup> groups;
	groups.resize(5);
	// Levels as the DAG builder orders them, finest first: 0 refines into 1, 1 into 2; 2 and 3 are
	// at the proxy level (parent error above eps); 4 is the root.
	groups[0] = { 0.001f, Vector3(0, 0, 0), Vector3(2, 5, 10) };
	groups[1] = { 0.01f, Vector3(0, 0, 0), Vector3(5, 5, 10) };
	groups[2] = { 0.05f, Vector3(0, 0, 0), Vector3(5, 10, 10) };
	groups[3] = { 0.04f, Vector3(5, 0, 0), Vector3(10, 10, 10) };
	groups[4] = { 3.402823466e+38f, Vector3(0, 0, 0), Vector3(10, 10, 10) };
	const Vector<uint8_t> header = make_dag_header(groups);
	NextGeoRTNativeDag dag;
	REQUIRE(dag.init(header.ptr(), uint32_t(header.size())));
	CHECK(dag.group_count == 5);
	CHECK(dag.group_cluster_offset(3) == 3);
	CHECK(dag.group_cluster_count(3) == 1);
	CHECK(dag.group_parent_error(2) == 0.05f);
	CHECK(dag.group_aabb(3).position == Vector3(5, 0, 0));
	CHECK(dag.group_aabb(3).size == Vector3(5, 10, 10));
	NextGeoRTNativeDag short_dag;
	CHECK_FALSE(short_dag.init(header.ptr(), 40)); // Too short for its groups.
	CHECK(short_dag.group_count == 0);

	uint32_t parent_of[5] = { 1, 2, 4, 4, UINT32_MAX };
	CHECK(nextgeo_rt_native_proxy_group(dag, 0, parent_of, 5, eps) == 2);
	CHECK(nextgeo_rt_native_proxy_group(dag, 1, parent_of, 5, eps) == 2);
	CHECK(nextgeo_rt_native_proxy_group(dag, 3, parent_of, 5, eps) == 3);
	CHECK(nextgeo_rt_native_proxy_group(dag, 4, parent_of, 5, eps) == 4);
	// A finer eps moves the proxy level down.
	CHECK(nextgeo_rt_native_proxy_group(dag, 0, parent_of, 5, 0.005f) == 1);
	// A parent that is not resident yet breaks the chain.
	parent_of[1] = UINT32_MAX;
	CHECK(nextgeo_rt_native_proxy_group(dag, 0, parent_of, 5, eps) == UINT32_MAX);
	CHECK(nextgeo_rt_native_proxy_group(dag, 9, parent_of, 5, eps) == UINT32_MAX);

	// Cards keyed by DAG group (no depth layers): the key is the group itself.
	PackedByteArray direct;
	append_card(direct, 2, 0, Vector3(2.5f, 5, 5), Vector3(0, 5, 5));
	append_card(direct, 2, 2, Vector3(2.5f, 5, 5), Vector3(2.5f, 0, 5));
	append_card(direct, 3, 1, Vector3(7.5f, 5, 5), Vector3(0, 5, 5));
	CHECK(nextgeo_rt_native_cards_are_dag_groups(dag, direct.ptr(), 3, eps));
	CHECK(nextgeo_rt_native_card_of_proxy_group(dag, 2, true, direct.ptr(), 3, eps) == 2);
	CHECK(nextgeo_rt_native_card_of_proxy_group(dag, 3, true, direct.ptr(), 3, eps) == 3);

	// Layered keys are sequential from 0 in proxy-group order: they name groups below the proxy
	// level or groups whose bounds do not hold the card, so they are not DAG-keyed and groups map by
	// bounds: the first card whose center lies in the group.
	PackedByteArray layered;
	append_card(layered, 0, 0, Vector3(2.5f, 5, 5), Vector3(0, 5, 5)); // Group 2, layer 0.
	append_card(layered, 1, 0, Vector3(1.0f, 5, 5), Vector3(0, 2, 2)); // Group 2, layer 1.
	append_card(layered, 2, 1, Vector3(7.5f, 5, 5), Vector3(0, 5, 5)); // Group 3.
	CHECK_FALSE(nextgeo_rt_native_cards_are_dag_groups(dag, layered.ptr(), 3, eps));
	CHECK(nextgeo_rt_native_card_of_proxy_group(dag, 2, false, layered.ptr(), 3, eps) == 0);
	CHECK(nextgeo_rt_native_card_of_proxy_group(dag, 3, false, layered.ptr(), 3, eps) == 2);
	PackedByteArray outside;
	append_card(outside, 3, 0, Vector3(2.5f, 5, 5), Vector3(0, 5, 5)); // Group 3's key in group 2's bounds.
	CHECK_FALSE(nextgeo_rt_native_cards_are_dag_groups(dag, outside.ptr(), 1, eps));
	PackedByteArray fine_key;
	append_card(fine_key, 1, 0, Vector3(2.5f, 2.5f, 5), Vector3(0, 2.5f, 5)); // Group 1 is below the proxy level.
	CHECK_FALSE(nextgeo_rt_native_cards_are_dag_groups(dag, fine_key.ptr(), 1, eps));
	CHECK_FALSE(nextgeo_rt_native_cards_are_dag_groups(dag, nullptr, 0, eps));
	// No card in a group's bounds: the hit falls back to the WRC.
	PackedByteArray far_away;
	append_card(far_away, 0, 0, Vector3(50, 50, 50), Vector3(1, 1, 0));
	CHECK(nextgeo_rt_native_card_of_proxy_group(dag, 2, false, far_away.ptr(), 1, eps) == NEXTGEO_RT_NATIVE_NO_CARD);
	CHECK(nextgeo_rt_native_card_of_proxy_group(dag, 7, false, layered.ptr(), 3, eps) == NEXTGEO_RT_NATIVE_NO_CARD);
}

TEST_CASE("[NextGI][NativeRT] The proxy's triangle order packs parts, then sorts by surface") {
	using namespace RendererRD;
	// Parts of at most 6 triangles: [c0 c1] (5) and [c2 c3] (5); surface 0 first inside each part.
	const uint32_t triangles[4] = { 3, 2, 4, 1 };
	const uint32_t surfaces[4] = { 1, 0, 1, 0 };
	LocalVector<uint32_t> bases;
	REQUIRE(nextgeo_rt_native_proxy_triangle_bases(triangles, surfaces, 4, 6, 10, bases));
	REQUIRE(bases.size() == 4);
	CHECK(bases[0] == 2);
	CHECK(bases[1] == 0);
	CHECK(bases[2] == 6);
	CHECK(bases[3] == 5);
	CHECK_FALSE(nextgeo_rt_native_proxy_triangle_bases(triangles, surfaces, 4, 6, 11, bases));
	// One part when the cap allows: sorted by surface across all four clusters.
	REQUIRE(nextgeo_rt_native_proxy_triangle_bases(triangles, surfaces, 4, 100, 10, bases));
	CHECK(bases[1] == 0);
	CHECK(bases[3] == 2);
	CHECK(bases[0] == 3);
	CHECK(bases[2] == 6);
	// A single cluster over the cap is its own part.
	const uint32_t big[2] = { 9, 2 };
	const uint32_t one_surface[2] = { 0, 0 };
	REQUIRE(nextgeo_rt_native_proxy_triangle_bases(big, one_surface, 2, 6, 11, bases));
	CHECK(bases[0] == 0);
	CHECK(bases[1] == 9);
}

TEST_CASE("[NextGI][NativeRT] Finer triangles take the card layer on their own axis that holds them") {
	using namespace RendererRD;
	using namespace TestNextGIRTNative;
	// A proxy-level DAG group split into depth layers (#77), as Concave Courtyard's root groups are:
	// card group 4 is the +X-facing wall at x = -8 and, on the -X axis, the wall at x = 8; group 5 is
	// the +X-facing wall at x = 4 and the floor. Descriptors sorted by (group, axis).
	PackedByteArray cards;
	append_card(cards, 3, 0, Vector3(0.0f, 2.0f, 20.0f), Vector3(0.0f, 2.0f, 2.0f)); // Another group's.
	append_card(cards, 4, 0, Vector3(-8.0f, 2.0f, 0.0f), Vector3(0.0f, 2.0f, 8.0f));
	append_card(cards, 4, 1, Vector3(8.0f, 2.0f, 0.0f), Vector3(0.0f, 2.0f, 8.0f));
	append_card(cards, 5, 0, Vector3(4.0f, 2.0f, 0.0f), Vector3(0.0f, 2.0f, 8.0f));
	append_card(cards, 5, 2, Vector3(0.0f, 0.0f, 0.0f), Vector3(8.0f, 0.0f, 8.0f));
	append_card(cards, 6, 0, Vector3(4.0f, 2.0f, 0.0f), Vector3(0.0f, 2.0f, 1.0f)); // A smaller box on group 5's wall.
	append_card(cards, 6, 2, Vector3(0.0f, 0.0f, 0.0f), Vector3(8.0f, 1.0f, 8.0f)); // A thicker floor.
	const uint32_t card_count = uint32_t(cards.size() / 32);

	uint32_t first = 0;
	uint32_t end = 0;
	nextgeo_rt_native_card_range(cards.ptr(), card_count, 4, first, end);
	CHECK(first == 1);
	CHECK(end == 3);
	nextgeo_rt_native_card_range(cards.ptr(), card_count, 6, first, end);
	CHECK(first == 5);
	CHECK(end == 7);
	nextgeo_rt_native_card_range(cards.ptr(), card_count, 2, first, end);
	CHECK(first == end);
	nextgeo_rt_native_card_range(cards.ptr(), card_count, 9, first, end);
	CHECK(first == card_count);
	CHECK(end == card_count);
	nextgeo_rt_native_card_range(nullptr, 0, 4, first, end);
	CHECK(end == 0);

	// The dominant axis from the outward normal (c - a) x (b - a), as the proxy's layers use it.
	uint32_t axis = 99;
	CHECK(nextgeo_rt_native_triangle_axis(Vector3(0, 0, 0), Vector3(0, 0, 1), Vector3(0, 1, 0), axis));
	CHECK(axis == 0); // Faces +X.
	CHECK(nextgeo_rt_native_triangle_axis(Vector3(0, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1), axis));
	CHECK(axis == 1);
	CHECK(nextgeo_rt_native_triangle_axis(Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 0, 1), axis));
	CHECK(axis == 2); // Faces +Y.
	CHECK(nextgeo_rt_native_triangle_axis(Vector3(0, 0, 0), Vector3(0, 1, 0), Vector3(1, 0, 0), axis));
	CHECK(axis == 4); // Faces +Z.
	CHECK_FALSE(nextgeo_rt_native_triangle_axis(Vector3(1, 1, 1), Vector3(1, 1, 1), Vector3(2, 2, 2), axis));

	// The candidates are the layers of the proxy-level groups above the cluster: groups 4 and 5.
	const uint32_t candidates[4] = { 1, 2, 3, 4 };
	const float tolerance = 0.04f;
	auto card_of = [&](const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_c, const uint32_t *p_candidates, uint32_t p_count, uint32_t p_limit) {
		return nextgeo_rt_native_triangle_card(cards.ptr(), card_count, p_candidates, p_count, p_limit, p_a, p_b, p_c, tolerance, 77);
	};
	// One cluster can hold triangles of three layers: each takes the layer on its own axis that holds it.
	CHECK(card_of(Vector3(-7.99f, 1, 0), Vector3(-7.99f, 1, 1), Vector3(-7.99f, 2, 0), candidates, 4, 0) == 4); // +X wall at x = -8.
	CHECK(card_of(Vector3(4.01f, 1, 0), Vector3(4.01f, 1, 1), Vector3(4.01f, 2, 0), candidates, 4, 0) == 5); // +X wall at x = 4.
	CHECK(card_of(Vector3(7.99f, 1, 0), Vector3(7.99f, 2, 0), Vector3(7.99f, 1, 1), candidates, 4, 0) == 4); // -X wall at x = 8.
	CHECK(card_of(Vector3(1, 0.01f, 1), Vector3(2, 0.01f, 1), Vector3(1, 0.01f, 2), candidates, 4, 0) == 5); // The floor.
	// Off every card on its axis: the nearest one.
	CHECK(card_of(Vector3(-5.0f, 1, 0), Vector3(-5.0f, 1, 1), Vector3(-5.0f, 2, 0), candidates, 4, 0) == 4);
	CHECK(card_of(Vector3(2.0f, 1, 0), Vector3(2.0f, 1, 1), Vector3(2.0f, 2, 0), candidates, 4, 0) == 5);
	// No candidate card on its axis (-Y): the default.
	CHECK(card_of(Vector3(1, 0.01f, 1), Vector3(1, 0.01f, 2), Vector3(2, 0.01f, 1), candidates, 4, 100) == 77);
	CHECK(card_of(Vector3(1, 1, 1), Vector3(1, 1, 1), Vector3(1, 1, 1), candidates, 4, 100) == 77); // No area.
	CHECK(nextgeo_rt_native_triangle_card(nullptr, 0, candidates, 4, 100, Vector3(-7.99f, 1, 0), Vector3(-7.99f, 1, 1), Vector3(-7.99f, 2, 0), tolerance, 77) == 77);

	// When no candidate holds it, every card of the mesh is searched (within the limit): a triangle on
	// another group's wall takes that group's card.
	const uint32_t walked[2] = { 1, 2 }; // Group 4 only.
	CHECK(card_of(Vector3(4.01f, 1, 0), Vector3(4.01f, 1, 1), Vector3(4.01f, 2, 0), walked, 2, 0) == 4); // Nearest of group 4.
	CHECK(card_of(Vector3(4.01f, 1, 0), Vector3(4.01f, 1, 1), Vector3(4.01f, 2, 0), walked, 2, 100) == 6); // Group 6's smaller box.
	CHECK(card_of(Vector3(0.0f, 1, 20), Vector3(0.0f, 1, 21), Vector3(0.0f, 2, 20), nullptr, 0, 100) == 3);
	CHECK(card_of(Vector3(0.0f, 1, 20), Vector3(0.0f, 1, 21), Vector3(0.0f, 2, 20), nullptr, 0, 6) == 77); // Over the limit.
	// Ties between boxes that both hold it: the thinner along the axis (group 5's floor, not 6's),
	// then the smaller across it (group 6's wall, not 5's).
	CHECK(card_of(Vector3(1, 0.01f, 1), Vector3(2, 0.01f, 1), Vector3(1, 0.01f, 2), nullptr, 0, 100) == 5);
	CHECK(card_of(Vector3(4.01f, 1, 0), Vector3(4.01f, 1, 0.5f), Vector3(4.01f, 2, 0), nullptr, 0, 100) == 6);
	// A candidate that is not a descriptor is skipped.
	const uint32_t broken[2] = { 99, 2 };
	CHECK(card_of(Vector3(7.99f, 1, 0), Vector3(7.99f, 2, 0), Vector3(7.99f, 1, 1), broken, 2, 0) == 4);
}

TEST_CASE("[NextGI][NativeRT] Finer groups reach every proxy-level group above them") {
	using namespace RendererRD;
	using namespace TestNextGIRTNative;
	const float eps = 0.02f;
	LocalVector<SyntheticGroup> groups;
	groups.resize(7);
	// 0 and 1 are finer than the proxy; 0 was simplified into clusters of 2 and 3, 1 into 3; 2 into
	// 4, 3 into 4 and 5 (the proxy level); 5 into 6 (the root).
	groups[0] = { 0.001f, Vector3(), Vector3(1, 1, 1) };
	groups[1] = { 0.002f, Vector3(), Vector3(1, 1, 1) };
	groups[2] = { 0.01f, Vector3(), Vector3(1, 1, 1) };
	groups[3] = { 0.015f, Vector3(), Vector3(1, 1, 1) };
	groups[4] = { 0.05f, Vector3(), Vector3(1, 1, 1) };
	groups[5] = { 0.04f, Vector3(), Vector3(1, 1, 1) };
	groups[6] = { 3.402823466e+38f, Vector3(), Vector3(1, 1, 1) };
	const Vector<uint8_t> header = make_dag_header(groups);
	NextGeoRTNativeDag dag;
	REQUIRE(dag.init(header.ptr(), uint32_t(header.size())));
	// parents[parent_first[g]] up to parent_first[g + 1].
	const uint32_t parent_first[8] = { 0, 2, 3, 4, 6, 6, 7, 7 };
	const uint32_t parents[7] = { 2, 3, 3, 4, 4, 5, 6 };
	LocalVector<uint32_t> found;
	nextgeo_rt_native_proxy_ancestors(dag, 0, parent_first, parents, 7, eps, found);
	REQUIRE(found.size() == 2);
	CHECK(found[0] == 4);
	CHECK(found[1] == 5);
	nextgeo_rt_native_proxy_ancestors(dag, 1, parent_first, parents, 7, eps, found);
	REQUIRE(found.size() == 2);
	CHECK(found[0] == 4);
	CHECK(found[1] == 5);
	nextgeo_rt_native_proxy_ancestors(dag, 2, parent_first, parents, 7, eps, found);
	REQUIRE(found.size() == 1);
	CHECK(found[0] == 4);
	// A proxy-level group is its own; a finer eps moves the level down.
	nextgeo_rt_native_proxy_ancestors(dag, 5, parent_first, parents, 7, eps, found);
	REQUIRE(found.size() == 1);
	CHECK(found[0] == 5);
	nextgeo_rt_native_proxy_ancestors(dag, 0, parent_first, parents, 7, 0.012f, found);
	REQUIRE(found.size() == 2);
	CHECK(found[0] == 3);
	CHECK(found[1] == 4);
	// No parent known yet, no parent table, or not a group: nothing.
	const uint32_t orphan_first[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	nextgeo_rt_native_proxy_ancestors(dag, 0, orphan_first, parents, 7, eps, found);
	CHECK(found.is_empty());
	nextgeo_rt_native_proxy_ancestors(dag, 0, nullptr, nullptr, 0, eps, found);
	CHECK(found.is_empty());
	nextgeo_rt_native_proxy_ancestors(dag, 9, parent_first, parents, 7, eps, found);
	CHECK(found.is_empty());
}

#ifdef MODULE_NEXTGEO_ENABLED
TEST_CASE("[NextGI][NativeRT] Proxy clusters find their own triangles in the imported proxy") {
	using namespace RendererRD;
	using namespace TestNextGIRTNative;
	// Two surfaces (halves of the wavy grid), so the proxy's per-part surface sort matters.
	NextGeoBuildInput input = make_wavy_input();
	const NextGeoSurfaceInput whole = input.surfaces[0];
	input.surfaces.clear();
	for (int half = 0; half < 2; half++) {
		NextGeoSurfaceInput surface;
		surface.eligible = true;
		surface.positions = whole.positions;
		surface.normals = whole.normals;
		for (int i = 0; i < whole.indices.size(); i += 3) {
			const Vector3 centroid = (whole.positions[whole.indices[i]] + whole.positions[whole.indices[i + 1]] + whole.positions[whole.indices[i + 2]]) / 3.0f;
			if ((centroid.x < 0.0f) == (half == 0)) {
				surface.indices.push_back(whole.indices[i]);
				surface.indices.push_back(whole.indices[i + 1]);
				surface.indices.push_back(whole.indices[i + 2]);
			}
		}
		input.surfaces.push_back(surface);
	}
	NextGeoBuildOutput dag;
	REQUIRE(nextgeo_build_dag(input, dag) == OK);
	AABB bounds;
	for (int i = 0; i < whole.positions.size(); i++) {
		if (i == 0) {
			bounds = AABB(whole.positions[i], Vector3());
		} else {
			bounds.expand_to(whole.positions[i]);
		}
	}
	NextGeoRTProxy proxy;
	REQUIRE(nextgeo_build_rt_proxy(dag, bounds, 0.05f, 2, proxy) == OK);
	REQUIRE(proxy.clusters.size() > 0);
	const uint32_t count = uint32_t(proxy.clusters.size());
	LocalVector<uint32_t> triangles;
	LocalVector<uint32_t> surfaces;
	for (uint32_t i = 0; i < count; i++) {
		const NextGeoCluster &cluster = dag.clusters[proxy.clusters[i]];
		triangles.push_back(cluster.triangle_count);
		surfaces.push_back(cluster.surface_id);
	}
	CHECK(surfaces.has(0));
	CHECK(surfaces.has(1));
	LocalVector<uint32_t> bases;
	REQUIRE(nextgeo_rt_native_proxy_triangle_bases(triangles.ptr(), surfaces.ptr(), count, NEXTGEO_RT_PROXY_MAX_PART_TRIANGLES, uint32_t(proxy.triangle_groups.size() / 2), bases));
	REQUIRE(proxy.part_count() >= 1);

	// Every proxy cluster's triangles sit at its base, vertex for vertex.
	const uint32_t *indices = reinterpret_cast<const uint32_t *>(proxy.indices.ptr());
	uint32_t mismatches = 0;
	for (uint32_t i = 0; i < count; i++) {
		const NextGeoCluster &cluster = dag.clusters[proxy.clusters[i]];
		for (uint32_t t = 0; t < cluster.triangle_count; t++) {
			const uint32_t proxy_triangle = bases[i] + t;
			int32_t part = -1;
			for (uint32_t p = 0; p < proxy.part_count(); p++) {
				const uint32_t first = uint32_t(proxy.parts[p * 4 + 2]) / 3;
				const uint32_t triangle_count = uint32_t(proxy.parts[p * 4 + 3]) / 3;
				if (proxy_triangle >= first && proxy_triangle < first + triangle_count) {
					part = int32_t(p);
					break;
				}
			}
			REQUIRE(part >= 0);
			const uint32_t vertex_base = uint32_t(proxy.parts[part * 4 + 0]);
			for (uint32_t k = 0; k < 3; k++) {
				const uint32_t index = indices[proxy_triangle * 3 + k];
				const float *proxy_position = proxy.positions.ptr() + (vertex_base + index) * 3;
				const uint8_t local = dag.triangles[(cluster.triangle_offset + t) * 3 + k];
				const float *dag_position = dag.vertices.ptr() + uint64_t(cluster.vertex_offset + local) * dag.vertex_stride + NEXTGEO_VERTEX_OFFSET_POSITION;
				if (proxy_position[0] != dag_position[0] || proxy_position[1] != dag_position[1] || proxy_position[2] != dag_position[2]) {
					mismatches++;
				}
			}
		}
	}
	CHECK(mismatches == 0);
}

TEST_CASE("[NextGI][NativeRT] With every page resident the bucket cut is the error-only DAG cut") {
	using namespace RendererRD;
	using namespace TestNextGIRTNative;
	StreamedMesh mesh;
	make_streamed_mesh(mesh);
	MockVirtualMeshStorage storage;
	const RID mesh_rid = RID::from_uint64(0x156001);
	LocalVector<uint32_t> all_pages;
	for (uint32_t page = 0; page < mesh.dag.pages.size(); page++) {
		all_pages.push_back(page);
	}
	stream_pages(storage, mesh_rid, mesh, all_pages);
	const RendererVirtualMeshStorage::MeshInfo *info = storage.get_meshes().getptr(mesh_rid);
	REQUIRE(info != nullptr);
	const uint32_t first = info->record.cluster_offset;

	// Thresholds between the DAG's distinct parent errors, from the leaves up.
	LocalVector<float> parent_errors;
	for (const NextGeoGroup &group : mesh.dag.groups) {
		if (group.parent_error < 3.0e38f && !parent_errors.has(group.parent_error)) {
			parent_errors.push_back(group.parent_error);
		}
	}
	parent_errors.sort();
	REQUIRE(parent_errors.size() >= 3);
	LocalVector<float> thresholds;
	thresholds.push_back(0.0f);
	thresholds.push_back((parent_errors[0] + parent_errors[1]) * 0.5f);
	thresholds.push_back((parent_errors[parent_errors.size() / 2] + parent_errors[parent_errors.size() / 2 + 1]) * 0.5f);
	thresholds.push_back(parent_errors[parent_errors.size() - 1] * 1.5f);

	uint64_t previous_hash = 0;
	uint32_t previous_triangles = UINT32_MAX;
	for (float error : thresholds) {
		NextGeoRTNativeCut cut;
		REQUIRE(nextgeo_rt_native_select_cut(storage, *info, error, 0u, cut));
		LocalVector<uint32_t> expected;
		uint32_t expected_triangles = 0;
		float expected_max_error = 0.0f;
		for (uint32_t c = 0; c < mesh.dag.clusters.size(); c++) {
			const NextGeoCluster &cluster = mesh.dag.clusters[c];
			if (cluster.group_error <= error && cluster.parent_error > error) {
				expected.push_back(c);
				expected_triangles += cluster.triangle_count;
				expected_max_error = MAX(expected_max_error, cluster.group_error);
			}
		}
		const LocalVector<uint32_t> got = global_clusters(cut, first);
		CHECK(same_clusters(got, expected));
		CHECK(cut.triangle_count == expected_triangles);
		CHECK(cut.max_error == expected_max_error);
		CHECK(nextgeo_check_cut(mesh.dag, got).is_valid_partition());
		CHECK(cut.hash != previous_hash);
		// Coarser targets never add triangles.
		CHECK(cut.triangle_count <= previous_triangles);
		previous_hash = cut.hash;
		previous_triangles = cut.triangle_count;
		// The same inputs give the same cut and hash.
		NextGeoRTNativeCut again;
		REQUIRE(nextgeo_rt_native_select_cut(storage, *info, error, 0u, again));
		CHECK(again.hash == cut.hash);
		CHECK(same_clusters(again.clusters, cut.clusters));
		// With every page resident the cut is never coarser than asked; a cut is usable only against
		// a proxy eps at least its own error.
		CHECK(cut.max_error <= error);
		CHECK(nextgeo_rt_native_cut_usable(cut, cut.max_error));
		if (cut.max_error > 0.0f) {
			CHECK_FALSE(nextgeo_rt_native_cut_usable(cut, cut.max_error * 0.5f));
		}
	}
	// The leaf cut is the whole source mesh.
	NextGeoRTNativeCut leaves;
	REQUIRE(nextgeo_rt_native_select_cut(storage, *info, 0.0f, 0u, leaves));
	CHECK(leaves.triangle_count == mesh.dag.source_triangle_count);
	CHECK(leaves.max_error == 0.0f);
	// An excluded surface (an alpha material) takes its clusters out.
	NextGeoRTNativeCut excluded;
	REQUIRE(nextgeo_rt_native_select_cut(storage, *info, 0.0f, 1u, excluded));
	CHECK(excluded.triangle_count == 0);
	CHECK_FALSE(nextgeo_rt_native_cut_usable(excluded, 1.0f));
}

TEST_CASE("[NextGI][NativeRT] Missing pages keep the resident parents: a valid partition, coarser than asked") {
	using namespace RendererRD;
	using namespace TestNextGIRTNative;
	StreamedMesh mesh;
	make_streamed_mesh(mesh);

	// Root pages only (pinned at registration).
	MockVirtualMeshStorage roots_only;
	const RID mesh_rid = RID::from_uint64(0x156002);
	stream_pages(roots_only, mesh_rid, mesh, LocalVector<uint32_t>());
	const RendererVirtualMeshStorage::MeshInfo *info = roots_only.get_meshes().getptr(mesh_rid);
	REQUIRE(info != nullptr);
	NextGeoRTNativeCut coarse;
	REQUIRE(nextgeo_rt_native_select_cut(roots_only, *info, 0.0f, 0u, coarse));
	const LocalVector<uint32_t> coarse_clusters = global_clusters(coarse, info->record.cluster_offset);
	CHECK(coarse.triangle_count > 0);
	CHECK(coarse.triangle_count < mesh.dag.source_triangle_count);
	CHECK(coarse.max_error > 0.0f);
	CHECK(nextgeo_check_cut(mesh.dag, coarse_clusters).is_valid_partition());
	// Coarser than a proxy at the leaf error: the bucket keeps the proxy (or its built set).
	CHECK_FALSE(nextgeo_rt_native_cut_usable(coarse, coarse.max_error * 0.5f));
	CHECK(nextgeo_rt_native_cut_usable(coarse, coarse.max_error));

	// One deep page (its parents come with it): finer than roots only, still a partition.
	MockVirtualMeshStorage one_branch;
	const RID branch_rid = RID::from_uint64(0x156003);
	LocalVector<uint32_t> deep;
	deep.push_back(mesh.dag.pages.size() - 1);
	stream_pages(one_branch, branch_rid, mesh, deep);
	const RendererVirtualMeshStorage::MeshInfo *branch_info = one_branch.get_meshes().getptr(branch_rid);
	REQUIRE(branch_info != nullptr);
	NextGeoRTNativeCut branch;
	REQUIRE(nextgeo_rt_native_select_cut(one_branch, *branch_info, 0.0f, 0u, branch));
	CHECK(branch.triangle_count > coarse.triangle_count);
	CHECK(branch.hash != coarse.hash);
	CHECK(nextgeo_check_cut(mesh.dag, global_clusters(branch, branch_info->record.cluster_offset)).is_valid_partition());
	// Residency changes bump the mesh's generation, which makes buckets take their cut again.
	CHECK(branch_info->residency_generation > info->residency_generation);
}
#endif // MODULE_NEXTGEO_ENABLED

// Distances to the 1 m test box: band 1 at 5 m, band 5 at 100 m, band 6 at 200 m (base 4 m, doubling).
TEST_CASE("[NextGI][NativeRT] Sets: a set over the cap is refused whole, once, and admitted when a reclaim makes room") {
	using namespace RendererRD;
	using namespace TestNextGIRTNative;
	FakeNativeSets sets;
	const RID mesh = RID::from_uint64(0x1561);
	sets.add_mesh(mesh); // Proxy 1000 bytes: MN4 cap 2000.
	LocalVector<FakeNativeSets::Trace> traces;
	traces.push_back(fake_trace(mesh, 1, 100.0f));
	traces.push_back(fake_trace(mesh, 2, 100.0f));
	sets.step(traces); // Band 5 (error 0.02): the 800-triangle set, one part.
	sets.step(traces);
	CHECK(traces[0].native);
	CHECK(traces[1].native);
	CHECK(traces[1].triangles == 800);
	CHECK(sets.get_stats().build_bytes == 800);
	CHECK(sets.get_stats().memory_cap_bytes == 2000);

	// Instance 2 moves to band 1: a 1500-triangle set in two parts. Its first part alone would fit
	// (800 + 1000 <= 2000), the whole set does not (2300), and instance 1 still traces the band 5 set:
	// refused before any part is built.
	traces[1].distance = 5.0f;
	const uint32_t extracts = sets.extracts;
	const uint32_t builds = sets.builds;
	sets.step(traces);
	CHECK(sets.extracts == extracts + 1);
	CHECK(sets.builds == builds);
	CHECK(sets.get_stats().memory_refusals == 1);
	CHECK(sets.get_stats().last_refusal_bytes == 1500);
	CHECK(sets.get_stats().build_bytes == 800);
	// Nothing changes: no extraction, no build, no new refusal; instance 2 keeps the band 5 set.
	for (int i = 0; i < 10; i++) {
		sets.step(traces);
		CHECK(traces[1].native);
		CHECK(traces[1].triangles == 800);
		CHECK(sets.get_stats().instances_previous == 1);
		CHECK(sets.get_stats().build_bytes <= sets.get_stats().memory_cap_bytes);
	}
	CHECK(sets.extracts == extracts + 1);
	CHECK(sets.builds == builds);
	CHECK(sets.get_stats().memory_refusals == 1);
	CHECK(sets.live_blases == 1);

	// Instance 1 leaves: the band 5 set is held only as instance 2's previous set, so the cap has room
	// for the refused set once it goes. It is cut again, the band 5 set is reclaimed, and the new set
	// is admitted and built one frame later; instance 2 traces the proxy for that one frame.
	traces.remove_at(0);
	sets.step(traces);
	CHECK(sets.get_stats().reclaims == 1);
	CHECK(traces[0].native);
	sets.step(traces);
	CHECK_FALSE(traces[0].native);
	CHECK(sets.get_stats().build_bytes == 1500);
	sets.step(traces);
	CHECK(traces[0].native);
	CHECK(traces[0].triangles == 1500);
	CHECK(sets.get_stats().memory_refusals == 1);
	CHECK(sets.get_stats().coarser_sets == 0);
	CHECK(sets.live_blases == 2);
	CHECK(sets.set_count(mesh) == 1);
	sets.release_all();
	CHECK(sets.live_blases == 0);
	CHECK(sets.live_ranges == 0);
}

TEST_CASE("[NextGI][NativeRT] Sets: an admitted set builds across frames within the budget, its bytes reserved") {
	using namespace RendererRD;
	using namespace TestNextGIRTNative;
	FakeNativeSets sets;
	sets.part_max_triangles = 400;
	const RID mesh = RID::from_uint64(0x1562);
	sets.add_mesh(mesh);
	LocalVector<FakeNativeSets::Trace> traces;
	traces.push_back(fake_trace(mesh, 1, 5.0f)); // Band 1: 1500 triangles, parts 400, 400, 400, 300.
	sets.step(traces, 500);
	CHECK(sets.builds == 1);
	CHECK(sets.creates == 4); // Every part created at admission, one built.
	CHECK(sets.get_stats().build_bytes == 1500); // All reserved.
	sets.step(traces, 500);
	sets.step(traces, 500);
	CHECK_FALSE(traces[0].native);
	CHECK(sets.builds == 3);
	sets.step(traces, 500);
	CHECK(sets.builds == 4);
	sets.step(traces, 500);
	CHECK(traces[0].native);
	CHECK(traces[0].triangles == 1500);
	CHECK(sets.creates == 4);
	CHECK(sets.get_stats().build_bytes == 1500);
}

TEST_CASE("[NextGI][NativeRT] Sets: bands with equal cuts share one set and keep tracing it while its replacement builds") {
	using namespace RendererRD;
	using namespace TestNextGIRTNative;
	FakeNativeSets sets;
	sets.part_max_triangles = 300;
	const RID mesh = RID::from_uint64(0x1563);
	FakeNativeSets::FakeMesh &script = sets.add_mesh(mesh);
	LocalVector<FakeNativeSets::Trace> traces;
	traces.push_back(fake_trace(mesh, 1, 100.0f)); // Band 5 and band 6 both cap at the proxy's eps.
	traces.push_back(fake_trace(mesh, 2, 200.0f));
	sets.step(traces);
	CHECK(sets.extracts == 1);
	sets.step(traces);
	CHECK(traces[0].native);
	CHECK(traces[1].native);
	CHECK(sets.get_stats().instances_band == 2);
	CHECK(sets.get_stats().current_sets == 1);
	CHECK(sets.get_stats().shared_buckets == 1);
	CHECK(sets.get_stats().build_bytes == 800);
	CHECK(sets.set_count(mesh) == 1);

	// The residency changes both bands' cut. The new cut is extracted once and built over several
	// frames (300 triangles a frame); both instances trace the old set until it swaps in.
	script.generation++;
	script.salt++;
	const uint32_t extracts = sets.extracts;
	for (int i = 0; i < 6; i++) {
		sets.step(traces, 300);
		CHECK(traces[0].native);
		CHECK(traces[1].native);
		CHECK(sets.get_stats().instances_proxy == 0);
		CHECK(sets.get_stats().build_bytes <= sets.get_stats().memory_cap_bytes);
	}
	CHECK(sets.extracts == extracts + 1);
	CHECK(sets.get_stats().swaps == 0); // Swapped in an earlier frame.
	CHECK(sets.get_stats().current_sets == 1);
	CHECK(sets.get_stats().shared_buckets == 1);
	CHECK(sets.get_stats().build_bytes == 800); // The old set left; nothing is held twice.
	CHECK(sets.set_count(mesh) == 1);
	CHECK(sets.live_blases == 3);

	// Band 5 alone moves to a finer cut; band 6 keeps the shared set.
	traces[0].distance = 20.0f; // Band 3: 900 triangles.
	sets.step(traces);
	sets.step(traces);
	CHECK(traces[0].triangles == 900);
	CHECK(traces[1].triangles == 800);
	// Band 5 keeps the shared set as instance 1's previous set until it goes unused.
	CHECK(sets.get_stats().current_sets == 2);
	CHECK(sets.set_count(mesh) == 2);
}

TEST_CASE("[NextGI][NativeRT] Sets: when the cap shrinks, sets leave at the next TLAS phase and stay refused") {
	using namespace RendererRD;
	using namespace TestNextGIRTNative;
	FakeNativeSets sets;
	const RID coarse_mesh = RID::from_uint64(0x1564);
	const RID mesh = RID::from_uint64(0x1565);
	// The first mesh's resident cut is coarser than its proxy: it has demand and a proxy, no set.
	FakeNativeSets::FakeMesh &coarse = sets.add_mesh(coarse_mesh);
	for (FakeNativeSets::Level &level : coarse.levels) {
		level.max_error = 0.05f;
	}
	sets.add_mesh(mesh);
	LocalVector<FakeNativeSets::Trace> traces;
	traces.push_back(fake_trace(coarse_mesh, 1, 100.0f));
	traces.push_back(fake_trace(mesh, 2, 5.0f)); // 1500 triangles.
	traces.push_back(fake_trace(mesh, 3, 100.0f)); // 800 triangles.
	sets.step(traces);
	sets.step(traces);
	CHECK_FALSE(traces[0].native);
	CHECK(sets.get_stats().coarse_buckets == 1);
	CHECK(traces[1].native);
	CHECK(traces[2].native);
	CHECK(sets.get_stats().memory_cap_bytes == 4000);
	CHECK(sets.get_stats().build_bytes == 2300);

	// A proxy being rebuilt (no BLAS for a while) keeps its share: nothing leaves.
	sets.scripted[mesh].proxy_built = false;
	sets.step(traces);
	CHECK(sets.get_stats().cap_releases == 0);
	CHECK(sets.get_stats().memory_cap_bytes == 4000);
	sets.scripted[mesh].proxy_built = true;

	// The coarse mesh's last instance leaves: the cap falls to 2000, under the 2300 held. The far
	// band's set leaves at the next TLAS phase, its instance takes the proxy, and the set is not cut
	// or extracted again while the cap has no room for it.
	sets.scripted[coarse_mesh].alive = false;
	traces.remove_at(0);
	const uint32_t extracts = sets.extracts;
	sets.step(traces);
	CHECK(sets.get_stats().cap_releases == 1);
	CHECK(sets.get_stats().memory_cap_bytes == 2000);
	CHECK(sets.get_stats().build_bytes == 1500);
	CHECK(traces[0].native);
	CHECK_FALSE(traces[1].native);
	for (int i = 0; i < 5; i++) {
		sets.step(traces);
		CHECK(sets.get_stats().build_bytes <= sets.get_stats().memory_cap_bytes);
	}
	CHECK(sets.extracts == extracts);
	CHECK_FALSE(traces[1].native);

	// The AS memory line fills (3000 of 3600 bytes taken by proxies): native gives way to it.
	sets.set_as_line(3600, 3000);
	sets.step(traces);
	CHECK(sets.get_stats().memory_cap_bytes == 600);
	CHECK(sets.get_stats().build_bytes == 0);
	CHECK(sets.get_stats().cap_releases == 2);
	CHECK_FALSE(traces[0].native);
	// The line frees up: the nearer band's set is cut and built again; the far band's does not fit
	// beside it under the 2000-byte MN4 cap and stays refused.
	sets.set_as_line(UINT64_MAX, 0);
	for (int i = 0; i < 4; i++) {
		sets.step(traces);
		CHECK(sets.get_stats().build_bytes <= sets.get_stats().memory_cap_bytes);
	}
	CHECK(traces[0].native);
	CHECK(traces[0].triangles == 1500);
	CHECK_FALSE(traces[1].native);
	CHECK(sets.get_stats().build_bytes == 1500);
	sets.release_all();
	CHECK(sets.live_blases == 0);
	CHECK(sets.live_ranges == 0);
}

TEST_CASE("[NextGI][NativeRT] Sets: the ray origin eps is the band's target or the cut's own error, whichever is larger") {
	using namespace RendererRD;
	using namespace TestNextGIRTNative;
	FakeNativeSets sets;
	const RID mesh = RID::from_uint64(0x1566);
	FakeNativeSets::FakeMesh &script = sets.add_mesh(mesh);
	// Pages are missing under the fine level: its cut is 0.015 coarse, still within the proxy's 0.02.
	script.levels[1].max_error = 0.015f;
	script.proxy_bytes = 2000;
	LocalVector<FakeNativeSets::Trace> traces;
	traces.push_back(fake_trace(mesh, 1, 5.0f)); // Band 1, target 0.004.
	traces.push_back(fake_trace(mesh, 2, 10.0f)); // Band 2, target 0.008: the same cut, shared.
	traces.push_back(fake_trace(mesh, 3, 100.0f)); // Band 5, target 0.02.
	sets.step(traces);
	sets.step(traces);
	REQUIRE(traces[0].native);
	REQUIRE(traces[1].native);
	REQUIRE(traces[2].native);
	CHECK(traces[0].eps == doctest::Approx(0.015f));
	CHECK(traces[1].eps == doctest::Approx(0.015f));
	CHECK(traces[2].eps == doctest::Approx(0.02f));
	CHECK(sets.get_stats().shared_buckets == 1);
}

#endif // !_3D_DISABLED && RD_ENABLED
