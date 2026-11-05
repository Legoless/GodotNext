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
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,         */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "servers/rendering/renderer_rd/storage_rd/virtual_mesh_storage.h"

// GodotNext: nextgeo_gpu_cull (#76, plan §7.3)
//
// The CPU reference for the GPU cut predicates. The shaders use the same
// formulas; the doctests and the Q17-style cut comparison pin them.
namespace RendererRD {

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

// Instance-level cull predicates (plan §7.3 step 4).
inline bool nextgeo_instance_cullable(uint32_t p_flags, uint32_t p_layer_mask, uint32_t p_cull_mask) {
	return (p_flags & RendererVirtualMeshStorage::RECORD_VALID) != 0 &&
			(p_flags & RendererVirtualMeshStorage::RECORD_VISIBLE) != 0 &&
			(p_flags & RendererVirtualMeshStorage::RECORD_HAS_SKELETON) == 0 &&
			(p_flags & RendererVirtualMeshStorage::RECORD_HAS_INSTANCE_UNIFORMS) == 0 &&
			(p_layer_mask & p_cull_mask) != 0;
}

// Order-independent 64-bit cut hash over (record, cluster) pairs, so the GPU
// compaction can be compared with the CPU reference regardless of order.
inline uint64_t nextgeo_cut_pair_hash(uint32_t p_record, uint32_t p_cluster) {
	uint64_t h = (uint64_t(p_record) << 32) | uint64_t(p_cluster);
	h ^= h >> 33;
	h *= 0xff51afd7ed558ccdull;
	h ^= h >> 33;
	h *= 0xc4ceb9fe1a85ec53ull;
	h ^= h >> 33;
	return h;
}

inline uint64_t nextgeo_cut_hash(const LocalVector<uint64_t> &p_pairs) {
	uint64_t hash = 0;
	for (const uint64_t &pair : p_pairs) {
		hash ^= nextgeo_cut_pair_hash(uint32_t(pair >> 32), uint32_t(pair));
	}
	return hash;
}

} // namespace RendererRD
