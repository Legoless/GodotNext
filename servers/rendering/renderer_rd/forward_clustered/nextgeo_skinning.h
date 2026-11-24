/**************************************************************************/
/*  nextgeo_skinning.h                                                    */
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

#include "core/math/math_funcs.h"
#include "core/math/vector3.h"
#include "core/typedefs.h"

#include <cmath>
#include <cstring>

// GodotNext: nextgeo_skinned_runtime (#160, plan §21.4 contract N8, GN-5, GN-12).
//
// CPU reference for skinned Nanite bone-bound union AABBs, maximum bone scale for LOD error
// scaling, linear-blend skinning (4 or 8 influences matching .ngm page skinning blocks and
// Godot's 3xvec4 skeleton palette layout), and off-screen NextGI RT-range proxy refit checks.
// Mirrors shaders/nextgeo/skinning_inc.glsl and cluster_cull.glsl.
namespace RendererRD {

struct NextGeoClusterSkinBone {
	uint16_t global_bone = 0;
	Vector3 aabb_min;
	Vector3 aabb_max;
};

// Gershgorin upper bound on the spectral norm of a 3x4 bone transform's linear 3x3 part,
// stored as 3 row-major vec4s (12 floats, matching Godot's skeleton.glsl layout).
// Matches sw_raster_instance_scale and skinning_inc.glsl's nextgeo_skin_bone_scale().
inline float nextgeo_bone_matrix_scale(const float p_rows[12]) {
	const Vector3 c0(p_rows[0], p_rows[4], p_rows[8]);
	const Vector3 c1(p_rows[1], p_rows[5], p_rows[9]);
	const Vector3 c2(p_rows[2], p_rows[6], p_rows[10]);
	const float d01 = std::abs(float(c0.dot(c1)));
	const float d02 = std::abs(float(c0.dot(c2)));
	const float d12 = std::abs(float(c1.dot(c2)));
	const float g0 = float(c0.dot(c0)) + d01 + d02;
	const float g1 = float(c1.dot(c1)) + d01 + d12;
	const float g2 = float(c2.dot(c2)) + d02 + d12;
	const float m = MAX(g0, MAX(g1, g2));
	return (m > 0.0f && Math::is_finite(m)) ? std::sqrt(m) : 0.0f;
}

// Maximum bone scale across an entire skeleton palette (12 floats per bone).
// Returns 0.0f when every bone matrix is degenerate/zero (freshly allocated skeleton),
// allowing callers to distinguish an uninitialized zero buffer from a posed skeleton.
inline float nextgeo_palette_max_bone_scale(const float *p_bones, uint32_t p_bone_count) {
	if (p_bones == nullptr || p_bone_count == 0u) {
		return 0.0f;
	}
	float max_scale = 0.0f;
	for (uint32_t b = 0; b < p_bone_count; b++) {
		max_scale = MAX(max_scale, nextgeo_bone_matrix_scale(p_bones + b * 12u));
	}
	return max_scale;
}

// Arvo's exact AABB-by-affine transform for one bone matrix (3 row-major vec4s).
inline void nextgeo_transform_aabb_by_bone(const float p_rows[12], const Vector3 &p_min, const Vector3 &p_max, Vector3 &r_min, Vector3 &r_max) {
	const Vector3 c = (p_min + p_max) * 0.5f;
	const Vector3 e = (p_max - p_min) * 0.5f;
	const Vector3 tc(
			p_rows[0] * c.x + p_rows[1] * c.y + p_rows[2] * c.z + p_rows[3],
			p_rows[4] * c.x + p_rows[5] * c.y + p_rows[6] * c.z + p_rows[7],
			p_rows[8] * c.x + p_rows[9] * c.y + p_rows[10] * c.z + p_rows[11]);
	const Vector3 te(
			std::abs(p_rows[0]) * e.x + std::abs(p_rows[1]) * e.y + std::abs(p_rows[2]) * e.z,
			std::abs(p_rows[4]) * e.x + std::abs(p_rows[5]) * e.y + std::abs(p_rows[6]) * e.z,
			std::abs(p_rows[8]) * e.x + std::abs(p_rows[9]) * e.y + std::abs(p_rows[10]) * e.z);
	r_min = tc - te;
	r_max = tc + te;
}

// Computes the union AABB and maximum bone scale over a cluster's influencing bones.
// Because LBS weights are non-negative and sum to 1, every skinned vertex in the cluster
// lies inside the convex hull of its bone-transformed positions and therefore inside [r_min, r_max].
inline bool nextgeo_skin_cluster_bounds_from_bones(
		const NextGeoClusterSkinBone *p_bones,
		uint32_t p_cluster_bone_count,
		const float *p_palette,
		uint32_t p_palette_bone_count,
		Vector3 &r_min,
		Vector3 &r_max,
		float &r_max_bone_scale) {
	if (p_bones == nullptr || p_cluster_bone_count == 0u || p_palette == nullptr || p_palette_bone_count == 0u) {
		return false;
	}
	bool any_bone = false;
	Vector3 umin;
	Vector3 umax;
	float max_scale = 0.0f;
	for (uint32_t b = 0; b < p_cluster_bone_count; b++) {
		const uint32_t gb = p_bones[b].global_bone;
		if (gb >= p_palette_bone_count) {
			continue;
		}
		const float *rows = p_palette + gb * 12u;
		Vector3 tmin;
		Vector3 tmax;
		nextgeo_transform_aabb_by_bone(rows, p_bones[b].aabb_min, p_bones[b].aabb_max, tmin, tmax);
		if (!any_bone) {
			umin = tmin;
			umax = tmax;
			any_bone = true;
		} else {
			umin = umin.min(tmin);
			umax = umax.max(tmax);
		}
		max_scale = MAX(max_scale, nextgeo_bone_matrix_scale(rows));
	}
	if (!any_bone) {
		return false;
	}
	r_min = umin;
	r_max = umax;
	r_max_bone_scale = max_scale > 0.0f ? max_scale : 1.0f;
	return true;
}

// Parses a cluster's #159 skinning block directly from raw .ngm page bytes and computes its
// posed bone-bound union AABB and max bone scale.
inline bool nextgeo_skin_cluster_bounds_raw(
		const uint8_t *p_page_bytes,
		uint32_t p_page_size,
		uint32_t p_skin_byte_offset,
		const float *p_palette,
		uint32_t p_palette_bone_count,
		Vector3 &r_min,
		Vector3 &r_max,
		float &r_max_bone_scale) {
	if (p_page_bytes == nullptr || p_palette == nullptr || p_palette_bone_count == 0u || p_skin_byte_offset + 4u > p_page_size) {
		return false;
	}
	const uint32_t cluster_bone_count = p_page_bytes[p_skin_byte_offset + 0u];
	const uint32_t influences = p_page_bytes[p_skin_byte_offset + 1u];
	if (cluster_bone_count == 0u || (influences != 4u && influences != 8u)) {
		return false;
	}
	const uint64_t bones_end = uint64_t(p_skin_byte_offset) + 4ull + uint64_t(cluster_bone_count) * 26ull;
	if (bones_end > uint64_t(p_page_size)) {
		return false;
	}
	bool any_bone = false;
	Vector3 umin;
	Vector3 umax;
	float max_scale = 0.0f;
	for (uint32_t b = 0; b < cluster_bone_count; b++) {
		const uint8_t *entry = p_page_bytes + p_skin_byte_offset + 4u + b * 26u;
		uint16_t gb = 0;
		memcpy(&gb, entry, sizeof(uint16_t));
		if (uint32_t(gb) >= p_palette_bone_count) {
			continue;
		}
		float bb[6];
		memcpy(bb, entry + 2u, sizeof(bb));
		const float *rows = p_palette + uint32_t(gb) * 12u;
		Vector3 tmin;
		Vector3 tmax;
		nextgeo_transform_aabb_by_bone(rows, Vector3(bb[0], bb[1], bb[2]), Vector3(bb[3], bb[4], bb[5]), tmin, tmax);
		if (!any_bone) {
			umin = tmin;
			umax = tmax;
			any_bone = true;
		} else {
			umin = umin.min(tmin);
			umax = umax.max(tmax);
		}
		max_scale = MAX(max_scale, nextgeo_bone_matrix_scale(rows));
	}
	if (!any_bone) {
		return false;
	}
	r_min = umin;
	r_max = umax;
	r_max_bone_scale = max_scale > 0.0f ? max_scale : 1.0f;
	return true;
}

// CPU linear-blend skinning for one vertex of a #159 cluster skin block (4 or 8 influences).
inline bool nextgeo_skin_vertex_lbs_raw(
		const uint8_t *p_page_bytes,
		uint32_t p_page_size,
		uint32_t p_skin_byte_offset,
		uint32_t p_vert_count,
		uint32_t p_v_idx,
		const float *p_palette,
		uint32_t p_palette_bone_count,
		Vector3 &r_pos,
		Vector3 *r_normal = nullptr,
		Vector3 *r_tangent = nullptr) {
	if (p_page_bytes == nullptr || p_palette == nullptr || p_palette_bone_count == 0u || p_v_idx >= p_vert_count || p_skin_byte_offset + 4u > p_page_size) {
		return false;
	}
	const uint32_t cluster_bone_count = p_page_bytes[p_skin_byte_offset + 0u];
	const uint32_t influences = p_page_bytes[p_skin_byte_offset + 1u];
	if (cluster_bone_count == 0u || (influences != 4u && influences != 8u)) {
		return false;
	}
	const uint64_t idx_base = uint64_t(p_skin_byte_offset) + 4ull + uint64_t(cluster_bone_count) * 26ull;
	const uint64_t w_base = idx_base + uint64_t(p_vert_count) * uint64_t(influences);
	const uint64_t block_end = w_base + uint64_t(p_vert_count) * uint64_t(influences) * 2ull;
	if (block_end > uint64_t(p_page_size)) {
		return false;
	}
	float m[12] = {};
	float w_sum = 0.0f;
	const uint8_t *v_idx_ptr = p_page_bytes + idx_base + uint64_t(p_v_idx) * uint64_t(influences);
	const uint8_t *v_w_ptr = p_page_bytes + w_base + uint64_t(p_v_idx) * uint64_t(influences) * 2ull;
	for (uint32_t k = 0; k < influences; k++) {
		uint16_t wu = 0;
		memcpy(&wu, v_w_ptr + k * 2u, sizeof(uint16_t));
		if (wu == 0u) {
			continue;
		}
		const uint32_t local_bone = v_idx_ptr[k];
		if (local_bone >= cluster_bone_count) {
			continue;
		}
		uint16_t gb = 0;
		memcpy(&gb, p_page_bytes + p_skin_byte_offset + 4u + local_bone * 26u, sizeof(uint16_t));
		if (uint32_t(gb) >= p_palette_bone_count) {
			continue;
		}
		const float w = float(wu) * (1.0f / 65535.0f);
		const float *rows = p_palette + uint32_t(gb) * 12u;
		for (int i = 0; i < 12; i++) {
			m[i] += w * rows[i];
		}
		w_sum += w;
	}
	if (!(w_sum > 0.0f)) {
		return false;
	}
	const float inv_w = 1.0f / w_sum;
	for (int i = 0; i < 12; i++) {
		m[i] *= inv_w;
	}
	const Vector3 p = r_pos;
	r_pos = Vector3(
			m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3],
			m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
			m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]);
	if (r_normal != nullptr) {
		const Vector3 n = *r_normal;
		const Vector3 r0(m[0], m[1], m[2]);
		const Vector3 r1(m[4], m[5], m[6]);
		const Vector3 r2(m[8], m[9], m[10]);
		const Vector3 c0 = r1.cross(r2);
		const Vector3 c1 = r2.cross(r0);
		const Vector3 c2 = r0.cross(r1);
		const float det = float(r0.dot(c0));
		Vector3 sn;
		if (std::abs(det) > 1.0e-10f) {
			const float sgn = det < 0.0f ? -1.0f : 1.0f;
			sn = Vector3(float(c0.dot(n)), float(c1.dot(n)), float(c2.dot(n))) * sgn;
		} else {
			sn = Vector3(float(r0.dot(n)), float(r1.dot(n)), float(r2.dot(n)));
		}
		const float len = float(sn.length());
		*r_normal = len > 1e-12f ? (sn / len) : n;
	}
	if (r_tangent != nullptr) {
		const Vector3 t = *r_tangent;
		const Vector3 st(
				m[0] * t.x + m[1] * t.y + m[2] * t.z,
				m[4] * t.x + m[5] * t.y + m[6] * t.z,
				m[8] * t.x + m[9] * t.y + m[10] * t.z);
		const float len = float(st.length());
		*r_tangent = len > 1e-12f ? (st / len) : t;
	}
	return true;
}

// GodotNext: nextgeo_skinned_runtime (#160, plan §21.4 contract N8).
// Returns true when a skinned instance is within the NextGI RT refit range and must receive
// mesh_instance_check_for_update() even when frustum-culled / off-screen.
inline bool nextgeo_skinned_within_rt_refit_range(const Vector3 &p_camera_origin, const Vector3 &p_instance_origin, float p_rt_range) {
	if (!(p_rt_range > 0.0f) || !Math::is_finite(p_rt_range)) {
		return false;
	}
	return float(p_camera_origin.distance_squared_to(p_instance_origin)) <= p_rt_range * p_rt_range;
}

} // namespace RendererRD
