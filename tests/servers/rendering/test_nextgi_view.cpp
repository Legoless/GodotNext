/**************************************************************************/
/*  test_nextgi_view.cpp                                                  */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_nextgi_view)

#include "servers/rendering/renderer_rd/environment/next_gi_view.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"

#ifdef RD_ENABLED
#include "servers/rendering/renderer_rd/environment/next_gi_irradiance_field.h"
#endif

namespace {

// RendererSceneCull's TAA jitter: the Halton (2, 3) sequence remapped to
// [-1, 1] and divided by the viewport size, so one phase moves the view by
// at most half a pixel along each axis.
float _halton(int p_index, int p_base) {
	float f = 1.0f;
	float r = 0.0f;
	while (p_index > 0) {
		f /= float(p_base);
		r += f * float(p_index % p_base);
		p_index /= p_base;
	}
	return r * 2.0f - 1.0f;
}

Vector2 _taa_jitter(int p_phase, const Vector2 &p_viewport) {
	return Vector2(_halton(p_phase, 2), _halton(p_phase, 3)) / p_viewport;
}

// What the Forward+ prepass writes for a static world point, built without
// NextGIView: the scene UBO's matrices as RenderSceneDataRD::update_ubo()
// builds them (the current projection with the view's flip_y, the previous
// one with flip_y on) and the MOTION_VECTORS block of
// scene_forward_clustered.glsl (previous minus current clip position, each
// without its jitter, times 0.5).
struct ForwardSample {
	Vector2 uv; // The depth texel the point lands on.
	float depth = 0.0f; // The stored reverse-Z depth.
	Vector2 motion; // The velocity buffer value.
	float prev_w = 0.0f; // Clip w in the previous frame; positive in front of the camera.
};

ForwardSample _forward_sample(const RenderSceneDataRD &p_scene_data, const Vector3 &p_world) {
	Projection correction;
	correction.set_depth_correction(p_scene_data.flip_y);
	correction.add_jitter_offset(p_scene_data.taa_jitter);
	const Projection projection = correction * p_scene_data.cam_projection;
	Projection prev_correction;
	prev_correction.set_depth_correction(true);
	prev_correction.add_jitter_offset(p_scene_data.prev_taa_jitter);
	const Projection prev_projection = prev_correction * p_scene_data.prev_cam_projection;

	const Vector3 view = p_scene_data.cam_transform.affine_inverse().xform(p_world);
	const Vector3 prev_view = p_scene_data.prev_cam_transform.affine_inverse().xform(p_world);
	const Vector4 screen = projection.xform(Vector4(view.x, view.y, view.z, 1.0f));
	const Vector4 prev_screen = prev_projection.xform(Vector4(prev_view.x, prev_view.y, prev_view.z, 1.0f));

	const Vector2 position_clip = Vector2(screen.x, screen.y) / screen.w;
	const Vector2 prev_position_clip = Vector2(prev_screen.x, prev_screen.y) / prev_screen.w;
	ForwardSample sample;
	sample.uv = position_clip * 0.5f + Vector2(0.5f, 0.5f);
	sample.depth = screen.z / screen.w;
	sample.motion = ((prev_position_clip - p_scene_data.prev_taa_jitter) - (position_clip - p_scene_data.taa_jitter)) * 0.5f;
	sample.prev_w = prev_screen.w;
	return sample;
}

// Godot's default camera, moving and turning between two frames.
RenderSceneDataRD *_moving_camera(const Vector2 &p_viewport) {
	RenderSceneDataRD *scene_data = memnew(RenderSceneDataRD);
	scene_data->cam_projection.set_perspective(75.0f, p_viewport.x / p_viewport.y, 0.05f, 4000.0f);
	scene_data->prev_cam_projection = scene_data->cam_projection;
	scene_data->prev_cam_transform = Transform3D(Basis::from_euler(Vector3(-0.27f, 0.66f, 0.02f)), Vector3(1.25f, 2.05f, 7.85f));
	scene_data->cam_transform = Transform3D(Basis::from_euler(Vector3(-0.3f, 0.7f, 0.0f)), Vector3(1.0f, 2.0f, 8.0f));
	// Only the main view writes motion vectors, and it draws with flip_y on.
	scene_data->flip_y = true;
	return scene_data;
}

// World points spread over the current view, from near the camera to far away.
LocalVector<Vector3> _view_points(const RenderSceneDataRD &p_scene_data) {
	const Transform3D &camera = p_scene_data.cam_transform;
	const Vector3 forward = -camera.basis.get_column(2);
	const Vector3 right = camera.basis.get_column(0);
	const Vector3 up = camera.basis.get_column(1);
	LocalVector<Vector3> points;
	const float distances[] = { 0.5f, 2.0f, 8.0f, 30.0f, 200.0f };
	const float offsets[] = { -0.45f, -0.15f, 0.15f, 0.45f };
	for (const float distance : distances) {
		for (const float x : offsets) {
			for (const float y : offsets) {
				points.push_back(camera.origin + forward * distance + right * (x * distance) + up * (0.6f * y * distance));
			}
		}
	}
	return points;
}

// Where the NextGI passes read the history: the stored depth turned back
// into a world position and projected with the previous view.
Vector2 _helper_history_uv(const RenderSceneDataRD &p_scene_data, const ForwardSample &p_sample) {
	const Vector3 world = NextGIViewMath::reconstruct(NextGIView::inv_view_projection(p_scene_data), p_sample.uv, p_sample.depth);
	const Vector3 prev = NextGIViewMath::project(NextGIView::prev_view_projection(p_scene_data), world);
	CHECK(prev.z > 0.0f);
	return Vector2(prev.x, prev.y);
}

} // namespace

TEST_CASE("[NextGI][View] Velocity and the previous view-projection reproject a static scene alike") {
	const Vector2 viewport(1920.0f, 1080.0f);
	RenderSceneDataRD *scene_data = _moving_camera(viewport);
	const LocalVector<Vector3> points = _view_points(*scene_data);
	REQUIRE(points.size() == 80u);

	// The same jitter in both frames (none, then one TAA phase): Godot's TAA
	// resolve and the diffuse denoiser read the history at uv + motion, and the
	// shared view helper must land on the same texel.
	const Vector2 jitters[] = { Vector2(), _taa_jitter(5, viewport) };
	for (const Vector2 &jitter : jitters) {
		scene_data->taa_jitter = jitter;
		scene_data->prev_taa_jitter = jitter;
		float max_error = 0.0f;
		float max_motion = 0.0f;
		for (const Vector3 &point : points) {
			const ForwardSample sample = _forward_sample(*scene_data, point);
			REQUIRE(sample.prev_w > 0.0f);
			const Vector2 velocity_uv = sample.uv + sample.motion;
			const Vector2 helper_uv = _helper_history_uv(*scene_data, sample);
			max_error = MAX(max_error, ((helper_uv - velocity_uv) * viewport).length());
			max_motion = MAX(max_motion, (sample.motion * viewport).length());
		}
		// The camera move is large enough that a helper using the current camera, or
		// mirroring Y, misses by far more than the tolerance.
		CHECK(max_motion > 40.0f);
		CHECK_MESSAGE(max_error < 0.05f, vformat("Velocity and helper history texels differ by up to %f px (jitter %s).", max_error, jitter));
	}

	// Different jitter in the two frames (every consecutive pair of the default
	// 16-phase sequence): the velocity buffer removes both jitters, while the
	// helper projects into the previous frame's jittered raster, where the NextGI
	// history was written. The two differ by exactly half the jitter change.
	const int phases = 16;
	for (int phase = 0; phase < phases; phase++) {
		scene_data->taa_jitter = _taa_jitter(phase, viewport);
		scene_data->prev_taa_jitter = _taa_jitter((phase + phases - 1) % phases, viewport);
		const Vector2 expected = (scene_data->prev_taa_jitter - scene_data->taa_jitter) * 0.5f * viewport;
		CHECK(expected.length() > 0.1f);
		float max_error = 0.0f;
		for (const Vector3 &point : points) {
			const ForwardSample sample = _forward_sample(*scene_data, point);
			const Vector2 residual = (_helper_history_uv(*scene_data, sample) - (sample.uv + sample.motion)) * viewport;
			max_error = MAX(max_error, (residual - expected).length());
		}
		CHECK_MESSAGE(max_error < 0.05f, vformat("Phase %d: the residual differs from the jitter change %s px by up to %f px.", phase, expected, max_error));
	}
	memdelete(scene_data);
}

TEST_CASE("[NextGI][View] With flip_y off the camera's up side is at uv.y = 1") {
	// Reflection probes draw with flip_y off; the main view draws with it on.
	for (int flip = 0; flip < 2; flip++) {
		RenderSceneDataRD *scene_data = memnew(RenderSceneDataRD);
		scene_data->cam_projection.set_perspective(90.0f, 1.0f, 0.05f, 1000.0f);
		scene_data->cam_transform = Transform3D(Basis::from_euler(Vector3(0.2f, -1.1f, 0.3f)), Vector3(-4.0f, 3.0f, 2.0f));
		scene_data->prev_cam_projection = scene_data->cam_projection;
		scene_data->prev_cam_transform = scene_data->cam_transform;
		scene_data->flip_y = flip == 1;

		const Transform3D &camera = scene_data->cam_transform;
		const Vector3 forward = -camera.basis.get_column(2);
		const Vector3 up = camera.basis.get_column(1);
		const Vector3 points[] = { camera.origin + forward * 5.0f + up * 1.5f, camera.origin + forward * 20.0f - up * 4.0f };
		for (int i = 0; i < 2; i++) {
			const Vector3 &point = points[i];
			const bool above = i == 0;
			// Where the prepass draws the point: the engine's corrected projection.
			const Vector3 view = camera.affine_inverse().xform(point);
			const Vector4 drawn = scene_data->get_cam_projection().xform(Vector4(view.x, view.y, view.z, 1.0f));
			const Vector2 drawn_uv = Vector2(drawn.x, drawn.y) / drawn.w * 0.5f + Vector2(0.5f, 0.5f);
			const float drawn_depth = drawn.z / drawn.w;
			const bool up_at_uv_one = flip == 0;
			CHECK((drawn_uv.y > 0.5f) == (above == up_at_uv_one));

			const Vector3 projected = NextGIViewMath::project(NextGIView::view_projection(*scene_data), point);
			CHECK((projected.y > 0.5f) == (above == up_at_uv_one));
			CHECK(Math::is_equal_approx(projected.x, drawn_uv.x));
			CHECK(Math::is_equal_approx(projected.y, drawn_uv.y));
			CHECK(Math::is_equal_approx(projected.z, drawn_depth));
			const Vector3 prev = NextGIViewMath::project(NextGIView::prev_view_projection(*scene_data), point);
			CHECK(Math::is_equal_approx(prev.y, drawn_uv.y));

			const Vector3 world = NextGIViewMath::reconstruct(NextGIView::inv_view_projection(*scene_data), drawn_uv, drawn_depth);
			CHECK_MESSAGE(world.distance_to(point) < 1.0e-3f * point.distance_to(camera.origin), vformat("flip_y %d: %s reconstructs as %s.", flip, point, world));
			const Vector3 view_reconstructed = NextGIViewMath::reconstruct(NextGIView::inv_projection(*scene_data), drawn_uv, drawn_depth);
			CHECK(view_reconstructed.distance_to(view) < 1.0e-3f * view.length());

#ifdef RD_ENABLED
			// The Lumen Lite field takes flip_y from the scene data as an argument.
			const bool wrc_field = true;
			const RendererRD::NextGIIrradianceField::Params params = RendererRD::NextGIIrradianceField::build_params(scene_data->cam_projection, camera, scene_data->flip_y, Vector2(), Size2i(512, 512), Size2i(256, 256), Size2i(512, 512), true, true, wrc_field, camera.origin);
			Projection inverse;
			for (int column = 0; column < 4; column++) {
				for (int row = 0; row < 4; row++) {
					inverse.columns[column][row] = params.inv_view_proj[column * 4 + row];
				}
			}
			const Vector3 lite_world = NextGIViewMath::reconstruct(inverse, drawn_uv, drawn_depth);
			CHECK_MESSAGE(lite_world.distance_to(point) < 1.0e-3f * point.distance_to(camera.origin), vformat("flip_y %d: Lite reconstructs %s as %s.", flip, point, lite_world));
#endif
		}
		memdelete(scene_data);
	}
}
