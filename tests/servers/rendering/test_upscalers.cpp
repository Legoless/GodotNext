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
#include "servers/rendering/renderer_rd/effects/temporal_upscaler.h"
#include "servers/rendering/renderer_rd/effects/tsr_presets.h"
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
