/**************************************************************************/
/*  test_path_tracer.cpp                                                  */
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

// GodotNext: path_tracer (#167). The device-free parts of the product path tracer: setting validation, the
// accumulation reset rule, the cull-mode rule, the backends and the platforms (Apple8 and Apple7 mocked).

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_path_tracer)

#include "core/math/basis.h"
#include "core/math/math_funcs.h"
#include "core/math/projection.h"
#include "scene/main/viewport.h"
#include "servers/rendering/renderer_rd/effects/path_tracer.h"
#include "servers/rendering/renderer_viewport.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_types.h"

#include <cmath>
#include <limits>

using namespace RendererRD;

namespace TestPathTracer {

TEST_CASE("[PathTracer] Settings are validated: samples, bounces and the firefly clamp") {
	// The issue's defaults: 2048 samples, 32 bounces; the clamp is off.
	CHECK_EQ(RenderingServerTypes::PATH_TRACING_DEFAULT_SAMPLES, 2048);
	CHECK_EQ(RenderingServerTypes::PATH_TRACING_DEFAULT_MAX_BOUNCES, 32);
	const PathTracerSettings defaults;
	CHECK_FALSE(defaults.enabled);
	CHECK_EQ(defaults.samples, 2048);
	CHECK_EQ(defaults.max_bounces, 32);
	CHECK_EQ(defaults.firefly_clamp, 0.0f);

	// At least one sample, at most 2^20.
	CHECK_EQ(RenderingServerTypes::path_tracing_validate_samples(0), 1);
	CHECK_EQ(RenderingServerTypes::path_tracing_validate_samples(-7), 1);
	CHECK_EQ(RenderingServerTypes::path_tracing_validate_samples(1), 1);
	CHECK_EQ(RenderingServerTypes::path_tracing_validate_samples(4096), 4096);
	CHECK_EQ(RenderingServerTypes::path_tracing_validate_samples(int64_t(1) << 40), RenderingServerTypes::PATH_TRACING_MAX_SAMPLES);

	// 0 bounces is direct lighting only; 63 is the D16 reference's hard stop of 64 path vertices.
	CHECK_EQ(RenderingServerTypes::PATH_TRACING_MAX_BOUNCES, 63);
	CHECK_EQ(RenderingServerTypes::path_tracing_validate_max_bounces(-1), 0);
	CHECK_EQ(RenderingServerTypes::path_tracing_validate_max_bounces(0), 0);
	CHECK_EQ(RenderingServerTypes::path_tracing_validate_max_bounces(32), 32);
	CHECK_EQ(RenderingServerTypes::path_tracing_validate_max_bounces(63), 63);
	CHECK_EQ(RenderingServerTypes::path_tracing_validate_max_bounces(64), 63);

	// 0 is off; negative, NaN and infinite values are off too; the largest clamp is the half-float maximum.
	CHECK_EQ(RenderingServerTypes::path_tracing_validate_firefly_clamp(0.0), 0.0f);
	CHECK_EQ(RenderingServerTypes::path_tracing_validate_firefly_clamp(-3.0), 0.0f);
	CHECK_EQ(RenderingServerTypes::path_tracing_validate_firefly_clamp(std::numeric_limits<double>::quiet_NaN()), 0.0f);
	CHECK_EQ(RenderingServerTypes::path_tracing_validate_firefly_clamp(std::numeric_limits<double>::infinity()), 0.0f);
	CHECK_EQ(RenderingServerTypes::path_tracing_validate_firefly_clamp(2.5), 2.5f);
	CHECK_EQ(RenderingServerTypes::path_tracing_validate_firefly_clamp(1.0e9), RenderingServerTypes::PATH_TRACING_MAX_FIREFLY_CLAMP);
}

TEST_CASE("[PathTracer] The path tracer debug draw turns path tracing on with the viewport's settings") {
	PathTracerSettings settings;
	settings.samples = 0;
	settings.max_bounces = 99;
	settings.firefly_clamp = -1.0f;
	const PathTracerSettings off = path_tracer_effective_settings(settings, RSE::VIEWPORT_DEBUG_DRAW_DISABLED);
	CHECK_FALSE(off.enabled);
	// The effective settings are validated too.
	CHECK_EQ(off.samples, 1);
	CHECK_EQ(off.max_bounces, 63);
	CHECK_EQ(off.firefly_clamp, 0.0f);
	CHECK(path_tracer_effective_settings(settings, RSE::VIEWPORT_DEBUG_DRAW_PATH_TRACER).enabled);
	CHECK_FALSE(path_tracer_effective_settings(settings, RSE::VIEWPORT_DEBUG_DRAW_TSR).enabled);
	settings.enabled = true;
	CHECK(path_tracer_effective_settings(settings, RSE::VIEWPORT_DEBUG_DRAW_UNSHADED).enabled);

	// RendererViewport applies the same rule when it hands the settings to the render buffers.
	CHECK_FALSE(RendererViewport::viewport_path_tracing_active(false, RSE::VIEWPORT_DEBUG_DRAW_DISABLED));
	CHECK(RendererViewport::viewport_path_tracing_active(true, RSE::VIEWPORT_DEBUG_DRAW_DISABLED));
	CHECK(RendererViewport::viewport_path_tracing_active(false, RSE::VIEWPORT_DEBUG_DRAW_PATH_TRACER));
	CHECK_FALSE(RendererViewport::viewport_path_tracing_active(false, RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_TIER));
}

TEST_CASE("[SceneTree][PathTracer] Viewport.path_tracing_* reach the RenderingServer validated") {
	SubViewport *vp = memnew(SubViewport);
	CHECK_FALSE(vp->is_path_tracing_enabled());
	CHECK_EQ(vp->get_path_tracing_samples(), RenderingServerTypes::PATH_TRACING_DEFAULT_SAMPLES);
	CHECK_EQ(vp->get_path_tracing_max_bounces(), RenderingServerTypes::PATH_TRACING_DEFAULT_MAX_BOUNCES);
	CHECK_EQ(vp->get_path_tracing_firefly_clamp(), 0.0f);
	CHECK_EQ(int(Viewport::DEBUG_DRAW_PATH_TRACER), int(RSE::VIEWPORT_DEBUG_DRAW_PATH_TRACER));

	RenderingServer *rs = RenderingServer::get_singleton();
	REQUIRE(rs != nullptr);
	Dictionary info = rs->viewport_get_path_tracing_info(vp->get_viewport_rid());
	CHECK_FALSE(bool(info["enabled"]));
	CHECK_EQ(int(info["samples_setting"]), 2048);
	CHECK_EQ(int(info["max_bounces_setting"]), 32);

	vp->set_path_tracing_enabled(true);
	vp->set_path_tracing_samples(0);
	vp->set_path_tracing_max_bounces(1000);
	vp->set_path_tracing_firefly_clamp(-2.0f);
	CHECK(vp->is_path_tracing_enabled());
	CHECK_EQ(vp->get_path_tracing_samples(), 1);
	CHECK_EQ(vp->get_path_tracing_max_bounces(), 63);
	CHECK_EQ(vp->get_path_tracing_firefly_clamp(), 0.0f);
	info = vp->get_path_tracing_info();
	CHECK(bool(info["enabled"]));
	CHECK_EQ(int(info["samples_setting"]), 1);
	CHECK_EQ(int(info["max_bounces_setting"]), 63);
	CHECK_EQ(float(info["firefly_clamp_setting"]), 0.0f);

	// The RenderingServer validates on its own as well.
	rs->viewport_set_path_tracing_samples(vp->get_viewport_rid(), -4);
	rs->viewport_set_path_tracing_max_bounces(vp->get_viewport_rid(), 7);
	rs->viewport_set_path_tracing_firefly_clamp(vp->get_viewport_rid(), 12.5f);
	info = rs->viewport_get_path_tracing_info(vp->get_viewport_rid());
	CHECK_EQ(int(info["samples_setting"]), 1);
	CHECK_EQ(int(info["max_bounces_setting"]), 7);
	CHECK_EQ(float(info["firefly_clamp_setting"]), 12.5f);

	// Off again, the debug draw alone turns it on.
	vp->set_path_tracing_enabled(false);
	CHECK_FALSE(bool(vp->get_path_tracing_info()["enabled"]));
	vp->set_debug_draw(Viewport::DEBUG_DRAW_PATH_TRACER);
	CHECK(bool(vp->get_path_tracing_info()["enabled"]));
	vp->set_debug_draw(Viewport::DEBUG_DRAW_DISABLED);
	CHECK_FALSE(bool(vp->get_path_tracing_info()["enabled"]));

	memdelete(vp);
}

static PathTracerFrameState _frame() {
	PathTracerFrameState state;
	state.size = Size2i(1920, 1080);
	state.settings = path_tracer_hash_settings(PathTracerSettings(), PATH_TRACER_BACKEND_HIT_GROUPS);
	state.camera = path_tracer_hash_camera(Transform3D(), Projection::create_perspective(70.0, 16.0 / 9.0, 0.05, 4000.0));
	state.scene = 0x1234;
	state.material_serial = 10;
	state.hit_groups_serial = 3;
	return state;
}

TEST_CASE("[PathTracer] The accumulation resets on the frame of every change event, and only then") {
	const PathTracerFrameState previous = _frame();

	// The first traced frame (or a history the render buffers dropped) starts a history.
	CHECK_EQ(path_tracer_reset_reasons(false, previous, previous), uint32_t(PATH_TRACER_RESET_HISTORY));
	// Nothing changed: the history keeps accumulating.
	CHECK_EQ(path_tracer_reset_reasons(true, previous, previous), 0u);

	PathTracerFrameState current = previous;
	current.size = Size2i(1280, 720);
	CHECK_EQ(path_tracer_reset_reasons(true, previous, current), uint32_t(PATH_TRACER_RESET_HISTORY));

	// Camera: a moved camera, and a new projection (field of view).
	current = previous;
	Transform3D moved;
	moved.origin = Vector3(0.0, 0.0, 0.001);
	current.camera = path_tracer_hash_camera(moved, Projection::create_perspective(70.0, 16.0 / 9.0, 0.05, 4000.0));
	CHECK_EQ(path_tracer_reset_reasons(true, previous, current), uint32_t(PATH_TRACER_RESET_CAMERA));
	current.camera = path_tracer_hash_camera(Transform3D(), Projection::create_perspective(71.0, 16.0 / 9.0, 0.05, 4000.0));
	CHECK_EQ(path_tracer_reset_reasons(true, previous, current), uint32_t(PATH_TRACER_RESET_CAMERA));

	// The view's lights, of any type, changed (NextGI raises no event for some changes, such as a light's energy).
	current = previous;
	const float params[2] = { 1.0f, 5.0f };
	current.lights = path_tracer_hash_light(1, uint32_t(RSE::LIGHT_OMNI), Transform3D(), Color(1, 1, 1), params, 2);
	PathTracerFrameState lit = current;
	CHECK_EQ(path_tracer_reset_reasons(true, previous, current), uint32_t(PATH_TRACER_RESET_LIGHTS));
	CHECK_EQ(path_tracer_reset_reasons(true, lit, current), 0u);
	const float brighter[2] = { 1.5f, 5.0f };
	current.lights = path_tracer_hash_light(1, uint32_t(RSE::LIGHT_OMNI), Transform3D(), Color(1, 1, 1), brighter, 2);
	CHECK_EQ(path_tracer_reset_reasons(true, lit, current), uint32_t(PATH_TRACER_RESET_LIGHTS));
	Transform3D moved_light;
	moved_light.origin.y = 0.1;
	current.lights = path_tracer_hash_light(1, uint32_t(RSE::LIGHT_OMNI), moved_light, Color(1, 1, 1), params, 2);
	CHECK_EQ(path_tracer_reset_reasons(true, lit, current), uint32_t(PATH_TRACER_RESET_LIGHTS));
	current.lights = path_tracer_hash_light(1, uint32_t(RSE::LIGHT_DIRECTIONAL), Transform3D(), Color(1, 1, 1), params, 2);
	CHECK_EQ(path_tracer_reset_reasons(true, lit, current), uint32_t(PATH_TRACER_RESET_LIGHTS));
	current.lights = path_tracer_hash_light(1, uint32_t(RSE::LIGHT_OMNI), Transform3D(), Color(1, 0.5, 1), params, 2);
	CHECK_EQ(path_tracer_reset_reasons(true, lit, current), uint32_t(PATH_TRACER_RESET_LIGHTS));

	// The NextGI dirty events of the frame: instances moved, added, freed or hidden; lights.
	current = previous;
	current.instance_events = 1;
	CHECK_EQ(path_tracer_reset_reasons(true, previous, current), uint32_t(PATH_TRACER_RESET_INSTANCES));
	current = previous;
	current.light_events = 2;
	CHECK_EQ(path_tracer_reset_reasons(true, previous, current), uint32_t(PATH_TRACER_RESET_LIGHTS));
	current = previous;
	current.directional_light_event = true;
	CHECK_EQ(path_tracer_reset_reasons(true, previous, current), uint32_t(PATH_TRACER_RESET_LIGHTS));
	// Events are per frame: last frame's do not reset this one.
	PathTracerFrameState after_event = previous;
	PathTracerFrameState event_frame = previous;
	event_frame.instance_events = 3;
	CHECK_EQ(path_tracer_reset_reasons(true, event_frame, after_event), 0u);

	// A material change, the traced scene (TLAS records, D16 tables, sky) and a relinked hit pipeline.
	current = previous;
	current.material_serial++;
	CHECK_EQ(path_tracer_reset_reasons(true, previous, current), uint32_t(PATH_TRACER_RESET_MATERIALS));
	current = previous;
	current.scene ^= 1;
	CHECK_EQ(path_tracer_reset_reasons(true, previous, current), uint32_t(PATH_TRACER_RESET_SCENE));
	current = previous;
	current.hit_groups_serial++;
	CHECK_EQ(path_tracer_reset_reasons(true, previous, current), uint32_t(PATH_TRACER_RESET_HIT_GROUPS));

	// Settings: bounces, the clamp and the backend restart the history; the sample target does not.
	PathTracerSettings settings;
	current = previous;
	settings.max_bounces = 8;
	current.settings = path_tracer_hash_settings(settings, PATH_TRACER_BACKEND_HIT_GROUPS);
	CHECK_EQ(path_tracer_reset_reasons(true, previous, current), uint32_t(PATH_TRACER_RESET_SETTINGS));
	settings = PathTracerSettings();
	settings.firefly_clamp = 10.0f;
	current.settings = path_tracer_hash_settings(settings, PATH_TRACER_BACKEND_HIT_GROUPS);
	CHECK_EQ(path_tracer_reset_reasons(true, previous, current), uint32_t(PATH_TRACER_RESET_SETTINGS));
	current.settings = path_tracer_hash_settings(PathTracerSettings(), PATH_TRACER_BACKEND_INLINE);
	CHECK_EQ(path_tracer_reset_reasons(true, previous, current), uint32_t(PATH_TRACER_RESET_SETTINGS));
	settings = PathTracerSettings();
	settings.reference_match = true;
	current.settings = path_tracer_hash_settings(settings, PATH_TRACER_BACKEND_INLINE);
	CHECK_EQ(path_tracer_reset_reasons(true, previous, current), uint32_t(PATH_TRACER_RESET_SETTINGS));
	settings = PathTracerSettings();
	settings.samples = 4096;
	settings.enabled = true;
	CHECK_EQ(path_tracer_hash_settings(settings, PATH_TRACER_BACKEND_HIT_GROUPS), previous.settings);
	// The clamp limits the exposed radiance, so with the clamp on an exposure change restarts the history; with it
	// off the exposure does not enter the samples (the composite applies it).
	CHECK_EQ(path_tracer_hash_settings(PathTracerSettings(), PATH_TRACER_BACKEND_HIT_GROUPS, 0.25f), previous.settings);
	settings = PathTracerSettings();
	settings.firefly_clamp = 10.0f;
	const uint64_t clamped = path_tracer_hash_settings(settings, PATH_TRACER_BACKEND_HIT_GROUPS, 1.0f);
	CHECK_EQ(path_tracer_hash_settings(settings, PATH_TRACER_BACKEND_HIT_GROUPS, 1.0f), clamped);
	CHECK_NE(path_tracer_hash_settings(settings, PATH_TRACER_BACKEND_HIT_GROUPS, 0.25f), clamped);

	// Several changes in one frame report every reason.
	current = previous;
	current.instance_events = 1;
	current.scene ^= 2;
	current.material_serial++;
	CHECK_EQ(path_tracer_reset_reasons(true, previous, current), uint32_t(PATH_TRACER_RESET_INSTANCES | PATH_TRACER_RESET_SCENE | PATH_TRACER_RESET_MATERIALS));

	// Every reason has its own name.
	for (uint32_t i = 0; i < PATH_TRACER_RESET_REASON_COUNT; i++) {
		CHECK_NE(String(path_tracer_reset_reason_name(i)), String("unknown"));
		for (uint32_t j = 0; j < i; j++) {
			CHECK_NE(String(path_tracer_reset_reason_name(i)), String(path_tracer_reset_reason_name(j)));
		}
	}
	CHECK_EQ(String(path_tracer_reset_reason_name(PATH_TRACER_RESET_REASON_COUNT)), String("unknown"));
}

TEST_CASE("[PathTracer] One sample per frame until the target, then the history is kept") {
	CHECK_EQ(path_tracer_samples_this_frame(0, 2048), 1u);
	CHECK_EQ(path_tracer_samples_this_frame(2047, 2048), 1u);
	CHECK_EQ(path_tracer_samples_this_frame(2048, 2048), 0u);
	// A lowered target stops at once; a raised one continues the same history.
	CHECK_EQ(path_tracer_samples_this_frame(3000, 2048), 0u);
	CHECK_EQ(path_tracer_samples_this_frame(2048, 4096), 1u);
}

TEST_CASE("[PathTracer] The cull mode rule: one-sided materials cull back faces, cull_disabled culls none") {
	// cull_back (BaseMaterial3D's default) is hit only from the front; cull_front only from the back.
	CHECK_FALSE(path_tracer_cull_skips_hit(RSE::CULL_MODE_BACK, true));
	CHECK(path_tracer_cull_skips_hit(RSE::CULL_MODE_BACK, false));
	CHECK(path_tracer_cull_skips_hit(RSE::CULL_MODE_FRONT, true));
	CHECK_FALSE(path_tracer_cull_skips_hit(RSE::CULL_MODE_FRONT, false));
	CHECK_FALSE(path_tracer_cull_skips_hit(RSE::CULL_MODE_DISABLED, true));
	CHECK_FALSE(path_tracer_cull_skips_hit(RSE::CULL_MODE_DISABLED, false));
	// The D16 table entry carries the cull mode; a material without one is one-sided.
	CHECK_EQ(PathTracerGPUMaterial().specular_mode[2], float(RSE::CULL_MODE_BACK));

	// A triangle that winds clockwise as seen from +Z, Godot's front face from there.
	const Vector3 p0(0.0, 0.0, 0.0);
	const Vector3 p1(0.0, 1.0, 0.0);
	const Vector3 p2(1.0, 0.0, 0.0);
	const Vector3 toward_minus_z(0.0, 0.0, -1.0);
	const Vector3 toward_plus_z(0.0, 0.0, 1.0);
	CHECK(path_tracer_hit_is_front(p0, p1, p2, Basis(), toward_minus_z));
	CHECK_FALSE(path_tracer_hit_is_front(p0, p1, p2, Basis(), toward_plus_z));
	// Seen from behind, a one-sided wall lets the ray through; a double-sided one stops it.
	CHECK(path_tracer_cull_skips_hit(RSE::CULL_MODE_BACK, path_tracer_hit_is_front(p0, p1, p2, Basis(), toward_plus_z)));
	CHECK_FALSE(path_tracer_cull_skips_hit(RSE::CULL_MODE_DISABLED, path_tracer_hit_is_front(p0, p1, p2, Basis(), toward_plus_z)));

	// A rotated instance turns its front with it: rotated 180 degrees about Y, the front faces -Z.
	const Basis turned(Vector3(0.0, 1.0, 0.0), Math::PI);
	CHECK_FALSE(path_tracer_hit_is_front(p0, p1, p2, turned, toward_minus_z));
	CHECK(path_tracer_hit_is_front(p0, p1, p2, turned, toward_plus_z));

	// A mirrored instance keeps its sides, as the rasterizer reverses the cull mode of mirrored instances. A mirror
	// across X keeps the +Z side the front; a mirror across Z moves the front side to -Z.
	const Basis mirror_x = Basis::from_scale(Vector3(-1.0, 1.0, 1.0));
	CHECK(path_tracer_hit_is_front(p0, p1, p2, mirror_x, toward_minus_z));
	CHECK_FALSE(path_tracer_hit_is_front(p0, p1, p2, mirror_x, toward_plus_z));
	const Basis mirror_z = Basis::from_scale(Vector3(1.0, 1.0, -1.0));
	CHECK_FALSE(path_tracer_hit_is_front(p0, p1, p2, mirror_z, toward_minus_z));
	CHECK(path_tracer_hit_is_front(p0, p1, p2, mirror_z, toward_plus_z));
	// The corners' own winding flips under a mirror; the rule does not follow it.
	const Vector3 world_cross = (mirror_x.xform(p2) - mirror_x.xform(p0)).cross(mirror_x.xform(p1) - mirror_x.xform(p0));
	CHECK(world_cross.dot(toward_minus_z) > 0.0);
}

TEST_CASE("[PathTracer] Backends and platforms, with Apple8 and Apple7 mocked") {
	// The hit groups shade where they run and their pipeline has the path tracer's stage; inline elsewhere.
	CHECK_EQ(path_tracer_backend(true, true), PATH_TRACER_BACKEND_HIT_GROUPS);
	CHECK_EQ(path_tracer_backend(true, false), PATH_TRACER_BACKEND_INLINE);
	CHECK_EQ(path_tracer_backend(false, true), PATH_TRACER_BACKEND_INLINE);
	CHECK_EQ(path_tracer_backend(false, false), PATH_TRACER_BACKEND_INLINE);

	// Vulkan and D3D12 with ray query.
	CHECK_EQ(path_tracer_device_support(false, 0, true), PATH_TRACER_DEVICE_SUPPORTED);
	CHECK_EQ(path_tracer_device_support(false, 0, false), PATH_TRACER_DEVICE_NO_RAY_QUERY);
	// Metal: Apple8 and later, Experimental (inline ray query and the D16 table materials, §21.0.6).
	CHECK_EQ(path_tracer_device_support(true, 8, true), PATH_TRACER_DEVICE_EXPERIMENTAL);
	CHECK_EQ(path_tracer_device_support(true, 9, true), PATH_TRACER_DEVICE_EXPERIMENTAL);
	CHECK_EQ(path_tracer_device_support(true, 7, true), PATH_TRACER_DEVICE_APPLE_FAMILY);
	CHECK_EQ(path_tracer_device_support(true, 7, false), PATH_TRACER_DEVICE_APPLE_FAMILY);
	CHECK_EQ(path_tracer_device_support(true, 8, false), PATH_TRACER_DEVICE_NO_RAY_QUERY);
	// An unknown family is decided by ray query, which #171 offers on Apple8 and later only.
	CHECK_EQ(path_tracer_device_support(true, 0, true), PATH_TRACER_DEVICE_EXPERIMENTAL);
	CHECK_EQ(path_tracer_device_support(true, 0, false), PATH_TRACER_DEVICE_NO_RAY_QUERY);
	// It runs where the support is SUPPORTED or EXPERIMENTAL; elsewhere the view reports UNSUPPORTED_DEVICE.
	CHECK(path_tracer_device_runs(path_tracer_device_support(false, 0, true)));
	CHECK(path_tracer_device_runs(path_tracer_device_support(true, 8, true)));
	CHECK_FALSE(path_tracer_device_runs(path_tracer_device_support(true, 7, true)));
	CHECK_FALSE(path_tracer_device_runs(path_tracer_device_support(false, 0, false)));
	CHECK_EQ(String(path_tracer_device_support_name(PATH_TRACER_DEVICE_SUPPORTED)), String("SUPPORTED"));
	CHECK_EQ(String(path_tracer_device_support_name(PATH_TRACER_DEVICE_EXPERIMENTAL)), String("EXPERIMENTAL"));
	CHECK_EQ(String(path_tracer_device_support_name(PATH_TRACER_DEVICE_NO_RAY_QUERY)), String("NO_RAY_QUERY"));
	CHECK_EQ(String(path_tracer_device_support_name(PATH_TRACER_DEVICE_APPLE_FAMILY)), String("APPLE_FAMILY"));
	CHECK_EQ(String(path_tracer_status_name(PATH_TRACER_STATUS_UNSUPPORTED_DEVICE)), String("UNSUPPORTED_DEVICE"));

	// A view reports its device's support: an Apple8 Mac shows EXPERIMENTAL, an Apple7 one why it cannot run.
	Ref<PathTracerView> view;
	view.instantiate();
	view->device_support = path_tracer_device_support(true, 8, true);
	Dictionary info;
	view->fill_info(info);
	CHECK_EQ(String(info["device_support"]), String("EXPERIMENTAL"));
	view->device_support = path_tracer_device_support(true, 7, true);
	view->status = PATH_TRACER_STATUS_UNSUPPORTED_DEVICE;
	view->fill_info(info);
	CHECK_EQ(String(info["device_support"]), String("APPLE_FAMILY"));
	CHECK_EQ(String(info["status"]), String("UNSUPPORTED_DEVICE"));

	for (int i = 0; i < PATH_TRACER_STATUS_MAX; i++) {
		CHECK_NE(String(path_tracer_status_name(PathTracerStatus(i))), String("UNKNOWN"));
	}
	for (int i = 0; i < PATH_TRACER_BACKEND_MAX; i++) {
		CHECK_NE(String(path_tracer_backend_name(PathTracerBackend(i))), String("UNKNOWN"));
	}
}

TEST_CASE("[PathTracer] The camera block is the D16 reference's") {
	const Projection projection = Projection::create_perspective(60.0, 2.0, 0.1, 100.0);
	Transform3D camera;
	camera.basis = Basis(Vector3(0.0, 1.0, 0.0), Math::PI * 0.5);
	camera.origin = Vector3(1.0, 2.0, 3.0);
	const PathTracerGPUCamera block = path_tracer_camera(camera, projection, Size2i(200, 100));
	CHECK_EQ(block.cam_origin_fov[0], 1.0f);
	CHECK_EQ(block.cam_origin_fov[1], 2.0f);
	CHECK_EQ(block.cam_origin_fov[2], 3.0f);
	// The vertical field of view from the projection, in radians.
	CHECK(Math::is_equal_approx(block.cam_origin_fov[3], float(Math::deg_to_rad(60.0)), 1e-5f));
	CHECK_EQ(block.cam_forward_aspect[3], 2.0f);
	// Forward is -Z of the basis: turned 90 degrees about Y, the camera looks along -X.
	CHECK(Math::is_equal_approx(block.cam_forward_aspect[0], -1.0f, 1e-6f));
	CHECK(Math::is_zero_approx(block.cam_forward_aspect[2]));
	CHECK(Math::is_equal_approx(block.cam_up[1], 1.0f, 1e-6f));
	CHECK(Math::is_equal_approx(block.cam_right[2], -1.0f, 1e-6f));
}

static Vector3 _ray_vec(const float *p_v) {
	return Vector3(p_v[0], p_v[1], p_v[2]);
}

TEST_CASE("[PathTracer] Camera rays follow the projection: perspective, orthogonal and off-axis frustum") {
	Transform3D camera;
	camera.basis = Basis(Vector3(0.0, 1.0, 0.0), Math::PI * 0.5); // Looks along -X; right is -Z, up is +Y.
	camera.origin = Vector3(1.0, 2.0, 3.0);
	const Vector3 forward(-1.0, 0.0, 0.0);
	const Vector3 right(0.0, 0.0, -1.0);
	const Vector3 up(0.0, 1.0, 0.0);
	const Size2i size(200, 100);

	// Perspective: the near-plane point at the view's center is near along the view direction, and the edges are
	// the field of view's: the same rays as the D16 camera block.
	const Projection perspective = Projection::create_perspective(60.0, 2.0, 0.1, 100.0);
	PathTracerGPURays rays = path_tracer_rays(camera, perspective, false, size);
	CHECK_EQ(rays.center[3], 0.0f);
	CHECK(_ray_vec(rays.center).is_equal_approx(camera.origin + forward * 0.1));
	const float tan_half = Math::tan(float(Math::deg_to_rad(30.0)));
	CHECK(_ray_vec(rays.dy).is_equal_approx(up * (0.1f * tan_half)));
	CHECK(_ray_vec(rays.dx).is_equal_approx(right * (0.1f * tan_half * 2.0f)));
	// The ray cone: none at the camera, one pixel's angle per unit of length.
	CHECK_EQ(rays.dx[3], 0.0f);
	CHECK(Math::is_equal_approx(rays.dy[3], 2.0f * tan_half / 100.0f, 1e-6f));

	// Orthogonal: rays start on the near plane, spread over the size (the height, 4 m) and all go forward.
	const Projection orthogonal = Projection::create_orthogonal_aspect(4.0, 2.0, 0.05, 100.0, false);
	rays = path_tracer_rays(camera, orthogonal, true, size);
	CHECK_EQ(rays.center[3], 1.0f);
	CHECK(_ray_vec(rays.center).is_equal_approx(camera.origin + forward * 0.05));
	CHECK(_ray_vec(rays.dy).is_equal_approx(up * 2.0));
	CHECK(_ray_vec(rays.dx).is_equal_approx(right * 4.0));
	// One pixel wide (4 m over 100 pixels) at every distance.
	CHECK(Math::is_equal_approx(rays.dx[3], 0.04f, 1e-6f));
	CHECK_EQ(rays.dy[3], 0.0f);

	// An off-axis frustum: the view's center moves with the offset, which the D16 camera block cannot hold.
	const Projection frustum = Projection::create_frustum_aspect(0.2, 2.0, Vector2(0.05, 0.02), 0.1, 100.0, false);
	rays = path_tracer_rays(camera, frustum, false, size);
	CHECK_EQ(rays.center[3], 0.0f);
	CHECK(_ray_vec(rays.center).is_equal_approx(camera.origin + forward * 0.1 + right * 0.05 + up * 0.02));
	CHECK(_ray_vec(rays.dy).is_equal_approx(up * 0.1));
	CHECK(_ray_vec(rays.dx).is_equal_approx(right * 0.2));
}

TEST_CASE("[PathTracer] A view reports its history and why it last restarted") {
	Ref<PathTracerView> view;
	view.instantiate();
	view->status = PATH_TRACER_STATUS_ACTIVE;
	view->backend = PATH_TRACER_BACKEND_INLINE;
	view->samples = 12;
	view->target = 2048;
	view->resets = 2;
	view->last_reset_reasons = PATH_TRACER_RESET_CAMERA | PATH_TRACER_RESET_LIGHTS;
	view->reset_counts[2] = 1;
	view->reset_counts[4] = 1;
	Dictionary info;
	view->fill_info(info);
	CHECK_EQ(String(info["status"]), String("ACTIVE"));
	CHECK_EQ(String(info["backend"]), String("INLINE"));
	CHECK_EQ(int(info["samples"]), 12);
	CHECK_EQ(int(info["target_samples"]), 2048);
	CHECK_EQ(int(info["resets"]), 2);
	const PackedStringArray last = info["last_reset_reasons"];
	REQUIRE_EQ(last.size(), 2);
	CHECK_EQ(last[0], String("camera"));
	CHECK_EQ(last[1], String("lights"));
	const Dictionary counts = info["reset_counts"];
	CHECK_EQ(int(counts["camera"]), 1);
	CHECK_EQ(int(counts["lights"]), 1);
	CHECK_EQ(int(counts["scene"]), 0);
	// Dropping the history keeps the counters.
	view->clear_history();
	CHECK_EQ(view->samples, 0u);
	CHECK_FALSE(view->has_state);
	CHECK_EQ(view->resets, 2u);
}

} // namespace TestPathTracer
