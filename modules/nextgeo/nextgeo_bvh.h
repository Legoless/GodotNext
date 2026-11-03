/**************************************************************************/
/*  nextgeo_bvh.h                                                         */
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

#include "core/templates/local_vector.h"

#include <cstdint>

struct NextGeoGroup;

// A child index with this bit set refers to a cluster group (a leaf); otherwise it
// refers to another node of the same BVH.
static constexpr uint32_t NEXTGEO_BVH_LEAF_BIT = 0x80000000u;
static constexpr uint32_t NEXTGEO_BVH_WIDTH = 8;
static constexpr uint32_t NEXTGEO_BVH_INVALID_CHILD = 0xFFFFFFFFu;

// One node of the offline BVH8 over cluster groups (plan §7.1). Node 0 is the root.
struct NextGeoBVHNode {
	float lod_sphere[4]; // Encloses the LOD spheres of every child (center xyz, radius).
	float max_parent_error; // Maximum parent error of the subtree (FLT_MAX if it holds a root group).
	float aabb_min[3]; // Encloses the geometry of every child.
	float aabb_max[3];
	uint32_t child_count;
	uint32_t children[NEXTGEO_BVH_WIDTH]; // Unused slots hold NEXTGEO_BVH_INVALID_CHILD.
};

static_assert(sizeof(NextGeoBVHNode) == 80, "NextGeoBVHNode must stay 80 bytes.");

// Computes a sphere that encloses `p_count` spheres. Each sphere is four floats (center
// xyz, radius) at the start of an element `p_stride` bytes long. The center comes from
// `meshopt_computeSphereBounds()`; the radius is then widened in double precision and
// rounded up, so the result encloses every input exactly. This keeps parent LOD spheres
// strictly enclosing their children, which the projected-error monotonicity relies on.
void nextgeo_enclose_spheres(const float *p_spheres, uint32_t p_count, uint32_t p_stride, float r_sphere[4]);

// Builds the BVH8 over `p_groups`: groups are sorted by the Morton code of their LOD
// sphere center and split 8 ways. Nodes merge their children's LOD spheres and AABBs
// and take the maximum parent error. Node 0 is the root; the result is empty when
// there are no groups.
void nextgeo_build_bvh(const NextGeoGroup *p_groups, uint32_t p_group_count, LocalVector<NextGeoBVHNode> &r_nodes);
