/**************************************************************************/
/*  nextgeo_cull.h                                                        */
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

#include "core/math/plane.h"
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2i.h"
#include "core/math/vector4.h"
#include "servers/rendering/renderer_rd/storage_rd/virtual_mesh_storage.h"

// GodotNext: nextgeo_gpu_cull (#76, plan §7.3), nextgeo_shadow_cuts (#82, plan §7.5)
//
// The CPU reference for the GPU cut predicates. The shaders use the same
// formulas; the doctests and the Q17-style cut comparison pin them.
namespace RendererRD {

enum NextGeoCutFlags : uint32_t {
	NEXTGEO_CUT_FLAG_OCCLUSION = 1u << 0,
	NEXTGEO_CUT_FLAG_MAIN_VIEW = 1u << 1,
	NEXTGEO_CUT_FLAG_ORTHOGRAPHIC = 1u << 2,
	NEXTGEO_CUT_FLAG_DUAL_PARABOLOID = 1u << 3,
	NEXTGEO_CUT_FLAG_SHADOW_VIEW = 1u << 4,
};

// Projected error in pixels for a mesh-space sphere (plan §7.3 step 3).
//   d  = max(|M*c - cam| - r*s, z_near)
//   px = err * s * 0.5 * H * P[1][1] / d * lod_bias
// Orthographic views drop the division by d.
inline float nextgeo_projected_error(const Vector3 &p_center, float p_radius, const Transform3D &p_transform, const Vector3 &p_camera, float p_lod_bias, float p_internal_height, const Projection &p_projection, bool p_orthographic = false) {
	const Vector3 center = p_transform.xform(p_center);
	const Vector3 scale = p_transform.basis.get_scale();
	const float s = MAX(scale.x, MAX(scale.y, scale.z));
	const float z_near = MAX(0.001f, p_projection.get_z_near());
	const float d = MAX(center.distance_to(p_camera) - p_radius * s, z_near);
	const float half_height = 0.5f * p_internal_height * p_projection.columns[1][1];
	return p_orthographic ? p_radius * s * half_height * p_lod_bias : p_radius * s * half_height / d * p_lod_bias;
}

// A cut is accepted when the projected error is at most the pixel budget.
inline bool nextgeo_cut_passes(float p_projected_error, float p_max_pixel_error) {
	return p_projected_error <= p_max_pixel_error;
}

inline uint32_t nextgeo_dispatch_group_count(uint32_t p_invocation_count) {
	return (p_invocation_count + 63) / 64;
}

// Instance-level cull predicates (plan §7.3 step 4, §7.5).
inline bool nextgeo_instance_cullable(uint32_t p_flags, uint32_t p_layer_mask, uint32_t p_cull_mask) {
	return (p_flags & RendererVirtualMeshStorage::RECORD_VALID) != 0 &&
			(p_flags & RendererVirtualMeshStorage::RECORD_VISIBLE) != 0 &&
			(p_flags & RendererVirtualMeshStorage::RECORD_CAST_SHADOW_MASK) != RendererVirtualMeshStorage::RECORD_CAST_SHADOW_SHADOWS_ONLY &&
			(p_flags & RendererVirtualMeshStorage::RECORD_HAS_SKELETON) == 0 &&
			(p_flags & RendererVirtualMeshStorage::RECORD_HAS_INSTANCE_UNIFORMS) == 0 &&
			(p_layer_mask & p_cull_mask) != 0;
}

inline bool nextgeo_shadow_instance_cullable(uint32_t p_flags, uint32_t p_layer_mask, uint32_t p_cull_mask) {
	return (p_flags & RendererVirtualMeshStorage::RECORD_VALID) != 0 &&
			(p_flags & RendererVirtualMeshStorage::RECORD_VISIBLE) != 0 &&
			(p_flags & RendererVirtualMeshStorage::RECORD_CAST_SHADOW_MASK) != RendererVirtualMeshStorage::RECORD_CAST_SHADOW_OFF &&
			(p_flags & RendererVirtualMeshStorage::RECORD_HAS_SKELETON) == 0 &&
			(p_flags & RendererVirtualMeshStorage::RECORD_HAS_INSTANCE_UNIFORMS) == 0 &&
			(p_layer_mask & p_cull_mask) != 0;
}

// Convert a Godot outward-normal Plane (normal.dot(p) - d <= 0 inside) into
// NextGeo's inward frustum plane (dot(xyz, p) + w >= 0 inside).
inline Vector4 nextgeo_plane_to_frustum(const Plane &p_plane) {
	const float len = p_plane.normal.length();
	const float inv = len > 0.0f ? 1.0f / len : 0.0f;
	return Vector4(-p_plane.normal.x * inv, -p_plane.normal.y * inv, -p_plane.normal.z * inv, p_plane.d * inv);
}

// Inverse world-space texel size for an orthographic shadow split rendered into p_tile_size.
inline float nextgeo_ortho_inv_texel_size(const Size2i &p_tile_size, const Projection &p_projection) {
	const float inv_tx = 0.5f * float(MAX(1, p_tile_size.x)) * Math::abs(p_projection.columns[0][0]);
	const float inv_ty = 0.5f * float(MAX(1, p_tile_size.y)) * Math::abs(p_projection.columns[1][1]);
	return MIN(inv_tx, inv_ty);
}

// Projected error in shadow texels across orthographic directional splits,
// perspective spot / omni-cube faces, and dual-paraboloid hemispheres (plan §7.5).
inline float nextgeo_shadow_projected_error(const Vector3 &p_center, float p_sphere_radius, float p_error, const Transform3D &p_transform, const Vector3 &p_light_pos, float p_lod_bias, const Size2i &p_tile_size, const Projection &p_projection, uint32_t p_cut_flags, float p_z_near = 0.001f) {
	if (p_error >= 3.4e38f) {
		return 1e30f;
	}
	const Vector3 scale_v = p_transform.basis.get_scale();
	const float s = MAX(scale_v.x, MAX(scale_v.y, scale_v.z));
	if ((p_cut_flags & NEXTGEO_CUT_FLAG_ORTHOGRAPHIC) != 0) {
		return p_error * s * nextgeo_ortho_inv_texel_size(p_tile_size, p_projection) * p_lod_bias;
	}
	const Vector3 center = p_transform.xform(p_center);
	const float zn = MAX(0.001f, p_z_near);
	const float d = MAX(center.distance_to(p_light_pos) - p_sphere_radius * s, zn);
	const float h = float(MAX(1, p_tile_size.y));
	if ((p_cut_flags & NEXTGEO_CUT_FLAG_DUAL_PARABOLOID) != 0) {
		return p_error * s * 0.5f * h / d * p_lod_bias;
	}
	return p_error * s * 0.5f * h * Math::abs(p_projection.columns[1][1]) / d * p_lod_bias;
}

// Order-independent 64-bit cut hash over (record, cluster) pairs, so the GPU
// compaction can be compared with the CPU reference regardless of order.
// 32-bit mix (Chris Wellons' lowbias32). The shader builds the same hash from
// two 32-bit words; 64-bit integer types are not portable to the Metal path.
inline uint32_t nextgeo_cut_mix32(uint32_t p_value) {
	p_value ^= p_value >> 16;
	p_value *= 0x7feb352du;
	p_value ^= p_value >> 15;
	p_value *= 0x846ca68bu;
	p_value ^= p_value >> 16;
	return p_value;
}

inline uint64_t nextgeo_cut_pair_hash(uint32_t p_record, uint32_t p_cluster) {
	const uint32_t lo = nextgeo_cut_mix32(p_record ^ nextgeo_cut_mix32(p_cluster ^ 0x9e3779b9u));
	const uint32_t hi = nextgeo_cut_mix32(p_record ^ 0x85ebca6bu) ^ nextgeo_cut_mix32(p_cluster ^ 0xc2b2ae35u);
	return (uint64_t(hi) << 32) | uint64_t(lo);
}

inline uint64_t nextgeo_cut_hash(const LocalVector<uint64_t> &p_pairs) {
	uint64_t hash = 0;
	for (const uint64_t &pair : p_pairs) {
		hash ^= nextgeo_cut_pair_hash(uint32_t(pair >> 32), uint32_t(pair));
	}
	return hash;
}

} // namespace RendererRD
