/**************************************************************************/
/*  nextgeo_bvh.cpp                                                       */
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

#include "nextgeo_bvh.h"

#include "nextgeo_dag_builder.h"

#include "core/math/math_funcs.h"
#include "core/templates/sort_array.h"

#include <thirdparty/meshoptimizer/meshoptimizer.h>

#include <cfloat>
#include <cmath>

void nextgeo_enclose_spheres(const float *p_spheres, uint32_t p_count, uint32_t p_stride, float r_sphere[4]) {
	if (p_count == 0) {
		r_sphere[0] = r_sphere[1] = r_sphere[2] = r_sphere[3] = 0.0f;
		return;
	}

	const meshopt_Bounds merged = meshopt_computeSphereBounds(p_spheres, p_count, p_stride, p_spheres + 3, p_stride);

	// The center is taken as is; the radius is recomputed so that every input sphere is
	// enclosed exactly, whatever rounding the merge did.
	double radius = 0.0;
	const uint8_t *base = reinterpret_cast<const uint8_t *>(p_spheres);
	for (uint32_t i = 0; i < p_count; i++) {
		const float *sphere = reinterpret_cast<const float *>(base + size_t(i) * p_stride);
		const double dx = double(sphere[0]) - double(merged.center[0]);
		const double dy = double(sphere[1]) - double(merged.center[1]);
		const double dz = double(sphere[2]) - double(merged.center[2]);
		radius = MAX(radius, std::sqrt(dx * dx + dy * dy + dz * dz) + double(sphere[3]));
	}
	// A small relative margin keeps the enclosure strict when it is evaluated in floating point.
	radius *= 1.0 + 1e-6;

	float radius_f = float(radius);
	if (double(radius_f) < radius) {
		radius_f = std::nextafter(radius_f, FLT_MAX);
	}

	r_sphere[0] = merged.center[0];
	r_sphere[1] = merged.center[1];
	r_sphere[2] = merged.center[2];
	r_sphere[3] = radius_f;
}

namespace {

struct MortonItem {
	uint32_t code;
	uint32_t group;
};

struct MortonItemCompare {
	_FORCE_INLINE_ bool operator()(const MortonItem &p_a, const MortonItem &p_b) const {
		return p_a.code != p_b.code ? p_a.code < p_b.code : p_a.group < p_b.group;
	}
};

uint32_t _expand_bits_10(uint32_t p_value) {
	uint32_t v = p_value & 0x3FFu;
	v = (v | (v << 16)) & 0x030000FFu;
	v = (v | (v << 8)) & 0x0300F00Fu;
	v = (v | (v << 4)) & 0x030C30C3u;
	v = (v | (v << 2)) & 0x09249249u;
	return v;
}

uint32_t _build_range(const NextGeoGroup *p_groups, const MortonItem *p_items, uint32_t p_begin, uint32_t p_end, LocalVector<NextGeoBVHNode> &r_nodes) {
	const uint32_t index = r_nodes.size();
	{
		NextGeoBVHNode empty = {};
		for (uint32_t k = 0; k < NEXTGEO_BVH_WIDTH; k++) {
			empty.children[k] = NEXTGEO_BVH_INVALID_CHILD;
		}
		r_nodes.push_back(empty);
	}

	// Each child takes the largest power of 8 that still leaves at most 8 children, which
	// fills nodes completely except at the right edge.
	const uint32_t count = p_end - p_begin;
	uint32_t span = 1;
	while (span * NEXTGEO_BVH_WIDTH < count) {
		span *= NEXTGEO_BVH_WIDTH;
	}

	uint32_t children[NEXTGEO_BVH_WIDTH];
	float spheres[NEXTGEO_BVH_WIDTH][4];
	float max_error = 0.0f;
	float aabb_min[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
	float aabb_max[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	uint32_t child_count = 0;

	for (uint32_t begin = p_begin; begin < p_end; begin += span) {
		const uint32_t end = MIN(p_end, begin + span);
		const float *sphere = nullptr;
		const float *child_min = nullptr;
		const float *child_max = nullptr;
		float error = 0.0f;
		if (end - begin == 1) {
			const NextGeoGroup &group = p_groups[p_items[begin].group];
			children[child_count] = p_items[begin].group | NEXTGEO_BVH_LEAF_BIT;
			sphere = group.parent_lod;
			error = group.parent_error;
			child_min = group.aabb_min;
			child_max = group.aabb_max;
		} else {
			const uint32_t child = _build_range(p_groups, p_items, begin, end, r_nodes);
			const NextGeoBVHNode &child_node = r_nodes[child];
			children[child_count] = child;
			sphere = child_node.lod_sphere;
			error = child_node.max_parent_error;
			child_min = child_node.aabb_min;
			child_max = child_node.aabb_max;
		}
		for (uint32_t k = 0; k < 4; k++) {
			spheres[child_count][k] = sphere[k];
		}
		max_error = MAX(max_error, error);
		for (uint32_t k = 0; k < 3; k++) {
			aabb_min[k] = MIN(aabb_min[k], child_min[k]);
			aabb_max[k] = MAX(aabb_max[k], child_max[k]);
		}
		child_count++;
	}

	NextGeoBVHNode &node = r_nodes[index];
	nextgeo_enclose_spheres(&spheres[0][0], child_count, sizeof(float) * 4, node.lod_sphere);
	node.max_parent_error = max_error;
	for (uint32_t k = 0; k < 3; k++) {
		node.aabb_min[k] = aabb_min[k];
		node.aabb_max[k] = aabb_max[k];
	}
	node.child_count = child_count;
	for (uint32_t k = 0; k < child_count; k++) {
		node.children[k] = children[k];
	}
	return index;
}

} // namespace

void nextgeo_build_bvh(const NextGeoGroup *p_groups, uint32_t p_group_count, LocalVector<NextGeoBVHNode> &r_nodes) {
	r_nodes.clear();
	if (p_group_count == 0) {
		return;
	}

	float center_min[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
	float center_max[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	for (uint32_t i = 0; i < p_group_count; i++) {
		for (uint32_t k = 0; k < 3; k++) {
			center_min[k] = MIN(center_min[k], p_groups[i].parent_lod[k]);
			center_max[k] = MAX(center_max[k], p_groups[i].parent_lod[k]);
		}
	}

	LocalVector<MortonItem> items;
	items.resize(p_group_count);
	for (uint32_t i = 0; i < p_group_count; i++) {
		uint32_t code = 0;
		for (uint32_t k = 0; k < 3; k++) {
			const float extent = center_max[k] - center_min[k];
			const float t = extent > 0.0f ? (p_groups[i].parent_lod[k] - center_min[k]) / extent : 0.0f;
			const uint32_t q = uint32_t(CLAMP(t * 1023.0f + 0.5f, 0.0f, 1023.0f));
			code |= _expand_bits_10(q) << (2 - k);
		}
		items[i].code = code;
		items[i].group = i;
	}
	items.sort_custom<MortonItemCompare>();

	r_nodes.reserve(p_group_count / (NEXTGEO_BVH_WIDTH - 1) + 1);
	_build_range(p_groups, items.ptr(), 0, p_group_count, r_nodes);
}
