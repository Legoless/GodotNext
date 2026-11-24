/**************************************************************************/
/*  nextgeo_voxels.h                                                      */
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
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_visibility.h"

#include <cmath>
#include <cstdint>

// GodotNext: nextgeo_voxels (#165, plan §21.4 contract N10, GN-7, GN-12, N6, MN3).
//
// Pure C++ reference and shared GPU buffer layouts for experimental Nanite Voxels:
//   - Sparse 4^3 (64-voxel) brick payloads stored inside each group's `.ngm` page payload so #78
//     page residency and streaming rules remain completely unchanged.
//   - Each occupied voxel stores: surface ID (8-bit, < 32 for virtual surfaces), mean UV (fp16x2),
//     oct16 normal (2 x snorm8), and 8-bit coverage (1..255).
//   - Provisional thresholds (documented for PQ measurement):
//       * Offline group error ratio: NEXTGEO_VOXEL_PROVISIONAL_ERROR_RATIO
//       * Runtime brick projected screen size V: NEXTGEO_VOXEL_PROVISIONAL_SCREEN_THRESHOLD_PX (8 px -> <= 2 px/voxel)
//   - SW-only compute splat into vis64 using reserved 25-bit visibility cluster range
//     [NEXTGEO_VIS_VOXEL_BASE .. NEXTGEO_VIS_VOXEL_BASE + NEXTGEO_VIS_VOXEL_MAX_COUNT - 1]
//     with 6-bit voxel index [0..63] in the 7-bit element slot.
//   - Deterministic frame-stable stochastic coverage hash (zero static-camera flicker).
// Keep in sync with shaders/nextgeo/voxel_inc.glsl, voxel_raster.glsl, and vsm_raster_voxel.glsl.
namespace RendererRD {

static constexpr bool NEXTGEO_VOXEL_DEFAULT_ENABLED = false;

static constexpr uint32_t NEXTGEO_VOXEL_BRICK_DIM = 4u;
static constexpr uint32_t NEXTGEO_VOXEL_BRICK_VOXELS = NEXTGEO_VOXEL_BRICK_DIM * NEXTGEO_VOXEL_BRICK_DIM * NEXTGEO_VOXEL_BRICK_DIM; // 64
static constexpr uint32_t NEXTGEO_VOXEL_BRICK_HEADER_WORDS = 8u; // 32 bytes
static constexpr uint32_t NEXTGEO_VOXEL_DATA_WORDS = 2u; // 8 bytes per occupied voxel
static constexpr uint32_t NEXTGEO_VOXEL_SECTION_MAGIC = 0x5856474Eu; // 'NGVX'

// Provisional thresholds until PQ measures quality/memory tradeoffs across foliage scenes.
static constexpr float NEXTGEO_VOXEL_PROVISIONAL_ERROR_RATIO = 0.005f;
static constexpr float NEXTGEO_VOXEL_TERMINAL_ERROR_FRACTION = 0.04f;
static constexpr float NEXTGEO_VOXEL_ERROR_TO_SIZE_SCALE = 1.0f;
static constexpr float NEXTGEO_VOXEL_PROVISIONAL_SCREEN_THRESHOLD_PX = 8.0f;

// Runtime visible-brick list capacities (allocated lazily ONLY when rendering/nextgeo/experimental/voxels == true).
static constexpr uint32_t NEXTGEO_VOXEL_BRICK_CAPACITY = 1u << 19u; // 524,288 bricks (262,144 per phase; 8 MiB)
static constexpr uint32_t NEXTGEO_VSM_VOXEL_BRICK_CAPACITY = 1u << 18u; // 262,144 VSM (brick, page) pairs (4 MiB)

static_assert(NEXTGEO_VOXEL_BRICK_CAPACITY <= NEXTGEO_VIS_VOXEL_MAX_COUNT, "Visible voxel brick capacity must fit inside the reserved voxel visibility ID range.");
static_assert(NEXTGEO_VOXEL_BRICK_VOXELS == 64u, "Voxel brick must be 4^3 = 64 voxels to fit in 6-bit visibility element slot.");

// 16-byte compact visible voxel brick record (uvec4, std430).
// Main view: x = record_idx, y = brick_word_offset (in global page_buffer), z = group_idx, w = brick_meta.
// VSM view:  x = record_idx, y = brick_word_offset (in global page_buffer), z = dirty_idx, w = (layer & 1) | (meta << 8).
struct NextGeoVisibleVoxelBrick {
	uint32_t record_idx = 0u;
	uint32_t brick_word_offset = 0u;
	uint32_t group_or_dirty = 0u;
	uint32_t flags_and_meta = 0u;
};
static_assert(sizeof(NextGeoVisibleVoxelBrick) == 16, "NextGeoVisibleVoxelBrick must match GLSL uvec4 layout (16 bytes).");

// Brick meta word bit layout (brick header word 6):
//   bits 0..7   = occupied_count (1..64)
//   bits 8..23  = dominant_surface_id
//   bit  24     = NEXTGEO_VOXEL_BRICK_FLAG_UNIFORM_SURFACE (all occupied voxels share dominant_surface_id)
static constexpr uint32_t NEXTGEO_VOXEL_BRICK_FLAG_UNIFORM_SURFACE = 1u << 24u;

inline uint32_t nextgeo_voxel_pack_brick_meta(uint32_t p_occupied_count, uint32_t p_dominant_surface, bool p_uniform_surface) {
	return (p_occupied_count & 0xFFu) | ((p_dominant_surface & 0xFFFFu) << 8u) | (p_uniform_surface ? NEXTGEO_VOXEL_BRICK_FLAG_UNIFORM_SURFACE : 0u);
}

inline uint32_t nextgeo_voxel_popcount32(uint32_t p_v) {
	p_v = p_v - ((p_v >> 1u) & 0x55555555u);
	p_v = (p_v & 0x33333333u) + ((p_v >> 2u) & 0x33333333u);
	return (((p_v + (p_v >> 4u)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24u;
}

inline uint32_t nextgeo_voxel_rank(uint32_t p_occ_lo, uint32_t p_occ_hi, uint32_t p_voxel_idx) {
	if (p_voxel_idx < 32u) {
		const uint32_t mask = (1u << p_voxel_idx) - 1u;
		return nextgeo_voxel_popcount32(p_occ_lo & mask);
	}
	const uint32_t mask = (1u << (p_voxel_idx - 32u)) - 1u;
	return nextgeo_voxel_popcount32(p_occ_lo) + nextgeo_voxel_popcount32(p_occ_hi & mask);
}

inline bool nextgeo_voxel_is_occupied(uint32_t p_occ_lo, uint32_t p_occ_hi, uint32_t p_voxel_idx) {
	if (p_voxel_idx >= NEXTGEO_VOXEL_BRICK_VOXELS) {
		return false;
	}
	const uint32_t word = p_voxel_idx < 32u ? p_occ_lo : p_occ_hi;
	return ((word >> (p_voxel_idx & 31u)) & 1u) != 0u;
}

// Signed octahedral 16-bit normal encoding (2 x snorm8 in [-127, 127]).
// Exact round-trip on all 6 cardinal axes and < 0.6 deg max error across S^2.
inline uint16_t nextgeo_voxel_encode_oct16(const Vector3 &p_normal) {
	Vector3 n = p_normal;
	const float len = float(n.length());
	if (!(len > 1.0e-8f) || !std::isfinite(len)) {
		n = Vector3(0.0f, 1.0f, 0.0f);
	} else {
		n /= len;
	}
	const float inv_l1 = 1.0f / MAX(Math::abs(float(n.x)) + Math::abs(float(n.y)) + Math::abs(float(n.z)), 1.0e-8f);
	float ox = float(n.x) * inv_l1;
	float oy = float(n.y) * inv_l1;
	if (float(n.z) < 0.0f) {
		const float rx = (1.0f - Math::abs(oy)) * (ox >= 0.0f ? 1.0f : -1.0f);
		const float ry = (1.0f - Math::abs(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
		ox = rx;
		oy = ry;
	}
	const int8_t qx = int8_t(CLAMP(int(std::lround(double(CLAMP(ox, -1.0f, 1.0f)) * 127.0)), -127, 127));
	const int8_t qy = int8_t(CLAMP(int(std::lround(double(CLAMP(oy, -1.0f, 1.0f)) * 127.0)), -127, 127));
	return uint16_t(uint8_t(qx)) | (uint16_t(uint8_t(qy)) << 8u);
}

inline Vector3 nextgeo_voxel_decode_oct16(uint16_t p_oct16) {
	const int8_t qx = int8_t(p_oct16 & 0xFFu);
	const int8_t qy = int8_t((p_oct16 >> 8u) & 0xFFu);
	float ox = CLAMP(float(qx) / 127.0f, -1.0f, 1.0f);
	float oy = CLAMP(float(qy) / 127.0f, -1.0f, 1.0f);
	const float oz = 1.0f - Math::abs(ox) - Math::abs(oy);
	if (oz < 0.0f) {
		const float rx = (1.0f - Math::abs(oy)) * (ox >= 0.0f ? 1.0f : -1.0f);
		const float ry = (1.0f - Math::abs(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
		ox = rx;
		oy = ry;
	}
	Vector3 n(ox, oy, oz);
	const float len = float(n.length());
	return (len > 1.0e-8f) ? (n / len) : Vector3(0.0f, 1.0f, 0.0f);
}

inline uint32_t nextgeo_voxel_pack_uv_half2(const Vector2 &p_uv) {
	const float u = std::isfinite(float(p_uv.x)) ? CLAMP(float(p_uv.x), -65504.0f, 65504.0f) : 0.0f;
	const float v = std::isfinite(float(p_uv.y)) ? CLAMP(float(p_uv.y), -65504.0f, 65504.0f) : 0.0f;
	return uint32_t(Math::make_half_float(u)) | (uint32_t(Math::make_half_float(v)) << 16u);
}

inline Vector2 nextgeo_voxel_unpack_uv_half2(uint32_t p_packed) {
	return Vector2(Math::half_to_float(uint16_t(p_packed & 0xFFFFu)), Math::half_to_float(uint16_t((p_packed >> 16u) & 0xFFFFu)));
}

inline uint32_t nextgeo_voxel_pack_word1(uint16_t p_oct16, uint8_t p_coverage, uint8_t p_surface_id) {
	return uint32_t(p_oct16) | (uint32_t(p_coverage) << 16u) | (uint32_t(p_surface_id) << 24u);
}

inline uint16_t nextgeo_voxel_unpack_oct16(uint32_t p_word1) {
	return uint16_t(p_word1 & 0xFFFFu);
}

inline uint8_t nextgeo_voxel_unpack_coverage(uint32_t p_word1) {
	return uint8_t((p_word1 >> 16u) & 0xFFu);
}

inline uint8_t nextgeo_voxel_unpack_surface_id(uint32_t p_word1) {
	return uint8_t((p_word1 >> 24u) & 0xFFu);
}

// Frame-stable deterministic stochastic coverage hash (bit-exact with voxel_inc.glsl).
// Zero temporal dependency -> 0.00 static-camera flicker.
inline bool nextgeo_voxel_coverage_pass(int32_t p_px, int32_t p_py, uint32_t p_record_idx, uint32_t p_brick_word, uint32_t p_voxel_idx, uint32_t p_coverage_u8) {
	if (p_coverage_u8 >= 255u) {
		return true;
	}
	if (p_coverage_u8 == 0u) {
		return false;
	}
	uint32_t h = uint32_t(p_px) * 374761393u + uint32_t(p_py) * 668265263u;
	h ^= p_record_idx * 2246822519u;
	h ^= p_brick_word * 3266489917u;
	h ^= p_voxel_idx * 1597334677u;
	h ^= h >> 13u;
	h *= 1274126177u;
	h ^= h >> 16u;
	return (h % 255u) < p_coverage_u8;
}

// Projected brick screen extent (pixels) matching cluster_cull.glsl.
inline float nextgeo_voxel_projected_brick_px(float p_voxel_size, float p_instance_scale, float p_group_radius, float p_view_z, float p_z_near, float p_px_scale, bool p_ortho) {
	const float brick_world = float(NEXTGEO_VOXEL_BRICK_DIM) * p_voxel_size * p_instance_scale;
	if (!(brick_world > 0.0f) || !(p_px_scale > 0.0f)) {
		return 1.0e30f;
	}
	if (p_ortho) {
		return brick_world * p_px_scale;
	}
	const float z_min = p_view_z - p_group_radius * p_instance_scale;
	if (z_min <= MAX(p_z_near, 1.0e-4f)) {
		return 1.0e30f;
	}
	return brick_world * p_px_scale / z_min;
}

} // namespace RendererRD
