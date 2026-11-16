/**************************************************************************/
/*  nextgeo_sw_raster.h                                                   */
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

#include "core/math/basis.h"
#include "core/math/math_funcs.h"
#include "core/math/projection.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_route.h"
#include "servers/rendering/rendering_device.h"

#include <cmath>
#include <cstring>

// GodotNext: nextgeo_sw_raster (#162, plan §21.4 N1-N3).
//
// The CPU reference for the hybrid HW/SW rasterizer: the device tier, the 64-bit visibility packing,
// the per-cluster triangle-size switch and the software rasterizer's triangle setup and edge rules.
// shaders/nextgeo/sw_raster_setup_inc.glsl, sw_raster.glsl and cluster_cull.glsl use the same
// formulas; test_nextgeo_sw_raster.cpp pins them against an independent reference rasterizer.
namespace RendererRD {

// The hybrid raster's visibility buffer storage (N2): an R64_UINT image, or a buffer of uint64 texels
// where images lack 64-bit atomics.
enum NextGeoVis64Storage : uint32_t {
	NEXTGEO_VIS64_NONE,
	NEXTGEO_VIS64_IMAGE,
	NEXTGEO_VIS64_BUFFER,
};

struct NextGeoRasterTier {
	bool hybrid = false;
	NextGeoVis64Storage storage = NEXTGEO_VIS64_NONE;
	// NEXTGEO_FALLBACK_NONE on the Nanite device tier, else NEXTGEO_FALLBACK_NO_INT64_ATOMICS: the view
	// keeps the v2.4 hardware raster with a depth attachment (N1). It is a raster-path reason, not a
	// route reason: the surfaces stay on NextGeo.
	NextGeoFallback reason = NEXTGEO_FALLBACK_NO_INT64_ATOMICS;
};

// N1: the Nanite device tier is Forward+ (the caller), 64-bit atomics on images or buffers, and
// indirect-count draws. Images win when both are present.
inline NextGeoRasterTier nextgeo_raster_tier(bool p_image_int64_atomics, bool p_buffer_int64_atomics, bool p_draw_indirect_count) {
	NextGeoRasterTier tier;
	if (!p_draw_indirect_count || !(p_image_int64_atomics || p_buffer_int64_atomics)) {
		return tier;
	}
	tier.hybrid = true;
	tier.storage = p_image_int64_atomics ? NEXTGEO_VIS64_IMAGE : NEXTGEO_VIS64_BUFFER;
	tier.reason = NEXTGEO_FALLBACK_NONE;
	return tier;
}

inline NextGeoRasterTier nextgeo_raster_tier(const RenderingDevice *p_device) {
	if (p_device == nullptr) {
		return NextGeoRasterTier();
	}
	return nextgeo_raster_tier(p_device->has_feature(RenderingDevice::SUPPORTS_IMAGE_INT64_ATOMICS), p_device->has_feature(RenderingDevice::SUPPORTS_BUFFER_INT64_ATOMICS), p_device->has_feature(RenderingDevice::SUPPORTS_DRAW_INDIRECT_COUNT));
}

// rendering/nextgeo/raster_mode.
enum NextGeoRasterMode : uint32_t {
	NEXTGEO_RASTER_MODE_HYBRID = 0,
	NEXTGEO_RASTER_MODE_HARDWARE = 1,
};

inline const char *nextgeo_raster_mode_name(NextGeoRasterMode p_mode) {
	return p_mode == NEXTGEO_RASTER_MODE_HYBRID ? "hybrid" : "hardware";
}

// N2: one 64-bit texel per internal pixel. The high word is reversed-Z depth as float bits (positive
// floats keep their order as unsigned integers), the low word the v2.4 visibility ID (25-bit visible
// cluster | 7-bit triangle). Both rasterizers write it with atomicMax, so the nearest fragment wins
// and, at equal depth, the larger ID, the same fragment the v2.4 GREATER_OR_EQUAL depth test keeps
// when the larger ID is drawn last. The buffer clears to 0; a texel with a zero depth word is empty.
static constexpr uint64_t NEXTGEO_VIS64_EMPTY = 0u;
// MN1: bytes per internal pixel.
static constexpr uint32_t NEXTGEO_VIS64_TEXEL_BYTES = 8u;

inline uint32_t nextgeo_depth_bits(float p_depth) {
	uint32_t bits = 0;
	memcpy(&bits, &p_depth, sizeof(uint32_t));
	return bits;
}

inline uint64_t nextgeo_pack_vis64(float p_depth, uint32_t p_visibility_id) {
	return (uint64_t(nextgeo_depth_bits(p_depth)) << 32u) | uint64_t(p_visibility_id);
}

inline bool nextgeo_vis64_is_empty(uint64_t p_texel) {
	return (p_texel >> 32u) == 0u;
}

inline float nextgeo_vis64_depth(uint64_t p_texel) {
	const uint32_t bits = uint32_t(p_texel >> 32u);
	float depth = 0.0f;
	memcpy(&depth, &bits, sizeof(float));
	return depth;
}

inline uint32_t nextgeo_vis64_visibility_id(uint64_t p_texel) {
	return uint32_t(p_texel & 0xFFFFFFFFu);
}

// N2 triangle-size switch. A cluster goes to the software rasterizer when its largest projected edge
// is below T px and its bounding sphere lies entirely in front of the near plane; every other
// cluster goes to the hardware rasterizer.
struct NextGeoSwRasterView {
	float max_edge_px = 0.0f; // T, rendering/nextgeo/sw_raster_max_edge_px. 0 sends every cluster to HW.
	float px_scale = 0.0f; // Pixels per unit length at unit view depth (see nextgeo_sw_raster_px_scale).
	float z_near = 0.0f;
	bool orthographic = false;
};

// Pixels per unit length at unit view depth. Perspective: the larger of the two axes' scales (half the
// internal width times P[0][0], half the height times P[1][1]), widened by the largest stretch of a
// pinhole projection inside the frustum, sqrt(1 + tan²x + tan²y). tan x and tan y are the largest
// view-space slopes on screen, (1 + |P[2][0]|) / P[0][0] and (1 + |P[2][1]|) / P[1][1], which also
// covers off-axis frustums (a Camera3D frustum offset, XR eye projections). The estimate is an upper
// bound for edges inside the frustum. Orthographic: the larger axis scale; a length projects the same
// everywhere.
inline float nextgeo_sw_raster_px_scale(const Projection &p_projection, float p_internal_width, float p_internal_height, bool p_orthographic) {
	const float p00 = Math::abs(p_projection.columns[0][0]);
	const float p11 = Math::abs(p_projection.columns[1][1]);
	const float axis_scale = MAX(0.5f * MAX(p_internal_width, 1.0f) * p00, 0.5f * MAX(p_internal_height, 1.0f) * p11);
	if (p_orthographic || p00 <= 0.0f || p11 <= 0.0f) {
		return axis_scale;
	}
	const float tan_x = (1.0f + Math::abs(p_projection.columns[2][0])) / p00;
	const float tan_y = (1.0f + Math::abs(p_projection.columns[2][1])) / p11;
	return axis_scale * Math::sqrt(1.0f + tan_x * tan_x + tan_y * tan_y);
}

// The most an instance's basis stretches a length, its largest singular value, bounded with
// Gershgorin's theorem on the Gram matrix of its columns: the square root of the largest row sum of
// |cᵢ·cⱼ|. The bound is exact for a rotation times a scale, mirrored or not, and stays an upper bound
// for a sheared basis (a rotated child under a non-uniformly scaled parent), where the longest column
// falls short. cluster_cull.glsl computes the same from the instance record's transform. The record's
// world_aabb_min.w cannot serve here: it is MAX(Basis::get_scale()), and get_scale() carries the
// determinant's sign, so a mirrored instance stores -min |scale|.
inline float nextgeo_sw_raster_instance_scale(const Basis &p_basis) {
	const Vector3 columns[3] = { p_basis.get_column(0), p_basis.get_column(1), p_basis.get_column(2) };
	float bound = 0.0f;
	for (int i = 0; i < 3; i++) {
		float row_sum = 0.0f;
		for (int j = 0; j < 3; j++) {
			row_sum += Math::abs(float(columns[i].dot(columns[j])));
		}
		bound = MAX(bound, row_sum);
	}
	return Math::sqrt(bound);
}

// p_max_edge: the cluster's longest edge in mesh units (the .ngm v2 record's f16, rounded up).
// p_scale: the instance's stretch, nextgeo_sw_raster_instance_scale(). p_view_depth: the view-space
// depth of the cluster's culling sphere center along the camera's forward axis. p_radius: that sphere's
// mesh-space radius.
inline float nextgeo_sw_raster_projected_edge(float p_max_edge, float p_scale, float p_view_depth, float p_radius, const NextGeoSwRasterView &p_view) {
	const float z_min = p_view_depth - p_radius * p_scale;
	if (p_view.orthographic) {
		return p_max_edge * p_scale * p_view.px_scale;
	}
	return p_max_edge * p_scale * p_view.px_scale / MAX(z_min, 1e-20f);
}

// A scale that is not positive (a degenerate basis, or a signed scale passed by mistake) goes to
// hardware.
inline bool nextgeo_sw_raster_route(float p_max_edge, float p_scale, float p_view_depth, float p_radius, const NextGeoSwRasterView &p_view) {
	if (!(p_view.max_edge_px > 0.0f) || !(p_scale > 0.0f) || !(p_max_edge >= 0.0f) || !std::isfinite(p_max_edge)) {
		return false;
	}
	const float z_min = p_view_depth - p_radius * p_scale;
	if (!(z_min > p_view.z_near)) {
		return false;
	}
	return nextgeo_sw_raster_projected_edge(p_max_edge, p_scale, p_view_depth, p_radius, p_view) < p_view.max_edge_px;
}

// N2 software triangle setup (sw_raster_setup_inc.glsl). Vertices snap to a fixed-point grid of
// 1 / 2^bits pixel, with 8 bits for triangles whose bounding box spans at most 64 pixels; larger ones
// drop one bit per doubling, down to 1 bit (pixel centers need half a pixel), so every edge function
// stays inside a 32-bit integer. Pixel (x, y) is
// sampled at its center. A triangle covers a sample when all three edge functions are positive, or
// zero on a top or left edge (the top-left rule, framebuffer y pointing down).
static constexpr int32_t NEXTGEO_SW_SUBPIXEL_BITS = 8;
static constexpr int32_t NEXTGEO_SW_FULL_PRECISION_EXTENT_PX = 64;
// Beyond this extent a triangle is not rasterized in software. The switch keeps software triangles
// inside the frustum below T px, so only a triangle reaching far outside it, where the edge estimate
// is not a bound, can get here.
static constexpr int32_t NEXTGEO_SW_MAX_EXTENT_PX = NEXTGEO_SW_FULL_PRECISION_EXTENT_PX << (NEXTGEO_SW_SUBPIXEL_BITS - 1);

// rendering/nextgeo/sw_raster_max_edge_px as the switch uses it. A triangle's bounding box is never
// wider than its longest edge, so with T at most 64 px every software triangle inside the frustum
// snaps to the full 8-bit grid, and two that share an edge snap it the same way. Larger values are
// clamped to 64; a value that is not positive turns the software path off.
inline float nextgeo_sw_raster_switch_px(float p_max_edge_px) {
	if (!(p_max_edge_px > 0.0f)) {
		return 0.0f;
	}
	return MIN(p_max_edge_px, float(NEXTGEO_SW_FULL_PRECISION_EXTENT_PX));
}

// Which faces survive: the v2.4 vertex shader's winding flip and back-face culling, folded into a sign.
enum NextGeoSwFacing : int32_t {
	NEXTGEO_SW_KEEP_POSITIVE = 1, // Front faces in framebuffer space (clockwise, y down).
	NEXTGEO_SW_KEEP_NEGATIVE = -1, // Mirrored instance or front-face culling.
	NEXTGEO_SW_KEEP_BOTH = 0, // Culling disabled (two-sided bucket or the back-face debug view).
};

struct NextGeoSwTriangle {
	bool valid = false;
	int32_t bits = NEXTGEO_SW_SUBPIXEL_BITS;
	// Inclusive pixel rectangle, clamped to the target.
	int32_t min_x = 0;
	int32_t min_y = 0;
	int32_t max_x = -1;
	int32_t max_y = -1;
	// Vertices in fixed point, relative to (min_x, min_y) << bits, wound so the area is positive.
	int32_t x[3] = {};
	int32_t y[3] = {};
	float z[3] = {};
	// 0 for a top or left edge, -1 otherwise; edge i runs from vertex i to vertex (i + 1) % 3.
	int32_t bias[3] = {};
	int32_t area = 0;
	// Which input vertex each wound vertex came from.
	uint8_t source[3] = { 0, 1, 2 };
};

inline int32_t nextgeo_sw_floor_shift(int32_t p_value, int32_t p_bits) {
	// Arithmetic shift: floor division by 2^bits, also for negative values.
	return p_value >= 0 ? (p_value >> p_bits) : -((-p_value + (1 << p_bits) - 1) >> p_bits);
}

inline int32_t nextgeo_sw_edge(int32_t p_ax, int32_t p_ay, int32_t p_bx, int32_t p_by, int32_t p_px, int32_t p_py) {
	return (p_bx - p_ax) * (p_py - p_ay) - (p_by - p_ay) * (p_px - p_ax);
}

inline int32_t nextgeo_sw_top_left_bias(int32_t p_ax, int32_t p_ay, int32_t p_bx, int32_t p_by) {
	const int32_t dx = p_bx - p_ax;
	const int32_t dy = p_by - p_ay;
	return (dy < 0 || (dy == 0 && dx > 0)) ? 0 : -1;
}

// p_px: framebuffer pixel coordinates (x right, y down), p_z: depth in [0, 1] (reversed Z).
inline NextGeoSwTriangle nextgeo_sw_setup_triangle(const float p_px[3][2], const float p_z[3], int32_t p_width, int32_t p_height, NextGeoSwFacing p_facing) {
	NextGeoSwTriangle tri;
	for (int i = 0; i < 3; i++) {
		if (!std::isfinite(p_px[i][0]) || !std::isfinite(p_px[i][1])) {
			return tri;
		}
	}
	float fmin_x = p_px[0][0];
	float fmax_x = p_px[0][0];
	float fmin_y = p_px[0][1];
	float fmax_y = p_px[0][1];
	for (int i = 1; i < 3; i++) {
		fmin_x = MIN(fmin_x, p_px[i][0]);
		fmax_x = MAX(fmax_x, p_px[i][0]);
		fmin_y = MIN(fmin_y, p_px[i][1]);
		fmax_y = MAX(fmax_y, p_px[i][1]);
	}
	// Reject triangles entirely outside the target before any integer conversion.
	if (!(fmax_x >= 0.0f && fmax_y >= 0.0f && fmin_x <= float(p_width) && fmin_y <= float(p_height))) {
		return tri;
	}
	const float extent = MAX(fmax_x - fmin_x, fmax_y - fmin_y);
	if (!(extent <= float(NEXTGEO_SW_MAX_EXTENT_PX))) {
		return tri;
	}
	int32_t bits = NEXTGEO_SW_SUBPIXEL_BITS;
	while (bits > 1 && extent > float(NEXTGEO_SW_FULL_PRECISION_EXTENT_PX << (NEXTGEO_SW_SUBPIXEL_BITS - bits))) {
		bits--;
	}
	const float scale = float(1 << bits);
	int32_t ax[3];
	int32_t ay[3];
	for (int i = 0; i < 3; i++) {
		ax[i] = int32_t(std::nearbyint(p_px[i][0] * scale));
		ay[i] = int32_t(std::nearbyint(p_px[i][1] * scale));
	}
	const int32_t half = (1 << bits) >> 1;
	const int32_t sx_min = MIN(ax[0], MIN(ax[1], ax[2]));
	const int32_t sx_max = MAX(ax[0], MAX(ax[1], ax[2]));
	const int32_t sy_min = MIN(ay[0], MIN(ay[1], ay[2]));
	const int32_t sy_max = MAX(ay[0], MAX(ay[1], ay[2]));
	// Pixel centers sit at (p << bits) + half; ceil and floor keep only the centers inside the box.
	tri.min_x = MAX(-nextgeo_sw_floor_shift(half - sx_min, bits), 0);
	tri.min_y = MAX(-nextgeo_sw_floor_shift(half - sy_min, bits), 0);
	tri.max_x = MIN(nextgeo_sw_floor_shift(sx_max - half, bits), p_width - 1);
	tri.max_y = MIN(nextgeo_sw_floor_shift(sy_max - half, bits), p_height - 1);
	if (tri.min_x > tri.max_x || tri.min_y > tri.max_y) {
		return tri;
	}
	const int32_t origin_x = tri.min_x << bits;
	const int32_t origin_y = tri.min_y << bits;
	int32_t x[3];
	int32_t y[3];
	for (int i = 0; i < 3; i++) {
		x[i] = ax[i] - origin_x;
		y[i] = ay[i] - origin_y;
	}
	int32_t area = nextgeo_sw_edge(x[0], y[0], x[1], y[1], x[2], y[2]);
	if (area == 0 || (p_facing == NEXTGEO_SW_KEEP_POSITIVE && area < 0) || (p_facing == NEXTGEO_SW_KEEP_NEGATIVE && area > 0)) {
		return tri;
	}
	uint8_t order[3] = { 0, 1, 2 };
	if (area < 0) {
		order[1] = 2;
		order[2] = 1;
		area = -area;
	}
	for (int i = 0; i < 3; i++) {
		tri.x[i] = x[order[i]];
		tri.y[i] = y[order[i]];
		tri.z[i] = p_z[order[i]];
		tri.source[i] = order[i];
	}
	for (int i = 0; i < 3; i++) {
		const int j = (i + 1) % 3;
		tri.bias[i] = nextgeo_sw_top_left_bias(tri.x[i], tri.y[i], tri.x[j], tri.y[j]);
	}
	tri.area = area;
	tri.bits = bits;
	tri.valid = true;
	return tri;
}

// Samples pixel (p_x, p_y) of a set-up triangle. On coverage, r_depth is the depth plane at the pixel
// center, interpolated from the integer edge functions as the hardware interpolates z.
inline bool nextgeo_sw_sample(const NextGeoSwTriangle &p_tri, int32_t p_x, int32_t p_y, float &r_depth) {
	if (!p_tri.valid || p_x < p_tri.min_x || p_x > p_tri.max_x || p_y < p_tri.min_y || p_y > p_tri.max_y) {
		return false;
	}
	const int32_t half = (1 << p_tri.bits) >> 1;
	const int32_t px = ((p_x - p_tri.min_x) << p_tri.bits) + half;
	const int32_t py = ((p_y - p_tri.min_y) << p_tri.bits) + half;
	const int32_t e0 = nextgeo_sw_edge(p_tri.x[1], p_tri.y[1], p_tri.x[2], p_tri.y[2], px, py);
	const int32_t e1 = nextgeo_sw_edge(p_tri.x[2], p_tri.y[2], p_tri.x[0], p_tri.y[0], px, py);
	const int32_t e2 = nextgeo_sw_edge(p_tri.x[0], p_tri.y[0], p_tri.x[1], p_tri.y[1], px, py);
	// e0 belongs to edge 1->2 (bias[1]), e1 to edge 2->0 (bias[2]), e2 to edge 0->1 (bias[0]).
	if (e0 + p_tri.bias[1] < 0 || e1 + p_tri.bias[2] < 0 || e2 + p_tri.bias[0] < 0) {
		return false;
	}
	const float inv_area = 1.0f / float(p_tri.area);
	r_depth = p_tri.z[0] + (float(e1) * (p_tri.z[1] - p_tri.z[0]) + float(e2) * (p_tri.z[2] - p_tri.z[0])) * inv_area;
	return true;
}

// Framebuffer pixel coordinates of a clip-space position, as the viewport transform maps them.
inline void nextgeo_sw_clip_to_pixel(float p_clip_x, float p_clip_y, float p_clip_z, float p_clip_w, int32_t p_width, int32_t p_height, float r_px[2], float &r_z) {
	const float inv_w = 1.0f / p_clip_w;
	r_px[0] = (p_clip_x * inv_w * 0.5f + 0.5f) * float(p_width);
	r_px[1] = (p_clip_y * inv_w * 0.5f + 0.5f) * float(p_height);
	r_z = p_clip_z * inv_w;
}

} // namespace RendererRD
