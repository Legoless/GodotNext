/**************************************************************************/
/*  spline_deform.h                                                       */
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

#include "core/math/aabb.h"
#include "core/math/math_funcs.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

#include <cfloat>
#include <cstdint>
#include <cstring>

// GodotNext: spline_meshes (#161, plan §21.4 contract N12).
// Pure CPU reference for cubic Hermite spline deformation. Matches
// servers/rendering/renderer_rd/shaders/spline_deform_inc.glsl identically.

enum SplineDeformAxis : uint32_t {
	SPLINE_DEFORM_AXIS_X = 0u,
	SPLINE_DEFORM_AXIS_Y = 1u,
	SPLINE_DEFORM_AXIS_Z = 2u,
};

enum SplineDeformFlags : uint32_t {
	SPLINE_DEFORM_FLAG_ACTIVE = 1u << 0u,
};

static constexpr uint32_t SPLINE_DEFORM_SWEEP_SAMPLES = 16u;

// std430-compatible per-pose parameters (112 bytes = 7 x vec4).
struct SplineDeformPose {
	float start_pos_roll[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // xyz: start_position, w: start_roll (rad)
	float start_tangent_end_roll[4] = { 0.0f, 0.0f, 1.0f, 0.0f }; // xyz: start_tangent, w: end_roll (rad)
	float end_pos_axis_min[4] = { 0.0f, 0.0f, 1.0f, 0.0f }; // xyz: end_position, w: mesh AABB min on forward_axis
	float end_tangent_axis_max[4] = { 0.0f, 0.0f, 1.0f, 1.0f }; // xyz: end_tangent, w: mesh AABB max on forward_axis
	float up_max_stretch[4] = { 0.0f, 1.0f, 0.0f, 1.0f }; // xyz: normalized up_vector, w: max_stretch (>= 1e-4)
	float scale_start_end[4] = { 1.0f, 1.0f, 1.0f, 1.0f }; // xy: start_scale, zw: end_scale
	uint32_t meta[4] = { SPLINE_DEFORM_AXIS_Z, 0u, 0u, 0u }; // x: forward_axis, y: flags, z/w: pad

	bool operator==(const SplineDeformPose &p_other) const {
		return memcmp(this, &p_other, sizeof(SplineDeformPose)) == 0;
	}
	bool operator!=(const SplineDeformPose &p_other) const {
		return !(*this == p_other);
	}
};

static_assert(sizeof(SplineDeformPose) == 112, "SplineDeformPose must match std430 layout (112 bytes)");

// std430-compatible temporal pair (224 bytes = 14 x vec4).
struct SplineDeformEntry {
	SplineDeformPose current;
	SplineDeformPose previous;
};

static_assert(sizeof(SplineDeformEntry) == 224, "SplineDeformEntry must match std430 layout (224 bytes)");

inline float spline_deform_sanitize_float(float p_v, float p_fallback) {
	return Math::is_finite(p_v) ? p_v : p_fallback;
}

inline Vector3 spline_deform_sanitize_vec3(const Vector3 &p_v, const Vector3 &p_fallback) {
	return Vector3(
			spline_deform_sanitize_float(float(p_v.x), float(p_fallback.x)),
			spline_deform_sanitize_float(float(p_v.y), float(p_fallback.y)),
			spline_deform_sanitize_float(float(p_v.z), float(p_fallback.z)));
}

inline Vector2 spline_deform_sanitize_vec2(const Vector2 &p_v, const Vector2 &p_fallback) {
	return Vector2(
			spline_deform_sanitize_float(float(p_v.x), float(p_fallback.x)),
			spline_deform_sanitize_float(float(p_v.y), float(p_fallback.y)));
}

// Right-handed local axis decomposition matching spline_deform_inc.glsl.
inline void spline_deform_decompose_axis(uint32_t p_axis, const Vector3 &p_v, float &r_along, float &r_u, float &r_v) {
	if (p_axis == SPLINE_DEFORM_AXIS_X) {
		r_along = float(p_v.x);
		r_u = float(-p_v.z);
		r_v = float(p_v.y);
	} else if (p_axis == SPLINE_DEFORM_AXIS_Y) {
		r_along = float(p_v.y);
		r_u = float(p_v.x);
		r_v = float(-p_v.z);
	} else {
		r_along = float(p_v.z);
		r_u = float(p_v.x);
		r_v = float(p_v.y);
	}
}

inline Vector3 spline_deform_default_forward(uint32_t p_axis) {
	if (p_axis == SPLINE_DEFORM_AXIS_X) {
		return Vector3(1.0f, 0.0f, 0.0f);
	}
	if (p_axis == SPLINE_DEFORM_AXIS_Y) {
		return Vector3(0.0f, 1.0f, 0.0f);
	}
	return Vector3(0.0f, 0.0f, 1.0f);
}

inline Vector3 spline_deform_eval_hermite_pos(const SplineDeformPose &p_pose, float p_t) {
	const float t = CLAMP(p_t, 0.0f, 1.0f);
	const float t2 = t * t;
	const float t3 = t2 * t;
	const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
	const float h10 = t3 - 2.0f * t2 + t;
	const float h01 = -2.0f * t3 + 3.0f * t2;
	const float h11 = t3 - t2;

	const Vector3 p0(p_pose.start_pos_roll[0], p_pose.start_pos_roll[1], p_pose.start_pos_roll[2]);
	const Vector3 t0(p_pose.start_tangent_end_roll[0], p_pose.start_tangent_end_roll[1], p_pose.start_tangent_end_roll[2]);
	const Vector3 p1(p_pose.end_pos_axis_min[0], p_pose.end_pos_axis_min[1], p_pose.end_pos_axis_min[2]);
	const Vector3 t1(p_pose.end_tangent_axis_max[0], p_pose.end_tangent_axis_max[1], p_pose.end_tangent_axis_max[2]);
	return p0 * h00 + t0 * h10 + p1 * h01 + t1 * h11;
}

inline Vector3 spline_deform_eval_hermite_deriv(const SplineDeformPose &p_pose, float p_t) {
	const float t = CLAMP(p_t, 0.0f, 1.0f);
	const float t2 = t * t;
	const float dh00 = 6.0f * t2 - 6.0f * t;
	const float dh10 = 3.0f * t2 - 4.0f * t + 1.0f;
	const float dh01 = -6.0f * t2 + 6.0f * t;
	const float dh11 = 3.0f * t2 - 2.0f * t;

	const Vector3 p0(p_pose.start_pos_roll[0], p_pose.start_pos_roll[1], p_pose.start_pos_roll[2]);
	const Vector3 t0(p_pose.start_tangent_end_roll[0], p_pose.start_tangent_end_roll[1], p_pose.start_tangent_end_roll[2]);
	const Vector3 p1(p_pose.end_pos_axis_min[0], p_pose.end_pos_axis_min[1], p_pose.end_pos_axis_min[2]);
	const Vector3 t1(p_pose.end_tangent_axis_max[0], p_pose.end_tangent_axis_max[1], p_pose.end_tangent_axis_max[2]);
	return p0 * dh00 + t0 * dh10 + p1 * dh01 + t1 * dh11;
}

inline float spline_deform_max_second_deriv(const SplineDeformPose &p_pose) {
	const Vector3 p0(p_pose.start_pos_roll[0], p_pose.start_pos_roll[1], p_pose.start_pos_roll[2]);
	const Vector3 t0(p_pose.start_tangent_end_roll[0], p_pose.start_tangent_end_roll[1], p_pose.start_tangent_end_roll[2]);
	const Vector3 p1(p_pose.end_pos_axis_min[0], p_pose.end_pos_axis_min[1], p_pose.end_pos_axis_min[2]);
	const Vector3 t1(p_pose.end_tangent_axis_max[0], p_pose.end_tangent_axis_max[1], p_pose.end_tangent_axis_max[2]);
	// H''(t) is affine in t, so its maximum magnitude on [0, 1] occurs at t = 0 or t = 1.
	const Vector3 h2_0 = p0 * -6.0f + t0 * -4.0f + p1 * 6.0f + t1 * -2.0f;
	const Vector3 h2_1 = p0 * 6.0f + t0 * 2.0f + p1 * -6.0f + t1 * 4.0f;
	return MAX(float(h2_0.length()), float(h2_1.length()));
}

inline void spline_deform_eval_frame(const SplineDeformPose &p_pose, float p_t, Vector3 &r_pos, Vector3 &r_right, Vector3 &r_up, Vector3 &r_forward, Vector2 &r_scale, float &r_deriv_len) {
	const float t = CLAMP(p_t, 0.0f, 1.0f);
	r_pos = spline_deform_eval_hermite_pos(p_pose, t);
	Vector3 deriv = spline_deform_eval_hermite_deriv(p_pose, t);
	r_deriv_len = float(deriv.length());

	Vector3 forward;
	if (r_deriv_len > 1e-6f) {
		forward = deriv / r_deriv_len;
	} else {
		const Vector3 p0(p_pose.start_pos_roll[0], p_pose.start_pos_roll[1], p_pose.start_pos_roll[2]);
		const Vector3 t0(p_pose.start_tangent_end_roll[0], p_pose.start_tangent_end_roll[1], p_pose.start_tangent_end_roll[2]);
		const Vector3 p1(p_pose.end_pos_axis_min[0], p_pose.end_pos_axis_min[1], p_pose.end_pos_axis_min[2]);
		const Vector3 t1(p_pose.end_tangent_axis_max[0], p_pose.end_tangent_axis_max[1], p_pose.end_tangent_axis_max[2]);
		const Vector3 chord = p1 - p0;
		if (chord.length_squared() > 1e-12f) {
			forward = chord.normalized();
		} else if (t0.length_squared() > 1e-12f) {
			forward = t0.normalized();
		} else if (t1.length_squared() > 1e-12f) {
			forward = t1.normalized();
		} else {
			forward = spline_deform_default_forward(p_pose.meta[0]);
		}
	}

	Vector3 ref_up(p_pose.up_max_stretch[0], p_pose.up_max_stretch[1], p_pose.up_max_stretch[2]);
	if (ref_up.length_squared() < 1e-10f) {
		ref_up = Vector3(0.0f, 1.0f, 0.0f);
	} else {
		ref_up.normalize();
	}
	if (Math::abs(float(forward.dot(ref_up))) > 0.999f) {
		ref_up = Math::abs(float(forward.y)) < 0.9f ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(0.0f, 0.0f, -1.0f);
	}

	const Vector3 base_right = ref_up.cross(forward).normalized();
	const Vector3 base_up = forward.cross(base_right);

	const float roll = Math::lerp(p_pose.start_pos_roll[3], p_pose.start_tangent_end_roll[3], t);
	const float cr = Math::cos(roll);
	const float sr = Math::sin(roll);

	r_right = base_right * cr + base_up * sr;
	r_up = base_right * -sr + base_up * cr;
	r_forward = forward;
	r_scale = Vector2(
			Math::lerp(p_pose.scale_start_end[0], p_pose.scale_start_end[2], t),
			Math::lerp(p_pose.scale_start_end[1], p_pose.scale_start_end[3], t));
}

inline Vector3 spline_deform_position(const SplineDeformPose &p_pose, const Vector3 &p_local_pos) {
	float along = 0.0f;
	float u = 0.0f;
	float v = 0.0f;
	spline_deform_decompose_axis(p_pose.meta[0], p_local_pos, along, u, v);
	const float axis_min = p_pose.end_pos_axis_min[3];
	const float axis_max = p_pose.end_tangent_axis_max[3];
	const float extent = axis_max - axis_min;
	const float t = extent > 1e-6f ? CLAMP((along - axis_min) / extent, 0.0f, 1.0f) : 0.0f;

	Vector3 curve_pos;
	Vector3 right;
	Vector3 up;
	Vector3 forward;
	Vector2 scale;
	float deriv_len = 1.0f;
	spline_deform_eval_frame(p_pose, t, curve_pos, right, up, forward, scale, deriv_len);
	return curve_pos + right * (u * float(scale.x)) + up * (v * float(scale.y));
}

inline void spline_deform_vertex(const SplineDeformPose &p_pose, Vector3 &r_pos, Vector3 *r_normal = nullptr, Vector3 *r_tangent = nullptr, Vector3 *r_binormal = nullptr) {
	float along = 0.0f;
	float u = 0.0f;
	float v = 0.0f;
	spline_deform_decompose_axis(p_pose.meta[0], r_pos, along, u, v);
	const float axis_min = p_pose.end_pos_axis_min[3];
	const float axis_max = p_pose.end_tangent_axis_max[3];
	const float extent = axis_max - axis_min;
	const float t = extent > 1e-6f ? CLAMP((along - axis_min) / extent, 0.0f, 1.0f) : 0.0f;

	Vector3 curve_pos;
	Vector3 right;
	Vector3 up;
	Vector3 forward;
	Vector2 scale;
	float deriv_len = 1.0f;
	spline_deform_eval_frame(p_pose, t, curve_pos, right, up, forward, scale, deriv_len);

	r_pos = curve_pos + right * (u * float(scale.x)) + up * (v * float(scale.y));

	const float f_scale = MAX(extent > 1e-6f ? (deriv_len / extent) : 1.0f, 1e-5f);
	const float sx = Math::abs(float(scale.x)) > 1e-5f ? float(scale.x) : (scale.x < 0.0f ? -1e-5f : 1e-5f);
	const float sy = Math::abs(float(scale.y)) > 1e-5f ? float(scale.y) : (scale.y < 0.0f ? -1e-5f : 1e-5f);

	if (r_normal != nullptr) {
		float na = 0.0f;
		float nu = 0.0f;
		float nv = 0.0f;
		spline_deform_decompose_axis(p_pose.meta[0], *r_normal, na, nu, nv);
		Vector3 dn = right * (nu / sx) + up * (nv / sy) + forward * (na / f_scale);
		const float len_sq = float(dn.length_squared());
		*r_normal = len_sq > 1e-12f ? (dn / Math::sqrt(len_sq)) : forward;
	}
	if (r_tangent != nullptr) {
		float ta = 0.0f;
		float tu = 0.0f;
		float tv = 0.0f;
		spline_deform_decompose_axis(p_pose.meta[0], *r_tangent, ta, tu, tv);
		Vector3 dt = right * (tu * float(scale.x)) + up * (tv * float(scale.y)) + forward * (ta * f_scale);
		const float len_sq = float(dt.length_squared());
		*r_tangent = len_sq > 1e-12f ? (dt / Math::sqrt(len_sq)) : right;
	}
	if (r_binormal != nullptr) {
		float ba = 0.0f;
		float bu = 0.0f;
		float bv = 0.0f;
		spline_deform_decompose_axis(p_pose.meta[0], *r_binormal, ba, bu, bv);
		Vector3 db = right * (bu * float(scale.x)) + up * (bv * float(scale.y)) + forward * (ba * f_scale);
		const float len_sq = float(db.length_squared());
		*r_binormal = len_sq > 1e-12f ? (db / Math::sqrt(len_sq)) : up;
	}
}

inline float spline_deform_compute_max_stretch(const SplineDeformPose &p_pose) {
	const float extent = MAX(p_pose.end_tangent_axis_max[3] - p_pose.end_pos_axis_min[3], 1e-6f);
	float max_stretch = 1e-4f;
	for (uint32_t i = 0; i < SPLINE_DEFORM_SWEEP_SAMPLES; i++) {
		const float t = float(i) / float(SPLINE_DEFORM_SWEEP_SAMPLES - 1u);
		const float deriv_len = float(spline_deform_eval_hermite_deriv(p_pose, t).length());
		const float along_stretch = deriv_len / extent;
		const float sx = Math::abs(Math::lerp(p_pose.scale_start_end[0], p_pose.scale_start_end[2], t));
		const float sy = Math::abs(Math::lerp(p_pose.scale_start_end[1], p_pose.scale_start_end[3], t));
		max_stretch = MAX(max_stretch, MAX(along_stretch, MAX(sx, sy)));
	}
	return max_stretch;
}

inline void spline_deform_apply_mesh_aabb(SplineDeformPose &r_pose, const AABB &p_mesh_aabb) {
	const uint32_t axis = r_pose.meta[0] <= SPLINE_DEFORM_AXIS_Z ? r_pose.meta[0] : SPLINE_DEFORM_AXIS_Z;
	float axis_min = 0.0f;
	float axis_max = 1.0f;
	if (p_mesh_aabb.size[axis] > 1e-6f) {
		axis_min = float(p_mesh_aabb.position[axis]);
		axis_max = float(p_mesh_aabb.position[axis] + p_mesh_aabb.size[axis]);
	} else {
		axis_min = float(p_mesh_aabb.position[axis]);
		axis_max = axis_min + 1.0f;
	}
	r_pose.end_pos_axis_min[3] = axis_min;
	r_pose.end_tangent_axis_max[3] = axis_max;
	r_pose.up_max_stretch[3] = spline_deform_compute_max_stretch(r_pose);
}

// 16-sample swept AABB along the Hermite segment x max scale x mesh cross-section bounds,
// inflated by exact cubic chord sag and inter-sample roll rotation sag so every deformed
// vertex is guaranteed inside the returned AABB.
inline AABB spline_deform_compute_swept_aabb(const SplineDeformPose &p_pose, const AABB &p_local_aabb) {
	const uint32_t axis = p_pose.meta[0] <= SPLINE_DEFORM_AXIS_Z ? p_pose.meta[0] : SPLINE_DEFORM_AXIS_Z;
	const Vector3 bmin = p_local_aabb.position;
	const Vector3 bmax = p_local_aabb.position + p_local_aabb.size;

	float a0, u0, v0;
	float a1, u1, v1;
	spline_deform_decompose_axis(axis, bmin, a0, u0, v0);
	spline_deform_decompose_axis(axis, bmax, a1, u1, v1);
	const float u_min = MIN(u0, u1);
	const float u_max = MAX(u0, u1);
	const float v_min = MIN(v0, v1);
	const float v_max = MAX(v0, v1);
	const float u_vals[2] = { u_min, u_max };
	const float v_vals[2] = { v_min, v_max };

	const float extent = p_pose.end_tangent_axis_max[3] - p_pose.end_pos_axis_min[3];
	const float a_min = MIN(a0, a1);
	const float a_max = MAX(a0, a1);
	const float t_start = extent > 1e-6f ? CLAMP((a_min - p_pose.end_pos_axis_min[3]) / extent, 0.0f, 1.0f) : 0.0f;
	const float t_end = extent > 1e-6f ? CLAMP((a_max - p_pose.end_pos_axis_min[3]) / extent, 0.0f, 1.0f) : 0.0f;

	Vector3 out_min(FLT_MAX, FLT_MAX, FLT_MAX);
	Vector3 out_max(-FLT_MAX, -FLT_MAX, -FLT_MAX);
	float max_scale_abs = 0.0f;

	for (uint32_t i = 0; i < SPLINE_DEFORM_SWEEP_SAMPLES; i++) {
		const float frac = float(i) / float(SPLINE_DEFORM_SWEEP_SAMPLES - 1u);
		const float t = Math::lerp(t_start, t_end, frac);
		Vector3 curve_pos;
		Vector3 right;
		Vector3 up;
		Vector3 forward;
		Vector2 scale;
		float deriv_len = 1.0f;
		spline_deform_eval_frame(p_pose, t, curve_pos, right, up, forward, scale, deriv_len);
		max_scale_abs = MAX(max_scale_abs, MAX(Math::abs(float(scale.x)), Math::abs(float(scale.y))));
		for (int ui = 0; ui < 2; ui++) {
			for (int vi = 0; vi < 2; vi++) {
				const Vector3 corner = curve_pos + right * (u_vals[ui] * float(scale.x)) + up * (v_vals[vi] * float(scale.y));
				out_min = out_min.min(corner);
				out_max = out_max.max(corner);
			}
		}
	}

	// Exact inter-sample chord sag for the cubic curve plus cross-section rotation sag.
	const float dt = (t_end - t_start) / float(SPLINE_DEFORM_SWEEP_SAMPLES - 1u);
	const float curve_sag = 0.125f * dt * dt * spline_deform_max_second_deriv(p_pose);
	const float max_u = MAX(Math::abs(u_min), Math::abs(u_max));
	const float max_v = MAX(Math::abs(v_min), Math::abs(v_max));
	const float cross_radius = Math::sqrt(max_u * max_u + max_v * max_v) * max_scale_abs;
	const float step_roll = Math::abs(p_pose.start_tangent_end_roll[3] - p_pose.start_pos_roll[3]) * dt;
	const float rot_sag = cross_radius * (step_roll >= Math::PI ? 2.0f : (1.0f - Math::cos(0.5f * step_roll))) + cross_radius * 0.25f * dt;
	const float margin = curve_sag + rot_sag + 1e-4f;
	out_min -= Vector3(margin, margin, margin);
	out_max += Vector3(margin, margin, margin);

	return AABB(out_min, (out_max - out_min).max(Vector3(0.0f, 0.0f, 0.0f)));
}

// Conservative bounding sphere deformation matching spline_deform_inc.glsl.
inline void spline_deform_bounds_sphere(const SplineDeformPose &p_pose, Vector3 &r_center, float &r_radius) {
	float along_c = 0.0f;
	float u_c = 0.0f;
	float v_c = 0.0f;
	spline_deform_decompose_axis(p_pose.meta[0], r_center, along_c, u_c, v_c);
	const float axis_min = p_pose.end_pos_axis_min[3];
	const float axis_max = p_pose.end_tangent_axis_max[3];
	const float extent = MAX(axis_max - axis_min, 1e-6f);
	const float t0 = CLAMP((along_c - r_radius - axis_min) / extent, 0.0f, 1.0f);
	const float t1 = CLAMP((along_c + r_radius - axis_min) / extent, 0.0f, 1.0f);
	const float tm = 0.5f * (t0 + t1);
	const float dt = t1 - t0;

	Vector3 c0, r0, u0, f0;
	Vector2 s0;
	float d0 = 0.0f;
	spline_deform_eval_frame(p_pose, t0, c0, r0, u0, f0, s0, d0);
	const Vector3 p0 = c0 + r0 * (u_c * float(s0.x)) + u0 * (v_c * float(s0.y));

	Vector3 cm, rm, um, fm;
	Vector2 sm;
	float dm = 0.0f;
	spline_deform_eval_frame(p_pose, tm, cm, rm, um, fm, sm, dm);
	const Vector3 pm = cm + rm * (u_c * float(sm.x)) + um * (v_c * float(sm.y));

	Vector3 c1, r1, u1, f1;
	Vector2 s1;
	float d1 = 0.0f;
	spline_deform_eval_frame(p_pose, t1, c1, r1, u1, f1, s1, d1);
	const Vector3 p1 = c1 + r1 * (u_c * float(s1.x)) + u1 * (v_c * float(s1.y));

	const Vector3 bmin = p0.min(pm.min(p1));
	const Vector3 bmax = p0.max(pm.max(p1));
	const Vector3 new_center = (bmin + bmax) * 0.5f;
	const float span_r = MAX(float((p0 - new_center).length()), MAX(float((pm - new_center).length()), float((p1 - new_center).length())));

	const float max_h2 = spline_deform_max_second_deriv(p_pose);
	const float curve_sag = 0.125f * dt * dt * max_h2;
	const float r_cross = Math::sqrt(u_c * u_c + v_c * v_c);
	const float max_s = MAX(
			MAX(Math::abs(p_pose.scale_start_end[0]), Math::abs(p_pose.scale_start_end[1])),
			MAX(Math::abs(p_pose.scale_start_end[2]), Math::abs(p_pose.scale_start_end[3])));
	const float frame_diff = MAX(
			float((r0 - rm).length() + (u0 - um).length()),
			float((r1 - rm).length() + (u1 - um).length()));
	const float roll_span = Math::abs(p_pose.start_tangent_end_roll[3] - p_pose.start_pos_roll[3]) * dt;
	const float rot_margin = r_cross * max_s * MIN(2.0f, MAX(frame_diff, 0.5f * roll_span) + 0.5f * dt);

	r_center = new_center;
	r_radius = span_r + r_radius * p_pose.up_max_stretch[3] + curve_sag + rot_margin + 1e-4f;
}

// Monotonic curvature- and roll-induced chord sag for a DAG node/group of undeformed radius
// p_raw_radius. Guarantees curved/twisted splines refine planar subdivided faces whose undeformed
// simplification error is zero while staying 0 on straight unrolled splines.
inline float spline_deform_curvature_error(const SplineDeformPose &p_pose, float p_raw_radius) {
	const float extent = MAX(p_pose.end_tangent_axis_max[3] - p_pose.end_pos_axis_min[3], 1e-6f);
	const float r = MAX(p_raw_radius, 0.0f);
	const float dt = CLAMP((2.0f * r) / extent, 0.0f, 1.0f);
	const float curve_sag = 0.125f * dt * dt * spline_deform_max_second_deriv(p_pose);
	const float roll_delta = Math::abs(p_pose.start_tangent_end_roll[3] - p_pose.start_pos_roll[3]);
	const float scale_max = MAX(
			MAX(Math::abs(p_pose.scale_start_end[0]), Math::abs(p_pose.scale_start_end[1])),
			MAX(Math::abs(p_pose.scale_start_end[2]), Math::abs(p_pose.scale_start_end[3])));
	const float roll_sag = 0.125f * dt * dt * roll_delta * roll_delta * r * scale_max;
	return curve_sag + roll_sag;
}

inline SplineDeformPose spline_deform_pose_from_dictionary(const Dictionary &p_params, bool &r_active) {
	SplineDeformPose pose;
	if (p_params.is_empty()) {
		r_active = false;
		pose.meta[1] = 0u;
		return pose;
	}
	r_active = true;
	const Vector3 start_pos = spline_deform_sanitize_vec3(p_params.get("start_position", Vector3(0.0f, 0.0f, 0.0f)), Vector3(0.0f, 0.0f, 0.0f));
	const Vector3 start_tan = spline_deform_sanitize_vec3(p_params.get("start_tangent", Vector3(0.0f, 0.0f, 1.0f)), Vector3(0.0f, 0.0f, 1.0f));
	const Vector3 end_pos = spline_deform_sanitize_vec3(p_params.get("end_position", Vector3(0.0f, 0.0f, 1.0f)), Vector3(0.0f, 0.0f, 1.0f));
	const Vector3 end_tan = spline_deform_sanitize_vec3(p_params.get("end_tangent", Vector3(0.0f, 0.0f, 1.0f)), Vector3(0.0f, 0.0f, 1.0f));
	const float start_roll = spline_deform_sanitize_float(float(p_params.get("start_roll", 0.0)), 0.0f);
	const float end_roll = spline_deform_sanitize_float(float(p_params.get("end_roll", 0.0)), 0.0f);
	const Vector2 start_scale = spline_deform_sanitize_vec2(p_params.get("start_scale", Vector2(1.0f, 1.0f)), Vector2(1.0f, 1.0f));
	const Vector2 end_scale = spline_deform_sanitize_vec2(p_params.get("end_scale", Vector2(1.0f, 1.0f)), Vector2(1.0f, 1.0f));
	const int axis_raw = int(p_params.get("forward_axis", int(SPLINE_DEFORM_AXIS_Z)));
	const uint32_t axis = uint32_t(CLAMP(axis_raw, int(SPLINE_DEFORM_AXIS_X), int(SPLINE_DEFORM_AXIS_Z)));
	Vector3 up_vec = spline_deform_sanitize_vec3(p_params.get("up_vector", Vector3(0.0f, 1.0f, 0.0f)), Vector3(0.0f, 1.0f, 0.0f));
	if (up_vec.length_squared() < 1e-10f) {
		up_vec = Vector3(0.0f, 1.0f, 0.0f);
	} else {
		up_vec.normalize();
	}

	pose.start_pos_roll[0] = float(start_pos.x);
	pose.start_pos_roll[1] = float(start_pos.y);
	pose.start_pos_roll[2] = float(start_pos.z);
	pose.start_pos_roll[3] = start_roll;

	pose.start_tangent_end_roll[0] = float(start_tan.x);
	pose.start_tangent_end_roll[1] = float(start_tan.y);
	pose.start_tangent_end_roll[2] = float(start_tan.z);
	pose.start_tangent_end_roll[3] = end_roll;

	pose.end_pos_axis_min[0] = float(end_pos.x);
	pose.end_pos_axis_min[1] = float(end_pos.y);
	pose.end_pos_axis_min[2] = float(end_pos.z);
	pose.end_pos_axis_min[3] = 0.0f;

	pose.end_tangent_axis_max[0] = float(end_tan.x);
	pose.end_tangent_axis_max[1] = float(end_tan.y);
	pose.end_tangent_axis_max[2] = float(end_tan.z);
	pose.end_tangent_axis_max[3] = 1.0f;

	pose.up_max_stretch[0] = float(up_vec.x);
	pose.up_max_stretch[1] = float(up_vec.y);
	pose.up_max_stretch[2] = float(up_vec.z);
	pose.up_max_stretch[3] = 1.0f;

	pose.scale_start_end[0] = float(start_scale.x);
	pose.scale_start_end[1] = float(start_scale.y);
	pose.scale_start_end[2] = float(end_scale.x);
	pose.scale_start_end[3] = float(end_scale.y);

	pose.meta[0] = axis;
	pose.meta[1] = SPLINE_DEFORM_FLAG_ACTIVE;
	pose.meta[2] = 0u;
	pose.meta[3] = 0u;

	pose.up_max_stretch[3] = spline_deform_compute_max_stretch(pose);
	return pose;
}
