// GodotNext: spline_meshes (#161, plan §21.4 contract N12).
// Shared cubic Hermite spline deformation math for legacy Forward+, NextGeo
// cull/raster/resolve/export, and VSM rasterizers. Matches
// servers/rendering/storage/spline_deform.h identically.

#ifndef SPLINE_DEFORM_INC_GLSL
#define SPLINE_DEFORM_INC_GLSL

const uint SPLINE_DEFORM_AXIS_X = 0u;
const uint SPLINE_DEFORM_AXIS_Y = 1u;
const uint SPLINE_DEFORM_AXIS_Z = 2u;
const uint SPLINE_DEFORM_FLAG_ACTIVE = 1u;

// 112 bytes (7 x vec4), matching SplineDeformPose in spline_deform.h.
struct SplineDeformPose {
	vec4 start_pos_roll; // xyz: start_position, w: start_roll (rad)
	vec4 start_tangent_end_roll; // xyz: start_tangent, w: end_roll (rad)
	vec4 end_pos_axis_min; // xyz: end_position, w: mesh AABB min along forward_axis
	vec4 end_tangent_axis_max; // xyz: end_tangent, w: mesh AABB max along forward_axis
	vec4 up_max_stretch; // xyz: normalized up_vector, w: max_stretch (>= 1e-4)
	vec4 scale_start_end; // xy: start_scale, zw: end_scale
	uvec4 meta; // x: forward_axis (0=X,1=Y,2=Z), y: flags, z/w: pad
};

// 224 bytes (14 x vec4), matching SplineDeformEntry in spline_deform.h.
struct SplineDeformEntry {
	SplineDeformPose current;
	SplineDeformPose previous;
};

void spline_deform_decompose_axis(uint p_axis, vec3 p_v, out float r_along, out float r_u, out float r_v) {
	if (p_axis == SPLINE_DEFORM_AXIS_X) {
		r_along = p_v.x;
		r_u = -p_v.z;
		r_v = p_v.y;
	} else if (p_axis == SPLINE_DEFORM_AXIS_Y) {
		r_along = p_v.y;
		r_u = p_v.x;
		r_v = -p_v.z;
	} else {
		r_along = p_v.z;
		r_u = p_v.x;
		r_v = p_v.y;
	}
}

vec3 spline_deform_default_forward(uint p_axis) {
	if (p_axis == SPLINE_DEFORM_AXIS_X) {
		return vec3(1.0, 0.0, 0.0);
	}
	if (p_axis == SPLINE_DEFORM_AXIS_Y) {
		return vec3(0.0, 1.0, 0.0);
	}
	return vec3(0.0, 0.0, 1.0);
}

vec3 spline_deform_eval_hermite_pos(SplineDeformPose p_pose, float p_t) {
	float t = clamp(p_t, 0.0, 1.0);
	float t2 = t * t;
	float t3 = t2 * t;
	float h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
	float h10 = t3 - 2.0 * t2 + t;
	float h01 = -2.0 * t3 + 3.0 * t2;
	float h11 = t3 - t2;
	return p_pose.start_pos_roll.xyz * h00 +
			p_pose.start_tangent_end_roll.xyz * h10 +
			p_pose.end_pos_axis_min.xyz * h01 +
			p_pose.end_tangent_axis_max.xyz * h11;
}

vec3 spline_deform_eval_hermite_deriv(SplineDeformPose p_pose, float p_t) {
	float t = clamp(p_t, 0.0, 1.0);
	float t2 = t * t;
	float dh00 = 6.0 * t2 - 6.0 * t;
	float dh10 = 3.0 * t2 - 4.0 * t + 1.0;
	float dh01 = -6.0 * t2 + 6.0 * t;
	float dh11 = 3.0 * t2 - 2.0 * t;
	return p_pose.start_pos_roll.xyz * dh00 +
			p_pose.start_tangent_end_roll.xyz * dh10 +
			p_pose.end_pos_axis_min.xyz * dh01 +
			p_pose.end_tangent_axis_max.xyz * dh11;
}

float spline_deform_max_second_deriv(SplineDeformPose p_pose) {
	vec3 p0 = p_pose.start_pos_roll.xyz;
	vec3 t0 = p_pose.start_tangent_end_roll.xyz;
	vec3 p1 = p_pose.end_pos_axis_min.xyz;
	vec3 t1 = p_pose.end_tangent_axis_max.xyz;
	vec3 h2_0 = -6.0 * p0 - 4.0 * t0 + 6.0 * p1 - 2.0 * t1;
	vec3 h2_1 = 6.0 * p0 + 2.0 * t0 - 6.0 * p1 + 4.0 * t1;
	return max(length(h2_0), length(h2_1));
}

void spline_deform_eval_frame(SplineDeformPose p_pose, float p_t, out vec3 r_pos, out vec3 r_right, out vec3 r_up, out vec3 r_forward, out vec2 r_scale, out float r_deriv_len) {
	float t = clamp(p_t, 0.0, 1.0);
	r_pos = spline_deform_eval_hermite_pos(p_pose, t);
	vec3 deriv = spline_deform_eval_hermite_deriv(p_pose, t);
	r_deriv_len = length(deriv);

	vec3 forward;
	if (r_deriv_len > 1e-6) {
		forward = deriv / r_deriv_len;
	} else {
		vec3 chord = p_pose.end_pos_axis_min.xyz - p_pose.start_pos_roll.xyz;
		if (dot(chord, chord) > 1e-12) {
			forward = normalize(chord);
		} else if (dot(p_pose.start_tangent_end_roll.xyz, p_pose.start_tangent_end_roll.xyz) > 1e-12) {
			forward = normalize(p_pose.start_tangent_end_roll.xyz);
		} else if (dot(p_pose.end_tangent_axis_max.xyz, p_pose.end_tangent_axis_max.xyz) > 1e-12) {
			forward = normalize(p_pose.end_tangent_axis_max.xyz);
		} else {
			forward = spline_deform_default_forward(p_pose.meta.x);
		}
	}

	vec3 ref_up = p_pose.up_max_stretch.xyz;
	if (dot(ref_up, ref_up) < 1e-10) {
		ref_up = vec3(0.0, 1.0, 0.0);
	} else {
		ref_up = normalize(ref_up);
	}
	if (abs(dot(forward, ref_up)) > 0.999) {
		ref_up = abs(forward.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, -1.0);
	}

	vec3 base_right = normalize(cross(ref_up, forward));
	vec3 base_up = cross(forward, base_right);

	float roll = mix(p_pose.start_pos_roll.w, p_pose.start_tangent_end_roll.w, t);
	float cr = cos(roll);
	float sr = sin(roll);

	r_right = base_right * cr + base_up * sr;
	r_up = -base_right * sr + base_up * cr;
	r_forward = forward;
	r_scale = mix(p_pose.scale_start_end.xy, p_pose.scale_start_end.zw, t);
}

vec3 spline_deform_position(SplineDeformPose p_pose, vec3 p_local_pos) {
	float along;
	float u;
	float v;
	spline_deform_decompose_axis(p_pose.meta.x, p_local_pos, along, u, v);
	float axis_min = p_pose.end_pos_axis_min.w;
	float axis_max = p_pose.end_tangent_axis_max.w;
	float extent = axis_max - axis_min;
	float t = extent > 1e-6 ? clamp((along - axis_min) / extent, 0.0, 1.0) : 0.0;

	vec3 curve_pos;
	vec3 right;
	vec3 up;
	vec3 forward;
	vec2 scale;
	float deriv_len;
	spline_deform_eval_frame(p_pose, t, curve_pos, right, up, forward, scale, deriv_len);
	return curve_pos + right * (u * scale.x) + up * (v * scale.y);
}

void spline_deform_vertex_pos_nrm(SplineDeformPose p_pose, inout vec3 r_pos, inout vec3 r_normal) {
	float along;
	float u;
	float v;
	spline_deform_decompose_axis(p_pose.meta.x, r_pos, along, u, v);
	float axis_min = p_pose.end_pos_axis_min.w;
	float axis_max = p_pose.end_tangent_axis_max.w;
	float extent = axis_max - axis_min;
	float t = extent > 1e-6 ? clamp((along - axis_min) / extent, 0.0, 1.0) : 0.0;

	vec3 curve_pos;
	vec3 right;
	vec3 up;
	vec3 forward;
	vec2 scale;
	float deriv_len;
	spline_deform_eval_frame(p_pose, t, curve_pos, right, up, forward, scale, deriv_len);
	r_pos = curve_pos + right * (u * scale.x) + up * (v * scale.y);

	float f_scale = max(extent > 1e-6 ? (deriv_len / extent) : 1.0, 1e-5);
	float sx = abs(scale.x) > 1e-5 ? scale.x : (scale.x < 0.0 ? -1e-5 : 1e-5);
	float sy = abs(scale.y) > 1e-5 ? scale.y : (scale.y < 0.0 ? -1e-5 : 1e-5);

	float na;
	float nu;
	float nv;
	spline_deform_decompose_axis(p_pose.meta.x, r_normal, na, nu, nv);
	vec3 dn = right * (nu / sx) + up * (nv / sy) + forward * (na / f_scale);
	float len_sq = dot(dn, dn);
	r_normal = len_sq > 1e-12 ? (dn * inversesqrt(len_sq)) : forward;
}

void spline_deform_vertex_full(SplineDeformPose p_pose, inout vec3 r_pos, inout vec3 r_normal, inout vec3 r_tangent, inout vec3 r_binormal) {
	float along;
	float u;
	float v;
	spline_deform_decompose_axis(p_pose.meta.x, r_pos, along, u, v);
	float axis_min = p_pose.end_pos_axis_min.w;
	float axis_max = p_pose.end_tangent_axis_max.w;
	float extent = axis_max - axis_min;
	float t = extent > 1e-6 ? clamp((along - axis_min) / extent, 0.0, 1.0) : 0.0;

	vec3 curve_pos;
	vec3 right;
	vec3 up;
	vec3 forward;
	vec2 scale;
	float deriv_len;
	spline_deform_eval_frame(p_pose, t, curve_pos, right, up, forward, scale, deriv_len);
	r_pos = curve_pos + right * (u * scale.x) + up * (v * scale.y);

	float f_scale = max(extent > 1e-6 ? (deriv_len / extent) : 1.0, 1e-5);
	float sx = abs(scale.x) > 1e-5 ? scale.x : (scale.x < 0.0 ? -1e-5 : 1e-5);
	float sy = abs(scale.y) > 1e-5 ? scale.y : (scale.y < 0.0 ? -1e-5 : 1e-5);

	float na;
	float nu;
	float nv;
	spline_deform_decompose_axis(p_pose.meta.x, r_normal, na, nu, nv);
	vec3 dn = right * (nu / sx) + up * (nv / sy) + forward * (na / f_scale);
	float n_len_sq = dot(dn, dn);
	r_normal = n_len_sq > 1e-12 ? (dn * inversesqrt(n_len_sq)) : forward;

	float ta;
	float tu;
	float tv;
	spline_deform_decompose_axis(p_pose.meta.x, r_tangent, ta, tu, tv);
	vec3 dt = right * (tu * scale.x) + up * (tv * scale.y) + forward * (ta * f_scale);
	float t_len_sq = dot(dt, dt);
	r_tangent = t_len_sq > 1e-12 ? (dt * inversesqrt(t_len_sq)) : right;

	float ba;
	float bu;
	float bv;
	spline_deform_decompose_axis(p_pose.meta.x, r_binormal, ba, bu, bv);
	vec3 db = right * (bu * scale.x) + up * (bv * scale.y) + forward * (ba * f_scale);
	float b_len_sq = dot(db, db);
	r_binormal = b_len_sq > 1e-12 ? (db * inversesqrt(b_len_sq)) : up;
}

void spline_deform_vertex_pos_tan(SplineDeformPose p_pose, inout vec3 r_pos, inout vec3 r_tangent, inout vec3 r_binormal) {
	float along;
	float u;
	float v;
	spline_deform_decompose_axis(p_pose.meta.x, r_pos, along, u, v);
	float axis_min = p_pose.end_pos_axis_min.w;
	float axis_max = p_pose.end_tangent_axis_max.w;
	float extent = axis_max - axis_min;
	float t = extent > 1e-6 ? clamp((along - axis_min) / extent, 0.0, 1.0) : 0.0;

	vec3 curve_pos;
	vec3 right;
	vec3 up;
	vec3 forward;
	vec2 scale;
	float deriv_len;
	spline_deform_eval_frame(p_pose, t, curve_pos, right, up, forward, scale, deriv_len);
	r_pos = curve_pos + right * (u * scale.x) + up * (v * scale.y);

	float f_scale = max(extent > 1e-6 ? (deriv_len / extent) : 1.0, 1e-5);

	float ta;
	float tu;
	float tv;
	spline_deform_decompose_axis(p_pose.meta.x, r_tangent, ta, tu, tv);
	vec3 dt = right * (tu * scale.x) + up * (tv * scale.y) + forward * (ta * f_scale);
	float t_len_sq = dot(dt, dt);
	r_tangent = t_len_sq > 1e-12 ? (dt * inversesqrt(t_len_sq)) : right;

	float ba;
	float bu;
	float bv;
	spline_deform_decompose_axis(p_pose.meta.x, r_binormal, ba, bu, bv);
	vec3 db = right * (bu * scale.x) + up * (bv * scale.y) + forward * (ba * f_scale);
	float b_len_sq = dot(db, db);
	r_binormal = b_len_sq > 1e-12 ? (db * inversesqrt(b_len_sq)) : up;
}

// Conservative bounding sphere deformation for NextGeo instance/BVH/group/cluster culling.
void spline_deform_bounds_sphere(SplineDeformPose p_pose, inout vec3 r_center, inout float r_radius) {
	float along_c;
	float u_c;
	float v_c;
	spline_deform_decompose_axis(p_pose.meta.x, r_center, along_c, u_c, v_c);
	float axis_min = p_pose.end_pos_axis_min.w;
	float axis_max = p_pose.end_tangent_axis_max.w;
	float extent = max(axis_max - axis_min, 1e-6);
	float t0 = clamp((along_c - r_radius - axis_min) / extent, 0.0, 1.0);
	float t1 = clamp((along_c + r_radius - axis_min) / extent, 0.0, 1.0);
	float tm = 0.5 * (t0 + t1);
	float dt = t1 - t0;

	vec3 c0, r0, u0, f0;
	vec2 s0;
	float d0;
	spline_deform_eval_frame(p_pose, t0, c0, r0, u0, f0, s0, d0);
	vec3 p0 = c0 + r0 * (u_c * s0.x) + u0 * (v_c * s0.y);

	vec3 cm, rm, um, fm;
	vec2 sm;
	float dm;
	spline_deform_eval_frame(p_pose, tm, cm, rm, um, fm, sm, dm);
	vec3 pm = cm + rm * (u_c * sm.x) + um * (v_c * sm.y);

	vec3 c1, r1, u1, f1;
	vec2 s1;
	float d1;
	spline_deform_eval_frame(p_pose, t1, c1, r1, u1, f1, s1, d1);
	vec3 p1 = c1 + r1 * (u_c * s1.x) + u1 * (v_c * s1.y);

	vec3 bmin = min(p0, min(pm, p1));
	vec3 bmax = max(p0, max(pm, p1));
	vec3 new_center = 0.5 * (bmin + bmax);
	float span_r = max(length(p0 - new_center), max(length(pm - new_center), length(p1 - new_center)));

	float max_h2 = spline_deform_max_second_deriv(p_pose);
	float curve_sag = 0.125 * dt * dt * max_h2;
	float r_cross = length(vec2(u_c, v_c));
	float max_s = max(max(abs(p_pose.scale_start_end.x), abs(p_pose.scale_start_end.y)),
			max(abs(p_pose.scale_start_end.z), abs(p_pose.scale_start_end.w)));
	float frame_diff = max(length(r0 - rm) + length(u0 - um), length(r1 - rm) + length(u1 - um));
	float roll_span = abs(p_pose.start_tangent_end_roll.w - p_pose.start_pos_roll.w) * dt;
	float rot_margin = r_cross * max_s * min(2.0, max(frame_diff, 0.5 * roll_span) + 0.5 * dt);

	r_center = new_center;
	r_radius = span_r + r_radius * p_pose.up_max_stretch.w + curve_sag + rot_margin + 1e-4;
}

// Monotonic curvature- and roll-induced chord sag for a DAG node/group of undeformed radius
// p_raw_radius. Guarantees curved/twisted splines refine planar subdivided faces whose undeformed
// simplification error is zero while staying 0 on straight unrolled splines.
float spline_deform_curvature_error(SplineDeformPose p_pose, float p_raw_radius) {
	float extent = max(p_pose.end_tangent_axis_max.w - p_pose.end_pos_axis_min.w, 1e-6);
	float r = max(p_raw_radius, 0.0);
	float dt = clamp((2.0 * r) / extent, 0.0, 1.0);
	float curve_sag = 0.125 * dt * dt * spline_deform_max_second_deriv(p_pose);
	float roll_delta = abs(p_pose.start_tangent_end_roll.w - p_pose.start_pos_roll.w);
	float scale_max = max(
			max(abs(p_pose.scale_start_end.x), abs(p_pose.scale_start_end.y)),
			max(abs(p_pose.scale_start_end.z), abs(p_pose.scale_start_end.w)));
	float roll_sag = 0.125 * dt * dt * roll_delta * roll_delta * r * scale_max;
	return curve_sag + roll_sag;
}

// Conservative local AABB deformation for BVH node culling (frustum, Hi-Z, and VSM page overlap).
void spline_deform_local_aabb(SplineDeformPose p_pose, inout vec3 r_bmin, inout vec3 r_bmax) {
	float a0, u0, v0;
	float a1, u1, v1;
	spline_deform_decompose_axis(p_pose.meta.x, r_bmin, a0, u0, v0);
	spline_deform_decompose_axis(p_pose.meta.x, r_bmax, a1, u1, v1);
	float u_min = min(u0, u1);
	float u_max = max(u0, u1);
	float v_min = min(v0, v1);
	float v_max = max(v0, v1);
	float a_min = min(a0, a1);
	float a_max = max(a0, a1);

	float axis_min = p_pose.end_pos_axis_min.w;
	float axis_max = p_pose.end_tangent_axis_max.w;
	float extent = axis_max - axis_min;
	float t_start = extent > 1e-6 ? clamp((a_min - axis_min) / extent, 0.0, 1.0) : 0.0;
	float t_end = extent > 1e-6 ? clamp((a_max - axis_min) / extent, 0.0, 1.0) : 0.0;

	vec3 out_min = vec3(3.4e38);
	vec3 out_max = vec3(-3.4e38);
	float max_scale_abs = 0.0;
	for (int i = 0; i < 4; i++) {
		float t = mix(t_start, t_end, float(i) / 3.0);
		vec3 curve_pos;
		vec3 right;
		vec3 up;
		vec3 forward;
		vec2 scale;
		float deriv_len;
		spline_deform_eval_frame(p_pose, t, curve_pos, right, up, forward, scale, deriv_len);
		max_scale_abs = max(max_scale_abs, max(abs(scale.x), abs(scale.y)));
		vec3 c00 = curve_pos + right * (u_min * scale.x) + up * (v_min * scale.y);
		vec3 c10 = curve_pos + right * (u_max * scale.x) + up * (v_min * scale.y);
		vec3 c01 = curve_pos + right * (u_min * scale.x) + up * (v_max * scale.y);
		vec3 c11 = curve_pos + right * (u_max * scale.x) + up * (v_max * scale.y);
		out_min = min(out_min, min(min(c00, c10), min(c01, c11)));
		out_max = max(out_max, max(max(c00, c10), max(c01, c11)));
	}
	float dt = (t_end - t_start) / 3.0;
	float curve_sag = 0.125 * dt * dt * spline_deform_max_second_deriv(p_pose);
	float cross_radius = length(vec2(max(abs(u_min), abs(u_max)), max(abs(v_min), abs(v_max)))) * max_scale_abs;
	float step_roll = abs(p_pose.start_tangent_end_roll.w - p_pose.start_pos_roll.w) * dt;
	float rot_sag = cross_radius * (step_roll >= 3.14159265 ? 2.0 : (1.0 - cos(0.5 * step_roll))) + cross_radius * 0.5 * dt;
	float margin = curve_sag + rot_sag + 1e-4;
	r_bmin = out_min - vec3(margin);
	r_bmax = out_max + vec3(margin);
}

#endif // SPLINE_DEFORM_INC_GLSL
