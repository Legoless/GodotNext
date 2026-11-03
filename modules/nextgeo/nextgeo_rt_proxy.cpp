/**************************************************************************/
/*  nextgeo_rt_proxy.cpp                                                  */
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

#include "nextgeo_rt_proxy.h"

#include "nextgeo_cut.h"

#include "core/error/error_macros.h"
#include "core/templates/hash_map.h"
#include "core/templates/hashfuncs.h"
#include "scene/resources/3d/virtual_geometry_data.h"

#include <cmath>
#include <cstring>
#include <limits>

float nextgeo_rt_proxy_epsilon(const AABB &p_mesh_aabb, float p_rt_max_error) {
	const double by_extent = double(p_mesh_aabb.get_longest_axis_size()) / 256.0;
	return float(MIN(by_extent, double(p_rt_max_error)));
}

namespace {

struct OrderedCluster {
	uint32_t group_rank;
	uint32_t cluster;
};

struct OrderedClusterCompare {
	_FORCE_INLINE_ bool operator()(const OrderedCluster &p_a, const OrderedCluster &p_b) const {
		return p_a.group_rank != p_b.group_rank ? p_a.group_rank < p_b.group_rank : p_a.cluster < p_b.cluster;
	}
};

struct ProxyTriangle {
	uint32_t surface;
	float position[9];
};

// Exact float32 bit equality: +0 and -0 stay distinct weld keys.
struct PositionKey {
	uint32_t bits[3];

	uint32_t hash() const {
		uint32_t h = hash_murmur3_one_32(bits[0]);
		h = hash_murmur3_one_32(bits[1], h);
		return hash_murmur3_one_32(bits[2], h);
	}

	bool operator==(const PositionKey &p_other) const {
		return bits[0] == p_other.bits[0] && bits[1] == p_other.bits[1] && bits[2] == p_other.bits[2];
	}
};

PositionKey position_key(const float *p_position) {
	PositionKey key;
	memcpy(key.bits, p_position, sizeof(key.bits));
	return key;
}

void append_index_le(PackedByteArray &r_indices, uint32_t p_index) {
	r_indices.push_back(uint8_t(p_index));
	r_indices.push_back(uint8_t(p_index >> 8));
	r_indices.push_back(uint8_t(p_index >> 16));
	r_indices.push_back(uint8_t(p_index >> 24));
}

// Rank of each group in BVH8 leaf order: a depth-first walk of the BVH in
// child-slot order (the groups were Morton-sorted before the BVH was built).
// Groups the walk never reaches keep index order after the visited ones, so
// the ranking is total and deterministic even on a malformed BVH.
void rank_groups_by_bvh(const NextGeoBuildOutput &p_dag, LocalVector<uint32_t> &r_group_rank) {
	const uint32_t group_count = p_dag.groups.size();
	r_group_rank.clear();
	r_group_rank.resize(group_count);
	for (uint32_t g = 0; g < group_count; g++) {
		r_group_rank[g] = UINT32_MAX;
	}

	uint32_t next_rank = 0;
	if (!p_dag.bvh.is_empty()) {
		// Depth-first in child-slot order: stack entries are raw child values
		// (leaf bit set = group leaf, clear = node index), pushed in reverse
		// so they pop in slot order. A node may mix both kinds.
		LocalVector<uint8_t> node_seen;
		node_seen.resize_initialized(p_dag.bvh.size());
		LocalVector<uint32_t> stack;
		stack.push_back(0);
		while (!stack.is_empty()) {
			const uint32_t entry = stack[stack.size() - 1];
			stack.remove_at(stack.size() - 1);
			if (entry == NEXTGEO_BVH_INVALID_CHILD) {
				continue;
			}
			if ((entry & NEXTGEO_BVH_LEAF_BIT) != 0) {
				const uint32_t group = entry & ~NEXTGEO_BVH_LEAF_BIT;
				if (group < group_count && r_group_rank[group] == UINT32_MAX) {
					r_group_rank[group] = next_rank++;
				}
				continue;
			}
			if (entry >= p_dag.bvh.size() || node_seen[entry]) {
				continue;
			}
			node_seen[entry] = 1;
			const NextGeoBVHNode &node = p_dag.bvh[entry];
			uint32_t children[NEXTGEO_BVH_WIDTH];
			const uint32_t child_count = MIN(node.child_count, NEXTGEO_BVH_WIDTH);
			for (uint32_t k = 0; k < child_count; k++) {
				children[k] = node.children[k];
			}
			for (uint32_t k = child_count; k > 0; k--) {
				stack.push_back(children[k - 1]);
			}
		}
	}
	for (uint32_t g = 0; g < group_count; g++) {
		if (r_group_rank[g] == UINT32_MAX) {
			r_group_rank[g] = next_rank++;
		}
	}
}

} // namespace

Error nextgeo_build_rt_proxy(const NextGeoBuildOutput &p_dag, const AABB &p_mesh_aabb, float p_rt_max_error, uint32_t p_surface_count, NextGeoRTProxy &r_proxy) {
	r_proxy = NextGeoRTProxy();
	if (!(p_rt_max_error > 0.0f) || !std::isfinite(p_rt_max_error)) {
		return ERR_INVALID_PARAMETER;
	}
	if (!p_mesh_aabb.is_finite() || p_mesh_aabb.size.x < 0.0 || p_mesh_aabb.size.y < 0.0 || p_mesh_aabb.size.z < 0.0) {
		return ERR_INVALID_PARAMETER;
	}

	const float eps = nextgeo_rt_proxy_epsilon(p_mesh_aabb, p_rt_max_error);
	r_proxy.eps = eps;
	r_proxy.surface_count = p_surface_count;

	const uint32_t cluster_count = p_dag.clusters.size();
	if (cluster_count == 0) {
		return OK;
	}
	if ((p_dag.vertex_layout & NEXTGEO_VERTEX_POSITION) == 0 || p_dag.vertex_stride < 3) {
		return ERR_INVALID_DATA;
	}
	const uint32_t stride = p_dag.vertex_stride;

	// Cluster -> group, from the groups' contiguous cluster ranges.
	LocalVector<uint32_t> cluster_group;
	cluster_group.resize(cluster_count);
	for (uint32_t i = 0; i < cluster_count; i++) {
		cluster_group[i] = UINT32_MAX;
	}
	for (uint32_t g = 0; g < p_dag.groups.size(); g++) {
		const NextGeoGroup &group = p_dag.groups[g];
		if (uint64_t(group.cluster_offset) + uint64_t(group.cluster_count) > uint64_t(cluster_count)) {
			return ERR_INVALID_DATA;
		}
		for (uint32_t i = group.cluster_offset; i < group.cluster_offset + group.cluster_count; i++) {
			if (cluster_group[i] != UINT32_MAX) {
				return ERR_INVALID_DATA;
			}
			cluster_group[i] = g;
		}
	}
	for (uint32_t i = 0; i < cluster_count; i++) {
		if (cluster_group[i] == UINT32_MAX) {
			return ERR_INVALID_DATA;
		}
	}

	// The error-only cut at eps_rt. Group-uniform bounds make it a partition
	// of the source surface; nextgeo_check_cut asserts that below.
	LocalVector<uint32_t> selected;
	float max_error = 0.0f;
	uint64_t total_triangles = 0;
	for (uint32_t i = 0; i < cluster_count; i++) {
		const NextGeoCluster &cluster = p_dag.clusters[i];
		if (cluster.surface_id >= p_surface_count) {
			return ERR_INVALID_DATA;
		}
		// A cluster holds at most 128 triangles (NEXTGEO_MAX_CLUSTER_TRIANGLES,
		// and the field is a uint8_t), so no cluster can overflow a part alone.
		if (cluster.group_error <= eps && cluster.parent_error > eps) {
			selected.push_back(i);
			max_error = MAX(max_error, cluster.group_error);
			total_triangles += cluster.triangle_count;
		}
	}
	if (!nextgeo_check_cut(p_dag, selected).is_valid_partition()) {
		return ERR_INVALID_DATA;
	}
	r_proxy.error = max_error;
	r_proxy.triangle_count = total_triangles;
	if (selected.is_empty()) {
		return OK;
	}

	// Pack whole clusters in BVH8 leaf order (then cluster index), greedily,
	// into parts of at most the cap. No cluster is split and the cut is never
	// coarsened: a part closes before the cluster that would overflow it.
	LocalVector<uint32_t> group_rank;
	rank_groups_by_bvh(p_dag, group_rank);
	LocalVector<OrderedCluster> ordered;
	ordered.resize(selected.size());
	for (uint32_t i = 0; i < selected.size(); i++) {
		ordered[i].group_rank = group_rank[cluster_group[selected[i]]];
		ordered[i].cluster = selected[i];
	}
	ordered.sort_custom<OrderedClusterCompare>();

	struct PartSpan {
		uint32_t first;
		uint32_t count;
		uint32_t triangles;
	};
	LocalVector<PartSpan> part_spans;
	{
		uint32_t part_first = 0;
		uint32_t part_triangles = 0;
		for (uint32_t i = 0; i < ordered.size(); i++) {
			const uint32_t triangles = p_dag.clusters[ordered[i].cluster].triangle_count;
			if (part_triangles > 0 && part_triangles + triangles > NEXTGEO_RT_PROXY_MAX_PART_TRIANGLES) {
				part_spans.push_back(PartSpan{ part_first, i - part_first, part_triangles });
				part_first = i;
				part_triangles = 0;
			}
			part_triangles += triangles;
		}
		part_spans.push_back(PartSpan{ part_first, ordered.size() - part_first, part_triangles });
	}

	for (const OrderedCluster &entry : ordered) {
		r_proxy.clusters.push_back(int32_t(entry.cluster));
	}

	// Per part: gather triangles, stable-sort them by surface (counting sort,
	// so BLAS geometry index = surface), weld positions in first-occurrence
	// order of that final triangle order, and emit part-local indices.
	LocalVector<uint32_t> surface_triangle_count;
	LocalVector<uint32_t> surface_cursor;
	for (const PartSpan &span : part_spans) {
		LocalVector<ProxyTriangle> triangles;
		triangles.reserve(span.triangles);
		for (uint32_t i = span.first; i < span.first + span.count; i++) {
			const NextGeoCluster &cluster = p_dag.clusters[ordered[i].cluster];
			const uint64_t vertex_begin = uint64_t(cluster.vertex_offset) * stride;
			const uint64_t vertex_floats = uint64_t(cluster.vertex_count) * stride;
			if (vertex_begin > p_dag.vertices.size() || vertex_floats > p_dag.vertices.size() - vertex_begin) {
				return ERR_INVALID_DATA;
			}
			const uint64_t triangle_begin = uint64_t(cluster.triangle_offset) * 3;
			const uint64_t triangle_bytes = uint64_t(cluster.triangle_count) * 3;
			if (triangle_begin > p_dag.triangles.size() || triangle_bytes > p_dag.triangles.size() - triangle_begin) {
				return ERR_INVALID_DATA;
			}
			const float *vertices = p_dag.vertices.ptr() + vertex_begin;
			const uint8_t *cluster_triangles = p_dag.triangles.ptr() + triangle_begin;
			for (uint32_t t = 0; t < cluster.triangle_count; t++) {
				ProxyTriangle triangle;
				triangle.surface = cluster.surface_id;
				for (uint32_t k = 0; k < 3; k++) {
					const uint8_t local = cluster_triangles[t * 3 + k];
					if (local >= cluster.vertex_count) {
						return ERR_INVALID_DATA;
					}
					const float *position = vertices + uint64_t(local) * stride + NEXTGEO_VERTEX_OFFSET_POSITION;
					triangle.position[k * 3 + 0] = position[0];
					triangle.position[k * 3 + 1] = position[1];
					triangle.position[k * 3 + 2] = position[2];
				}
				triangles.push_back(triangle);
			}
		}

		surface_triangle_count.clear();
		surface_triangle_count.resize_initialized(p_surface_count);
		for (const ProxyTriangle &triangle : triangles) {
			surface_triangle_count[triangle.surface]++;
		}
		surface_cursor.clear();
		surface_cursor.resize(p_surface_count);
		uint32_t surface_first_triangle = 0;
		for (uint32_t s = 0; s < p_surface_count; s++) {
			surface_cursor[s] = surface_first_triangle;
			surface_first_triangle += surface_triangle_count[s];
		}
		LocalVector<ProxyTriangle> sorted_triangles;
		sorted_triangles.resize(triangles.size());
		for (const ProxyTriangle &triangle : triangles) {
			sorted_triangles[surface_cursor[triangle.surface]++] = triangle;
		}

		const uint64_t part_vertex_offset = uint64_t(r_proxy.positions.size() / 3);
		const uint64_t part_index_offset = uint64_t(r_proxy.indices.size() / 4);
		HashMap<PositionKey, uint32_t> welded;
		uint32_t part_vertex_count = 0;
		for (const ProxyTriangle &triangle : sorted_triangles) {
			for (uint32_t k = 0; k < 3; k++) {
				const float *position = &triangle.position[k * 3];
				const PositionKey key = position_key(position);
				const HashMap<PositionKey, uint32_t>::Iterator found = welded.find(key);
				uint32_t index = 0;
				if (found != welded.end()) {
					index = found->value;
				} else {
					index = part_vertex_count++;
					welded[key] = index;
					r_proxy.positions.push_back(position[0]);
					r_proxy.positions.push_back(position[1]);
					r_proxy.positions.push_back(position[2]);
				}
				append_index_le(r_proxy.indices, index);
			}
		}

		const uint64_t part_index_count = uint64_t(triangles.size()) * 3;
		if (part_vertex_offset > uint64_t(std::numeric_limits<int32_t>::max()) ||
				part_index_offset > uint64_t(std::numeric_limits<int32_t>::max()) ||
				part_index_count > uint64_t(std::numeric_limits<int32_t>::max())) {
			return ERR_OUT_OF_MEMORY;
		}
		r_proxy.parts.push_back(int32_t(part_vertex_offset));
		r_proxy.parts.push_back(int32_t(part_vertex_count));
		r_proxy.parts.push_back(int32_t(part_index_offset));
		r_proxy.parts.push_back(int32_t(part_index_count));

		uint32_t first_index = 0;
		for (uint32_t s = 0; s < p_surface_count; s++) {
			const uint32_t index_count = surface_triangle_count[s] * 3;
			if (index_count == 0) {
				r_proxy.surface_ranges.push_back(0);
				r_proxy.surface_ranges.push_back(0);
			} else {
				r_proxy.surface_ranges.push_back(int32_t(first_index));
				r_proxy.surface_ranges.push_back(int32_t(index_count));
				first_index += index_count;
			}
		}
	}

	return OK;
}

Error nextgeo_fill_vgd_rt_proxy(const NextGeoRTProxy &p_proxy, const Ref<VirtualGeometryData> &p_data) {
	ERR_FAIL_COND_V(p_data.is_null(), ERR_INVALID_PARAMETER);
	p_data->set_rt_proxy_positions(p_proxy.positions);
	p_data->set_rt_proxy_indices(p_proxy.indices);
	p_data->set_rt_proxy_parts(p_proxy.parts);
	p_data->set_rt_proxy_surface_ranges(p_proxy.surface_ranges);
	p_data->set_rt_proxy_clusters(p_proxy.clusters);
	p_data->set_rt_proxy_eps(p_proxy.eps);
	p_data->set_rt_proxy_error(p_proxy.error);
	return OK;
}
