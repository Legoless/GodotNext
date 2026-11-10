/**************************************************************************/
/*  next_gi_view.h                                                        */
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
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"

// GodotNext: nextgi_view_math
//
// One convention for every NextGI pass that turns a depth-buffer sample into
// a world position, or a world position into a screen UV.
//
// The depth prepass rasterizes with Godot's corrected projection: the camera
// projection (OpenGL-style clip space) times the depth correction (flip_y,
// reverse Z, [0, 1] remap) and the TAA jitter. RenderSceneDataRD builds it in
// get_cam_projection() and in the scene UBO. Its clip space is exactly the
// sampling space of the depth texture: uv * 2 - 1 is the clip xy of the texel
// (row 0 is the top of the view when flip_y is set) and the stored depth is
// the clip z (1 at the near plane, 0 at the far plane or the sky).
//
// So a pass builds view_projection() here, uploads its inverse, and the GLSL
// twin in nextgi_view_inc.glsl reconstructs with vec4(uv * 2 - 1, depth, 1).
// The raw scene_data->cam_projection is OpenGL-style; feeding it depth * 2 - 1
// collapses every surface onto the near plane and mirrors Y.
struct NextGIViewMath {
	// Godot's depth correction plus the jitter, the matrix the prepass uses.
	static Projection depth_projection(const Projection &p_camera_projection, bool p_flip_y, const Vector2 &p_jitter) {
		Projection correction;
		correction.set_depth_correction(p_flip_y);
		correction.add_jitter_offset(p_jitter);
		return correction * p_camera_projection;
	}

	// World to depth-texture clip space.
	static Projection view_projection(const Projection &p_camera_projection, const Transform3D &p_camera_transform, bool p_flip_y, const Vector2 &p_jitter) {
		return depth_projection(p_camera_projection, p_flip_y, p_jitter) * Projection(p_camera_transform.affine_inverse());
	}

	// CPU twin of nextgi_view_reconstruct(): uv in [0, 1] (texel centers at
	// (pixel + 0.5) / size), depth as stored in the depth texture.
	static Vector3 reconstruct(const Projection &p_inv_view_projection, const Vector2 &p_uv, float p_depth) {
		const Vector4 clip(p_uv.x * 2.0f - 1.0f, p_uv.y * 2.0f - 1.0f, p_depth, 1.0f);
		const Vector4 world = p_inv_view_projection.xform(clip);
		if (world.w == 0.0f) {
			return Vector3(world.x, world.y, world.z);
		}
		return Vector3(world.x, world.y, world.z) / world.w;
	}

	// CPU twin of nextgi_view_project(): (uv.x, uv.y, depth). A point behind
	// the camera returns a negative depth.
	static Vector3 project(const Projection &p_view_projection, const Vector3 &p_world) {
		const Vector4 clip = p_view_projection.xform(Vector4(p_world.x, p_world.y, p_world.z, 1.0f));
		if (!(clip.w > 0.0f)) {
			return Vector3(0.0f, 0.0f, -1.0f);
		}
		return Vector3(clip.x / clip.w * 0.5f + 0.5f, clip.y / clip.w * 0.5f + 0.5f, clip.z / clip.w);
	}

	// The z and w rows' view-z and constant terms of a depth projection, as
	// nextgi_view_depth_from_linear() and nextgi_view_linear_depth() take them.
	static Vector4 depth_coefficients(const Projection &p_depth_projection) {
		return Vector4(p_depth_projection.columns[2][2], p_depth_projection.columns[3][2], p_depth_projection.columns[2][3], p_depth_projection.columns[3][3]);
	}

	// CPU twins of the GLSL pair: the depth-texture value of a point
	// p_linear_depth meters in front of the camera (view-space -z), and back.
	static float depth_from_linear(const Vector4 &p_coefficients, float p_linear_depth) {
		const float z = -p_linear_depth;
		return (p_coefficients.x * z + p_coefficients.y) / (p_coefficients.z * z + p_coefficients.w);
	}
	static float linear_depth(const Vector4 &p_coefficients, float p_depth) {
		return -(p_coefficients.y - p_depth * p_coefficients.w) / (p_depth * p_coefficients.z - p_coefficients.x);
	}

	// Reverse Z: the cleared depth (sky, far plane) is 0.
	static bool is_background(float p_depth) {
		return !(p_depth > 0.0f);
	}

	// The camera basis as three vec4 columns, for nextgi_view_world_normal():
	// the prepass normal-roughness buffer holds view-space normals.
	static void store_basis(const Basis &p_basis, float r_columns[12]) {
		for (int column = 0; column < 3; column++) {
			const Vector3 axis = p_basis.get_column(column);
			r_columns[column * 4] = axis.x;
			r_columns[column * 4 + 1] = axis.y;
			r_columns[column * 4 + 2] = axis.z;
			r_columns[column * 4 + 3] = 0.0f;
		}
	}

	// CPU twin of nextgi_view_world_normal().
	static Vector3 world_normal(const Basis &p_view_to_world, const Vector3 &p_encoded) {
		const Vector3 normal = p_view_to_world.xform(p_encoded * 2.0f - Vector3(1.0f, 1.0f, 1.0f));
		return normal.length_squared() > 1.0e-12f ? normal.normalized() : Vector3(0.0f, 1.0f, 0.0f);
	}

	// Column-major floats for an std430 mat4.
	static void store(const Projection &p_projection, float r_matrix[16]) {
		for (int column = 0; column < 4; column++) {
			for (int row = 0; row < 4; row++) {
				r_matrix[column * 4 + row] = p_projection.columns[column][row];
			}
		}
	}
};

class RenderSceneDataRD;

// The same matrices built from the renderer's per-view data. The current view
// uses the jitter it was rasterized with; the previous view uses the previous
// jitter, as the scene UBO's prev_projection_matrix does.
struct NextGIView {
	static Projection depth_projection(const RenderSceneDataRD &p_scene_data);
	static Projection view_projection(const RenderSceneDataRD &p_scene_data);
	static Projection inv_view_projection(const RenderSceneDataRD &p_scene_data);
	static Projection prev_view_projection(const RenderSceneDataRD &p_scene_data);
	// Depth texture to view space (no camera transform).
	static Projection inv_projection(const RenderSceneDataRD &p_scene_data);
};
