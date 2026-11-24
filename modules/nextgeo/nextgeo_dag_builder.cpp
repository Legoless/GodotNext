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
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_voxels.h"

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
// Skin weights count in the simplifier's attribute error (issue #159, step 3).
constexpr float SKIN_WEIGHT = 1.0f;
// Normal (3) + color (4) + skin weights (at most 8).
constexpr uint32_t MAX_ATTRIBUTES = 15;

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

	// Skinning, padded to `skin_influences` per merged vertex (0 = unskinned mesh).
	uint32_t skin_influences = 0;
	LocalVector<int32_t> skin_bones;
	LocalVector<float> skin_weights;
};

struct BuildCluster {
	LocalVector<uint32_t> indices; // Merged vertex indices, 3 per triangle.
	uint32_t vertex_count = 0;
	LocalVector<uint16_t> bones; // Sorted global bones touched with weight > 0 (skinned builds).
	uint16_t surface_id = 0;
	// GodotNext: nextgeo_foliage_instancing (#151). Centroid linear scale applied on emission (> 1.0 when preserve_area is active).
	float area_scale = 1.0f;
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

// Sorted global bones touched (weight > 0) by the merged vertices of `p_indices`.
void _collect_bones(const MergedMesh &p_mesh, const LocalVector<uint32_t> &p_indices, LocalVector<uint16_t> &r_bones) {
	r_bones.clear();
	if (p_mesh.skin_influences == 0) {
		return;
	}
	for (uint32_t index : p_indices) {
		const uint32_t base = index * p_mesh.skin_influences;
		for (uint32_t k = 0; k < p_mesh.skin_influences; k++) {
			if (p_mesh.skin_weights[base + k] > 0.0f) {
				r_bones.push_back(uint16_t(p_mesh.skin_bones[base + k]));
			}
		}
	}
	r_bones.sort();
	uint32_t write = 0;
	for (uint32_t i = 0; i < r_bones.size(); i++) {
		if (write == 0 || r_bones[write - 1] != r_bones[i]) {
			r_bones[write++] = r_bones[i];
		}
	}
	r_bones.resize(write);
}

bool _bone_in(const LocalVector<uint16_t> &p_bones, uint16_t p_bone) {
	for (uint16_t bone : p_bones) {
		if (bone == p_bone) {
			return true;
		}
	}
	return false;
}

double _triangles_area(const MergedMesh &p_mesh, const LocalVector<uint32_t> &p_indices) {
	double total = 0.0;
	for (uint32_t t = 0; t + 2 < p_indices.size(); t += 3) {
		const float *p0 = &p_mesh.positions[p_indices[t + 0] * 3];
		const float *p1 = &p_mesh.positions[p_indices[t + 1] * 3];
		const float *p2 = &p_mesh.positions[p_indices[t + 2] * 3];
		const double ex1 = double(p1[0]) - double(p0[0]);
		const double ey1 = double(p1[1]) - double(p0[1]);
		const double ez1 = double(p1[2]) - double(p0[2]);
		const double ex2 = double(p2[0]) - double(p0[0]);
		const double ey2 = double(p2[1]) - double(p0[1]);
		const double ez2 = double(p2[2]) - double(p0[2]);
		const double cx = ey1 * ez2 - ez1 * ey2;
		const double cy = ez1 * ex2 - ex1 * ez2;
		const double cz = ex1 * ey2 - ey1 * ex2;
		total += 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
	}
	return total;
}

// One build cluster over a merged-index triangle list: bounds, cone and bone list.
void _make_cluster(const MergedMesh &p_mesh, LocalVector<uint32_t> &&p_indices, uint16_t p_surface_id, LocalVector<BuildCluster> &r_clusters, float p_area_scale = 1.0f) {
	if (p_indices.is_empty()) {
		return;
	}
	BuildCluster cluster;
	cluster.surface_id = p_surface_id;
	cluster.area_scale = p_area_scale;
	cluster.indices = std::move(p_indices);

	_collect_bones(p_mesh, cluster.indices, cluster.bones);

	meshopt_Bounds bounds;
	if (p_area_scale > 1.0f) {
		// Unshared vertices per triangle scaled about each triangle's centroid.
		const uint32_t corner_count = cluster.indices.size();
		cluster.vertex_count = corner_count;
		LocalVector<float> scaled_positions;
		scaled_positions.resize(corner_count * 3);
		LocalVector<uint32_t> seq_indices;
		seq_indices.resize(corner_count);
		for (uint32_t t = 0; t + 2 < corner_count; t += 3) {
			const float *p0 = &p_mesh.positions[cluster.indices[t + 0] * 3];
			const float *p1 = &p_mesh.positions[cluster.indices[t + 1] * 3];
			const float *p2 = &p_mesh.positions[cluster.indices[t + 2] * 3];
			const float cx = (p0[0] + p1[0] + p2[0]) / 3.0f;
			const float cy = (p0[1] + p1[1] + p2[1]) / 3.0f;
			const float cz = (p0[2] + p1[2] + p2[2]) / 3.0f;
			const float *pts[3] = { p0, p1, p2 };
			for (uint32_t c = 0; c < 3; c++) {
				const uint32_t idx = t + c;
				scaled_positions[idx * 3 + 0] = cx + (pts[c][0] - cx) * p_area_scale;
				scaled_positions[idx * 3 + 1] = cy + (pts[c][1] - cy) * p_area_scale;
				scaled_positions[idx * 3 + 2] = cz + (pts[c][2] - cz) * p_area_scale;
				seq_indices[idx] = idx;
			}
		}
		bounds = meshopt_computeClusterBounds(seq_indices.ptr(), corner_count, scaled_positions.ptr(), corner_count, sizeof(float) * 3);
	} else {
		LocalVector<uint32_t> unique(cluster.indices);
		_sort_unique(unique);
		cluster.vertex_count = unique.size();
		bounds = meshopt_computeClusterBounds(cluster.indices.ptr(), cluster.indices.size(), p_mesh.positions.ptr(), p_mesh.vertex_count, sizeof(float) * 3);
	}
	_write_sphere(bounds, cluster.bounds);
	for (uint32_t k = 0; k < 3; k++) {
		cluster.cone_axis[k] = bounds.cone_axis_s8[k];
	}
	cluster.cone_cutoff = bounds.cone_cutoff_s8;
	r_clusters.push_back(std::move(cluster));
}

// Splits a single-surface triangle list into clusters of at most 128 vertices and 128
// triangles (plan §7.1, step 4.2). Vertices are compacted first so that the meshlet
// builder's cost does not scale with the size of the whole merged stream.
void _clusterize(const MergedMesh &p_mesh, const uint32_t *p_indices, uint32_t p_index_count, uint16_t p_surface_id, LocalVector<BuildCluster> &r_clusters, float p_area_scale = 1.0f) {
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

	const uint32_t max_triangles = p_area_scale > 1.0f ? (NEXTGEO_MAX_CLUSTER_VERTICES / 3u) : NEXTGEO_MAX_CLUSTER_TRIANGLES;
	const size_t max_meshlets = meshopt_buildMeshletsBound(p_index_count, NEXTGEO_MAX_CLUSTER_VERTICES, max_triangles);
	LocalVector<meshopt_Meshlet> meshlets;
	meshlets.resize(uint32_t(max_meshlets));
	LocalVector<uint32_t> meshlet_vertices;
	meshlet_vertices.resize(p_index_count);
	LocalVector<uint8_t> meshlet_triangles;
	meshlet_triangles.resize(p_index_count);

	const size_t meshlet_count = meshopt_buildMeshlets(meshlets.ptr(), meshlet_vertices.ptr(), meshlet_triangles.ptr(),
			local_indices.ptr(), p_index_count, local_positions.ptr(), unique.size(), sizeof(float) * 3,
			NEXTGEO_MAX_CLUSTER_VERTICES, max_triangles, CLUSTER_CONE_WEIGHT);

	for (size_t m = 0; m < meshlet_count; m++) {
		const meshopt_Meshlet &meshlet = meshlets[uint32_t(m)];
		meshopt_optimizeMeshlet(&meshlet_vertices[meshlet.vertex_offset], &meshlet_triangles[meshlet.triangle_offset], meshlet.triangle_count, meshlet.vertex_count);

		LocalVector<uint32_t> indices;
		indices.resize(meshlet.triangle_count * 3);
		for (uint32_t j = 0; j < meshlet.triangle_count * 3; j++) {
			indices[j] = unique[meshlet_vertices[meshlet.vertex_offset + meshlet_triangles[meshlet.triangle_offset + j]]];
		}

		if (p_mesh.skin_influences == 0) {
			_make_cluster(p_mesh, std::move(indices), p_surface_id, r_clusters, p_area_scale);
			continue;
		}

		// Bone limit (issue #159, step 4): split the meshlet's triangles, in order,
		// until every part touches at most NEXTGEO_MAX_CLUSTER_BONES bones. A lone
		// triangle that touches more keeps all of its bones; dropping an influence
		// would change the skinned result.
		LocalVector<uint32_t> current;
		LocalVector<uint16_t> current_bones;
		for (uint32_t t = 0; t < indices.size(); t += 3) {
			LocalVector<uint16_t> tri_bones;
			for (uint32_t c = 0; c < 3; c++) {
				const uint32_t base = indices[t + c] * p_mesh.skin_influences;
				for (uint32_t k = 0; k < p_mesh.skin_influences; k++) {
					if (p_mesh.skin_weights[base + k] > 0.0f) {
						const uint16_t bone = uint16_t(p_mesh.skin_bones[base + k]);
						if (!_bone_in(tri_bones, bone)) {
							tri_bones.push_back(bone);
						}
					}
				}
			}
			uint32_t extra = 0;
			for (uint16_t bone : tri_bones) {
				if (!_bone_in(current_bones, bone)) {
					extra++;
				}
			}
			if (!current.is_empty() && current_bones.size() + extra > NEXTGEO_MAX_CLUSTER_BONES) {
				_make_cluster(p_mesh, std::move(current), p_surface_id, r_clusters, p_area_scale);
				current = LocalVector<uint32_t>();
				current_bones.clear();
			}
			for (uint16_t bone : tri_bones) {
				if (!_bone_in(current_bones, bone)) {
					current_bones.push_back(bone);
				}
			}
			current.push_back(indices[t + 0]);
			current.push_back(indices[t + 1]);
			current.push_back(indices[t + 2]);
		}
		_make_cluster(p_mesh, std::move(current), p_surface_id, r_clusters, p_area_scale);
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
		ERR_FAIL_COND_V_MSG(surface.skin_influences != 0 && surface.skin_influences != 4 && surface.skin_influences != 8, ERR_INVALID_DATA, vformat("NextGeo: surface %d has %d skin influences; only 0, 4 or 8 are supported.", s, surface.skin_influences));
		if (surface.skin_influences == 0) {
			ERR_FAIL_COND_V_MSG(!surface.bones.is_empty() || !surface.weights.is_empty(), ERR_INVALID_DATA, vformat("NextGeo: surface %d has bones or weights without skin influences.", s));
		} else {
			ERR_FAIL_COND_V_MSG(surface.bones.size() != count * int(surface.skin_influences) || surface.weights.size() != count * int(surface.skin_influences), ERR_INVALID_DATA, vformat("NextGeo: surface %d has a bone or weight count that does not match its vertices.", s));
			for (int i = 0; i < surface.bones.size(); i++) {
				ERR_FAIL_COND_V_MSG(surface.bones[i] < 0 || surface.bones[i] > int(UINT16_MAX), ERR_INVALID_DATA, vformat("NextGeo: surface %d has an out-of-range bone index.", s));
				ERR_FAIL_COND_V_MSG(!std::isfinite(double(surface.weights[i])) || surface.weights[i] < 0.0f, ERR_INVALID_DATA, vformat("NextGeo: surface %d has a non-finite or negative skin weight.", s));
			}
		}
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
			r_mesh.skin_influences = MAX(r_mesh.skin_influences, surface.skin_influences);
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
	for (uint32_t k = 0; k < r_mesh.skin_influences; k++) {
		r_mesh.attribute_weights[r_mesh.attribute_count++] = SKIN_WEIGHT;
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
			// Skinned vertices weld on their bones and weights too, so copies that only
			// differ in skinning stay apart.
			LocalVector<float> weld_vertices;
			const float *weld_data = surface_vertices.ptr();
			uint32_t weld_stride = r_mesh.stride;
			if (surface.skin_influences > 0 && count > 0) {
				weld_stride = r_mesh.stride + surface.skin_influences * 2;
				weld_vertices.resize(count * weld_stride);
				for (uint32_t i = 0; i < count; i++) {
					float *dst = &weld_vertices[i * weld_stride];
					for (uint32_t k = 0; k < r_mesh.stride; k++) {
						dst[k] = surface_vertices[i * r_mesh.stride + k];
					}
					for (uint32_t k = 0; k < surface.skin_influences; k++) {
						dst[r_mesh.stride + k] = float(surface.bones[i * surface.skin_influences + k]);
						dst[r_mesh.stride + surface.skin_influences + k] = surface.weights[i * surface.skin_influences + k];
					}
				}
				weld_data = weld_vertices.ptr();
			}
			LocalVector<uint32_t> remap;
			remap.resize(count);
			const size_t unique_count = count == 0 ? 0 : meshopt_generateVertexRemap(remap.ptr(), nullptr, count, weld_data, count, sizeof(float) * weld_stride);
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
			// Skin weights are simplifier attributes; bones and weights are kept per
			// merged vertex, padded with zero weights to the mesh influence count.
			const uint32_t source_vertex = surface_sources[i];
			for (uint32_t k = 0; k < r_mesh.skin_influences; k++) {
				const bool have = k < surface.skin_influences;
				const int32_t bone = have ? surface.bones[source_vertex * surface.skin_influences + k] : 0;
				const float weight = have ? surface.weights[source_vertex * surface.skin_influences + k] : 0.0f;
				r_mesh.attributes.push_back(weight);
				r_mesh.skin_bones.push_back(bone);
				r_mesh.skin_weights.push_back(weight);
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

uint32_t _cluster_bytes(const BuildCluster &p_cluster, uint32_t p_stride, uint32_t p_influences) {
	uint32_t bytes = nextgeo_cluster_payload_bytes(p_cluster.vertex_count, p_cluster.indices.size() / 3, p_stride);
	if (p_influences > 0) {
		bytes += nextgeo_cluster_skin_bytes(p_cluster.bones.size(), p_cluster.vertex_count, p_influences);
	}
	return bytes;
}

// Splits a group into consecutive runs that each fit in one page.
void _append_page_sized(const LocalVector<BuildCluster> &p_clusters, const LocalVector<uint32_t> &p_members, uint32_t p_stride, uint32_t p_influences, uint32_t p_page_size, LocalVector<LocalVector<uint32_t>> &r_groups) {
	LocalVector<uint32_t> current;
	uint32_t current_bytes = 0;
	for (uint32_t member : p_members) {
		const uint32_t bytes = _cluster_bytes(p_clusters[member], p_stride, p_influences);
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
			_append_page_sized(p_clusters, partition, p_mesh.stride, p_mesh.skin_influences, p_page_size, r_groups);
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
void _simplify_group(const MergedMesh &p_mesh, const LocalVector<BuildCluster> &p_clusters, const LocalVector<uint32_t> &p_members, const LocalVector<uint8_t> &p_locks, bool p_preserve_area, GroupResult &r_result) {
	float members_error = 0.0f;
	_merge_member_bounds(p_clusters, p_members, r_result.lod, members_error);
	r_result.terminal = true;
	r_result.error = FLT_MAX;

	LocalVector<uint32_t> merged;
	double source_area = 0.0;
	for (uint32_t member : p_members) {
		const BuildCluster &mc = p_clusters[member];
		for (uint32_t index : mc.indices) {
			merged.push_back(index);
		}
		if (p_preserve_area) {
			const double s = double(mc.area_scale);
			source_area += _triangles_area(p_mesh, mc.indices) * s * s;
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

	// GodotNext: nextgeo_foliage_instancing (#151). Preserve Area scales each simplified
	// triangle about its centroid so the group's surface area matches the source group, clamped at 1.5x.
	float area_scale = 1.0f;
	if (p_preserve_area) {
		const double simplified_area = _triangles_area(p_mesh, simplified);
		area_scale = nextgeo_preserve_area_scale(source_area, simplified_area);
	}

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
		_clusterize(p_mesh, surface_indices.ptr(), surface_indices.size(), uint16_t(surface), r_result.clusters, area_scale);
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

float nextgeo_preserve_area_scale(double p_source_area, double p_simplified_area) {
	if (!Math::is_finite(p_source_area) || !Math::is_finite(p_simplified_area) || !(p_simplified_area > 0.0) || !(p_source_area > p_simplified_area)) {
		return 1.0f;
	}
	const double linear_scale = std::sqrt(p_source_area / p_simplified_area);
	return float(CLAMP(linear_scale, 1.0, double(NEXTGEO_PRESERVE_AREA_MAX_SCALE)));
}

uint32_t nextgeo_cluster_payload_bytes(uint32_t p_vertex_count, uint32_t p_triangle_count, uint32_t p_vertex_stride) {
	return uint32_t(sizeof(NextGeoCluster)) + p_vertex_count * p_vertex_stride * uint32_t(sizeof(float)) + p_triangle_count * 3;
}

uint32_t nextgeo_cluster_skin_bytes(uint32_t p_bone_count, uint32_t p_vertex_count, uint32_t p_influences) {
	if (p_influences == 0) {
		return 0;
	}
	return 4 + p_bone_count * 26 + p_vertex_count * p_influences * 3;
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

	uint32_t full_cluster_bytes = nextgeo_cluster_payload_bytes(NEXTGEO_MAX_CLUSTER_VERTICES, NEXTGEO_MAX_CLUSTER_TRIANGLES, mesh.stride);
	if (mesh.skin_influences > 0) {
		full_cluster_bytes += nextgeo_cluster_skin_bytes(NEXTGEO_MAX_CLUSTER_BONES, NEXTGEO_MAX_CLUSTER_VERTICES, mesh.skin_influences);
	}
	const uint32_t max_cluster_bytes = nextgeo_page_bytes(full_cluster_bytes);
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
			_append_page_sized(clusters, pending, mesh.stride, mesh.skin_influences, p_input.page_size, parts);
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
				_simplify_group(mesh, clusters, parts[p_index], locks, p_input.preserve_area, results[p_index]);
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
	r_output.skin_influences = mesh.skin_influences;
	if (mesh.skin_influences > 0) {
		r_output.cluster_skin.resize_initialized(total_clusters);
	}

	// GodotNext: nextgeo_voxels (#165, plan §21.4 contract N10).
	// Voxelize coarse cluster groups past the provisional LOD error threshold using their
	// descendant source triangles so leaf canopies preserve volume and coverage at distance.
	struct GroupVoxelBuild {
		LocalVector<NextGeoVoxelBrick> bricks;
		LocalVector<NextGeoVoxelData> voxels;
		uint32_t bricks_bytes = 0u;
		float voxel_size = 0.0f;
		float group_error = 0.0f;
	};
	LocalVector<GroupVoxelBuild> built_voxels;
	if (p_input.voxels && !order.is_empty()) {
		// Compute mesh bounding sphere radius for scale-invariant provisional error threshold.
		float mesh_min[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
		float mesh_max[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		for (uint32_t v = 0; v < mesh.vertex_count; v++) {
			for (uint32_t k = 0; k < 3; k++) {
				const float p = mesh.positions[v * 3 + k];
				mesh_min[k] = MIN(mesh_min[k], p);
				mesh_max[k] = MAX(mesh_max[k], p);
			}
		}
		const float mdx = mesh_max[0] - mesh_min[0];
		const float mdy = mesh_max[1] - mesh_min[1];
		const float mdz = mesh_max[2] - mesh_min[2];
		const float mesh_radius = 0.5f * std::sqrt(mdx * mdx + mdy * mdy + mdz * mdz);
		const float error_threshold = mesh_radius * RendererRD::NEXTGEO_VOXEL_PROVISIONAL_ERROR_RATIO;

		built_voxels.resize(order.size());
		auto vox_task = [&](uint32_t p_i) {
			const uint32_t raw_group_idx = order[p_i].group;
			const BuildGroup &group = groups[raw_group_idx];
			float members_own_error = 0.0f;
			uint32_t group_cluster_payload = 0u;
			for (uint32_t member : group.members) {
				members_own_error = MAX(members_own_error, clusters[member].own_error);
				group_cluster_payload += _cluster_bytes(clusters[member], mesh.stride, mesh.skin_influences);
			}
			// Derive each group's voxel size from its own LOD error (members_own_error) rather than
			// its parent's error: when own_error passes at <= 1 px, each voxel projects to <= 1 px
			// and each 4^3 brick projects to <= 4 px (strictly within the provisional 8 px threshold).
			const float effective_error = group.terminal
					? MAX(members_own_error, group.lod[3] * RendererRD::NEXTGEO_VOXEL_TERMINAL_ERROR_FRACTION)
					: members_own_error;
			if (!(group.depth > 0u || group.terminal) || !(effective_error >= error_threshold)) {
				return;
			}

			// Gather all descendant source clusters (refined < 0) reachable from this group.
			LocalVector<uint8_t> visited_groups;
			visited_groups.resize_initialized(groups.size());
			LocalVector<uint32_t> stack;
			stack.push_back(raw_group_idx);
			visited_groups[raw_group_idx] = 1u;

			LocalVector<uint32_t> source_clusters;
			while (!stack.is_empty()) {
				const uint32_t g_id = stack[stack.size() - 1];
				stack.resize(stack.size() - 1);
				for (uint32_t member : groups[g_id].members) {
					const int32_t ref_g = clusters[member].refined;
					if (ref_g < 0) {
						source_clusters.push_back(member);
					} else if (uint32_t(ref_g) < groups.size() && !visited_groups[ref_g]) {
						visited_groups[ref_g] = 1u;
						stack.push_back(uint32_t(ref_g));
					}
				}
			}
			if (source_clusters.is_empty()) {
				return;
			}

			LocalVector<NextGeoVoxelTriInput> tris;
			float gmin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
			float gmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
			for (uint32_t c_idx : source_clusters) {
				const BuildCluster &cl = clusters[c_idx];
				const uint32_t ic = cl.indices.size();
				for (uint32_t t = 0; t + 2 < ic; t += 3) {
					NextGeoVoxelTriInput tri;
					tri.surface_id = cl.surface_id;
					for (int c = 0; c < 3; c++) {
						const uint32_t mv = cl.indices[t + c];
						const float *vd = &mesh.vertex_data[mv * mesh.stride];
						tri.p[c] = Vector3(vd[NEXTGEO_VERTEX_OFFSET_POSITION], vd[NEXTGEO_VERTEX_OFFSET_POSITION + 1], vd[NEXTGEO_VERTEX_OFFSET_POSITION + 2]);
						tri.n[c] = Vector3(vd[NEXTGEO_VERTEX_OFFSET_NORMAL], vd[NEXTGEO_VERTEX_OFFSET_NORMAL + 1], vd[NEXTGEO_VERTEX_OFFSET_NORMAL + 2]);
						tri.uv[c] = Vector2(vd[NEXTGEO_VERTEX_OFFSET_UV0], vd[NEXTGEO_VERTEX_OFFSET_UV0 + 1]);
						for (int k = 0; k < 3; k++) {
							gmin[k] = MIN(gmin[k], float(tri.p[c][k]));
							gmax[k] = MAX(gmax[k], float(tri.p[c][k]));
						}
					}
					tris.push_back(tri);
				}
			}
			if (tris.is_empty()) {
				return;
			}

			const float gex = MAX(gmax[0] - gmin[0], MAX(gmax[1] - gmin[1], gmax[2] - gmin[2]));
			const float min_vs = MAX(gex / 32.0f, 1.0e-4f);
			const float max_vs = MAX(gex / 4.0f, min_vs);
			float voxel_size = CLAMP(effective_error * RendererRD::NEXTGEO_VOXEL_ERROR_TO_SIZE_SCALE, min_vs, max_vs);

			// Keep this group's voxel bricks strictly inside the remaining page budget.
			const uint32_t cluster_page_used = nextgeo_page_bytes(group_cluster_payload);
			const uint32_t max_section_bytes = (p_input.page_size > cluster_page_used + 64u) ? MIN<uint32_t>(p_input.page_size - cluster_page_used - 32u, p_input.page_size / 2u) : 0u;
			if (max_section_bytes < 64u) {
				return;
			}

			GroupVoxelBuild &out_v = built_voxels[p_i];
			for (int attempt = 0; attempt < 6; attempt++) {
				nextgeo_voxelize_triangles(tris, voxel_size, out_v.bricks, out_v.voxels);
				const uint32_t bbytes = nextgeo_voxel_bricks_bytes(out_v.bricks.ptr(), out_v.bricks.size());
				if (!out_v.bricks.is_empty() && nextgeo_voxel_page_section_max_bytes(1u, bbytes) <= max_section_bytes) {
					out_v.bricks_bytes = bbytes;
					out_v.voxel_size = voxel_size;
					out_v.group_error = effective_error;
					return;
				}
				voxel_size *= 1.5f;
			}
			out_v.bricks.clear();
			out_v.voxels.clear();
			out_v.bricks_bytes = 0u;
		};
		_run_tasks(order.size(), p_input.use_threads, vox_task, "NextGeo DAG voxelize");

		bool any_voxels = false;
		for (uint32_t i = 0; i < built_voxels.size(); i++) {
			if (!built_voxels[i].bricks.is_empty()) {
				any_voxels = true;
				break;
			}
		}
		if (any_voxels) {
			r_output.group_voxels.resize_initialized(order.size());
		} else {
			built_voxels.clear();
		}
	}

	uint32_t local_vertices[NEXTGEO_MAX_CLUSTER_VERTICES];
	uint8_t local_triangles[NEXTGEO_MAX_CLUSTER_TRIANGLES * 3];
	uint32_t cluster_cursor = 0;
	bool in_root_pages = true;
	uint32_t page_payload = 0;
	uint32_t page_voxel_bricks_bytes = 0;
	uint32_t page_group_count = 0;
	for (uint32_t i = 0; i < order.size(); i++) {
		const BuildGroup &group = groups[order[i].group];

		uint32_t group_payload = 0;
		for (uint32_t member : group.members) {
			group_payload += _cluster_bytes(clusters[member], mesh.stride, mesh.skin_influences);
		}
		const uint32_t group_vox_bytes = !built_voxels.is_empty() ? built_voxels[i].bricks_bytes : 0u;
		const uint32_t solo_vox_section = nextgeo_voxel_page_section_max_bytes(1u, group_vox_bytes);
		ERR_FAIL_COND_V(nextgeo_page_bytes(group_payload + solo_vox_section) > p_input.page_size, ERR_BUG);

		// Open a new page when the group does not fit, or when the root pages end.
		const bool leaving_roots = in_root_pages && !group.terminal;
		if (leaving_roots) {
			r_output.root_page_count = r_output.pages.size();
			in_root_pages = false;
		}
		const uint32_t next_vox_bricks = page_voxel_bricks_bytes + group_vox_bytes;
		const uint32_t next_vox_section = nextgeo_voxel_page_section_max_bytes(page_group_count + 1u, next_vox_bricks);
		const bool payload_overflows = page_payload > UINT32_MAX - group_payload || (page_payload + group_payload) > UINT32_MAX - next_vox_section;
		if (r_output.pages.is_empty() || leaving_roots || payload_overflows || nextgeo_page_bytes(page_payload + group_payload + next_vox_section) > p_input.page_size) {
			NextGeoPage page = {};
			page.group_offset = i;
			page.cluster_offset = cluster_cursor;
			r_output.pages.push_back(page);
			page_payload = 0;
			page_voxel_bricks_bytes = 0;
			page_group_count = 0;
		}
		const uint32_t page_id = r_output.pages.size() - 1;
		NextGeoPage &page = r_output.pages[page_id];
		page.group_count++;
		page.cluster_count += group.members.size();
		page_payload += group_payload;
		page_voxel_bricks_bytes += group_vox_bytes;
		page_group_count++;
		const uint32_t cur_vox_section = nextgeo_voxel_page_section_max_bytes(page_group_count, page_voxel_bricks_bytes);
		page.byte_size = nextgeo_page_bytes(page_payload + cur_vox_section);

		if (!r_output.group_voxels.is_empty() && !built_voxels[i].bricks.is_empty()) {
			const GroupVoxelBuild &gv = built_voxels[i];
			NextGeoGroupVoxels &out_gv = r_output.group_voxels[i];
			out_gv.brick_offset = r_output.voxel_bricks.size();
			out_gv.brick_count = gv.bricks.size();
			out_gv.voxel_size = gv.voxel_size;
			out_gv.group_error = gv.group_error;
			const uint32_t voxel_base = r_output.voxel_data.size();
			for (uint32_t b = 0; b < gv.bricks.size(); b++) {
				NextGeoVoxelBrick brick = gv.bricks[b];
				brick.voxel_offset += voxel_base;
				r_output.voxel_bricks.push_back(brick);
			}
			for (uint32_t v = 0; v < gv.voxels.size(); v++) {
				r_output.voxel_data.push_back(gv.voxels[v]);
			}
		}

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
			const bool apply_area_scale = cluster.area_scale > 1.0f;
			size_t vertex_count = 0;
			float scaled_pos_scratch[NEXTGEO_MAX_CLUSTER_VERTICES * 3];
			if (apply_area_scale) {
				ERR_FAIL_COND_V(index_count > NEXTGEO_MAX_CLUSTER_VERTICES, ERR_BUG);
				vertex_count = index_count;
				ERR_FAIL_COND_V(vertex_count != cluster.vertex_count, ERR_BUG);
				for (uint32_t t = 0; t + 2 < index_count; t += 3) {
					const float *p0 = &mesh.positions[cluster.indices[t + 0] * 3];
					const float *p1 = &mesh.positions[cluster.indices[t + 1] * 3];
					const float *p2 = &mesh.positions[cluster.indices[t + 2] * 3];
					const float cx = (p0[0] + p1[0] + p2[0]) / 3.0f;
					const float cy = (p0[1] + p1[1] + p2[1]) / 3.0f;
					const float cz = (p0[2] + p1[2] + p2[2]) / 3.0f;
					const float *pts[3] = { p0, p1, p2 };
					for (uint32_t c = 0; c < 3; c++) {
						const uint32_t idx = t + c;
						local_vertices[idx] = cluster.indices[idx];
						local_triangles[idx] = uint8_t(idx);
						scaled_pos_scratch[idx * 3 + 0] = cx + (pts[c][0] - cx) * cluster.area_scale;
						scaled_pos_scratch[idx * 3 + 1] = cy + (pts[c][1] - cy) * cluster.area_scale;
						scaled_pos_scratch[idx * 3 + 2] = cz + (pts[c][2] - cz) * cluster.area_scale;
					}
				}
			} else {
				vertex_count = meshopt_extractMeshletIndices(local_vertices, local_triangles, cluster.indices.ptr(), index_count);
				ERR_FAIL_COND_V(vertex_count != cluster.vertex_count, ERR_BUG);
			}

			const uint32_t out_index = cluster_cursor;
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
				const float *pos_ptr = apply_area_scale ? &scaled_pos_scratch[v * 3] : &vertex[NEXTGEO_VERTEX_OFFSET_POSITION];
				for (uint32_t k = 0; k < mesh.stride; k++) {
					if (apply_area_scale && k >= NEXTGEO_VERTEX_OFFSET_POSITION && k < NEXTGEO_VERTEX_OFFSET_POSITION + 3) {
						r_output.vertices.push_back(pos_ptr[k - NEXTGEO_VERTEX_OFFSET_POSITION]);
					} else {
						r_output.vertices.push_back(vertex[k]);
					}
				}
				r_output.vertex_sources.push_back(mesh.vertex_source[source]);
				for (uint32_t k = 0; k < 3; k++) {
					out_group.aabb_min[k] = MIN(out_group.aabb_min[k], pos_ptr[k]);
					out_group.aabb_max[k] = MAX(out_group.aabb_max[k], pos_ptr[k]);
				}
			}
			for (uint32_t j = 0; j < index_count; j++) {
				r_output.triangles.push_back(local_triangles[j]);
			}

			if (mesh.skin_influences > 0) {
				// Per-cluster bones and bind-space bounds (issue #159, steps 4-5): for
				// each bone, the AABB of this cluster's vertices with weight > 0 on it.
				// Linear blend skinning stays in the hull of the per-bone transforms,
				// so the union of (bone transform x AABB) bounds the skinned cluster.
				NextGeoClusterSkin &skin = r_output.cluster_skin[out_index];
				skin.bone_offset = r_output.skin_bones.size();
				skin.bone_count = uint8_t(cluster.bones.size());
				const uint32_t bounds_base = r_output.skin_bounds.size();
				for (uint16_t bone : cluster.bones) {
					r_output.skin_bones.push_back(bone);
					for (uint32_t k = 0; k < 3; k++) {
						r_output.skin_bounds.push_back(FLT_MAX);
					}
					for (uint32_t k = 0; k < 3; k++) {
						r_output.skin_bounds.push_back(-FLT_MAX);
					}
				}
				for (uint32_t v = 0; v < vertex_count; v++) {
					const uint32_t source = local_vertices[v];
					const float *position = apply_area_scale ? &scaled_pos_scratch[v * 3] : &mesh.positions[source * 3];
					const uint32_t base = source * mesh.skin_influences;
					for (uint32_t k = 0; k < mesh.skin_influences; k++) {
						const float weight = mesh.skin_weights[base + k];
						uint8_t local_bone = 0;
						if (weight > 0.0f) {
							const uint16_t bone = uint16_t(mesh.skin_bones[base + k]);
							uint32_t slot = cluster.bones.size();
							for (uint32_t b = 0; b < cluster.bones.size(); b++) {
								if (cluster.bones[b] == bone) {
									slot = b;
									break;
								}
							}
							ERR_FAIL_COND_V(slot >= cluster.bones.size(), ERR_BUG);
							local_bone = uint8_t(slot);
							float *aabb = &r_output.skin_bounds[bounds_base + slot * 6];
							for (uint32_t axis = 0; axis < 3; axis++) {
								aabb[axis] = MIN(aabb[axis], position[axis]);
								aabb[3 + axis] = MAX(aabb[3 + axis], position[axis]);
							}
						}
						r_output.skin_indices.push_back(local_bone);
						const float clamped = CLAMP(weight, 0.0f, 1.0f);
						r_output.skin_weights.push_back(uint16_t(clamped * 65535.0f + 0.5f));
					}
				}
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
