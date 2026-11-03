/**************************************************************************/
/*  nextgeo_cut.h                                                         */
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

#include "nextgeo_dag_builder.h"

#include "core/math/math_defs.h"
#include "core/math/vector3.h"
#include "core/templates/local_vector.h"

#include <cstdint>

// View parameters of the CPU cut reference (plan §7.3 and §6.3). Values are evaluated in
// double precision.
struct NextGeoCutParams {
	Vector3 eye; // Eye position in the instance's rotated frame, relative to its origin.
	double instance_scale = 1.0; // The instance's maximum scale.
	double znear = 0.05;
	double viewport_height = 1080.0; // Internal resolution, in pixels.
	double fov_y = Math::PI / 3.0; // Vertical field of view, in radians.
	double lod_bias = 1.0;
	double threshold = 1.0; // Maximum projected error, in pixels.
};

// Pixels per unit of error at unit distance: 0.5 * h / tan(fov_y / 2) * lod_bias.
double nextgeo_cut_pixel_scale(const NextGeoCutParams &p_params);

// Projected error in pixels of an LOD bound:
// err * s / max(|c * s - eye| - r * s, znear) * 0.5 * h / tan(fov_y / 2) * lod_bias,
// where `p_pixel_scale` is nextgeo_cut_pixel_scale(p_params).
// Returns +infinity for FLT_MAX (root) errors.
double nextgeo_projected_error(const float p_sphere[4], float p_error, const NextGeoCutParams &p_params, double p_pixel_scale);

// Selects the clusters to render. Cluster c is rendered iff
// resident(page(c)) && proj(parent_lod) > t && (refined_group < 0 || proj(group_lod) <= t || !resident(page(refined_group))).
// `p_page_resident` holds one byte per page (non-zero = resident); nullptr means every page is resident.
void nextgeo_select_cut(const NextGeoBuildOutput &p_dag, const NextGeoCutParams &p_params, const uint8_t *p_page_resident, LocalVector<uint32_t> &r_clusters);

struct NextGeoCutCheck {
	uint64_t leaf_triangles = 0; // Triangles in the source clusters.
	uint64_t uncovered_triangles = 0; // Source triangles that the cut may leave uncovered.
	uint64_t overlapped_triangles = 0; // Source triangles that the cut may cover twice or more.

	bool is_valid_partition() const { return uncovered_triangles == 0 && overlapped_triangles == 0; }
};

// Checks that a cut is a partition of the source surface. With
// lo/hi(c) = rendered(c) + min/max over the output clusters of c's group (0 at roots),
// a source cluster is uncovered if lo == 0 and overlapped if hi >= 2, weighted by its
// triangles. Clusters listed twice count twice.
NextGeoCutCheck nextgeo_check_cut(const NextGeoBuildOutput &p_dag, const LocalVector<uint32_t> &p_clusters);
