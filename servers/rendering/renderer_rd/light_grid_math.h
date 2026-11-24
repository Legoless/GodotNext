/**************************************************************************/
/*  light_grid_math.h                                                     */
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

// GodotNext: light_grid (#139, plan §21.3 contract M2).
// Shared constants, GPU/CPU structs, barn-door frustum math, culling predicates,
// selection rule, and CPU brute-force reference builder for the packed froxel light grid.

#include "core/math/aabb.h"
#include "core/math/math_funcs.h"
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "core/math/vector3i.h"
#include "core/math/vector4.h"
#include "core/templates/local_vector.h"
#include "core/variant/dictionary.h"

#include <cmath>
#include <cstdint>

namespace RendererRD {

namespace LightGrid {

static constexpr uint32_t TILE_SIZE = 64u;
static constexpr uint32_t TILE_SHIFT = 6u;
static constexpr uint32_t Z_SLICES = 32u;
static constexpr uint32_t COARSE_GROUP = 4u;
static constexpr uint32_t COARSE_SHIFT = 2u;
static constexpr uint32_t MAX_LIGHTS = 65536u;
static constexpr uint32_t MAX_LIGHTS_PER_CELL = 64u;
static constexpr uint32_t COARSE_LIST_CAP = 1024u;

static constexpr uint32_t LIGHT_TYPE_OMNI = 0u;
static constexpr uint32_t LIGHT_TYPE_SPOT = 1u;
static constexpr uint32_t LIGHT_TYPE_AREA = 2u;

static constexpr uint32_t ENTRY_INDEX_MASK = 0xFFFFu;
static constexpr uint32_t ENTRY_TYPE_SHIFT = 16u;
static constexpr uint32_t ENTRY_TYPE_MASK = 0x3u;
static constexpr uint32_t ENTRY_FLAG_MEGALIGHTS = 1u << 18u;

static constexpr uint32_t GRID_FLAG_ENABLED = 1u << 0u;
static constexpr uint32_t GRID_FLAG_ORTHOGONAL = 1u << 1u;

static constexpr uint32_t BUILD_FLAG_ORTHOGONAL = 1u << 0u;
static constexpr uint32_t BUILD_FLAG_HAS_HIZ = 1u << 1u;
static constexpr uint32_t BUILD_FLAG_CULL_FRONT = 1u << 2u;

static constexpr float DEFAULT_BARN_DOOR_ANGLE_DEG = 90.0f;
static constexpr float DEFAULT_BARN_DOOR_LENGTH = 0.2f;
static constexpr float BARN_DOOR_OPEN_ANGLE_DEG = 89.99f;
static constexpr float BARN_DOOR_MIN_LENGTH = 1e-4f;

// Relative conservative margin on Z slice bounds and Hi-Z depth bounds to avoid boundary misses.
static constexpr float HIZ_FAR_TOLERANCE = 1.0005f;
static constexpr float HIZ_NEAR_TOLERANCE = 0.9995f;
static constexpr float SLICE_MARGIN_NEAR = 0.9999f;
static constexpr float SLICE_MARGIN_FAR = 1.0001f;

// Packed GPU light culling record (80 bytes = 5 x vec4, std430).
struct PackedLightData {
	float position[3];
	float radius;

	float direction[3];
	float cos_spot_angle;

	float axis_x[3];
	float half_width;

	float axis_y[3];
	float half_height;

	float barn_door_angle_rad;
	float barn_door_length;
	float intensity;
	uint32_t packed_entry;
};

static_assert(sizeof(PackedLightData) == 80, "PackedLightData must match light_grid_build.glsl (80 bytes).");
static_assert(sizeof(PackedLightData) % 16 == 0, "PackedLightData must stay 16-byte aligned.");

struct CellHeader {
	uint32_t offset = 0;
	uint32_t count = 0;
};

static_assert(sizeof(CellHeader) == 8, "CellHeader must match uvec2 in GLSL.");

struct GridStats {
	uint32_t overflow_cells = 0;
	uint32_t overflow_lights = 0;
	uint32_t total_assignments = 0;
	uint32_t max_cell_count = 0;
	uint32_t sink = 0;
	uint32_t pad[3] = {};
};

static_assert(sizeof(GridStats) == 32, "GridStats must match light_grid_build.glsl.");

struct BuildParamsUBO {
	float inv_projection[16];

	uint32_t screen_size[2];
	uint32_t mid_size[2];

	uint32_t grid_dims[3];
	uint32_t flags;

	uint32_t coarse_dims[3];
	uint32_t coarse_stride;

	float z_near;
	float z_far;
	float z_scale;
	float z_bias;

	uint32_t light_count;
	uint32_t omni_count;
	uint32_t spot_count;
	uint32_t area_count;
};

static_assert(sizeof(BuildParamsUBO) == 144, "BuildParamsUBO must match light_grid_build.glsl.");
static_assert(sizeof(BuildParamsUBO) % 16 == 0, "BuildParamsUBO must stay 16-byte aligned.");

struct ZParams {
	float z_near = 0.05f;
	float z_far = 100.0f;
	float z_scale = 1.0f;
	float z_bias = 0.0f;
	bool orthogonal = false;
};

struct CellBounds {
	Vector3 bmin;
	Vector3 bmax;
	Vector4 planes[4]; // Outward-pointing side frustum planes: dot(n, p) + d <= 0 inside froxel.
};

_FORCE_INLINE_ bool should_use_packed_grid(bool p_megalights_enabled, uint32_t p_positional_light_count, uint32_t p_bitmask_cap) {
	return p_megalights_enabled || p_positional_light_count > p_bitmask_cap;
}

_FORCE_INLINE_ uint32_t pack_entry(uint32_t p_type, uint32_t p_per_type_index, bool p_megalights_handled = false) {
	return (p_per_type_index & ENTRY_INDEX_MASK) | ((p_type & ENTRY_TYPE_MASK) << ENTRY_TYPE_SHIFT) | (p_megalights_handled ? ENTRY_FLAG_MEGALIGHTS : 0u);
}

_FORCE_INLINE_ uint32_t unpack_entry_index(uint32_t p_entry) {
	return p_entry & ENTRY_INDEX_MASK;
}

_FORCE_INLINE_ uint32_t unpack_entry_type(uint32_t p_entry) {
	return (p_entry >> ENTRY_TYPE_SHIFT) & ENTRY_TYPE_MASK;
}

_FORCE_INLINE_ bool unpack_entry_megalights(uint32_t p_entry) {
	return (p_entry & ENTRY_FLAG_MEGALIGHTS) != 0u;
}

_FORCE_INLINE_ uint32_t entry_to_global_index(uint32_t p_entry, uint32_t p_omni_count, uint32_t p_spot_count) {
	const uint32_t ltype = unpack_entry_type(p_entry);
	const uint32_t idx = unpack_entry_index(p_entry);
	if (ltype == LIGHT_TYPE_OMNI) {
		return idx;
	}
	if (ltype == LIGHT_TYPE_SPOT) {
		return p_omni_count + idx;
	}
	return p_omni_count + p_spot_count + idx;
}

_FORCE_INLINE_ Vector3i compute_grid_dims(const Size2i &p_screen_size) {
	const uint32_t w = uint32_t(MAX(p_screen_size.x, 1));
	const uint32_t h = uint32_t(MAX(p_screen_size.y, 1));
	return Vector3i(
			int((w + TILE_SIZE - 1u) / TILE_SIZE),
			int((h + TILE_SIZE - 1u) / TILE_SIZE),
			int(Z_SLICES));
}

_FORCE_INLINE_ Vector3i compute_coarse_dims(const Vector3i &p_grid_dims) {
	return Vector3i(
			int((uint32_t(p_grid_dims.x) + COARSE_GROUP - 1u) / COARSE_GROUP),
			int((uint32_t(p_grid_dims.y) + COARSE_GROUP - 1u) / COARSE_GROUP),
			int((uint32_t(p_grid_dims.z) + COARSE_GROUP - 1u) / COARSE_GROUP));
}

_FORCE_INLINE_ uint32_t cell_index(uint32_t p_tx, uint32_t p_ty, uint32_t p_tz, const Vector3i &p_grid_dims) {
	return (p_tz * uint32_t(p_grid_dims.y) + p_ty) * uint32_t(p_grid_dims.x) + p_tx;
}

_FORCE_INLINE_ ZParams compute_z_params(float p_z_near, float p_z_far, bool p_orthogonal) {
	ZParams zp;
	zp.orthogonal = p_orthogonal;
	zp.z_near = MAX(p_z_near, 1e-4f);
	zp.z_far = MAX(p_z_far, zp.z_near * 1.001f);
	if (p_orthogonal) {
		zp.z_scale = float(Z_SLICES) / (zp.z_far - zp.z_near);
		zp.z_bias = -zp.z_near * zp.z_scale;
	} else {
		zp.z_scale = float(Z_SLICES) / std::log(zp.z_far / zp.z_near);
		zp.z_bias = -std::log(zp.z_near) * zp.z_scale;
	}
	return zp;
}

_FORCE_INLINE_ uint32_t depth_to_slice(float p_linear_depth, const ZParams &p_zp) {
	const float d = MAX(p_linear_depth, p_zp.z_near);
	const float s = p_zp.orthogonal ? (d * p_zp.z_scale + p_zp.z_bias) : (std::log(d) * p_zp.z_scale + p_zp.z_bias);
	const float clamped = CLAMP(std::floor(s), 0.0f, float(Z_SLICES - 1u));
	return uint32_t(clamped);
}

_FORCE_INLINE_ void slice_depth_bounds(uint32_t p_slice_begin, uint32_t p_slice_end, const ZParams &p_zp, float &r_z0, float &r_z1) {
	const float t0 = float(p_slice_begin) / float(Z_SLICES);
	const float t1 = float(p_slice_end) / float(Z_SLICES);
	if (p_zp.orthogonal) {
		r_z0 = p_zp.z_near + (p_zp.z_far - p_zp.z_near) * t0;
		r_z1 = p_zp.z_near + (p_zp.z_far - p_zp.z_near) * t1;
	} else {
		const float log2_ratio = std::log2(p_zp.z_far / p_zp.z_near);
		r_z0 = (p_slice_begin == 0u) ? p_zp.z_near : p_zp.z_near * std::exp2(t0 * log2_ratio);
		r_z1 = (p_slice_end >= Z_SLICES) ? p_zp.z_far : p_zp.z_near * std::exp2(t1 * log2_ratio);
	}
}

_FORCE_INLINE_ bool is_barn_door_active(float p_angle_deg, float p_length) {
	return p_angle_deg < BARN_DOOR_OPEN_ANGLE_DEG && p_length > BARN_DOOR_MIN_LENGTH;
}

_FORCE_INLINE_ bool is_barn_door_active_rad(float p_angle_rad, float p_length) {
	return p_angle_rad < float(Math::deg_to_rad(double(BARN_DOOR_OPEN_ANGLE_DEG))) && p_length > BARN_DOOR_MIN_LENGTH;
}

// Maximum lateral half-extent (along +X or +Y from light center) of an area light's barn-door frustum
// at forward depth p_f >= 0 (clamped to the full range box p_half_size + p_range).
_FORCE_INLINE_ float barn_door_max_extent_at_depth(float p_half_size, float p_range, float p_angle_rad, float p_length, float p_f) {
	const float box_max = p_half_size + p_range;
	if (!is_barn_door_active_rad(p_angle_rad, p_length)) {
		return box_max;
	}
	const float clamped_angle = CLAMP(p_angle_rad, 0.0f, float(Math::PI * 0.5) - 1e-4f);
	const float sin_a = std::sin(clamped_angle);
	const float cos_a = std::cos(clamped_angle);
	const float z_d = p_length * cos_a;
	const float f = MAX(p_f, 0.0f);
	if (z_d <= 1e-6f) {
		return box_max;
	}
	float ext;
	if (f <= z_d) {
		ext = p_half_size + f * (sin_a / cos_a);
	} else {
		ext = -p_half_size + f * ((2.0f * p_half_size + p_length * sin_a) / z_d);
	}
	return MIN(ext, box_max);
}

// Compute tight local-space AABB half-extents (width_half, height_half) for an AreaLight3D with barn doors.
_FORCE_INLINE_ Vector2 area_light_barn_door_aabb_extents(const Vector2 &p_area_size, float p_range, float p_angle_deg, float p_length) {
	const float hw = float(p_area_size.x) * 0.5f;
	const float hh = float(p_area_size.y) * 0.5f;
	const float range = MAX(p_range, 0.0f);
	if (!is_barn_door_active(p_angle_deg, p_length)) {
		return Vector2(hw + range, hh + range);
	}
	const float angle_rad = float(Math::deg_to_rad(double(CLAMP(p_angle_deg, 0.0f, 90.0f))));
	return Vector2(
			barn_door_max_extent_at_depth(hw, range, angle_rad, p_length, range),
			barn_door_max_extent_at_depth(hh, range, angle_rad, p_length, range));
}

// Scalar intensity proxy used for deterministic top-64 ranking on cell overflow.
_FORCE_INLINE_ float compute_light_intensity_proxy(float p_r, float p_g, float p_b) {
	return 0.2126f * std::fabs(p_r) + 0.7152f * std::fabs(p_g) + 0.0722f * std::fabs(p_b);
}

// Strict total order: higher intensity wins; ties break toward lower global light index.
_FORCE_INLINE_ bool is_stronger_light(float p_int_a, uint32_t p_li_a, float p_int_b, uint32_t p_li_b) {
	return (p_int_a > p_int_b) || (p_int_a == p_int_b && p_li_a < p_li_b);
}

_FORCE_INLINE_ bool is_weaker_light(float p_int_a, uint32_t p_li_a, float p_int_b, uint32_t p_li_b) {
	return (p_int_a < p_int_b) || (p_int_a == p_int_b && p_li_a > p_li_b);
}

_FORCE_INLINE_ PackedLightData make_packed_light(
		uint32_t p_type,
		uint32_t p_per_type_index,
		const Transform3D &p_view_from_world,
		const Transform3D &p_light_world_xform,
		float p_range,
		float p_spot_angle_deg,
		const Vector2 &p_area_size,
		float p_barn_door_angle_deg,
		float p_barn_door_length,
		float p_intensity,
		bool p_megalights_handled = false) {
	PackedLightData out = {};

	Transform3D xform = p_view_from_world * p_light_world_xform;
	float scale = float(xform.basis.get_uniform_scale());
	if (p_type != LIGHT_TYPE_AREA) {
		if (scale < 0.98f || scale > 1.02f) {
			xform.basis.orthonormalize();
		} else {
			scale = 1.0f;
		}
	} else {
		scale = 1.0f;
		xform.basis.orthonormalize();
	}

	const Vector3 pos = xform.origin;
	const Vector3 dir = (-xform.basis.get_column(Vector3::AXIS_Z)).normalized();
	const Vector3 ax = xform.basis.get_column(Vector3::AXIS_X).normalized();
	const Vector3 ay = xform.basis.get_column(Vector3::AXIS_Y).normalized();

	out.position[0] = float(pos.x);
	out.position[1] = float(pos.y);
	out.position[2] = float(pos.z);
	out.radius = MAX(0.001f, p_range * scale);

	out.direction[0] = float(dir.x);
	out.direction[1] = float(dir.y);
	out.direction[2] = float(dir.z);

	if (p_type == LIGHT_TYPE_SPOT) {
		const float rad = float(Math::deg_to_rad(double(CLAMP(p_spot_angle_deg, 0.0f, 180.0f))));
		out.cos_spot_angle = std::cos(rad);
		out.axis_x[0] = std::sin(rad);
		out.axis_x[1] = 0.0f;
		out.axis_x[2] = 0.0f;
	} else if (p_type == LIGHT_TYPE_AREA) {
		out.cos_spot_angle = -1.0f;
		out.axis_x[0] = float(ax.x);
		out.axis_x[1] = float(ax.y);
		out.axis_x[2] = float(ax.z);
		out.half_width = MAX(0.0f, float(p_area_size.x) * 0.5f);

		out.axis_y[0] = float(ay.x);
		out.axis_y[1] = float(ay.y);
		out.axis_y[2] = float(ay.z);
		out.half_height = MAX(0.0f, float(p_area_size.y) * 0.5f);

		out.barn_door_angle_rad = float(Math::deg_to_rad(double(CLAMP(p_barn_door_angle_deg, 0.0f, 90.0f))));
		out.barn_door_length = MAX(0.0f, p_barn_door_length);
	} else {
		out.cos_spot_angle = -1.0f;
	}

	out.intensity = MAX(0.0f, p_intensity);
	out.packed_entry = pack_entry(p_type, p_per_type_index, p_megalights_handled);
	return out;
}

// Float32 matrix-vector multiply matching GLSL column-major mat4 * vec4.
_FORCE_INLINE_ Vector4 mul_mat4_f32(const float p_m[16], const Vector4 &p_v) {
	const float x = float(p_v.x);
	const float y = float(p_v.y);
	const float z = float(p_v.z);
	const float w = float(p_v.w);
	return Vector4(
			p_m[0] * x + p_m[4] * y + p_m[8] * z + p_m[12] * w,
			p_m[1] * x + p_m[5] * y + p_m[9] * z + p_m[13] * w,
			p_m[2] * x + p_m[6] * y + p_m[10] * z + p_m[14] * w,
			p_m[3] * x + p_m[7] * y + p_m[11] * z + p_m[15] * w);
}

_FORCE_INLINE_ void store_projection_f32(const Projection &p_proj, float r_out[16]) {
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			r_out[col * 4 + row] = float(p_proj.columns[col][row]);
		}
	}
}

_FORCE_INLINE_ Vector3 unproject_corner_at_depth(const float p_inv_proj[16], float p_ndc_x, float p_ndc_y, float p_depth) {
	const Vector4 p0 = mul_mat4_f32(p_inv_proj, Vector4(p_ndc_x, p_ndc_y, 1.0f, 1.0f));
	const Vector4 p1 = mul_mat4_f32(p_inv_proj, Vector4(p_ndc_x, p_ndc_y, 0.0f, 1.0f));
	const float inv_w0 = 1.0f / float(p0.w);
	const float inv_w1 = 1.0f / float(p1.w);
	const Vector3 v_near(float(p0.x) * inv_w0, float(p0.y) * inv_w0, float(p0.z) * inv_w0);
	const Vector3 v_far(float(p1.x) * inv_w1, float(p1.y) * inv_w1, float(p1.z) * inv_w1);
	const float denom = float(v_near.z - v_far.z);
	const float t = (std::fabs(denom) > 1e-7f) ? ((p_depth + float(v_near.z)) / denom) : 0.0f;
	return Vector3(
			float(v_near.x) + (float(v_far.x) - float(v_near.x)) * t,
			float(v_near.y) + (float(v_far.y) - float(v_near.y)) * t,
			float(v_near.z) + (float(v_far.z) - float(v_near.z)) * t);
}

_FORCE_INLINE_ Vector4 make_outward_plane(const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_c, const Vector3 &p_interior_center) {
	const Vector3 e1 = p_b - p_a;
	const Vector3 e2 = p_c - p_a;
	Vector3 n(
			float(e1.y) * float(e2.z) - float(e1.z) * float(e2.y),
			float(e1.z) * float(e2.x) - float(e1.x) * float(e2.z),
			float(e1.x) * float(e2.y) - float(e1.y) * float(e2.x));
	const float len = std::sqrt(float(n.x) * float(n.x) + float(n.y) * float(n.y) + float(n.z) * float(n.z));
	if (len <= 1e-12f) {
		return Vector4(0.0f, 0.0f, 0.0f, 0.0f);
	}
	const float inv_len = 1.0f / len;
	n = Vector3(float(n.x) * inv_len, float(n.y) * inv_len, float(n.z) * inv_len);
	float d = -(float(n.x) * float(p_a.x) + float(n.y) * float(p_a.y) + float(n.z) * float(p_a.z));
	if (float(n.x) * float(p_interior_center.x) + float(n.y) * float(p_interior_center.y) + float(n.z) * float(p_interior_center.z) + d > 0.0f) {
		n = -n;
		d = -d;
	}
	return Vector4(float(n.x), float(n.y), float(n.z), d);
}

_FORCE_INLINE_ CellBounds compute_cell_bounds(
		const float p_inv_proj[16],
		uint32_t p_screen_w,
		uint32_t p_screen_h,
		uint32_t p_px0,
		uint32_t p_py0,
		uint32_t p_px1,
		uint32_t p_py1,
		float p_z0,
		float p_z1) {
	const float inv_w = 1.0f / float(MAX(p_screen_w, 1u));
	const float inv_h = 1.0f / float(MAX(p_screen_h, 1u));
	const float x0 = float(MIN(p_px0, p_screen_w)) * inv_w * 2.0f - 1.0f;
	const float x1 = float(MIN(p_px1, p_screen_w)) * inv_w * 2.0f - 1.0f;
	const float y0 = float(MIN(p_py0, p_screen_h)) * inv_h * 2.0f - 1.0f;
	const float y1 = float(MIN(p_py1, p_screen_h)) * inv_h * 2.0f - 1.0f;

	const float z0 = p_z0 * SLICE_MARGIN_NEAR;
	const float z1 = p_z1 * SLICE_MARGIN_FAR;

	const Vector3 c[8] = {
		unproject_corner_at_depth(p_inv_proj, x0, y0, z0), // 0: n00
		unproject_corner_at_depth(p_inv_proj, x1, y0, z0), // 1: n10
		unproject_corner_at_depth(p_inv_proj, x0, y1, z0), // 2: n01
		unproject_corner_at_depth(p_inv_proj, x1, y1, z0), // 3: n11
		unproject_corner_at_depth(p_inv_proj, x0, y0, z1), // 4: f00
		unproject_corner_at_depth(p_inv_proj, x1, y0, z1), // 5: f10
		unproject_corner_at_depth(p_inv_proj, x0, y1, z1), // 6: f01
		unproject_corner_at_depth(p_inv_proj, x1, y1, z1), // 7: f11
	};

	Vector3 bmin = c[0];
	Vector3 bmax = c[0];
	Vector3 center = c[0];
	for (int i = 1; i < 8; i++) {
		bmin.x = MIN(bmin.x, c[i].x);
		bmin.y = MIN(bmin.y, c[i].y);
		bmin.z = MIN(bmin.z, c[i].z);
		bmax.x = MAX(bmax.x, c[i].x);
		bmax.y = MAX(bmax.y, c[i].y);
		bmax.z = MAX(bmax.z, c[i].z);
		center += c[i];
	}
	center *= 0.125f;

	CellBounds bounds;
	bounds.bmin = bmin;
	bounds.bmax = bmax;
	bounds.planes[0] = make_outward_plane(c[0], c[2], c[4], center); // Left
	bounds.planes[1] = make_outward_plane(c[1], c[5], c[3], center); // Right
	bounds.planes[2] = make_outward_plane(c[0], c[4], c[1], center); // Top
	bounds.planes[3] = make_outward_plane(c[2], c[3], c[6], center); // Bottom
	return bounds;
}

_FORCE_INLINE_ bool sphere_intersects_cell(const Vector3 &p_pos, float p_radius, const CellBounds &p_cell) {
	const float qx = CLAMP(float(p_pos.x), float(p_cell.bmin.x), float(p_cell.bmax.x));
	const float qy = CLAMP(float(p_pos.y), float(p_cell.bmin.y), float(p_cell.bmax.y));
	const float qz = CLAMP(float(p_pos.z), float(p_cell.bmin.z), float(p_cell.bmax.z));
	const float dx = float(p_pos.x) - qx;
	const float dy = float(p_pos.y) - qy;
	const float dz = float(p_pos.z) - qz;
	if (dx * dx + dy * dy + dz * dz > p_radius * p_radius) {
		return false;
	}
	for (int i = 0; i < 4; i++) {
		const Vector4 &pl = p_cell.planes[i];
		const float dist = float(pl.x) * float(p_pos.x) + float(pl.y) * float(p_pos.y) + float(pl.z) * float(p_pos.z) + float(pl.w);
		if (dist > p_radius) {
			return false;
		}
	}
	return true;
}

_FORCE_INLINE_ bool light_intersects_cell(const PackedLightData &p_light, const CellBounds &p_cell) {
	const uint32_t ltype = unpack_entry_type(p_light.packed_entry);
	const Vector3 pos(p_light.position[0], p_light.position[1], p_light.position[2]);
	const float radius = p_light.radius;

	if (ltype == LIGHT_TYPE_OMNI) {
		return sphere_intersects_cell(pos, radius, p_cell);
	}

	const Vector3 center = (p_cell.bmin + p_cell.bmax) * 0.5f;
	const Vector3 extents = (p_cell.bmax - p_cell.bmin) * 0.5f;
	const Vector3 dir(p_light.direction[0], p_light.direction[1], p_light.direction[2]);

	if (ltype == LIGHT_TYPE_SPOT) {
		if (!sphere_intersects_cell(pos, radius, p_cell)) {
			return false;
		}
		// Wide spotlights (>= 90 deg) illuminate the back hemisphere; bounding sphere is exact.
		if (p_light.cos_spot_angle <= 0.0f) {
			return true;
		}
		const Vector3 rel = center - pos;
		const float proj_r = std::fabs(float(dir.x)) * float(extents.x) + std::fabs(float(dir.y)) * float(extents.y) + std::fabs(float(dir.z)) * float(extents.z);
		const float v1 = float(rel.x) * float(dir.x) + float(rel.y) * float(dir.y) + float(rel.z) * float(dir.z);
		if (v1 + proj_r < 0.0f || v1 - proj_r > radius) {
			return false;
		}
		const float cos_a = p_light.cos_spot_angle;
		const float sin_a = p_light.axis_x[0];
		// 4 tangent planes of the circumscribed cone pyramid (exact on AABB, strictly monotone under box inclusion).
		const Vector3 ref_up = (std::fabs(float(dir.y)) < 0.999f) ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
		const Vector3 cu = dir.cross(ref_up).normalized();
		const Vector3 cv = cu.cross(dir);
		const Vector3 spot_planes[4] = {
			cu * cos_a - dir * sin_a,
			-cu * cos_a - dir * sin_a,
			cv * cos_a - dir * sin_a,
			-cv * cos_a - dir * sin_a,
		};
		for (int i = 0; i < 4; i++) {
			const Vector3 &pn = spot_planes[i];
			const float d_c = float(rel.x) * float(pn.x) + float(rel.y) * float(pn.y) + float(rel.z) * float(pn.z);
			const float d_e = std::fabs(float(pn.x)) * float(extents.x) + std::fabs(float(pn.y)) * float(extents.y) + std::fabs(float(pn.z)) * float(extents.z);
			if (d_c - d_e > 0.0f) {
				return false;
			}
		}
		// Fine cone-vs-bounding-sphere refinement.
		const float bs_r = std::sqrt(float(extents.x) * float(extents.x) + float(extents.y) * float(extents.y) + float(extents.z) * float(extents.z));
		const float v_len_sq = float(rel.x) * float(rel.x) + float(rel.y) * float(rel.y) + float(rel.z) * float(rel.z);
		const float b = std::sqrt(MAX(v_len_sq - v1 * v1, 0.0f));
		if (v1 * cos_a + b * sin_a < 0.0f) {
			return v_len_sq <= bs_r * bs_r;
		}
		const float signed_dist = b * cos_a - v1 * sin_a;
		return signed_dist <= bs_r;
	}

	// LIGHT_TYPE_AREA (rect light with optional barn-door frustum culling).
	const Vector3 ax(p_light.axis_x[0], p_light.axis_x[1], p_light.axis_x[2]);
	const Vector3 ay(p_light.axis_y[0], p_light.axis_y[1], p_light.axis_y[2]);
	const float hw = p_light.half_width;
	const float hh = p_light.half_height;

	// Fast side-plane test using exact support distance of rounded rectangle along each plane normal.
	for (int i = 0; i < 4; i++) {
		const Vector4 &pl = p_cell.planes[i];
		const float support = radius + std::fabs(float(pl.x) * float(ax.x) + float(pl.y) * float(ax.y) + float(pl.z) * float(ax.z)) * hw + std::fabs(float(pl.x) * float(ay.x) + float(pl.y) * float(ay.y) + float(pl.z) * float(ay.z)) * hh;
		const float dist = float(pl.x) * float(pos.x) + float(pl.y) * float(pos.y) + float(pl.z) * float(pos.z) + float(pl.w);
		if (dist > support) {
			return false;
		}
	}

	// Project cell AABB onto the area light's local orthonormal frame (ax, ay, dir).
	const Vector3 rel = center - pos;
	const float f_c = float(rel.x) * float(dir.x) + float(rel.y) * float(dir.y) + float(rel.z) * float(dir.z);
	const float f_e = std::fabs(float(dir.x)) * float(extents.x) + std::fabs(float(dir.y)) * float(extents.y) + std::fabs(float(dir.z)) * float(extents.z);
	const float f_min = f_c - f_e;
	const float f_max = f_c + f_e;
	if (f_max < 0.0f || f_min > radius) {
		return false;
	}

	const float x_c = float(rel.x) * float(ax.x) + float(rel.y) * float(ax.y) + float(rel.z) * float(ax.z);
	const float x_e = std::fabs(float(ax.x)) * float(extents.x) + std::fabs(float(ax.y)) * float(extents.y) + std::fabs(float(ax.z)) * float(extents.z);
	const float x_min = x_c - x_e;
	const float x_max = x_c + x_e;

	const float y_c = float(rel.x) * float(ay.x) + float(rel.y) * float(ay.y) + float(rel.z) * float(ay.z);
	const float y_e = std::fabs(float(ay.x)) * float(extents.x) + std::fabs(float(ay.y)) * float(extents.y) + std::fabs(float(ay.z)) * float(extents.z);
	const float y_min = y_c - y_e;
	const float y_max = y_c + y_e;

	// Closest distance from local cell box to emitter rectangle [-hw, hw] x [-hh, hh] x {0}.
	const float dx = MAX(0.0f, MAX(-hw - x_max, x_min - hw));
	const float dy = MAX(0.0f, MAX(-hh - y_max, y_min - hh));
	const float df = MAX(0.0f, f_min);
	if (dx * dx + dy * dy + df * df > radius * radius) {
		return false;
	}

	// Barn-door frustum culling (4 separating planes in view space).
	if (is_barn_door_active_rad(p_light.barn_door_angle_rad, p_light.barn_door_length)) {
		const float angle = CLAMP(p_light.barn_door_angle_rad, 0.0f, float(Math::PI * 0.5) - 1e-4f);
		const float sin_a = std::sin(angle);
		const float cos_a = std::cos(angle);
		const float z_d = p_light.barn_door_length * cos_a;
		if (z_d > 1e-6f) {
			const float f_hi = f_max;
			const float d_ext = p_light.barn_door_length * sin_a;

			float slope_x;
			float rhs_x;
			float slope_y;
			float rhs_y;
			if (f_hi <= z_d) {
				const float tan_a = sin_a / cos_a;
				slope_x = tan_a;
				rhs_x = hw;
				slope_y = tan_a;
				rhs_y = hh;
			} else if (f_min >= z_d) {
				slope_x = (2.0f * hw + d_ext) / z_d;
				rhs_x = -hw;
				slope_y = (2.0f * hh + d_ext) / z_d;
				rhs_y = -hh;
			} else {
				const float x_at_fhi = -hw + f_hi * ((2.0f * hw + d_ext) / z_d);
				const float y_at_fhi = -hh + f_hi * ((2.0f * hh + d_ext) / z_d);
				slope_x = (x_at_fhi - hw) / f_hi;
				rhs_x = hw;
				slope_y = (y_at_fhi - hh) / f_hi;
				rhs_y = hh;
			}

			auto plane_rejects = [&](const Vector3 &p_n, float p_rhs) -> bool {
				const float d_c = float(rel.x) * float(p_n.x) + float(rel.y) * float(p_n.y) + float(rel.z) * float(p_n.z);
				const float d_e = std::fabs(float(p_n.x)) * float(extents.x) + std::fabs(float(p_n.y)) * float(extents.y) + std::fabs(float(p_n.z)) * float(extents.z);
				return (d_c - d_e) > p_rhs;
			};

			if (plane_rejects(ax - dir * slope_x, rhs_x)) {
				return false;
			}
			if (plane_rejects(-ax - dir * slope_x, rhs_x)) {
				return false;
			}
			if (plane_rejects(ay - dir * slope_y, rhs_y)) {
				return false;
			}
			if (plane_rejects(-ay - dir * slope_y, rhs_y)) {
				return false;
			}
		}
	}

	return true;
}

// CPU brute-force reference builder: tests every light against every fine cell independently
// (no coarse-pass shortcut), keeping top-64 highest-intensity lights on overflow.
struct ReferenceGrid {
	Vector3i grid_dims;
	LocalVector<CellHeader> headers;
	LocalVector<uint32_t> indices;
	GridStats stats;
	uint32_t hiz_occluded_cells = 0;
};

inline ReferenceGrid build_cpu_reference(
		const PackedLightData *p_lights,
		uint32_t p_light_count,
		uint32_t p_omni_count,
		uint32_t p_spot_count,
		const float p_inv_proj[16],
		const Size2i &p_screen_size,
		float p_z_near,
		float p_z_far,
		bool p_orthogonal,
		const Vector2 *p_tile_depths = nullptr) {
	ReferenceGrid ref;
	ref.grid_dims = compute_grid_dims(p_screen_size);
	const uint32_t gx = uint32_t(ref.grid_dims.x);
	const uint32_t gy = uint32_t(ref.grid_dims.y);
	const uint32_t gz = uint32_t(ref.grid_dims.z);
	const uint32_t total_cells = gx * gy * gz;
	ref.headers.resize(total_cells);
	ref.indices.resize(total_cells * MAX_LIGHTS_PER_CELL);

	const ZParams zp = compute_z_params(p_z_near, p_z_far, p_orthogonal);
	const uint32_t screen_w = uint32_t(MAX(p_screen_size.x, 1));
	const uint32_t screen_h = uint32_t(MAX(p_screen_size.y, 1));

	for (uint32_t tz = 0; tz < gz; tz++) {
		float slice_z0 = 0.0f;
		float slice_z1 = 0.0f;
		slice_depth_bounds(tz, tz + 1u, zp, slice_z0, slice_z1);

		for (uint32_t ty = 0; ty < gy; ty++) {
			const uint32_t py0 = ty * TILE_SIZE;
			const uint32_t py1 = MIN(py0 + TILE_SIZE, screen_h);

			for (uint32_t tx = 0; tx < gx; tx++) {
				const uint32_t px0 = tx * TILE_SIZE;
				const uint32_t px1 = MIN(px0 + TILE_SIZE, screen_w);
				const uint32_t ci = cell_index(tx, ty, tz, ref.grid_dims);
				const uint32_t base = ci * MAX_LIGHTS_PER_CELL;

				float z0 = slice_z0;
				float z1 = slice_z1;
				if (p_tile_depths != nullptr) {
					const Vector2 td = p_tile_depths[ty * gx + tx];
					const float min_z = float(td.x);
					const float max_z = float(td.y);
					if (max_z <= 0.0f || slice_z0 > max_z * HIZ_FAR_TOLERANCE || slice_z1 < min_z * HIZ_NEAR_TOLERANCE) {
						ref.headers[ci] = { base, 0u };
						ref.hiz_occluded_cells++;
						continue;
					}
					z0 = MAX(slice_z0, min_z * HIZ_NEAR_TOLERANCE);
					z1 = MIN(slice_z1, max_z * HIZ_FAR_TOLERANCE);
					if (z1 < z0) {
						z1 = z0;
					}
				}

				const CellBounds bounds = compute_cell_bounds(p_inv_proj, screen_w, screen_h, px0, py0, px1, py1, z0, z1);

				uint32_t count = 0u;
				uint32_t overflow = 0u;
				float min_int = 3.402823466e+38f;
				uint32_t min_li = 0u;
				uint32_t min_slot = 0u;

				for (uint32_t li = 0; li < p_light_count; li++) {
					const PackedLightData &L = p_lights[li];
					if (!light_intersects_cell(L, bounds)) {
						continue;
					}
					if (count < MAX_LIGHTS_PER_CELL) {
						ref.indices[base + count] = L.packed_entry;
						if (count == 0u || is_weaker_light(L.intensity, li, min_int, min_li)) {
							min_int = L.intensity;
							min_li = li;
							min_slot = count;
						}
						count++;
					} else {
						overflow++;
						if (is_stronger_light(L.intensity, li, min_int, min_li)) {
							ref.indices[base + min_slot] = L.packed_entry;
							min_int = 3.402823466e+38f;
							min_li = 0u;
							min_slot = 0u;
							for (uint32_t s = 0; s < MAX_LIGHTS_PER_CELL; s++) {
								const uint32_t s_li = entry_to_global_index(ref.indices[base + s], p_omni_count, p_spot_count);
								const float s_int = p_lights[s_li].intensity;
								if (s == 0u || is_weaker_light(s_int, s_li, min_int, min_li)) {
									min_int = s_int;
									min_li = s_li;
									min_slot = s;
								}
							}
						}
					}
				}

				ref.headers[ci] = { base, count };
				if (overflow > 0u) {
					ref.stats.overflow_cells++;
					ref.stats.overflow_lights += overflow;
				}
				ref.stats.total_assignments += count;
				if (count > ref.stats.max_cell_count) {
					ref.stats.max_cell_count = count;
				}
			}
		}
	}

	return ref;
}

// Compare a GPU-built packed grid against the CPU brute-force reference (set comparison per cell).
struct GridDiff {
	uint32_t missing_lights = 0;
	uint32_t extra_lights = 0;
	uint32_t mismatched_cells = 0;
	uint32_t hiz_occluded_nonempty_cells = 0;
};

inline GridDiff compare_grids(
		const ReferenceGrid &p_ref,
		const CellHeader *p_gpu_headers,
		const uint32_t *p_gpu_indices,
		const Vector2 *p_tile_depths,
		float p_z_near,
		float p_z_far,
		bool p_orthogonal) {
	GridDiff diff;
	const uint32_t gx = uint32_t(p_ref.grid_dims.x);
	const uint32_t gy = uint32_t(p_ref.grid_dims.y);
	const uint32_t gz = uint32_t(p_ref.grid_dims.z);
	const ZParams zp = compute_z_params(p_z_near, p_z_far, p_orthogonal);

	for (uint32_t tz = 0; tz < gz; tz++) {
		float slice_z0 = 0.0f;
		float slice_z1 = 0.0f;
		slice_depth_bounds(tz, tz + 1u, zp, slice_z0, slice_z1);

		for (uint32_t ty = 0; ty < gy; ty++) {
			for (uint32_t tx = 0; tx < gx; tx++) {
				const uint32_t ci = cell_index(tx, ty, tz, p_ref.grid_dims);
				const CellHeader ref_h = p_ref.headers[ci];
				const CellHeader gpu_h = p_gpu_headers[ci];
				const uint32_t gpu_count = MIN(gpu_h.count, MAX_LIGHTS_PER_CELL);

				if (p_tile_depths != nullptr) {
					const Vector2 td = p_tile_depths[ty * gx + tx];
					if (float(td.y) <= 0.0f || slice_z0 > float(td.y) * HIZ_FAR_TOLERANCE || slice_z1 < float(td.x) * HIZ_NEAR_TOLERANCE) {
						if (gpu_count > 0u) {
							diff.hiz_occluded_nonempty_cells++;
						}
					}
				}

				uint32_t missing_here = 0u;
				for (uint32_t r = 0; r < ref_h.count; r++) {
					const uint32_t entry = p_ref.indices[ref_h.offset + r];
					bool found = false;
					for (uint32_t g = 0; g < gpu_count; g++) {
						if (p_gpu_indices[gpu_h.offset + g] == entry) {
							found = true;
							break;
						}
					}
					if (!found) {
						missing_here++;
					}
				}

				uint32_t extra_here = 0u;
				for (uint32_t g = 0; g < gpu_count; g++) {
					const uint32_t entry = p_gpu_indices[gpu_h.offset + g];
					bool found = false;
					for (uint32_t r = 0; r < ref_h.count; r++) {
						if (p_ref.indices[ref_h.offset + r] == entry) {
							found = true;
							break;
						}
					}
					if (!found) {
						extra_here++;
					}
				}

				if (missing_here > 0u || extra_here > 0u || gpu_h.count != ref_h.count) {
					diff.mismatched_cells++;
					diff.missing_lights += missing_here;
					diff.extra_lights += extra_here;
				}
			}
		}
	}

	return diff;
}

// One-shot GPU verification check state (armed by RenderingServer.light_grid_request_check()).
struct CheckState {
	bool pending = false;
	Dictionary result;
};

inline CheckState &_check_state() {
	static CheckState state;
	return state;
}

inline void request_check() {
	CheckState &s = _check_state();
	s.pending = true;
	s.result = Dictionary();
}

inline bool consume_check_request() {
	CheckState &s = _check_state();
	if (!s.pending) {
		return false;
	}
	s.pending = false;
	return true;
}

inline void store_check_result(const Dictionary &p_result) {
	_check_state().result = p_result;
}

inline Dictionary get_check_result() {
	return _check_state().result;
}

} // namespace LightGrid

} // namespace RendererRD
