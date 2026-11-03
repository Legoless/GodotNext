/**************************************************************************/
/*  mesh_sdf_builder.h                                                    */
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

#include "core/error/error_list.h"
#include "core/math/aabb.h"
#include "core/math/face3.h"
#include "core/templates/local_vector.h"
#include "core/variant/variant.h"

// Per-mesh signed distance field generation (GodotNext #184, plan §21.2 L2).
// The distance magnitude comes from MeshSDFBVH below, the median-split
// triangle BVH extracted from GPUParticlesCollisionSDF3D, which now shares
// this code. The sign follows the issue's back-face rule: 48
// Fibonacci-sphere rays are cast per voxel (through StaticRaycaster when
// Embree is available, TriangleMesh::intersect_ray otherwise) and the
// voxel is inside when more than 25% of the hits are back faces, provided
// the hits cover a majority of the directions. An open sheet covers at
// most half the direction sphere from any point, so it gains no false
// interior; on closed meshes the coverage condition changes nothing.
//
// The result is stored sparsely: 8x8x8 bricks of R8 values, allocated only
// near the surface, with a page table per mip. Page table entries are a
// brick index (>= 0), PAGE_FAR_OUTSIDE or PAGE_FAR_INSIDE. Brick values are
// the signed distance divided by that mip's band, clamped to [-1, 1] and
// mapped to [0, 255] (0 = -band, 255 = +band).

// Node of the shared SDF triangle BVH. Leaves are encoded in child indices
// with LEAF_BIT set and hold a face index in the low bits.
struct MeshSDFBVHNode {
	enum {
		LEAF_BIT = 1 << 30,
		LEAF_MASK = LEAF_BIT - 1
	};
	AABB bounds;
	uint32_t children[2] = {};
};

struct MeshSDFFacePos {
	Vector3 center;
	uint32_t index = 0;
};

// Median-split BVH over triangles optionally thickened along their normals,
// extracted from GPUParticlesCollisionSDF3D (#184). With a thickness, the
// query is the particle bake's signed distance to the thickened sheet; with
// thickness 0 it is the unsigned point-triangle distance MeshSDFBuilder
// uses. The face centroid is (v0 + v1 + v2) / 3 (the node's original code
// divided by 2).
class MeshSDFBVH {
public:
	static uint32_t create_bvh(LocalVector<MeshSDFBVHNode> &r_bvh_tree, MeshSDFFacePos *p_faces, uint32_t p_face_count, const Face3 *p_triangles, float p_thickness);
	static void find_closest_distance(const Vector3 &p_pos, const MeshSDFBVHNode *p_bvh, uint32_t p_bvh_cell, const Face3 *p_triangles, float p_thickness, float &r_closest_distance);
};

struct MeshSDFSettings {
	float voxels_per_meter = 10.0f;
	float resolution_scale = 1.0f;
	int max_voxels_per_axis = 128;
	int band_voxels = 4;
	int ray_count = 48;
	int mip_count = 3;
	// Surfaces rendered double-sided (BaseMaterial3D cull_mode disabled)
	// have no well-defined interior: the field stays unsigned and bricks
	// are only kept within one voxel of the surface.
	bool two_sided = false;
	// Use StaticRaycaster (Embree) for the sign rays when it is available,
	// falling back to TriangleMesh::intersect_ray. Tests clear this to
	// exercise the fallback path on machines that have Embree.
	bool prefer_static_raycaster = true;
};

struct MeshSDFData {
	enum Flags {
		FLAG_TWO_SIDED = 1 << 0,
		FLAG_UNSIGNED = 1 << 1,
	};

	enum PageEntry {
		PAGE_FAR_OUTSIDE = -1,
		PAGE_FAR_INSIDE = -2,
	};

	static constexpr int BRICK_SIZE = 8;
	static constexpr int BRICK_VOXELS = BRICK_SIZE * BRICK_SIZE * BRICK_SIZE;

	AABB bounds; // Grid bounds at mip 0 (origin at the first voxel corner).
	float voxel_size = 0.0f; // Mip 0 voxel size in meters.
	uint32_t flags = 0;
	Vector<Vector3i> mip_dimensions; // Voxel counts, one per mip.
	Vector<Vector3i> mip_brick_grid_dimensions; // Brick counts, one per mip.
	Vector<float> mip_bands; // Normalization band in meters, one per mip.
	Vector<PackedInt32Array> page_tables; // One entry per brick slot, per mip.
	Vector<PackedByteArray> brick_data; // BRICK_VOXELS bytes per brick, per mip.

	bool is_valid() const { return voxel_size > 0.0f && !mip_dimensions.is_empty(); }
	int get_mip_count() const { return mip_dimensions.size(); }
	float get_mip_voxel_size(int p_mip) const;
	int get_brick_count(int p_mip) const;
	int64_t get_data_size() const;
	// Signed distance stored at one voxel center. Out-of-range coordinates
	// clamp to the grid; unallocated bricks return +/- the mip band.
	float get_voxel_distance(int p_mip, const Vector3i &p_voxel) const;
	// Trilinear sample at a point in the same space as the source faces.
	float sample_distance(const Vector3 &p_position, int p_mip = 0) const;
};

class MeshSDFBuilder {
	struct ComputeJob;

	void _compute_sdf_z(uint32_t p_z, ComputeJob *p_job);
	static bool _is_inside_by_back_faces(const ComputeJob *p_job, const Vector3 &p_point);

public:
	// Builds the field for non-indexed triangle faces in mesh-local space.
	// Deterministic for identical input: fixed ray set, fixed traversal
	// order, and per-voxel results independent of the thread schedule.
	static Error build(const Vector<Face3> &p_faces, const MeshSDFSettings &p_settings, MeshSDFData &r_data);

	// The fixed Fibonacci-sphere directions used for the sign rays.
	static Vector<Vector3> get_ray_directions(int p_ray_count);
};
