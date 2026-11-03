/**************************************************************************/
/*  nextgeo_cut.cpp                                                       */
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

#include "nextgeo_cut.h"

#include "core/math/math_funcs.h"
#include "core/templates/sort_array.h"

#include <cfloat>
#include <cmath>

double nextgeo_cut_pixel_scale(const NextGeoCutParams &p_params) {
	return 0.5 * p_params.viewport_height / std::tan(p_params.fov_y * 0.5) * p_params.lod_bias;
}

double nextgeo_projected_error(const float p_sphere[4], float p_error, const NextGeoCutParams &p_params, double p_pixel_scale) {
	if (p_error >= FLT_MAX) {
		return INFINITY;
	}
	const double s = p_params.instance_scale;
	const double dx = double(p_sphere[0]) * s - double(p_params.eye.x);
	const double dy = double(p_sphere[1]) * s - double(p_params.eye.y);
	const double dz = double(p_sphere[2]) * s - double(p_params.eye.z);
	const double distance = std::sqrt(dx * dx + dy * dy + dz * dz) - double(p_sphere[3]) * s;
	return double(p_error) * s / MAX(distance, p_params.znear) * p_pixel_scale;
}

void nextgeo_select_cut(const NextGeoBuildOutput &p_dag, const NextGeoCutParams &p_params, const uint8_t *p_page_resident, LocalVector<uint32_t> &r_clusters) {
	r_clusters.clear();
	const double t = p_params.threshold;
	const double pixel_scale = nextgeo_cut_pixel_scale(p_params);
	for (uint32_t i = 0; i < p_dag.clusters.size(); i++) {
		const NextGeoCluster &cluster = p_dag.clusters[i];
		if (p_page_resident && !p_page_resident[cluster.page_id]) {
			continue;
		}
		if (!(nextgeo_projected_error(cluster.parent_lod, cluster.parent_error, p_params, pixel_scale) > t)) {
			continue;
		}
		if (cluster.refined_group >= 0) {
			const uint32_t refined_page = p_dag.groups[cluster.refined_group].page_id;
			const bool refined_resident = !p_page_resident || p_page_resident[refined_page];
			if (refined_resident && !(nextgeo_projected_error(cluster.group_lod, cluster.group_error, p_params, pixel_scale) <= t)) {
				continue;
			}
		}
		r_clusters.push_back(i);
	}
}

namespace {

struct DepthOrder {
	uint32_t group;
	uint32_t depth;
};

struct DepthOrderCompare {
	// Coarsest first: the clusters a group outputs always sit in groups of greater depth.
	_FORCE_INLINE_ bool operator()(const DepthOrder &p_a, const DepthOrder &p_b) const {
		return p_a.depth != p_b.depth ? p_a.depth > p_b.depth : p_a.group < p_b.group;
	}
};

} // namespace

NextGeoCutCheck nextgeo_check_cut(const NextGeoBuildOutput &p_dag, const LocalVector<uint32_t> &p_clusters) {
	NextGeoCutCheck check;
	const uint32_t cluster_count = p_dag.clusters.size();
	const uint32_t group_count = p_dag.groups.size();

	LocalVector<uint32_t> rendered;
	rendered.resize_initialized(cluster_count);
	for (uint32_t cluster : p_clusters) {
		ERR_CONTINUE(cluster >= cluster_count);
		rendered[cluster]++;
	}

	// Coverage that the output clusters of each group give to the group's members.
	LocalVector<uint32_t> out_lo;
	LocalVector<uint32_t> out_hi;
	LocalVector<uint8_t> has_out;
	out_lo.resize(group_count);
	out_hi.resize_initialized(group_count);
	has_out.resize_initialized(group_count);
	for (uint32_t g = 0; g < group_count; g++) {
		out_lo[g] = UINT32_MAX;
	}

	LocalVector<DepthOrder> order;
	order.resize(group_count);
	for (uint32_t g = 0; g < group_count; g++) {
		order[g].group = g;
		order[g].depth = p_dag.groups[g].depth;
	}
	order.sort_custom<DepthOrderCompare>();

	for (const DepthOrder &entry : order) {
		const NextGeoGroup &group = p_dag.groups[entry.group];
		const uint32_t from_lo = has_out[entry.group] ? out_lo[entry.group] : 0;
		const uint32_t from_hi = has_out[entry.group] ? out_hi[entry.group] : 0;
		for (uint32_t i = group.cluster_offset; i < group.cluster_offset + group.cluster_count; i++) {
			const NextGeoCluster &cluster = p_dag.clusters[i];
			const uint32_t lo = rendered[i] + from_lo;
			const uint32_t hi = rendered[i] + from_hi;
			if (cluster.refined_group >= 0) {
				const uint32_t refined = uint32_t(cluster.refined_group);
				out_lo[refined] = MIN(out_lo[refined], lo);
				out_hi[refined] = MAX(out_hi[refined], hi);
				has_out[refined] = 1;
			} else {
				check.leaf_triangles += cluster.triangle_count;
				if (lo == 0) {
					check.uncovered_triangles += cluster.triangle_count;
				}
				if (hi >= 2) {
					check.overlapped_triangles += cluster.triangle_count;
				}
			}
		}
	}
	return check;
}
