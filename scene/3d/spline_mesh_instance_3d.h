/**************************************************************************/
/*  spline_mesh_instance_3d.h                                             */
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

#include "scene/3d/mesh_instance_3d.h"

class Path3D;

// GodotNext: spline_meshes (#161, plan §21.4 contract N12).
class SplineMeshInstance3D : public MeshInstance3D {
	GDCLASS(SplineMeshInstance3D, MeshInstance3D);

public:
	enum Axis {
		AXIS_X = 0,
		AXIS_Y = 1,
		AXIS_Z = 2,
	};

private:
	Vector3 start_position = Vector3(0.0, 0.0, 0.0);
	Vector3 start_tangent = Vector3(0.0, 0.0, 1.0);
	Vector3 end_position = Vector3(0.0, 0.0, 1.0);
	Vector3 end_tangent = Vector3(0.0, 0.0, 1.0);
	float start_roll = 0.0f;
	float end_roll = 0.0f;
	Vector2 start_scale = Vector2(1.0, 1.0);
	Vector2 end_scale = Vector2(1.0, 1.0);
	Axis forward_axis = AXIS_Z;
	Vector3 up_vector = Vector3(0.0, 1.0, 0.0);

	Dictionary _build_spline_params() const;
	void _update_spline_deform();

protected:
	static void _bind_methods();

public:
	void set_start_position(const Vector3 &p_position);
	Vector3 get_start_position() const;

	void set_start_tangent(const Vector3 &p_tangent);
	Vector3 get_start_tangent() const;

	void set_end_position(const Vector3 &p_position);
	Vector3 get_end_position() const;

	void set_end_tangent(const Vector3 &p_tangent);
	Vector3 get_end_tangent() const;

	void set_start_roll(float p_roll);
	float get_start_roll() const;

	void set_end_roll(float p_roll);
	float get_end_roll() const;

	void set_start_scale(const Vector2 &p_scale);
	Vector2 get_start_scale() const;

	void set_end_scale(const Vector2 &p_scale);
	Vector2 get_end_scale() const;

	void set_forward_axis(Axis p_axis);
	Axis get_forward_axis() const;

	void set_up_vector(const Vector3 &p_up_vector);
	Vector3 get_up_vector() const;

	void set_from_path(Path3D *p_path, int p_segment);

	virtual AABB get_aabb() const override;

	SplineMeshInstance3D();
};

VARIANT_ENUM_CAST(SplineMeshInstance3D::Axis);
