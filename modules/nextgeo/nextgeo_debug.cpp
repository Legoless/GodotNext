/**************************************************************************/
/*  nextgeo_debug.cpp                                                     */
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

#include "nextgeo_debug.h"

#include "nextgeo_dag_builder.h"

#include "core/io/resource_format_ngm.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hash_map.h"

#include <thirdparty/meshoptimizer/meshoptimizer.h>

#include <cfloat>
#include <cmath>
#include <cstring>

namespace {

struct QuantizedPosKey {
	int32_t x = 0;
	int32_t y = 0;
	int32_t z = 0;

	bool operator==(const QuantizedPosKey &p_other) const {
		return x == p_other.x && y == p_other.y && z == p_other.z;
	}
};

struct QuantizedPosHasher {
	static uint32_t hash(const QuantizedPosKey &p_key) {
		uint32_t h = 2166136261u;
		h = (h ^ uint32_t(p_key.x)) * 16777619u;
		h = (h ^ uint32_t(p_key.y)) * 16777619u;
		h = (h ^ uint32_t(p_key.z)) * 16777619u;
		return h;
	}
};

static QuantizedPosKey _quantize_pos(const Vector3 &p_pos) {
	QuantizedPosKey k;
	k.x = int32_t(std::llround(double(p_pos.x) * 10000.0));
	k.y = int32_t(std::llround(double(p_pos.y) * 10000.0));
	k.z = int32_t(std::llround(double(p_pos.z) * 10000.0));
	return k;
}

static bool _extract_surface_input(const Ref<Mesh> &p_mesh, int p_surface, NextGeoSurfaceInput &r_out) {
	if (p_mesh->surface_get_primitive_type(p_surface) != Mesh::PRIMITIVE_TRIANGLES) {
		return false;
	}
	const Array arrays = p_mesh->surface_get_arrays(p_surface);
	if (arrays.size() != Mesh::ARRAY_MAX || arrays[Mesh::ARRAY_VERTEX].get_type() == Variant::NIL) {
		return false;
	}
	r_out.positions = arrays[Mesh::ARRAY_VERTEX];
	if (r_out.positions.is_empty()) {
		return false;
	}
	if (arrays[Mesh::ARRAY_NORMAL].get_type() != Variant::NIL) {
		r_out.normals = arrays[Mesh::ARRAY_NORMAL];
		if (r_out.normals.size() != r_out.positions.size()) {
			r_out.normals.clear();
		}
	}
	if (arrays[Mesh::ARRAY_TANGENT].get_type() != Variant::NIL) {
		r_out.tangents = arrays[Mesh::ARRAY_TANGENT];
		if (r_out.tangents.size() != r_out.positions.size() * 4) {
			r_out.tangents.clear();
		}
	}
	if (arrays[Mesh::ARRAY_TEX_UV].get_type() != Variant::NIL) {
		r_out.uv0 = arrays[Mesh::ARRAY_TEX_UV];
		if (r_out.uv0.size() != r_out.positions.size()) {
			r_out.uv0.clear();
		}
	}
	if (arrays[Mesh::ARRAY_TEX_UV2].get_type() != Variant::NIL) {
		r_out.uv2 = arrays[Mesh::ARRAY_TEX_UV2];
		if (r_out.uv2.size() != r_out.positions.size()) {
			r_out.uv2.clear();
		}
	}
	if (arrays[Mesh::ARRAY_COLOR].get_type() != Variant::NIL) {
		r_out.colors = arrays[Mesh::ARRAY_COLOR];
		if (r_out.colors.size() != r_out.positions.size()) {
			r_out.colors.clear();
		}
	}
	if (arrays[Mesh::ARRAY_INDEX].get_type() != Variant::NIL) {
		r_out.indices = arrays[Mesh::ARRAY_INDEX];
		if (r_out.indices.size() % 3 != 0) {
			return false;
		}
	} else if (r_out.positions.size() % 3 != 0) {
		return false;
	}
	r_out.eligible = true;
	return true;
}

} // namespace

void NextGeoDebug::_bind_methods() {
	ClassDB::bind_static_method("NextGeoDebug", D_METHOD("build_stats", "mesh"), &NextGeoDebug::build_stats);
	ClassDB::bind_static_method("NextGeoDebug", D_METHOD("export_clusters", "mesh"), &NextGeoDebug::export_clusters);
}

Dictionary NextGeoDebug::build_stats(const Ref<Mesh> &p_mesh) {
	Dictionary out;
	ERR_FAIL_COND_V(p_mesh.is_null(), out);

	NextGeoBuildInput input;
	input.page_size = NGM_PROVISIONAL_PAGE_SIZE;
	input.use_threads = true;

	// Track per-position surface ownership to measure material-seam vertex fraction.
	HashMap<QuantizedPosKey, int32_t, QuantizedPosHasher> pos_owner;
	uint64_t total_vertices = 0;

	const int surface_count = p_mesh->get_surface_count();
	for (int s = 0; s < surface_count; s++) {
		NextGeoSurfaceInput surf;
		if (!_extract_surface_input(p_mesh, s, surf)) {
			continue;
		}
		for (int v = 0; v < surf.positions.size(); v++) {
			const QuantizedPosKey key = _quantize_pos(surf.positions[v]);
			int32_t *existing = pos_owner.getptr(key);
			if (existing == nullptr) {
				pos_owner.insert(key, s);
			} else if (*existing != s) {
				*existing = -1; // Shared across >= 2 surfaces (material seam).
			}
		}
		total_vertices += uint64_t(surf.positions.size());
		input.surfaces.push_back(surf);
	}
	ERR_FAIL_COND_V(input.surfaces.is_empty(), out);

	uint64_t seam_vertices = 0;
	for (int s = 0; s < input.surfaces.size(); s++) {
		const Vector<Vector3> &positions = input.surfaces[s].positions;
		for (int v = 0; v < positions.size(); v++) {
			const int32_t *owner = pos_owner.getptr(_quantize_pos(positions[v]));
			if (owner != nullptr && *owner == -1) {
				seam_vertices++;
			}
		}
	}

	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	NextGeoBuildOutput dag;
	const Error err = nextgeo_build_dag(input, dag);
	const uint64_t t1 = OS::get_singleton()->get_ticks_usec();
	ERR_FAIL_COND_V(err != OK, out);

	uint64_t root_cut_triangles = 0;
	for (const NextGeoCluster &c : dag.clusters) {
		if (!std::isfinite(c.parent_error) || c.parent_error >= FLT_MAX * 0.5f) {
			root_cut_triangles += uint64_t(c.triangle_count);
		}
	}

	uint64_t terminal_groups = 0;
	for (const NextGeoGroup &g : dag.groups) {
		if (!std::isfinite(g.parent_error) || g.parent_error >= FLT_MAX * 0.5f) {
			terminal_groups++;
		}
	}

	out["source_triangles"] = int64_t(dag.source_triangle_count);
	out["root_cut_triangles"] = int64_t(root_cut_triangles);
	out["dag_depth"] = int64_t(dag.dag_depth);
	out["cluster_count"] = int64_t(dag.clusters.size());
	out["group_count"] = int64_t(dag.groups.size());
	out["terminal_groups"] = int64_t(terminal_groups);
	out["locked_vertex_fraction"] = total_vertices > 0 ? (double(seam_vertices) / double(total_vertices)) : 0.0;
	out["build_ms"] = double(t1 - t0) / 1000.0;
	out["surface_count"] = int64_t(input.surfaces.size());
	return out;
}

PackedByteArray NextGeoDebug::export_clusters(const Ref<Mesh> &p_mesh) {
	PackedByteArray out;
	ERR_FAIL_COND_V(p_mesh.is_null(), out);

	// Binary layout (little-endian):
	//   Header (16 B): uint32 magic ('NGCL' = 0x4C43474E), cluster_count, vertex_count, triangle_count.
	//   Cluster records (cluster_count * 48 B):
	//     float bounds[4] (center xyz, radius)
	//     float cone[4]   (cone_axis xyz, cone_cutoff)
	//     uint32 counts[4] (vertex_offset, triangle_offset, vertex_count, triangle_count)
	//   Vertices (vertex_count * 16 B): float pos[4] (x, y, z, 0)
	//   Triangles (triangle_count * 4 B): uint32 packed = v0 | (v1 << 8) | (v2 << 16)
	struct ExportCluster {
		float bounds[4];
		float cone[4];
		uint32_t vertex_offset;
		uint32_t triangle_offset;
		uint32_t vertex_count;
		uint32_t triangle_count;
	};
	static_assert(sizeof(ExportCluster) == 48, "ExportCluster must be 48 bytes.");

	LocalVector<ExportCluster> clusters;
	LocalVector<float> vertices; // 4 floats per vertex (vec4).
	LocalVector<uint32_t> triangles; // 1 uint32 per triangle.

	const int surface_count = p_mesh->get_surface_count();
	for (int s = 0; s < surface_count; s++) {
		NextGeoSurfaceInput surf;
		if (!_extract_surface_input(p_mesh, s, surf)) {
			continue;
		}
		const uint32_t vcount = uint32_t(surf.positions.size());
		LocalVector<float> pos_f32;
		pos_f32.resize(vcount * 3u);
		for (uint32_t v = 0; v < vcount; v++) {
			pos_f32[v * 3u + 0] = surf.positions[v].x;
			pos_f32[v * 3u + 1] = surf.positions[v].y;
			pos_f32[v * 3u + 2] = surf.positions[v].z;
		}
		LocalVector<uint32_t> idx;
		if (!surf.indices.is_empty()) {
			idx.resize(surf.indices.size());
			for (int i = 0; i < surf.indices.size(); i++) {
				idx[i] = uint32_t(surf.indices[i]);
			}
		} else {
			idx.resize(vcount);
			for (uint32_t i = 0; i < vcount; i++) {
				idx[i] = i;
			}
		}
		if (idx.size() < 3) {
			continue;
		}

		const size_t max_meshlets = meshopt_buildMeshletsBound(idx.size(), NEXTGEO_MAX_CLUSTER_VERTICES, NEXTGEO_MAX_CLUSTER_TRIANGLES);
		LocalVector<meshopt_Meshlet> meshlets;
		LocalVector<uint32_t> meshlet_vertices;
		LocalVector<uint8_t> meshlet_triangles;
		meshlets.resize(uint32_t(max_meshlets));
		meshlet_vertices.resize(uint32_t(max_meshlets * NEXTGEO_MAX_CLUSTER_VERTICES));
		meshlet_triangles.resize(uint32_t(max_meshlets * NEXTGEO_MAX_CLUSTER_TRIANGLES * 3u));

		const size_t meshlet_count = meshopt_buildMeshlets(
				meshlets.ptr(),
				meshlet_vertices.ptr(),
				meshlet_triangles.ptr(),
				idx.ptr(),
				idx.size(),
				pos_f32.ptr(),
				vcount,
				sizeof(float) * 3u,
				NEXTGEO_MAX_CLUSTER_VERTICES,
				NEXTGEO_MAX_CLUSTER_TRIANGLES,
				0.25f);

		for (size_t m = 0; m < meshlet_count; m++) {
			const meshopt_Meshlet &ml = meshlets[m];
			if (ml.triangle_count == 0 || ml.vertex_count == 0) {
				continue;
			}
			const meshopt_Bounds b = meshopt_computeMeshletBounds(
					&meshlet_vertices[ml.vertex_offset],
					&meshlet_triangles[ml.triangle_offset],
					ml.triangle_count,
					pos_f32.ptr(),
					vcount,
					sizeof(float) * 3u);

			ExportCluster ec;
			ec.bounds[0] = b.center[0];
			ec.bounds[1] = b.center[1];
			ec.bounds[2] = b.center[2];
			ec.bounds[3] = b.radius;
			ec.cone[0] = b.cone_axis[0];
			ec.cone[1] = b.cone_axis[1];
			ec.cone[2] = b.cone_axis[2];
			ec.cone[3] = b.cone_cutoff;
			ec.vertex_offset = vertices.size() / 4u;
			ec.triangle_offset = triangles.size();
			ec.vertex_count = ml.vertex_count;
			ec.triangle_count = ml.triangle_count;
			clusters.push_back(ec);

			for (uint32_t v = 0; v < ml.vertex_count; v++) {
				const uint32_t src_v = meshlet_vertices[ml.vertex_offset + v];
				vertices.push_back(pos_f32[src_v * 3u + 0]);
				vertices.push_back(pos_f32[src_v * 3u + 1]);
				vertices.push_back(pos_f32[src_v * 3u + 2]);
				vertices.push_back(0.0f);
			}
			for (uint32_t t = 0; t < ml.triangle_count; t++) {
				const uint32_t i0 = meshlet_triangles[ml.triangle_offset + t * 3u + 0];
				const uint32_t i1 = meshlet_triangles[ml.triangle_offset + t * 3u + 1];
				const uint32_t i2 = meshlet_triangles[ml.triangle_offset + t * 3u + 2];
				triangles.push_back(i0 | (i1 << 8) | (i2 << 16));
			}
		}
	}

	const uint32_t header[4] = {
		0x4C43474Eu, // 'NGCL'
		clusters.size(),
		vertices.size() / 4u,
		triangles.size(),
	};
	const size_t clusters_bytes = size_t(clusters.size()) * sizeof(ExportCluster);
	const size_t vertices_bytes = size_t(vertices.size()) * sizeof(float);
	const size_t triangles_bytes = size_t(triangles.size()) * sizeof(uint32_t);
	const size_t total_bytes = sizeof(header) + clusters_bytes + vertices_bytes + triangles_bytes;

	out.resize(int64_t(total_bytes));
	uint8_t *dst = out.ptrw();
	memcpy(dst, header, sizeof(header));
	dst += sizeof(header);
	if (clusters_bytes > 0) {
		memcpy(dst, clusters.ptr(), clusters_bytes);
		dst += clusters_bytes;
	}
	if (vertices_bytes > 0) {
		memcpy(dst, vertices.ptr(), vertices_bytes);
		dst += vertices_bytes;
	}
	if (triangles_bytes > 0) {
		memcpy(dst, triangles.ptr(), triangles_bytes);
	}
	return out;
}
