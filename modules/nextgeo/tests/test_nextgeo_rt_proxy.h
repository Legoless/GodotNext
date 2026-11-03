/**************************************************************************/
/*  test_nextgeo_rt_proxy.h                                               */
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

#include "../nextgeo_rt_proxy.h"
#include "test_nextgeo.h"

#include "core/math/aabb.h"
#include "core/math/random_pcg.h"
#include "scene/resources/3d/virtual_geometry_data.h"
#include "tests/test_macros.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace TestNextGeoRTProxy {

using namespace TestNextGeo;

inline uint32_t proxy_index(const NextGeoRTProxy &p_proxy, int64_t p_index) {
	const uint8_t *bytes = p_proxy.indices.ptr() + p_index * 4;
	return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}

inline AABB input_aabb(const NextGeoBuildInput &p_input) {
	AABB aabb;
	bool first = true;
	for (int s = 0; s < p_input.surfaces.size(); s++) {
		for (int i = 0; i < p_input.surfaces[s].positions.size(); i++) {
			const Vector3 position = p_input.surfaces[s].positions[i];
			if (first) {
				aabb = AABB(position, Vector3());
				first = false;
			} else {
				aabb.expand_to(position);
			}
		}
	}
	return aabb;
}

// Every structural invariant of a built proxy against its DAG.
inline void check_proxy_invariants(const NextGeoBuildOutput &p_dag, const NextGeoRTProxy &p_proxy) {
	CHECK(p_proxy.error <= p_proxy.eps);
	CHECK(p_proxy.positions.size() % 3 == 0);
	CHECK(p_proxy.indices.size() % 4 == 0);
	CHECK(p_proxy.parts.size() % 4 == 0);
	const uint32_t part_count = p_proxy.part_count();
	REQUIRE(p_proxy.surface_ranges.size() == int64_t(part_count) * int64_t(p_proxy.surface_count) * 2);

	// The stored clusters are a valid partition of the source surface, and
	// the largest of their group errors is the stored proxy error.
	LocalVector<uint32_t> clusters;
	clusters.resize(p_proxy.clusters.size());
	LocalVector<uint64_t> surface_indices_expected;
	surface_indices_expected.resize_initialized(p_proxy.surface_count);
	float max_error = 0.0f;
	uint64_t cluster_triangles = 0;
	for (int i = 0; i < p_proxy.clusters.size(); i++) {
		const uint32_t cluster_id = uint32_t(p_proxy.clusters[i]);
		clusters[i] = cluster_id;
		REQUIRE(cluster_id < p_dag.clusters.size());
		const NextGeoCluster &cluster = p_dag.clusters[cluster_id];
		CHECK(cluster.group_error <= p_proxy.eps);
		CHECK(cluster.parent_error > p_proxy.eps);
		max_error = MAX(max_error, cluster.group_error);
		cluster_triangles += cluster.triangle_count;
		surface_indices_expected[cluster.surface_id] += uint64_t(cluster.triangle_count) * 3;
	}
	CHECK(nextgeo_check_cut(p_dag, clusters).is_valid_partition());
	CHECK(p_proxy.error == max_error);
	CHECK(p_proxy.triangle_count == cluster_triangles);

	// The stored clusters are in BVH8 leaf order of their groups, then by
	// cluster index (the packing order).
	{
		LocalVector<uint32_t> cluster_group;
		cluster_group.resize(p_dag.clusters.size());
		for (uint32_t g = 0; g < p_dag.groups.size(); g++) {
			for (uint32_t i = p_dag.groups[g].cluster_offset; i < p_dag.groups[g].cluster_offset + p_dag.groups[g].cluster_count; i++) {
				cluster_group[i] = g;
			}
		}
		LocalVector<uint32_t> group_rank;
		group_rank.resize(p_dag.groups.size());
		uint32_t next_rank = 0;
		LocalVector<uint32_t> stack;
		if (!p_dag.bvh.is_empty()) {
			stack.push_back(0);
		}
		LocalVector<uint8_t> group_ranked;
		group_ranked.resize_initialized(p_dag.groups.size());
		while (!stack.is_empty()) {
			const uint32_t entry = stack[stack.size() - 1];
			stack.remove_at(stack.size() - 1);
			if (entry == NEXTGEO_BVH_INVALID_CHILD) {
				continue;
			}
			if ((entry & NEXTGEO_BVH_LEAF_BIT) != 0) {
				const uint32_t group = entry & ~NEXTGEO_BVH_LEAF_BIT;
				if (group < p_dag.groups.size() && !group_ranked[group]) {
					group_ranked[group] = 1;
					group_rank[group] = next_rank++;
				}
				continue;
			}
			REQUIRE(entry < p_dag.bvh.size());
			const NextGeoBVHNode &node = p_dag.bvh[entry];
			for (uint32_t k = node.child_count; k > 0; k--) {
				stack.push_back(node.children[k - 1]);
			}
		}
		uint32_t out_of_order = 0;
		for (int i = 1; i < p_proxy.clusters.size(); i++) {
			const uint32_t prev = uint32_t(p_proxy.clusters[i - 1]);
			const uint32_t current = uint32_t(p_proxy.clusters[i]);
			const uint32_t prev_rank = group_rank[cluster_group[prev]];
			const uint32_t current_rank = group_rank[cluster_group[current]];
			if (current_rank < prev_rank || (current_rank == prev_rank && current < prev)) {
				out_of_order++;
			}
		}
		CHECK_MESSAGE(out_of_order == 0, "Proxy clusters follow BVH8 leaf order, then cluster index.");
	}

	// Parts: disjoint, in order, at most the cap, indices part-local, and the
	// surface ranges tile each part's index range in surface order.
	uint64_t part_triangles = 0;
	LocalVector<uint64_t> surface_indices_seen;
	surface_indices_seen.resize_initialized(p_proxy.surface_count);
	int32_t expected_vertex_offset = 0;
	int32_t expected_index_offset = 0;
	uint32_t bad_indices = 0;
	for (uint32_t part = 0; part < part_count; part++) {
		const int32_t vertex_offset = p_proxy.parts[int64_t(part) * 4 + 0];
		const int32_t vertex_count = p_proxy.parts[int64_t(part) * 4 + 1];
		const int32_t index_offset = p_proxy.parts[int64_t(part) * 4 + 2];
		const int32_t index_count = p_proxy.parts[int64_t(part) * 4 + 3];
		CHECK(vertex_offset == expected_vertex_offset);
		CHECK(index_offset == expected_index_offset);
		CHECK(vertex_count > 0);
		CHECK(index_count > 0);
		CHECK(index_count % 3 == 0);
		CHECK(uint32_t(index_count / 3) <= NEXTGEO_RT_PROXY_MAX_PART_TRIANGLES);
		part_triangles += uint32_t(index_count / 3);
		expected_vertex_offset += vertex_count;
		expected_index_offset += index_count;

		for (int32_t i = 0; i < index_count; i++) {
			if (proxy_index(p_proxy, int64_t(index_offset) + i) >= uint32_t(vertex_count)) {
				bad_indices++;
			}
		}

		int32_t range_cursor = 0;
		for (uint32_t s = 0; s < p_proxy.surface_count; s++) {
			const int64_t range_index = (int64_t(part) * int64_t(p_proxy.surface_count) + int64_t(s)) * 2;
			const int32_t first_index = p_proxy.surface_ranges[range_index + 0];
			const int32_t range_count = p_proxy.surface_ranges[range_index + 1];
			if (range_count == 0) {
				CHECK(first_index == 0);
			} else {
				CHECK(first_index == range_cursor);
				range_cursor += range_count;
			}
			surface_indices_seen[s] += uint64_t(range_count);
		}
		CHECK(range_cursor == index_count);
	}
	CHECK_MESSAGE(bad_indices == 0, "Every proxy index is part-local.");
	CHECK(part_triangles == cluster_triangles);
	CHECK(int64_t(expected_vertex_offset) * 3 == p_proxy.positions.size());
	CHECK(int64_t(expected_index_offset) * 4 == p_proxy.indices.size());
	for (uint32_t s = 0; s < p_proxy.surface_count; s++) {
		CHECK(surface_indices_seen[s] == surface_indices_expected[s]);
	}
}

// One tessellated box face: a (p_nu + 1) x (p_nv + 1) vertex grid from
// `p_origin` along `p_u` and `p_v`, with its own vertices.
inline void append_face_grid(NextGeoSurfaceInput &r_surface, const Vector3 &p_origin, const Vector3 &p_u, const Vector3 &p_v, int p_nu, int p_nv, const Vector3 &p_normal) {
	const int base = r_surface.positions.size();
	for (int v = 0; v <= p_nv; v++) {
		for (int u = 0; u <= p_nu; u++) {
			r_surface.positions.push_back(p_origin + p_u * (float(u) / p_nu) + p_v * (float(v) / p_nv));
			r_surface.normals.push_back(p_normal);
			r_surface.tangents.push_back(1.0f);
			r_surface.tangents.push_back(0.0f);
			r_surface.tangents.push_back(0.0f);
			r_surface.tangents.push_back(1.0f);
			r_surface.uv0.push_back(Vector2(float(u) / p_nu, float(v) / p_nv));
		}
	}
	const int columns = p_nu + 1;
	for (int v = 0; v < p_nv; v++) {
		for (int u = 0; u < p_nu; u++) {
			const int a = base + v * columns + u;
			const int b = a + 1;
			const int c = a + columns;
			const int d = c + 1;
			r_surface.indices.push_back(a);
			r_surface.indices.push_back(c);
			r_surface.indices.push_back(b);
			r_surface.indices.push_back(b);
			r_surface.indices.push_back(c);
			r_surface.indices.push_back(d);
		}
	}
}

// A single 40 m long, 3 m high, 10 cm thick wall as one surface (G5-6 shape).
inline NextGeoBuildInput make_wall_input() {
	NextGeoSurfaceInput surface;
	const float half_length = 20.0f;
	const float height = 3.0f;
	const float half_thickness = 0.05f;
	const Vector3 along(2.0f * half_length, 0.0f, 0.0f);
	const Vector3 up(0.0f, height, 0.0f);
	const Vector3 through(0.0f, 0.0f, 2.0f * half_thickness);
	append_face_grid(surface, Vector3(-half_length, 0.0f, half_thickness), along, up, 160, 12, Vector3(0, 0, 1));
	append_face_grid(surface, Vector3(half_length, 0.0f, -half_thickness), -along, up, 160, 12, Vector3(0, 0, -1));
	append_face_grid(surface, Vector3(-half_length, height, -half_thickness), along, through, 160, 1, Vector3(0, 1, 0));
	append_face_grid(surface, Vector3(-half_length, 0.0f, half_thickness), along, -through, 160, 1, Vector3(0, -1, 0));
	append_face_grid(surface, Vector3(half_length, 0.0f, -half_thickness), through, up, 1, 12, Vector3(1, 0, 0));
	append_face_grid(surface, Vector3(-half_length, 0.0f, half_thickness), -through, up, 1, 12, Vector3(-1, 0, 0));

	NextGeoBuildInput input;
	input.surfaces.push_back(surface);
	input.page_size = TEST_PAGE_SIZE;
	return input;
}

// A 1 m x 1 m grid of 1000 x 1000 quads (2M triangles), displaced per vertex
// by seeded noise of +-2 cm: far above eps_rt = 1/256 m, so the error cut
// stays near the source and far over the 128k part cap.
inline NextGeoBuildInput make_big_grid_input() {
	constexpr int quads = 1000;
	NextGeoSurfaceInput surface;
	const auto height = [](int p_x, int p_z) {
		return 0.02f * lattice_noise(p_x, p_z, TEST_SEED);
	};
	for (int z = 0; z <= quads; z++) {
		for (int x = 0; x <= quads; x++) {
			const float hx = height(x + 1, z) - height(x - 1, z);
			const float hz = height(x, z + 1) - height(x, z - 1);
			surface.positions.push_back(Vector3(x / float(quads), height(x, z), z / float(quads)));
			surface.normals.push_back(Vector3(-hx, 2.0f / quads, -hz).normalized());
			surface.tangents.push_back(1.0f);
			surface.tangents.push_back(0.0f);
			surface.tangents.push_back(0.0f);
			surface.tangents.push_back(1.0f);
			surface.uv0.push_back(Vector2(x / float(quads), z / float(quads)));
		}
	}
	const int columns = quads + 1;
	for (int z = 0; z < quads; z++) {
		for (int x = 0; x < quads; x++) {
			const int a = z * columns + x;
			const int b = a + 1;
			const int c = a + columns;
			const int d = c + 1;
			surface.indices.push_back(a);
			surface.indices.push_back(c);
			surface.indices.push_back(b);
			surface.indices.push_back(b);
			surface.indices.push_back(c);
			surface.indices.push_back(d);
		}
	}

	NextGeoBuildInput input;
	input.surfaces.push_back(surface);
	input.page_size = TEST_PAGE_SIZE;
	return input;
}

inline Vector3 proxy_vertex(const NextGeoRTProxy &p_proxy, int32_t p_part_vertex_offset, uint32_t p_local_index) {
	const int64_t base = (int64_t(p_part_vertex_offset) + int64_t(p_local_index)) * 3;
	return Vector3(p_proxy.positions[base + 0], p_proxy.positions[base + 1], p_proxy.positions[base + 2]);
}

// All double-sided ray/triangle hit distances against the whole proxy, sorted
// ascending with duplicates closer than 1e-5 merged (shared edges).
inline LocalVector<double> proxy_ray_hits(const NextGeoRTProxy &p_proxy, const Vector3 &p_origin, const Vector3 &p_direction) {
	LocalVector<double> hits;
	for (uint32_t part = 0; part < p_proxy.part_count(); part++) {
		const int32_t vertex_offset = p_proxy.parts[int64_t(part) * 4 + 0];
		const int32_t index_offset = p_proxy.parts[int64_t(part) * 4 + 2];
		const int32_t index_count = p_proxy.parts[int64_t(part) * 4 + 3];
		for (int32_t i = 0; i < index_count; i += 3) {
			const Vector3 a = proxy_vertex(p_proxy, vertex_offset, proxy_index(p_proxy, int64_t(index_offset) + i + 0));
			const Vector3 b = proxy_vertex(p_proxy, vertex_offset, proxy_index(p_proxy, int64_t(index_offset) + i + 1));
			const Vector3 c = proxy_vertex(p_proxy, vertex_offset, proxy_index(p_proxy, int64_t(index_offset) + i + 2));
			const Vector3 edge1 = b - a;
			const Vector3 edge2 = c - a;
			const Vector3 pvec = p_direction.cross(edge2);
			const double det = double(edge1.dot(pvec));
			if (std::abs(det) < 1e-12) {
				continue;
			}
			const double inv_det = 1.0 / det;
			const Vector3 tvec = p_origin - a;
			const double u = double(tvec.dot(pvec)) * inv_det;
			if (u < 0.0 || u > 1.0) {
				continue;
			}
			const Vector3 qvec = tvec.cross(edge1);
			const double v = double(p_direction.dot(qvec)) * inv_det;
			if (v < 0.0 || u + v > 1.0) {
				continue;
			}
			const double t = double(edge2.dot(qvec)) * inv_det;
			if (t > 0.0) {
				hits.push_back(t);
			}
		}
	}
	hits.sort();
	LocalVector<double> merged;
	for (double hit : hits) {
		if (merged.is_empty() || hit - merged[merged.size() - 1] > 1e-5) {
			merged.push_back(hit);
		}
	}
	return merged;
}

TEST_CASE("[NextGeo][RTProxy] eps_rt is min(max_extent/256, rt_max_error)") {
	CHECK(nextgeo_rt_proxy_epsilon(AABB(Vector3(), Vector3(2, 1, 1)), 0.02f) == doctest::Approx(2.0 / 256.0));
	CHECK(nextgeo_rt_proxy_epsilon(AABB(Vector3(), Vector3(40, 3, 0.1)), 0.02f) == doctest::Approx(0.02));
	CHECK(nextgeo_rt_proxy_epsilon(AABB(Vector3(), Vector3(40, 3, 0.1)), 0.05f) == doctest::Approx(0.05));
	// The 2 cm default binds exactly at 5.12 m of extent and above it.
	CHECK(nextgeo_rt_proxy_epsilon(AABB(Vector3(), Vector3(5.12, 1, 1)), 0.02f) == doctest::Approx(0.02));
	CHECK(nextgeo_rt_proxy_epsilon(AABB(Vector3(), Vector3(5.0, 1, 1)), 0.02f) == doctest::Approx(5.0 / 256.0));
	CHECK(nextgeo_rt_proxy_epsilon(AABB(Vector3(), Vector3(1, 30, 1)), 0.02f) == doctest::Approx(0.02));
}

TEST_CASE("[NextGeo][RTProxy] Invalid parameters fail, an empty DAG gives an empty proxy") {
	const AABB aabb(Vector3(-1, -1, -1), Vector3(2, 2, 2));
	NextGeoRTProxy proxy;
	CHECK(nextgeo_build_rt_proxy(get_sphere_dag(), aabb, 0.0f, 1, proxy) == ERR_INVALID_PARAMETER);
	CHECK(nextgeo_build_rt_proxy(get_sphere_dag(), aabb, -0.02f, 1, proxy) == ERR_INVALID_PARAMETER);
	CHECK(nextgeo_build_rt_proxy(get_sphere_dag(), aabb, std::numeric_limits<float>::quiet_NaN(), 1, proxy) == ERR_INVALID_PARAMETER);
	CHECK(nextgeo_build_rt_proxy(get_sphere_dag(), AABB(Vector3(), Vector3(-1, 1, 1)), 0.02f, 1, proxy) == ERR_INVALID_PARAMETER);
	// A surface count that hides a cluster's surface is invalid data.
	CHECK(nextgeo_build_rt_proxy(get_sphere_dag(), aabb, 0.02f, 0, proxy) == ERR_INVALID_DATA);

	const NextGeoBuildOutput empty_dag;
	REQUIRE(nextgeo_build_rt_proxy(empty_dag, aabb, 0.02f, 2, proxy) == OK);
	CHECK(proxy.part_count() == 0);
	CHECK(proxy.triangle_count == 0);
	CHECK(proxy.error == 0.0f);
	CHECK(proxy.eps == doctest::Approx(2.0 / 256.0));
	CHECK(proxy.positions.is_empty());
	CHECK(proxy.indices.is_empty());
	CHECK(proxy.clusters.is_empty());
}

TEST_CASE("[NextGeo][RTProxy] A 200k-triangle 2 m sphere fits in one part at eps_rt = 7.8 mm") {
	const NextGeoBuildOutput &dag = get_sphere_dag();
	NextGeoRTProxy proxy;
	REQUIRE(nextgeo_build_rt_proxy(dag, AABB(Vector3(-1, -1, -1), Vector3(2, 2, 2)), 0.02f, 1, proxy) == OK);
	CHECK(proxy.eps == doctest::Approx(0.0078125));
	REQUIRE(proxy.part_count() == 1);
	CHECK(proxy.triangle_count > 0);
	CHECK(proxy.triangle_count <= NEXTGEO_RT_PROXY_MAX_PART_TRIANGLES);
	check_proxy_invariants(dag, proxy);
}

TEST_CASE("[NextGeo][RTProxy] Two builds are byte-identical and fill VirtualGeometryData") {
	const NextGeoBuildOutput &dag = get_sphere_dag();
	const AABB aabb(Vector3(-1, -1, -1), Vector3(2, 2, 2));
	NextGeoRTProxy first;
	NextGeoRTProxy second;
	REQUIRE(nextgeo_build_rt_proxy(dag, aabb, 0.02f, 1, first) == OK);
	REQUIRE(nextgeo_build_rt_proxy(dag, aabb, 0.02f, 1, second) == OK);
	CHECK(first.positions == second.positions);
	CHECK(first.indices == second.indices);
	CHECK(first.parts == second.parts);
	CHECK(first.surface_ranges == second.surface_ranges);
	CHECK(first.clusters == second.clusters);
	CHECK(first.eps == second.eps);
	CHECK(first.error == second.error);
	// Positions are float32 and indices uint32, whatever the build precision.
	CHECK(sizeof(first.positions[0]) == 4);
	CHECK(first.indices.size() == first.parts[3] * 4);

	Ref<VirtualGeometryData> data;
	data.instantiate();
	REQUIRE(nextgeo_fill_vgd_rt_proxy(first, data) == OK);
	CHECK(data->get_rt_proxy_positions() == first.positions);
	CHECK(data->get_rt_proxy_indices() == first.indices);
	CHECK(data->get_rt_proxy_parts() == first.parts);
	CHECK(data->get_rt_proxy_surface_ranges() == first.surface_ranges);
	CHECK(data->get_rt_proxy_clusters() == first.clusters);
	CHECK(data->get_rt_proxy_eps() == first.eps);
	CHECK(data->get_rt_proxy_error() == first.error);

	ERR_PRINT_OFF;
	CHECK(nextgeo_fill_vgd_rt_proxy(first, Ref<VirtualGeometryData>()) == ERR_INVALID_PARAMETER);
	ERR_PRINT_ON;
}

TEST_CASE("[NextGeo][RTProxy] An ineligible surface is [0, 0] in every part") {
	const NextGeoBuildInput input = make_sibling_plane_input();
	NextGeoBuildOutput dag;
	REQUIRE(nextgeo_build_dag(input, dag) == OK);
	NextGeoRTProxy proxy;
	REQUIRE(nextgeo_build_rt_proxy(dag, input_aabb(input), 0.02f, 2, proxy) == OK);
	REQUIRE(proxy.part_count() >= 1);
	for (uint32_t part = 0; part < proxy.part_count(); part++) {
		const int64_t legacy = (int64_t(part) * 2 + 1) * 2;
		CHECK(proxy.surface_ranges[legacy + 0] == 0);
		CHECK(proxy.surface_ranges[legacy + 1] == 0);
		// The one eligible surface covers the whole part.
		const int64_t eligible = int64_t(part) * 4;
		CHECK(proxy.surface_ranges[eligible + 0] == 0);
		CHECK(proxy.surface_ranges[eligible + 1] == proxy.parts[int64_t(part) * 4 + 3]);
	}
	check_proxy_invariants(dag, proxy);
}

TEST_CASE("[NextGeo][RTProxy] Eight materials keep per-surface ranges in every part") {
	const NextGeoBuildInput input = make_seam_kit_input(TEST_PAGE_SIZE);
	const NextGeoBuildOutput &dag = get_seam_kit_dag();
	NextGeoRTProxy proxy;
	REQUIRE(nextgeo_build_rt_proxy(dag, input_aabb(input), 0.02f, 8, proxy) == OK);
	REQUIRE(proxy.part_count() >= 1);
	uint32_t surfaces_present = 0;
	for (uint32_t s = 0; s < proxy.surface_count; s++) {
		uint64_t indices = 0;
		for (uint32_t part = 0; part < proxy.part_count(); part++) {
			indices += uint32_t(proxy.surface_ranges[(int64_t(part) * 8 + int64_t(s)) * 2 + 1]);
		}
		if (indices > 0) {
			surfaces_present++;
		}
	}
	CHECK(surfaces_present == 8);
	check_proxy_invariants(dag, proxy);
}

TEST_CASE("[NextGeo][RTProxy] A 2M-triangle displaced 1 m grid splits without coarsening") {
	const NextGeoBuildInput input = make_big_grid_input();
	NextGeoBuildOutput dag;
	REQUIRE(nextgeo_build_dag(input, dag) == OK);
	NextGeoRTProxy proxy;
	REQUIRE(nextgeo_build_rt_proxy(dag, input_aabb(input), 0.02f, 1, proxy) == OK);
	CHECK(proxy.eps == doctest::Approx(1.0 / 256.0));
	CHECK(proxy.triangle_count > NEXTGEO_RT_PROXY_MAX_PART_TRIANGLES);
	REQUIRE(proxy.part_count() >= 2);
	check_proxy_invariants(dag, proxy);
}

TEST_CASE("[NextGeo][RTProxy] A 40 m mesh keeps its 10 cm walls under eps_rt = rt_max_error") {
	const NextGeoBuildInput input = make_wall_input();
	NextGeoBuildOutput dag;
	REQUIRE(nextgeo_build_dag(input, dag) == OK);
	const AABB aabb = input_aabb(input);
	REQUIRE(aabb.get_longest_axis_size() == doctest::Approx(40.0));

	NextGeoRTProxy looser;
	REQUIRE(nextgeo_build_rt_proxy(dag, aabb, 0.05f, 1, looser) == OK);
	CHECK(looser.eps == doctest::Approx(0.05));
	CHECK(looser.error <= looser.eps);

	NextGeoRTProxy proxy;
	REQUIRE(nextgeo_build_rt_proxy(dag, aabb, 0.02f, 1, proxy) == OK);
	// max_extent/256 would be 15.6 cm, coarser than the wall; the 2 cm default binds.
	CHECK(proxy.eps == doctest::Approx(0.02));
	check_proxy_invariants(dag, proxy);

	// 1k seeded rays across the wall each enter and leave the proxy once,
	// 10 cm plus or minus 2 * eps_rt apart.
	RandomPCG rng(TEST_SEED);
	uint32_t bad_rays = 0;
	double worst_deviation = 0.0;
	for (int i = 0; i < 1000; i++) {
		const float x = float(rng.randd() * 38.0 - 19.0);
		const float y = float(0.2 + rng.randd() * 2.6);
		const LocalVector<double> hits = proxy_ray_hits(proxy, Vector3(x, y, -1.0f), Vector3(0, 0, 1));
		if (hits.size() != 2) {
			bad_rays++;
			continue;
		}
		const double thickness = hits[1] - hits[0];
		worst_deviation = MAX(worst_deviation, std::abs(thickness - 0.1));
		if (std::abs(thickness - 0.1) > 2.0 * double(proxy.eps)) {
			bad_rays++;
		}
	}
	CHECK_MESSAGE(bad_rays == 0, "Every ray hits the proxy twice, 10 cm +- 2*eps_rt apart (worst deviation ", worst_deviation, ").");
}

} // namespace TestNextGeoRTProxy
