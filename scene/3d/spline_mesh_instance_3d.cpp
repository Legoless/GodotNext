/**************************************************************************/
/*  spline_mesh_instance_3d.cpp                                           */
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

#include "spline_mesh_instance_3d.h"

#include "core/object/class_db.h"
#include "scene/3d/path_3d.h"
#include "scene/resources/curve.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/storage/spline_deform.h"

Dictionary SplineMeshInstance3D::_build_spline_params() const {
	Dictionary params;
	params["start_position"] = start_position;
	params["start_tangent"] = start_tangent;
	params["end_position"] = end_position;
	params["end_tangent"] = end_tangent;
	params["start_roll"] = start_roll;
	params["end_roll"] = end_roll;
	params["start_scale"] = start_scale;
	params["end_scale"] = end_scale;
	params["forward_axis"] = int(forward_axis);
	params["up_vector"] = up_vector;
	return params;
}

void SplineMeshInstance3D::_update_spline_deform() {
	if (RenderingServer::get_singleton() != nullptr && get_instance().is_valid()) {
		RenderingServer::get_singleton()->instance_geometry_set_spline_deform(get_instance(), _build_spline_params());
	}
	update_gizmos();
}

void SplineMeshInstance3D::set_start_position(const Vector3 &p_position) {
	if (start_position == p_position) {
		return;
	}
	start_position = p_position;
	_update_spline_deform();
}

Vector3 SplineMeshInstance3D::get_start_position() const {
	return start_position;
}

void SplineMeshInstance3D::set_start_tangent(const Vector3 &p_tangent) {
	if (start_tangent == p_tangent) {
		return;
	}
	start_tangent = p_tangent;
	_update_spline_deform();
}

Vector3 SplineMeshInstance3D::get_start_tangent() const {
	return start_tangent;
}

void SplineMeshInstance3D::set_end_position(const Vector3 &p_position) {
	if (end_position == p_position) {
		return;
	}
	end_position = p_position;
	_update_spline_deform();
}

Vector3 SplineMeshInstance3D::get_end_position() const {
	return end_position;
}

void SplineMeshInstance3D::set_end_tangent(const Vector3 &p_tangent) {
	if (end_tangent == p_tangent) {
		return;
	}
	end_tangent = p_tangent;
	_update_spline_deform();
}

Vector3 SplineMeshInstance3D::get_end_tangent() const {
	return end_tangent;
}

void SplineMeshInstance3D::set_start_roll(float p_roll) {
	if (start_roll == p_roll) {
		return;
	}
	start_roll = p_roll;
	_update_spline_deform();
}

float SplineMeshInstance3D::get_start_roll() const {
	return start_roll;
}

void SplineMeshInstance3D::set_end_roll(float p_roll) {
	if (end_roll == p_roll) {
		return;
	}
	end_roll = p_roll;
	_update_spline_deform();
}

float SplineMeshInstance3D::get_end_roll() const {
	return end_roll;
}

void SplineMeshInstance3D::set_start_scale(const Vector2 &p_scale) {
	if (start_scale == p_scale) {
		return;
	}
	start_scale = p_scale;
	_update_spline_deform();
}

Vector2 SplineMeshInstance3D::get_start_scale() const {
	return start_scale;
}

void SplineMeshInstance3D::set_end_scale(const Vector2 &p_scale) {
	if (end_scale == p_scale) {
		return;
	}
	end_scale = p_scale;
	_update_spline_deform();
}

Vector2 SplineMeshInstance3D::get_end_scale() const {
	return end_scale;
}

void SplineMeshInstance3D::set_forward_axis(Axis p_axis) {
	ERR_FAIL_INDEX(int(p_axis), 3);
	if (forward_axis == p_axis) {
		return;
	}
	forward_axis = p_axis;
	_update_spline_deform();
}

SplineMeshInstance3D::Axis SplineMeshInstance3D::get_forward_axis() const {
	return forward_axis;
}

void SplineMeshInstance3D::set_up_vector(const Vector3 &p_up_vector) {
	if (up_vector == p_up_vector) {
		return;
	}
	up_vector = p_up_vector;
	_update_spline_deform();
}

Vector3 SplineMeshInstance3D::get_up_vector() const {
	return up_vector;
}

void SplineMeshInstance3D::set_from_path(Path3D *p_path, int p_segment) {
	ERR_FAIL_NULL(p_path);
	Ref<Curve3D> curve = p_path->get_curve();
	ERR_FAIL_COND(curve.is_null());
	const int point_count = curve->get_point_count();
	ERR_FAIL_COND(point_count < 2);
	const int segment_count = curve->is_closed() ? point_count : (point_count - 1);
	ERR_FAIL_INDEX(p_segment, segment_count);

	const int i0 = p_segment;
	const int i1 = (p_segment + 1) % point_count;

	Vector3 p0 = curve->get_point_position(i0);
	Vector3 p1 = curve->get_point_position(i1);
	const Vector3 out0 = curve->get_point_out(i0);
	const Vector3 in1 = curve->get_point_in(i1);

	// Convert Curve3D's cubic Bezier segment (P0, P0 + out0, P1 + in1, P1) to cubic Hermite
	// tangents (T0 = 3 * out0, T1 = -3 * in1). For straight segments with zero handles, use
	// the chord so the frame tangent remains well-defined along the whole segment.
	Vector3 t0;
	Vector3 t1;
	if (out0.is_zero_approx() && in1.is_zero_approx()) {
		t0 = p1 - p0;
		t1 = p1 - p0;
	} else {
		t0 = out0 * 3.0;
		t1 = -in1 * 3.0;
	}

	if (is_inside_tree() && p_path->is_inside_tree()) {
		const Transform3D rel = get_global_transform().affine_inverse() * p_path->get_global_transform();
		p0 = rel.xform(p0);
		p1 = rel.xform(p1);
		t0 = rel.basis.xform(t0);
		t1 = rel.basis.xform(t1);
	}

	start_position = p0;
	start_tangent = t0;
	end_position = p1;
	end_tangent = t1;
	start_roll = float(curve->get_point_tilt(i0));
	end_roll = float(curve->get_point_tilt(i1));
	_update_spline_deform();
}

AABB SplineMeshInstance3D::get_aabb() const {
	Ref<Mesh> m = get_mesh();
	if (m.is_null()) {
		return AABB();
	}
	const AABB mesh_aabb = m->get_aabb();
	bool active = false;
	SplineDeformPose pose = spline_deform_pose_from_dictionary(_build_spline_params(), active);
	spline_deform_apply_mesh_aabb(pose, mesh_aabb);
	return spline_deform_compute_swept_aabb(pose, mesh_aabb);
}

void SplineMeshInstance3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_start_position", "position"), &SplineMeshInstance3D::set_start_position);
	ClassDB::bind_method(D_METHOD("get_start_position"), &SplineMeshInstance3D::get_start_position);

	ClassDB::bind_method(D_METHOD("set_start_tangent", "tangent"), &SplineMeshInstance3D::set_start_tangent);
	ClassDB::bind_method(D_METHOD("get_start_tangent"), &SplineMeshInstance3D::get_start_tangent);

	ClassDB::bind_method(D_METHOD("set_end_position", "position"), &SplineMeshInstance3D::set_end_position);
	ClassDB::bind_method(D_METHOD("get_end_position"), &SplineMeshInstance3D::get_end_position);

	ClassDB::bind_method(D_METHOD("set_end_tangent", "tangent"), &SplineMeshInstance3D::set_end_tangent);
	ClassDB::bind_method(D_METHOD("get_end_tangent"), &SplineMeshInstance3D::get_end_tangent);

	ClassDB::bind_method(D_METHOD("set_start_roll", "roll"), &SplineMeshInstance3D::set_start_roll);
	ClassDB::bind_method(D_METHOD("get_start_roll"), &SplineMeshInstance3D::get_start_roll);

	ClassDB::bind_method(D_METHOD("set_end_roll", "roll"), &SplineMeshInstance3D::set_end_roll);
	ClassDB::bind_method(D_METHOD("get_end_roll"), &SplineMeshInstance3D::get_end_roll);

	ClassDB::bind_method(D_METHOD("set_start_scale", "scale"), &SplineMeshInstance3D::set_start_scale);
	ClassDB::bind_method(D_METHOD("get_start_scale"), &SplineMeshInstance3D::get_start_scale);

	ClassDB::bind_method(D_METHOD("set_end_scale", "scale"), &SplineMeshInstance3D::set_end_scale);
	ClassDB::bind_method(D_METHOD("get_end_scale"), &SplineMeshInstance3D::get_end_scale);

	ClassDB::bind_method(D_METHOD("set_forward_axis", "axis"), &SplineMeshInstance3D::set_forward_axis);
	ClassDB::bind_method(D_METHOD("get_forward_axis"), &SplineMeshInstance3D::get_forward_axis);

	ClassDB::bind_method(D_METHOD("set_up_vector", "up_vector"), &SplineMeshInstance3D::set_up_vector);
	ClassDB::bind_method(D_METHOD("get_up_vector"), &SplineMeshInstance3D::get_up_vector);

	ClassDB::bind_method(D_METHOD("set_from_path", "path", "segment"), &SplineMeshInstance3D::set_from_path);

	ADD_GROUP("Spline", "");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "start_position", PROPERTY_HINT_NONE, "suffix:m"), "set_start_position", "get_start_position");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "start_tangent"), "set_start_tangent", "get_start_tangent");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "end_position", PROPERTY_HINT_NONE, "suffix:m"), "set_end_position", "get_end_position");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "end_tangent"), "set_end_tangent", "get_end_tangent");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "start_roll", PROPERTY_HINT_RANGE, "-360,360,0.1,or_less,or_greater,radians_as_degrees"), "set_start_roll", "get_start_roll");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "end_roll", PROPERTY_HINT_RANGE, "-360,360,0.1,or_less,or_greater,radians_as_degrees"), "set_end_roll", "get_end_roll");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "start_scale"), "set_start_scale", "get_start_scale");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "end_scale"), "set_end_scale", "get_end_scale");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "forward_axis", PROPERTY_HINT_ENUM, "X,Y,Z"), "set_forward_axis", "get_forward_axis");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "up_vector"), "set_up_vector", "get_up_vector");

	BIND_ENUM_CONSTANT(AXIS_X);
	BIND_ENUM_CONSTANT(AXIS_Y);
	BIND_ENUM_CONSTANT(AXIS_Z);
}

SplineMeshInstance3D::SplineMeshInstance3D() {
	_update_spline_deform();
}
