/**************************************************************************/
/*  nextgeo_tessellation.h                                                */
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
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_visibility.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_vsm.h"

#include <cmath>
#include <cstdint>

// GodotNext: nextgeo_tessellation (#163, plan §21.4 contract N11, GN-6, GN-12, N5).
//
// Pure C++ reference and shared GPU buffer layouts for Nanite runtime displacement tessellation:
//   - Settings defaults and clamping (experimental off by default, dicing_rate in [0.5, 16.0] px,
//     VSM directional/local toggles).
//   - Canonical crack-free edge factor computation (strictly from the two edge endpoints sorted
//     lexicographically, guaranteeing bit-identical factors across shared edges and locked DAG borders).
//   - Crack-free triangle triangulation (inner uniform grid + three outer zipper strips) with O(1)
//     micro-triangle lookup and edge-only attribute interpolation (zero dependence on opposite vertex).
//   - Sub-patch splitting into <= 128 micro-triangles per patch, occupying the reserved visibility ID
//     range [NEXTGEO_VIS_PATCH_BASE .. NEXTGEO_VIS_VOXEL_BASE - 1] (disjoint from #165 voxel bricks).
//   - Overflow coarsening policy that clamps writes safely and records dicing_overflow.
// Keep in sync with shaders/nextgeo/tessellation_inc.glsl, tessellation_dice.glsl, and tessellation_hw.glsl.
namespace RendererRD {

static constexpr bool NEXTGEO_TESS_DEFAULT_ENABLED = false;
static constexpr float NEXTGEO_TESS_DEFAULT_DICING_RATE = 2.0f;
static constexpr float NEXTGEO_TESS_MIN_DICING_RATE = 0.5f;
static constexpr float NEXTGEO_TESS_MAX_DICING_RATE = 16.0f;
static constexpr bool NEXTGEO_TESS_VSM_DEFAULT_DIRECTIONAL = true;
static constexpr bool NEXTGEO_TESS_VSM_DEFAULT_LOCAL = false;

static constexpr uint32_t NEXTGEO_TESS_MAX_EDGE_FACTOR = 64u;
static constexpr uint32_t NEXTGEO_TESS_MAX_PATCH_TRIANGLES = 128u;
static constexpr uint32_t NEXTGEO_TESS_MAX_SUB_PATCHES_PER_TRI = (NEXTGEO_TESS_MAX_EDGE_FACTOR * NEXTGEO_TESS_MAX_EDGE_FACTOR + NEXTGEO_TESS_MAX_PATCH_TRIANGLES - 1u) / NEXTGEO_TESS_MAX_PATCH_TRIANGLES; // 32

// Default runtime buffer capacities (allocated lazily ONLY when rendering/nextgeo/experimental/tessellation == true).
static constexpr uint32_t NEXTGEO_TESS_PATCH_CAPACITY = 1u << 18u; // 262,144 patches (8 MiB, strictly < 4M ID range)
static constexpr uint32_t NEXTGEO_TESS_HW_PATCH_CAPACITY = 1u << 16u; // 65,536 bounded HW patch entries (256 KiB)
static constexpr uint32_t NEXTGEO_TESS_VSM_PATCH_CAPACITY = 1u << 16u; // 65,536 VSM HW patches (2 MiB)
static constexpr uint32_t NEXTGEO_TESS_VSM_HW_PATCH_CAPACITY = 1u << 15u; // 32,768 bounded VSM HW patch entries

static_assert(NEXTGEO_TESS_PATCH_CAPACITY <= NEXTGEO_VIS_PATCH_MAX_COUNT, "Patch capacity must fit inside the reserved 4M patch visibility ID range.");
static_assert(NEXTGEO_TESS_MAX_PATCH_TRIANGLES == NEXTGEO_MAX_CLUSTER_TRIANGLES, "Diced sub-patches must fit inside the 7-bit visibility triangle index.");

// 32-byte compact patch record (2 x uvec4, std430). Read by visibility_export, resolve_classify,
// shading_binning, scene_forward_clustered (visibility resolve), tessellation_hw, and nextgeo_debug.
struct NextGeoDicedPatchRecord {
	uint32_t cluster_slot = 0u; // Index into visible_clusters[] (or vsm_clusters[] for VSM)
	uint32_t tri_and_sub = 0u; // bits 0..7 = src_tri [0..127], bits 8..15 = sub_patch_idx [0..31], bits 16..31 = micro_tri_count [1..128]
	uint32_t packed_factors = 0u; // bits 0..7 = f0 [1..64], bits 8..15 = f1 [1..64], bits 16..23 = f2 [1..64], bits 24..31 = flags/bin
	uint32_t micro_tri_base = 0u; // First global micro-triangle index of this sub-patch (sub_patch_idx * 128)
	float disp_magnitude = 0.0f; // Material displacement_magnitude (meters)
	float disp_center = 0.5f; // Material displacement_center [0..1]
	uint32_t pad0 = 0u;
	uint32_t pad1 = 0u;
};
static_assert(sizeof(NextGeoDicedPatchRecord) == 32, "NextGeoDicedPatchRecord must match GLSL std430 layout (32 bytes).");

// Shared GPU state buffer written by tessellation_dice.glsl (320 bytes, 80 uint32 words).
struct NextGeoTessState {
	uint32_t patch_count = 0u; // 0: total allocated patch slots (may exceed capacity on overflow; writes are clamped)
	uint32_t dicing_overflow = 0u; // 1: patch/HW-stream overflow counter exposed in debug stats
	uint32_t sw_micro_tris = 0u; // 2: micro-triangles SW-rasterized in tessellation_dice
	uint32_t hw_patches = 0u; // 3: sub-patches routed to the bounded HW stream
	uint32_t hw_stream_overflow = 0u; // 4: bounded HW stream overflow events
	uint32_t coarsened_tris = 0u; // 5: triangles coarsened due to patch budget pressure/overflow
	uint32_t diced_clusters = 0u; // 6: clusters processed by tessellation_dice
	uint32_t sink = 0u; // 7: #277 D3D12 GBV atomic sink
	uint32_t hw_bin_alloc[64] = {}; // 8..71: per-bin HW patch allocation cursors (reset each phase)
	uint32_t pad[8] = {}; // 72..79
};
static_assert(sizeof(NextGeoTessState) == 320, "NextGeoTessState must match GLSL std430 layout (320 bytes).");

static constexpr uint32_t NEXTGEO_TESS_STATS_WORDS = 8u;

inline float nextgeo_tessellation_clamp_dicing_rate(float p_rate_px) {
	if (!std::isfinite(p_rate_px)) {
		return NEXTGEO_TESS_DEFAULT_DICING_RATE;
	}
	return CLAMP(p_rate_px, NEXTGEO_TESS_MIN_DICING_RATE, NEXTGEO_TESS_MAX_DICING_RATE);
}

inline bool nextgeo_tessellation_vsm_chain_enabled(bool p_tessellation_enabled, uint32_t p_chain, bool p_vsm_directional, bool p_vsm_local) {
	if (!p_tessellation_enabled) {
		return false;
	}
	return p_chain == NEXTGEO_VSM_CHAIN_DIRECTIONAL ? p_vsm_directional : p_vsm_local;
}

// Conservative local-space displacement bound accounting for magnitude and center offset.
inline float nextgeo_tessellation_displacement_bound(float p_magnitude, float p_center) {
	if (!std::isfinite(p_magnitude) || p_magnitude <= 0.0f) {
		return 0.0f;
	}
	const float c = std::isfinite(p_center) ? p_center : 0.5f;
	const float scale = MAX(1.0f, MAX(Math::abs(c), Math::abs(1.0f - c)));
	return p_magnitude * scale;
}

inline uint32_t nextgeo_tess_pack_tri_and_sub(uint32_t p_src_tri, uint32_t p_sub_patch, uint32_t p_micro_count) {
	return (p_src_tri & 0xFFu) | ((p_sub_patch & 0xFFu) << 8u) | ((p_micro_count & 0xFFFFu) << 16u);
}

inline uint32_t nextgeo_tess_pack_factors(uint32_t p_f0, uint32_t p_f1, uint32_t p_f2, uint32_t p_flags = 0u) {
	return (p_f0 & 0xFFu) | ((p_f1 & 0xFFu) << 8u) | ((p_f2 & 0xFFu) << 16u) | ((p_flags & 0xFFu) << 24u);
}

inline bool nextgeo_tess_vec3_less(const Vector3 &p_a, const Vector3 &p_b) {
	if (p_a.x != p_b.x) {
		return p_a.x < p_b.x;
	}
	if (p_a.y != p_b.y) {
		return p_a.y < p_b.y;
	}
	return p_a.z < p_b.z;
}

inline bool nextgeo_tess_canonicalize_edge(Vector3 &r_a, Vector3 &r_b) {
	if (nextgeo_tess_vec3_less(r_b, r_a)) {
		const Vector3 tmp = r_a;
		r_a = r_b;
		r_b = tmp;
		return true;
	}
	return false;
}

inline Vector3 nextgeo_tess_xform_point(const float p_transform[3][4], const Vector3 &p_local) {
	return Vector3(
			p_transform[0][0] * float(p_local.x) + p_transform[0][1] * float(p_local.y) + p_transform[0][2] * float(p_local.z) + p_transform[0][3],
			p_transform[1][0] * float(p_local.x) + p_transform[1][1] * float(p_local.y) + p_transform[1][2] * float(p_local.z) + p_transform[1][3],
			p_transform[2][0] * float(p_local.x) + p_transform[2][1] * float(p_local.y) + p_transform[2][2] * float(p_local.z) + p_transform[2][3]);
}

// Crack-free canonical edge factor strictly from the two endpoints (a, b) sorted lexicographically.
// Flipping endpoint order (a, b) -> (b, a) produces the bit-identical integer edge factor.
inline uint32_t nextgeo_tess_edge_factor(Vector3 p_a_local, Vector3 p_b_local, const float p_transform[3][4], const Vector3 &p_cam_pos, float p_px_scale, float p_z_near, bool p_ortho, float p_dicing_rate) {
	nextgeo_tess_canonicalize_edge(p_a_local, p_b_local);
	const Vector3 a_w = nextgeo_tess_xform_point(p_transform, p_a_local);
	const Vector3 b_w = nextgeo_tess_xform_point(p_transform, p_b_local);
	const float world_len = float((b_w - a_w).length());
	const float rate = nextgeo_tessellation_clamp_dicing_rate(p_dicing_rate);
	if (!(world_len > 0.0f) || !(p_px_scale > 0.0f)) {
		return 1u;
	}
	float proj_len = world_len * p_px_scale;
	if (!p_ortho) {
		const Vector3 mid_w = (a_w + b_w) * 0.5f;
		const float min_dist = MAX(float((mid_w - p_cam_pos).length()) - 0.5f * world_len, MAX(p_z_near, 1.0e-4f));
		proj_len /= min_dist;
	}
	const float raw = std::ceil(proj_len / rate);
	if (!std::isfinite(raw) || raw <= 1.0f) {
		return 1u;
	}
	return MIN(uint32_t(raw), NEXTGEO_TESS_MAX_EDGE_FACTOR);
}

// VSM variant: projected edge length in shadow texels of the target page mip.
inline uint32_t nextgeo_tess_vsm_edge_factor(Vector3 p_a_local, Vector3 p_b_local, const float p_transform[3][4], const Vector3 &p_light_origin, float p_page_texel, float p_near_w, bool p_directional, float p_dicing_rate) {
	nextgeo_tess_canonicalize_edge(p_a_local, p_b_local);
	const Vector3 a_w = nextgeo_tess_xform_point(p_transform, p_a_local);
	const Vector3 b_w = nextgeo_tess_xform_point(p_transform, p_b_local);
	const float world_len = float((b_w - a_w).length());
	const float rate = nextgeo_tessellation_clamp_dicing_rate(p_dicing_rate);
	if (!(world_len > 0.0f) || !(p_page_texel > 0.0f)) {
		return 1u;
	}
	float proj_texels = world_len / p_page_texel;
	if (!p_directional) {
		const Vector3 mid_w = (a_w + b_w) * 0.5f;
		const float min_dist = MAX(float((mid_w - p_light_origin).length()) - 0.5f * world_len, MAX(p_near_w, 1.0e-4f));
		proj_texels /= min_dist;
	}
	const float raw = std::ceil(proj_texels / rate);
	if (!std::isfinite(raw) || raw <= 1.0f) {
		return 1u;
	}
	return MIN(uint32_t(raw), NEXTGEO_TESS_MAX_EDGE_FACTOR);
}

inline uint32_t nextgeo_tess_interior_factor(uint32_t p_f0, uint32_t p_f1, uint32_t p_f2) {
	return MAX(p_f0, MAX(p_f1, p_f2));
}

// Total micro-triangles generated by the exact inner-grid + 3-outer-zipper-strip triangulation.
// For f0 == f1 == f2 == N, this equals N * N (zero degenerate or overlapping triangles).
inline uint32_t nextgeo_tess_micro_tri_count(uint32_t p_f0, uint32_t p_f1, uint32_t p_f2) {
	const uint32_t n = nextgeo_tess_interior_factor(p_f0, p_f1, p_f2);
	if (n <= 1u) {
		return 1u;
	}
	if (n <= 3u) {
		return p_f0 + p_f1 + p_f2;
	}
	const uint32_t m = n - 3u;
	return m * m + p_f0 + p_f1 + p_f2 + 3u * m;
}

inline uint32_t nextgeo_tess_patch_count(uint32_t p_f0, uint32_t p_f1, uint32_t p_f2) {
	const uint32_t total = nextgeo_tess_micro_tri_count(p_f0, p_f1, p_f2);
	return (total + NEXTGEO_TESS_MAX_PATCH_TRIANGLES - 1u) / NEXTGEO_TESS_MAX_PATCH_TRIANGLES;
}

// Conservative cluster micro-triangle count estimate used in cull routing.
inline uint32_t nextgeo_tess_estimate_cluster_micro_tris(float p_max_edge, float p_scale, float p_px_scale, float p_min_dist, bool p_ortho, float p_dicing_rate, uint32_t p_tri_count) {
	if (p_tri_count == 0u || !(p_max_edge > 0.0f) || !(p_scale > 0.0f) || !(p_px_scale > 0.0f)) {
		return p_tri_count;
	}
	const float rate = nextgeo_tessellation_clamp_dicing_rate(p_dicing_rate);
	float proj = p_max_edge * p_scale * p_px_scale;
	if (!p_ortho) {
		proj /= MAX(p_min_dist, 1.0e-4f);
	}
	const uint32_t f = CLAMP(uint32_t(std::ceil(proj / rate)), 1u, NEXTGEO_TESS_MAX_EDGE_FACTOR);
	return p_tri_count * f * f;
}

// Coarsens (r_f0, r_f1, r_f2) cleanly when remaining patch slots are insufficient.
// Returns true if the coarsened triangle fits in p_avail_patches (>= 1), false if p_avail_patches == 0.
inline bool nextgeo_tess_coarsen_for_budget(uint32_t &r_f0, uint32_t &r_f1, uint32_t &r_f2, uint32_t p_avail_patches, uint32_t &r_coarsen_steps) {
	r_coarsen_steps = 0u;
	if (p_avail_patches == 0u) {
		return false;
	}
	while (nextgeo_tess_patch_count(r_f0, r_f1, r_f2) > p_avail_patches && (r_f0 > 1u || r_f1 > 1u || r_f2 > 1u)) {
		r_f0 = MAX((r_f0 + 1u) >> 1u, 1u);
		r_f1 = MAX((r_f1 + 1u) >> 1u, 1u);
		r_f2 = MAX((r_f2 + 1u) >> 1u, 1u);
		r_coarsen_steps++;
	}
	return nextgeo_tess_patch_count(r_f0, r_f1, r_f2) <= p_avail_patches;
}

// Vertex location tag for edge-only interpolation (zero dependence on opposite vertex along shared edges).
//   kind 0..2: exact source corner V0, V1, V2
//   kind 3..5: interior edge vertex on Edge 0 (V0->V1), Edge 1 (V1->V2), Edge 2 (V2->V0) at step num/den
//   kind 6:    strictly interior vertex with barycentric (u, v, w)
struct NextGeoTessVertexCoord {
	uint32_t kind = 0u;
	uint32_t num = 0u;
	uint32_t den = 1u;
	float u = 1.0f;
	float v = 0.0f;
	float w = 0.0f;
};

inline NextGeoTessVertexCoord nextgeo_tess_make_corner(uint32_t p_corner) {
	NextGeoTessVertexCoord c;
	c.kind = p_corner;
	c.num = 0u;
	c.den = 1u;
	c.u = (p_corner == 0u) ? 1.0f : 0.0f;
	c.v = (p_corner == 1u) ? 1.0f : 0.0f;
	c.w = (p_corner == 2u) ? 1.0f : 0.0f;
	return c;
}

inline NextGeoTessVertexCoord nextgeo_tess_make_edge(uint32_t p_edge, uint32_t p_step, uint32_t p_factor) {
	if (p_step == 0u) {
		return nextgeo_tess_make_corner(p_edge);
	}
	if (p_step >= p_factor) {
		return nextgeo_tess_make_corner((p_edge + 1u) % 3u);
	}
	NextGeoTessVertexCoord c;
	c.kind = 3u + p_edge;
	c.num = p_step;
	c.den = p_factor;
	const float t = float(p_step) / float(p_factor);
	const float omt = 1.0f - t;
	if (p_edge == 0u) {
		c.u = omt;
		c.v = t;
		c.w = 0.0f;
	} else if (p_edge == 1u) {
		c.u = 0.0f;
		c.v = omt;
		c.w = t;
	} else {
		c.u = t;
		c.v = 0.0f;
		c.w = omt;
	}
	return c;
}

inline NextGeoTessVertexCoord nextgeo_tess_make_inner(uint32_t p_i, uint32_t p_j, uint32_t p_k, uint32_t p_n) {
	NextGeoTessVertexCoord c;
	c.kind = 6u;
	const float inv_n = 1.0f / float(p_n);
	c.u = float(p_i) * inv_n;
	c.v = float(p_j) * inv_n;
	c.w = float(p_k) * inv_n;
	return c;
}

inline NextGeoTessVertexCoord nextgeo_tess_make_inner_edge(uint32_t p_edge, uint32_t p_b, uint32_t p_m, uint32_t p_n) {
	if (p_edge == 0u) {
		return nextgeo_tess_make_inner(p_m - p_b + 1u, p_b + 1u, 1u, p_n);
	}
	if (p_edge == 1u) {
		return nextgeo_tess_make_inner(1u, p_m - p_b + 1u, p_b + 1u, p_n);
	}
	return nextgeo_tess_make_inner(p_b + 1u, 1u, p_m - p_b + 1u, p_n);
}

inline NextGeoTessVertexCoord nextgeo_tess_make_center() {
	NextGeoTessVertexCoord c;
	c.kind = 6u;
	c.u = 1.0f / 3.0f;
	c.v = 1.0f / 3.0f;
	c.w = 1.0f / 3.0f;
	return c;
}

inline uint32_t nextgeo_tess_isqrt(uint32_t p_x) {
	uint32_t s = uint32_t(std::floor(std::sqrt(float(p_x))));
	while ((s + 1u) * (s + 1u) <= p_x) {
		s++;
	}
	while (s * s > p_x) {
		s--;
	}
	return s;
}

// O(1) closed-form lookup of the 3 vertex coordinates of global micro-triangle p_idx in [0 .. count - 1].
inline void nextgeo_tess_micro_tri_coords(uint32_t p_f0, uint32_t p_f1, uint32_t p_f2, uint32_t p_idx, NextGeoTessVertexCoord r_out[3]) {
	const uint32_t n = nextgeo_tess_interior_factor(p_f0, p_f1, p_f2);
	if (n <= 1u) {
		r_out[0] = nextgeo_tess_make_corner(0u);
		r_out[1] = nextgeo_tess_make_corner(1u);
		r_out[2] = nextgeo_tess_make_corner(2u);
		return;
	}
	if (n <= 3u) {
		const NextGeoTessVertexCoord ctr = nextgeo_tess_make_center();
		if (p_idx < p_f0) {
			r_out[0] = nextgeo_tess_make_edge(0u, p_idx, p_f0);
			r_out[1] = nextgeo_tess_make_edge(0u, p_idx + 1u, p_f0);
			r_out[2] = ctr;
		} else if (p_idx < p_f0 + p_f1) {
			const uint32_t s = p_idx - p_f0;
			r_out[0] = nextgeo_tess_make_edge(1u, s, p_f1);
			r_out[1] = nextgeo_tess_make_edge(1u, s + 1u, p_f1);
			r_out[2] = ctr;
		} else {
			const uint32_t s = p_idx - p_f0 - p_f1;
			r_out[0] = nextgeo_tess_make_edge(2u, s, p_f2);
			r_out[1] = nextgeo_tess_make_edge(2u, s + 1u, p_f2);
			r_out[2] = ctr;
		}
		return;
	}

	const uint32_t m = n - 3u;
	const uint32_t inner_tris = m * m;
	if (p_idx < inner_tris) {
		const uint32_t s = nextgeo_tess_isqrt(p_idx);
		const uint32_t col = p_idx - s * s;
		const uint32_t p = col >> 1u;
		if ((col & 1u) == 0u) {
			r_out[0] = nextgeo_tess_make_inner(s + 2u - p, p + 1u, m - s, n);
			r_out[1] = nextgeo_tess_make_inner(s + 1u - p, p + 2u, m - s, n);
			r_out[2] = nextgeo_tess_make_inner(s + 1u - p, p + 1u, m - s + 1u, n);
		} else {
			r_out[0] = nextgeo_tess_make_inner(s + 1u - p, p + 2u, m - s, n);
			r_out[1] = nextgeo_tess_make_inner(s - p, p + 2u, m - s + 1u, n);
			r_out[2] = nextgeo_tess_make_inner(s + 1u - p, p + 1u, m - s + 1u, n);
		}
		return;
	}

	uint32_t rem = p_idx - inner_tris;
	const uint32_t s0 = p_f0 + m;
	const uint32_t s1 = p_f1 + m;
	uint32_t edge = 0u;
	uint32_t outer_segs = p_f0;
	if (rem < s0) {
		edge = 0u;
		outer_segs = p_f0;
	} else if (rem < s0 + s1) {
		edge = 1u;
		outer_segs = p_f1;
		rem -= s0;
	} else {
		edge = 2u;
		outer_segs = p_f2;
		rem -= (s0 + s1);
	}
	const uint32_t total_steps = outer_segs + m;
	const uint32_t a0 = (rem * outer_segs + (total_steps >> 1u)) / total_steps;
	const uint32_t a1 = ((rem + 1u) * outer_segs + (total_steps >> 1u)) / total_steps;
	const uint32_t b0 = rem - a0;
	if (a1 > a0) {
		r_out[0] = nextgeo_tess_make_edge(edge, a0, outer_segs);
		r_out[1] = nextgeo_tess_make_edge(edge, a0 + 1u, outer_segs);
		r_out[2] = nextgeo_tess_make_inner_edge(edge, b0, m, n);
	} else {
		r_out[0] = nextgeo_tess_make_edge(edge, a0, outer_segs);
		r_out[1] = nextgeo_tess_make_inner_edge(edge, b0 + 1u, m, n);
		r_out[2] = nextgeo_tess_make_inner_edge(edge, b0, m, n);
	}
}

// Crack-free canonical edge-only position/attribute interpolation: zero dependence on opposite vertex.
inline Vector3 nextgeo_tess_interp_vec3(const Vector3 p_pos[3], const Vector3 p_attr[3], const NextGeoTessVertexCoord &p_coord) {
	if (p_coord.kind < 3u) {
		return p_attr[p_coord.kind];
	}
	if (p_coord.kind < 6u) {
		uint32_t ia = p_coord.kind - 3u;
		uint32_t ib = (ia + 1u) % 3u;
		uint32_t num = p_coord.num;
		if (nextgeo_tess_vec3_less(p_pos[ib], p_pos[ia])) {
			const uint32_t tmp = ia;
			ia = ib;
			ib = tmp;
			num = p_coord.den - num;
		}
		const float t = float(num) / float(p_coord.den);
		return p_attr[ia].lerp(p_attr[ib], t);
	}
	return p_attr[0] * p_coord.u + p_attr[1] * p_coord.v + p_attr[2] * p_coord.w;
}

inline Vector2 nextgeo_tess_interp_vec2(const Vector3 p_pos[3], const Vector2 p_attr[3], const NextGeoTessVertexCoord &p_coord) {
	if (p_coord.kind < 3u) {
		return p_attr[p_coord.kind];
	}
	if (p_coord.kind < 6u) {
		uint32_t ia = p_coord.kind - 3u;
		uint32_t ib = (ia + 1u) % 3u;
		uint32_t num = p_coord.num;
		if (nextgeo_tess_vec3_less(p_pos[ib], p_pos[ia])) {
			const uint32_t tmp = ia;
			ia = ib;
			ib = tmp;
			num = p_coord.den - num;
		}
		const float t = float(num) / float(p_coord.den);
		return p_attr[ia].lerp(p_attr[ib], t);
	}
	return p_attr[0] * p_coord.u + p_attr[1] * p_coord.v + p_attr[2] * p_coord.w;
}

// Splits a source triangle into sub-patches of <= 128 micro-triangles each, respecting patch buffer capacity
// with overflow coarsening and recording overflow/coarsening statistics cleanly without out-of-bounds writes.
inline uint32_t nextgeo_tess_emit_triangle_patches(
		uint32_t p_cluster_slot,
		uint32_t p_src_tri,
		uint32_t p_f0,
		uint32_t p_f1,
		uint32_t p_f2,
		float p_disp_magnitude,
		float p_disp_center,
		uint32_t p_patch_capacity,
		LocalVector<NextGeoDicedPatchRecord> &r_patches,
		NextGeoTessState &r_state) {
	uint32_t f0 = CLAMP(p_f0, 1u, NEXTGEO_TESS_MAX_EDGE_FACTOR);
	uint32_t f1 = CLAMP(p_f1, 1u, NEXTGEO_TESS_MAX_EDGE_FACTOR);
	uint32_t f2 = CLAMP(p_f2, 1u, NEXTGEO_TESS_MAX_EDGE_FACTOR);
	const uint32_t used = r_state.patch_count;
	const uint32_t avail = used < p_patch_capacity ? (p_patch_capacity - used) : 0u;
	uint32_t needed = nextgeo_tess_patch_count(f0, f1, f2);
	if (needed > avail) {
		r_state.dicing_overflow++;
		uint32_t steps = 0u;
		if (!nextgeo_tess_coarsen_for_budget(f0, f1, f2, avail, steps)) {
			return 0u;
		}
		if (steps > 0u) {
			r_state.coarsened_tris++;
		}
		needed = nextgeo_tess_patch_count(f0, f1, f2);
	}
	const uint32_t total_micro = nextgeo_tess_micro_tri_count(f0, f1, f2);
	for (uint32_t sp = 0u; sp < needed; sp++) {
		const uint32_t base = sp * NEXTGEO_TESS_MAX_PATCH_TRIANGLES;
		const uint32_t count = MIN(NEXTGEO_TESS_MAX_PATCH_TRIANGLES, total_micro - base);
		NextGeoDicedPatchRecord rec;
		rec.cluster_slot = p_cluster_slot;
		rec.tri_and_sub = nextgeo_tess_pack_tri_and_sub(p_src_tri, sp, count);
		rec.packed_factors = nextgeo_tess_pack_factors(f0, f1, f2, 0u);
		rec.micro_tri_base = base;
		rec.disp_magnitude = p_disp_magnitude;
		rec.disp_center = p_disp_center;
		r_patches.push_back(rec);
		r_state.patch_count++;
	}
	return needed;
}

} // namespace RendererRD
