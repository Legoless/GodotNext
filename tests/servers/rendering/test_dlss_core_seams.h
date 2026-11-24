/**************************************************************************/
/*  test_dlss_core_seams.h                                                */
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

// GodotNext: vendor-neutral core seams for #206 (RR settings/reasons/DenoiserBypass),
// #209 (core/os/latency_markers.h, Reflex settings & status API), and #200/#201
// (FrameGenerationExtension C++ virtual overrides, new reasons, dynamic_target_fps,
// provider capabilities in status).

#include "core/config/project_settings.h"
#include "core/error/error_macros.h"
#include "core/object/class_db.h"
#include "core/os/latency_markers.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/storage/frame_generation_extension.h"
#include "tests/test_macros.h"

#ifdef RD_ENABLED
#include "servers/rendering/renderer_rd/effects/denoise.h"
#include "servers/rendering/renderer_rd/effects/denoiser_bypass.h"
#include "servers/rendering/renderer_rd/effects/temporal_upscaler.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#endif

namespace TestDLSSCoreSeams {

struct WarningCounter {
	ErrorHandlerList handler;
	int warnings = 0;
	int errors = 0;
	LocalVector<String> messages;

	static void _handle(void *p_self, const char *p_func, const char *p_file, int p_line, const char *p_error, const char *p_errorexp, bool p_editor_notify, ErrorHandlerType p_type) {
		WarningCounter *self = static_cast<WarningCounter *>(p_self);
		if (p_type == ERR_HANDLER_WARNING) {
			self->warnings++;
		} else {
			self->errors++;
		}
		self->messages.push_back(String::utf8(p_error) + " " + String::utf8(p_errorexp));
	}

	int lines_containing(const String &p_token) const {
		int count = 0;
		for (const String &msg : messages) {
			if (msg.contains(p_token)) {
				count++;
			}
		}
		return count;
	}

	WarningCounter() {
		handler.errfunc = _handle;
		handler.userdata = this;
		add_error_handler(&handler);
	}
	~WarningCounter() {
		remove_error_handler(&handler);
	}
};

struct LatencyMarkerEvent {
	LatencyMarker marker;
	uint64_t frame_id;
};

static LocalVector<LatencyMarkerEvent> recorded_latency_events;

static void record_latency_marker(LatencyMarker p_marker, uint64_t p_frame_id) {
	recorded_latency_events.push_back({ p_marker, p_frame_id });
}

struct LatencyMarkerScope {
	LatencyMarkerScope() {
		recorded_latency_events.clear();
		reset_latency_markers_for_tests();
	}
	~LatencyMarkerScope() {
		reset_latency_markers_for_tests();
		recorded_latency_events.clear();
	}
};

static Dictionary mock_latency_status_callback() {
	Dictionary d;
	d["mode"] = 2;
	d["reason"] = String("OK");
	return d;
}

// Pure C++ FrameGenerationExtension provider exercising the virtual overrides without a ScriptInstance.
class MockCppFrameGenerationProvider : public FrameGenerationExtension {
	GDCLASS(MockCppFrameGenerationProvider, FrameGenerationExtension);

public:
	String name = "MockCppFG";
	FrameGenerationReason availability = REASON_OK;
	Dictionary capabilities;
	Dictionary last_mode;
	int set_mode_calls = 0;
	int available_calls = 0;
	int frame_inputs_calls = 0;
	int present_calls = 0;
	int reset_calls = 0;

	String get_provider_name() const override {
		return name;
	}

	FrameGenerationReason is_available() const override {
		const_cast<MockCppFrameGenerationProvider *>(this)->available_calls++;
		return availability;
	}

	void set_mode(const Dictionary &p_mode) override {
		set_mode_calls++;
		last_mode = p_mode;
	}

	void on_frame_inputs(const Ref<FrameGenerationInputs> &p_inputs) override {
		frame_inputs_calls++;
	}

	void on_present(Object *p_inputs, bool p_before) override {
		present_calls++;
	}

	void reset() override {
		reset_calls++;
	}

	Dictionary get_provider_capabilities() const override {
		return capabilities;
	}
};

#ifdef RD_ENABLED
// Minimal mock TemporalUpscaler exercising the RR query seam on TemporalUpscaler.
class MockRRTemporalUpscaler : public TemporalUpscaler {
public:
	bool rr_active = false;
	String rr_maturity = "beta";
	RSE::ViewportScaling3DReason rr_reason = RSE::VIEWPORT_SCALING_3D_REASON_NONE;

	String get_display_name() const override { return "Mock DLSS"; }
	const Span<char> get_label() const override { return "Mock DLSS"; }
	bool supports_api(const String &p_api_name) const override { return true; }
	bool is_ray_reconstruction_active(const RenderSceneBuffersRD *p_rb) const override { return rr_active; }
	String get_rr_maturity_label() const override { return rr_maturity; }
	RSE::ViewportScaling3DReason get_rr_reason(const RenderSceneBuffersRD *p_rb) const override { return rr_reason; }
	RSE::ViewportScaling3DReason ensure_context(RenderSceneBuffersRD *p_render_buffers, uint32_t p_view) override { return RSE::VIEWPORT_SCALING_3D_REASON_NONE; }
	void process(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Inputs &p_inputs) override {}
	void free_contexts(RenderSceneBuffersRD *p_render_buffers) override {}
};

struct DenoiserBypassScope {
	Vector<StringName> saved_denoisers;
	DenoiserBypassScope() {
		saved_denoisers = DenoiserBypass::get_registered_denoisers();
		DenoiserBypass::clear_for_tests();
	}
	~DenoiserBypassScope() {
		DenoiserBypass::clear_for_tests();
		for (const StringName &name : saved_denoisers) {
			DenoiserBypass::register_denoiser(name);
		}
	}
};
#endif

} // namespace TestDLSSCoreSeams

#ifdef RD_ENABLED
TEST_CASE("[DLSS][NextGI][MegaLights] DenoiserBypass contract, registration, and line-8 skip") {
	using namespace TestDLSSCoreSeams;
	DenoiserBypassScope scope;

	CHECK(DenoiserBypass::get_registered_denoisers().is_empty());
	DenoiserBypass::register_denoiser(StringName());
	CHECK(DenoiserBypass::get_registered_denoisers().is_empty());

	// Constructing NextGI and NextGIDenoise(false) registers nextgi_line8 and megalights_denoise.
	{
		RendererRD::NextGI ngi;
		RendererRD::NextGIDenoise ml_denoise(false);
		const Vector<StringName> registered = DenoiserBypass::get_registered_denoisers();
		CHECK(registered.has(SNAME("nextgi_line8")));
		CHECK(registered.has(SNAME("megalights_denoise")));

		// Duplicate registrations are deduplicated.
		DenoiserBypass::register_denoiser(SNAME("nextgi_line8"));
		CHECK_EQ(DenoiserBypass::get_registered_denoisers().size(), registered.size());

		// Per-rb bypass isolation and null safety.
		const RenderSceneBuffersRD *rb_a = reinterpret_cast<const RenderSceneBuffersRD *>(uintptr_t(0x1000));
		const RenderSceneBuffersRD *rb_b = reinterpret_cast<const RenderSceneBuffersRD *>(uintptr_t(0x2000));
		CHECK_FALSE(DenoiserBypass::is_bypassed(nullptr));
		CHECK_FALSE(DenoiserBypass::is_bypassed(rb_a));
		CHECK_FALSE(DenoiserBypass::is_bypassed(rb_b));

		DenoiserBypass::set_bypassed(rb_a, true);
		CHECK(DenoiserBypass::is_bypassed(rb_a));
		CHECK_FALSE(DenoiserBypass::is_bypassed(rb_b));

		// MegaLights denoiser honours DenoiserBypass::is_bypassed(rb) even when dp.rr_bypass == false.
		RendererRD::NextGIDenoise::MegaLightsDenoiseParams dp;
		dp.rr_bypass = false;
		dp.render_buffers = rb_a;
		dp.noisy_diffuse = RID::from_uint64(101);
		dp.noisy_specular = RID::from_uint64(102);
		dp.depth = RID::from_uint64(103);
		dp.params_ubo = RID::from_uint64(104);
		dp.screen_size = Size2i(0, 0);
		const RendererRD::NextGIDenoise::MegaLightsDenoiseOutput bypassed_out = ml_denoise.megalights_denoise(dp);
		CHECK(bypassed_out.bypassed);
		CHECK_EQ(bypassed_out.diffuse, dp.noisy_diffuse);
		CHECK_EQ(bypassed_out.specular, dp.noisy_specular);

		dp.render_buffers = rb_b;
		const RendererRD::NextGIDenoise::MegaLightsDenoiseOutput active_out = ml_denoise.megalights_denoise(dp);
		CHECK_FALSE(active_out.bypassed);

		DenoiserBypass::clear_rb(rb_a);
		CHECK_FALSE(DenoiserBypass::is_bypassed(rb_a));
	}

	// Under RR bypass, NextGI emits ZERO line-8 (PASS_SLOT_DENOISE) timestamps while keeping line 7 active.
	CHECK_FALSE(RendererRD::NextGI::should_emit_pass_slot(RendererRD::NextGI::PASS_SLOT_DENOISE, true));
	CHECK(RendererRD::NextGI::should_emit_pass_slot(RendererRD::NextGI::PASS_SLOT_DENOISE, false));
	CHECK(RendererRD::NextGI::should_emit_pass_slot(RendererRD::NextGI::PASS_SLOT_INTEGRATION, true));
	CHECK(RendererRD::NextGI::should_emit_pass_slot(RendererRD::NextGI::PASS_SLOT_DIFFUSE_GATHER, true));
}

TEST_CASE("[DLSS][TemporalUpscaler][RRGuides] RR settings, reasons, activation rules, and >=32 jitter phases") {
	using namespace TestDLSSCoreSeams;
	using R = RSE::ViewportScaling3DReason;

	// Append-only enum values and ClassDB bindings.
	CHECK_EQ(int(R::VIEWPORT_SCALING_3D_REASON_RR_REQUIRES_NEXTGI_EPIC), 13);
	CHECK_EQ(int(R::VIEWPORT_SCALING_3D_REASON_RR_UNAVAILABLE), 14);
	bool valid = false;
	CHECK_EQ(ClassDB::get_integer_constant("RenderingServer", "VIEWPORT_SCALING_3D_REASON_RR_REQUIRES_NEXTGI_EPIC", &valid), 13);
	CHECK(valid);
	CHECK_EQ(ClassDB::get_integer_constant("RenderingServer", "VIEWPORT_SCALING_3D_REASON_RR_UNAVAILABLE", &valid), 14);
	CHECK(valid);

	// ProjectSettings registration & defaults.
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);
	REQUIRE(ps->has_setting("rendering/scaling_3d/dlss/ray_reconstruction"));
	REQUIRE(ps->has_setting("rendering/scaling_3d/dlss/rr_preset"));
	CHECK_FALSE(bool(ps->property_get_revert("rendering/scaling_3d/dlss/ray_reconstruction")));
	CHECK_EQ(int(ps->property_get_revert("rendering/scaling_3d/dlss/rr_preset")), 0);
	const HashMap<StringName, PropertyInfo> &prop_info = ps->get_custom_property_info();
	REQUIRE(prop_info.has(SNAME("rendering/scaling_3d/dlss/ray_reconstruction")));
	CHECK(prop_info[SNAME("rendering/scaling_3d/dlss/ray_reconstruction")].hint_string.contains("Beta on Vulkan"));
	REQUIRE(prop_info.has(SNAME("rendering/scaling_3d/dlss/rr_preset")));
	CHECK_EQ(prop_info[SNAME("rendering/scaling_3d/dlss/rr_preset")].hint_string, String("Default,D,E,F"));

	// Activation matrix (#206 step 2).
	// Off when not requested.
	{
		const RRActivationResult r = nextgi_resolve_rr_activation(false, true, true, true, false, 1, true);
		CHECK_FALSE(r.active);
		CHECK_EQ(r.reason, R::VIEWPORT_SCALING_3D_REASON_NONE);
	}
	// Active ONLY when requested + DLSS active + NextGI HWRT + explicit Epic + not auto + single view + backend available.
	{
		const RRActivationResult r = nextgi_resolve_rr_activation(true, true, true, true, false, 1, true);
		CHECK(r.active);
		CHECK_EQ(r.reason, R::VIEWPORT_SCALING_3D_REASON_NONE);
	}
	// quality == AUTO NEVER enables RR, even if auto resolved to Epic!
	{
		const RRActivationResult r = nextgi_resolve_rr_activation(true, true, true, true, true, 1, true);
		CHECK_FALSE(r.active);
		CHECK_EQ(r.reason, R::VIEWPORT_SCALING_3D_REASON_RR_REQUIRES_NEXTGI_EPIC);
	}
	// Non-HWRT NextGI (SWRT or disabled) -> RR_REQUIRES_NEXTGI_EPIC.
	{
		const RRActivationResult r = nextgi_resolve_rr_activation(true, true, false, true, false, 1, true);
		CHECK_FALSE(r.active);
		CHECK_EQ(r.reason, R::VIEWPORT_SCALING_3D_REASON_RR_REQUIRES_NEXTGI_EPIC);
	}
	// NextGI HWRT at Medium/High (not Epic) -> RR_REQUIRES_NEXTGI_EPIC.
	{
		const RRActivationResult r = nextgi_resolve_rr_activation(true, true, true, false, false, 1, true);
		CHECK_FALSE(r.active);
		CHECK_EQ(r.reason, R::VIEWPORT_SCALING_3D_REASON_RR_REQUIRES_NEXTGI_EPIC);
	}
	// DLSS not active -> RR_UNAVAILABLE.
	{
		const RRActivationResult r = nextgi_resolve_rr_activation(true, false, true, true, false, 1, true);
		CHECK_FALSE(r.active);
		CHECK_EQ(r.reason, R::VIEWPORT_SCALING_3D_REASON_RR_UNAVAILABLE);
	}
	// Multiview (view_count > 1) -> RR_UNAVAILABLE.
	{
		const RRActivationResult r = nextgi_resolve_rr_activation(true, true, true, true, false, 2, true);
		CHECK_FALSE(r.active);
		CHECK_EQ(r.reason, R::VIEWPORT_SCALING_3D_REASON_RR_UNAVAILABLE);
	}
	// Backend reports RR unavailable -> RR_UNAVAILABLE.
	{
		const RRActivationResult r = nextgi_resolve_rr_activation(true, true, true, true, false, 1, false);
		CHECK_FALSE(r.active);
		CHECK_EQ(r.reason, R::VIEWPORT_SCALING_3D_REASON_RR_UNAVAILABLE);
	}

	// >= 32 jitter phases at every scale when RR is active; untouched when RR is off.
	const float scales[] = { 1.0f, 0.75f, 0.667f, 0.58f, 0.5f, 0.33f };
	for (float scale : scales) {
		const Size2i target(1920, 1080);
		const Size2i render(MAX(int(target.width * scale), 1), MAX(int(target.height * scale), 1));
		const uint32_t base = nextgi_temporal_jitter_phase_count(target, render, true);
		CHECK_EQ(nextgi_rr_jitter_phase_count(base, false), base);
		CHECK_GE(nextgi_rr_jitter_phase_count(base, true), 32u);
	}

	// TemporalUpscaler::Inputs RR fields default to invalid / false.
	TemporalUpscaler::Inputs inputs;
	CHECK_FALSE(inputs.rr_active);
	CHECK_FALSE(inputs.rr_diffuse_albedo.is_valid());
	CHECK_FALSE(inputs.rr_specular_albedo.is_valid());
	CHECK_FALSE(inputs.rr_normal_roughness.is_valid());
	CHECK_FALSE(inputs.rr_specular_hit_distance.is_valid());

	// Viewport status reports rr_active, rr_reason, rr_maturity when DLSS is requested.
	RenderingServer *rs = RenderingServer::get_singleton();
	REQUIRE(rs != nullptr);
	TemporalUpscaler *prev_dlss = TemporalUpscalerRegistry::get(RSE::VIEWPORT_SCALING_3D_MODE_DLSS);
	MockRRTemporalUpscaler mock_dlss;
	mock_dlss.rr_active = true;
	mock_dlss.rr_maturity = "beta";
	mock_dlss.rr_reason = R::VIEWPORT_SCALING_3D_REASON_NONE;
	TemporalUpscalerRegistry::register_upscaler(RSE::VIEWPORT_SCALING_3D_MODE_DLSS, &mock_dlss);

	const RID camera = rs->camera_create();
	const RID viewport = rs->viewport_create();
	rs->viewport_attach_camera(viewport, camera);
	rs->viewport_set_size(viewport, 640, 360, 1);
	rs->viewport_set_active(viewport, true);
	rs->viewport_set_scaling_3d_mode(viewport, RSE::VIEWPORT_SCALING_3D_MODE_DLSS);
	rs->draw(false, 0.0);

	Dictionary status = rs->viewport_get_scaling_3d_status(viewport);
	CHECK(status.has("rr_active"));
	CHECK(status.has("rr_reason"));
	CHECK(status.has("rr_maturity"));
	CHECK(bool(status["rr_active"]));
	CHECK_EQ(int(status["rr_reason"]), int(R::VIEWPORT_SCALING_3D_REASON_NONE));
	CHECK_EQ(String(status["rr_maturity"]), String("beta"));
	CHECK_GE(int64_t(status["jitter_phase_count"]), int64_t(32));

	rs->free_rid(viewport);
	rs->free_rid(camera);
	if (prev_dlss != nullptr) {
		TemporalUpscalerRegistry::register_upscaler(RSE::VIEWPORT_SCALING_3D_MODE_DLSS, prev_dlss);
	} else {
		TemporalUpscalerRegistry::unregister_upscaler(RSE::VIEWPORT_SCALING_3D_MODE_DLSS);
	}
}
#endif

TEST_CASE("[DLSS][LatencyMarkers] Ordering, per-frame INPUT_SAMPLE dedup, and RenderingServer::get_latency_status") {
	using namespace TestDLSSCoreSeams;
	LatencyMarkerScope scope;

	// Null callback is a safe no-op.
	CHECK(get_latency_marker_callback() == nullptr);
	const uint64_t f0 = begin_latency_frame();
	CHECK_EQ(f0, 1u);
	emit_latency_marker(LATENCY_MARKER_SLEEP, f0);
	emit_latency_input_sample_once(f0);
	CHECK(recorded_latency_events.is_empty());

	// Register callback and verify strict ordering + INPUT_SAMPLE dedup within one frame.
	set_latency_marker_callback(&record_latency_marker);
	CHECK(get_latency_marker_callback() == &record_latency_marker);

	const uint64_t f1 = begin_latency_frame();
	CHECK_EQ(f1, 2u);
	emit_latency_marker(LATENCY_MARKER_SLEEP, f1);
	emit_latency_marker(LATENCY_MARKER_SIMULATION_START, f1);
	// Multiple flushes in the same frame emit INPUT_SAMPLE only once.
	emit_latency_input_sample_once(f1);
	emit_latency_input_sample_once(f1);
	emit_latency_input_sample_once(f1);
	emit_latency_marker(LATENCY_MARKER_SIMULATION_END, f1);
	emit_latency_marker(LATENCY_MARKER_RENDER_SUBMIT_START, f1);
	emit_latency_marker(LATENCY_MARKER_RENDER_SUBMIT_END, f1);
	emit_latency_marker(LATENCY_MARKER_PRESENT_START, f1);
	emit_latency_marker(LATENCY_MARKER_PRESENT_END, f1);

	REQUIRE_EQ(recorded_latency_events.size(), 8u);
	const LatencyMarker expected_order[8] = {
		LATENCY_MARKER_SLEEP,
		LATENCY_MARKER_SIMULATION_START,
		LATENCY_MARKER_INPUT_SAMPLE,
		LATENCY_MARKER_SIMULATION_END,
		LATENCY_MARKER_RENDER_SUBMIT_START,
		LATENCY_MARKER_RENDER_SUBMIT_END,
		LATENCY_MARKER_PRESENT_START,
		LATENCY_MARKER_PRESENT_END,
	};
	for (uint32_t i = 0; i < 8u; i++) {
		CHECK_EQ(recorded_latency_events[i].marker, expected_order[i]);
		CHECK_EQ(recorded_latency_events[i].frame_id, f1);
	}

	// Next frame emits INPUT_SAMPLE again once, and RenderingServer::draw emits RENDER_SUBMIT_START..END.
	recorded_latency_events.clear();
	const uint64_t f2 = begin_latency_frame();
	CHECK_EQ(f2, 3u);
	emit_latency_input_sample_once(f2);
	emit_latency_input_sample_once(f2);
	RenderingServer *rs = RenderingServer::get_singleton();
	REQUIRE(rs != nullptr);
	rs->draw(false, 0.0);
	REQUIRE_GE(recorded_latency_events.size(), 3u);
	CHECK_EQ(recorded_latency_events[0].marker, LATENCY_MARKER_INPUT_SAMPLE);
	CHECK_EQ(recorded_latency_events[0].frame_id, f2);
	CHECK_EQ(recorded_latency_events[1].marker, LATENCY_MARKER_RENDER_SUBMIT_START);
	CHECK_EQ(recorded_latency_events[1].frame_id, f2);
	CHECK_EQ(recorded_latency_events[2].marker, LATENCY_MARKER_RENDER_SUBMIT_END);
	CHECK_EQ(recorded_latency_events[2].frame_id, f2);

	// Reflex ProjectSetting and RenderingServer::get_latency_status().
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);
	REQUIRE(ps->has_setting("rendering/latency/reflex_mode"));
	CHECK_EQ(int(ps->property_get_revert("rendering/latency/reflex_mode")), 0);

	const int prev_reflex_mode = int(ps->get_setting("rendering/latency/reflex_mode"));
	RenderingServer::LatencyStatusCallback prev_cb = RenderingServer::get_latency_status_callback();

	RenderingServer::set_latency_status_callback(nullptr);
	ps->set_setting("rendering/latency/reflex_mode", 0);
	Dictionary status = rs->get_latency_status();
	CHECK_EQ(int(status["mode"]), 0);
	CHECK_EQ(String(status["reason"]), String("OFF"));

	// Non-D3D12 (headless dummy in doctests) reports API_UNSUPPORTED when Reflex is requested.
	ps->set_setting("rendering/latency/reflex_mode", 1);
	status = rs->get_latency_status();
	CHECK_EQ(int(status["mode"]), 0);
	CHECK_EQ(String(status["reason"]), String("API_UNSUPPORTED"));

	// Module callback hook overrides default fallback.
	RenderingServer::set_latency_status_callback(&mock_latency_status_callback);
	status = rs->get_latency_status();
	CHECK_EQ(int(status["mode"]), 2);
	CHECK_EQ(String(status["reason"]), String("OK"));

	// Restore settings and callback.
	RenderingServer::set_latency_status_callback(prev_cb);
	ps->set_setting("rendering/latency/reflex_mode", prev_reflex_mode);
}

TEST_CASE("[DLSS][FrameGeneration] New reasons, dynamic_target_fps, and C++ virtual provider override") {
	using namespace TestDLSSCoreSeams;
	using Reason = FrameGenerationExtension::FrameGenerationReason;

	// New reasons and dynamic_target_fps setting.
	CHECK_EQ(int(FrameGenerationExtension::REASON_STREAMLINE_UNAVAILABLE), 8);
	CHECK_EQ(int(FrameGenerationExtension::REASON_DYNAMIC_MFG_UNSUPPORTED), 9);
	CHECK_EQ(int(FrameGenerationExtension::REASON_MAX), 10);
	CHECK_EQ(FrameGenerationExtension::get_reason_token(FrameGenerationExtension::REASON_STREAMLINE_UNAVAILABLE), String("STREAMLINE_UNAVAILABLE"));
	CHECK_EQ(FrameGenerationExtension::get_reason_token(FrameGenerationExtension::REASON_DYNAMIC_MFG_UNSUPPORTED), String("DYNAMIC_MFG_UNSUPPORTED"));

	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);
	REQUIRE(ps->has_setting("rendering/frame_generation/dynamic_target_fps"));
	CHECK_EQ(int(ps->property_get_revert("rendering/frame_generation/dynamic_target_fps")), 0);

	FrameGenerationRegistry::clear();

	Ref<MockCppFrameGenerationProvider> provider;
	provider.instantiate();
	provider->capabilities["max_multiplier"] = 6;
	provider->capabilities["dynamic_supported"] = true;
	provider->capabilities["num_frames_presented"] = int64_t(42);
	provider->capabilities["vram_bytes"] = int64_t(128 * 1024 * 1024);

	FrameGenerationRegistry::register_extension(provider);

	FrameGenerationRegistry::Context ctx;
	ctx.provider = "MockCppFG";
	ctx.multiplier = 4;
	ctx.dynamic = true;
	ctx.dynamic_target_fps = 144;
	ctx.editor = false;
	ctx.xr_main_window = false;
	ctx.rendering_device = true;

	FrameGenerationRegistry::update(ctx);
	CHECK(FrameGenerationRegistry::is_active());
	CHECK_EQ(FrameGenerationRegistry::get_reason(), Reason::REASON_OK);
	CHECK_EQ(FrameGenerationRegistry::get_active_multiplier(), 4);
	CHECK_EQ(provider->set_mode_calls, 1);
	CHECK_EQ(int(provider->last_mode["multiplier"]), 4);
	CHECK(bool(provider->last_mode["dynamic"]));
	CHECK_EQ(int(provider->last_mode["dynamic_target_fps"]), 144);

	Dictionary status = FrameGenerationRegistry::get_status();
	CHECK_EQ(int(status["dynamic_target_fps"]), 144);
	CHECK_EQ(int(status["max_multiplier"]), 6);
	CHECK(bool(status["dynamic_supported"]));
	CHECK_EQ(int64_t(status["num_frames_presented"]), int64_t(42));
	CHECK_EQ(int64_t(status["vram_bytes"]), int64_t(128 * 1024 * 1024));

	// Changing dynamic_target_fps re-sends set_mode.
	ctx.dynamic_target_fps = 240;
	FrameGenerationRegistry::update(ctx);
	CHECK_EQ(provider->set_mode_calls, 2);
	CHECK_EQ(int(provider->last_mode["dynamic_target_fps"]), 240);

	// Dispatch inputs and present go straight to C++ virtual overrides.
	FrameGenerationRegistry::dispatch_frame_inputs();
	FrameGenerationRegistry::dispatch_present(0x1234, 7, true);
	FrameGenerationRegistry::dispatch_present(0x1234, 7, false);
	CHECK_EQ(provider->reset_calls, 1);
	CHECK_EQ(provider->frame_inputs_calls, 1);
	CHECK_EQ(provider->present_calls, 2);

	// REASON_DYNAMIC_MFG_UNSUPPORTED keeps provider active at fixed multiplier and logs once.
	{
		WarningCounter warn;
		provider->availability = FrameGenerationExtension::REASON_DYNAMIC_MFG_UNSUPPORTED;
		provider->capabilities["dynamic_supported"] = false;
		FrameGenerationRegistry::register_extension(provider);
		ERR_PRINT_OFF;
		FrameGenerationRegistry::update(ctx);
		FrameGenerationRegistry::update(ctx);
		ERR_PRINT_ON;
		CHECK(FrameGenerationRegistry::is_active());
		CHECK_EQ(FrameGenerationRegistry::get_reason(), FrameGenerationExtension::REASON_DYNAMIC_MFG_UNSUPPORTED);
		CHECK_EQ(FrameGenerationRegistry::get_active_multiplier(), 4);
		CHECK_EQ(warn.warnings, 1);
		CHECK_EQ(warn.lines_containing("DYNAMIC_MFG_UNSUPPORTED"), 1);
		status = FrameGenerationRegistry::get_status();
		CHECK_FALSE(bool(status["dynamic_supported"]));
	}

	// REASON_STREAMLINE_UNAVAILABLE turns provider off and logs once.
	{
		WarningCounter warn;
		provider->availability = FrameGenerationExtension::REASON_STREAMLINE_UNAVAILABLE;
		FrameGenerationRegistry::register_extension(provider);
		ERR_PRINT_OFF;
		FrameGenerationRegistry::update(ctx);
		FrameGenerationRegistry::update(ctx);
		ERR_PRINT_ON;
		CHECK_FALSE(FrameGenerationRegistry::is_active());
		CHECK_EQ(FrameGenerationRegistry::get_reason(), FrameGenerationExtension::REASON_STREAMLINE_UNAVAILABLE);
		CHECK_EQ(FrameGenerationRegistry::get_active_multiplier(), 1);
		CHECK_EQ(warn.warnings, 1);
		CHECK_EQ(warn.lines_containing("STREAMLINE_UNAVAILABLE"), 1);
	}

	FrameGenerationRegistry::clear();
}
