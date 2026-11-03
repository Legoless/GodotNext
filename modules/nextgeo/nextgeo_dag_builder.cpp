/**************************************************************************/
/*  nextgeo_dag_builder.cpp                                               */
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

// The DAG construction follows the structure of `demo/clusterlod.h` from
// zeux/meshoptimizer (MIT, Copyright (c) 2016-2026 Arseny Kapoulkine), as recorded in
// docs/godotnext/source-log/nextgeo.md: clusterize, partition clusters into groups,
// lock group borders, simplify each group, merge LOD bounds from the members, and
// re-clusterize. GodotNext adds single-material clusters, sibling locks, page-sized
// groups, deterministic threaded levels and the page assignment.

#include "nextgeo_dag_builder.h"

#include "core/math/math_funcs.h"
#include "core/object/worker_thread_pool.h"
#include "core/templates/hash_set.h"
#include "core/templates/span.h"

#include <thirdparty/meshoptimizer/meshoptimizer.h>

#include <cfloat>
#include <cmath>
#include <utility>
namespace {

// Group simplification (plan §7.1, step 4.3).
constexpr float SIMPLIFY_RATIO = 0.5f;
// A group that keeps more than this share of its indices is terminal.
constexpr uint32_t SIMPLIFY_TERMINAL_PERCENT = 85;
// Weight of the normal cone in meshopt_buildMeshlets(); clusters carry a cone for culling.
constexpr float CLUSTER_CONE_WEIGHT = 0.25f;
// Safety net; a real DAG stops long before this.
constexpr uint32_t MAX_LEVELS = 64;
// Attribute weights for the simplifier metric, as in ImporterMesh::generate_lods():
// normals always, color when present. UVs, tangents and surfaces are kept by seams.
constexpr float NORMAL_WEIGHT = 1.0f;
constexpr float COLOR_WEIGHT = 1.0f;
constexpr uint32_t MAX_ATTRIBUTES = 7;

struct MergedSurface {
	uint16_t surface_id = 0;
	LocalVector<uint32_t> indices; // Into the merged stream.
};

struct MergedMesh {
	uint32_t vertex_count = 0;
	uint32_t layout = 0;
	uint32_t stride = 0; // Floats per vertex in `vertex_data`.
	uint32_t uv2_offset = 0;
	uint32_t color_offset = 0;
	uint32_t attribute_count = 0;
	float attribute_weights[MAX_ATTRIBUTES] = {};

	LocalVector<float> positions; // 3 floats per vertex.
	LocalVector<float> attributes; // attribute_count floats per vertex.
	LocalVector<float> vertex_data; // stride floats per vertex.
	LocalVector<uint32_t> vertex_source; // Vertex index in the source surface.
	LocalVector<uint16_t> vertex_surface;
	LocalVector<uint8_t> vertex_lock; // Permanent locks (shared with ineligible siblings).
	LocalVector<uint32_t> position_remap; // First vertex with the same position.
	LocalVector<MergedSurface> surfaces;
};

struct BuildCluster {
	LocalVector<uint32_t> indices; // Merged vertex indices, 3 per triangle.
	uint32_t vertex_count = 0;
	uint16_t surface_id = 0;
	float bounds[4] = {};
	int8_t cone_axis[3] = {};
	int8_t cone_cutoff = 0;
	float own_lod[4] = {};
	float own_error = 0.0f;
	float parent_lod[4] = {};
	float parent_error = FLT_MAX;
	int32_t refined = -1;
	int32_t group = -1;
};

struct BuildGroup {
	LocalVector<uint32_t> members;
	uint32_t depth = 0;
	float lod[4] = {};
	float error = FLT_MAX;
	bool terminal = true;
};

struct GroupResult {
	float lod[4] = {};
	float error = FLT_MAX;
	bool terminal = true;
	Error error_code = OK;
	LocalVector<BuildCluster> clusters;
};

template <typename F>
void _run_tasks(uint32_t p_count, bool p_threaded, F &p_function, const char *p_description) {
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	if (!p_threaded || p_count < 2 || pool == nullptr) {
		for (uint32_t i = 0; i < p_count; i++) {
			p_function(i);
		}
		return;
	}

	struct Call {
		static void run(void *p_userdata, uint32_t p_index) {
			(*static_cast<F *>(p_userdata))(p_index);
		}
	};

	const WorkerThreadPool::GroupID id = pool->add_native_group_task(&Call::run, &p_function, int(p_count), -1, true, String(p_description));
	pool->wait_for_group_task_completion(id);
}

void _sort_unique(LocalVector<uint32_t> &r_values) {
	r_values.sort();
	uint32_t write = 0;
	for (uint32_t i = 0; i < r_values.size(); i++) {
		if (write == 0 || r_values[write - 1] != r_values[i]) {
			r_values[write++] = r_values[i];
		}
	}
	r_values.resize(write);
}

void _write_sphere(const meshopt_Bounds &p_bounds, float r_sphere[4]) {
	r_sphere[0] = p_bounds.center[0];
	r_sphere[1] = p_bounds.center[1];
	r_sphere[2] = p_bounds.center[2];
	r_sphere[3] = p_bounds.radius;
}

// Splits a single-surface triangle list into clusters of at most 128 vertices and 128
// triangles (plan §7.1, step 4.2). Vertices are compacted first so that the meshlet
// builder's cost does not scale with the size of the whole merged stream.
void _clusterize(const MergedMesh &p_mesh, const uint32_t *p_indices, uint32_t p_index_count, uint16_t p_surface_id, LocalVector<BuildCluster> &r_clusters) {
	if (p_index_count == 0) {
		return;
	}

	LocalVector<uint32_t> unique;
	unique.resize(p_index_count);
	for (uint32_t i = 0; i < p_index_count; i++) {
		unique[i] = p_indices[i];
	}
	_sort_unique(unique);

	const Span<uint32_t> unique_span = unique.span();
	LocalVector<uint32_t> local_indices;
	local_indices.resize(p_index_count);
	for (uint32_t i = 0; i < p_index_count; i++) {
		local_indices[i] = uint32_t(unique_span.bisect(p_indices[i], true));
	}

	LocalVector<float> local_positions;
	local_positions.resize(unique.size() * 3);
	for (uint32_t i = 0; i < unique.size(); i++) {
		for (uint32_t k = 0; k < 3; k++) {
			local_positions[i * 3 + k] = p_mesh.positions[unique[i] * 3 + k];
		}
	}

	const size_t max_meshlets = meshopt_buildMeshletsBound(p_index_count, NEXTGEO_MAX_CLUSTER_VERTICES, NEXTGEO_MAX_CLUSTER_TRIANGLES);
	LocalVector<meshopt_Meshlet> meshlets;
	meshlets.resize(uint32_t(max_meshlets));
	LocalVector<uint32_t> meshlet_vertices;
	meshlet_vertices.resize(p_index_count);
	LocalVector<uint8_t> meshlet_triangles;
	meshlet_triangles.resize(p_index_count);

	const size_t meshlet_count = meshopt_buildMeshlets(meshlets.ptr(), meshlet_vertices.ptr(), meshlet_triangles.ptr(),
			local_indices.ptr(), p_index_count, local_positions.ptr(), unique.size(), sizeof(float) * 3,
			NEXTGEO_MAX_CLUSTER_VERTICES, NEXTGEO_MAX_CLUSTER_TRIANGLES, CLUSTER_CONE_WEIGHT);

	for (size_t m = 0; m < meshlet_count; m++) {
		const meshopt_Meshlet &meshlet = meshlets[uint32_t(m)];
		meshopt_optimizeMeshlet(&meshlet_vertices[meshlet.vertex_offset], &meshlet_triangles[meshlet.triangle_offset], meshlet.triangle_count, meshlet.vertex_count);

		BuildCluster cluster;
		cluster.surface_id = p_surface_id;
		cluster.vertex_count = meshlet.vertex_count;
		cluster.indices.resize(meshlet.triangle_count * 3);
		for (uint32_t j = 0; j < meshlet.triangle_count * 3; j++) {
			cluster.indices[j] = unique[meshlet_vertices[meshlet.vertex_offset + meshlet_triangles[meshlet.triangle_offset + j]]];
		}

		const meshopt_Bounds bounds = meshopt_computeClusterBounds(cluster.indices.ptr(), cluster.indices.size(), p_mesh.positions.ptr(), p_mesh.vertex_count, sizeof(float) * 3);
		_write_sphere(bounds, cluster.bounds);
		for (uint32_t k = 0; k < 3; k++) {
			cluster.cone_axis[k] = bounds.cone_axis_s8[k];
		}
		cluster.cone_cutoff = bounds.cone_cutoff_s8;

		r_clusters.push_back(std::move(cluster));
	}
}

bool _is_finite(const Vector3 &p_value) {
	return std::isfinite(double(p_value.x)) && std::isfinite(double(p_value.y)) && std::isfinite(double(p_value.z));
}

bool _is_degenerate(const float *p_a, const float *p_b, const float *p_c) {
	const double ab[3] = { double(p_b[0]) - p_a[0], double(p_b[1]) - p_a[1], double(p_b[2]) - p_a[2] };
	const double ac[3] = { double(p_c[0]) - p_a[0], double(p_c[1]) - p_a[1], double(p_c[2]) - p_a[2] };
	const double cx = ab[1] * ac[2] - ab[2] * ac[1];
	const double cy = ab[2] * ac[0] - ab[0] * ac[2];
	const double cz = ab[0] * ac[1] - ab[1] * ac[0];
	return cx * cx + cy * cy + cz * cz == 0.0;
}

// Writes one vertex of a surface in the merged layout.
void _write_vertex(const NextGeoSurfaceInput &p_surface, uint32_t p_index, const MergedMesh &p_mesh, float *r_vertex) {
	const Vector3 position = p_surface.positions[p_index];
	r_vertex[NEXTGEO_VERTEX_OFFSET_POSITION + 0] = float(position.x);
	r_vertex[NEXTGEO_VERTEX_OFFSET_POSITION + 1] = float(position.y);
	r_vertex[NEXTGEO_VERTEX_OFFSET_POSITION + 2] = float(position.z);

	const Vector3 normal = p_surface.normals.is_empty() ? Vector3() : p_surface.normals[p_index];
	r_vertex[NEXTGEO_VERTEX_OFFSET_NORMAL + 0] = float(normal.x);
	r_vertex[NEXTGEO_VERTEX_OFFSET_NORMAL + 1] = float(normal.y);
	r_vertex[NEXTGEO_VERTEX_OFFSET_NORMAL + 2] = float(normal.z);

	if (p_surface.tangents.is_empty()) {
		r_vertex[NEXTGEO_VERTEX_OFFSET_TANGENT + 0] = 1.0f;
		r_vertex[NEXTGEO_VERTEX_OFFSET_TANGENT + 1] = 0.0f;
		r_vertex[NEXTGEO_VERTEX_OFFSET_TANGENT + 2] = 0.0f;
		r_vertex[NEXTGEO_VERTEX_OFFSET_TANGENT + 3] = 1.0f;
	} else {
		for (uint32_t k = 0; k < 4; k++) {
			r_vertex[NEXTGEO_VERTEX_OFFSET_TANGENT + k] = p_surface.tangents[p_index * 4 + k];
		}
	}

	const Vector2 uv0 = p_surface.uv0.is_empty() ? Vector2() : p_surface.uv0[p_index];
	r_vertex[NEXTGEO_VERTEX_OFFSET_UV0 + 0] = float(uv0.x);
	r_vertex[NEXTGEO_VERTEX_OFFSET_UV0 + 1] = float(uv0.y);

	if (p_mesh.layout & NEXTGEO_VERTEX_UV2) {
		const Vector2 uv2 = p_surface.uv2.is_empty() ? Vector2() : p_surface.uv2[p_index];
		r_vertex[p_mesh.uv2_offset + 0] = float(uv2.x);
		r_vertex[p_mesh.uv2_offset + 1] = float(uv2.y);
	}
	if (p_mesh.layout & NEXTGEO_VERTEX_COLOR) {
		const Color color = p_surface.colors.is_empty() ? Color(1, 1, 1, 1) : p_surface.colors[p_index];
		r_vertex[p_mesh.color_offset + 0] = color.r;
		r_vertex[p_mesh.color_offset + 1] = color.g;
		r_vertex[p_mesh.color_offset + 2] = color.b;
		r_vertex[p_mesh.color_offset + 3] = color.a;
	}
}

// Merges the eligible surfaces into one vertex stream (plan §7.1, step 4.1). Each surface
// keeps its own vertices, so every material border is an attribute seam.
Error _merge_surfaces(const NextGeoBuildInput &p_input, MergedMesh &r_mesh, uint32_t &r_dropped) {
	ERR_FAIL_COND_V_MSG(p_input.surfaces.size() > int(UINT16_MAX) + 1, ERR_INVALID_PARAMETER, "NextGeo: too many surfaces.");

	r_mesh.layout = NEXTGEO_VERTEX_POSITION | NEXTGEO_VERTEX_NORMAL | NEXTGEO_VERTEX_TANGENT | NEXTGEO_VERTEX_UV0;
	for (int s = 0; s < p_input.surfaces.size(); s++) {
		const NextGeoSurfaceInput &surface = p_input.surfaces[s];
		const int count = surface.positions.size();
		ERR_FAIL_COND_V_MSG(!surface.normals.is_empty() && surface.normals.size() != count, ERR_INVALID_DATA, vformat("NextGeo: surface %d has a normal count that does not match its vertices.", s));
		ERR_FAIL_COND_V_MSG(!surface.tangents.is_empty() && surface.tangents.size() != count * 4, ERR_INVALID_DATA, vformat("NextGeo: surface %d has a tangent count that does not match its vertices.", s));
		ERR_FAIL_COND_V_MSG(!surface.uv0.is_empty() && surface.uv0.size() != count, ERR_INVALID_DATA, vformat("NextGeo: surface %d has a UV count that does not match its vertices.", s));
		ERR_FAIL_COND_V_MSG(!surface.uv2.is_empty() && surface.uv2.size() != count, ERR_INVALID_DATA, vformat("NextGeo: surface %d has a UV2 count that does not match its vertices.", s));
		ERR_FAIL_COND_V_MSG(!surface.colors.is_empty() && surface.colors.size() != count, ERR_INVALID_DATA, vformat("NextGeo: surface %d has a color count that does not match its vertices.", s));
		ERR_FAIL_COND_V_MSG(surface.indices.size() % 3 != 0, ERR_INVALID_DATA, vformat("NextGeo: surface %d has an index count that is not a multiple of 3.", s));
		ERR_FAIL_COND_V_MSG(surface.indices.is_empty() && count % 3 != 0, ERR_INVALID_DATA, vformat("NextGeo: non-indexed surface %d has a vertex count that is not a multiple of 3.", s));
		for (int i = 0; i < count; i++) {
			ERR_FAIL_COND_V_MSG(!_is_finite(surface.positions[i]), ERR_INVALID_DATA, vformat("NextGeo: surface %d has a non-finite position.", s));
		}
		for (int i = 0; i < surface.indices.size(); i++) {
			ERR_FAIL_INDEX_V_MSG(surface.indices[i], count, ERR_INVALID_DATA, vformat("NextGeo: surface %d has an out-of-range index.", s));
		}
		if (surface.eligible) {
			if (!surface.uv2.is_empty()) {
				r_mesh.layout |= NEXTGEO_VERTEX_UV2;
			}
			if (!surface.colors.is_empty()) {
				r_mesh.layout |= NEXTGEO_VERTEX_COLOR;
			}
		}
	}

	r_mesh.stride = NEXTGEO_VERTEX_OFFSET_UV0 + 2;
	if (r_mesh.layout & NEXTGEO_VERTEX_UV2) {
		r_mesh.uv2_offset = r_mesh.stride;
		r_mesh.stride += 2;
	}
	if (r_mesh.layout & NEXTGEO_VERTEX_COLOR) {
		r_mesh.color_offset = r_mesh.stride;
		r_mesh.stride += 4;
	}

	r_mesh.attribute_count = 3;
	r_mesh.attribute_weights[0] = r_mesh.attribute_weights[1] = r_mesh.attribute_weights[2] = NORMAL_WEIGHT;
	if (r_mesh.layout & NEXTGEO_VERTEX_COLOR) {
		for (uint32_t k = 0; k < 4; k++) {
			r_mesh.attribute_weights[r_mesh.attribute_count++] = COLOR_WEIGHT;
		}
	}

	HashSet<Vector3> ineligible_positions;
	for (int s = 0; s < p_input.surfaces.size(); s++) {
		const NextGeoSurfaceInput &surface = p_input.surfaces[s];
		if (surface.eligible) {
			continue;
		}
		for (int i = 0; i < surface.positions.size(); i++) {
			const Vector3 position = surface.positions[i];
			ineligible_positions.insert(Vector3(float(position.x), float(position.y), float(position.z)));
		}
	}

	r_dropped = 0;
	LocalVector<float> surface_vertices;
	LocalVector<uint32_t> surface_sources;
	LocalVector<uint32_t> surface_indices;
	for (int s = 0; s < p_input.surfaces.size(); s++) {
		const NextGeoSurfaceInput &surface = p_input.surfaces[s];
		if (!surface.eligible) {
			continue;
		}
		const uint32_t count = uint32_t(surface.positions.size());

		surface_vertices.resize(count * r_mesh.stride);
		for (uint32_t i = 0; i < count; i++) {
			_write_vertex(surface, i, r_mesh, &surface_vertices[i * r_mesh.stride]);
		}

		surface_sources.clear();
		surface_indices.clear();
		if (surface.indices.is_empty()) {
			// Non-indexed: weld identical vertices so the triangles become connected.
			LocalVector<uint32_t> remap;
			remap.resize(count);
			const size_t unique_count = count == 0 ? 0 : meshopt_generateVertexRemap(remap.ptr(), nullptr, count, surface_vertices.ptr(), count, sizeof(float) * r_mesh.stride);
			LocalVector<float> welded;
			welded.resize(uint32_t(unique_count) * r_mesh.stride);
			surface_sources.resize(uint32_t(unique_count));
			for (uint32_t i = count; i > 0; i--) {
				// Walk backwards so the first occurrence wins.
				const uint32_t target = remap[i - 1];
				surface_sources[target] = i - 1;
				for (uint32_t k = 0; k < r_mesh.stride; k++) {
					welded[target * r_mesh.stride + k] = surface_vertices[(i - 1) * r_mesh.stride + k];
				}
			}
			surface_vertices = welded;
			surface_indices = remap;
		} else {
			surface_sources.resize(count);
			for (uint32_t i = 0; i < count; i++) {
				surface_sources[i] = i;
			}
			surface_indices.resize(uint32_t(surface.indices.size()));
			for (int i = 0; i < surface.indices.size(); i++) {
				surface_indices[i] = uint32_t(surface.indices[i]);
			}
		}

		const uint32_t base = r_mesh.vertex_count;
		const uint32_t local_count = surface_sources.size();
		MergedSurface merged;
		merged.surface_id = uint16_t(s);
		merged.indices.reserve(surface_indices.size());
		for (uint32_t t = 0; t + 2 < surface_indices.size(); t += 3) {
			const uint32_t a = surface_indices[t + 0];
			const uint32_t b = surface_indices[t + 1];
			const uint32_t c = surface_indices[t + 2];
			const float *pa = &surface_vertices[a * r_mesh.stride];
			const float *pb = &surface_vertices[b * r_mesh.stride];
			const float *pc = &surface_vertices[c * r_mesh.stride];
			if (a == b || b == c || a == c || _is_degenerate(pa, pb, pc)) {
				r_dropped++;
				continue;
			}
			merged.indices.push_back(base + a);
			merged.indices.push_back(base + b);
			merged.indices.push_back(base + c);
		}

		r_mesh.vertex_count += local_count;
		for (uint32_t i = 0; i < local_count; i++) {
			const float *vertex = &surface_vertices[i * r_mesh.stride];
			for (uint32_t k = 0; k < r_mesh.stride; k++) {
				r_mesh.vertex_data.push_back(vertex[k]);
			}
			for (uint32_t k = 0; k < 3; k++) {
				r_mesh.positions.push_back(vertex[NEXTGEO_VERTEX_OFFSET_POSITION + k]);
			}
			for (uint32_t k = 0; k < 3; k++) {
				r_mesh.attributes.push_back(vertex[NEXTGEO_VERTEX_OFFSET_NORMAL + k]);
			}
			if (r_mesh.layout & NEXTGEO_VERTEX_COLOR) {
				for (uint32_t k = 0; k < 4; k++) {
					r_mesh.attributes.push_back(vertex[r_mesh.color_offset + k]);
				}
			}
			r_mesh.vertex_source.push_back(surface_sources[i]);
			r_mesh.vertex_surface.push_back(uint16_t(s));
			const Vector3 position(vertex[0], vertex[1], vertex[2]);
			r_mesh.vertex_lock.push_back(ineligible_positions.has(position) ? uint8_t(meshopt_SimplifyVertex_Lock) : uint8_t(0));
		}

		if (!merged.indices.is_empty()) {
			r_mesh.surfaces.push_back(std::move(merged));
		}
	}

	r_mesh.position_remap.resize(r_mesh.vertex_count);
	if (r_mesh.vertex_count > 0) {
		meshopt_generatePositionRemap(r_mesh.position_remap.ptr(), r_mesh.positions.ptr(), r_mesh.vertex_count, sizeof(float) * 3);
	}
	return OK;
}

uint32_t _cluster_bytes(const BuildCluster &p_cluster, uint32_t p_stride) {
	return nextgeo_cluster_payload_bytes(p_cluster.vertex_count, p_cluster.indices.size() / 3, p_stride);
}

// Splits a group into consecutive runs that each fit in one page.
void _append_page_sized(const LocalVector<BuildCluster> &p_clusters, const LocalVector<uint32_t> &p_members, uint32_t p_stride, uint32_t p_page_size, LocalVector<LocalVector<uint32_t>> &r_groups) {
	LocalVector<uint32_t> current;
	uint32_t current_bytes = 0;
	for (uint32_t member : p_members) {
		const uint32_t bytes = _cluster_bytes(p_clusters[member], p_stride);
		const bool overflows = current_bytes > UINT32_MAX - bytes;
		const uint32_t next_payload = overflows ? UINT32_MAX : current_bytes + bytes;
		if (!current.is_empty() && (overflows || nextgeo_page_bytes(next_payload) > p_page_size)) {
			r_groups.push_back(std::move(current));
			current = LocalVector<uint32_t>();
			current_bytes = 0;
		}
		current.push_back(member);
		current_bytes += bytes;
	}
	if (!current.is_empty()) {
		r_groups.push_back(std::move(current));
	}
}

// Partitions the pending clusters into groups of about 8 (plan §7.1, step 4.3), split
// further where a group would not fit in one page.
void _partition(const MergedMesh &p_mesh, const LocalVector<BuildCluster> &p_clusters, const LocalVector<uint32_t> &p_pending, uint32_t p_page_size, LocalVector<LocalVector<uint32_t>> &r_groups) {
	LocalVector<LocalVector<uint32_t>> partitions;
	if (p_pending.size() <= NEXTGEO_GROUP_CLUSTERS) {
		partitions.push_back(p_pending);
	} else {
		LocalVector<uint32_t> cluster_indices;
		LocalVector<uint32_t> cluster_counts;
		cluster_counts.resize(p_pending.size());
		for (uint32_t i = 0; i < p_pending.size(); i++) {
			const BuildCluster &cluster = p_clusters[p_pending[i]];
			cluster_counts[i] = cluster.indices.size();
			for (uint32_t index : cluster.indices) {
				cluster_indices.push_back(p_mesh.position_remap[index]);
			}
		}

		LocalVector<uint32_t> partition_of;
		partition_of.resize(p_pending.size());
		const size_t partition_count = meshopt_partitionClusters(partition_of.ptr(), cluster_indices.ptr(), cluster_indices.size(), cluster_counts.ptr(), cluster_counts.size(),
				p_mesh.positions.ptr(), p_mesh.vertex_count, sizeof(float) * 3, NEXTGEO_GROUP_CLUSTERS);

		partitions.resize(uint32_t(partition_count));
		for (uint32_t i = 0; i < p_pending.size(); i++) {
			partitions[partition_of[i]].push_back(p_pending[i]);
		}
	}

	for (const LocalVector<uint32_t> &partition : partitions) {
		if (!partition.is_empty()) {
			_append_page_sized(p_clusters, partition, p_mesh.stride, p_page_size, r_groups);
		}
	}
}

// Locks every vertex position used by more than one group, or shared with a cluster of
// an earlier terminal group, so simplification never opens a crack; adds the permanent
// locks against ineligible siblings.
void _lock_borders(const MergedMesh &p_mesh, const LocalVector<BuildCluster> &p_clusters, const LocalVector<LocalVector<uint32_t>> &p_groups, const LocalVector<uint32_t> &p_frozen,
		LocalVector<uint32_t> &r_owner, LocalVector<uint8_t> &r_shared, LocalVector<uint8_t> &r_locks) {
	const uint32_t frozen_owner = UINT32_MAX - 1;
	for (uint32_t i = 0; i < p_mesh.vertex_count; i++) {
		r_owner[i] = UINT32_MAX;
		r_shared[i] = 0;
	}

	const auto mark = [&](uint32_t p_cluster, uint32_t p_owner) {
		for (uint32_t index : p_clusters[p_cluster].indices) {
			const uint32_t r = p_mesh.position_remap[index];
			if (r_owner[r] == UINT32_MAX) {
				r_owner[r] = p_owner;
			} else if (r_owner[r] != p_owner) {
				r_shared[r] = 1;
			}
		}
	};

	for (uint32_t g = 0; g < p_groups.size(); g++) {
		for (uint32_t member : p_groups[g]) {
			mark(member, g);
		}
	}
	for (uint32_t cluster : p_frozen) {
		mark(cluster, frozen_owner);
	}

	for (uint32_t i = 0; i < p_mesh.vertex_count; i++) {
		r_locks[i] = (r_shared[p_mesh.position_remap[i]] ? uint8_t(meshopt_SimplifyVertex_Lock) : uint8_t(0)) | p_mesh.vertex_lock[i];
	}
}

void _merge_member_bounds(const LocalVector<BuildCluster> &p_clusters, const LocalVector<uint32_t> &p_members, float r_sphere[4], float &r_error) {
	LocalVector<float> spheres;
	spheres.resize(p_members.size() * 4);
	r_error = 0.0f;
	for (uint32_t i = 0; i < p_members.size(); i++) {
		const BuildCluster &cluster = p_clusters[p_members[i]];
		for (uint32_t k = 0; k < 4; k++) {
			spheres[i * 4 + k] = cluster.own_lod[k];
		}
		r_error = MAX(r_error, cluster.own_error);
	}
	nextgeo_enclose_spheres(spheres.ptr(), p_members.size(), sizeof(float) * 4, r_sphere);
}

// Simplifies one group and re-clusterizes the result per surface (plan §7.1, steps 4.3
// and 4.4). Reads shared state only and writes only `r_result`.
void _simplify_group(const MergedMesh &p_mesh, const LocalVector<BuildCluster> &p_clusters, const LocalVector<uint32_t> &p_members, const LocalVector<uint8_t> &p_locks, GroupResult &r_result) {
	float members_error = 0.0f;
	_merge_member_bounds(p_clusters, p_members, r_result.lod, members_error);
	r_result.terminal = true;
	r_result.error = FLT_MAX;

	LocalVector<uint32_t> merged;
	for (uint32_t member : p_members) {
		for (uint32_t index : p_clusters[member].indices) {
			merged.push_back(index);
		}
	}

	const uint32_t target = uint32_t(float(merged.size() / 3) * SIMPLIFY_RATIO) * 3;
	LocalVector<uint32_t> simplified;
	simplified.resize(merged.size());
	float simplify_error = 0.0f;
	const size_t simplified_count = meshopt_simplifyWithAttributes(simplified.ptr(), merged.ptr(), merged.size(),
			p_mesh.positions.ptr(), p_mesh.vertex_count, sizeof(float) * 3,
			p_mesh.attributes.ptr(), sizeof(float) * p_mesh.attribute_count, p_mesh.attribute_weights, p_mesh.attribute_count,
			p_locks.ptr(), target, FLT_MAX, meshopt_SimplifyErrorAbsolute | meshopt_SimplifySparse, &simplify_error);

	if (simplified_count == 0 || uint64_t(simplified_count) * 100 > uint64_t(merged.size()) * SIMPLIFY_TERMINAL_PERCENT) {
		return; // Simplification is stuck: the group is terminal and its clusters are roots.
	}
	simplified.resize(uint32_t(simplified_count));

	// Error stays monotonic: never below the members' own errors.
	r_result.error = MAX(simplify_error, members_error);
	r_result.terminal = false;

	// Re-clusterize per surface so every cluster stays single-material.
	LocalVector<uint32_t> surfaces;
	for (uint32_t t = 0; t < simplified.size(); t += 3) {
		const uint16_t surface = p_mesh.vertex_surface[simplified[t]];
		if (p_mesh.vertex_surface[simplified[t + 1]] != surface || p_mesh.vertex_surface[simplified[t + 2]] != surface) {
			r_result.error_code = ERR_BUG;
			return;
		}
		surfaces.push_back(surface);
	}
	_sort_unique(surfaces);

	LocalVector<uint32_t> surface_indices;
	for (uint32_t surface : surfaces) {
		surface_indices.clear();
		for (uint32_t t = 0; t < simplified.size(); t += 3) {
			if (p_mesh.vertex_surface[simplified[t]] == surface) {
				surface_indices.push_back(simplified[t + 0]);
				surface_indices.push_back(simplified[t + 1]);
				surface_indices.push_back(simplified[t + 2]);
			}
		}
		_clusterize(p_mesh, surface_indices.ptr(), surface_indices.size(), uint16_t(surface), r_result.clusters);
	}
}

struct GroupOrder {
	uint32_t group;
	uint32_t depth;
	bool terminal;
};

struct GroupOrderCompare {
	// Root (terminal) groups first, then coarsest first, then build order.
	_FORCE_INLINE_ bool operator()(const GroupOrder &p_a, const GroupOrder &p_b) const {
		if (p_a.terminal != p_b.terminal) {
			return p_a.terminal;
		}
		if (p_a.depth != p_b.depth) {
			return p_a.depth > p_b.depth;
		}
		return p_a.group < p_b.group;
	}
};

} // namespace

uint32_t nextgeo_cluster_payload_bytes(uint32_t p_vertex_count, uint32_t p_triangle_count, uint32_t p_vertex_stride) {
	return uint32_t(sizeof(NextGeoCluster)) + p_vertex_count * p_vertex_stride * uint32_t(sizeof(float)) + p_triangle_count * 3;
}

uint32_t nextgeo_page_bytes(uint32_t p_cluster_payload_sum) {
	// Header plus padding must fit in the sum. A payload this large cannot be a page.
	if (p_cluster_payload_sum > UINT32_MAX - (NEXTGEO_PAGE_HEADER_BYTES + NEXTGEO_PAGE_ALIGN - 1)) {
		return UINT32_MAX;
	}
	const uint32_t raw = NEXTGEO_PAGE_HEADER_BYTES + p_cluster_payload_sum;
	const uint32_t remainder = raw % NEXTGEO_PAGE_ALIGN;
	const uint32_t pad = remainder == 0 ? 0 : NEXTGEO_PAGE_ALIGN - remainder;
	return raw + pad;
}

Error nextgeo_build_dag(const NextGeoBuildInput &p_input, NextGeoBuildOutput &r_output) {
	r_output = NextGeoBuildOutput();
	ERR_FAIL_COND_V_MSG(p_input.page_size == 0, ERR_INVALID_PARAMETER, "NextGeo: page_size is required.");

	MergedMesh mesh;
	uint32_t dropped = 0;
	const Error merge_error = _merge_surfaces(p_input, mesh, dropped);
	if (merge_error != OK) {
		return merge_error;
	}

	const uint32_t max_cluster_bytes = nextgeo_page_bytes(nextgeo_cluster_payload_bytes(NEXTGEO_MAX_CLUSTER_VERTICES, NEXTGEO_MAX_CLUSTER_TRIANGLES, mesh.stride));
	ERR_FAIL_COND_V_MSG(p_input.page_size < max_cluster_bytes, ERR_INVALID_PARAMETER, vformat("NextGeo: page_size (%d bytes) must hold one full cluster (%d bytes for this vertex layout).", p_input.page_size, max_cluster_bytes));
	ERR_FAIL_COND_V_MSG(mesh.surfaces.is_empty(), ERR_INVALID_DATA, "NextGeo: no eligible, non-degenerate triangles to build.");

	// Level 0: clusterize each surface on its own.
	LocalVector<BuildCluster> clusters;
	{
		LocalVector<LocalVector<BuildCluster>> slots;
		slots.resize(mesh.surfaces.size());
		auto task = [&](uint32_t p_index) {
			const MergedSurface &surface = mesh.surfaces[p_index];
			_clusterize(mesh, surface.indices.ptr(), surface.indices.size(), surface.surface_id, slots[p_index]);
		};
		_run_tasks(slots.size(), p_input.use_threads, task, "NextGeo DAG clusterize");
		for (LocalVector<BuildCluster> &slot : slots) {
			for (BuildCluster &cluster : slot) {
				// Source clusters: the own LOD bound is the culling sphere with zero error.
				for (uint32_t k = 0; k < 4; k++) {
					cluster.own_lod[k] = cluster.bounds[k];
				}
				cluster.own_error = 0.0f;
				clusters.push_back(std::move(cluster));
			}
		}
	}

	uint32_t source_triangles = 0;
	for (const BuildCluster &cluster : clusters) {
		source_triangles += cluster.indices.size() / 3;
	}

	LocalVector<uint32_t> pending;
	pending.resize(clusters.size());
	for (uint32_t i = 0; i < clusters.size(); i++) {
		pending[i] = i;
	}

	LocalVector<BuildGroup> groups;
	LocalVector<uint32_t> frozen; // Clusters of terminal groups from earlier levels.
	LocalVector<uint8_t> locks;
	LocalVector<uint32_t> owner;
	LocalVector<uint8_t> shared;
	locks.resize_initialized(mesh.vertex_count);
	owner.resize(mesh.vertex_count);
	shared.resize(mesh.vertex_count);

	for (uint32_t depth = 0; !pending.is_empty(); depth++) {
		// Stop once what is left fits in one cluster's worth of triangles. For a single
		// surface that is a single pending cluster (as in clusterlod.h); with several
		// surfaces, every surface keeps at least one cluster, so the count alone would
		// keep adding nearly empty levels.
		uint32_t pending_triangles = 0;
		for (uint32_t cluster : pending) {
			pending_triangles += clusters[cluster].indices.size() / 3;
		}
		LocalVector<LocalVector<uint32_t>> parts;
		const bool last = pending.size() == 1 || pending_triangles <= NEXTGEO_MAX_CLUSTER_TRIANGLES || depth + 1 >= MAX_LEVELS;
		if (last) {
			_append_page_sized(clusters, pending, mesh.stride, p_input.page_size, parts);
		} else {
			_partition(mesh, clusters, pending, p_input.page_size, parts);
		}

		LocalVector<GroupResult> results;
		results.resize(parts.size());
		if (last) {
			for (uint32_t i = 0; i < parts.size(); i++) {
				float members_error = 0.0f;
				_merge_member_bounds(clusters, parts[i], results[i].lod, members_error);
			}
		} else {
			_lock_borders(mesh, clusters, parts, frozen, owner, shared, locks);
			auto task = [&](uint32_t p_index) {
				_simplify_group(mesh, clusters, parts[p_index], locks, results[p_index]);
			};
			_run_tasks(parts.size(), p_input.use_threads, task, "NextGeo DAG level");
		}

		// Concatenate in group order, so the result never depends on thread timing.
		pending.clear();
		for (uint32_t i = 0; i < parts.size(); i++) {
			GroupResult &result = results[i];
			ERR_FAIL_COND_V_MSG(result.error_code != OK, result.error_code, "NextGeo: simplification produced a triangle that spans two surfaces.");

			const uint32_t group_id = groups.size();
			BuildGroup group;
			group.members = parts[i];
			group.depth = depth;
			group.terminal = result.terminal;
			group.error = result.terminal ? FLT_MAX : result.error;
			for (uint32_t k = 0; k < 4; k++) {
				group.lod[k] = result.lod[k];
			}

			for (uint32_t member : group.members) {
				BuildCluster &cluster = clusters[member];
				cluster.group = int32_t(group_id);
				for (uint32_t k = 0; k < 4; k++) {
					cluster.parent_lod[k] = group.lod[k];
				}
				cluster.parent_error = group.error;
				if (group.terminal) {
					frozen.push_back(member);
				}
			}

			if (!group.terminal) {
				for (BuildCluster &cluster : result.clusters) {
					cluster.refined = int32_t(group_id);
					for (uint32_t k = 0; k < 4; k++) {
						cluster.own_lod[k] = group.lod[k];
					}
					cluster.own_error = group.error;
					clusters.push_back(std::move(cluster));
					pending.push_back(clusters.size() - 1);
				}
			}
			groups.push_back(std::move(group));
		}
	}

	// Pages (plan §7.1, step 6): root groups first, then coarsest first, whole groups only.
	LocalVector<GroupOrder> order;
	order.resize(groups.size());
	for (uint32_t i = 0; i < groups.size(); i++) {
		order[i].group = i;
		order[i].depth = groups[i].depth;
		order[i].terminal = groups[i].terminal;
	}
	order.sort_custom<GroupOrderCompare>();

	LocalVector<uint32_t> final_group;
	final_group.resize(groups.size());
	for (uint32_t i = 0; i < order.size(); i++) {
		final_group[order[i].group] = i;
	}

	r_output.vertex_layout = mesh.layout;
	r_output.vertex_stride = mesh.stride;
	r_output.page_size = p_input.page_size;
	r_output.source_triangle_count = source_triangles;
	r_output.dropped_triangle_count = dropped;
	r_output.groups.resize_initialized(groups.size());

	uint32_t total_clusters = 0;
	for (const BuildGroup &group : groups) {
		total_clusters += group.members.size();
	}
	r_output.clusters.resize_initialized(total_clusters);

	uint32_t local_vertices[NEXTGEO_MAX_CLUSTER_VERTICES];
	uint8_t local_triangles[NEXTGEO_MAX_CLUSTER_TRIANGLES * 3];
	uint32_t cluster_cursor = 0;
	bool in_root_pages = true;
	uint32_t page_payload = 0;
	for (uint32_t i = 0; i < order.size(); i++) {
		const BuildGroup &group = groups[order[i].group];

		uint32_t group_payload = 0;
		for (uint32_t member : group.members) {
			group_payload += _cluster_bytes(clusters[member], mesh.stride);
		}
		ERR_FAIL_COND_V(nextgeo_page_bytes(group_payload) > p_input.page_size, ERR_BUG);

		// Open a new page when the group does not fit, or when the root pages end.
		const bool leaving_roots = in_root_pages && !group.terminal;
		if (leaving_roots) {
			r_output.root_page_count = r_output.pages.size();
			in_root_pages = false;
		}
		const bool payload_overflows = page_payload > UINT32_MAX - group_payload;
		if (r_output.pages.is_empty() || leaving_roots || payload_overflows || nextgeo_page_bytes(page_payload + group_payload) > p_input.page_size) {
			NextGeoPage page = {};
			page.group_offset = i;
			page.cluster_offset = cluster_cursor;
			r_output.pages.push_back(page);
			page_payload = 0;
		}
		const uint32_t page_id = r_output.pages.size() - 1;
		NextGeoPage &page = r_output.pages[page_id];
		page.group_count++;
		page.cluster_count += group.members.size();
		page_payload += group_payload;
		page.byte_size = nextgeo_page_bytes(page_payload);

		NextGeoGroup &out_group = r_output.groups[i];
		out_group.cluster_offset = cluster_cursor;
		out_group.cluster_count = group.members.size();
		out_group.page_id = page_id;
		out_group.depth = group.depth;
		for (uint32_t k = 0; k < 4; k++) {
			out_group.parent_lod[k] = group.lod[k];
		}
		out_group.parent_error = group.error;
		for (uint32_t k = 0; k < 3; k++) {
			out_group.aabb_min[k] = FLT_MAX;
			out_group.aabb_max[k] = -FLT_MAX;
		}

		for (uint32_t member : group.members) {
			const BuildCluster &cluster = clusters[member];
			const uint32_t index_count = cluster.indices.size();
			const size_t vertex_count = meshopt_extractMeshletIndices(local_vertices, local_triangles, cluster.indices.ptr(), index_count);
			ERR_FAIL_COND_V(vertex_count != cluster.vertex_count, ERR_BUG);

			NextGeoCluster &out = r_output.clusters[cluster_cursor++];
			for (uint32_t k = 0; k < 4; k++) {
				out.bounds[k] = cluster.bounds[k];
				out.group_lod[k] = cluster.own_lod[k];
				out.parent_lod[k] = cluster.parent_lod[k];
			}
			for (uint32_t k = 0; k < 3; k++) {
				out.cone_axis[k] = cluster.cone_axis[k];
			}
			out.cone_cutoff = cluster.cone_cutoff;
			out.group_error = cluster.own_error;
			out.parent_error = cluster.parent_error;
			out.refined_group = cluster.refined < 0 ? -1 : int32_t(final_group[cluster.refined]);
			out.page_id = page_id;
			out.vertex_offset = r_output.vertex_sources.size();
			out.triangle_offset = r_output.triangles.size() / 3;
			out.vertex_count = uint8_t(vertex_count);
			out.triangle_count = uint8_t(index_count / 3);
			out.surface_id = cluster.surface_id;

			for (uint32_t v = 0; v < vertex_count; v++) {
				const uint32_t source = local_vertices[v];
				const float *vertex = &mesh.vertex_data[source * mesh.stride];
				for (uint32_t k = 0; k < mesh.stride; k++) {
					r_output.vertices.push_back(vertex[k]);
				}
				r_output.vertex_sources.push_back(mesh.vertex_source[source]);
				for (uint32_t k = 0; k < 3; k++) {
					out_group.aabb_min[k] = MIN(out_group.aabb_min[k], vertex[NEXTGEO_VERTEX_OFFSET_POSITION + k]);
					out_group.aabb_max[k] = MAX(out_group.aabb_max[k], vertex[NEXTGEO_VERTEX_OFFSET_POSITION + k]);
				}
			}
			for (uint32_t j = 0; j < index_count; j++) {
				r_output.triangles.push_back(local_triangles[j]);
			}
		}

		r_output.dag_depth = MAX(r_output.dag_depth, group.depth + 1);
	}
	if (in_root_pages) {
		r_output.root_page_count = r_output.pages.size();
	}

	nextgeo_build_bvh(r_output.groups.ptr(), r_output.groups.size(), r_output.bvh);
	return OK;
}
