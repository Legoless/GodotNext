/**************************************************************************/
/*  test_nextgeo_cards.h                                                  */
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

#include "../nextgeo_import.h"
#include "../nextgeo_rt_proxy.h"
#include "test_nextgeo_rt_proxy.h"

#include "core/crypto/crypto_core.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/marshalls.h"
#include "core/io/resource_saver.h"
#include "scene/resources/3d/importer_mesh.h"
#include "scene/resources/3d/virtual_geometry_data.h"
#include "tests/test_macros.h"
#include "tests/test_utils.h"

#ifdef RD_ENABLED
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_scene.h"
#include "servers/rendering/rendering_device.h"
#endif

namespace TestNextGeoCards {

using namespace TestNextGeo;
using namespace TestNextGeoRTProxy;

inline uint8_t triangle_dominant_axis(const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_c, bool &r_valid) {
	const Vector3 cross = (p_c - p_a).cross(p_b - p_a);
	const double ax = std::abs(double(cross.x));
	const double ay = std::abs(double(cross.y));
	const double az = std::abs(double(cross.z));
	if (!(ax > 0.0 || ay > 0.0 || az > 0.0) || !(cross.length_squared() > 1e-24)) {
		r_valid = false;
		return 0;
	}
	r_valid = true;
	if (ax >= ay && ax >= az) {
		return cross.x >= 0.0f ? 0 : 1;
	}
	if (ay >= az) {
		return cross.y >= 0.0f ? 2 : 3;
	}
	return cross.z >= 0.0f ? 4 : 5;
}

inline PackedByteArray sha256_bytes(const uint8_t *p_ptr, int64_t p_size) {
	PackedByteArray out;
	out.resize(32);
	CryptoCore::sha256(p_ptr, p_size, out.ptrw());
	return out;
}

inline void verify_proxy_card_table(const NextGeoRTProxy &p_proxy) {
	REQUIRE(p_proxy.card_descriptors.size() % int64_t(sizeof(NextGeoCardDescriptor)) == 0);
	REQUIRE(uint64_t(p_proxy.triangle_groups.size()) == p_proxy.triangle_count * sizeof(uint16_t));

	const uint32_t card_count = p_proxy.card_count();
	REQUIRE(card_count > 0);
	const float texel_size = 2.0f * p_proxy.eps;
	REQUIRE(texel_size > 0.0f);

	LocalVector<NextGeoCardDescriptor> cards;
	cards.resize(card_count);
	for (uint32_t i = 0; i < card_count; i++) {
		nextgeo_decode_card_descriptor(p_proxy.card_descriptors.ptr() + i * sizeof(NextGeoCardDescriptor), cards[i]);
		CHECK(cards[i].axis < 6);
		CHECK(cards[i].flags == 0);
		CHECK(cards[i].res_u >= 4);
		CHECK(cards[i].res_u <= 128);
		CHECK(cards[i].res_v >= 4);
		CHECK(cards[i].res_v <= 128);

		const int normal_axis = int(cards[i].axis / 2);
		const int u_axis = (normal_axis + 1) % 3;
		const int v_axis = (u_axis + 1) % 3;
		const uint16_t expected_u = uint16_t(CLAMP(std::ceil(double(2.0f * cards[i].half_extents[u_axis]) / double(texel_size) - 0.001), 4.0, 128.0));
		const uint16_t expected_v = uint16_t(CLAMP(std::ceil(double(2.0f * cards[i].half_extents[v_axis]) / double(texel_size) - 0.001), 4.0, 128.0));
		CHECK(cards[i].res_u == expected_u);
		CHECK(cards[i].res_v == expected_v);

		if (i > 0) {
			const bool ordered = cards[i - 1].group < cards[i].group || (cards[i - 1].group == cards[i].group && cards[i - 1].axis < cards[i].axis);
			CHECK_MESSAGE(ordered, "Stage-2 card descriptors are strictly sorted by (group, axis).");
		}
	}

	// Every non-degenerate proxy triangle's (group, dominant_axis) must resolve via binary search
	// on group and fit inside that card's tight AABB.
	uint32_t missing_cards = 0;
	uint32_t outside_bounds = 0;
	for (uint32_t part = 0; part < p_proxy.part_count(); part++) {
		const int32_t v_off = p_proxy.parts[int64_t(part) * 4 + 0];
		const int32_t i_off = p_proxy.parts[int64_t(part) * 4 + 2];
		const int32_t i_cnt = p_proxy.parts[int64_t(part) * 4 + 3];
		const uint32_t part_first_tri = uint32_t(i_off / 3);
		for (int32_t k = 0; k + 2 < i_cnt; k += 3) {
			const uint32_t tri_index = part_first_tri + uint32_t(k / 3);
			const uint16_t hit_group = decode_uint16(p_proxy.triangle_groups.ptr() + uint64_t(tri_index) * 2u);
			const Vector3 pa = proxy_vertex(p_proxy, v_off, proxy_index(p_proxy, int64_t(i_off) + k + 0));
			const Vector3 pb = proxy_vertex(p_proxy, v_off, proxy_index(p_proxy, int64_t(i_off) + k + 1));
			const Vector3 pc = proxy_vertex(p_proxy, v_off, proxy_index(p_proxy, int64_t(i_off) + k + 2));
			bool valid = false;
			const uint8_t axis = triangle_dominant_axis(pa, pb, pc, valid);
			if (!valid) {
				continue;
			}

			uint32_t lo = 0;
			uint32_t hi = card_count;
			while (lo < hi) {
				const uint32_t mid = (lo + hi) >> 1u;
				if (cards[mid].group < hit_group) {
					lo = mid + 1u;
				} else {
					hi = mid;
				}
			}
			const NextGeoCardDescriptor *matched = nullptr;
			for (uint32_t c = lo; c < card_count && cards[c].group == hit_group; c++) {
				if (cards[c].axis == axis) {
					matched = &cards[c];
					break;
				}
			}
			if (matched == nullptr) {
				missing_cards++;
				continue;
			}
			const Vector3 corners[3] = { pa, pb, pc };
			for (const Vector3 &pt : corners) {
				for (int a = 0; a < 3; a++) {
					if (std::abs(pt[a] - matched->center[a]) > matched->half_extents[a] + 1e-4f) {
						outside_bounds++;
					}
				}
			}
		}
	}
	CHECK_MESSAGE(missing_cards == 0, "Every proxy triangle finds its (group, dominant_axis) Stage-2 card via binary search.");
	CHECK_MESSAGE(outside_bounds == 0, "Every proxy triangle lies inside its Stage-2 card AABB.");
}

TEST_CASE("[NextGeo][Cards] Dominant-axis binning covers all 6 axes and clamps card resolutions to [4, 128]") {
	CHECK(sizeof(NextGeoCardDescriptor) == 32);

	// 40 m x 3 m x 0.1 m closed box: exercises all 6 dominant axes (+-X, +-Y, +-Z),
	// small 10 cm thickness clamped at 4 texels, and long 40 m span clamped at 128 texels.
	const NextGeoBuildInput input = make_wall_input();
	NextGeoBuildOutput dag;
	REQUIRE(nextgeo_build_dag(input, dag) == OK);

	NextGeoRTProxy proxy;
	REQUIRE(nextgeo_build_rt_proxy(dag, input_aabb(input), 0.02f, 1, proxy) == OK);
	verify_proxy_card_table(proxy);

	uint32_t axis_mask = 0;
	bool saw_min_clamp = false;
	bool saw_max_clamp = false;
	for (uint32_t i = 0; i < proxy.card_count(); i++) {
		NextGeoCardDescriptor desc;
		nextgeo_decode_card_descriptor(proxy.card_descriptors.ptr() + i * sizeof(NextGeoCardDescriptor), desc);
		axis_mask |= (1u << desc.axis);
		if (desc.res_u == 4 || desc.res_v == 4) {
			saw_min_clamp = true;
		}
		if (desc.res_u == 128 || desc.res_v == 128) {
			saw_max_clamp = true;
		}
	}
	CHECK(axis_mask == 0x3Fu);
	CHECK(saw_min_clamp);
	CHECK(saw_max_clamp);
}

TEST_CASE("[NextGeo][Cards] Triangle group table stays aligned across multi-surface counting-sort and >128k part splits") {
	// 1. Multi-surface mesh where surface 0 and surface 1 interleave in space.
	{
		NextGeoBuildInput multi;
		NextGeoSurfaceInput s0;
		NextGeoSurfaceInput s1;
		append_face_grid(s0, Vector3(-2.0f, 0.0f, 0.0f), Vector3(4.0f, 0.0f, 0.0f), Vector3(0.0f, 2.0f, 0.0f), 24, 16, Vector3(0, 0, -1));
		append_face_grid(s1, Vector3(0.0f, 0.0f, -2.0f), Vector3(0.0f, 0.0f, 4.0f), Vector3(0.0f, 2.0f, 0.0f), 24, 16, Vector3(1, 0, 0));
		multi.surfaces.push_back(s0);
		multi.surfaces.push_back(s1);
		multi.page_size = TEST_PAGE_SIZE;

		NextGeoBuildOutput dag;
		REQUIRE(nextgeo_build_dag(multi, dag) == OK);
		NextGeoRTProxy proxy;
		REQUIRE(nextgeo_build_rt_proxy(dag, input_aabb(multi), 0.02f, 2, proxy) == OK);
		verify_proxy_card_table(proxy);
	}

	// 2. >128k-triangle proxy split across multiple BLAS parts.
	{
		const NextGeoBuildInput big = make_big_grid_input();
		NextGeoBuildOutput dag;
		REQUIRE(nextgeo_build_dag(big, dag) == OK);
		NextGeoRTProxy proxy;
		REQUIRE(nextgeo_build_rt_proxy(dag, input_aabb(big), 0.02f, 1, proxy) == OK);
		REQUIRE(proxy.part_count() >= 2);
		verify_proxy_card_table(proxy);
	}
}

TEST_CASE("[NextGeo][Cards] Parallel slabs merged into one coarse DAG group split into non-occluding Stage-2 depth layers") {
	// Three parallel 4 m x 3 m x 0.1 m walls at z = -3 m, 0 m, +3 m in a single surface,
	// plus a side ramp bridging z = -3 m .. +3 m outside the walls' X range so both
	// normal-gap separation and 2D (u, v) interior SAT separation are exercised.
	NextGeoBuildInput input;
	NextGeoSurfaceInput surface;
	const float z_planes[3] = { -3.0f, 0.0f, 3.0f };
	for (float z_center : z_planes) {
		append_face_grid(surface, Vector3(-2.0f, 0.0f, z_center + 0.05f), Vector3(4.0f, 0.0f, 0.0f), Vector3(0.0f, 3.0f, 0.0f), 12, 8, Vector3(0, 0, 1));
		append_face_grid(surface, Vector3(2.0f, 0.0f, z_center - 0.05f), Vector3(-4.0f, 0.0f, 0.0f), Vector3(0.0f, 3.0f, 0.0f), 12, 8, Vector3(0, 0, -1));
	}
	// Side panel at x in [2.5, 3.5] tilted across z in [-3, +3] with +Z-dominant normal.
	append_face_grid(surface, Vector3(2.5f, 0.0f, -3.0f), Vector3(1.0f, 0.0f, 0.0f), Vector3(0.0f, 8.0f, 6.0f), 4, 16, Vector3(0.0f, -0.6f, 0.8f));
	input.surfaces.push_back(surface);
	input.page_size = TEST_PAGE_SIZE;

	NextGeoBuildOutput dag;
	REQUIRE(nextgeo_build_dag(input, dag) == OK);
	NextGeoRTProxy proxy;
	REQUIRE(nextgeo_build_rt_proxy(dag, input_aabb(input), 0.02f, 1, proxy) == OK);
	verify_proxy_card_table(proxy);

	// Collect the Stage-2 card groups assigned to proxy triangles on each of the three +Z walls (x in [-2, 2]).
	HashSet<uint16_t> wall_groups[3];
	const int32_t v_off = proxy.parts[0];
	const int32_t i_off = proxy.parts[2];
	const int32_t i_cnt = proxy.parts[3];
	for (int32_t k = 0; k + 2 < i_cnt; k += 3) {
		const uint32_t tri_idx = uint32_t(k / 3);
		const uint16_t grp = decode_uint16(proxy.triangle_groups.ptr() + uint64_t(tri_idx) * 2u);
		const Vector3 pa = proxy_vertex(proxy, v_off, proxy_index(proxy, int64_t(i_off) + k + 0));
		const Vector3 pb = proxy_vertex(proxy, v_off, proxy_index(proxy, int64_t(i_off) + k + 1));
		const Vector3 pc = proxy_vertex(proxy, v_off, proxy_index(proxy, int64_t(i_off) + k + 2));
		bool valid = false;
		const uint8_t axis = triangle_dominant_axis(pa, pb, pc, valid);
		if (!valid || axis != 4) {
			continue;
		}
		const Vector3 centroid = (pa + pb + pc) / 3.0f;
		if (centroid.x > 2.1f) {
			continue;
		}
		for (int w = 0; w < 3; w++) {
			if (std::abs(centroid.z - (z_planes[w] + 0.05f)) < 0.05f) {
				wall_groups[w].insert(grp);
			}
		}
	}
	for (int w = 0; w < 3; w++) {
		CHECK_FALSE(wall_groups[w].is_empty());
	}
	for (int a = 0; a < 3; a++) {
		for (int b = a + 1; b < 3; b++) {
			for (uint16_t ga : wall_groups[a]) {
				CHECK_MESSAGE(!wall_groups[b].has(ga), "Parallel +Z walls must not share a Stage-2 card group.");
			}
		}
	}
}

TEST_CASE("[NextGeo][Cards] Two imports produce bit-identical .ngm and VirtualGeometryData card tables") {
	Ref<ImporterMesh> src_a;
	Ref<ImporterMesh> src_b;
	src_a.instantiate();
	src_b.instantiate();

	const NextGeoBuildInput wall = make_wall_input();
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = wall.surfaces[0].positions;
	arrays[Mesh::ARRAY_NORMAL] = wall.surfaces[0].normals;
	arrays[Mesh::ARRAY_TEX_UV] = wall.surfaces[0].uv0;
	arrays[Mesh::ARRAY_INDEX] = wall.surfaces[0].indices;
	src_a->add_surface(Mesh::PRIMITIVE_TRIANGLES, arrays);
	src_b->add_surface(Mesh::PRIMITIVE_TRIANGLES, arrays);

	const String ngm_a = TestUtils::get_temp_path("ngm77_det_a.ngm");
	const String ngm_b = TestUtils::get_temp_path("ngm77_det_b.ngm");
	const String res_path = TestUtils::get_temp_path("ngm77_det.res");

	REQUIRE(src_a->build_virtual_geometry(ngm_a) == OK);
	REQUIRE(src_b->build_virtual_geometry(ngm_b) == OK);
	Ref<VirtualGeometryData> vgd_a = src_a->get_virtual_geometry();
	Ref<VirtualGeometryData> vgd_b = src_b->get_virtual_geometry();
	REQUIRE(vgd_a.is_valid());
	REQUIRE(vgd_b.is_valid());
	CHECK(vgd_a->get_format_version() == 2);
	CHECK(!vgd_a->get_card_descriptors().is_empty());
	CHECK(!vgd_a->get_rt_proxy_triangle_groups().is_empty());
	CHECK(vgd_a->get_ngm_sha256() == vgd_b->get_ngm_sha256());
	CHECK(vgd_a->get_card_descriptors() == vgd_b->get_card_descriptors());
	CHECK(vgd_a->get_rt_proxy_triangle_groups() == vgd_b->get_rt_proxy_triangle_groups());

	const Vector<uint8_t> bytes_ngm_a = FileAccess::get_file_as_bytes(ngm_a);
	const Vector<uint8_t> bytes_ngm_b = FileAccess::get_file_as_bytes(ngm_b);
	REQUIRE(!bytes_ngm_a.is_empty());
	CHECK(sha256_bytes(bytes_ngm_a.ptr(), bytes_ngm_a.size()) == sha256_bytes(bytes_ngm_b.ptr(), bytes_ngm_b.size()));

	// Re-saving at the same target path (re-import) reuses the resource UID and must be bit-identical.
	vgd_a->set_ngm_path("res://det.ngm");
	vgd_b->set_ngm_path("res://det.ngm");
	REQUIRE(ResourceSaver::save(vgd_a, res_path) == OK);
	const Vector<uint8_t> bytes_res_a = FileAccess::get_file_as_bytes(res_path);
	REQUIRE(ResourceSaver::save(vgd_b, res_path) == OK);
	const Vector<uint8_t> bytes_res_b = FileAccess::get_file_as_bytes(res_path);
	REQUIRE(!bytes_res_a.is_empty());
	CHECK(sha256_bytes(bytes_res_a.ptr(), bytes_res_a.size()) == sha256_bytes(bytes_res_b.ptr(), bytes_res_b.size()));

	DirAccess::remove_absolute(ngm_a);
	DirAccess::remove_absolute(ngm_b);
	DirAccess::remove_absolute(res_path);
	DirAccess::remove_absolute(res_path + ".uid");
}

#ifdef RD_ENABLED
TEST_CASE("[NextGeo][Cards] Mocked device without ray tracing allocates zero proxy arena and BLAS bytes") {
	using namespace RendererRD;

	const NextGeoBuildInput wall = make_wall_input();
	NextGeoBuildOutput dag;
	REQUIRE(nextgeo_build_dag(wall, dag) == OK);
	NextGeoRTProxy proxy;
	REQUIRE(nextgeo_build_rt_proxy(dag, input_aabb(wall), 0.02f, 1, proxy) == OK);

	RenderingServerTypes::VirtualGeometryDesc desc;
	desc.eligible_surface_mask = 1u;
	desc.rt_proxy_positions = proxy.positions;
	desc.rt_proxy_indices = proxy.indices;
	desc.rt_proxy_parts = proxy.parts;
	desc.rt_proxy_surface_ranges = proxy.surface_ranges;
	desc.rt_proxy_triangle_groups = proxy.triangle_groups;
	desc.card_descriptors = proxy.card_descriptors;
	desc.rt_proxy_eps = proxy.eps;

	// Mocked driverless RD without ray query (Vulkan without KHR_ray_query / D3D12 without DXR 1.1).
	RenderingDevice *mock_rd = memnew(RenderingDevice);
	mock_rd->set_feature_mock(true, 0);
	const bool mock_has_rq = mock_rd->has_feature(RenderingDevice::SUPPORTS_RAY_QUERY);
	CHECK_FALSE(mock_has_rq);

	NextGI::TierInputs d3d12_inputs;
	d3d12_inputs.forward_plus = true;
	d3d12_inputs.project_enabled = true;
	d3d12_inputs.hwrt_setting = NextGI::HARDWARE_RAY_TRACING_AUTO;
	NextGI::collect_device_inputs(mock_rd, "D3D12", RenderingDeviceEnums::DEVICE_TYPE_DISCRETE_GPU, d3d12_inputs);
	const NextGI::TierResult d3d12_tier = NextGI::evaluate_tier(d3d12_inputs);
	CHECK(d3d12_tier.tier == NextGI::TIER_FALLBACK);
	CHECK(d3d12_tier.reason == NextGI::D3D12_NO_DXR);

	NextGI::TierInputs vk_no_rq_inputs = d3d12_inputs;
	NextGI::collect_device_inputs(mock_rd, "Vulkan", RenderingDeviceEnums::DEVICE_TYPE_DISCRETE_GPU, vk_no_rq_inputs);
	const NextGI::TierResult vk_no_rq_tier = NextGI::evaluate_tier(vk_no_rq_inputs);
	CHECK(vk_no_rq_tier.tier == NextGI::TIER_FALLBACK);
	CHECK(vk_no_rq_tier.reason == NextGI::NO_RAY_QUERY);
	memdelete(mock_rd);

	NextGIArenaAllocator allocator(64 * 1024 * 1024);
	LocalVector<NextGIRTProxyPartUpload> uploads;
	uint32_t alpha_excluded = 0;

	// 1. D3D12 without DXR / Vulkan without ray query -> 0 arena bytes, 0 BLAS parts.
	CHECK_FALSE(nextgi_rt_plan_vgd_proxy(d3d12_tier.tier == NextGI::TIER_HWRT, mock_has_rq, 1, desc, 1, nullptr, nullptr, allocator, uploads, alpha_excluded));
	CHECK(allocator.get_allocated_bytes() == 0);
	CHECK(uploads.is_empty());

	// 2. HWRT off on a ray-query device -> 0 arena bytes, 0 BLAS parts.
	CHECK_FALSE(nextgi_rt_plan_vgd_proxy(false, true, 1, desc, 1, nullptr, nullptr, allocator, uploads, alpha_excluded));
	CHECK(allocator.get_allocated_bytes() == 0);
	CHECK(uploads.is_empty());

	// 3. HWRT active + ray query supported, but 0 live instances -> 0 arena bytes, 0 BLAS parts.
	CHECK_FALSE(nextgi_rt_plan_vgd_proxy(true, true, 0, desc, 1, nullptr, nullptr, allocator, uploads, alpha_excluded));
	CHECK(allocator.get_allocated_bytes() == 0);
	CHECK(uploads.is_empty());

	// 4. HWRT active + ray query + 1 live instance -> uploads planned cleanly with 16 B aligned group table.
	CHECK(nextgi_rt_plan_vgd_proxy(true, true, 1, desc, 1, nullptr, nullptr, allocator, uploads, alpha_excluded));
	CHECK(allocator.get_allocated_bytes() > 0);
	REQUIRE(uploads.size() == 1);
	REQUIRE(uploads[0].geometries.size() == 1);
	CHECK(uploads[0].geometries[0].has_stage2_cards);
	CHECK((uploads[0].geometries[0].group_offset & 15u) == 0u);
}
#endif // RD_ENABLED

} // namespace TestNextGeoCards
