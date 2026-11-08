/**************************************************************************/
/*  test_upscalers.cpp                                                    */
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

TEST_FORCE_LINK(test_upscalers)

#include "core/config/project_settings.h"
#include "scene/main/viewport.h"
#include "servers/rendering/dynamic_resolution.h"
#include "servers/rendering/renderer_rd/effects/temporal_upscaler.h"
#include "servers/rendering/renderer_rd/effects/tsr.h"
#include "servers/rendering/renderer_rd/effects/tsr_presets.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgi_velocity.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_enums.h"
#include "servers/rendering/storage/render_scene_buffers.h"

namespace {

class MockUpscaler : public TemporalUpscaler {
public:
	RSE::ViewportScaling3DReason reason = RSE::VIEWPORT_SCALING_3D_REASON_NONE;
	float clamped_scale = -1.0f;
	float bias = -1.0f;
	bool multiview = false;
	HashMap<RenderSceneBuffersRD *, uint32_t> contexts;
	Inputs last_inputs;
	uint32_t processed_views = 0;

	String get_display_name() const override { return "Mock DLSS"; }
	const Span<char> get_label() const override { return "Mock Temporal Upscale"; }
	float mip_bias_offset() const override { return bias; }
	bool supports_multiview() const override { return multiview; }
	RSE::ViewportScaling3DReason ensure_context(RenderSceneBuffersRD *p_render_buffers, uint32_t p_view) override {
		if (p_view > 0 && !supports_multiview()) {
			return RSE::VIEWPORT_SCALING_3D_REASON_VIEW_UNSUPPORTED;
		}
		contexts[p_render_buffers] |= 1u << p_view;
		return RSE::VIEWPORT_SCALING_3D_REASON_NONE;
	}
	void process(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Inputs &p_inputs) override {
		(void)p_render_buffers;
		(void)p_view;
		last_inputs = p_inputs;
		processed_views++;
	}
	void free_contexts(RenderSceneBuffersRD *p_render_buffers) override { contexts.erase(p_render_buffers); }
	RSE::ViewportScaling3DReason get_availability(uint32_t p_view_count, float p_scale, float &r_effective_scale) const override {
		(void)p_view_count;
		r_effective_scale = clamped_scale > 0.0f ? clamped_scale : p_scale;
		return reason;
	}
};

} // namespace

TEST_CASE("[Upscalers] DLSS enum values and type") {
	// GodotNext: pu_dlss_scaling_mode (#103), pt_tsr_mode_presets (#197), GU-1.
	CHECK_EQ(int(RSE::VIEWPORT_SCALING_3D_MODE_NEAREST), 5);
	CHECK_EQ(int(RSE::VIEWPORT_SCALING_3D_MODE_DLSS), 6);
	CHECK_EQ(int(RSE::VIEWPORT_SCALING_3D_MODE_MAX), 9);
	CHECK_EQ(int(Viewport::SCALING_3D_MODE_DLSS), 6);
	CHECK_EQ(int(Viewport::SCALING_3D_MODE_MAX), 9);
	CHECK(RSE::scaling_3d_mode_type(RSE::VIEWPORT_SCALING_3D_MODE_DLSS) == RSE::VIEWPORT_SCALING_3D_TYPE_TEMPORAL);
}

TEST_CASE("[Upscalers] Registry and mode hint") {
	MockUpscaler mock;
	CHECK(TemporalUpscalerRegistry::get(RSE::VIEWPORT_SCALING_3D_MODE_DLSS) == nullptr);
	const String without = TemporalUpscalerRegistry::build_mode_hint(true);
	CHECK_FALSE(without.contains("DLSS"));
	CHECK(without.contains("MetalFX (Temporal - Slow):4"));

	TemporalUpscalerRegistry::register_upscaler(RSE::VIEWPORT_SCALING_3D_MODE_DLSS, &mock);
	CHECK(TemporalUpscalerRegistry::get(RSE::VIEWPORT_SCALING_3D_MODE_DLSS) == &mock);
	const String with = TemporalUpscalerRegistry::build_mode_hint(true);
	CHECK(with.contains("DLSS (Slow):6"));
	CHECK(TemporalUpscalerRegistry::build_mode_hint(false).contains("DLSS"));
	CHECK_FALSE(TemporalUpscalerRegistry::build_mode_hint(false).contains("MetalFX"));

	TemporalUpscalerRegistry::unregister_upscaler(RSE::VIEWPORT_SCALING_3D_MODE_DLSS);
	CHECK(TemporalUpscalerRegistry::get(RSE::VIEWPORT_SCALING_3D_MODE_DLSS) == nullptr);
	CHECK_FALSE(TemporalUpscalerRegistry::build_mode_hint(true).contains("DLSS"));
}

TEST_CASE("[Upscalers] Viewport status exposes the requested mode") {
	RenderingServer *rs = RenderingServer::get_singleton();
	REQUIRE(rs != nullptr);
	TemporalUpscalerRegistry::clear();
	const RID camera = rs->camera_create();
	const RID viewport = rs->viewport_create();
	rs->viewport_attach_camera(viewport, camera);
	rs->viewport_set_size(viewport, 640, 360, 1);
	rs->viewport_set_active(viewport, true);
	rs->viewport_set_scaling_3d_mode(viewport, RSE::VIEWPORT_SCALING_3D_MODE_DLSS);
	rs->draw(false, 0.0);
	const Dictionary status = rs->viewport_get_scaling_3d_status(viewport);
	CHECK_EQ(int(status["requested_mode"]), int(RSE::VIEWPORT_SCALING_3D_MODE_DLSS));
	CHECK(status.has("active_mode"));
	CHECK(status.has("reason"));
	CHECK(status.has("effective_scale"));
	CHECK_EQ(int(status["preset"]), 0);
	rs->free_rid(viewport);
	rs->free_rid(camera);
}

TEST_CASE("[Upscalers] Reason precedence") {
	// GodotNext: pu_dlss_scaling_mode (#103), §3.2 order.
	using R = RSE::ViewportScaling3DReason;
	CHECK_EQ(nextgi_resolve_dlss_reason(false, true, R::VIEWPORT_SCALING_3D_REASON_NONE, 1.0f, 1, 1920, 1080), R::VIEWPORT_SCALING_3D_REASON_API_UNSUPPORTED);
	CHECK_EQ(nextgi_resolve_dlss_reason(true, false, R::VIEWPORT_SCALING_3D_REASON_NONE, 1.0f, 1, 1920, 1080), R::VIEWPORT_SCALING_3D_REASON_MODULE_DISABLED);
	CHECK_EQ(nextgi_resolve_dlss_reason(true, true, R::VIEWPORT_SCALING_3D_REASON_NOT_NVIDIA_RTX, 0.25f, 1, 1920, 1080), R::VIEWPORT_SCALING_3D_REASON_NOT_NVIDIA_RTX);
	CHECK_EQ(nextgi_resolve_dlss_reason(true, true, R::VIEWPORT_SCALING_3D_REASON_NONE, 0.25f, 1, 1920, 1080), R::VIEWPORT_SCALING_3D_REASON_SCALE_UNSUPPORTED);
	CHECK_EQ(nextgi_resolve_dlss_reason(true, true, R::VIEWPORT_SCALING_3D_REASON_NONE, 0.25f, 2, 1920, 1080), R::VIEWPORT_SCALING_3D_REASON_SCALE_UNSUPPORTED);
	CHECK_EQ(nextgi_resolve_dlss_reason(true, true, R::VIEWPORT_SCALING_3D_REASON_NONE, 0.75f, 2, 1920, 1080), R::VIEWPORT_SCALING_3D_REASON_VIEW_UNSUPPORTED);
	CHECK_EQ(nextgi_resolve_dlss_reason(true, true, R::VIEWPORT_SCALING_3D_REASON_NONE, 0.75f, 1, 16, 16), R::VIEWPORT_SCALING_3D_REASON_SCALE_UNSUPPORTED);
	CHECK_EQ(nextgi_resolve_dlss_reason(true, true, R::VIEWPORT_SCALING_3D_REASON_NONE, 0.75f, 1, 1920, 1080), R::VIEWPORT_SCALING_3D_REASON_NONE);
}

TEST_CASE("[Upscalers] Mock availability and server query") {
	MockUpscaler mock;
	mock.reason = RSE::VIEWPORT_SCALING_3D_REASON_RUNTIME_MISSING;
	mock.clamped_scale = 0.5f;
	TemporalUpscalerRegistry::register_upscaler(RSE::VIEWPORT_SCALING_3D_MODE_DLSS, &mock);
	float scale = 1.0f;
	CHECK_EQ(mock.get_availability(1, 1.0f, scale), RSE::VIEWPORT_SCALING_3D_REASON_RUNTIME_MISSING);
	CHECK_EQ(scale, 0.5f);
	// Non-DLSS modes are always available; DLSS has no GPU path in this test run.
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs != nullptr) {
		CHECK_EQ(rs->scaling_3d_mode_get_availability(RSE::VIEWPORT_SCALING_3D_MODE_BILINEAR), RSE::VIEWPORT_SCALING_3D_REASON_NONE);
		CHECK_EQ(rs->scaling_3d_mode_get_availability(RSE::VIEWPORT_SCALING_3D_MODE_DLSS), RSE::VIEWPORT_SCALING_3D_REASON_API_UNSUPPORTED);
	}
	TemporalUpscalerRegistry::unregister_upscaler(RSE::VIEWPORT_SCALING_3D_MODE_DLSS);
}

TEST_CASE("[Upscalers] Per-view contexts and dispatch inputs") {
	MockUpscaler mock;
	RenderSceneBuffersRD *first = reinterpret_cast<RenderSceneBuffersRD *>(uintptr_t(1));
	RenderSceneBuffersRD *second = reinterpret_cast<RenderSceneBuffersRD *>(uintptr_t(2));
	CHECK_EQ(mock.ensure_context(first, 0), RSE::VIEWPORT_SCALING_3D_REASON_NONE);
	CHECK_EQ(mock.ensure_context(second, 0), RSE::VIEWPORT_SCALING_3D_REASON_NONE);
	CHECK_EQ(mock.contexts.size(), 2);
	CHECK_EQ(mock.ensure_context(first, 1), RSE::VIEWPORT_SCALING_3D_REASON_VIEW_UNSUPPORTED);
	mock.free_contexts(first);
	CHECK_EQ(mock.contexts.size(), 1);
	mock.multiview = true;
	CHECK_EQ(mock.ensure_context(first, 0), RSE::VIEWPORT_SCALING_3D_REASON_NONE);
	CHECK_EQ(mock.ensure_context(first, 1), RSE::VIEWPORT_SCALING_3D_REASON_NONE);
	CHECK_EQ(mock.contexts[first], 3);
	CHECK_EQ(nextgi_resolve_dlss_reason(true, true, RSE::VIEWPORT_SCALING_3D_REASON_NONE, 0.75f, 2, 1920, 1080, true), RSE::VIEWPORT_SCALING_3D_REASON_NONE);
	mock.free_contexts(first);
	mock.free_contexts(second);

	TemporalUpscaler::Inputs inputs;
	inputs.internal_size = Size2i(1280, 720);
	inputs.motion_vector_scale = inputs.internal_size;
	inputs.jitter = Vector2(0.25f, -0.5f) * Vector2(inputs.internal_size) * 0.5f;
	inputs.reset = true;
	mock.process(Ref<RenderSceneBuffersRD>(), 0, inputs);
	CHECK_EQ(mock.processed_views, 1);
	CHECK(mock.last_inputs.motion_vector_scale == inputs.internal_size);
	CHECK(mock.last_inputs.jitter.is_equal_approx(Vector2(160, -180)));
	CHECK(mock.last_inputs.reset);
}

TEST_CASE("[Upscalers] Temporal mip bias and jitter phases") {
	const float scales[] = { 1.0f, 0.667f, 0.58f, 0.5f, 0.333f };
	for (float scale : scales) {
		CHECK(Math::is_equal_approx(nextgi_temporal_mip_bias(scale, 0.125f, -1.0f), Math::log2(scale) + 0.125f - 1.0f, 1e-6f));
		CHECK(Math::is_equal_approx(nextgi_temporal_mip_bias(scale, 0.0f, -0.5f), Math::log2(scale) - 0.5f, 1e-6f));
	}
	for (const Size2i &target : { Size2i(2560, 1440), Size2i(3840, 2160) }) {
		const float scales[] = { 1.0f, 2.0f / 3.0f, 0.58f, 0.5f, 1.0f / 3.0f };
		const uint32_t expected[] = { 8, 18, 24, 32, 72 };
		for (int i = 0; i < 5; i++) {
			Size2i render = MockUpscaler().get_render_size(target, scales[i]);
			CHECK_EQ(nextgi_temporal_jitter_phase_count(target, render, true), expected[i]);
		}
	}
	CHECK_EQ(nextgi_temporal_jitter_phase_count(Size2i(1920, 1080), Size2i(1120, 630), false), 23);
}

TEST_CASE("[Upscalers] Temporal history reset is one-shot") {
	TemporalHistoryReset reset;
	CHECK_FALSE(reset.consume());
	reset.request();
	reset.request();
	CHECK(reset.consume());
	CHECK_FALSE(reset.consume());
	reset.request();
	CHECK(reset.consume());
	CHECK_FALSE(reset.consume());
}

TEST_CASE("[Upscalers][TSR] Enum values, types, and mode hints") {
	// GodotNext: pt_tsr_mode_presets (#197), GT-1 subset.
	CHECK_EQ(int(RSE::VIEWPORT_SCALING_3D_MODE_BILINEAR), 0);
	CHECK_EQ(int(RSE::VIEWPORT_SCALING_3D_MODE_FSR), 1);
	CHECK_EQ(int(RSE::VIEWPORT_SCALING_3D_MODE_FSR2), 2);
	CHECK_EQ(int(RSE::VIEWPORT_SCALING_3D_MODE_METALFX_SPATIAL), 3);
	CHECK_EQ(int(RSE::VIEWPORT_SCALING_3D_MODE_METALFX_TEMPORAL), 4);
	CHECK_EQ(int(RSE::VIEWPORT_SCALING_3D_MODE_NEAREST), 5);
	CHECK_EQ(int(RSE::VIEWPORT_SCALING_3D_MODE_DLSS), 6);
	CHECK_EQ(int(RSE::VIEWPORT_SCALING_3D_MODE_TSR), 7);
	CHECK_EQ(int(RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM), 8);
	CHECK_EQ(int(RSE::VIEWPORT_SCALING_3D_MODE_MAX), 9);

	CHECK_EQ(int(Viewport::SCALING_3D_MODE_BILINEAR), 0);
	CHECK_EQ(int(Viewport::SCALING_3D_MODE_FSR), 1);
	CHECK_EQ(int(Viewport::SCALING_3D_MODE_FSR2), 2);
	CHECK_EQ(int(Viewport::SCALING_3D_MODE_METALFX_SPATIAL), 3);
	CHECK_EQ(int(Viewport::SCALING_3D_MODE_METALFX_TEMPORAL), 4);
	CHECK_EQ(int(Viewport::SCALING_3D_MODE_NEAREST), 5);
	CHECK_EQ(int(Viewport::SCALING_3D_MODE_DLSS), 6);
	CHECK_EQ(int(Viewport::SCALING_3D_MODE_TSR), 7);
	CHECK_EQ(int(Viewport::SCALING_3D_MODE_CUSTOM), 8);
	CHECK_EQ(int(Viewport::SCALING_3D_MODE_MAX), 9);

	CHECK_EQ(RSE::scaling_3d_mode_type(RSE::VIEWPORT_SCALING_3D_MODE_TSR), RSE::VIEWPORT_SCALING_3D_TYPE_TEMPORAL);
	CHECK_EQ(RSE::scaling_3d_mode_type(RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM), RSE::VIEWPORT_SCALING_3D_TYPE_TEMPORAL);

	CHECK_EQ(int(RSE::VIEWPORT_TSR_QUALITY_LOW), 0);
	CHECK_EQ(int(RSE::VIEWPORT_TSR_QUALITY_MEDIUM), 1);
	CHECK_EQ(int(RSE::VIEWPORT_TSR_QUALITY_HIGH), 2);
	CHECK_EQ(int(RSE::VIEWPORT_TSR_QUALITY_EPIC), 3);
	CHECK_EQ(int(RSE::VIEWPORT_TSR_QUALITY_CINEMATIC), 4);
	CHECK_EQ(int(RSE::VIEWPORT_TSR_QUALITY_MAX), 5);

	CHECK_EQ(int(Viewport::TSR_QUALITY_LOW), 0);
	CHECK_EQ(int(Viewport::TSR_QUALITY_MEDIUM), 1);
	CHECK_EQ(int(Viewport::TSR_QUALITY_HIGH), 2);
	CHECK_EQ(int(Viewport::TSR_QUALITY_EPIC), 3);
	CHECK_EQ(int(Viewport::TSR_QUALITY_CINEMATIC), 4);
	CHECK_EQ(int(Viewport::TSR_QUALITY_MAX), 5);

	TemporalUpscalerRegistry::clear();
	const String non_metal_hint = TemporalUpscalerRegistry::build_mode_hint(false);
	const String metal_hint = TemporalUpscalerRegistry::build_mode_hint(true);
	CHECK(non_metal_hint.contains("TSR:7"));
	CHECK(non_metal_hint.contains("Custom:8"));
	CHECK(metal_hint.contains("TSR:7"));
	CHECK(metal_hint.contains("Custom:8"));
}

TEST_CASE("[Upscalers][TSR] Preset table and project settings") {
	// Low: 1.0, off, off, off, off.
	CHECK_EQ(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_LOW].history_scale, 1.0f);
	CHECK_FALSE(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_LOW].resurrection);
	CHECK_FALSE(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_LOW].reprojection_field);
	CHECK_FALSE(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_LOW].flicker_analysis);
	CHECK_FALSE(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_LOW].rejection_aa);

	// Medium: 1.0, on, off, off, on.
	CHECK_EQ(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_MEDIUM].history_scale, 1.0f);
	CHECK(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_MEDIUM].resurrection);
	CHECK_FALSE(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_MEDIUM].reprojection_field);
	CHECK_FALSE(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_MEDIUM].flicker_analysis);
	CHECK(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_MEDIUM].rejection_aa);

	// High: 1.0, on, on, on, on.
	CHECK_EQ(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_HIGH].history_scale, 1.0f);
	CHECK(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_HIGH].resurrection);
	CHECK(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_HIGH].reprojection_field);
	CHECK(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_HIGH].flicker_analysis);
	CHECK(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_HIGH].rejection_aa);

	// Epic: 2.0, on, on, on, on.
	CHECK_EQ(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_EPIC].history_scale, 2.0f);
	CHECK(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_EPIC].resurrection);
	CHECK(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_EPIC].reprojection_field);
	CHECK(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_EPIC].flicker_analysis);
	CHECK(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_EPIC].rejection_aa);

	// Cinematic: 2.0, on, on, on, on.
	CHECK_EQ(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_CINEMATIC].history_scale, 2.0f);
	CHECK(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_CINEMATIC].resurrection);
	CHECK(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_CINEMATIC].reprojection_field);
	CHECK(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_CINEMATIC].flicker_analysis);
	CHECK(TSR_PRESETS[RSE::VIEWPORT_TSR_QUALITY_CINEMATIC].rejection_aa);

	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);
	CHECK(ps->has_setting("rendering/anti_aliasing/tsr/quality"));
	CHECK_EQ(int(ps->get_setting("rendering/anti_aliasing/tsr/quality")), int(RSE::VIEWPORT_TSR_QUALITY_EPIC));
	CHECK(ps->has_setting("rendering/anti_aliasing/tsr/thin_geometry_detection"));
	CHECK_FALSE(bool(ps->get_setting("rendering/anti_aliasing/tsr/thin_geometry_detection")));
}

TEST_CASE("[Upscalers][TSR] Scale clamp, renderer fallbacks, MSAA and TAA rules") {
	// Scale 0.2 -> 0.25, 2.5 -> 2.0, 1.5 stays TSR (supersampling).
	const TSRScalingResolution low_scale = nextgi_resolve_tsr_scaling(
			RSE::VIEWPORT_SCALING_3D_MODE_TSR, 0.2f, "forward_plus", true, false, RSE::VIEWPORT_MSAA_DISABLED);
	CHECK_EQ(low_scale.mode, RSE::VIEWPORT_SCALING_3D_MODE_TSR);
	CHECK(Math::is_equal_approx(low_scale.scale, 0.25f));
	CHECK(low_scale.scale_clamped);

	const TSRScalingResolution high_scale = nextgi_resolve_tsr_scaling(
			RSE::VIEWPORT_SCALING_3D_MODE_TSR, 2.5f, "forward_plus", true, false, RSE::VIEWPORT_MSAA_DISABLED);
	CHECK_EQ(high_scale.mode, RSE::VIEWPORT_SCALING_3D_MODE_TSR);
	CHECK(Math::is_equal_approx(high_scale.scale, 2.0f));
	CHECK(high_scale.scale_clamped);

	const TSRScalingResolution supersample = nextgi_resolve_tsr_scaling(
			RSE::VIEWPORT_SCALING_3D_MODE_TSR, 1.5f, "forward_plus", true, false, RSE::VIEWPORT_MSAA_DISABLED);
	CHECK_EQ(supersample.mode, RSE::VIEWPORT_SCALING_3D_MODE_TSR);
	CHECK(Math::is_equal_approx(supersample.scale, 1.5f));
	CHECK_FALSE(supersample.scale_clamped);

	const TSRScalingResolution min_edge = nextgi_resolve_tsr_scaling(
			RSE::VIEWPORT_SCALING_3D_MODE_TSR, 0.25f, "forward_plus", true, false, RSE::VIEWPORT_MSAA_DISABLED);
	CHECK_EQ(min_edge.mode, RSE::VIEWPORT_SCALING_3D_MODE_TSR);
	CHECK(Math::is_equal_approx(min_edge.scale, 0.25f));
	CHECK_FALSE(min_edge.scale_clamped);

	const TSRScalingResolution max_edge = nextgi_resolve_tsr_scaling(
			RSE::VIEWPORT_SCALING_3D_MODE_TSR, 2.0f, "forward_plus", true, false, RSE::VIEWPORT_MSAA_DISABLED);
	CHECK_EQ(max_edge.mode, RSE::VIEWPORT_SCALING_3D_MODE_TSR);
	CHECK(Math::is_equal_approx(max_edge.scale, 2.0f));
	CHECK_FALSE(max_edge.scale_clamped);

	// Non-TSR modes still fall back to bilinear when scale > 1.0.
	const TSRScalingResolution fsr2_downsample = nextgi_resolve_tsr_scaling(
			RSE::VIEWPORT_SCALING_3D_MODE_FSR2, 1.5f, "forward_plus", true, false, RSE::VIEWPORT_MSAA_DISABLED);
	CHECK_EQ(fsr2_downsample.mode, RSE::VIEWPORT_SCALING_3D_MODE_BILINEAR);

	// CUSTOM stays CUSTOM on Forward+ when scale <= 1.0 and upscalers are available.
	const TSRScalingResolution custom_forward = nextgi_resolve_tsr_scaling(
			RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM, 0.75f, "forward_plus", true, false, RSE::VIEWPORT_MSAA_DISABLED);
	CHECK_EQ(custom_forward.mode, RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM);

	// Mobile and Compatibility resolve TSR and CUSTOM to bilinear.
	for (RSE::ViewportScaling3DMode mode : { RSE::VIEWPORT_SCALING_3D_MODE_TSR, RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM }) {
		const TSRScalingResolution mobile = nextgi_resolve_tsr_scaling(
				mode, 0.75f, "mobile", true, false, RSE::VIEWPORT_MSAA_DISABLED);
		CHECK_EQ(mobile.mode, RSE::VIEWPORT_SCALING_3D_MODE_BILINEAR);

		const TSRScalingResolution compat = nextgi_resolve_tsr_scaling(
				mode, 0.75f, "gl_compatibility", false, false, RSE::VIEWPORT_MSAA_DISABLED);
		CHECK_EQ(compat.mode, RSE::VIEWPORT_SCALING_3D_MODE_BILINEAR);

		const TSRScalingResolution unavailable = nextgi_resolve_tsr_scaling(
				mode, 0.75f, "forward_plus", false, false, RSE::VIEWPORT_MSAA_DISABLED);
		CHECK_EQ(unavailable.mode, RSE::VIEWPORT_SCALING_3D_MODE_BILINEAR);
	}

	// use_taa is forced off and 3D MSAA is disabled internally under TSR.
	const TSRScalingResolution taa_and_msaa = nextgi_resolve_tsr_scaling(
			RSE::VIEWPORT_SCALING_3D_MODE_TSR, 0.75f, "forward_plus", true, true, RSE::VIEWPORT_MSAA_4X);
	CHECK_EQ(taa_and_msaa.mode, RSE::VIEWPORT_SCALING_3D_MODE_TSR);
	CHECK_FALSE(taa_and_msaa.use_taa);
	CHECK(taa_and_msaa.taa_disabled);
	CHECK_EQ(taa_and_msaa.msaa_3d, RSE::VIEWPORT_MSAA_DISABLED);
	CHECK(taa_and_msaa.msaa_disabled);
}

TEST_CASE("[SceneTree][Upscalers][TSR] Viewport and RenderSceneBuffersConfiguration plumbing") {
	Ref<RenderSceneBuffersConfiguration> config;
	config.instantiate();
	CHECK_EQ(config->get_tsr_quality(), RSE::VIEWPORT_TSR_QUALITY_EPIC);
	for (int q = 0; q < RSE::VIEWPORT_TSR_QUALITY_MAX; q++) {
		config->set("tsr_quality", q);
		CHECK_EQ(int(config->get("tsr_quality")), q);
		CHECK_EQ(int(config->get_tsr_quality()), q);
	}

	SubViewport *vp = memnew(SubViewport);
	CHECK_EQ(vp->get_tsr_quality(), Viewport::TSR_QUALITY_EPIC);
	vp->set_scaling_3d_mode(Viewport::SCALING_3D_MODE_TSR);
	CHECK_EQ(vp->get_scaling_3d_mode(), Viewport::SCALING_3D_MODE_TSR);
	vp->set_tsr_quality(Viewport::TSR_QUALITY_HIGH);
	CHECK_EQ(vp->get_tsr_quality(), Viewport::TSR_QUALITY_HIGH);

	RenderingServer *rs = RenderingServer::get_singleton();
	REQUIRE(rs != nullptr);
	Dictionary status = rs->viewport_get_scaling_3d_status(vp->get_viewport_rid());
	CHECK_EQ(int(status["requested_mode"]), int(RSE::VIEWPORT_SCALING_3D_MODE_TSR));
	CHECK_EQ(int(status["preset"]), int(RSE::VIEWPORT_TSR_QUALITY_HIGH));

	rs->viewport_set_tsr_quality(vp->get_viewport_rid(), RSE::VIEWPORT_TSR_QUALITY_CINEMATIC);
	status = rs->viewport_get_scaling_3d_status(vp->get_viewport_rid());
	CHECK_EQ(int(status["preset"]), int(RSE::VIEWPORT_TSR_QUALITY_CINEMATIC));

	vp->set_scaling_3d_mode(Viewport::SCALING_3D_MODE_CUSTOM);
	CHECK_EQ(vp->get_scaling_3d_mode(), Viewport::SCALING_3D_MODE_CUSTOM);

	memdelete(vp);
}

TEST_CASE("[Upscalers][TSR] Jitter phase count at scales 0.5, 0.667, 1.0 and 2.0 and mip bias") {
	const Size2i target_2160p(3840, 2160);
	const Size2i target_1440p(2560, 1440);
	const Size2i target_1080p(1920, 1080);

	// Scale 0.5 -> round(8 * 2.0^2) = 32.
	CHECK_EQ(RendererRD::TSR::jitter_phase_count(target_2160p, Size2i(1920, 1080)), 32u);
	CHECK_EQ(RendererRD::TSR::jitter_phase_count(target_1440p, Size2i(1280, 720)), 32u);

	// Scale 0.667 -> round(8 * (1 / 0.667)^2) = 18.
	CHECK_EQ(RendererRD::TSR::jitter_phase_count(target_2160p, Size2i(int(3840 * 0.667f), int(2160 * 0.667f))), 18u);
	CHECK_EQ(RendererRD::TSR::jitter_phase_count(target_1440p, Size2i(int(2560 * 0.667f), int(1440 * 0.667f))), 18u);
	CHECK_EQ(RendererRD::TSR::jitter_phase_count(target_1080p, Size2i(int(1920 * 0.667f), int(1080 * 0.667f))), 18u);

	// Scale 1.0 -> round(8 * 1.0^2) = 8.
	CHECK_EQ(RendererRD::TSR::jitter_phase_count(target_2160p, target_2160p), 8u);

	// Scale 2.0 (supersampling) -> round(8 * 0.5^2) = 2, clamped to minimum 8.
	CHECK_EQ(RendererRD::TSR::jitter_phase_count(target_2160p, Size2i(7680, 4320)), 8u);
	CHECK_EQ(RendererRD::TSR::jitter_phase_count(target_1080p, Size2i(3840, 2160)), 8u);

	// Provisional mip bias offset must come from {-1.0, -0.5, 0.0}.
	const float bias = RendererRD::TSR::mip_bias_offset();
	CHECK((bias == -1.0f || bias == -0.5f || bias == 0.0f));
	CHECK(Math::is_equal_approx(nextgi_temporal_mip_bias(0.5f, 0.0f, bias), -1.0f + bias));
	CHECK(Math::is_equal_approx(nextgi_temporal_mip_bias(2.0f, 0.0f, bias), bias));
}

TEST_CASE("[Upscalers][TSR] One-shot history reset on creation, explicit reset, and history resize") {
	RendererRD::TSRContext ctx(Size2i(1280, 720), Size2i(2560, 1440), RSE::VIEWPORT_TSR_QUALITY_HIGH);
	CHECK_EQ(ctx.get_history_size(), Size2i(2560, 1440));

	// First frame after creation/configure clears history once.
	CHECK(ctx.consume_reset());
	CHECK_FALSE(ctx.consume_reset());

	// Explicit viewport_reset_temporal_history clears history once.
	ctx.request_reset();
	CHECK(ctx.consume_reset());
	CHECK_FALSE(ctx.consume_reset());

	// Internal-only resolution change preserves 100%/200% history without forcing a reset.
	CHECK_FALSE(ctx.sync_dimensions(Size2i(1600, 900), Size2i(2560, 1440), RSE::VIEWPORT_TSR_QUALITY_HIGH));
	CHECK_EQ(ctx.get_internal_size(), Size2i(1600, 900));
	CHECK_FALSE(ctx.consume_reset());

	// Switching from High (100% history) to Epic (200% history) resizes history and resets once.
	CHECK(ctx.sync_dimensions(Size2i(1600, 900), Size2i(2560, 1440), RSE::VIEWPORT_TSR_QUALITY_EPIC));
	CHECK_EQ(ctx.get_history_size(), Size2i(5120, 2880));
	CHECK(ctx.consume_reset());
	CHECK_FALSE(ctx.consume_reset());

	// Output target resize also resets history once.
	CHECK(ctx.sync_dimensions(Size2i(1920, 1080), Size2i(3840, 2160), RSE::VIEWPORT_TSR_QUALITY_EPIC));
	CHECK_EQ(ctx.get_history_size(), Size2i(7680, 4320));
	CHECK(ctx.consume_reset());
	CHECK_FALSE(ctx.consume_reset());
}

TEST_CASE("[Upscalers][TSR] Two viewports allocate two distinct TSRContexts and free cleanly") {
	const uint32_t base_live = RendererRD::TSRContext::get_live_context_count();

	Ref<RendererSceneRenderImplementation::RenderForwardClustered::RenderBufferDataForwardClustered> vp_a;
	Ref<RendererSceneRenderImplementation::RenderForwardClustered::RenderBufferDataForwardClustered> vp_b;
	vp_a.instantiate();
	vp_b.instantiate();

	RendererRD::TSRContext *ctx_a = vp_a->ensure_tsr_context(0, Size2i(960, 540), Size2i(1920, 1080), RSE::VIEWPORT_TSR_QUALITY_HIGH);
	RendererRD::TSRContext *ctx_b = vp_b->ensure_tsr_context(0, Size2i(1280, 720), Size2i(2560, 1440), RSE::VIEWPORT_TSR_QUALITY_EPIC);

	REQUIRE(ctx_a != nullptr);
	REQUIRE(ctx_b != nullptr);
	CHECK(ctx_a != ctx_b);
	CHECK_EQ(vp_a->get_tsr_context(0), ctx_a);
	CHECK_EQ(vp_b->get_tsr_context(0), ctx_b);
	CHECK_EQ(RendererRD::TSRContext::get_live_context_count(), base_live + 2u);

	// Reset and ping-pong parity state are isolated per viewport context.
	CHECK(ctx_a->consume_reset());
	CHECK_FALSE(ctx_a->consume_reset());
	CHECK(ctx_b->consume_reset());
	ctx_a->advance_frame();
	CHECK_EQ(ctx_a->get_read_index(), 1u);
	CHECK_EQ(ctx_b->get_read_index(), 0u);

	// Stereo view 1 on viewport A gets its own per-(viewport, view) TSRContext.
	RendererRD::TSRContext *ctx_a_v1 = vp_a->ensure_tsr_context(1, Size2i(960, 540), Size2i(1920, 1080), RSE::VIEWPORT_TSR_QUALITY_HIGH);
	REQUIRE(ctx_a_v1 != nullptr);
	CHECK(ctx_a_v1 != ctx_a);
	CHECK_EQ(RendererRD::TSRContext::get_live_context_count(), base_live + 3u);

	vp_a->free_data();
	CHECK_EQ(vp_a->get_tsr_context(0), nullptr);
	CHECK_EQ(vp_a->get_tsr_context(1), nullptr);
	CHECK_EQ(RendererRD::TSRContext::get_live_context_count(), base_live + 1u);

	vp_b->free_data();
	CHECK_EQ(vp_b->get_tsr_context(0), nullptr);
	CHECK_EQ(RendererRD::TSRContext::get_live_context_count(), base_live);
}

TEST_CASE("[Upscalers][TSR] Format table bytes per history pixel, T6 budgets, and DENSE velocity plan") {
	for (const RendererRD::TSRTextureFormatEntry &entry : RendererRD::TSR_FORMAT_TABLE) {
		CHECK_EQ(RD::get_image_format_pixel_size(entry.format), entry.bytes_per_pixel);
	}

	const uint32_t hist_bpp = RendererRD::TSR::history_bytes_per_pixel();
	const uint32_t int_bpp = RendererRD::TSR::internal_bytes_per_pixel();
	CHECK_EQ(hist_bpp, 10u);
	CHECK_LE(hist_bpp, 14u);
	CHECK_EQ(int_bpp, 8u);

	// T6 <= 14 B per history pixel: <= 52 MB at 1440p High, <= 116 MB at 2160p High, <= 465 MB at 2160p Epic.
	// Checked against conservative decimal MB (1,000,000 B), which also implies binary MiB (1,048,576 B).
	const uint64_t MB_DEC = 1000ull * 1000ull;

	const Size2i out_1440p(2560, 1440);
	const Size2i int_1440p_q(int(2560 * 0.667f), int(1440 * 0.667f));
	const uint64_t bytes_1440p_high = RendererRD::TSR::estimate_memory_bytes(int_1440p_q, out_1440p, RSE::VIEWPORT_TSR_QUALITY_HIGH);
	const uint64_t hist_px_1440p_high = uint64_t(2560) * uint64_t(1440);
	CHECK_LE(bytes_1440p_high, 52ull * MB_DEC);
	CHECK_LE(double(bytes_1440p_high) / double(hist_px_1440p_high), 14.0);

	const Size2i out_2160p(3840, 2160);
	const Size2i int_2160p_p(1920, 1080);
	const Size2i int_2160p_q(int(3840 * 0.667f), int(2160 * 0.667f));
	const uint64_t bytes_2160p_high_p = RendererRD::TSR::estimate_memory_bytes(int_2160p_p, out_2160p, RSE::VIEWPORT_TSR_QUALITY_HIGH);
	const uint64_t bytes_2160p_high_q = RendererRD::TSR::estimate_memory_bytes(int_2160p_q, out_2160p, RSE::VIEWPORT_TSR_QUALITY_HIGH);
	const uint64_t hist_px_2160p_high = uint64_t(3840) * uint64_t(2160);
	CHECK_LE(bytes_2160p_high_p, 116ull * MB_DEC);
	CHECK_LE(bytes_2160p_high_q, 116ull * MB_DEC);
	CHECK_LE(double(bytes_2160p_high_q) / double(hist_px_2160p_high), 14.0);

	const uint64_t bytes_2160p_epic_p = RendererRD::TSR::estimate_memory_bytes(int_2160p_p, out_2160p, RSE::VIEWPORT_TSR_QUALITY_EPIC);
	const uint64_t bytes_2160p_epic_native = RendererRD::TSR::estimate_memory_bytes(out_2160p, out_2160p, RSE::VIEWPORT_TSR_QUALITY_EPIC);
	const uint64_t hist_px_2160p_epic = uint64_t(7680) * uint64_t(4320);
	CHECK_LE(bytes_2160p_epic_p, 465ull * MB_DEC);
	CHECK_LE(bytes_2160p_epic_native, 465ull * MB_DEC);
	CHECK_LE(double(bytes_2160p_epic_native) / double(hist_px_2160p_epic), 14.0);

	// Step 2: SCALE_TSR maps to the DENSE NextGI velocity row alongside SCALE_EXTERNAL.
	const NextGIVelocityPlan tsr_vel = nextgi_velocity_plan(NEXTGI_VELOCITY_SCALE_TSR, NEXTGI_VELOCITY_DENSE, false, true);
	const NextGIVelocityPlan ext_vel = nextgi_velocity_plan(NEXTGI_VELOCITY_SCALE_EXTERNAL, NEXTGI_VELOCITY_DENSE, false, true);
	CHECK(tsr_vel.active);
	CHECK(tsr_vel.prepass_writes);
	CHECK_EQ(tsr_vel.pre_clear_r, ext_vel.pre_clear_r);
	CHECK_EQ(tsr_vel.pre_clear_g, ext_vel.pre_clear_g);
	CHECK_EQ(tsr_vel.mfx_preserve, ext_vel.mfx_preserve);
	CHECK_EQ(tsr_vel.skip_clear_pass, ext_vel.skip_clear_pass);
}

TEST_CASE("[Upscalers][DynRes] 16-bucket table, project settings defaults, and scale_range intersection") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);
	CHECK(ps->has_setting("rendering/scaling_3d/dynamic_resolution/enabled"));
	CHECK(ps->has_setting("rendering/scaling_3d/dynamic_resolution/target_frame_rate"));
	CHECK(ps->has_setting("rendering/scaling_3d/dynamic_resolution/min_scale"));
	CHECK(ps->has_setting("rendering/scaling_3d/dynamic_resolution/max_scale"));
	CHECK_FALSE(bool(ps->get_setting("rendering/scaling_3d/dynamic_resolution/enabled")));
	CHECK_EQ(int(ps->get_setting("rendering/scaling_3d/dynamic_resolution/target_frame_rate")), 60);
	CHECK(Math::is_equal_approx(float(ps->get_setting("rendering/scaling_3d/dynamic_resolution/min_scale")), 0.5f));
	CHECK(Math::is_equal_approx(float(ps->get_setting("rendering/scaling_3d/dynamic_resolution/max_scale")), 1.0f));

	CHECK_EQ(DynamicResolutionController::BUCKET_COUNT, 16);
	CHECK_EQ(DynamicResolutionController::RING_SIZE, 30);
	CHECK_EQ(DynamicResolutionController::MIN_FRAMES_BETWEEN_CHANGES, 30);

	float buckets[DynamicResolutionController::BUCKET_COUNT] = {};
	DynamicResolutionController::build_buckets(0.5f, 1.0f, Vector2(0.25f, 2.0f), buckets);
	CHECK(Math::is_equal_approx(buckets[0], 0.5f));
	CHECK(Math::is_equal_approx(buckets[15], 1.0f));
	for (int i = 0; i < DynamicResolutionController::BUCKET_COUNT; i++) {
		const float expected = 0.5f + (0.5f * float(i)) / 15.0f;
		CHECK(Math::is_equal_approx(buckets[i], expected));
		if (i > 0) {
			CHECK_GT(buckets[i], buckets[i - 1]);
		}
	}

	// Intersect [min_scale, max_scale] with each upscaler's scale_range.
	const Vector2 dlss_bounds = DynamicResolutionController::intersect_scale_bounds(0.1f, 2.0f, Vector2(0.30f, 1.0f));
	CHECK(Math::is_equal_approx(dlss_bounds.x, 0.30f));
	CHECK(Math::is_equal_approx(dlss_bounds.y, 1.0f));

	const Vector2 tsr_bounds = DynamicResolutionController::intersect_scale_bounds(0.2f, 1.5f, Vector2(NEXTGI_TSR_MIN_SCALE, NEXTGI_TSR_MAX_SCALE));
	CHECK(Math::is_equal_approx(tsr_bounds.x, 0.25f));
	CHECK(Math::is_equal_approx(tsr_bounds.y, 1.5f));

	const Vector2 mfx_bounds = DynamicResolutionController::intersect_scale_bounds(0.2f, 1.2f, Vector2(1.0f / 3.0f, 1.0f));
	CHECK(Math::is_equal_approx(mfx_bounds.x, 1.0f / 3.0f));
	CHECK(Math::is_equal_approx(mfx_bounds.y, 1.0f));

	// Inverted min > max and disjoint bounds normalize cleanly without NaN or inverted buckets.
	const Vector2 inverted = DynamicResolutionController::intersect_scale_bounds(0.9f, 0.6f, Vector2(0.25f, 2.0f));
	CHECK(Math::is_equal_approx(inverted.x, 0.6f));
	CHECK(Math::is_equal_approx(inverted.y, 0.9f));

	const Vector2 disjoint = DynamicResolutionController::intersect_scale_bounds(0.1f, 0.2f, Vector2(0.30f, 1.0f));
	CHECK(Math::is_equal_approx(disjoint.x, 0.30f));
	CHECK(Math::is_equal_approx(disjoint.y, 0.30f));
}

TEST_CASE("[Upscalers][DynRes] 30-frame rate limit and p95 single-spike rejection") {
	DynamicResolutionController ctrl;
	CHECK(ctrl.configure_bounds(0.5f, 1.0f, Vector2(0.25f, 2.0f)));
	CHECK_EQ(ctrl.get_current_bucket(), 15);
	CHECK(Math::is_equal_approx(ctrl.get_current_scale(), 1.0f));

	// 29 frames at 10 ms + 1 single-frame spike at 40 ms: p95 of 30 samples (sorted[28]) is 10 ms <= 16.667 ms.
	for (int i = 0; i < 29; i++) {
		CHECK_FALSE(ctrl.push_gpu_frame_time_ms(10.0, 60));
	}
	CHECK_FALSE(ctrl.push_gpu_frame_time_ms(40.0, 60));
	CHECK_EQ(ctrl.get_current_bucket(), 15);

	// A second overloaded frame makes 2/30 samples > target_ms, so p95 (sorted[28]) > target_ms and triggers a drop.
	CHECK(ctrl.push_gpu_frame_time_ms(33.333333, 60));
	const int dropped_bucket = ctrl.get_current_bucket();
	CHECK_LT(dropped_bucket, 15);
	CHECK_EQ(ctrl.get_frames_since_change(), 0);

	// Rate limit: at most 1 bucket change per 30 frames even under continuous heavy GPU overload.
	for (int i = 0; i < 29; i++) {
		CHECK_FALSE(ctrl.push_gpu_frame_time_ms(50.0, 60));
		CHECK_EQ(ctrl.get_current_bucket(), dropped_bucket);
	}
	// On the 30th frame after the change, the controller may change bucket again.
	CHECK(ctrl.push_gpu_frame_time_ms(50.0, 60));
	const int second_drop = ctrl.get_current_bucket();
	CHECK_LT(second_drop, dropped_bucket);

	// Same 30-frame rate limit in the upward direction.
	for (int i = 0; i < 29; i++) {
		CHECK_FALSE(ctrl.push_gpu_frame_time_ms(5.0, 60));
		CHECK_EQ(ctrl.get_current_bucket(), second_drop);
	}
	CHECK(ctrl.push_gpu_frame_time_ms(5.0, 60));
	CHECK_EQ(ctrl.get_current_bucket(), second_drop + 1);
}

TEST_CASE("[Upscalers][DynRes] Down-jump formula and up-step hysteresis") {
	DynamicResolutionController ctrl;
	ctrl.configure_bounds(0.5f, 1.0f, Vector2(0.25f, 2.0f));
	REQUIRE_EQ(ctrl.get_current_bucket(), 15);

	// 2x frame-budget load step (33.3333 ms vs 16.6667 ms at 60 Hz):
	// desired = 1.0 * sqrt(0.95 * 16.6667 / 33.3333) = sqrt(0.475) = 0.689202.
	// Over [0.5, 1.0] in 16 buckets (step 1/30): bucket 5 = 0.666667 <= 0.689202 < bucket 6 = 0.70.
	const double target_ms = 1000.0 / 60.0;
	for (int i = 0; i < 29; i++) {
		CHECK_FALSE(ctrl.push_gpu_frame_time_ms(2.0 * target_ms, 60));
	}
	CHECK(ctrl.push_gpu_frame_time_ms(2.0 * target_ms, 60));
	CHECK_EQ(ctrl.get_current_bucket(), 5);
	CHECK(Math::is_equal_approx(ctrl.get_current_scale(), 0.5f + 5.0f / 30.0f));

	// Up-step rule requires ALL 30 frames < 0.85 * target_ms (~14.1667 ms).
	// 29 light frames + 1 frame at 0.86 * target_ms must hold the current bucket.
	for (int i = 0; i < 29; i++) {
		CHECK_FALSE(ctrl.push_gpu_frame_time_ms(0.50 * target_ms, 60));
	}
	CHECK_FALSE(ctrl.push_gpu_frame_time_ms(0.86 * target_ms, 60));
	CHECK_EQ(ctrl.get_current_bucket(), 5);

	// Pushing 29 more light frames still has that one 0.86 * target_ms sample in the 30-entry ring.
	for (int i = 0; i < 29; i++) {
		CHECK_FALSE(ctrl.push_gpu_frame_time_ms(0.50 * target_ms, 60));
		CHECK_EQ(ctrl.get_current_bucket(), 5);
	}

	// Pushing one more light frame evicts the 0.86 * target_ms sample -> all 30 < 0.85 * target_ms -> steps up by +1.
	CHECK(ctrl.push_gpu_frame_time_ms(0.50 * target_ms, 60));
	CHECK_EQ(ctrl.get_current_bucket(), 6);
}

TEST_CASE("[Upscalers][DynRes] RenderSceneBuffersRD internal-only fast path preserves contexts and history without reset") {
	// Output/history-size named-texture allowlist vs internal-size render targets.
	CHECK(RenderSceneBuffersRD::is_output_sized_named_texture(RB_SCOPE_BUFFERS, RB_TEX_COLOR_UPSCALED));
	CHECK(RenderSceneBuffersRD::is_output_sized_named_texture(RB_SCOPE_BUFFERS, RB_TEX_BLUR_0));
	CHECK(RenderSceneBuffersRD::is_output_sized_named_texture(RB_SCOPE_BUFFERS, RB_TEX_BLUR_1));
	CHECK(RenderSceneBuffersRD::is_output_sized_named_texture(RB_SCOPE_TSR, SNAME("history_color_0")));
	CHECK(RenderSceneBuffersRD::is_output_sized_named_texture(RB_SCOPE_TSR, SNAME("history_color_1")));
	CHECK(RenderSceneBuffersRD::is_output_sized_named_texture(RB_SCOPE_TSR, SNAME("history_meta_0")));
	CHECK(RenderSceneBuffersRD::is_output_sized_named_texture(RB_SCOPE_TSR, SNAME("history_meta_1")));
	CHECK(RenderSceneBuffersRD::is_output_sized_named_texture(SNAME("Tonemapper"), SNAME("destination")));
	CHECK(RenderSceneBuffersRD::is_output_sized_named_texture(SNAME("rb_smaa"), SNAME("edges_tex")));

	CHECK_FALSE(RenderSceneBuffersRD::is_output_sized_named_texture(RB_SCOPE_BUFFERS, RB_TEX_COLOR));
	CHECK_FALSE(RenderSceneBuffersRD::is_output_sized_named_texture(RB_SCOPE_BUFFERS, RB_TEX_DEPTH));
	CHECK_FALSE(RenderSceneBuffersRD::is_output_sized_named_texture(RB_SCOPE_BUFFERS, RB_TEX_VELOCITY));
	CHECK_FALSE(RenderSceneBuffersRD::is_output_sized_named_texture(RB_SCOPE_BUFFERS, RB_TEX_BACK_COLOR));
	CHECK_FALSE(RenderSceneBuffersRD::is_output_sized_named_texture(RB_SCOPE_BUFFERS, RB_TEX_BACK_DEPTH));
	CHECK_FALSE(RenderSceneBuffersRD::is_output_sized_named_texture(RB_SCOPE_TSR, SNAME("dilated_velocity")));
	CHECK_FALSE(RenderSceneBuffersRD::is_output_sized_named_texture(RB_SCOPE_TSR, SNAME("prev_depth")));
	CHECK_FALSE(RenderSceneBuffersRD::is_output_sized_named_texture(RB_SCOPE_TSR, SNAME("disocclusion")));
	CHECK_FALSE(RenderSceneBuffersRD::is_output_sized_named_texture(RB_SCOPE_TSR, SNAME("rejection")));

	Ref<RenderSceneBuffersRD> rb;
	rb.instantiate();
	Ref<RendererSceneRenderImplementation::RenderForwardClustered::RenderBufferDataForwardClustered> rb_data;
	rb_data.instantiate();
	rb->set_custom_data(RB_SCOPE_FORWARD_CLUSTERED, rb_data);

	MockUpscaler mock_dlss;
	TemporalUpscalerRegistry::register_upscaler(RSE::VIEWPORT_SCALING_3D_MODE_DLSS, &mock_dlss);

	RenderSceneBuffersConfiguration cfg;
	cfg.set_internal_size(Size2i(1920, 1080));
	cfg.set_max_internal_size(Size2i(1920, 1080));
	cfg.set_min_internal_size(Size2i(960, 540));
	cfg.set_target_size(Size2i(1920, 1080));
	cfg.set_view_count(1);
	cfg.set_scaling_3d_mode(RSE::VIEWPORT_SCALING_3D_MODE_TSR);
	cfg.set_tsr_quality(RSE::VIEWPORT_TSR_QUALITY_EPIC);
	cfg.set_texture_mipmap_bias(0.0f);

	// Initial configure must run the full path and request a one-shot temporal history reset.
	CHECK_FALSE(rb->can_configure_internal_only(&cfg));
	rb->configure(&cfg);
	CHECK_EQ(rb->get_internal_size(), Size2i(1920, 1080));
	CHECK_EQ(rb->get_max_internal_size(), Size2i(1920, 1080));
	CHECK(rb->consume_temporal_history_reset());
	CHECK_FALSE(rb->consume_temporal_history_reset());

	RendererRD::TSRContext *tsr_ctx = rb_data->ensure_tsr_context(0, rb->get_internal_size(), rb->get_target_size(), rb->get_tsr_quality());
	REQUIRE(tsr_ctx != nullptr);
	CHECK(tsr_ctx->consume_reset());
	CHECK_FALSE(tsr_ctx->consume_reset());
	CHECK_EQ(mock_dlss.ensure_context(rb.ptr(), 0), RSE::VIEWPORT_SCALING_3D_REASON_NONE);
	CHECK(mock_dlss.contexts.has(rb.ptr()));

	// Stepping internal_size down (1920x1080 -> 1280x720) with updated mipmap bias takes the fast path:
	// keeps TSRContext and external upscaler context alive and does NOT request a temporal history reset.
	cfg.set_internal_size(Size2i(1280, 720));
	cfg.set_texture_mipmap_bias(-0.585f);
	CHECK(rb->can_configure_internal_only(&cfg));
	rb->configure(&cfg);
	CHECK_EQ(rb->get_internal_size(), Size2i(1280, 720));
	CHECK_EQ(rb->get_max_internal_size(), Size2i(1920, 1080));
	CHECK_FALSE(rb->consume_temporal_history_reset());
	CHECK_EQ(rb_data->get_tsr_context(0), tsr_ctx);
	CHECK(mock_dlss.contexts.has(rb.ptr()));
	CHECK_FALSE(tsr_ctx->sync_dimensions(rb->get_internal_size(), rb->get_target_size(), rb->get_tsr_quality()));
	CHECK_FALSE(tsr_ctx->consume_reset());

	// Stepping internal_size back up (1280x720 -> 1600x900 <= max_internal_size) also takes the fast path.
	cfg.set_internal_size(Size2i(1600, 900));
	cfg.set_texture_mipmap_bias(-0.263f);
	CHECK(rb->can_configure_internal_only(&cfg));
	rb->configure(&cfg);
	CHECK_EQ(rb->get_internal_size(), Size2i(1600, 900));
	CHECK_FALSE(rb->consume_temporal_history_reset());
	CHECK_EQ(rb_data->get_tsr_context(0), tsr_ctx);
	CHECK(mock_dlss.contexts.has(rb.ptr()));

	// Exceeding max_internal_size or mutating non-internal-size properties rejects the fast path.
	cfg.set_internal_size(Size2i(2048, 1152));
	CHECK_FALSE(rb->can_configure_internal_only(&cfg));
	cfg.set_internal_size(Size2i(1600, 900));

	cfg.set_target_size(Size2i(2560, 1440));
	CHECK_FALSE(rb->can_configure_internal_only(&cfg));
	cfg.set_target_size(Size2i(1920, 1080));

	cfg.set_tsr_quality(RSE::VIEWPORT_TSR_QUALITY_HIGH);
	CHECK_FALSE(rb->can_configure_internal_only(&cfg));
	cfg.set_tsr_quality(RSE::VIEWPORT_TSR_QUALITY_EPIC);

	cfg.set_scaling_3d_mode(RSE::VIEWPORT_SCALING_3D_MODE_FSR2);
	CHECK_FALSE(rb->can_configure_internal_only(&cfg));
	cfg.set_scaling_3d_mode(RSE::VIEWPORT_SCALING_3D_MODE_TSR);

	// Spatial modes keep full reallocation (no internal-only fast path).
	cfg.set_scaling_3d_mode(RSE::VIEWPORT_SCALING_3D_MODE_BILINEAR);
	CHECK_FALSE(rb->can_configure_internal_only(&cfg));
	rb->configure(&cfg);
	CHECK(rb->consume_temporal_history_reset());
	CHECK_EQ(rb_data->get_tsr_context(0), nullptr);
	CHECK_FALSE(mock_dlss.contexts.has(rb.ptr()));

	cfg.set_internal_size(Size2i(1280, 720));
	CHECK_FALSE(rb->can_configure_internal_only(&cfg));

	rb->cleanup();
	TemporalUpscalerRegistry::unregister_upscaler(RSE::VIEWPORT_SCALING_3D_MODE_DLSS);
}
