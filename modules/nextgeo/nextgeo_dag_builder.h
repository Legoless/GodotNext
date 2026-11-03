/**************************************************************************/
/*  nextgeo_dag_builder.h                                                 */
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

#include "nextgeo_bvh.h"

#include "core/error/error_list.h"
#include "core/math/color.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"

#include <cstdint>

// Cluster limits (plan §6.3): the visibility ID keeps 7 bits for the triangle.
static constexpr uint32_t NEXTGEO_MAX_CLUSTER_TRIANGLES = 128;
static constexpr uint32_t NEXTGEO_MAX_CLUSTER_VERTICES = 128;
// Target clusters per group handed to meshopt_partitionClusters().
static constexpr uint32_t NEXTGEO_GROUP_CLUSTERS = 8;

// Attributes in the merged vertex stream. Position, normal, tangent and UV0 are always
// present; UV2 and color are present when any eligible surface has them.
enum NextGeoVertexLayout : uint32_t {
	NEXTGEO_VERTEX_POSITION = 1u << 0, // 3 floats.
	NEXTGEO_VERTEX_NORMAL = 1u << 1, // 3 floats.
	NEXTGEO_VERTEX_TANGENT = 1u << 2, // 4 floats, w = binormal handedness.
	NEXTGEO_VERTEX_UV0 = 1u << 3, // 2 floats.
	NEXTGEO_VERTEX_UV2 = 1u << 4, // 2 floats.
	NEXTGEO_VERTEX_COLOR = 1u << 5, // 4 floats.
};

// Float offsets of the always-present attributes inside one output vertex.
static constexpr uint32_t NEXTGEO_VERTEX_OFFSET_POSITION = 0;
static constexpr uint32_t NEXTGEO_VERTEX_OFFSET_NORMAL = 3;
static constexpr uint32_t NEXTGEO_VERTEX_OFFSET_TANGENT = 6;
static constexpr uint32_t NEXTGEO_VERTEX_OFFSET_UV0 = 10;
// UV2 (if present) follows at 12, then color (if present).

// The 80-byte cluster record (plan §6.3). Frozen before #72 starts.
struct NextGeoCluster {
	// Culling.
	float bounds[4]; // Culling sphere of the cluster's own geometry (center xyz, radius).
	int8_t cone_axis[3]; // Normal cone axis, SNORM8 (x / 127).
	int8_t cone_cutoff; // Normal cone cutoff, SNORM8.
	// Own LOD bound: the parent_lod of `refined_group`, or this cluster's culling sphere
	// with error 0 for source clusters.
	float group_lod[4];
	float group_error;
	// LOD bound of the group this cluster belongs to; FLT_MAX error at roots.
	float parent_lod[4];
	float parent_error;
	int32_t refined_group; // Group whose simplification produced this cluster; -1 = source cluster.
	uint32_t page_id; // Page of the group this cluster belongs to.
	uint32_t vertex_offset; // First vertex in NextGeoBuildOutput::vertices (in vertices, not floats).
	uint32_t triangle_offset; // First triangle in NextGeoBuildOutput::triangles (in triangles, 3 bytes each).
	uint8_t vertex_count;
	uint8_t triangle_count;
	uint16_t surface_id; // Index of the source surface (material) in NextGeoBuildInput::surfaces.
};

static_assert(sizeof(NextGeoCluster) == 80, "NextGeoCluster must stay 80 bytes (plan §6.3).");

// One cluster group. Its clusters are contiguous in NextGeoBuildOutput::clusters and all
// carry this group's LOD bound as their parent_lod.
struct NextGeoGroup {
	uint32_t cluster_offset;
	uint32_t cluster_count;
	uint32_t page_id;
	uint32_t depth; // DAG level the group was formed at (0 = source clusters).
	float parent_lod[4]; // Encloses the members' group_lod spheres.
	float parent_error; // max(simplification error, members' group_error); FLT_MAX for root groups.
	float aabb_min[3]; // Bounds of the members' geometry.
	float aabb_max[3];
};

// One streaming page: whole groups only, at most NextGeoBuildInput::page_size bytes.
// Pages are ordered coarsest first, so a parent's page never comes after its child's.
struct NextGeoPage {
	uint32_t group_offset;
	uint32_t group_count;
	uint32_t cluster_offset;
	uint32_t cluster_count;
	uint32_t byte_size; // nextgeo_page_bytes() of this page: header, cluster records, vertices, triangles, padding.
};

struct NextGeoSurfaceInput {
	Vector<Vector3> positions;
	Vector<Vector3> normals; // Optional; empty or one per vertex.
	Vector<float> tangents; // Optional; empty or four per vertex (Mesh::ARRAY_TANGENT layout).
	Vector<Vector2> uv0; // Optional; empty or one per vertex.
	Vector<Vector2> uv2; // Optional; empty or one per vertex.
	Vector<Color> colors; // Optional; empty or one per vertex.
	Vector<int32_t> indices; // Triangle list. Empty = non-indexed (identical vertices are welded).
	// Ineligible surfaces stay legacy-only. They are not virtualized, but eligible vertices
	// that share a position with them are locked, so the border never opens.
	bool eligible = true;
};

struct NextGeoBuildInput {
	Vector<NextGeoSurfaceInput> surfaces;
	// Maximum bytes per page. Required (no default): #72's owner picks the provisional value
	// and records it in plan Appendix A (§22.10). It must hold at least one full cluster.
	uint32_t page_size = 0;
	// Run each DAG level on the WorkerThreadPool. The output is identical either way.
	bool use_threads = true;
};

struct NextGeoBuildOutput {
	uint32_t vertex_layout = 0; // NextGeoVertexLayout bits.
	uint32_t vertex_stride = 0; // Floats per output vertex.
	uint32_t page_size = 0;
	uint32_t root_page_count = 0; // Pages [0, root_page_count) hold the root groups and stay resident.
	uint32_t dag_depth = 0; // Number of DAG levels (max group depth + 1).
	uint32_t source_triangle_count = 0; // Triangles in the source clusters.
	uint32_t dropped_triangle_count = 0; // Degenerate input triangles that were skipped.

	LocalVector<NextGeoCluster> clusters; // Ordered by group.
	LocalVector<NextGeoGroup> groups; // Ordered by page.
	LocalVector<NextGeoPage> pages;
	LocalVector<NextGeoBVHNode> bvh; // Node 0 is the root.

	LocalVector<float> vertices; // vertex_stride floats per vertex, cluster-local runs.
	LocalVector<uint32_t> vertex_sources; // Per output vertex: its vertex index in the source surface.
	LocalVector<uint8_t> triangles; // Three cluster-local vertex indices per triangle.
};

// The raw `.ngm` page header (cluster count, vertex count, triangle count, reserved) and the
// alignment of each page. Both match the layout written by the `.ngm` reader and writer.
static constexpr uint32_t NEXTGEO_PAGE_HEADER_BYTES = 16;
static constexpr uint32_t NEXTGEO_PAGE_ALIGN = 16;

// Bytes one cluster contributes to a raw page: its 80-byte record plus its vertices and
// triangles. The page header and the alignment padding are added by nextgeo_page_bytes().
uint32_t nextgeo_cluster_payload_bytes(uint32_t p_vertex_count, uint32_t p_triangle_count, uint32_t p_vertex_stride);

// On-disk size of a raw page whose clusters contribute `p_cluster_payload_sum` bytes: the
// 16-byte page header, those records and their vertices and triangles, then padding to 16
// bytes. A build keeps this at most NextGeoBuildInput::page_size, so raw_size fits too.
uint32_t nextgeo_page_bytes(uint32_t p_cluster_payload_sum);

// Builds one cluster DAG over the eligible surfaces of a mesh (plan §7.1). The output is
// deterministic: two builds of the same input are byte-identical, threaded or not.
Error nextgeo_build_dag(const NextGeoBuildInput &p_input, NextGeoBuildOutput &r_output);
