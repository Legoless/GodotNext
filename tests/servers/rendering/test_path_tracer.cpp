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

// GodotNext: path_tracer_denoise (#168). Denoiser settings, scheduling, buffer preparation and OIDN seam.
TEST_CASE("[PathTracer] Denoiser setting validation, Viewport wiring and history invariance") {
	CHECK_EQ(int(RenderingServerTypes::PATH_TRACING_DEFAULT_DENOISER), int(RSE::VIEWPORT_PATH_TRACING_DENOISER_BUILTIN));
	CHECK_EQ(RenderingServerTypes::path_tracing_validate_denoiser(-1), RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF);
	CHECK_EQ(RenderingServerTypes::path_tracing_validate_denoiser(0), RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF);
	CHECK_EQ(RenderingServerTypes::path_tracing_validate_denoiser(1), RSE::VIEWPORT_PATH_TRACING_DENOISER_BUILTIN);
	CHECK_EQ(RenderingServerTypes::path_tracing_validate_denoiser(2), RSE::VIEWPORT_PATH_TRACING_DENOISER_OIDN);
	CHECK_EQ(RenderingServerTypes::path_tracing_validate_denoiser(3), RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF);
	CHECK_EQ(RenderingServerTypes::path_tracing_validate_denoiser(99), RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF);

	CHECK_EQ(int(Viewport::PATH_TRACING_DENOISER_OFF), int(RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF));
	CHECK_EQ(int(Viewport::PATH_TRACING_DENOISER_BUILTIN), int(RSE::VIEWPORT_PATH_TRACING_DENOISER_BUILTIN));
	CHECK_EQ(int(Viewport::PATH_TRACING_DENOISER_OIDN), int(RSE::VIEWPORT_PATH_TRACING_DENOISER_OIDN));
	CHECK_EQ(int(Viewport::PATH_TRACING_DENOISER_MAX), int(RSE::VIEWPORT_PATH_TRACING_DENOISER_MAX));

	// Changing the denoiser or requesting an on-demand denoise does not change the settings hash (never resets history).
	PathTracerSettings base;
	base.denoiser = RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF;
	const uint64_t h_off = path_tracer_hash_settings(base, PATH_TRACER_BACKEND_HIT_GROUPS);
	base.denoiser = RSE::VIEWPORT_PATH_TRACING_DENOISER_BUILTIN;
	CHECK_EQ(path_tracer_hash_settings(base, PATH_TRACER_BACKEND_HIT_GROUPS), h_off);
	base.denoiser = RSE::VIEWPORT_PATH_TRACING_DENOISER_OIDN;
	base.denoise_requested = true;
	CHECK_EQ(path_tracer_hash_settings(base, PATH_TRACER_BACKEND_HIT_GROUPS), h_off);

	// Effective settings validate the denoiser enum too.
	PathTracerSettings bad;
	bad.denoiser = RSE::ViewportPathTracingDenoiser(99);
	CHECK_EQ(path_tracer_effective_settings(bad, RSE::VIEWPORT_DEBUG_DRAW_DISABLED).denoiser, RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF);

	for (int i = 0; i < PATH_TRACER_DENOISE_STATUS_MAX; i++) {
		CHECK_NE(String(path_tracer_denoise_status_name(PathTracerDenoiseStatus(i))), String("UNKNOWN"));
	}
	for (int i = 0; i < RSE::VIEWPORT_PATH_TRACING_DENOISER_MAX; i++) {
		CHECK_NE(String(path_tracer_denoiser_name(RSE::ViewportPathTracingDenoiser(i))), String("UNKNOWN"));
	}

	// JNLM parameter validation sanitizes NaN, negative and out-of-range values.
	PathTracerDenoiseParams invalid_params;
	invalid_params.spatial_bandwidth = std::numeric_limits<float>::quiet_NaN();
	invalid_params.light_bandwidth = -1.0f;
	invalid_params.albedo_bandwidth = 0.0f;
	invalid_params.normal_bandwidth = 100.0f;
	invalid_params.filter_strength = std::numeric_limits<float>::infinity();
	invalid_params.half_search_window = 99;
	const PathTracerDenoiseParams valid = path_tracer_validate_denoise_params(invalid_params);
	CHECK_EQ(valid.spatial_bandwidth, 5.0f);
	CHECK_EQ(valid.light_bandwidth, 0.1f);
	CHECK_EQ(valid.albedo_bandwidth, 0.2f);
	CHECK_EQ(valid.normal_bandwidth, 4.0f);
	CHECK_EQ(valid.filter_strength, 10.0f);
	CHECK_EQ(valid.half_search_window, PATH_TRACER_DENOISE_MAX_HALF_SEARCH);
	invalid_params.half_search_window = 0;
	CHECK_EQ(path_tracer_validate_denoise_params(invalid_params).half_search_window, 1);
}

TEST_CASE("[SceneTree][PathTracer] Viewport.path_tracing_denoiser and request_path_tracing_denoise reach RenderingServer") {
	SubViewport *vp = memnew(SubViewport);
	CHECK_EQ(vp->get_path_tracing_denoiser(), Viewport::PATH_TRACING_DENOISER_BUILTIN);

	RenderingServer *rs = RenderingServer::get_singleton();
	REQUIRE(rs != nullptr);
	Dictionary info = rs->viewport_get_path_tracing_info(vp->get_viewport_rid());
	CHECK_EQ(int(info["denoiser_setting"]), int(RSE::VIEWPORT_PATH_TRACING_DENOISER_BUILTIN));

	vp->set_path_tracing_denoiser(Viewport::PATH_TRACING_DENOISER_OFF);
	CHECK_EQ(vp->get_path_tracing_denoiser(), Viewport::PATH_TRACING_DENOISER_OFF);
	CHECK_EQ(int(vp->get_path_tracing_info()["denoiser_setting"]), int(RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF));

	vp->set_path_tracing_denoiser(Viewport::PATH_TRACING_DENOISER_OIDN);
	CHECK_EQ(vp->get_path_tracing_denoiser(), Viewport::PATH_TRACING_DENOISER_OIDN);
	CHECK_EQ(int(rs->viewport_get_path_tracing_info(vp->get_viewport_rid())["denoiser_setting"]), int(RSE::VIEWPORT_PATH_TRACING_DENOISER_OIDN));

	rs->viewport_set_path_tracing_denoiser(vp->get_viewport_rid(), RSE::VIEWPORT_PATH_TRACING_DENOISER_BUILTIN);
	CHECK_EQ(int(rs->viewport_get_path_tracing_info(vp->get_viewport_rid())["denoiser_setting"]), int(RSE::VIEWPORT_PATH_TRACING_DENOISER_BUILTIN));

	// On-demand trigger accepts calls safely without crashing.
	vp->request_path_tracing_denoise();
	rs->viewport_request_path_tracing_denoise(vp->get_viewport_rid());

	memdelete(vp);
}

TEST_CASE("[PathTracer] Denoise scheduling runs once at convergence or on demand") {
	// Off or zero samples never runs.
	CHECK_FALSE(path_tracer_should_denoise(RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF, 64, 64, false, false, 0, RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF));
	CHECK_FALSE(path_tracer_should_denoise(RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF, 64, 64, true, false, 0, RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF));
	CHECK_FALSE(path_tracer_should_denoise(RSE::VIEWPORT_PATH_TRACING_DENOISER_BUILTIN, 0, 64, true, false, 0, RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF));

	// Accumulating without on-demand request does not run.
	CHECK_FALSE(path_tracer_should_denoise(RSE::VIEWPORT_PATH_TRACING_DENOISER_BUILTIN, 16, 64, false, false, 0, RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF));
	// Accumulating with on-demand request runs once for that sample count.
	CHECK(path_tracer_should_denoise(RSE::VIEWPORT_PATH_TRACING_DENOISER_BUILTIN, 16, 64, true, false, 0, RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF));
	CHECK_FALSE(path_tracer_should_denoise(RSE::VIEWPORT_PATH_TRACING_DENOISER_BUILTIN, 16, 64, true, true, 16, RSE::VIEWPORT_PATH_TRACING_DENOISER_BUILTIN));
	// A new sample (17) with a new on-demand request runs again.
	CHECK(path_tracer_should_denoise(RSE::VIEWPORT_PATH_TRACING_DENOISER_BUILTIN, 17, 64, true, true, 16, RSE::VIEWPORT_PATH_TRACING_DENOISER_BUILTIN));

	// Reaching target (convergence) runs once, then skips subsequent idle frames.
	CHECK(path_tracer_should_denoise(RSE::VIEWPORT_PATH_TRACING_DENOISER_BUILTIN, 64, 64, false, false, 0, RSE::VIEWPORT_PATH_TRACING_DENOISER_OFF));
	CHECK_FALSE(path_tracer_should_denoise(RSE::VIEWPORT_PATH_TRACING_DENOISER_BUILTIN, 64, 64, false, true, 64, RSE::VIEWPORT_PATH_TRACING_DENOISER_BUILTIN));

	// Switching mode while converged triggers one denoise with the new mode.
	CHECK(path_tracer_should_denoise(RSE::VIEWPORT_PATH_TRACING_DENOISER_OIDN, 64, 64, false, true, 64, RSE::VIEWPORT_PATH_TRACING_DENOISER_BUILTIN));
	CHECK_FALSE(path_tracer_should_denoise(RSE::VIEWPORT_PATH_TRACING_DENOISER_OIDN, 64, 64, false, true, 64, RSE::VIEWPORT_PATH_TRACING_DENOISER_OIDN));
}

TEST_CASE("[PathTracer] OIDN buffer preparation and RGBA repacking normalize guides and sanitize non-finite values") {
	// 2x1 image: pixel 0 is a valid surface hit (4 samples); pixel 1 is sky miss with NaN radiance.
	const float accum[8] = {
		4.0f, 8.0f, -2.0f, 4.0f,
		std::numeric_limits<float>::quiet_NaN(), 2.0f, std::numeric_limits<float>::infinity(), 2.0f,
	};
	const float albedo[8] = {
		3.0f, 6.0f, 1.0f, 4.0f,
		0.0f, 0.0f, 0.0f, 0.0f,
	};
	const float normal[8] = {
		0.0f, 6.0f, 8.0f, 4.0f,
		0.0f, 0.0f, 0.0f, 0.0f,
	};

	LocalVector<float> color_rgb;
	LocalVector<float> albedo_rgb;
	LocalVector<float> normal_rgb;
	REQUIRE(path_tracer_prepare_oidn_buffers(2, 1, accum, albedo, normal, color_rgb, albedo_rgb, normal_rgb));
	REQUIRE_EQ(color_rgb.size(), 6u);
	REQUIRE_EQ(albedo_rgb.size(), 6u);
	REQUIRE_EQ(normal_rgb.size(), 6u);

	// Pixel 0: mean radiance (1, 2, 0 clamped), albedo (0.75, 1.0 clamped, 0.25), unit normal (0, 0.6, 0.8).
	CHECK(Math::is_equal_approx(color_rgb[0], 1.0f));
	CHECK(Math::is_equal_approx(color_rgb[1], 2.0f));
	CHECK_EQ(color_rgb[2], 0.0f);
	CHECK(Math::is_equal_approx(albedo_rgb[0], 0.75f));
	CHECK(Math::is_equal_approx(albedo_rgb[1], 1.0f));
	CHECK(Math::is_equal_approx(albedo_rgb[2], 0.25f));
	CHECK(Math::is_equal_approx(normal_rgb[0], 0.0f));
	CHECK(Math::is_equal_approx(normal_rgb[1], 0.6f));
	CHECK(Math::is_equal_approx(normal_rgb[2], 0.8f));

	// Pixel 1: NaN/Inf sanitized to 0, sky normal stays 0.
	CHECK_EQ(color_rgb[3], 0.0f);
	CHECK(Math::is_equal_approx(color_rgb[4], 1.0f));
	CHECK_EQ(color_rgb[5], 0.0f);
	CHECK_EQ(albedo_rgb[3], 0.0f);
	CHECK_EQ(normal_rgb[3], 0.0f);
	CHECK_EQ(normal_rgb[4], 0.0f);
	CHECK_EQ(normal_rgb[5], 0.0f);

	// Repack RGB32F -> RGBA32F with alpha = 1.0 and non-finite sanitization.
	const float rgb_out[6] = {
		0.5f, -1.0f, std::numeric_limits<float>::quiet_NaN(),
		2.0f, 3.0f, std::numeric_limits<float>::infinity(),
	};
	Vector<uint8_t> rgba_bytes;
	path_tracer_pack_denoised_rgba(2, 1, rgb_out, rgba_bytes);
	REQUIRE_EQ(rgba_bytes.size(), 32);
	const float *rgba = reinterpret_cast<const float *>(rgba_bytes.ptr());
	CHECK(Math::is_equal_approx(rgba[0], 0.5f));
	CHECK_EQ(rgba[1], 0.0f);
	CHECK_EQ(rgba[2], 0.0f);
	CHECK_EQ(rgba[3], 1.0f);
	CHECK(Math::is_equal_approx(rgba[4], 2.0f));
	CHECK(Math::is_equal_approx(rgba[5], 3.0f));
	CHECK_EQ(rgba[6], 0.0f);
	CHECK_EQ(rgba[7], 1.0f);
}

static Error _test_oidn_stub(const PathTracerOIDNInput &p_input, String &r_error) {
	if (p_input.width == 0 || p_input.height == 0 || p_input.color == nullptr || p_input.output == nullptr) {
		r_error = "invalid";
		return ERR_INVALID_PARAMETER;
	}
	const uint32_t n = p_input.width * p_input.height * 3u;
	for (uint32_t i = 0; i < n; i++) {
		p_input.output[i] = p_input.color[i] * 0.5f;
	}
	return OK;
}

TEST_CASE("[PathTracer] OIDN backend hook registration, dispatch and view info reporting") {
	// Save any pre-existing backend (e.g. when built with module_oidn_enabled=yes).
	const bool had_backend = PathTracer::is_oidn_available();
	const String original_name = String(PathTracer::get_oidn_backend_name());
	if (!had_backend) {
		CHECK_EQ(original_name, String("none"));
		PathTracerOIDNInput dummy;
		String err;
		CHECK_EQ(PathTracer::run_oidn_denoise(dummy, err), ERR_UNAVAILABLE);
		CHECK_FALSE(err.is_empty());

		static const PathTracerOIDNBackend test_backend = { "test_stub", &_test_oidn_stub };
		PathTracer::register_oidn_backend(&test_backend);
		CHECK(PathTracer::is_oidn_available());
		CHECK_EQ(String(PathTracer::get_oidn_backend_name()), String("test_stub"));

		const float in_rgb[3] = { 2.0f, 4.0f, 6.0f };
		float out_rgb[3] = {};
		dummy.width = 1;
		dummy.height = 1;
		dummy.color = in_rgb;
		dummy.output = out_rgb;
		err = String();
		CHECK_EQ(PathTracer::run_oidn_denoise(dummy, err), OK);
		CHECK(Math::is_equal_approx(out_rgb[0], 1.0f));
		CHECK(Math::is_equal_approx(out_rgb[1], 2.0f));
		CHECK(Math::is_equal_approx(out_rgb[2], 3.0f));

		PathTracer::unregister_oidn_backend(&test_backend);
		CHECK_FALSE(PathTracer::is_oidn_available());
		CHECK_EQ(String(PathTracer::get_oidn_backend_name()), String("none"));
	}

	// PathTracerView::fill_info reports denoiser fields cleanly.
	Ref<PathTracerView> view;
	view.instantiate();
	view->samples = 32;
	view->settings.denoiser = RSE::VIEWPORT_PATH_TRACING_DENOISER_OIDN;
	view->denoised_valid = true;
	view->denoised_samples = 32;
	view->denoise_backend = RSE::VIEWPORT_PATH_TRACING_DENOISER_BUILTIN;
	view->denoise_status = PATH_TRACER_DENOISE_STATUS_FALLBACK_BUILTIN;
	view->denoise_runs = 3;
	Dictionary info;
	view->fill_info(info);
	CHECK_EQ(String(info["denoiser"]), String("OIDN"));
	CHECK_EQ(String(info["denoise_status"]), String("FALLBACK_BUILTIN"));
	CHECK_EQ(String(info["denoise_backend"]), String("BUILTIN"));
	CHECK(bool(info["denoised"]));
	CHECK_EQ(int(info["denoised_samples"]), 32);
	CHECK_EQ(int(info["denoise_runs"]), 3);
	CHECK_EQ(bool(info["oidn_available"]), had_backend);
	CHECK_EQ(String(info["oidn_backend"]), original_name);
}

// GodotNext: path_tracer_lights_camera (#169).

TEST_CASE("[PathTracer] Scenario light ordering, deduplication and bake-disabled inclusion (R14)") {
	CHECK_EQ(sizeof(PathTracerGPULight), size_t(144));
	CHECK_EQ(sizeof(PathTracerGPUFogVolume), size_t(128));

	PathTracerLightEntry raw[7];
	// Intentionally unsorted types, duplicate rid_id, invalid rid_id=0, and bake_disabled lights.
	raw[0] = { PATH_TRACER_LIGHT_AREA, 50, RID(), false };
	raw[1] = { PATH_TRACER_LIGHT_OMNI, 20, RID(), true }; // bake-disabled must be kept!
	raw[2] = { PATH_TRACER_LIGHT_DIRECTIONAL, 99, RID(), false };
	raw[3] = { PATH_TRACER_LIGHT_SPOT, 10, RID(), true }; // bake-disabled spot
	raw[4] = { PATH_TRACER_LIGHT_OMNI, 20, RID(), false }; // duplicate rid_id=20
	raw[5] = { PATH_TRACER_LIGHT_OMNI, 5, RID(), false };
	raw[6] = { PATH_TRACER_LIGHT_SPOT, 0, RID(), false }; // invalid rid_id=0 dropped

	LocalVector<PathTracerLightEntry> sorted;
	path_tracer_collect_and_sort_lights(raw, 7, sorted);
	REQUIRE_EQ(sorted.size(), 5u);

	// Deterministic order: (type ascending, rid_id ascending).
	CHECK_EQ(sorted[0].type, uint32_t(PATH_TRACER_LIGHT_DIRECTIONAL));
	CHECK_EQ(sorted[0].rid_id, 99ull);
	CHECK_EQ(sorted[1].type, uint32_t(PATH_TRACER_LIGHT_OMNI));
	CHECK_EQ(sorted[1].rid_id, 5ull);
	CHECK_EQ(sorted[2].type, uint32_t(PATH_TRACER_LIGHT_OMNI));
	CHECK_EQ(sorted[2].rid_id, 20ull);
	CHECK_EQ(sorted[3].type, uint32_t(PATH_TRACER_LIGHT_SPOT));
	CHECK_EQ(sorted[3].rid_id, 10ull);
	CHECK(sorted[3].bake_disabled);
	CHECK_EQ(sorted[4].type, uint32_t(PATH_TRACER_LIGHT_AREA));
	CHECK_EQ(sorted[4].rid_id, 50ull);

	// Order is invariant under input permutation.
	PathTracerLightEntry reversed[7];
	for (int i = 0; i < 7; i++) {
		reversed[i] = raw[6 - i];
	}
	LocalVector<PathTracerLightEntry> sorted_rev;
	path_tracer_collect_and_sort_lights(reversed, 7, sorted_rev);
	REQUIRE_EQ(sorted_rev.size(), sorted.size());
	for (uint32_t i = 0; i < sorted.size(); i++) {
		CHECK_EQ(sorted_rev[i].type, sorted[i].type);
		CHECK_EQ(sorted_rev[i].rid_id, sorted[i].rid_id);
	}
}

TEST_CASE("[PathTracer] NEE/MIS balance heuristic and solid-angle light PDFs") {
	// Partition of unity: w(a, b) + w(b, a) == 1 for all positive PDFs.
	const float pairs[4][2] = { { 1.0f, 1.0f }, { 0.25f, 3.75f }, { 100.0f, 0.01f }, { 1e-4f, 1e-3f } };
	for (const auto &p : pairs) {
		const float wa = path_tracer_mis_balance_weight(p[0], p[1]);
		const float wb = path_tracer_mis_balance_weight(p[1], p[0]);
		CHECK(Math::is_equal_approx(wa + wb, 1.0f));
		CHECK(wa > 0.0f);
		CHECK(wb > 0.0f);
	}
	// Delta light (pdf_b == 0 -> w = 1), invalid/zero pdf_a -> 0.
	CHECK(Math::is_equal_approx(path_tracer_mis_balance_weight(2.0f, 0.0f), 1.0f));
	CHECK_EQ(path_tracer_mis_balance_weight(0.0f, 2.0f), 0.0f);
	CHECK_EQ(path_tracer_mis_balance_weight(-1.0f, 2.0f), 0.0f);
	CHECK_EQ(path_tracer_mis_balance_weight(std::numeric_limits<float>::quiet_NaN(), 1.0f), 0.0f);

	// Cone solid angle: 2*pi*(1 - cos_max).
	CHECK(Math::is_equal_approx(path_tracer_cone_solid_angle(1.0f), 0.0f));
	CHECK(Math::is_equal_approx(path_tracer_cone_solid_angle(0.0f), float(Math::TAU)));
	CHECK(Math::is_equal_approx(path_tracer_cone_solid_angle(-1.0f), float(4.0 * Math::PI)));

	// Area light solid-angle PDF: dist^2 / (area * cos_light).
	CHECK(Math::is_equal_approx(path_tracer_area_light_pdf_omega(4.0f, 2.0f, 0.5f), 4.0f));
	CHECK_EQ(path_tracer_area_light_pdf_omega(4.0f, 2.0f, 0.0f), 0.0f);
	CHECK_EQ(path_tracer_area_light_pdf_omega(4.0f, 0.0f, 1.0f), 0.0f);
}

TEST_CASE("[PathTracer] Sky octahedral Jacobian and 2D CDF importance sampling") {
	// Octahedral encode/decode round-trip across all 8 octants and axes.
	const Vector3 dirs[] = {
		Vector3(1, 0, 0),
		Vector3(-1, 0, 0),
		Vector3(0, 1, 0),
		Vector3(0, -1, 0),
		Vector3(0, 0, 1),
		Vector3(0, 0, -1),
		Vector3(1, 2, 3).normalized(),
		Vector3(-2, 1, -3).normalized(),
		Vector3(1, -3, -1).normalized(),
		Vector3(-1, -1, 2).normalized(),
	};
	for (const Vector3 &d : dirs) {
		const Vector2 uv = path_tracer_sky_dir_to_oct(d);
		const Vector3 rt = path_tracer_sky_oct_to_dir(uv);
		CHECK(d.dot(rt) > 0.9999f);
	}

	// Numerical integration of octahedral Jacobian over [0, 1]^2 must equal 4*pi.
	const uint32_t qn = 64;
	double solid_angle_sum = 0.0;
	for (uint32_t y = 0; y < qn; y++) {
		for (uint32_t x = 0; x < qn; x++) {
			const Vector2 uv((float(x) + 0.5f) / float(qn), (float(y) + 0.5f) / float(qn));
			solid_angle_sum += double(path_tracer_sky_oct_jacobian(path_tracer_sky_oct_to_dir(uv))) / double(qn * qn);
		}
	}
	CHECK(Math::abs(solid_angle_sum - 4.0 * Math::PI) < 0.02);

	// 2D sky CDF: bright localized sun patch in upper hemisphere vs dim ambient sky.
	const uint32_t dim = 32;
	LocalVector<float> lum;
	lum.resize(dim * dim);
	double exact_integral = 0.0;
	const Vector3 sun_dir = Vector3(0.3f, 0.8f, 0.5f).normalized();
	for (uint32_t y = 0; y < dim; y++) {
		for (uint32_t x = 0; x < dim; x++) {
			const Vector2 uv((float(x) + 0.5f) / float(dim), (float(y) + 0.5f) / float(dim));
			const Vector3 d = path_tracer_sky_oct_to_dir(uv);
			const float val = (d.dot(sun_dir) > 0.92f) ? 50.0f : 0.1f;
			lum[y * dim + x] = val;
			exact_integral += double(val) * double(path_tracer_sky_oct_jacobian(d)) / double(dim * dim);
		}
	}

	PathTracerSkyCdfCPU cdf;
	path_tracer_build_sky_cdf_cpu(lum.ptr(), dim, cdf);
	CHECK(cdf.total_weight > 0.0f);
	CHECK(Math::is_equal_approx(cdf.marginal[dim - 1], 1.0f));

	// Monte Carlo estimator E[L(d) / pdf(d)] via 2D CDF sampling must match exact_integral and concentrate on sun.
	uint32_t rng = 0x12345678u;
	auto rnd = [&rng]() -> float {
		rng = rng * 747796405u + 2891336453u;
		uint32_t w = ((rng >> ((rng >> 28u) + 4u)) ^ rng) * 277803737u;
		w = (w >> 22u) ^ w;
		return float(w & 0x00FFFFFFu) * (1.0f / 16777216.0f);
	};
	const uint32_t samples = 8192;
	double mc_sum = 0.0;
	uint32_t sun_hits = 0;
	for (uint32_t i = 0; i < samples; i++) {
		Vector3 d;
		float pdf = 0.0f;
		path_tracer_sample_sky_cdf_cpu(cdf, Vector2(rnd(), rnd()), d, pdf);
		REQUIRE(pdf > 0.0f);
		// Consistency between sampled PDF and lookup PDF for MIS.
		const float lookup_pdf = path_tracer_sky_cdf_pdf_cpu(cdf, d);
		CHECK(Math::abs(lookup_pdf - pdf) <= pdf * 0.05f + 1e-4f);
		const Vector2 uv = path_tracer_sky_dir_to_oct(d);
		const uint32_t ix = MIN(uint32_t(CLAMP(uv.x, 0.0f, 0.999999f) * float(dim)), dim - 1);
		const uint32_t iy = MIN(uint32_t(CLAMP(uv.y, 0.0f, 0.999999f) * float(dim)), dim - 1);
		const float lval = lum[iy * dim + ix];
		if (lval > 10.0f) {
			sun_hits++;
		}
		mc_sum += double(lval) / double(pdf);
	}
	const double mc_est = mc_sum / double(samples);
	CHECK(sun_hits > samples / 2); // Importance sampling concentrates majority of samples on bright sun patch.
	CHECK(Math::abs(mc_est - exact_integral) / exact_integral < 0.03);
}

static float _test_sigma_const(float p_t, void *p_ctx) {
	(void)p_t;
	return *static_cast<const float *>(p_ctx);
}

static float _test_sigma_ramp(float p_t, void *p_ctx) {
	const float *params = static_cast<const float *>(p_ctx); // [peak, length]
	const float u = CLAMP(p_t / params[1], 0.0f, 1.0f);
	return params[0] * (1.0f - u);
}

TEST_CASE("[PathTracer] Volumetric fog SDFs, Henyey-Greenstein phase and delta/ratio tracking unbiasedness") {
	// Analytic SDF signs for all 5 Godot FogVolume shapes.
	const Vector3 hs(1.0f, 1.0f, 1.0f);
	CHECK(path_tracer_fog_volume_sdf(Vector3(0, 0, 0), hs, uint32_t(RSE::FOG_VOLUME_SHAPE_ELLIPSOID)) < 0.0f);
	CHECK(path_tracer_fog_volume_sdf(Vector3(2, 0, 0), hs, uint32_t(RSE::FOG_VOLUME_SHAPE_ELLIPSOID)) > 0.0f);
	CHECK(path_tracer_fog_volume_sdf(Vector3(0, -0.5f, 0), hs, uint32_t(RSE::FOG_VOLUME_SHAPE_CONE)) < 0.0f);
	CHECK(path_tracer_fog_volume_sdf(Vector3(2, 0, 0), hs, uint32_t(RSE::FOG_VOLUME_SHAPE_CONE)) > 0.0f);
	CHECK(path_tracer_fog_volume_sdf(Vector3(0, 0, 0), hs, uint32_t(RSE::FOG_VOLUME_SHAPE_CYLINDER)) < 0.0f);
	CHECK(path_tracer_fog_volume_sdf(Vector3(2, 0, 0), hs, uint32_t(RSE::FOG_VOLUME_SHAPE_CYLINDER)) > 0.0f);
	CHECK(path_tracer_fog_volume_sdf(Vector3(0, 0, 0), hs, uint32_t(RSE::FOG_VOLUME_SHAPE_BOX)) < 0.0f);
	CHECK(path_tracer_fog_volume_sdf(Vector3(2, 0, 0), hs, uint32_t(RSE::FOG_VOLUME_SHAPE_BOX)) > 0.0f);
	CHECK(path_tracer_fog_volume_sdf(Vector3(100, 100, 100), hs, uint32_t(RSE::FOG_VOLUME_SHAPE_WORLD)) < 0.0f);

	// Henyey-Greenstein phase function and importance sampler.
	CHECK(Math::is_equal_approx(path_tracer_hg_phase(0.5f, 0.0f), float(1.0 / (4.0 * Math::PI))));
	CHECK(path_tracer_hg_phase(1.0f, 0.7f) > path_tracer_hg_phase(-1.0f, 0.7f));
	CHECK(path_tracer_hg_phase(-1.0f, -0.7f) > path_tracer_hg_phase(1.0f, -0.7f));
	{
		uint32_t rng = 0xCAFEBABEu;
		auto rnd = [&rng]() -> float {
			rng = rng * 747796405u + 2891336453u;
			uint32_t w = ((rng >> ((rng >> 28u) + 4u)) ^ rng) * 277803737u;
			w = (w >> 22u) ^ w;
			return float(w & 0x00FFFFFFu) * (1.0f / 16777216.0f);
		};
		const float g = 0.65f;
		const Vector3 fwd(0.0f, 0.0f, -1.0f);
		double cos_sum = 0.0;
		const uint32_t n = 4096;
		for (uint32_t i = 0; i < n; i++) {
			float pdf = 0.0f;
			const Vector3 s = path_tracer_sample_hg(fwd, g, Vector2(rnd(), rnd()), pdf);
			CHECK(pdf > 0.0f);
			cos_sum += double(fwd.dot(s));
		}
		// Mean cosine of Henyey-Greenstein phase equals g.
		CHECK(Math::abs(cos_sum / double(n) - double(g)) < 0.03);
	}

	// Unbiased Woodcock delta tracking and ratio tracking on homogeneous and heterogeneous media.
	{
		float sigma = 0.4f;
		const float seg_len = 3.0f;
		const double exact = Math::exp(-double(sigma) * double(seg_len));
		uint32_t rng_d = 0x11112222u;
		uint32_t rng_r = 0x33334444u;
		double sum_d = 0.0;
		double sum_r = 0.0;
		const uint32_t trials = 16384;
		// Use majorant > sigma (2.5x) to exercise null collisions heavily.
		for (uint32_t i = 0; i < trials; i++) {
			sum_d += double(path_tracer_delta_tracking_transmittance_cpu(seg_len, 1.0f, &_test_sigma_const, &sigma, rng_d));
			sum_r += double(path_tracer_ratio_tracking_transmittance_cpu(seg_len, 1.0f, &_test_sigma_const, &sigma, rng_r));
		}
		CHECK(Math::abs(sum_d / double(trials) - exact) < 0.015);
		CHECK(Math::abs(sum_r / double(trials) - exact) < 0.015);
	}
	{
		float ramp[2] = { 0.8f, 3.0f }; // integral_0^3 0.8*(1 - t/3) dt = 1.2
		const double exact = Math::exp(-1.2);
		uint32_t rng_d = 0x55556666u;
		uint32_t rng_r = 0x77778888u;
		double sum_d = 0.0;
		double sum_r = 0.0;
		const uint32_t trials = 16384;
		for (uint32_t i = 0; i < trials; i++) {
			sum_d += double(path_tracer_delta_tracking_transmittance_cpu(ramp[1], 1.2f, &_test_sigma_ramp, ramp, rng_d));
			sum_r += double(path_tracer_ratio_tracking_transmittance_cpu(ramp[1], 1.2f, &_test_sigma_ramp, ramp, rng_r));
		}
		CHECK(Math::abs(sum_d / double(trials) - exact) < 0.015);
		CHECK(Math::abs(sum_r / double(trials) - exact) < 0.015);
	}
}

TEST_CASE("[PathTracer] Thin-lens DOF parameter mapping and focal-plane ray convergence") {
	const float fov = Math::deg_to_rad(60.0f);
	// Disabled when orthogonal, zero blur, or neither near/far enabled.
	CHECK_FALSE(path_tracer_compute_dof_params(true, 5.0f, 2.0f, false, 1.0f, 1.0f, 0.2f, fov, 512, true).enabled);
	CHECK_FALSE(path_tracer_compute_dof_params(true, 5.0f, 2.0f, false, 1.0f, 1.0f, 0.0f, fov, 512, false).enabled);
	CHECK_FALSE(path_tracer_compute_dof_params(false, 5.0f, 2.0f, false, 1.0f, 1.0f, 0.2f, fov, 512, false).enabled);

	// Practical far DOF scales aperture monotonically with blur_amount.
	const PathTracerDOFParams low = path_tracer_compute_dof_params(true, 5.0f, 2.0f, false, 1.0f, 1.0f, 0.1f, fov, 512, false);
	const PathTracerDOFParams high = path_tracer_compute_dof_params(true, 5.0f, 2.0f, false, 1.0f, 1.0f, 0.4f, fov, 512, false);
	CHECK(low.enabled);
	CHECK(high.enabled);
	CHECK(Math::is_equal_approx(low.focus_distance, 5.0f));
	CHECK(high.aperture_radius > low.aperture_radius);

	// Concentric disk maps [0, 1]^2 inside unit disk, center -> (0, 0).
	CHECK(path_tracer_sample_concentric_disk(Vector2(0.5f, 0.5f)).length() < 1e-6f);
	for (int y = 0; y <= 8; y++) {
		for (int x = 0; x <= 8; x++) {
			const Vector2 d = path_tracer_sample_concentric_disk(Vector2(float(x) / 8.0f, float(y) / 8.0f));
			CHECK(d.length() <= 1.0001f);
		}
	}

	// All thin-lens rays for a pixel must intersect at the exact focal plane point (zero CoC in focus),
	// while spreading apart at an out-of-focus plane.
	const Vector3 cam_origin(0.0f, 1.5f, 0.0f);
	const Vector3 cam_fwd(0.0f, 0.0f, -1.0f);
	const Vector3 cam_right(1.0f, 0.0f, 0.0f);
	const Vector3 cam_up(0.0f, 1.0f, 0.0f);
	const Vector3 pinhole_dir = Vector3(0.2f, -0.1f, -1.0f).normalized();
	const Vector3 expected_focus = cam_origin + pinhole_dir * (high.focus_distance / pinhole_dir.dot(cam_fwd));

	Vector3 far_min(1e9f, 1e9f, 1e9f);
	Vector3 far_max(-1e9f, -1e9f, -1e9f);
	const Vector2 lens_samples[5] = { Vector2(0.1f, 0.2f), Vector2(0.9f, 0.2f), Vector2(0.1f, 0.9f), Vector2(0.9f, 0.9f), Vector2(0.5f, 0.5f) };
	for (const Vector2 &u : lens_samples) {
		Vector3 ro;
		Vector3 rd;
		path_tracer_apply_thin_lens(cam_origin, pinhole_dir, cam_fwd, cam_right, cam_up, high, u, ro, rd);
		// Intersect focal plane (z = -focus_distance).
		const float t_focus = (expected_focus.z - ro.z) / rd.z;
		const Vector3 hit_focus = ro + rd * t_focus;
		CHECK(hit_focus.distance_to(expected_focus) < 1e-4f);
		// Intersect out-of-focus plane at z = -2 * focus_distance.
		const float t_far = (-2.0f * high.focus_distance - ro.z) / rd.z;
		const Vector3 hit_far = ro + rd * t_far;
		far_min = far_min.min(hit_far);
		far_max = far_max.max(hit_far);
	}
	CHECK((far_max - far_min).length() > high.aperture_radius * 0.5f);
}

TEST_CASE("[PathTracer] Motion blur transform interpolation and shutter interval sampling") {
	Transform3D prev(Basis(Vector3(0, 1, 0), 0.0f).scaled(Vector3(1.0f, 1.0f, 1.0f)), Vector3(0.0f, 0.0f, 0.0f));
	Transform3D curr(Basis(Vector3(0, 1, 0), float(Math::PI * 0.5)).scaled(Vector3(2.0f, 2.0f, 2.0f)), Vector3(4.0f, 2.0f, -6.0f));

	const Transform3D at0 = path_tracer_interpolate_transform(prev, curr, 0.0f);
	const Transform3D at1 = path_tracer_interpolate_transform(prev, curr, 1.0f);
	const Transform3D mid = path_tracer_interpolate_transform(prev, curr, 0.5f);

	CHECK(at0.origin.is_equal_approx(prev.origin));
	CHECK(at1.origin.is_equal_approx(curr.origin));
	CHECK(mid.origin.is_equal_approx(Vector3(2.0f, 1.0f, -3.0f)));
	CHECK(mid.basis.get_scale().is_equal_approx(Vector3(1.5f, 1.5f, 1.5f)));
	// Midpoint rotation of 0 -> 90 deg around Y must be 45 deg around Y.
	const Vector3 rotated_x = mid.basis.orthonormalized().xform(Vector3(1.0f, 0.0f, 0.0f));
	const Vector3 expected_x(Math::cos(float(Math::PI * 0.25)), 0.0f, -Math::sin(float(Math::PI * 0.25)));
	CHECK(rotated_x.is_equal_approx(expected_x));

	// Shutter time helper: shutter=0 pins to 1.0; shutter=1 spans [0, 1] strictly increasing across slices.
	CHECK(Math::is_equal_approx(path_tracer_shutter_time(0, 3, 8, 0.0f), 1.0f));
	float prev_t = -1.0f;
	for (uint32_t s = 0; s < PATH_TRACER_MOTION_SLICES; s++) {
		const float t = path_tracer_shutter_time(0, s, PATH_TRACER_MOTION_SLICES, 1.0f);
		CHECK(t >= 0.0f);
		CHECK(t <= 1.0f);
		CHECK(t > prev_t);
		prev_t = t;
	}
	// Sample index jitters sub-slice shutter time deterministically.
	CHECK_FALSE(Math::is_equal_approx(
			path_tracer_shutter_time(0, 2, PATH_TRACER_MOTION_SLICES, 1.0f),
			path_tracer_shutter_time(1, 2, PATH_TRACER_MOTION_SLICES, 1.0f)));
}

} // namespace TestPathTracer
