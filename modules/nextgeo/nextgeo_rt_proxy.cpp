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
	uint16_t group;
	float position[9];
};

struct CardBin {
	bool used = false;
	float min_p[3] = {};
	float max_p[3] = {};

	void include_vertex(const float *p_pos) {
		if (!used) {
			min_p[0] = max_p[0] = p_pos[0];
			min_p[1] = max_p[1] = p_pos[1];
			min_p[2] = max_p[2] = p_pos[2];
			used = true;
			return;
		}
		for (int k = 0; k < 3; k++) {
			if (p_pos[k] < min_p[k]) {
				min_p[k] = p_pos[k];
			}
			if (p_pos[k] > max_p[k]) {
				max_p[k] = p_pos[k];
			}
		}
	}
};

struct AxisTri {
	uint64_t flat_tri_index = 0;
	float u[3] = {};
	float v[3] = {};
	float u_min = 0.0f;
	float u_max = 0.0f;
	float v_min = 0.0f;
	float v_max = 0.0f;
	float n_min = 0.0f;
	float n_max = 0.0f;
	float pos[9] = {};
};

struct AxisTriCompare {
	_FORCE_INLINE_ bool operator()(const AxisTri &p_a, const AxisTri &p_b) const {
		if (p_a.n_min != p_b.n_min) {
			return p_a.n_min < p_b.n_min;
		}
		if (p_a.n_max != p_b.n_max) {
			return p_a.n_max < p_b.n_max;
		}
		return p_a.flat_tri_index < p_b.flat_tri_index;
	}
};

struct DepthLayer {
	CardBin bin;
	float min_n = std::numeric_limits<float>::infinity();
	float max_n = -std::numeric_limits<float>::infinity();
	LocalVector<uint32_t> tri_indices;
};

bool triangles_overlap_uv_interior(const float p_a_u[3], const float p_a_v[3], const float p_b_u[3], const float p_b_v[3], float p_eps) {
	const float *poly_u[2] = { p_a_u, p_b_u };
	const float *poly_v[2] = { p_a_v, p_b_v };
	bool had_valid_edge = false;
	for (int poly = 0; poly < 2; poly++) {
		const float *u = poly_u[poly];
		const float *v = poly_v[poly];
		for (int e = 0; e < 3; e++) {
			const int next = (e + 1) % 3;
			const double du = double(u[next]) - double(u[e]);
			const double dv = double(v[next]) - double(v[e]);
			const double len2 = du * du + dv * dv;
			if (!(len2 > 1e-20)) {
				continue;
			}
			had_valid_edge = true;
			const double inv_len = 1.0 / std::sqrt(len2);
			const double nx = -dv * inv_len;
			const double ny = du * inv_len;
			double min_a = nx * double(p_a_u[0]) + ny * double(p_a_v[0]);
			double max_a = min_a;
			double min_b = nx * double(p_b_u[0]) + ny * double(p_b_v[0]);
			double max_b = min_b;
			for (int k = 1; k < 3; k++) {
				const double pa = nx * double(p_a_u[k]) + ny * double(p_a_v[k]);
				min_a = MIN(min_a, pa);
				max_a = MAX(max_a, pa);
				const double pb = nx * double(p_b_u[k]) + ny * double(p_b_v[k]);
				min_b = MIN(min_b, pb);
				max_b = MAX(max_b, pb);
			}
			if (max_a <= min_b + double(p_eps) || max_b <= min_a + double(p_eps)) {
				return false;
			}
		}
	}
	return had_valid_edge;
}

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

void append_u16_le(PackedByteArray &r_bytes, uint16_t p_value) {
	r_bytes.push_back(uint8_t(p_value));
	r_bytes.push_back(uint8_t(p_value >> 8));
}

void append_index_le(PackedByteArray &r_indices, uint32_t p_index) {
	r_indices.push_back(uint8_t(p_index));
	r_indices.push_back(uint8_t(p_index >> 8));
	r_indices.push_back(uint8_t(p_index >> 16));
	r_indices.push_back(uint8_t(p_index >> 24));
}

void append_f32_le(PackedByteArray &r_bytes, float p_value) {
	uint32_t bits = 0;
	memcpy(&bits, &p_value, sizeof(bits));
	append_index_le(r_bytes, bits);
}

uint16_t read_u16_le(const uint8_t *p_bytes) {
	return uint16_t(p_bytes[0]) | (uint16_t(p_bytes[1]) << 8);
}

float read_f32_le(const uint8_t *p_bytes) {
	const uint32_t bits = uint32_t(p_bytes[0]) | (uint32_t(p_bytes[1]) << 8) | (uint32_t(p_bytes[2]) << 16) | (uint32_t(p_bytes[3]) << 24);
	float value = 0.0f;
	memcpy(&value, &bits, sizeof(value));
	return value;
}

void append_card_descriptor_le(PackedByteArray &r_bytes, const NextGeoCardDescriptor &p_desc) {
	append_u16_le(r_bytes, p_desc.group);
	r_bytes.push_back(p_desc.axis);
	r_bytes.push_back(p_desc.flags);
	append_f32_le(r_bytes, p_desc.center[0]);
	append_f32_le(r_bytes, p_desc.center[1]);
	append_f32_le(r_bytes, p_desc.center[2]);
	append_f32_le(r_bytes, p_desc.half_extents[0]);
	append_f32_le(r_bytes, p_desc.half_extents[1]);
	append_f32_le(r_bytes, p_desc.half_extents[2]);
	append_u16_le(r_bytes, p_desc.res_u);
	append_u16_le(r_bytes, p_desc.res_v);
}

bool dominant_face_axis(const float *p_tri_positions, uint8_t &r_axis) {
	// Godot front-facing triangles wind clockwise, so the outward face normal is (c - a) x (b - a),
	// matching NextGISRC (next_gi_src.cpp) and nextgi_gather.glsl.
	const double ax = double(p_tri_positions[0]);
	const double ay = double(p_tri_positions[1]);
	const double az = double(p_tri_positions[2]);
	const double bx = double(p_tri_positions[3]);
	const double by = double(p_tri_positions[4]);
	const double bz = double(p_tri_positions[5]);
	const double cx = double(p_tri_positions[6]);
	const double cy = double(p_tri_positions[7]);
	const double cz = double(p_tri_positions[8]);
	const double ux = cx - ax;
	const double uy = cy - ay;
	const double uz = cz - az;
	const double vx = bx - ax;
	const double vy = by - ay;
	const double vz = bz - az;
	const double nx = uy * vz - uz * vy;
	const double ny = uz * vx - ux * vz;
	const double nz = ux * vy - uy * vx;
	const double len2 = nx * nx + ny * ny + nz * nz;
	if (!(len2 > 1e-24)) {
		return false;
	}
	const double anx = std::fabs(nx);
	const double any = std::fabs(ny);
	const double anz = std::fabs(nz);
	if (anx >= any && anx >= anz) {
		r_axis = nx >= 0.0 ? 0 : 1;
	} else if (any >= anz) {
		r_axis = ny >= 0.0 ? 2 : 3;
	} else {
		r_axis = nz >= 0.0 ? 4 : 5;
	}
	return true;
}

uint16_t stage2_card_texels(float p_extent, float p_texel_size) {
	const float safe_texel = MAX(p_texel_size, 1e-6f);
	const float texels = std::ceil(p_extent / safe_texel - 0.001f);
	return uint16_t(CLAMP(texels, 4.0f, 128.0f));
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

void nextgeo_decode_card_descriptor(const uint8_t *p_bytes, NextGeoCardDescriptor &r_desc) {
	r_desc.group = read_u16_le(p_bytes + 0);
	r_desc.axis = p_bytes[2];
	r_desc.flags = p_bytes[3];
	r_desc.center[0] = read_f32_le(p_bytes + 4);
	r_desc.center[1] = read_f32_le(p_bytes + 8);
	r_desc.center[2] = read_f32_le(p_bytes + 12);
	r_desc.half_extents[0] = read_f32_le(p_bytes + 16);
	r_desc.half_extents[1] = read_f32_le(p_bytes + 20);
	r_desc.half_extents[2] = read_f32_le(p_bytes + 24);
	r_desc.res_u = read_u16_le(p_bytes + 28);
	r_desc.res_v = read_u16_le(p_bytes + 30);
}

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

	// Stage-2 card group IDs (#77): separate coarse DAG groups into non-occluding
	// normal-depth layers per dominant axis so parallel walls, floors, ceilings,
	// and columns merged at coarse DAG levels never occlude each other in Stage-2
	// orthographic card depth buffers. When every active DAG group stays 1 layer
	// and the DAG fits in uint16_t, preserve direct DAG group indices.
	if (total_triangles > uint64_t(std::numeric_limits<uint32_t>::max())) {
		return ERR_OUT_OF_MEMORY;
	}
	const uint32_t group_count = p_dag.groups.size();
	LocalVector<uint8_t> group_in_cut;
	group_in_cut.resize_initialized(group_count);
	for (uint32_t cluster_idx : selected) {
		group_in_cut[cluster_group[cluster_idx]] = 1;
	}
	LocalVector<uint32_t> dag_to_active_slot;
	dag_to_active_slot.resize(group_count);
	LocalVector<uint32_t> active_dag_groups;
	for (uint32_t g = 0; g < group_count; g++) {
		if (!group_in_cut[g]) {
			dag_to_active_slot[g] = UINT32_MAX;
			continue;
		}
		if (active_dag_groups.size() >= 0x10000u) {
			return ERR_OUT_OF_MEMORY;
		}
		dag_to_active_slot[g] = active_dag_groups.size();
		active_dag_groups.push_back(g);
	}
	const uint32_t active_slot_count = active_dag_groups.size();

	LocalVector<uint32_t> ordered_tri_offset;
	ordered_tri_offset.resize(ordered.size());
	LocalVector<uint16_t> ordered_tri_layer;
	ordered_tri_layer.resize_initialized(uint32_t(total_triangles));
	LocalVector<LocalVector<AxisTri>> slot_axis_tris;
	slot_axis_tris.resize(active_slot_count * 6);

	uint32_t flat_tri_cursor = 0;
	for (uint32_t i = 0; i < ordered.size(); i++) {
		const uint32_t cluster_idx = ordered[i].cluster;
		const NextGeoCluster &cluster = p_dag.clusters[cluster_idx];
		const uint32_t active_slot = dag_to_active_slot[cluster_group[cluster_idx]];
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
		ordered_tri_offset[i] = flat_tri_cursor;
		const float *vertices = p_dag.vertices.ptr() + vertex_begin;
		const uint8_t *cluster_triangles = p_dag.triangles.ptr() + triangle_begin;
		for (uint32_t t = 0; t < cluster.triangle_count; t++) {
			AxisTri at;
			at.flat_tri_index = uint64_t(flat_tri_cursor + t);
			for (uint32_t k = 0; k < 3; k++) {
				const uint8_t local = cluster_triangles[t * 3 + k];
				if (local >= cluster.vertex_count) {
					return ERR_INVALID_DATA;
				}
				const float *position = vertices + uint64_t(local) * stride + NEXTGEO_VERTEX_OFFSET_POSITION;
				at.pos[k * 3 + 0] = position[0];
				at.pos[k * 3 + 1] = position[1];
				at.pos[k * 3 + 2] = position[2];
			}
			uint8_t axis = 0;
			if (dominant_face_axis(at.pos, axis)) {
				const int normal_axis = int(axis / 2);
				const int u_axis = (normal_axis + 1) % 3;
				const int v_axis = (u_axis + 1) % 3;
				const float n_sign = (axis & 1u) != 0u ? -1.0f : 1.0f;
				for (int k = 0; k < 3; k++) {
					const float u_val = at.pos[k * 3 + u_axis];
					const float v_val = at.pos[k * 3 + v_axis];
					const float n_val = n_sign * at.pos[k * 3 + normal_axis];
					at.u[k] = u_val;
					at.v[k] = v_val;
					if (k == 0) {
						at.u_min = at.u_max = u_val;
						at.v_min = at.v_max = v_val;
						at.n_min = at.n_max = n_val;
					} else {
						at.u_min = MIN(at.u_min, u_val);
						at.u_max = MAX(at.u_max, u_val);
						at.v_min = MIN(at.v_min, v_val);
						at.v_max = MAX(at.v_max, v_val);
						at.n_min = MIN(at.n_min, n_val);
						at.n_max = MAX(at.n_max, n_val);
					}
				}
				slot_axis_tris[active_slot * 6 + axis].push_back(at);
			}
		}
		flat_tri_cursor += cluster.triangle_count;
	}

	const float card_texel_size = 2.0f * eps;
	const float gap_tol = 2.0f * card_texel_size;
	const float overlap_depth_tol = 1.5f * card_texel_size;
	const float uv_eps = MAX(1e-4f, 1e-3f * card_texel_size);

	LocalVector<LocalVector<DepthLayer>> slot_axis_layers;
	slot_axis_layers.resize(active_slot_count * 6);
	LocalVector<uint32_t> slot_layer_counts;
	slot_layer_counts.resize(active_slot_count);
	uint32_t extra_group_budget = 0x10000u - active_slot_count;
	bool any_multilayer_slot = false;

	for (uint32_t slot = 0; slot < active_slot_count; slot++) {
		const uint32_t max_allowed_layers = 1u + extra_group_budget;
		uint32_t max_axis_layers = 1u;
		for (uint32_t axis = 0; axis < 6; axis++) {
			LocalVector<AxisTri> &tris = slot_axis_tris[slot * 6 + axis];
			if (tris.is_empty()) {
				continue;
			}
			tris.sort_custom<AxisTriCompare>();
			LocalVector<DepthLayer> &layers = slot_axis_layers[slot * 6 + axis];
			for (uint32_t ti = 0; ti < tris.size(); ti++) {
				const AxisTri &tri = tris[ti];
				uint32_t chosen_layer = UINT32_MAX;
				for (uint32_t l = 0; l < layers.size(); l++) {
					const DepthLayer &layer = layers[l];
					if (tri.n_min > layer.max_n + gap_tol) {
						continue;
					}
					bool conflict = false;
					if (MAX(tri.n_max, layer.max_n) - MIN(tri.n_min, layer.min_n) > overlap_depth_tol) {
						for (uint32_t existing_ti : layer.tri_indices) {
							const AxisTri &e = tris[existing_ti];
							if (MAX(tri.n_max, e.n_max) - MIN(tri.n_min, e.n_min) <= overlap_depth_tol) {
								continue;
							}
							if (!(tri.u_min < e.u_max - uv_eps && tri.u_max > e.u_min + uv_eps && tri.v_min < e.v_max - uv_eps && tri.v_max > e.v_min + uv_eps)) {
								continue;
							}
							if (triangles_overlap_uv_interior(tri.u, tri.v, e.u, e.v, uv_eps)) {
								conflict = true;
								break;
							}
						}
					}
					if (!conflict) {
						chosen_layer = l;
						break;
					}
				}
				if (chosen_layer == UINT32_MAX) {
					if (layers.size() < max_allowed_layers) {
						chosen_layer = layers.size();
						layers.resize(chosen_layer + 1u);
					} else {
						chosen_layer = max_allowed_layers - 1u;
					}
				}
				DepthLayer &dst = layers[chosen_layer];
				dst.bin.include_vertex(&tri.pos[0]);
				dst.bin.include_vertex(&tri.pos[3]);
				dst.bin.include_vertex(&tri.pos[6]);
				dst.min_n = MIN(dst.min_n, tri.n_min);
				dst.max_n = MAX(dst.max_n, tri.n_max);
				dst.tri_indices.push_back(ti);
				ordered_tri_layer[uint32_t(tri.flat_tri_index)] = uint16_t(chosen_layer);
			}
			max_axis_layers = MAX(max_axis_layers, layers.size());
		}
		slot_layer_counts[slot] = max_axis_layers;
		if (max_axis_layers > 1u) {
			any_multilayer_slot = true;
			extra_group_budget -= (max_axis_layers - 1u);
		}
	}

	LocalVector<uint16_t> slot_base_group;
	slot_base_group.resize(active_slot_count);
	if (!any_multilayer_slot && group_count <= 0x10000u) {
		for (uint32_t slot = 0; slot < active_slot_count; slot++) {
			slot_base_group[slot] = uint16_t(active_dag_groups[slot]);
		}
	} else {
		uint32_t next_group_id = 0;
		for (uint32_t slot = 0; slot < active_slot_count; slot++) {
			slot_base_group[slot] = uint16_t(next_group_id);
			next_group_id += slot_layer_counts[slot];
		}
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
			const uint32_t cluster_idx = ordered[i].cluster;
			const NextGeoCluster &cluster = p_dag.clusters[cluster_idx];
			const uint32_t active_slot = dag_to_active_slot[cluster_group[cluster_idx]];
			const uint16_t base_group = slot_base_group[active_slot];
			const uint32_t tri_offset = ordered_tri_offset[i];
			const float *vertices = p_dag.vertices.ptr() + uint64_t(cluster.vertex_offset) * stride;
			const uint8_t *cluster_triangles = p_dag.triangles.ptr() + uint64_t(cluster.triangle_offset) * 3;
			for (uint32_t t = 0; t < cluster.triangle_count; t++) {
				ProxyTriangle triangle;
				triangle.surface = cluster.surface_id;
				triangle.group = uint16_t(base_group + ordered_tri_layer[tri_offset + t]);
				for (uint32_t k = 0; k < 3; k++) {
					const uint8_t local = cluster_triangles[t * 3 + k];
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
			append_u16_le(r_proxy.triangle_groups, triangle.group);
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

	for (uint32_t slot = 0; slot < active_slot_count; slot++) {
		for (uint32_t layer = 0; layer < slot_layer_counts[slot]; layer++) {
			const uint16_t group_id = uint16_t(slot_base_group[slot] + layer);
			for (uint32_t axis = 0; axis < 6; axis++) {
				const LocalVector<DepthLayer> &layers = slot_axis_layers[slot * 6 + axis];
				if (layer >= layers.size() || !layers[layer].bin.used) {
					continue;
				}
				const CardBin &bin = layers[layer].bin;
				NextGeoCardDescriptor desc;
				desc.group = group_id;
				desc.axis = uint8_t(axis);
				desc.flags = 0;
				for (int k = 0; k < 3; k++) {
					desc.center[k] = 0.5f * (bin.min_p[k] + bin.max_p[k]);
					desc.half_extents[k] = 0.5f * (bin.max_p[k] - bin.min_p[k]);
				}
				const int normal_axis = int(axis / 2);
				const int u_axis = (normal_axis + 1) % 3;
				const int v_axis = (u_axis + 1) % 3;
				desc.res_u = stage2_card_texels(2.0f * desc.half_extents[u_axis], card_texel_size);
				desc.res_v = stage2_card_texels(2.0f * desc.half_extents[v_axis], card_texel_size);
				append_card_descriptor_le(r_proxy.card_descriptors, desc);
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
	p_data->set_rt_proxy_triangle_groups(p_proxy.triangle_groups);
	p_data->set_rt_proxy_eps(p_proxy.eps);
	p_data->set_rt_proxy_error(p_proxy.error);
	p_data->set_card_descriptors(p_proxy.card_descriptors);
	return OK;
}
