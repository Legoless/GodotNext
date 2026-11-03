/**************************************************************************/
/*  test_nextgeo.h                                                        */
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

#include "../nextgeo_bvh.h"
#include "../nextgeo_cut.h"
#include "../nextgeo_dag_builder.h"

#include "core/math/math_funcs.h"
#include "core/math/random_pcg.h"
#include "core/math/transform_3d.h"
#include "core/templates/hash_set.h"
#include "tests/test_macros.h"

#include <cfloat>
#include <cmath>
#include <cstring>

namespace TestNextGeo {

// The tests' own page size. It is not the provisional value, which #72's owner picks.
constexpr uint32_t TEST_PAGE_SIZE = 64 * 1024;
constexpr uint64_t TEST_SEED = 0x6e657874u;
constexpr double TEST_SCALES[3] = { 0.5, 1.0, 4.0 };

// Seeded value noise in [-1, 1] on an integer lattice, so duplicated border vertices of
// different surfaces get identical positions.
inline float lattice_noise(int p_x, int p_z, uint64_t p_seed) {
	uint64_t h = p_seed ^ (uint64_t(uint32_t(p_x)) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(uint32_t(p_z)) * 0xC2B2AE3D27D4EB4Full);
	h ^= h >> 33;
	h *= 0xFF51AFD7ED558CCDull;
	h ^= h >> 33;
	h *= 0xC4CEB9FE1A85EC53ull;
	h ^= h >> 33;
	return float(double(h >> 11) / double(1ull << 53)) * 2.0f - 1.0f;
}

enum HeightMode {
	HEIGHT_FLAT,
	HEIGHT_TERRAIN,
};

inline float grid_height(HeightMode p_mode, int p_x, int p_z) {
	if (p_mode == HEIGHT_FLAT) {
		return 0.0f;
	}
	return 2.0f * std::sin(p_x * 0.11f) * std::cos(p_z * 0.07f) + 0.3f * lattice_noise(p_x, p_z, TEST_SEED);
}

// Appends the quads [x0, x1) x [z0, z1) of a global lattice to a surface, with their own
// vertices. `p_tangent_w` sets the tangent handedness of the block.
inline void append_grid_block(NextGeoSurfaceInput &r_surface, int p_x0, int p_x1, int p_z0, int p_z1, float p_step, float p_tangent_w, HeightMode p_height) {
	const int base = r_surface.positions.size();
	const int columns = p_x1 - p_x0 + 1;
	for (int z = p_z0; z <= p_z1; z++) {
		for (int x = p_x0; x <= p_x1; x++) {
			const float h = grid_height(p_height, x, z);
			const float hx = grid_height(p_height, x + 1, z) - grid_height(p_height, x - 1, z);
			const float hz = grid_height(p_height, x, z + 1) - grid_height(p_height, x, z - 1);
			r_surface.positions.push_back(Vector3(x * p_step, h, z * p_step));
			r_surface.normals.push_back(Vector3(-hx, 2.0f * p_step, -hz).normalized());
			r_surface.tangents.push_back(1.0f);
			r_surface.tangents.push_back(0.0f);
			r_surface.tangents.push_back(0.0f);
			r_surface.tangents.push_back(p_tangent_w);
			// Mirrored UVs on the negative-handedness side.
			r_surface.uv0.push_back(Vector2(p_tangent_w * x / 64.0f, z / 64.0f));
		}
	}
	for (int z = 0; z < p_z1 - p_z0; z++) {
		for (int x = 0; x < p_x1 - p_x0; x++) {
			const int a = base + z * columns + x;
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

// A UV sphere with a duplicated UV seam column and seeded radial noise:
// 2 * segments * (rings - 1) triangles.
inline NextGeoSurfaceInput make_sphere(int p_segments, int p_rings) {
	NextGeoSurfaceInput surface;
	const auto add_vertex = [&](int p_ring, int p_segment) {
		const double theta = Math::PI * p_ring / p_rings;
		const double phi = Math::TAU * p_segment / p_segments;
		const bool pole = p_ring == 0 || p_ring == p_rings;
		const float noise = pole ? 1.0f : 1.0f + 0.002f * lattice_noise(p_ring, p_segment % p_segments, TEST_SEED);
		const Vector3 normal(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
		surface.positions.push_back(normal * noise);
		surface.normals.push_back(normal);
		surface.tangents.push_back(float(-std::sin(phi)));
		surface.tangents.push_back(0.0f);
		surface.tangents.push_back(float(std::cos(phi)));
		surface.tangents.push_back(1.0f);
		surface.uv0.push_back(Vector2(pole ? 0.5f : float(p_segment) / p_segments, float(p_ring) / p_rings));
	};

	add_vertex(0, 0);
	for (int r = 1; r < p_rings; r++) {
		for (int s = 0; s <= p_segments; s++) {
			add_vertex(r, s);
		}
	}
	add_vertex(p_rings, 0);

	const int top = 0;
	const int bottom = surface.positions.size() - 1;
	const auto ring_vertex = [&](int p_ring, int p_segment) {
		return 1 + (p_ring - 1) * (p_segments + 1) + p_segment;
	};
	for (int s = 0; s < p_segments; s++) {
		surface.indices.push_back(top);
		surface.indices.push_back(ring_vertex(1, s + 1));
		surface.indices.push_back(ring_vertex(1, s));
	}
	for (int r = 1; r < p_rings - 1; r++) {
		for (int s = 0; s < p_segments; s++) {
			const int a = ring_vertex(r, s);
			const int b = ring_vertex(r, s + 1);
			const int c = ring_vertex(r + 1, s);
			const int d = ring_vertex(r + 1, s + 1);
			surface.indices.push_back(a);
			surface.indices.push_back(b);
			surface.indices.push_back(c);
			surface.indices.push_back(b);
			surface.indices.push_back(d);
			surface.indices.push_back(c);
		}
	}
	for (int s = 0; s < p_segments; s++) {
		surface.indices.push_back(bottom);
		surface.indices.push_back(ring_vertex(p_rings - 1, s));
		surface.indices.push_back(ring_vertex(p_rings - 1, s + 1));
	}
	return surface;
}

inline NextGeoBuildInput make_sphere_input() {
	NextGeoBuildInput input;
	input.surfaces.push_back(make_sphere(400, 251)); // 200,000 triangles.
	input.page_size = TEST_PAGE_SIZE;
	return input;
}

// Eight surfaces (materials) in a 4 x 2 tile layout over one terrain; tile borders are
// duplicated vertices, so every material border is an attribute seam.
inline NextGeoBuildInput make_seam_kit_input(uint32_t p_page_size) {
	NextGeoBuildInput input;
	for (int tile = 0; tile < 8; tile++) {
		NextGeoSurfaceInput surface;
		const int x0 = (tile % 4) * 64;
		const int z0 = (tile / 4) * 64;
		append_grid_block(surface, x0, x0 + 64, z0, z0 + 64, 0.25f, 1.0f, HEIGHT_TERRAIN);
		input.surfaces.push_back(surface);
	}
	input.page_size = p_page_size;
	return input;
}

// A flat plane: the eligible surface covers x in [0, 32] with a handedness seam at
// x = 16 (w = +1 left, w = -1 right); an ineligible sibling covers x in [32, 64].
inline NextGeoBuildInput make_sibling_plane_input() {
	NextGeoBuildInput input;
	NextGeoSurfaceInput eligible;
	append_grid_block(eligible, 0, 16, 0, 64, 1.0f, 1.0f, HEIGHT_FLAT);
	append_grid_block(eligible, 16, 32, 0, 64, 1.0f, -1.0f, HEIGHT_FLAT);
	NextGeoSurfaceInput ineligible;
	append_grid_block(ineligible, 32, 64, 0, 64, 1.0f, 1.0f, HEIGHT_FLAT);
	ineligible.eligible = false;
	input.surfaces.push_back(eligible);
	input.surfaces.push_back(ineligible);
	input.page_size = TEST_PAGE_SIZE;
	return input;
}

inline const NextGeoBuildOutput &get_sphere_dag() {
	static NextGeoBuildOutput output;
	static bool built = false;
	if (!built) {
		REQUIRE(nextgeo_build_dag(make_sphere_input(), output) == OK);
		built = true;
	}
	return output;
}

inline const NextGeoBuildOutput &get_seam_kit_dag() {
	static NextGeoBuildOutput output;
	static bool built = false;
	if (!built) {
		REQUIRE(nextgeo_build_dag(make_seam_kit_input(TEST_PAGE_SIZE), output) == OK);
		built = true;
	}
	return output;
}

inline double sphere_gap(const float p_outer[4], const float p_inner[4]) {
	// Positive or zero when `p_outer` encloses `p_inner`.
	const double dx = double(p_outer[0]) - p_inner[0];
	const double dy = double(p_outer[1]) - p_inner[1];
	const double dz = double(p_outer[2]) - p_inner[2];
	return double(p_outer[3]) - (std::sqrt(dx * dx + dy * dy + dz * dz) + p_inner[3]);
}

inline bool same_sphere(const float p_a[4], const float p_b[4]) {
	return std::memcmp(p_a, p_b, sizeof(float) * 4) == 0;
}

inline bool aabb_contains(const float p_min[3], const float p_max[3], const float p_inner_min[3], const float p_inner_max[3]) {
	for (int k = 0; k < 3; k++) {
		if (p_inner_min[k] < p_min[k] || p_inner_max[k] > p_max[k]) {
			return false;
		}
	}
	return true;
}

// Page bytes of one cluster: geometry payload plus its skinning section, if any.
inline uint32_t cluster_page_bytes(const NextGeoBuildOutput &p_dag, uint32_t p_index) {
	const NextGeoCluster &cluster = p_dag.clusters[p_index];
	uint32_t bytes = nextgeo_cluster_payload_bytes(cluster.vertex_count, cluster.triangle_count, p_dag.vertex_stride);
	if (p_dag.skin_influences > 0) {
		bytes += nextgeo_cluster_skin_bytes(p_dag.cluster_skin[p_index].bone_count, cluster.vertex_count, p_dag.skin_influences);
	}
	return bytes;
}

// Checks every structural invariant of a built DAG against its input.
inline void check_dag_invariants(const NextGeoBuildInput &p_input, const NextGeoBuildOutput &p_dag) {
	const uint32_t cluster_count = p_dag.clusters.size();
	const uint32_t group_count = p_dag.groups.size();
	REQUIRE(cluster_count > 0);
	REQUIRE(group_count > 0);
	REQUIRE(!p_dag.pages.is_empty());
	REQUIRE(!p_dag.bvh.is_empty());

	// Clusters: at most 128 triangles from one eligible surface, data copied unchanged.
	uint32_t bad_clusters = 0;
	uint32_t bad_vertices = 0;
	uint64_t leaf_triangles = 0;
	for (const NextGeoCluster &cluster : p_dag.clusters) {
		if (cluster.triangle_count == 0 || cluster.triangle_count > NEXTGEO_MAX_CLUSTER_TRIANGLES || cluster.vertex_count == 0 || cluster.vertex_count > NEXTGEO_MAX_CLUSTER_VERTICES ||
				cluster.surface_id >= uint32_t(p_input.surfaces.size()) || !p_input.surfaces[cluster.surface_id].eligible) {
			bad_clusters++;
			continue;
		}
		if (cluster.refined_group < 0) {
			leaf_triangles += cluster.triangle_count;
		}
		for (uint32_t j = 0; j < uint32_t(cluster.triangle_count) * 3; j++) {
			if (p_dag.triangles[cluster.triangle_offset * 3 + j] >= cluster.vertex_count) {
				bad_clusters++;
			}
		}
		const NextGeoSurfaceInput &surface = p_input.surfaces[cluster.surface_id];
		for (uint32_t v = cluster.vertex_offset; v < cluster.vertex_offset + cluster.vertex_count; v++) {
			const uint32_t source = p_dag.vertex_sources[v];
			const float *vertex = &p_dag.vertices[v * p_dag.vertex_stride];
			if (source >= uint32_t(surface.positions.size())) {
				bad_vertices++;
				continue;
			}
			const Vector3 position = surface.positions[source];
			const Vector2 uv = surface.uv0.is_empty() ? Vector2() : surface.uv0[source];
			const float tangent_w = surface.tangents.is_empty() ? 1.0f : surface.tangents[source * 4 + 3];
			if (vertex[NEXTGEO_VERTEX_OFFSET_POSITION + 0] != float(position.x) || vertex[NEXTGEO_VERTEX_OFFSET_POSITION + 1] != float(position.y) ||
					vertex[NEXTGEO_VERTEX_OFFSET_POSITION + 2] != float(position.z) || vertex[NEXTGEO_VERTEX_OFFSET_UV0 + 0] != float(uv.x) ||
					vertex[NEXTGEO_VERTEX_OFFSET_UV0 + 1] != float(uv.y) || vertex[NEXTGEO_VERTEX_OFFSET_TANGENT + 3] != tangent_w) {
				bad_vertices++;
			}
		}
	}
	CHECK_MESSAGE(bad_clusters == 0, "Every cluster has 1-128 triangles and vertices from one eligible surface.");
	CHECK_MESSAGE(bad_vertices == 0, "Every output vertex is an unchanged copy of its source vertex.");
	CHECK(leaf_triangles == p_dag.source_triangle_count);

	// Groups: contiguous cluster ranges, group-shared LOD bounds, enclosure and monotonic error.
	uint32_t next_cluster = 0;
	uint32_t bad_groups = 0;
	uint32_t bad_shared = 0;
	uint32_t bad_enclosure = 0;
	uint32_t max_depth = 0;
	for (const NextGeoGroup &group : p_dag.groups) {
		if (group.cluster_offset != next_cluster || group.cluster_count == 0 || group.page_id >= p_dag.pages.size()) {
			bad_groups++;
		}
		next_cluster = group.cluster_offset + group.cluster_count;
		max_depth = MAX(max_depth, group.depth);
		uint32_t bytes = 0;
		for (uint32_t i = group.cluster_offset; i < next_cluster && i < cluster_count; i++) {
			const NextGeoCluster &cluster = p_dag.clusters[i];
			bytes += cluster_page_bytes(p_dag, i);
			if (!same_sphere(cluster.parent_lod, group.parent_lod) || cluster.parent_error != group.parent_error || cluster.page_id != group.page_id) {
				bad_shared++;
			}
			if (sphere_gap(group.parent_lod, cluster.group_lod) < 0.0 || !(group.parent_error >= cluster.group_error)) {
				bad_enclosure++;
			}
			if (cluster.refined_group >= 0) {
				if (uint32_t(cluster.refined_group) >= group_count) {
					bad_shared++;
					continue;
				}
				const NextGeoGroup &refined = p_dag.groups[cluster.refined_group];
				if (!same_sphere(cluster.group_lod, refined.parent_lod) || cluster.group_error != refined.parent_error || refined.parent_error >= FLT_MAX || refined.depth >= group.depth) {
					bad_shared++;
				}
			} else if (cluster.group_error != 0.0f || !same_sphere(cluster.group_lod, cluster.bounds)) {
				bad_shared++;
			}
		}
		if (nextgeo_page_bytes(bytes) > p_dag.page_size) {
			bad_groups++;
		}
	}
	CHECK(next_cluster == cluster_count);
	CHECK_MESSAGE(bad_groups == 0, "Groups have contiguous cluster ranges and fit in one page.");
	CHECK_MESSAGE(bad_shared == 0, "group_lod and parent_lod are shared as the cluster record requires.");
	CHECK_MESSAGE(bad_enclosure == 0, "Parent LOD spheres enclose the members' spheres, and errors are monotonic.");
	CHECK(p_dag.dag_depth == max_depth + 1);

	// Pages: whole groups, at most page_size bytes, root groups first, parents before children.
	uint32_t next_group = 0;
	uint32_t bad_pages = 0;
	for (uint32_t p = 0; p < p_dag.pages.size(); p++) {
		const NextGeoPage &page = p_dag.pages[p];
		if (page.group_offset != next_group || page.group_count == 0 || page.byte_size > p_dag.page_size) {
			bad_pages++;
		}
		uint32_t bytes = 0;
		uint32_t clusters = 0;
		for (uint32_t g = page.group_offset; g < page.group_offset + page.group_count && g < group_count; g++) {
			const NextGeoGroup &group = p_dag.groups[g];
			if (group.page_id != p || (g == page.group_offset && group.cluster_offset != page.cluster_offset)) {
				bad_pages++;
			}
			const bool root = group.parent_error >= FLT_MAX;
			if (root != (p < p_dag.root_page_count)) {
				bad_pages++;
			}
			clusters += group.cluster_count;
			for (uint32_t i = group.cluster_offset; i < group.cluster_offset + group.cluster_count; i++) {
				bytes += cluster_page_bytes(p_dag, i);
			}
		}
		if (nextgeo_page_bytes(bytes) != page.byte_size || clusters != page.cluster_count) {
			bad_pages++;
		}
		next_group = page.group_offset + page.group_count;
	}
	CHECK(next_group == group_count);
	CHECK(p_dag.root_page_count >= 1);
	CHECK_MESSAGE(bad_pages == 0, "Pages hold whole groups within page_size, and root pages hold exactly the root groups.");

	uint32_t bad_order = 0;
	for (const NextGeoCluster &cluster : p_dag.clusters) {
		if (cluster.refined_group >= 0 && uint32_t(cluster.refined_group) < group_count && cluster.page_id > p_dag.groups[cluster.refined_group].page_id) {
			bad_order++;
		}
	}
	CHECK_MESSAGE(bad_order == 0, "A parent group's page never comes after its child's.");

	// BVH8: every group is one leaf; nodes enclose their children.
	LocalVector<uint32_t> leaf_seen;
	leaf_seen.resize_initialized(group_count);
	uint32_t bad_nodes = 0;
	for (const NextGeoBVHNode &node : p_dag.bvh) {
		if (node.child_count == 0 || node.child_count > NEXTGEO_BVH_WIDTH) {
			bad_nodes++;
			continue;
		}
		float max_error = 0.0f;
		for (uint32_t k = 0; k < node.child_count; k++) {
			const uint32_t child = node.children[k];
			const float *sphere = nullptr;
			const float *child_min = nullptr;
			const float *child_max = nullptr;
			float error = 0.0f;
			if (child & NEXTGEO_BVH_LEAF_BIT) {
				const uint32_t g = child & ~NEXTGEO_BVH_LEAF_BIT;
				if (g >= group_count) {
					bad_nodes++;
					continue;
				}
				leaf_seen[g]++;
				sphere = p_dag.groups[g].parent_lod;
				error = p_dag.groups[g].parent_error;
				child_min = p_dag.groups[g].aabb_min;
				child_max = p_dag.groups[g].aabb_max;
			} else {
				if (child == 0 || child >= p_dag.bvh.size()) {
					bad_nodes++;
					continue;
				}
				sphere = p_dag.bvh[child].lod_sphere;
				error = p_dag.bvh[child].max_parent_error;
				child_min = p_dag.bvh[child].aabb_min;
				child_max = p_dag.bvh[child].aabb_max;
			}
			max_error = MAX(max_error, error);
			if (sphere_gap(node.lod_sphere, sphere) < 0.0 || !aabb_contains(node.aabb_min, node.aabb_max, child_min, child_max)) {
				bad_nodes++;
			}
		}
		if (node.max_parent_error != max_error) {
			bad_nodes++;
		}
	}
	uint32_t bad_leaves = 0;
	for (uint32_t g = 0; g < group_count; g++) {
		if (leaf_seen[g] != 1) {
			bad_leaves++;
		}
	}
	CHECK_MESSAGE(bad_nodes == 0, "BVH8 nodes enclose their children and carry the maximum parent error.");
	CHECK_MESSAGE(bad_leaves == 0, "Every group is exactly one BVH8 leaf.");
}

inline NextGeoCutParams random_view(RandomPCG &r_rng, const NextGeoBuildOutput &p_dag, double p_scale) {
	const float *bounds = p_dag.bvh[0].lod_sphere;
	const double z = r_rng.randd() * 2.0 - 1.0;
	const double phi = r_rng.randd() * Math::TAU;
	const double planar = std::sqrt(MAX(0.0, 1.0 - z * z));
	// Log-uniform distance from inside the mesh to far away.
	const double distance = double(bounds[3]) * p_scale * std::exp(std::log(0.05) + r_rng.randd() * (std::log(5000.0) - std::log(0.05)));

	NextGeoCutParams params;
	params.instance_scale = p_scale;
	params.eye = Vector3(
			bounds[0] * p_scale + planar * std::cos(phi) * distance,
			bounds[1] * p_scale + z * distance,
			bounds[2] * p_scale + planar * std::sin(phi) * distance);
	params.fov_y = Math::deg_to_rad(30.0 + r_rng.randd() * 60.0);
	params.viewport_height = 720.0 + r_rng.randd() * 1440.0;
	params.lod_bias = 0.5 + r_rng.randd() * 1.5;
	return params;
}

// Random resident page set, closed parents-first, with the root pages always resident.
inline void random_resident_pages(RandomPCG &r_rng, const NextGeoBuildOutput &p_dag, const LocalVector<LocalVector<uint32_t>> &p_page_parents, LocalVector<uint8_t> &r_resident) {
	const uint32_t page_count = p_dag.pages.size();
	r_resident.resize(page_count);
	const double keep = r_rng.randd();
	for (uint32_t p = 0; p < page_count; p++) {
		bool resident = p < p_dag.root_page_count || r_rng.randd() < keep;
		for (uint32_t parent : p_page_parents[p]) {
			resident = resident && r_resident[parent];
		}
		r_resident[p] = resident ? 1 : 0;
	}
}

inline void collect_page_parents(const NextGeoBuildOutput &p_dag, LocalVector<LocalVector<uint32_t>> &r_page_parents) {
	r_page_parents.resize(p_dag.pages.size());
	for (const NextGeoCluster &cluster : p_dag.clusters) {
		if (cluster.refined_group < 0) {
			continue;
		}
		const uint32_t child_page = p_dag.groups[cluster.refined_group].page_id;
		if (cluster.page_id != child_page && !r_page_parents[child_page].has(cluster.page_id)) {
			r_page_parents[child_page].push_back(cluster.page_id);
		}
	}
}

inline uint32_t count_invalid_cuts(const NextGeoBuildOutput &p_dag, uint32_t p_cuts, uint64_t p_seed, uint32_t &r_min_cut, uint32_t &r_max_cut) {
	RandomPCG rng(p_seed);
	LocalVector<uint32_t> cut;
	uint32_t invalid = 0;
	r_min_cut = UINT32_MAX;
	r_max_cut = 0;
	for (uint32_t i = 0; i < p_cuts; i++) {
		const NextGeoCutParams params = random_view(rng, p_dag, TEST_SCALES[i % 3]);
		nextgeo_select_cut(p_dag, params, nullptr, cut);
		if (!nextgeo_check_cut(p_dag, cut).is_valid_partition()) {
			invalid++;
		}
		r_min_cut = MIN(r_min_cut, cut.size());
		r_max_cut = MAX(r_max_cut, cut.size());
	}
	return invalid;
}

inline uint32_t count_invalid_resident_cuts(const NextGeoBuildOutput &p_dag, uint32_t p_cuts, uint64_t p_seed) {
	RandomPCG rng(p_seed);
	LocalVector<LocalVector<uint32_t>> page_parents;
	collect_page_parents(p_dag, page_parents);
	LocalVector<uint8_t> resident;
	LocalVector<uint32_t> cut;
	uint32_t invalid = 0;
	for (uint32_t i = 0; i < p_cuts; i++) {
		random_resident_pages(rng, p_dag, page_parents, resident);
		const NextGeoCutParams params = random_view(rng, p_dag, TEST_SCALES[i % 3]);
		nextgeo_select_cut(p_dag, params, resident.ptr(), cut);
		if (!nextgeo_check_cut(p_dag, cut).is_valid_partition()) {
			invalid++;
		}
	}
	return invalid;
}

inline uint64_t count_monotonicity_violations(const NextGeoBuildOutput &p_dag, uint32_t p_views, uint64_t p_seed, uint64_t &r_edges) {
	RandomPCG rng(p_seed);
	uint64_t violations = 0;
	r_edges = 0;
	for (uint32_t v = 0; v < p_views; v++) {
		for (double scale : TEST_SCALES) {
			const NextGeoCutParams params = random_view(rng, p_dag, scale);
			const double pixel_scale = nextgeo_cut_pixel_scale(params);
			for (const NextGeoCluster &cluster : p_dag.clusters) {
				if (cluster.refined_group < 0) {
					continue;
				}
				r_edges++;
				const double parent = nextgeo_projected_error(cluster.parent_lod, cluster.parent_error, params, pixel_scale);
				const double child = nextgeo_projected_error(cluster.group_lod, cluster.group_error, params, pixel_scale);
				if (!(parent >= child)) {
					violations++;
				}
			}
		}
	}
	return violations;
}

inline double depth_bound(uint32_t p_source_triangles) {
	return std::log2(double(p_source_triangles) / 128.0) + 3.0;
}

// Byte-level comparison of two builds.
template <typename T>
bool same_bytes(const LocalVector<T> &p_a, const LocalVector<T> &p_b) {
	return p_a.size() == p_b.size() && (p_a.is_empty() || std::memcmp(p_a.ptr(), p_b.ptr(), sizeof(T) * p_a.size()) == 0);
}

inline bool same_output(const NextGeoBuildOutput &p_a, const NextGeoBuildOutput &p_b) {
	return p_a.vertex_layout == p_b.vertex_layout && p_a.vertex_stride == p_b.vertex_stride && p_a.page_size == p_b.page_size &&
			p_a.root_page_count == p_b.root_page_count && p_a.dag_depth == p_b.dag_depth && p_a.source_triangle_count == p_b.source_triangle_count &&
			p_a.dropped_triangle_count == p_b.dropped_triangle_count && same_bytes(p_a.clusters, p_b.clusters) && same_bytes(p_a.groups, p_b.groups) &&
			same_bytes(p_a.pages, p_b.pages) && same_bytes(p_a.bvh, p_b.bvh) && same_bytes(p_a.vertices, p_b.vertices) &&
			same_bytes(p_a.vertex_sources, p_b.vertex_sources) && same_bytes(p_a.triangles, p_b.triangles) &&
			p_a.skin_influences == p_b.skin_influences && same_bytes(p_a.cluster_skin, p_b.cluster_skin) && same_bytes(p_a.skin_bones, p_b.skin_bones) &&
			same_bytes(p_a.skin_bounds, p_b.skin_bounds) && same_bytes(p_a.skin_indices, p_b.skin_indices) && same_bytes(p_a.skin_weights, p_b.skin_weights);
}

inline uint64_t cut_triangles(const NextGeoBuildOutput &p_dag, const LocalVector<uint32_t> &p_cut) {
	uint64_t triangles = 0;
	for (uint32_t cluster : p_cut) {
		triangles += p_dag.clusters[cluster].triangle_count;
	}
	return triangles;
}

// The view that selects the coarsest cut: the root clusters.
inline NextGeoCutParams far_view(const NextGeoBuildOutput &p_dag) {
	NextGeoCutParams params;
	params.eye = Vector3(p_dag.bvh[0].lod_sphere[0], p_dag.bvh[0].lod_sphere[1], p_dag.bvh[0].lod_sphere[2] + 1.0e7f);
	return params;
}

TEST_CASE("[NextGeo] Record layouts") {
	CHECK(sizeof(NextGeoCluster) == 80);
	CHECK(sizeof(NextGeoBVHNode) == 80);
	const uint32_t full_cluster = uint32_t(sizeof(NextGeoCluster)) + 128 * 12 * 4 + 128 * 3;
	CHECK(nextgeo_cluster_payload_bytes(128, 128, 12) == full_cluster);
	CHECK(nextgeo_page_bytes(full_cluster) == NEXTGEO_PAGE_HEADER_BYTES + full_cluster);
}

TEST_CASE("[NextGeo] Enclosing spheres") {
	RandomPCG rng(TEST_SEED);
	uint32_t failures = 0;
	for (int trial = 0; trial < 1000; trial++) {
		float spheres[9][4];
		const uint32_t count = 1 + rng.rand(9);
		for (uint32_t i = 0; i < count; i++) {
			for (int k = 0; k < 3; k++) {
				spheres[i][k] = float(rng.randd() * 200.0 - 100.0);
			}
			spheres[i][3] = float(rng.randd() * 10.0);
		}
		float merged[4];
		nextgeo_enclose_spheres(&spheres[0][0], count, sizeof(float) * 4, merged);
		for (uint32_t i = 0; i < count; i++) {
			if (!(sphere_gap(merged, spheres[i]) > 0.0)) {
				failures++;
			}
		}
	}
	CHECK_MESSAGE(failures == 0, "The merged sphere strictly encloses every input sphere.");
}

TEST_CASE("[NextGeo] 200k-triangle sphere DAG") {
	const NextGeoBuildInput input = make_sphere_input();
	const NextGeoBuildOutput &dag = get_sphere_dag();

	CHECK(dag.source_triangle_count == 200000);
	CHECK(dag.dropped_triangle_count == 0);
	CHECK(dag.vertex_layout == (NEXTGEO_VERTEX_POSITION | NEXTGEO_VERTEX_NORMAL | NEXTGEO_VERTEX_TANGENT | NEXTGEO_VERTEX_UV0));
	CHECK(dag.vertex_stride == 12);
	check_dag_invariants(input, dag);

	CHECK_MESSAGE(dag.dag_depth > 1, "The sphere simplifies into a multi-level DAG.");
	CHECK_MESSAGE(double(dag.dag_depth) <= depth_bound(dag.source_triangle_count), "DAG depth <= log2(source triangles / 128) + 3.");

	LocalVector<uint32_t> cut;
	nextgeo_select_cut(dag, far_view(dag), nullptr, cut);
	CHECK(nextgeo_check_cut(dag, cut).is_valid_partition());
	CHECK_MESSAGE(cut_triangles(dag, cut) < dag.source_triangle_count / 16, "The root cut is far coarser than the source.");
}

TEST_CASE("[NextGeo] Projected error is monotonic on every DAG edge") {
	const NextGeoBuildOutput &dag = get_sphere_dag();
	uint64_t edges = 0;
	const uint64_t violations = count_monotonicity_violations(dag, 10000, TEST_SEED + 1, edges);
	CHECK(edges > 0);
	CHECK_MESSAGE(violations == 0, "10k random views x instance scales {0.5, 1, 4}: projected(parent) >= projected(child) on 100% of DAG edges.");
}

TEST_CASE("[NextGeo] Random cuts are valid partitions") {
	const NextGeoBuildOutput &dag = get_sphere_dag();
	uint32_t min_cut = 0;
	uint32_t max_cut = 0;
	CHECK_MESSAGE(count_invalid_cuts(dag, 1000, TEST_SEED + 2, min_cut, max_cut) == 0, "1k random cuts: 0 overlapped and 0 uncovered source triangles.");
	CHECK_MESSAGE(min_cut < max_cut, "The random views select different cuts.");

	const NextGeoBuildOutput &kit = get_seam_kit_dag();
	CHECK(count_invalid_cuts(kit, 1000, TEST_SEED + 3, min_cut, max_cut) == 0);
	CHECK(min_cut < max_cut);
}

TEST_CASE("[NextGeo] Random resident page sets give valid partitions") {
	const NextGeoBuildOutput &dag = get_sphere_dag();
	CHECK(dag.pages.size() > dag.root_page_count);
	CHECK_MESSAGE(count_invalid_resident_cuts(dag, 10000, TEST_SEED + 4) == 0, "10k random resident page sets (parents-first closed, roots resident): every cut is a valid partition.");
	CHECK(count_invalid_resident_cuts(get_seam_kit_dag(), 1000, TEST_SEED + 5) == 0);
}

TEST_CASE("[NextGeo] Cut check detects holes and overlaps") {
	const NextGeoBuildOutput &dag = get_sphere_dag();
	LocalVector<uint32_t> cut;

	const NextGeoCutCheck empty = nextgeo_check_cut(dag, cut);
	CHECK(empty.leaf_triangles == dag.source_triangle_count);
	CHECK(empty.uncovered_triangles == dag.source_triangle_count);
	CHECK(empty.overlapped_triangles == 0);

	for (uint32_t i = 0; i < dag.clusters.size(); i++) {
		cut.push_back(i);
	}
	const NextGeoCutCheck everything = nextgeo_check_cut(dag, cut);
	CHECK(everything.uncovered_triangles == 0);
	CHECK(everything.overlapped_triangles == dag.source_triangle_count);

	// Dropping one cluster from a valid cut leaves a hole; adding its refinement twice overlaps.
	nextgeo_select_cut(dag, far_view(dag), nullptr, cut);
	REQUIRE(nextgeo_check_cut(dag, cut).is_valid_partition());
	LocalVector<uint32_t> holed(cut);
	holed.remove_at(0);
	CHECK(nextgeo_check_cut(dag, holed).uncovered_triangles > 0);
	LocalVector<uint32_t> doubled(cut);
	doubled.push_back(cut[0]);
	CHECK(nextgeo_check_cut(dag, doubled).overlapped_triangles > 0);
}

TEST_CASE("[NextGeo] Two threaded builds are byte-identical") {
	const NextGeoBuildInput input = make_sphere_input();
	NextGeoBuildOutput first;
	NextGeoBuildOutput second;
	REQUIRE(nextgeo_build_dag(input, first) == OK);
	REQUIRE(nextgeo_build_dag(input, second) == OK);
	CHECK_MESSAGE(same_output(first, second), "Two threaded builds give byte-identical output.");
	CHECK(same_output(first, get_sphere_dag()));

	NextGeoBuildInput single_input = input;
	single_input.use_threads = false;
	NextGeoBuildOutput single;
	REQUIRE(nextgeo_build_dag(single_input, single) == OK);
	CHECK_MESSAGE(same_output(first, single), "A single-threaded build matches the threaded builds.");

	const NextGeoBuildInput kit_input = make_seam_kit_input(TEST_PAGE_SIZE);
	NextGeoBuildOutput kit;
	REQUIRE(nextgeo_build_dag(kit_input, kit) == OK);
	CHECK(same_output(kit, get_seam_kit_dag()));
}

TEST_CASE("[NextGeo] 8-surface seam kit") {
	const NextGeoBuildInput input = make_seam_kit_input(TEST_PAGE_SIZE);
	const NextGeoBuildOutput &dag = get_seam_kit_dag();

	CHECK(dag.source_triangle_count == 8 * 64 * 64 * 2);
	check_dag_invariants(input, dag);
	CHECK(double(dag.dag_depth) <= depth_bound(dag.source_triangle_count));

	// One DAG over all materials: some groups mix surfaces, while every cluster keeps one.
	uint32_t mixed_groups = 0;
	HashSet<uint32_t> root_surfaces;
	for (const NextGeoGroup &group : dag.groups) {
		const uint16_t first = dag.clusters[group.cluster_offset].surface_id;
		for (uint32_t i = group.cluster_offset; i < group.cluster_offset + group.cluster_count; i++) {
			if (dag.clusters[i].surface_id != first) {
				mixed_groups++;
				break;
			}
		}
	}
	CHECK_MESSAGE(mixed_groups > 0, "Groups partition across material seams.");

	LocalVector<uint32_t> cut;
	nextgeo_select_cut(dag, far_view(dag), nullptr, cut);
	CHECK(nextgeo_check_cut(dag, cut).is_valid_partition());
	for (uint32_t cluster : cut) {
		root_surfaces.insert(dag.clusters[cluster].surface_id);
	}
	CHECK_MESSAGE(root_surfaces.size() == 8, "Every material survives to the root cut.");
	CHECK(cut_triangles(dag, cut) < dag.source_triangle_count / 8);
}

TEST_CASE("[NextGeo] Small pages split groups") {
	// One full cluster's record, vertices and triangles are 6608 bytes. With the 16-byte page
	// header, two fit in a 16 KiB page and three do not.
	const NextGeoBuildInput input = make_seam_kit_input(16 * 1024);
	NextGeoBuildOutput dag;
	REQUIRE(nextgeo_build_dag(input, dag) == OK);
	check_dag_invariants(input, dag);
	CHECK(dag.pages.size() > get_seam_kit_dag().pages.size());
	uint32_t min_cut = 0;
	uint32_t max_cut = 0;
	CHECK(count_invalid_cuts(dag, 200, TEST_SEED + 6, min_cut, max_cut) == 0);
	CHECK(count_invalid_resident_cuts(dag, 200, TEST_SEED + 7) == 0);
}

TEST_CASE("[NextGeo] Plane with an ineligible sibling") {
	const NextGeoBuildInput input = make_sibling_plane_input();
	NextGeoBuildOutput dag;
	REQUIRE(nextgeo_build_dag(input, dag) == OK);
	check_dag_invariants(input, dag);
	CHECK(dag.source_triangle_count == 32 * 64 * 2);
	for (const NextGeoCluster &cluster : dag.clusters) {
		CHECK(cluster.surface_id == 0);
	}

	// Tangent handedness survives simplification: no triangle at any level mixes signs.
	uint32_t mixed_triangles = 0;
	for (const NextGeoCluster &cluster : dag.clusters) {
		for (uint32_t t = 0; t < cluster.triangle_count; t++) {
			float w[3];
			for (uint32_t c = 0; c < 3; c++) {
				const uint32_t v = cluster.vertex_offset + dag.triangles[(cluster.triangle_offset + t) * 3 + c];
				w[c] = dag.vertices[v * dag.vertex_stride + NEXTGEO_VERTEX_OFFSET_TANGENT + 3];
			}
			if (w[0] != w[1] || w[1] != w[2]) {
				mixed_triangles++;
			}
		}
	}
	CHECK_MESSAGE(mixed_triangles == 0, "Tangent handedness survives simplification.");

	// Vertices locked against the ineligible sibling (x = 32) stay in every cut.
	RandomPCG rng(TEST_SEED + 8);
	LocalVector<uint32_t> cut;
	uint32_t cuts_missing_border = 0;
	for (int i = 0; i < 200; i++) {
		const NextGeoCutParams params = i == 0 ? far_view(dag) : random_view(rng, dag, TEST_SCALES[i % 3]);
		nextgeo_select_cut(dag, params, nullptr, cut);
		CHECK(nextgeo_check_cut(dag, cut).is_valid_partition());
		HashSet<int> border_rows;
		for (uint32_t cluster_index : cut) {
			const NextGeoCluster &cluster = dag.clusters[cluster_index];
			for (uint32_t v = cluster.vertex_offset; v < cluster.vertex_offset + cluster.vertex_count; v++) {
				const float *vertex = &dag.vertices[v * dag.vertex_stride];
				if (vertex[0] == 32.0f) {
					border_rows.insert(int(vertex[2]));
				}
			}
		}
		if (border_rows.size() != 65) {
			cuts_missing_border++;
		}
		if (i == 0) {
			CHECK_MESSAGE(cut_triangles(dag, cut) < dag.source_triangle_count / 2, "The flat plane simplifies away from the locked border.");
		}
	}
	CHECK_MESSAGE(cuts_missing_border == 0, "All 65 vertices shared with the ineligible sibling survive in every cut.");
}

TEST_CASE("[NextGeo] Degenerate input") {
	NextGeoBuildInput input;
	NextGeoSurfaceInput surface;
	append_grid_block(surface, 0, 16, 0, 16, 1.0f, 1.0f, HEIGHT_TERRAIN);
	const int grid_triangles = surface.indices.size() / 3;

	// A repeated index, two coincident positions and three collinear points.
	const int duplicate = surface.positions.size();
	surface.positions.push_back(surface.positions[0]);
	surface.normals.push_back(surface.normals[0]);
	for (int k = 0; k < 4; k++) {
		surface.tangents.push_back(surface.tangents[k]);
	}
	surface.uv0.push_back(Vector2(5, 5));
	const int line = surface.positions.size();
	for (int i = 0; i < 3; i++) {
		surface.positions.push_back(Vector3(100.0f + i, 0, 0));
		surface.normals.push_back(Vector3(0, 1, 0));
		surface.tangents.push_back(1.0f);
		surface.tangents.push_back(0.0f);
		surface.tangents.push_back(0.0f);
		surface.tangents.push_back(1.0f);
		surface.uv0.push_back(Vector2());
	}
	const int degenerate[9] = { 0, 0, 1, 0, duplicate, 1, line, line + 1, line + 2 };
	for (int index : degenerate) {
		surface.indices.push_back(index);
	}
	input.surfaces.push_back(surface);
	input.page_size = TEST_PAGE_SIZE;

	NextGeoBuildOutput dag;
	REQUIRE(nextgeo_build_dag(input, dag) == OK);
	CHECK(dag.dropped_triangle_count == 3);
	CHECK(dag.source_triangle_count == uint32_t(grid_triangles));
	check_dag_invariants(input, dag);
	uint32_t min_cut = 0;
	uint32_t max_cut = 0;
	CHECK(count_invalid_cuts(dag, 200, TEST_SEED + 9, min_cut, max_cut) == 0);

	// Nothing but degenerate triangles leaves nothing to build.
	NextGeoBuildInput all_degenerate = input;
	all_degenerate.surfaces.write[0].indices.clear();
	for (int index : degenerate) {
		all_degenerate.surfaces.write[0].indices.push_back(index);
	}
	NextGeoBuildOutput empty;
	ERR_PRINT_OFF;
	CHECK(nextgeo_build_dag(all_degenerate, empty) == ERR_INVALID_DATA);
	ERR_PRINT_ON;
	CHECK(empty.clusters.is_empty());
}

TEST_CASE("[NextGeo] Non-indexed input") {
	NextGeoSurfaceInput indexed;
	append_grid_block(indexed, 0, 32, 0, 32, 1.0f, 1.0f, HEIGHT_TERRAIN);

	// Expand to a non-indexed triangle list; the builder welds identical vertices again.
	NextGeoSurfaceInput expanded;
	for (int i = 0; i < indexed.indices.size(); i++) {
		const int v = indexed.indices[i];
		expanded.positions.push_back(indexed.positions[v]);
		expanded.normals.push_back(indexed.normals[v]);
		for (int k = 0; k < 4; k++) {
			expanded.tangents.push_back(indexed.tangents[v * 4 + k]);
		}
		expanded.uv0.push_back(indexed.uv0[v]);
	}

	NextGeoBuildInput input;
	input.surfaces.push_back(expanded);
	input.page_size = TEST_PAGE_SIZE;
	NextGeoBuildOutput dag;
	REQUIRE(nextgeo_build_dag(input, dag) == OK);
	check_dag_invariants(input, dag);
	CHECK(dag.source_triangle_count == uint32_t(indexed.indices.size() / 3));

	// Welded triangles are connected, so the DAG simplifies like the indexed mesh.
	LocalVector<uint32_t> cut;
	nextgeo_select_cut(dag, far_view(dag), nullptr, cut);
	CHECK(nextgeo_check_cut(dag, cut).is_valid_partition());
	CHECK(dag.dag_depth > 1);
	CHECK(cut_triangles(dag, cut) < dag.source_triangle_count / 2);

	NextGeoBuildInput indexed_input;
	indexed_input.surfaces.push_back(indexed);
	indexed_input.page_size = TEST_PAGE_SIZE;
	NextGeoBuildOutput indexed_dag;
	REQUIRE(nextgeo_build_dag(indexed_input, indexed_dag) == OK);
	CHECK(indexed_dag.source_triangle_count == dag.source_triangle_count);
	CHECK(indexed_dag.dag_depth > 1);
}

TEST_CASE("[NextGeo] Invalid input is rejected") {
	NextGeoBuildInput input;
	NextGeoSurfaceInput surface;
	append_grid_block(surface, 0, 4, 0, 4, 1.0f, 1.0f, HEIGHT_FLAT);
	input.surfaces.push_back(surface);
	NextGeoBuildOutput output;

	ERR_PRINT_OFF;
	SUBCASE("page_size is required") {
		input.page_size = 0;
		CHECK(nextgeo_build_dag(input, output) == ERR_INVALID_PARAMETER);
	}
	SUBCASE("page_size must hold one full cluster") {
		const uint32_t one_cluster_page = nextgeo_page_bytes(nextgeo_cluster_payload_bytes(NEXTGEO_MAX_CLUSTER_VERTICES, NEXTGEO_MAX_CLUSTER_TRIANGLES, 12));
		input.page_size = one_cluster_page - 1;
		CHECK(nextgeo_build_dag(input, output) == ERR_INVALID_PARAMETER);
		input.page_size += 1;
		CHECK(nextgeo_build_dag(input, output) == OK);
	}
	SUBCASE("Out-of-range index") {
		input.page_size = TEST_PAGE_SIZE;
		input.surfaces.write[0].indices.write[0] = 100000;
		CHECK(nextgeo_build_dag(input, output) == ERR_INVALID_DATA);
	}
	SUBCASE("Mismatched attribute arrays") {
		input.page_size = TEST_PAGE_SIZE;
		input.surfaces.write[0].normals.resize(3);
		CHECK(nextgeo_build_dag(input, output) == ERR_INVALID_DATA);
	}
	SUBCASE("Non-finite position") {
		input.page_size = TEST_PAGE_SIZE;
		input.surfaces.write[0].positions.write[0] = Vector3(NAN, 0, 0);
		CHECK(nextgeo_build_dag(input, output) == ERR_INVALID_DATA);
	}
	SUBCASE("No eligible surface") {
		input.page_size = TEST_PAGE_SIZE;
		input.surfaces.write[0].eligible = false;
		CHECK(nextgeo_build_dag(input, output) == ERR_INVALID_DATA);
	}
	ERR_PRINT_ON;
}

// --- Skinned rigs (issue #159) ---
// The Character Walk asset is not in this repository, so two synthetic rigs and one
// character-like grid stand in for it; the asset run is PQ work.

inline void init_skin(NextGeoSurfaceInput &r_surface, uint32_t p_influences) {
	r_surface.skin_influences = p_influences;
	r_surface.bones.resize(r_surface.positions.size() * int(p_influences));
	r_surface.weights.resize(r_surface.positions.size() * int(p_influences));
}

inline void set_skin(NextGeoSurfaceInput &r_surface, int p_vertex, const int32_t *p_bones, const float *p_weights) {
	for (uint32_t k = 0; k < r_surface.skin_influences; k++) {
		r_surface.bones.write[p_vertex * int(r_surface.skin_influences) + int(k)] = p_bones[k];
		r_surface.weights.write[p_vertex * int(r_surface.skin_influences) + int(k)] = p_weights[k];
	}
}

// Rig 1: a chain along x. Bone i owns the strip [i, i + 1) and blends into i + 1.
inline NextGeoBuildInput make_chain_rig_input() {
	NextGeoSurfaceInput surface;
	append_grid_block(surface, 0, 48, 0, 8, 0.25f, 1.0f, HEIGHT_TERRAIN);
	init_skin(surface, 4);
	for (int v = 0; v < surface.positions.size(); v++) {
		const float x = float(surface.positions[v].x);
		const int bone = CLAMP(int(x), 0, 11);
		const float frac = CLAMP(x - float(bone), 0.0f, 1.0f);
		const int32_t bones[4] = { bone, MIN(bone + 1, 11), bone, bone };
		const float weights[4] = { 1.0f - frac, bone < 11 ? frac : 0.0f, 0.0f, 0.0f };
		const float fixed[4] = { bone < 11 ? weights[0] : 1.0f, weights[1], 0.0f, 0.0f };
		set_skin(surface, v, bones, fixed);
	}
	NextGeoBuildInput input;
	input.surfaces.push_back(surface);
	input.page_size = TEST_PAGE_SIZE;
	return input;
}

// Rig 2: a sphere with 8 influences per vertex over 24 bones.
inline NextGeoBuildInput make_sphere_rig_input() {
	NextGeoSurfaceInput surface = make_sphere(32, 20);
	init_skin(surface, 8);
	for (int v = 0; v < surface.positions.size(); v++) {
		const Vector3 position = surface.positions[v];
		const int base = CLAMP(int((float(position.y) + 1.0f) * 6.0f), 0, 11);
		int32_t bones[8];
		float weights[8];
		for (int k = 0; k < 8; k++) {
			bones[k] = (base * 2 + k) % 24;
			weights[k] = float(8 - k) / 36.0f; // Sums to 1.
		}
		set_skin(surface, v, bones, weights);
	}
	NextGeoBuildInput input;
	input.surfaces.push_back(surface);
	input.page_size = TEST_PAGE_SIZE;
	return input;
}

// Rig 3: a character-like sheet. Sixteen bones on a 4 x 4 lattice; every vertex
// blends the four nearest bone centers with bilinear weights.
inline NextGeoBuildInput make_grid_rig_input() {
	NextGeoSurfaceInput surface;
	append_grid_block(surface, 0, 32, 0, 32, 0.25f, 1.0f, HEIGHT_TERRAIN);
	init_skin(surface, 4);
	for (int v = 0; v < surface.positions.size(); v++) {
		const float gx = CLAMP(float(surface.positions[v].x) / 8.0f * 3.0f, 0.0f, 2.999f);
		const float gz = CLAMP(float(surface.positions[v].z) / 8.0f * 3.0f, 0.0f, 2.999f);
		const int ix = int(gx);
		const int iz = int(gz);
		const float fx = gx - float(ix);
		const float fz = gz - float(iz);
		const int32_t bones[4] = { iz * 4 + ix, iz * 4 + ix + 1, (iz + 1) * 4 + ix, (iz + 1) * 4 + ix + 1 };
		const float weights[4] = { (1.0f - fx) * (1.0f - fz), fx * (1.0f - fz), (1.0f - fx) * fz, fx * fz };
		set_skin(surface, v, bones, weights);
	}
	NextGeoBuildInput input;
	input.surfaces.push_back(surface);
	input.page_size = TEST_PAGE_SIZE;
	return input;
}

inline void check_skin_invariants(const NextGeoBuildInput &p_input, const NextGeoBuildOutput &p_dag) {
	REQUIRE((p_dag.skin_influences == 4 || p_dag.skin_influences == 8));
	REQUIRE(p_dag.cluster_skin.size() == p_dag.clusters.size());
	CHECK(p_dag.skin_indices.size() == p_dag.vertex_sources.size() * p_dag.skin_influences);
	CHECK(p_dag.skin_weights.size() == p_dag.vertex_sources.size() * p_dag.skin_influences);
	CHECK(p_dag.skin_bounds.size() == p_dag.skin_bones.size() * 6);

	uint32_t bad_bone_lists = 0;
	uint32_t bad_weights = 0;
	uint32_t bad_bones = 0;
	uint32_t bad_bounds = 0;
	for (uint32_t ci = 0; ci < p_dag.clusters.size(); ci++) {
		const NextGeoCluster &cluster = p_dag.clusters[ci];
		const NextGeoClusterSkin &skin = p_dag.cluster_skin[ci];
		// Split clusters touch at most 16 bones. A lone (coarse) triangle cannot be
		// split further, so it may touch up to 3 * influences: at most 12 with the
		// 4-weight layout, 24 with 8 weights.
		const uint32_t bone_limit = cluster.triangle_count == 1 ? 3 * p_dag.skin_influences : NEXTGEO_MAX_CLUSTER_BONES;
		if (skin.bone_count > bone_limit || skin.bone_offset + skin.bone_count > p_dag.skin_bones.size()) {
			bad_bone_lists++;
			continue;
		}
		for (uint32_t b = 1; b < skin.bone_count; b++) {
			if (p_dag.skin_bones[skin.bone_offset + b] <= p_dag.skin_bones[skin.bone_offset + b - 1]) {
				bad_bone_lists++;
			}
		}
		const NextGeoSurfaceInput &surface = p_input.surfaces[cluster.surface_id];
		for (uint32_t v = 0; v < cluster.vertex_count; v++) {
			const uint32_t out_vertex = cluster.vertex_offset + v;
			const uint32_t source = p_dag.vertex_sources[out_vertex];
			const float *position = &p_dag.vertices[out_vertex * p_dag.vertex_stride];
			for (uint32_t k = 0; k < p_dag.skin_influences; k++) {
				const uint32_t slot = out_vertex * p_dag.skin_influences + k;
				const float weight = float(p_dag.skin_weights[slot]) / 65535.0f;
				// Retained vertices keep their source weights (unorm16 rounding only).
				if (k < surface.skin_influences && std::abs(weight - surface.weights[source * surface.skin_influences + k]) > 1.0f / 65535.0f) {
					bad_weights++;
				}
				if (p_dag.skin_weights[slot] == 0) {
					continue;
				}
				const uint8_t local_bone = p_dag.skin_indices[slot];
				if (local_bone >= skin.bone_count) {
					bad_bones++;
					continue;
				}
				if (k < surface.skin_influences && surface.weights[source * surface.skin_influences + k] > 0.0f &&
						p_dag.skin_bones[skin.bone_offset + local_bone] != uint16_t(surface.bones[source * surface.skin_influences + k])) {
					bad_bones++;
				}
				// The bind-space AABB of a bone contains every vertex weighted on it.
				const float *aabb = &p_dag.skin_bounds[uint32_t(skin.bone_offset + local_bone) * 6];
				for (int axis = 0; axis < 3; axis++) {
					if (position[axis] < aabb[axis] || position[axis] > aabb[3 + axis]) {
						bad_bounds++;
					}
				}
			}
		}
	}
	CHECK_MESSAGE(bad_bone_lists == 0, "Cluster bone lists are sorted, in range and at most 16 long.");
	CHECK_MESSAGE(bad_weights == 0, "Retained vertices keep their source weights (unorm16 rounding only).");
	CHECK_MESSAGE(bad_bones == 0, "Cluster-local bone indices resolve to the source bones.");
	CHECK_MESSAGE(bad_bounds == 0, "Bone AABBs contain their bind-pose vertices.");
}

// Poses every output vertex under random bone transforms and checks it against the
// union of (bone transform x bind-space AABB) of its cluster.
inline uint32_t count_pose_bound_violations(const NextGeoBuildOutput &p_dag, uint32_t p_poses, uint64_t p_seed) {
	uint32_t bone_total = 0;
	for (uint16_t bone : p_dag.skin_bones) {
		bone_total = MAX(bone_total, uint32_t(bone) + 1);
	}
	RandomPCG rng(p_seed);
	LocalVector<Transform3D> transforms;
	transforms.resize(bone_total);
	uint32_t violations = 0;
	constexpr double EPS = 0.01;
	for (uint32_t pose = 0; pose < p_poses; pose++) {
		for (uint32_t b = 0; b < bone_total; b++) {
			Basis basis = Basis::from_euler(Vector3(rng.randd() * Math::TAU, rng.randd() * Math::TAU, rng.randd() * Math::TAU));
			basis.scale(Vector3(0.5 + rng.randd() * 1.5, 0.5 + rng.randd() * 1.5, 0.5 + rng.randd() * 1.5));
			transforms[b] = Transform3D(basis, Vector3(rng.randd() * 4.0 - 2.0, rng.randd() * 4.0 - 2.0, rng.randd() * 4.0 - 2.0));
		}
		for (uint32_t ci = 0; ci < p_dag.clusters.size(); ci++) {
			const NextGeoCluster &cluster = p_dag.clusters[ci];
			const NextGeoClusterSkin &skin = p_dag.cluster_skin[ci];
			if (skin.bone_count == 0) {
				continue;
			}
			Vector3 bound_min(FLT_MAX, FLT_MAX, FLT_MAX);
			Vector3 bound_max(-FLT_MAX, -FLT_MAX, -FLT_MAX);
			for (uint32_t b = 0; b < skin.bone_count; b++) {
				const float *aabb = &p_dag.skin_bounds[uint32_t(skin.bone_offset + b) * 6];
				const Transform3D &xf = transforms[p_dag.skin_bones[skin.bone_offset + b]];
				for (int corner = 0; corner < 8; corner++) {
					const Vector3 posed = xf.xform(Vector3(aabb[(corner & 1) ? 3 : 0], aabb[(corner & 2) ? 4 : 1], aabb[(corner & 4) ? 5 : 2]));
					bound_min = bound_min.min(posed);
					bound_max = bound_max.max(posed);
				}
			}
			for (uint32_t v = 0; v < cluster.vertex_count; v++) {
				const uint32_t out_vertex = cluster.vertex_offset + v;
				const float *vertex = &p_dag.vertices[out_vertex * p_dag.vertex_stride];
				const Vector3 bind(vertex[0], vertex[1], vertex[2]);
				Vector3 posed;
				for (uint32_t k = 0; k < p_dag.skin_influences; k++) {
					const uint32_t slot = out_vertex * p_dag.skin_influences + k;
					const float weight = float(p_dag.skin_weights[slot]) / 65535.0f;
					if (weight == 0.0f) {
						continue;
					}
					const uint16_t bone = p_dag.skin_bones[skin.bone_offset + p_dag.skin_indices[slot]];
					posed += transforms[bone].xform(bind) * weight;
				}
				if (posed.x < bound_min.x - EPS || posed.y < bound_min.y - EPS || posed.z < bound_min.z - EPS ||
						posed.x > bound_max.x + EPS || posed.y > bound_max.y + EPS || posed.z > bound_max.z + EPS) {
					violations++;
				}
			}
		}
	}
	return violations;
}

TEST_CASE("[NextGeo] Skinned DAGs keep the 16-bone limit, source weights and bone bounds") {
	const NextGeoBuildInput inputs[3] = { make_chain_rig_input(), make_sphere_rig_input(), make_grid_rig_input() };
	for (int rig = 0; rig < 3; rig++) {
		NextGeoBuildOutput dag;
		REQUIRE(nextgeo_build_dag(inputs[rig], dag) == OK);
		check_dag_invariants(inputs[rig], dag);
		check_skin_invariants(inputs[rig], dag);
		CHECK_MESSAGE(count_pose_bound_violations(dag, 1000, TEST_SEED + 100 + rig) == 0, "1,000 random poses: every skinned vertex inside its cluster bound.");
	}
}

TEST_CASE("[NextGeo] Skinned clusters split at the bone limit") {
	NextGeoSurfaceInput surface;
	append_grid_block(surface, 0, 32, 0, 32, 1.0f, 1.0f, HEIGHT_TERRAIN);
	init_skin(surface, 4);
	// One distinct bone per vertex: an unsplit 128-vertex cluster would touch 128.
	for (int v = 0; v < surface.positions.size(); v++) {
		const int32_t bones[4] = { v, v, v, v };
		const float weights[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
		set_skin(surface, v, bones, weights);
	}
	NextGeoBuildInput input;
	input.surfaces.push_back(surface);
	input.page_size = TEST_PAGE_SIZE;
	NextGeoBuildOutput dag;
	REQUIRE(nextgeo_build_dag(input, dag) == OK);
	check_dag_invariants(input, dag);
	check_skin_invariants(input, dag);
	uint32_t one_triangles = 0;
	for (const NextGeoCluster &cluster : dag.clusters) {
		if (cluster.refined_group < 0) {
			one_triangles += cluster.triangle_count;
		}
	}
	CHECK(one_triangles == dag.source_triangle_count);
}

TEST_CASE("[NextGeo] Skinned pages fit a one-cluster page_size") {
	NextGeoBuildInput input = make_chain_rig_input();
	input.page_size = nextgeo_page_bytes(nextgeo_cluster_payload_bytes(NEXTGEO_MAX_CLUSTER_VERTICES, NEXTGEO_MAX_CLUSTER_TRIANGLES, 12) +
			nextgeo_cluster_skin_bytes(NEXTGEO_MAX_CLUSTER_BONES, NEXTGEO_MAX_CLUSTER_VERTICES, 4));
	NextGeoBuildOutput dag;
	REQUIRE(nextgeo_build_dag(input, dag) == OK);
	check_dag_invariants(input, dag);
	check_skin_invariants(input, dag);
	CHECK(dag.pages.size() > 1);
}

TEST_CASE("[NextGeo] Two skinned builds are byte-identical") {
	const NextGeoBuildInput input = make_grid_rig_input();
	NextGeoBuildOutput first;
	NextGeoBuildOutput second;
	REQUIRE(nextgeo_build_dag(input, first) == OK);
	REQUIRE(nextgeo_build_dag(input, second) == OK);
	CHECK_MESSAGE(same_output(first, second), "Two threaded skinned builds give byte-identical output, skinning included.");
	NextGeoBuildInput single_input = input;
	single_input.use_threads = false;
	NextGeoBuildOutput single;
	REQUIRE(nextgeo_build_dag(single_input, single) == OK);
	CHECK(same_output(first, single));
}

TEST_CASE("[NextGeo] Invalid skinning input is rejected") {
	NextGeoBuildInput input = make_chain_rig_input();
	NextGeoBuildOutput output;
	ERR_PRINT_OFF;
	SUBCASE("Mismatched bone count") {
		input.surfaces.write[0].bones.resize(3);
		CHECK(nextgeo_build_dag(input, output) == ERR_INVALID_DATA);
	}
	SUBCASE("Negative bone index") {
		input.surfaces.write[0].bones.write[0] = -1;
		CHECK(nextgeo_build_dag(input, output) == ERR_INVALID_DATA);
	}
	SUBCASE("Unsupported influence count") {
		input.surfaces.write[0].skin_influences = 5;
		CHECK(nextgeo_build_dag(input, output) == ERR_INVALID_DATA);
	}
	SUBCASE("Negative weight") {
		input.surfaces.write[0].weights.write[0] = -0.5f;
		CHECK(nextgeo_build_dag(input, output) == ERR_INVALID_DATA);
	}
	SUBCASE("Bones without influences") {
		input.surfaces.write[0].skin_influences = 0;
		CHECK(nextgeo_build_dag(input, output) == ERR_INVALID_DATA);
	}
	ERR_PRINT_ON;
}

} // namespace TestNextGeo
