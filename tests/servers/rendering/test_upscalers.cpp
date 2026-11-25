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
#include "core/object/class_db.h"
#include "core/object/script_instance.h"
#include "core/os/os.h"
#include "scene/main/viewport.h"
#include "servers/rendering/dynamic_resolution.h"
#include "servers/rendering/renderer_rd/effects/temporal_upscaler.h"
#include "servers/rendering/renderer_rd/effects/tsr.h"
#include "servers/rendering/renderer_rd/effects/tsr_presets.h"
#include "servers/rendering/renderer_rd/effects/tsr_stability.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgi_velocity.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_enums.h"
#include "servers/rendering/storage/render_scene_buffers.h"
#include "servers/rendering/storage/temporal_upscaler_extension.h"

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

struct ScopedDLSSUpscalerSlot {
	TemporalUpscaler *saved = nullptr;
	ScopedDLSSUpscalerSlot() {
		saved = TemporalUpscalerRegistry::get(RSE::VIEWPORT_SCALING_3D_MODE_DLSS);
		if (saved != nullptr) {
			TemporalUpscalerRegistry::unregister_upscaler(RSE::VIEWPORT_SCALING_3D_MODE_DLSS);
		}
	}
	~ScopedDLSSUpscalerSlot() {
		TemporalUpscalerRegistry::unregister_upscaler(RSE::VIEWPORT_SCALING_3D_MODE_DLSS);
		if (saved != nullptr) {
			TemporalUpscalerRegistry::register_upscaler(RSE::VIEWPORT_SCALING_3D_MODE_DLSS, saved);
		}
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
	const ScopedDLSSUpscalerSlot clean_dlss;
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
	const ScopedDLSSUpscalerSlot clean_dlss;
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
	const ScopedDLSSUpscalerSlot clean_dlss;
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
		const float phase_scales[] = { 1.0f, 2.0f / 3.0f, 0.58f, 0.5f, 1.0f / 3.0f };
		const uint32_t expected[] = { 8, 18, 24, 32, 72 };
		for (int i = 0; i < 5; i++) {
			Size2i render = MockUpscaler().get_render_size(target, phase_scales[i]);
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

	const ScopedDLSSUpscalerSlot clean_dlss;
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
	// GodotNext: pt_temporal_upscaler_extension (#192). CUSTOM, like TSR, may
	// supersample and disables 3D MSAA internally.
	const TSRScalingResolution custom_supersample = nextgi_resolve_tsr_scaling(
			RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM, 1.5f, "forward_plus", true, false, RSE::VIEWPORT_MSAA_4X);
	CHECK_EQ(custom_supersample.mode, RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM);
	CHECK(Math::is_equal_approx(custom_supersample.scale, 1.5f));
	CHECK_EQ(custom_supersample.msaa_3d, RSE::VIEWPORT_MSAA_DISABLED);
	CHECK(custom_supersample.msaa_disabled);

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

// GodotNext: pt_tsr_d3d12_metal (#195).
TEST_CASE("[Upscalers][TSR] Storage format fallback follows a mock capability table") {
	using RendererRD::TSR;
	using RendererRD::TSRFormatSelection;

	// A mock device: the formats it cannot use as storage images. Every query is recorded.
	Vector<RD::DataFormat> asked;
	uint32_t wrong_usage = 0;
	auto select = [&](const Vector<RD::DataFormat> &p_no_storage) {
		asked.clear();
		wrong_usage = 0;
		return TSR::select_formats([&](RD::DataFormat p_format, uint32_t p_usage) {
			asked.push_back(p_format);
			if (p_usage != RendererRD::TSR_TEXTURE_USAGE) {
				wrong_usage++;
			}
			return !p_no_storage.has(p_format);
		});
	};
	auto asked_count = [&](RD::DataFormat p_format) {
		int count = 0;
		for (const RD::DataFormat format : asked) {
			count += format == p_format ? 1 : 0;
		}
		return count;
	};
	// The query covers storage, sampling and the clear's copy-to, as the textures are created.
	CHECK_EQ(RendererRD::TSR_TEXTURE_USAGE & RD::TEXTURE_USAGE_STORAGE_BIT, uint32_t(RD::TEXTURE_USAGE_STORAGE_BIT));
	CHECK_EQ(RendererRD::TSR_TEXTURE_USAGE & RD::TEXTURE_USAGE_SAMPLING_BIT, uint32_t(RD::TEXTURE_USAGE_SAMPLING_BIT));
	CHECK_EQ(RendererRD::TSR_TEXTURE_USAGE & RD::TEXTURE_USAGE_CAN_COPY_TO_BIT, uint32_t(RD::TEXTURE_USAGE_CAN_COPY_TO_BIT));

	const RD::DataFormat r11g11b10 = RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32;
	const RD::DataFormat rgba16f = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;

	// A device with every format: R11G11B10 history, each format of both tables asked once.
	{
		const TSRFormatSelection all = select({});
		CHECK(all.supported);
		CHECK_FALSE(all.history_rgba16f);
		CHECK_EQ(all.missing_format, RD::DATA_FORMAT_MAX);
		CHECK_EQ(all.resolve(r11g11b10), r11g11b10);
		CHECK_EQ(wrong_usage, 0u);
		CHECK_EQ(all.formats_checked, 4u); // R11G11B10, R8, RG16F and R16F.
		CHECK_EQ(uint32_t(asked.size()), all.formats_checked);
		CHECK_EQ(asked_count(rgba16f), 0);
		for (const RendererRD::TSRTextureFormatEntry &entry : RendererRD::TSR_FORMAT_TABLE) {
			CHECK_EQ(asked_count(entry.format), 1);
			CHECK_EQ(all.resolve(entry.format), entry.format);
			// GODOTNEXT_TSR_DENY_STORAGE_FORMATS can name every RB_SCOPE_TSR format.
			CHECK(String(TSR::format_name(entry.format)).begins_with("DATA_FORMAT_"));
		}
		for (const RendererRD::TSRStabilityStoreEntry &entry : RendererRD::TSR_STABILITY_STORE_TABLE) {
			CHECK_EQ(asked_count(entry.format), 1);
			CHECK(String(TSR::format_name(entry.format)).begins_with("DATA_FORMAT_"));
		}
	}

	// R11G11B10 cannot be a storage image (a Vulkan device without B10G11R11 storage, or
	// GODOTNEXT_TSR_DENY_STORAGE_FORMATS): history colour and the persistent frames take RGBA16F.
	{
		const TSRFormatSelection wide = select({ r11g11b10 });
		CHECK(wide.supported);
		CHECK(wide.history_rgba16f);
		CHECK_EQ(wide.missing_format, RD::DATA_FORMAT_MAX);
		CHECK_EQ(wide.formats_checked, 5u);
		CHECK_EQ(asked_count(r11g11b10), 1);
		CHECK_EQ(asked_count(rgba16f), 1);
		CHECK_EQ(wrong_usage, 0u);
		CHECK_EQ(wide.resolve(r11g11b10), rgba16f);
		CHECK_EQ(wide.resolve(RendererRD::TSR_STABILITY_STORE_TABLE[0].format), rgba16f); // The persistent frames.
		CHECK_EQ(wide.resolve(RD::DATA_FORMAT_R8_UNORM), RD::DATA_FORMAT_R8_UNORM);
		CHECK_EQ(wide.resolve(RD::DATA_FORMAT_R16G16_SFLOAT), RD::DATA_FORMAT_R16G16_SFLOAT);
		CHECK_EQ(RD::get_image_format_pixel_size(rgba16f), 8u);

		// T6 counts the fallback's extra bytes: 4 B more per history-colour texel, on both
		// history targets, so 18 B per history pixel for the #194 textures alone, over the
		// 14 B line. TSR still runs; layout.over_budget records the overrun (logged once when
		// the fallback allocates RB_SCOPE_TSR) and the stability stores shrink to their floor.
		const Size2i out_1440p(2560, 1440);
		const Size2i int_1440p_q(int(2560 * 0.667f), int(1440 * 0.667f));
		const uint64_t history_pixels = uint64_t(2560) * uint64_t(1440);
		const uint64_t packed_bytes = TSR::estimate_memory_bytes(int_1440p_q, out_1440p, RSE::VIEWPORT_TSR_QUALITY_HIGH, 1, false);
		const uint64_t wide_bytes = TSR::estimate_memory_bytes(int_1440p_q, out_1440p, RSE::VIEWPORT_TSR_QUALITY_HIGH, 1, wide.history_rgba16f);
		CHECK_EQ(wide_bytes - packed_bytes, 8ull * history_pixels);
		CHECK_EQ(TSR::history_bytes_per_pixel() + 8u, 18u);
		const uint32_t high = nextgi_tsr_feature_mask(RSE::VIEWPORT_TSR_QUALITY_HIGH, false);
		const RendererRD::TSRStabilityLayout packed_layout = TSR::stability_layout(int_1440p_q, int_1440p_q, out_1440p, RSE::VIEWPORT_TSR_QUALITY_HIGH, high, false);
		const RendererRD::TSRStabilityLayout wide_layout = TSR::stability_layout(int_1440p_q, int_1440p_q, out_1440p, RSE::VIEWPORT_TSR_QUALITY_HIGH, high, wide.history_rgba16f);
		CHECK_FALSE(packed_layout.over_budget);
		CHECK_EQ(wide_layout.core_bytes, wide_bytes);
		CHECK_GT(wide_layout.core_bytes, 14ull * history_pixels);
		CHECK(wide_layout.over_budget);
		CHECK_EQ(wide_layout.store_bytes_per_pixel(RendererRD::TSR_STABILITY_STORE_TABLE[0]), 8u);
		// The stores shrink to their floor instead of growing the overrun further.
		CHECK_EQ(wide_layout.persistent_fit_q16, TSR_FIT_MIN_Q16);
		CHECK_EQ(wide_layout.state_fit_q16, TSR_FIT_MIN_Q16);
	}

	// Neither R11G11B10 nor RGBA16F: no history format, and the selection says which.
	{
		const TSRFormatSelection none = select({ r11g11b10, rgba16f });
		CHECK_FALSE(none.supported);
		CHECK_FALSE(none.history_rgba16f);
		CHECK_EQ(none.missing_format, r11g11b10);
		CHECK_EQ(asked_count(r11g11b10), 1);
		CHECK_EQ(asked_count(rgba16f), 1);
	}

	// The other formats have no fallback; the first one missing, in table order, is reported.
	{
		const TSRFormatSelection no_r8 = select({ RD::DATA_FORMAT_R8_UNORM });
		CHECK_FALSE(no_r8.supported);
		CHECK_FALSE(no_r8.history_rgba16f);
		CHECK_EQ(no_r8.missing_format, RD::DATA_FORMAT_R8_UNORM);
		CHECK_EQ(asked_count(rgba16f), 0);

		const TSRFormatSelection no_rg16f = select({ RD::DATA_FORMAT_R16G16_SFLOAT });
		CHECK_FALSE(no_rg16f.supported);
		CHECK_EQ(no_rg16f.missing_format, RD::DATA_FORMAT_R16G16_SFLOAT);

		const TSRFormatSelection no_r16f = select({ RD::DATA_FORMAT_R16_SFLOAT });
		CHECK_FALSE(no_r16f.supported);
		CHECK_EQ(no_r16f.missing_format, RD::DATA_FORMAT_R16_SFLOAT);

		// dilated_velocity (RG16F) comes before prev_depth (R16F) in TSR_FORMAT_TABLE.
		const TSRFormatSelection no_half = select({ RD::DATA_FORMAT_R16_SFLOAT, RD::DATA_FORMAT_R16G16_SFLOAT });
		CHECK_EQ(no_half.missing_format, RD::DATA_FORMAT_R16G16_SFLOAT);

		// A missing R8 and a missing R11G11B10 with RGBA16F available: R8 is reported, and the
		// history still takes its fallback.
		const TSRFormatSelection mixed = select({ r11g11b10, RD::DATA_FORMAT_R8_UNORM });
		CHECK_FALSE(mixed.supported);
		CHECK(mixed.history_rgba16f);
		CHECK_EQ(mixed.missing_format, RD::DATA_FORMAT_R8_UNORM);
	}

	// GODOTNEXT_TSR_DENY_STORAGE_FORMATS: RB_SCOPE_TSR formats by name, any case, with or without
	// the DATA_FORMAT_ prefix; repeats, other formats and unknown names are ignored.
	{
		const Vector<RD::DataFormat> denied = TSR::parse_denied_formats(" b10g11r11_ufloat_pack32, DATA_FORMAT_R8_UNORM ,bogus,R8_UNORM,R32_SFLOAT,");
		REQUIRE_EQ(denied.size(), 2);
		CHECK_EQ(denied[0], r11g11b10);
		CHECK_EQ(denied[1], RD::DATA_FORMAT_R8_UNORM);
		CHECK(TSR::parse_denied_formats("").is_empty());
		CHECK(TSR::parse_denied_formats("R32_SFLOAT").is_empty());
		CHECK_EQ(String(TSR::format_name(r11g11b10)), String("DATA_FORMAT_B10G11R11_UFLOAT_PACK32"));
		CHECK(String(TSR::format_name(RD::DATA_FORMAT_R32_SFLOAT)).is_empty());

		// Exercise TSR's shared helpers (select_formats_for and select_env_formats, which
		// select_device_formats and TSR::TSR call) with OS's environment variable.
		const TSRFormatSelection by_list = TSR::select_formats_for("B10G11R11_UFLOAT_PACK32", [](RD::DataFormat, uint32_t p_usage) {
			return p_usage == RendererRD::TSR_TEXTURE_USAGE;
		});
		CHECK(by_list.supported);
		CHECK(by_list.history_rgba16f);

		struct ScopedDenyEnv {
			String prev_value;
			bool had_previous = false;

			explicit ScopedDenyEnv(const String &p_value) {
				OS *os = OS::get_singleton();
				if (os != nullptr) {
					had_previous = os->has_environment("GODOTNEXT_TSR_DENY_STORAGE_FORMATS");
					if (had_previous) {
						prev_value = os->get_environment("GODOTNEXT_TSR_DENY_STORAGE_FORMATS");
					}
					os->set_environment("GODOTNEXT_TSR_DENY_STORAGE_FORMATS", p_value);
				}
			}

			~ScopedDenyEnv() {
				OS *os = OS::get_singleton();
				if (os != nullptr) {
					if (had_previous) {
						os->set_environment("GODOTNEXT_TSR_DENY_STORAGE_FORMATS", prev_value);
					} else {
						os->unset_environment("GODOTNEXT_TSR_DENY_STORAGE_FORMATS");
					}
				}
			}
		};

		REQUIRE(OS::get_singleton() != nullptr);
		{
			ScopedDenyEnv env_fallback("B10G11R11_UFLOAT_PACK32");
			CHECK_EQ(TSR::get_deny_list(), String("B10G11R11_UFLOAT_PACK32"));
			const TSRFormatSelection forced = TSR::select_env_formats([](RD::DataFormat, uint32_t p_usage) {
				return p_usage == RendererRD::TSR_TEXTURE_USAGE;
			});
			CHECK(forced.supported);
			CHECK(forced.history_rgba16f);
			CHECK_EQ(forced.resolve(r11g11b10), rgba16f);
		}
		{
			ScopedDenyEnv env_unsupported("R8_UNORM");
			const TSRFormatSelection denied_r8 = TSR::select_env_formats([](RD::DataFormat, uint32_t p_usage) {
				return p_usage == RendererRD::TSR_TEXTURE_USAGE;
			});
			CHECK_FALSE(denied_r8.supported);
			CHECK_FALSE(denied_r8.history_rgba16f);
			CHECK_EQ(denied_r8.missing_format, RD::DATA_FORMAT_R8_UNORM);
		}
		if (RD::get_singleton() == nullptr) {
			const TSRFormatSelection headless = TSR::select_device_formats();
			CHECK_FALSE(headless.supported);
			CHECK_EQ(headless.missing_format, RD::DATA_FORMAT_MAX);
		}
	}
}

// GodotNext: pt_tsr_stability (#198).
TEST_CASE("[Upscalers][TSR] Stability feature mask follows TSR_PRESETS and the thin-geometry setting") {
	for (int q = 0; q < RSE::VIEWPORT_TSR_QUALITY_MAX; q++) {
		for (int thin = 0; thin < 2; thin++) {
			const uint32_t mask = nextgi_tsr_feature_mask(RSE::ViewportTSRQuality(q), thin != 0);
			// Each preset-driven feature reads TSR_PRESETS[q]; thin geometry reads only its setting.
			CHECK_EQ((mask & TSR_FEATURE_RESURRECTION) != 0, TSR_PRESETS[q].resurrection);
			CHECK_EQ((mask & TSR_FEATURE_REPROJECTION_FIELD) != 0, TSR_PRESETS[q].reprojection_field);
			CHECK_EQ((mask & TSR_FEATURE_FLICKER) != 0, TSR_PRESETS[q].flicker_analysis);
			CHECK_EQ((mask & TSR_FEATURE_THIN_GEOMETRY) != 0, thin != 0);
			CHECK_EQ(mask & ~uint32_t(TSR_FEATURE_ALL), 0u);
		}
	}

	const uint32_t preset_features = TSR_FEATURE_RESURRECTION | TSR_FEATURE_REPROJECTION_FIELD | TSR_FEATURE_FLICKER;
	CHECK_EQ(nextgi_tsr_feature_mask(RSE::VIEWPORT_TSR_QUALITY_LOW, false), 0u);
	CHECK_EQ(nextgi_tsr_feature_mask(RSE::VIEWPORT_TSR_QUALITY_LOW, true), uint32_t(TSR_FEATURE_THIN_GEOMETRY));
	CHECK_EQ(nextgi_tsr_feature_mask(RSE::VIEWPORT_TSR_QUALITY_MEDIUM, false), uint32_t(TSR_FEATURE_RESURRECTION));
	CHECK_EQ(nextgi_tsr_feature_mask(RSE::VIEWPORT_TSR_QUALITY_HIGH, false), preset_features);
	CHECK_EQ(nextgi_tsr_feature_mask(RSE::VIEWPORT_TSR_QUALITY_EPIC, false), preset_features);
	CHECK_EQ(nextgi_tsr_feature_mask(RSE::VIEWPORT_TSR_QUALITY_CINEMATIC, false), preset_features);
	CHECK_EQ(nextgi_tsr_feature_mask(RSE::VIEWPORT_TSR_QUALITY_CINEMATIC, true), uint32_t(TSR_FEATURE_ALL));

	// A quality above the table clamps to the last preset, as TSR::process does.
	CHECK_EQ(nextgi_tsr_feature_mask(RSE::VIEWPORT_TSR_QUALITY_MAX, false), nextgi_tsr_feature_mask(RSE::VIEWPORT_TSR_QUALITY_CINEMATIC, false));
	CHECK_EQ(nextgi_tsr_feature_mask(RSE::ViewportTSRQuality(7), true), nextgi_tsr_feature_mask(RSE::VIEWPORT_TSR_QUALITY_CINEMATIC, true));

	// The thin-geometry project setting is off by default, so by default Low adds nothing to #194.
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);
	REQUIRE(ps->has_setting("rendering/anti_aliasing/tsr/thin_geometry_detection"));
	const bool thin_setting = bool(ps->get_setting("rendering/anti_aliasing/tsr/thin_geometry_detection"));
	CHECK_FALSE(thin_setting);
	CHECK_EQ(nextgi_tsr_feature_mask(RSE::VIEWPORT_TSR_QUALITY_LOW, thin_setting), 0u);
}

TEST_CASE("[Upscalers][TSR] Stability pass plan leaves the core passes alone with no feature") {
	// No feature: the #194 shader variants, no uniform buffer, no extra pass.
	const TSRPassPlan none = nextgi_tsr_pass_plan(0);
	CHECK_FALSE(none.stability_ubo);
	CHECK_EQ(none.rejection_variant, 0u);
	CHECK_EQ(none.history_variant, 0u);
	CHECK_FALSE(none.analysis_flicker);
	CHECK_FALSE(none.analysis_thin);
	CHECK_FALSE(none.persistent_store);

	// Low with the thin-geometry setting off is that plan.
	const TSRPassPlan low = nextgi_tsr_pass_plan(nextgi_tsr_feature_mask(RSE::VIEWPORT_TSR_QUALITY_LOW, false));
	CHECK_FALSE(low.stability_ubo);
	CHECK_EQ(low.rejection_variant, 0u);
	CHECK_EQ(low.history_variant, 0u);
	CHECK_FALSE(low.analysis_flicker);
	CHECK_FALSE(low.analysis_thin);
	CHECK_FALSE(low.persistent_store);

	const TSRPassPlan resurrection = nextgi_tsr_pass_plan(TSR_FEATURE_RESURRECTION);
	CHECK(resurrection.stability_ubo);
	CHECK_EQ(resurrection.rejection_variant, 0u);
	CHECK_EQ(resurrection.history_variant, 1u);
	CHECK_FALSE(resurrection.analysis_flicker);
	CHECK_FALSE(resurrection.analysis_thin);
	CHECK(resurrection.persistent_store);

	const TSRPassPlan field = nextgi_tsr_pass_plan(TSR_FEATURE_REPROJECTION_FIELD);
	CHECK(field.stability_ubo);
	CHECK_EQ(field.rejection_variant, 0u);
	CHECK_EQ(field.history_variant, 1u);
	CHECK_FALSE(field.analysis_flicker);
	CHECK_FALSE(field.analysis_thin);
	CHECK_FALSE(field.persistent_store);

	const TSRPassPlan flicker = nextgi_tsr_pass_plan(TSR_FEATURE_FLICKER);
	CHECK(flicker.stability_ubo);
	CHECK_EQ(flicker.rejection_variant, 1u);
	CHECK_EQ(flicker.history_variant, 1u);
	CHECK(flicker.analysis_flicker);
	CHECK_FALSE(flicker.analysis_thin);
	CHECK_FALSE(flicker.persistent_store);

	const TSRPassPlan thin = nextgi_tsr_pass_plan(TSR_FEATURE_THIN_GEOMETRY);
	CHECK(thin.stability_ubo);
	CHECK_EQ(thin.rejection_variant, 1u);
	CHECK_EQ(thin.history_variant, 1u);
	CHECK_FALSE(thin.analysis_flicker);
	CHECK(thin.analysis_thin);
	CHECK_FALSE(thin.persistent_store);

	const TSRPassPlan all = nextgi_tsr_pass_plan(TSR_FEATURE_ALL);
	CHECK(all.stability_ubo);
	CHECK_EQ(all.rejection_variant, 1u);
	CHECK_EQ(all.history_variant, 1u);
	CHECK(all.analysis_flicker);
	CHECK(all.analysis_thin);
	CHECK(all.persistent_store);

	// Bits outside the feature set are ignored.
	CHECK_FALSE(nextgi_tsr_pass_plan(~uint32_t(TSR_FEATURE_ALL)).stability_ubo);
}

TEST_CASE("[Upscalers][TSR] Stability layout holds T6 at every pinned cell") {
	// T6: every RB_SCOPE_TSR texture together, resurrection and the other stability
	// stores included, <= 14 B per history pixel; <= 52 MB at 1440p High, <= 116 MB at
	// 2160p High, <= 465 MB at 2160p Epic (decimal MB, which also implies binary MiB).
	const uint64_t MB_DEC = 1000ull * 1000ull;

	struct Cell {
		Size2i output;
		Size2i internal;
		bool full_fit_at_100_percent_history; // Scale 0.5: the stability stores are full size at every preset.
		bool only_200_percent_history; // Scale 1.0 is pinned for Epic and Cinematic only.
	};
	const Size2i out_1440p(2560, 1440);
	const Size2i out_2160p(3840, 2160);
	const Cell cells[] = {
		{ out_1440p, Size2i(1280, 720), true, false },
		{ out_1440p, Size2i(int(2560 * 0.667f), int(1440 * 0.667f)), false, false },
		{ out_2160p, Size2i(1920, 1080), true, false },
		{ out_2160p, Size2i(int(3840 * 0.667f), int(2160 * 0.667f)), false, false },
		{ out_2160p, out_2160p, false, true },
	};

	uint32_t cells_checked = 0;
	for (const Cell &cell : cells) {
		for (int q = 0; q < RSE::VIEWPORT_TSR_QUALITY_MAX; q++) {
			const RSE::ViewportTSRQuality quality = RSE::ViewportTSRQuality(q);
			const bool history_200 = TSR_PRESETS[q].history_scale > 1.0f;
			if (cell.only_200_percent_history && !history_200) {
				continue;
			}
			for (int thin = 0; thin < 2; thin++) {
				// Every preset's own features on, whether or not their passes are built yet.
				const uint32_t features = nextgi_tsr_feature_mask(quality, thin != 0);
				const RendererRD::TSRStabilityLayout layout = RendererRD::TSR::stability_layout(cell.internal, cell.internal, cell.output, quality, features);
				const Size2i history_size = RendererRD::TSR::compute_history_size(cell.output, quality);
				const uint64_t history_pixels = uint64_t(history_size.width) * uint64_t(history_size.height);
				INFO("output ", cell.output.width, "x", cell.output.height, " internal ", cell.internal.width, "x", cell.internal.height, " quality ", q, " thin ", thin);

				CHECK_EQ(layout.features, features);
				CHECK_EQ(layout.core_bytes, RendererRD::TSR::estimate_memory_bytes(cell.internal, cell.output, quality));
				CHECK_LE(layout.total_bytes(), 14ull * history_pixels);
				CHECK_LE(layout.total_bytes(), layout.budget_bytes);
				CHECK_FALSE(layout.over_budget);
				CHECK_EQ(layout.persistent_count > 0, TSR_PRESETS[q].resurrection);

				if (!history_200 && cell.output == out_1440p) {
					CHECK_LE(layout.total_bytes(), 52ull * MB_DEC);
				}
				if (!history_200 && cell.output == out_2160p) {
					CHECK_LE(layout.total_bytes(), 116ull * MB_DEC);
				}
				if (history_200 && cell.output == out_2160p) {
					CHECK_LE(layout.total_bytes(), 465ull * MB_DEC);
				}

				if (cell.full_fit_at_100_percent_history || history_200) {
					CHECK_EQ(layout.persistent_fit_q16, TSR_FIT_FULL_Q16);
					CHECK_EQ(layout.state_fit_q16, TSR_FIT_FULL_Q16);
				}
				CHECK_GE(layout.persistent_fit_q16, TSR_FIT_MIN_Q16);
				CHECK_GE(layout.state_fit_q16, TSR_FIT_MIN_Q16);
				cells_checked++;
			}
		}
	}
	// 4 cells x 5 presets x 2, plus scale 1.0 x 2 presets x 2.
	CHECK_EQ(cells_checked, 44u);

	// The tuning table stays inside the uniform buffer's slots, and Low keeps no frames.
	for (int q = 0; q < RSE::VIEWPORT_TSR_QUALITY_MAX; q++) {
		CHECK_LE(uint32_t(TSR_STABILITY_TUNING[q].persistent_frames), TSR_PERSISTENT_MAX_FRAMES);
		CHECK_GE(uint32_t(TSR_STABILITY_TUNING[q].persistent_divisor), 1u);
		CHECK_EQ(TSR_STABILITY_TUNING[q].persistent_frames > 0, TSR_PRESETS[q].resurrection);
	}
	CHECK_LE(TSR_STABILITY_BUDGET_X100, 1400u);
}

TEST_CASE("[Upscalers][TSR] Stability layout sizes, fit scale and store table") {
	const Size2i out_1440p(2560, 1440);
	const Size2i out_2160p(3840, 2160);
	const Size2i int_1440p_q(int(2560 * 0.667f), int(1440 * 0.667f));
	const Size2i int_2160p_q(int(3840 * 0.667f), int(2160 * 0.667f));
	const uint32_t high_features = nextgi_tsr_feature_mask(RSE::VIEWPORT_TSR_QUALITY_HIGH, false);
	const uint32_t high_thin_features = nextgi_tsr_feature_mask(RSE::VIEWPORT_TSR_QUALITY_HIGH, true);
	const uint32_t medium_features = nextgi_tsr_feature_mask(RSE::VIEWPORT_TSR_QUALITY_MEDIUM, false);
	const uint32_t medium_thin_features = nextgi_tsr_feature_mask(RSE::VIEWPORT_TSR_QUALITY_MEDIUM, true);
	const uint32_t epic_features = nextgi_tsr_feature_mask(RSE::VIEWPORT_TSR_QUALITY_EPIC, false);
	const uint32_t epic_thin_features = nextgi_tsr_feature_mask(RSE::VIEWPORT_TSR_QUALITY_EPIC, true);

	// The store table matches the real formats.
	for (const RendererRD::TSRStabilityStoreEntry &entry : RendererRD::TSR_STABILITY_STORE_TABLE) {
		CHECK_EQ(RD::get_image_format_pixel_size(entry.format), entry.bytes_per_pixel);
		CHECK_EQ(entry.feature & ~uint32_t(TSR_FEATURE_ALL), 0u);
	}

	// No feature: nothing is added to the #194 textures, at any preset.
	for (int q = 0; q < RSE::VIEWPORT_TSR_QUALITY_MAX; q++) {
		const RSE::ViewportTSRQuality quality = RSE::ViewportTSRQuality(q);
		const RendererRD::TSRStabilityLayout off = RendererRD::TSR::stability_layout(Size2i(1920, 1080), Size2i(1920, 1080), out_2160p, quality, 0);
		CHECK_EQ(off.stability_bytes, 0ull);
		CHECK_EQ(off.total_bytes(), RendererRD::TSR::estimate_memory_bytes(Size2i(1920, 1080), out_2160p, quality));
		CHECK_EQ(off.persistent_count, 0u);
		CHECK_EQ(off.persistent_signature(), 0ull);
		for (const RendererRD::TSRStabilityStoreEntry &entry : RendererRD::TSR_STABILITY_STORE_TABLE) {
			CHECK_FALSE(off.has_store(entry));
		}
	}
	// Low has no persistent frames even when asked for resurrection.
	{
		const RendererRD::TSRStabilityLayout low = RendererRD::TSR::stability_layout(Size2i(1920, 1080), Size2i(1920, 1080), out_2160p, RSE::VIEWPORT_TSR_QUALITY_LOW, TSR_FEATURE_RESURRECTION);
		CHECK_EQ(low.persistent_count, 0u);
		CHECK_EQ(low.stability_bytes, 0ull);
		CHECK_FALSE(low.has_store(RendererRD::TSR_STABILITY_STORE_TABLE[0]));
	}

	struct Expected {
		Size2i output;
		Size2i internal;
		RSE::ViewportTSRQuality quality;
		uint32_t features;
		Size2i persistent;
		Size2i flicker;
		Size2i thin;
	};
	const Expected expected[] = {
		// Full fit.
		{ out_2160p, Size2i(1920, 1080), RSE::VIEWPORT_TSR_QUALITY_HIGH, high_features, Size2i(1280, 720), Size2i(1920, 1080), Size2i() },
		{ out_2160p, Size2i(1920, 1080), RSE::VIEWPORT_TSR_QUALITY_HIGH, high_thin_features, Size2i(1280, 720), Size2i(1920, 1080), Size2i(960, 540) },
		{ out_2160p, Size2i(1920, 1080), RSE::VIEWPORT_TSR_QUALITY_EPIC, epic_features, Size2i(1920, 1080), Size2i(1920, 1080), Size2i() },
		{ out_2160p, Size2i(1920, 1080), RSE::VIEWPORT_TSR_QUALITY_EPIC, epic_thin_features, Size2i(1920, 1080), Size2i(1920, 1080), Size2i(960, 540) },
		// Scale 0.667 at 100% history: the #194 textures leave 0.44 B per history pixel, so the stores shrink.
		{ out_1440p, int_1440p_q, RSE::VIEWPORT_TSR_QUALITY_HIGH, high_features, Size2i(401, 225), Size2i(805, 452), Size2i() },
		{ out_1440p, int_1440p_q, RSE::VIEWPORT_TSR_QUALITY_HIGH, high_thin_features, Size2i(378, 212), Size2i(759, 427), Size2i(379, 213) },
		{ out_1440p, int_1440p_q, RSE::VIEWPORT_TSR_QUALITY_MEDIUM, medium_features, Size2i(567, 319), Size2i(), Size2i() },
		{ out_1440p, int_1440p_q, RSE::VIEWPORT_TSR_QUALITY_MEDIUM, medium_thin_features, Size2i(507, 285), Size2i(), Size2i(512, 288) },
		{ out_2160p, int_2160p_q, RSE::VIEWPORT_TSR_QUALITY_HIGH, high_features, Size2i(601, 338), Size2i(1205, 677), Size2i() },
		{ out_2160p, int_2160p_q, RSE::VIEWPORT_TSR_QUALITY_HIGH, high_thin_features, Size2i(567, 319), Size2i(1135, 638), Size2i(567, 319) },
	};
	for (const Expected &e : expected) {
		const RendererRD::TSRStabilityLayout layout = RendererRD::TSR::stability_layout(e.internal, e.internal, e.output, e.quality, e.features);
		INFO("output ", e.output.width, "x", e.output.height, " internal ", e.internal.width, "x", e.internal.height, " quality ", int(e.quality), " features ", e.features);
		CHECK_EQ(layout.persistent_count, 2u);
		CHECK_EQ(layout.persistent_size, e.persistent);
		CHECK_EQ(layout.flicker_size, e.flicker);
		CHECK_EQ(layout.thin_size, e.thin);
		CHECK_FALSE(layout.over_budget);

		// stability_bytes is the sum over the store table at the layout's sizes.
		uint64_t table_bytes = 0;
		for (const RendererRD::TSRStabilityStoreEntry &entry : RendererRD::TSR_STABILITY_STORE_TABLE) {
			CHECK_EQ(layout.has_store(entry), (e.features & entry.feature) != 0);
			if (layout.has_store(entry)) {
				const Size2i size = layout.store_size(entry.store);
				CHECK_GE(size.width, 1);
				CHECK_GE(size.height, 1);
				CHECK_EQ(layout.store_bytes_per_pixel(entry), entry.bytes_per_pixel);
				table_bytes += uint64_t(size.width) * uint64_t(size.height) * uint64_t(layout.store_layers(entry.store)) * uint64_t(entry.bytes_per_pixel);
			}
		}
		CHECK_EQ(layout.stability_bytes, table_bytes);
	}

	// Totals of the three T6 megabyte lines with High's and Epic's features on.
	CHECK_EQ(RendererRD::TSR::stability_layout(int_1440p_q, int_1440p_q, out_1440p, RSE::VIEWPORT_TSR_QUALITY_HIGH, high_features).total_bytes(), 51423280ull);
	CHECK_EQ(RendererRD::TSR::stability_layout(Size2i(1920, 1080), Size2i(1920, 1080), out_2160p, RSE::VIEWPORT_TSR_QUALITY_HIGH, high_features).total_bytes(), 111052800ull);
	CHECK_EQ(RendererRD::TSR::stability_layout(Size2i(1920, 1080), Size2i(1920, 1080), out_2160p, RSE::VIEWPORT_TSR_QUALITY_EPIC, epic_features).total_bytes(), 369100800ull);

	// Dynamic resolution, 0.5 bucket of a 0.5 to 1.0 range at 1440p High: the persistent
	// frames are sized for the largest bucket (the floor here, where #194 alone is over
	// the line) so a resolution step never resizes them; the state stores follow the
	// current bucket and are full size.
	{
		const RendererRD::TSRStabilityLayout bucket = RendererRD::TSR::stability_layout(Size2i(1280, 720), out_1440p, out_1440p, RSE::VIEWPORT_TSR_QUALITY_HIGH, high_thin_features);
		const RendererRD::TSRStabilityLayout top = RendererRD::TSR::stability_layout(out_1440p, out_1440p, out_1440p, RSE::VIEWPORT_TSR_QUALITY_HIGH, high_thin_features);
		CHECK_EQ(bucket.persistent_fit_q16, TSR_FIT_MIN_Q16);
		CHECK_EQ(bucket.persistent_size, Size2i(213, 120));
		CHECK_EQ(bucket.persistent_size, top.persistent_size);
		CHECK_EQ(bucket.persistent_signature(), top.persistent_signature());
		CHECK_EQ(bucket.state_fit_q16, TSR_FIT_FULL_Q16);
		CHECK_EQ(bucket.flicker_size, Size2i(1280, 720));
		CHECK_EQ(bucket.thin_size, Size2i(640, 360));
		CHECK_FALSE(bucket.over_budget);
		CHECK_LE(bucket.total_bytes(), bucket.budget_bytes);
		CHECK_EQ(bucket.total_bytes(), 46745280ull);
		// A missing maximum means the current internal size.
		CHECK_EQ(RendererRD::TSR::stability_layout(Size2i(1280, 720), Size2i(), out_1440p, RSE::VIEWPORT_TSR_QUALITY_HIGH, high_thin_features).persistent_size, Size2i(853, 480));
	}

	// 100% history at scale 1.0: the #194 textures alone are over the line (18 B per
	// history pixel). The stores sit at the floor and the features stay on.
	{
		const RendererRD::TSRStabilityLayout native = RendererRD::TSR::stability_layout(out_1440p, out_1440p, out_1440p, RSE::VIEWPORT_TSR_QUALITY_HIGH, high_features);
		CHECK(native.over_budget);
		CHECK_EQ(native.persistent_fit_q16, TSR_FIT_MIN_Q16);
		CHECK_EQ(native.state_fit_q16, TSR_FIT_MIN_Q16);
		CHECK_EQ(native.persistent_size, Size2i(213, 120));
		CHECK_EQ(native.flicker_size, Size2i(640, 360));
		CHECK_EQ(native.stability_bytes, 665280ull);
	}

	// RGBA16F history fallback: the persistent frames take 8 B per pixel, like the history colour.
	{
		const RendererRD::TSRStabilityLayout packed = RendererRD::TSR::stability_layout(Size2i(1920, 1080), Size2i(1920, 1080), out_2160p, RSE::VIEWPORT_TSR_QUALITY_EPIC, TSR_FEATURE_RESURRECTION, false);
		const RendererRD::TSRStabilityLayout wide = RendererRD::TSR::stability_layout(Size2i(1920, 1080), Size2i(1920, 1080), out_2160p, RSE::VIEWPORT_TSR_QUALITY_EPIC, TSR_FEATURE_RESURRECTION, true);
		CHECK_EQ(packed.persistent_size, Size2i(1920, 1080));
		CHECK_EQ(packed.stability_bytes, 2ull * 4ull * 1920ull * 1080ull);
		CHECK_EQ(packed.store_bytes_per_pixel(RendererRD::TSR_STABILITY_STORE_TABLE[0]), 4u);
		CHECK_EQ(wide.store_bytes_per_pixel(RendererRD::TSR_STABILITY_STORE_TABLE[0]), 8u);
		CHECK_EQ(wide.core_bytes, RendererRD::TSR::estimate_memory_bytes(Size2i(1920, 1080), out_2160p, RSE::VIEWPORT_TSR_QUALITY_EPIC, 1, true));
		CHECK_EQ(wide.stability_bytes, 2ull * 8ull * uint64_t(wide.persistent_size.width) * uint64_t(wide.persistent_size.height));
		CHECK_NE(wide.persistent_signature(), packed.persistent_signature());
	}

	// The persistent signature follows the frames' size and count only.
	{
		const RendererRD::TSRStabilityLayout a = RendererRD::TSR::stability_layout(Size2i(1920, 1080), Size2i(1920, 1080), out_2160p, RSE::VIEWPORT_TSR_QUALITY_HIGH, high_features);
		const RendererRD::TSRStabilityLayout b = RendererRD::TSR::stability_layout(Size2i(1920, 1080), Size2i(1920, 1080), out_2160p, RSE::VIEWPORT_TSR_QUALITY_HIGH, high_thin_features);
		const RendererRD::TSRStabilityLayout c = RendererRD::TSR::stability_layout(Size2i(1920, 1080), Size2i(1920, 1080), out_2160p, RSE::VIEWPORT_TSR_QUALITY_EPIC, epic_features);
		CHECK_NE(a.persistent_signature(), 0ull);
		CHECK_EQ(a.persistent_signature(), b.persistent_signature());
		CHECK_NE(a.persistent_signature(), c.persistent_signature());
	}

	// A tighter budget shrinks the stores; the default one is TSR_STABILITY_BUDGET_X100.
	{
		const RendererRD::TSRStabilityLayout tight = RendererRD::TSR::stability_layout(Size2i(1920, 1080), Size2i(1920, 1080), out_2160p, RSE::VIEWPORT_TSR_QUALITY_HIGH, high_features, false, 1250);
		CHECK_LT(tight.persistent_fit_q16, TSR_FIT_FULL_Q16);
		CHECK_FALSE(tight.over_budget);
		CHECK_LE(tight.total_bytes(), uint64_t(3840) * uint64_t(2160) * 1250ull / 100ull);
	}
}

TEST_CASE("[Upscalers][TSR] Persistent frame schedule") {
	// No capture during the 16-frame warm-up; then one slot every 35 frames, in turn.
	for (uint32_t frame = 0; frame < TSR_PERSISTENT_WARMUP_FRAMES; frame++) {
		CHECK_EQ(nextgi_tsr_persistent_capture_slot(frame, 2), -1);
	}
	CHECK_EQ(TSR_PERSISTENT_WARMUP_FRAMES, 16u);
	CHECK_EQ(TSR_PERSISTENT_INTERVAL_FRAMES, 35u);
	CHECK_EQ(nextgi_tsr_persistent_capture_slot(16, 2), 0);
	CHECK_EQ(nextgi_tsr_persistent_capture_slot(17, 2), -1);
	CHECK_EQ(nextgi_tsr_persistent_capture_slot(50, 2), -1);
	CHECK_EQ(nextgi_tsr_persistent_capture_slot(51, 2), 1);
	CHECK_EQ(nextgi_tsr_persistent_capture_slot(86, 2), 0);
	CHECK_EQ(nextgi_tsr_persistent_capture_slot(121, 2), 1);
	uint32_t captures = 0;
	for (uint32_t frame = 0; frame < 16 + 35 * 8; frame++) {
		const int32_t slot = nextgi_tsr_persistent_capture_slot(frame, 4);
		if (slot >= 0) {
			CHECK_EQ(uint32_t(slot), captures % 4);
			captures++;
		}
		// No frames, no capture.
		CHECK_EQ(nextgi_tsr_persistent_capture_slot(frame, 0), -1);
	}
	CHECK_EQ(captures, 8u);

	RendererRD::TSRContext ctx(Size2i(1280, 720), Size2i(2560, 1440), RSE::VIEWPORT_TSR_QUALITY_HIGH);
	CHECK_EQ(ctx.get_frames_since_reset(), 0u);
	for (uint32_t slot = 0; slot < TSR_PERSISTENT_MAX_FRAMES; slot++) {
		CHECK_FALSE(ctx.get_persistent_frame(slot).valid);
	}

	// A frame advances the count; a stored frame is valid and keeps its camera.
	for (int i = 0; i < 20; i++) {
		ctx.advance_frame();
	}
	CHECK_EQ(ctx.get_frames_since_reset(), 20u);
	Projection projection;
	projection.set_perspective(70.0, 16.0 / 9.0, 0.05, 4000.0);
	const Transform3D camera(Basis(), Vector3(1.0, 2.0, 3.0));
	ctx.set_persistent_frame(0, projection, camera);
	ctx.set_persistent_frame(1, projection, camera);
	CHECK(ctx.get_persistent_frame(0).valid);
	CHECK(ctx.get_persistent_frame(1).valid);
	CHECK_FALSE(ctx.get_persistent_frame(2).valid);
	CHECK_EQ(ctx.get_persistent_frame(1).projection, projection);
	CHECK_EQ(ctx.get_persistent_frame(1).camera_transform, camera);

	// invalidate_persistent() clears every slot and restarts the schedule.
	ctx.invalidate_persistent();
	CHECK_EQ(ctx.get_frames_since_reset(), 0u);
	for (uint32_t slot = 0; slot < TSR_PERSISTENT_MAX_FRAMES; slot++) {
		CHECK_FALSE(ctx.get_persistent_frame(slot).valid);
	}

	// An internal-only resolution change keeps the persistent frames, as it keeps the history.
	for (int i = 0; i < 20; i++) {
		ctx.advance_frame();
	}
	ctx.set_persistent_frame(0, projection, camera);
	CHECK_FALSE(ctx.sync_dimensions(Size2i(1600, 900), Size2i(2560, 1440), RSE::VIEWPORT_TSR_QUALITY_HIGH));
	CHECK(ctx.get_persistent_frame(0).valid);
	CHECK_EQ(ctx.get_frames_since_reset(), 20u);

	// A history resize through sync_dimensions invalidates them.
	CHECK(ctx.sync_dimensions(Size2i(1600, 900), Size2i(2560, 1440), RSE::VIEWPORT_TSR_QUALITY_EPIC));
	CHECK_FALSE(ctx.get_persistent_frame(0).valid);
	CHECK_EQ(ctx.get_frames_since_reset(), 0u);

	// So does a change of the persistent_frames texture or of its layout; the same
	// texture and layout again does not.
	const uint64_t signature = RendererRD::TSR::stability_layout(Size2i(1600, 900), Size2i(1600, 900), Size2i(2560, 1440), RSE::VIEWPORT_TSR_QUALITY_EPIC, TSR_FEATURE_RESURRECTION).persistent_signature();
	ctx.sync_persistent_storage(RID(), signature);
	ctx.advance_frame();
	ctx.set_persistent_frame(0, projection, camera);
	ctx.sync_persistent_storage(RID(), signature);
	CHECK(ctx.get_persistent_frame(0).valid);
	CHECK_EQ(ctx.get_frames_since_reset(), 1u);
	ctx.sync_persistent_storage(RID(), signature + 1);
	CHECK_FALSE(ctx.get_persistent_frame(0).valid);
	CHECK_EQ(ctx.get_frames_since_reset(), 0u);
	ctx.set_persistent_frame(0, projection, camera);
	ctx.sync_persistent_storage(RID::from_uint64(1), signature + 1);
	CHECK_FALSE(ctx.get_persistent_frame(0).valid);
}

TEST_CASE("[Upscalers][TSR] Flicker ticks and decay follow frame time") {
	// Frame times as the renderer passes them (TSR::Parameters::delta_time is a float).
	const int rates[] = { 30, 60, 120 };
	const uint32_t ticks_per_frame[] = { 4, 2, 1 };
	const uint32_t decay_per_frame[] = { 2, 1, 0 }; // 120 Hz: 0 and 1 in turn.
	const float max_motion[] = { 0.5f, 0.25f, 0.125f };
	for (int i = 0; i < 3; i++) {
		const int hz = rates[i];
		const double delta = double(1.0f / float(hz));
		INFO("rate ", hz, " Hz");
		double tick_remainder = 0.0;
		double decay_remainder = 0.0;
		uint32_t tick_sum = 0;
		uint32_t decay_sum = 0;
		for (int frame = 0; frame < hz; frame++) {
			const uint32_t ticks = nextgi_tsr_flicker_ticks(tick_remainder, delta);
			const uint32_t decay = nextgi_tsr_flicker_decay_steps(decay_remainder, delta);
			CHECK_EQ(ticks, ticks_per_frame[i]);
			if (hz == 120) {
				CHECK_EQ(decay, uint32_t(frame & 1));
			} else {
				CHECK_EQ(decay, decay_per_frame[i]);
			}
			tick_sum += ticks;
			decay_sum += decay;
		}
		// One second of frames is 120 ticks and 60 decay steps at every rate.
		CHECK_EQ(tick_sum, TSR_FLICKER_TICK_HZ);
		CHECK_EQ(decay_sum, TSR_FLICKER_DECAY_HZ);
		// The motion limit is the same per second: 15 internal pixels.
		CHECK(Math::is_equal_approx(nextgi_tsr_flicker_max_motion(delta), max_motion[i], 1e-6f));
		CHECK(Math::is_equal_approx(nextgi_tsr_flicker_max_motion(delta) * float(hz), float(TSR_FLICKER_MAX_MOTION_PX_PER_S), 1e-4f));
	}

	// Above 120 Hz some frames carry no tick: 240 Hz alternates 0 and 1.
	{
		double remainder = 0.0;
		const double delta = double(1.0f / 240.0f);
		uint32_t sum = 0;
		for (int frame = 0; frame < 240; frame++) {
			const uint32_t ticks = nextgi_tsr_flicker_ticks(remainder, delta);
			CHECK_EQ(ticks, uint32_t(frame & 1));
			sum += ticks;
		}
		CHECK_EQ(sum, TSR_FLICKER_TICK_HZ);
	}

	// A hitch counts as at most TSR_FLICKER_MAX_FRAME_TIME and yields at most 6 steps;
	// the remainder does not run away, so the next frames are normal again.
	{
		double tick_remainder = 0.0;
		double decay_remainder = 0.0;
		CHECK_EQ(nextgi_tsr_flicker_ticks(tick_remainder, 1.0), TSR_FLICKER_MAX_STEPS);
		CHECK_EQ(nextgi_tsr_flicker_decay_steps(decay_remainder, 1.0), TSR_FLICKER_MAX_STEPS);
		CHECK_LE(TSR_FLICKER_MAX_STEPS, 6u);
		CHECK_EQ(nextgi_tsr_flicker_ticks(tick_remainder, double(1.0f / 60.0f)), 2u);
		CHECK_EQ(nextgi_tsr_flicker_decay_steps(decay_remainder, double(1.0f / 60.0f)), 1u);
		CHECK(Math::is_equal_approx(nextgi_tsr_flicker_max_motion(1.0), float(TSR_FLICKER_MAX_FRAME_TIME * TSR_FLICKER_MAX_MOTION_PX_PER_S)));
	}

	// No time, no steps; a bad frame time counts as none.
	{
		double remainder = 0.0;
		CHECK_EQ(nextgi_tsr_flicker_ticks(remainder, 0.0), 0u);
		CHECK_EQ(nextgi_tsr_flicker_ticks(remainder, -1.0), 0u);
		CHECK_EQ(nextgi_tsr_flicker_ticks(remainder, Math::NaN), 0u);
		CHECK_EQ(remainder, 0.0);
		CHECK_EQ(nextgi_tsr_flicker_max_motion(0.0), 0.0f);
	}

	// Each TSR context carries its own remainders.
	{
		RendererRD::TSRContext ctx(Size2i(1280, 720), Size2i(2560, 1440), RSE::VIEWPORT_TSR_QUALITY_HIGH);
		uint32_t tick_sum = 0;
		uint32_t decay_sum = 0;
		for (int frame = 0; frame < 120; frame++) {
			uint32_t ticks = 0;
			uint32_t decay = 0;
			ctx.step_flicker_clock(double(1.0f / 120.0f), ticks, decay);
			CHECK_EQ(ticks, 1u);
			CHECK_EQ(decay, uint32_t(frame & 1));
			tick_sum += ticks;
			decay_sum += decay;
		}
		CHECK_EQ(tick_sum, TSR_FLICKER_TICK_HZ);
		CHECK_EQ(decay_sum, TSR_FLICKER_DECAY_HZ);
	}
}

TEST_CASE("[Upscalers][TSR] Flicker step classifies the same signals at 30, 60 and 120 Hz") {
	// The state byte: score in bits 7..4, direction in bit 3, ticks since the last
	// counted reversal in bits 2..0 (at most 6; 7 only as "disabled").
	auto score_of = [](uint8_t p_state) { return int32_t(p_state >> 4); };
	auto since_of = [](uint8_t p_state) { return int32_t(p_state & 7u); };

	// Every state and input keeps the byte inside that layout.
	for (uint32_t state = 0; state < 256; state++) {
		for (uint32_t ticks = 0; ticks <= 7; ticks++) {
			for (uint32_t decay = 0; decay <= 7; decay++) {
				for (int inputs = 0; inputs < 4; inputs++) {
					const uint8_t next = nextgi_tsr_flicker_step(uint8_t(state), (inputs & 1) != 0, (inputs & 2) != 0, ticks, decay);
					CHECK_LE(since_of(next), TSR_FLICKER_SINCE_MAX);
					CHECK_LE(score_of(next), TSR_FLICKER_SCORE_MAX);
				}
			}
		}
	}

	// A texel just enabled (cleared or disabled) does not score on its first reversal.
	CHECK_EQ(score_of(nextgi_tsr_flicker_step(TSR_FLICKER_STATE_DISABLED, true, true, 1, 0)), 0);
	CHECK_EQ(score_of(nextgi_tsr_flicker_step(TSR_FLICKER_STATE_DISABLED, false, true, 1, 0)), 0);
	CHECK_EQ(since_of(nextgi_tsr_flicker_step(TSR_FLICKER_STATE_DISABLED, true, true, 1, 0)), 0);
	// Releases cannot wrap below 0.
	CHECK_EQ(score_of(nextgi_tsr_flicker_step(uint8_t((1u << 4) | 6u), false, false, 1, 6)), 0);
	CHECK_EQ(score_of(nextgi_tsr_flicker_step(uint8_t((1u << 4) | (1u << 3) | 6u), false, true, 1, 0)), 0);

	// The signals are square waves in luma, built on the 1/120 s tick so they are exact at
	// every rate: the direction flips every 4 ticks (15 Hz) or every 12 ticks (5 Hz).
	struct Run {
		int32_t peak_score = 0;
		int32_t final_score = 0;
		int32_t last_reversal_frame = -1;
		int32_t release_frame = -1; // First frame with score 0 after the signal stopped.
		bool layout_ok = true;
	};
	auto simulate = [&](int p_hz, int p_flip_ticks, int p_signal_frames, int p_total_frames) -> Run {
		Run run;
		double tick_remainder = 0.0;
		double decay_remainder = 0.0;
		const double delta = double(1.0f / float(p_hz));
		const int ticks_per_frame = int(TSR_FLICKER_TICK_HZ) / p_hz;
		uint8_t state = TSR_FLICKER_STATE_DISABLED;
		bool previous_positive = false;
		for (int frame = 0; frame < p_total_frames; frame++) {
			const uint32_t ticks = nextgi_tsr_flicker_ticks(tick_remainder, delta);
			const uint32_t decay = nextgi_tsr_flicker_decay_steps(decay_remainder, delta);
			const bool signal = frame < p_signal_frames;
			const bool positive = ((frame * ticks_per_frame) / p_flip_ticks) % 2 == 0;
			// While the signal runs, the luma is always off the history mean; then it holds still.
			state = nextgi_tsr_flicker_step(state, positive, signal, ticks, decay);
			if (signal && frame > 0 && positive != previous_positive) {
				run.last_reversal_frame = frame;
			}
			previous_positive = positive;
			run.peak_score = MAX(run.peak_score, score_of(state));
			if (!signal && run.release_frame < 0 && score_of(state) == 0) {
				run.release_frame = frame;
			}
			run.layout_ok = run.layout_ok && since_of(state) <= TSR_FLICKER_SINCE_MAX;
		}
		run.final_score = score_of(state);
		return run;
	};

	const int rates[] = { 30, 60, 120 };
	for (const int hz : rates) {
		INFO("rate ", hz, " Hz");
		// 15 Hz flicker (a reversal every 1/30 s) for 1 s, then 0.5 s of still luma.
		const Run fast = simulate(hz, 4, hz, hz + hz / 2);
		CHECK(fast.layout_ok);
		CHECK_EQ(fast.peak_score, TSR_FLICKER_SCORE_MAX);
		CHECK_EQ(fast.final_score, 0);
		REQUIRE(fast.last_reversal_frame > 0);
		REQUIRE(fast.release_frame > fast.last_reversal_frame);
		// Released 0.25 s to 0.32 s after the last reversal: the 1/30 s half period, then
		// 15 levels at 60 per second.
		const double release_s = double(fast.release_frame - fast.last_reversal_frame) / double(hz);
		CHECK_GE(release_s, 0.25);
		CHECK_LE(release_s, 0.32);

		// 5 Hz (a reversal every 0.1 s) is animation at every rate: it never scores.
		const Run slow = simulate(hz, 12, 2 * hz, 2 * hz);
		CHECK(slow.layout_ok);
		CHECK_EQ(slow.peak_score, 0);
		CHECK_EQ(slow.final_score, 0);
	}

	// Detection takes a few frames at any rate: 5 qualifying reversals reach the full score.
	{
		uint8_t state = TSR_FLICKER_STATE_DISABLED;
		bool positive = true;
		state = nextgi_tsr_flicker_step(state, positive, true, 2, 1); // First reversal from disabled: no score.
		int reversals = 0;
		while (score_of(state) < TSR_FLICKER_SCORE_MAX && reversals < 16) {
			positive = !positive;
			state = nextgi_tsr_flicker_step(state, positive, true, 2, 1);
			reversals++;
		}
		CHECK_EQ(reversals, 5);
	}
}

// GodotNext: pt_tsr_stability (#198).
TEST_CASE("[Upscalers][TSR] Debug draw value and debug view setting") {
	// The TSR pass visualizer is the last debug-draw value, appended after the fork's others.
	CHECK_EQ(int(RSE::VIEWPORT_DEBUG_DRAW_TSR), 61);
	CHECK_EQ(int(RSE::VIEWPORT_DEBUG_DRAW_TSR), int(RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_LITE_PROBES) + 1);
	CHECK_EQ(int(Viewport::DEBUG_DRAW_TSR), int(RSE::VIEWPORT_DEBUG_DRAW_TSR));

	// The setting picks the pass, defaults to history validity and lists the six views in TSRDebugView order.
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);
	const String setting = "rendering/anti_aliasing/tsr/debug_view";
	REQUIRE(ps->has_setting(setting));
	CHECK_EQ(int(ps->get_setting(setting)), int(TSR_DEBUG_VIEW_HISTORY_VALIDITY));
	REQUIRE(ps->get_custom_property_info().has(StringName(setting)));
	const PropertyInfo &info = ps->get_custom_property_info()[StringName(setting)];
	CHECK_EQ(int(info.type), int(Variant::INT));
	CHECK_EQ(int(info.hint), int(PROPERTY_HINT_ENUM));
	const Vector<String> names = info.hint_string.split(",");
	REQUIRE_EQ(names.size(), int(TSR_DEBUG_VIEW_MAX));
	CHECK_EQ(names[TSR_DEBUG_VIEW_HISTORY_VALIDITY], "History Validity");
	CHECK_EQ(names[TSR_DEBUG_VIEW_REJECTION], "Rejection");
	CHECK_EQ(names[TSR_DEBUG_VIEW_DISOCCLUSION], "Disocclusion");
	CHECK_EQ(names[TSR_DEBUG_VIEW_RESURRECTION_HITS], "Resurrection Hits");
	CHECK_EQ(names[TSR_DEBUG_VIEW_FLICKER_PERIOD], "Flicker Period");
	CHECK_EQ(names[TSR_DEBUG_VIEW_THIN_COVERAGE], "Thin Coverage");

	CHECK_EQ(int(TSR_DEBUG_VIEW_MAX), 6);
	CHECK_EQ(int(nextgi_tsr_debug_view_from_setting(0)), int(TSR_DEBUG_VIEW_HISTORY_VALIDITY));
	CHECK_EQ(int(nextgi_tsr_debug_view_from_setting(3)), int(TSR_DEBUG_VIEW_RESURRECTION_HITS));
	CHECK_EQ(int(nextgi_tsr_debug_view_from_setting(5)), int(TSR_DEBUG_VIEW_THIN_COVERAGE));
	// Out-of-range values are clamped.
	CHECK_EQ(int(nextgi_tsr_debug_view_from_setting(-4)), int(TSR_DEBUG_VIEW_HISTORY_VALIDITY));
	CHECK_EQ(int(nextgi_tsr_debug_view_from_setting(6)), int(TSR_DEBUG_VIEW_THIN_COVERAGE));
	CHECK_EQ(int(nextgi_tsr_debug_view_from_setting(1000)), int(TSR_DEBUG_VIEW_THIN_COVERAGE));

	// The visualizer only reads: the parameters default to off, and its texture lives outside
	// RB_SCOPE_TSR, so neither the feature mask, the pass plan nor the T6 accounting sees it.
	RendererRD::TSR::Parameters params;
	CHECK_EQ(params.debug_view, -1);
	CHECK(RB_SCOPE_TSR_DEBUG != RB_SCOPE_TSR);
	for (const RendererRD::TSRStabilityStoreEntry &entry : RendererRD::TSR_STABILITY_STORE_TABLE) {
		CHECK(StringName(entry.name) != SNAME("debug_view"));
	}
}

// GodotNext: pt_tsr_async_compute (#193).
namespace TestTSRAsync {

constexpr uint32_t ALL_PASSES = (1u << TSR_PASS_MAX) - 1u;

bool is_depth_velocity_pass(TSRPass p_pass) {
	return p_pass == TSR_PASS_VELOCITY_DILATION || p_pass == TSR_PASS_DISOCCLUSION;
}

// One frame as TSR::process() records it: per record point in order, the role and the passes.
struct FrameRecord {
	TSRPointRole roles[TSR_RECORD_MAX];
	TSRPointPasses passes[TSR_RECORD_MAX];
	TSRPass order[TSR_PASS_MAX] = {};
	uint32_t order_count = 0;
	uint32_t async_mask = 0;
};

FrameRecord record_frame(const TSRAsyncSchedule &p_schedule, uint32_t p_frame_passes) {
	FrameRecord frame;
	for (uint32_t point = 0; point < TSR_RECORD_MAX; point++) {
		frame.roles[point] = nextgi_tsr_point_role(p_schedule, TSRRecordPoint(point));
		frame.passes[point] = nextgi_tsr_point_passes(p_schedule, TSRRecordPoint(point), p_frame_passes);
		if (!frame.roles[point].runs()) {
			continue; // process() returns before recording anything.
		}
		for (uint32_t i = 0; i < TSR_PASS_MAX; i++) {
			if (frame.passes[point].records(TSRPass(i))) {
				frame.order[frame.order_count++] = TSRPass(i);
			}
		}
		frame.async_mask |= frame.passes[point].async_mask;
	}
	return frame;
}

// What every frame must do, at any level, with or without async compute: the once-per-frame work
// once, at the first point that records, before any pass; advance_frame() once, at the upscaler's
// place, which is the last point; every pass the frame runs recorded exactly once, at a point
// whose process() call runs, in TSRPass order; no other pass; async passes only among them.
void check_frame_invariants(const TSRAsyncSchedule &p_schedule, uint32_t p_frame_passes) {
	const FrameRecord frame = record_frame(p_schedule, p_frame_passes);
	uint32_t begins = 0;
	uint32_t ends = 0;
	int32_t first_running = -1;
	uint32_t recorded = 0;
	for (uint32_t point = 0; point < TSR_RECORD_MAX; point++) {
		CAPTURE(point);
		const TSRPointRole &role = frame.roles[point];
		const TSRPointPasses &passes = frame.passes[point];
		CHECK((passes.async_mask & ~passes.record_mask) == 0u);
		CHECK((passes.record_mask & ~p_frame_passes) == 0u);
		if (passes.record_mask != 0) {
			CHECK(role.runs());
		}
		if (role.runs() && first_running < 0) {
			first_running = int32_t(point);
		}
		begins += role.frame_begin ? 1 : 0;
		ends += role.frame_end ? 1 : 0;
		if (role.frame_begin) {
			CHECK_EQ(int32_t(point), first_running);
		}
		if (role.frame_end) {
			CHECK_EQ(point, uint32_t(TSR_RECORD_UPSCALE));
		}
		CHECK((recorded & passes.record_mask) == 0u);
		recorded |= passes.record_mask;
	}
	CHECK_EQ(begins, 1u);
	CHECK_EQ(ends, 1u);
	CHECK_EQ(recorded, p_frame_passes);
	for (uint32_t i = 1; i < frame.order_count; i++) {
		CHECK(frame.order[i - 1] < frame.order[i]);
	}
}

// The level-0 frame: one process() call at the upscaler's place that begins and ends the frame and
// records every pass the frame runs on the main queue, in TSRPass order.
void check_level_zero_frame(const TSRAsyncSchedule &p_schedule, uint32_t p_frame_passes) {
	CHECK_FALSE(p_schedule.split);
	CHECK_FALSE(p_schedule.uses_async());
	CHECK_EQ(p_schedule.first_point(), TSR_RECORD_UPSCALE);
	const FrameRecord frame = record_frame(p_schedule, p_frame_passes);
	CHECK_FALSE(frame.roles[TSR_RECORD_AFTER_MOTION_VECTORS].runs());
	CHECK_EQ(frame.passes[TSR_RECORD_AFTER_MOTION_VECTORS].record_mask, 0u);
	CHECK(frame.roles[TSR_RECORD_UPSCALE].frame_begin);
	CHECK(frame.roles[TSR_RECORD_UPSCALE].frame_end);
	CHECK_EQ(frame.passes[TSR_RECORD_UPSCALE].record_mask, p_frame_passes);
	CHECK_EQ(frame.async_mask, 0u);
}

} // namespace TestTSRAsync

TEST_CASE("[Upscalers][TSR] Async compute setting and the passes each level moves") {
	using namespace TestTSRAsync;

	// The setting follows r.TSR.AsyncCompute's four levels, default 2, read live.
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);
	const String setting = "rendering/anti_aliasing/tsr/async_compute";
	REQUIRE(ps->has_setting(setting));
	CHECK_EQ(int64_t(ps->get_setting(setting)), TSR_ASYNC_DEFAULT_LEVEL);
	CHECK_EQ(TSR_ASYNC_DEFAULT_LEVEL, int64_t(TSR_ASYNC_DEPTH_VELOCITY));
	REQUIRE(ps->get_custom_property_info().has(StringName(setting)));
	const PropertyInfo &info = ps->get_custom_property_info()[StringName(setting)];
	CHECK_EQ(int(info.type), int(Variant::INT));
	CHECK_EQ(int(info.hint), int(PROPERTY_HINT_ENUM));
	CHECK_EQ(info.hint_string.split(",").size(), int(TSR_ASYNC_MAX));

	// What each pass reads that this frame rendered decides the lowest level that moves it. No
	// pass reads only earlier frames, so level 1 moves nothing.
	CHECK_EQ(nextgi_tsr_pass_async_level(TSR_PASS_VELOCITY_DILATION), TSR_ASYNC_DEPTH_VELOCITY);
	CHECK_EQ(nextgi_tsr_pass_async_level(TSR_PASS_DISOCCLUSION), TSR_ASYNC_DEPTH_VELOCITY);
	for (uint32_t i = 0; i < TSR_PASS_MAX; i++) {
		CAPTURE(i);
		CHECK(nextgi_tsr_pass_async_level(TSRPass(i)) > TSR_ASYNC_HISTORY_CLEARS);
		if (!is_depth_velocity_pass(TSRPass(i))) {
			CHECK_EQ(nextgi_tsr_pass_async_level(TSRPass(i)), TSR_ASYNC_ALL);
		}
	}

	// The passes a frame runs: the five core passes always, each optional pass with its feature.
	const uint32_t core = nextgi_tsr_frame_passes(false, false, false, false, false);
	CHECK_EQ(core, nextgi_tsr_pass_bit(TSR_PASS_VELOCITY_DILATION) | nextgi_tsr_pass_bit(TSR_PASS_DISOCCLUSION) | nextgi_tsr_pass_bit(TSR_PASS_SHADING_REJECTION) | nextgi_tsr_pass_bit(TSR_PASS_HISTORY_UPDATE) | nextgi_tsr_pass_bit(TSR_PASS_RESOLVE));
	CHECK_EQ(nextgi_tsr_frame_passes(true, true, true, true, true), ALL_PASSES);
	CHECK_EQ(nextgi_tsr_frame_passes(true, false, false, false, false), core | nextgi_tsr_pass_bit(TSR_PASS_FLICKER_ANALYSIS));
	CHECK_EQ(nextgi_tsr_frame_passes(false, true, false, false, false), core | nextgi_tsr_pass_bit(TSR_PASS_THIN_ANALYSIS));
	CHECK_EQ(nextgi_tsr_frame_passes(false, false, true, false, false), core | nextgi_tsr_pass_bit(TSR_PASS_REJECTION_AA));
	CHECK_EQ(nextgi_tsr_frame_passes(false, false, false, true, false), core | nextgi_tsr_pass_bit(TSR_PASS_DEBUG_VIEW));
	CHECK_EQ(nextgi_tsr_frame_passes(false, false, false, false, true), core | nextgi_tsr_pass_bit(TSR_PASS_PERSISTENT_STORE));

	// The default parameters record every pass on the main queue in one call, as before #193.
	const RendererRD::TSR::Parameters params;
	CHECK_EQ(params.record_point, TSR_RECORD_UPSCALE);
	check_level_zero_frame(params.async_schedule, ALL_PASSES);

	// Every level, with and without async compute and the split, for every combination of the
	// optional passes: the frame invariants hold; without async compute, at level 1, and at level 2
	// without the split, the frame is level 0's.
	const int64_t levels[] = { -3, 0, 1, 2, 3, 9 };
	for (int64_t level : levels) {
		for (int split_allowed = 0; split_allowed < 2; split_allowed++) {
			for (uint32_t optional = 0; optional < 32; optional++) {
				CAPTURE(level);
				CAPTURE(split_allowed);
				CAPTURE(optional);
				const uint32_t frame_passes = nextgi_tsr_frame_passes((optional & 1) != 0, (optional & 2) != 0, (optional & 4) != 0, (optional & 8) != 0, (optional & 16) != 0);
				const TSRAsyncSchedule without = nextgi_tsr_async_schedule(level, false, split_allowed != 0);
				CHECK_EQ(without.level, uint32_t(CLAMP(level, int64_t(0), int64_t(3))));
				CHECK_FALSE(without.async_active);
				check_frame_invariants(without, frame_passes);
				check_level_zero_frame(without, frame_passes);

				const TSRAsyncSchedule with = nextgi_tsr_async_schedule(level, true, split_allowed != 0);
				CHECK(with.async_active);
				check_frame_invariants(with, frame_passes);
				if (with.level <= TSR_ASYNC_HISTORY_CLEARS || (with.level == TSR_ASYNC_DEPTH_VELOCITY && split_allowed == 0)) {
					check_level_zero_frame(with, frame_passes);
				}
			}
		}
	}

	// Level 2: velocity dilation and parallax disocclusion on the async queue at the frame's first
	// process() call, right after the motion vectors, which does the once-per-frame work; every
	// other pass on the main queue at the upscaler's place, which ends the frame.
	const TSRAsyncSchedule level2 = nextgi_tsr_async_schedule(2, true, true);
	CHECK(level2.split);
	CHECK(level2.uses_async());
	const FrameRecord frame2 = record_frame(level2, ALL_PASSES);
	CHECK(frame2.roles[TSR_RECORD_AFTER_MOTION_VECTORS].frame_begin);
	CHECK_FALSE(frame2.roles[TSR_RECORD_AFTER_MOTION_VECTORS].frame_end);
	CHECK_FALSE(frame2.roles[TSR_RECORD_UPSCALE].frame_begin);
	CHECK(frame2.roles[TSR_RECORD_UPSCALE].frame_end);
	for (uint32_t i = 0; i < TSR_PASS_MAX; i++) {
		CAPTURE(i);
		const bool early = is_depth_velocity_pass(TSRPass(i));
		CHECK_EQ(frame2.passes[TSR_RECORD_AFTER_MOTION_VECTORS].records(TSRPass(i)), early);
		CHECK_EQ(frame2.passes[TSR_RECORD_AFTER_MOTION_VECTORS].runs_async(TSRPass(i)), early);
		CHECK_EQ(frame2.passes[TSR_RECORD_UPSCALE].records(TSRPass(i)), !early);
		CHECK_FALSE(frame2.passes[TSR_RECORD_UPSCALE].runs_async(TSRPass(i)));
	}
	REQUIRE_EQ(frame2.order_count, uint32_t(TSR_PASS_MAX));
	for (uint32_t i = 0; i < TSR_PASS_MAX; i++) {
		CHECK_EQ(frame2.order[i], TSRPass(i));
	}

	// Level 3: every pass on the async queue, all at the upscaler's place (one fork-join window).
	for (int split_allowed = 0; split_allowed < 2; split_allowed++) {
		const TSRAsyncSchedule level3 = nextgi_tsr_async_schedule(3, true, split_allowed != 0);
		CHECK_FALSE(level3.split);
		CHECK_EQ(level3.first_point(), TSR_RECORD_UPSCALE);
		const FrameRecord frame3 = record_frame(level3, ALL_PASSES);
		CHECK_FALSE(frame3.roles[TSR_RECORD_AFTER_MOTION_VECTORS].runs());
		CHECK_EQ(frame3.passes[TSR_RECORD_UPSCALE].record_mask, ALL_PASSES);
		CHECK_EQ(frame3.passes[TSR_RECORD_UPSCALE].async_mask, ALL_PASSES);
	}
	CHECK_EQ(nextgi_tsr_async_schedule(9, true, true).level, uint32_t(TSR_ASYNC_ALL));
	CHECK(nextgi_tsr_point_passes(nextgi_tsr_async_schedule(9, true, true), TSR_RECORD_UPSCALE, ALL_PASSES).runs_async(TSR_PASS_RESOLVE));

	// Only the first single-view TSR viewport of an RD frame that uses the async queue gets it.
	CHECK(nextgi_tsr_async_available(true, 1, 7, UINT64_MAX));
	CHECK(nextgi_tsr_async_available(true, 1, 7, 6));
	CHECK_FALSE(nextgi_tsr_async_available(true, 1, 7, 7));
	CHECK_FALSE(nextgi_tsr_async_available(false, 1, 7, 6));
	CHECK_FALSE(nextgi_tsr_async_available(true, 2, 7, 6));
	CHECK_FALSE(nextgi_tsr_async_available(true, 2, 7, UINT64_MAX));
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
	CHECK(RenderSceneBuffersRD::is_output_sized_named_texture(RB_SCOPE_TSR, SNAME("persistent_frames")));
	CHECK(RenderSceneBuffersRD::is_output_sized_named_texture(RB_SCOPE_TSR_DEBUG, SNAME("debug_view")));
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
	CHECK_FALSE(RenderSceneBuffersRD::is_output_sized_named_texture(RB_SCOPE_TSR, SNAME("flicker_state_0")));
	CHECK_FALSE(RenderSceneBuffersRD::is_output_sized_named_texture(RB_SCOPE_TSR, SNAME("thin_coverage_0")));

	Ref<RenderSceneBuffersRD> rb;
	rb.instantiate();
	Ref<RendererSceneRenderImplementation::RenderForwardClustered::RenderBufferDataForwardClustered> rb_data;
	rb_data.instantiate();
	rb->set_custom_data(RB_SCOPE_FORWARD_CLUSTERED, rb_data);

	const ScopedDLSSUpscalerSlot clean_dlss;
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

// GodotNext: pt_temporal_upscaler_extension (#192), GT-1 subset.
namespace {

// Counts warnings, so tests can check "1 log line" for a fallback.
struct WarningCounter {
	ErrorHandlerList handler;
	int warnings = 0;

	static void _count(void *p_self, const char *p_func, const char *p_file, int p_line, const char *p_error, const char *p_errorexp, bool p_editor_notify, ErrorHandlerType p_type) {
		if (p_type == ERR_HANDLER_WARNING) {
			static_cast<WarningCounter *>(p_self)->warnings++;
		}
	}

	WarningCounter() {
		handler.errfunc = _count;
		handler.userdata = this;
		add_error_handler(&handler);
	}
	~WarningCounter() { remove_error_handler(&handler); }
};

struct TestCopyUpscalerState {
	String name = "TestCopy";
	bool available = true;
	Vector2 scale_range = Vector2(0.5f, 1.0f);
	float mip_bias_offset = -0.75f;
	int jitter_phases = 13;
	int process_calls = 0;
	int reset_calls = 0;
	Vector2i last_jitter_target;
	Vector2i last_jitter_render;
	Ref<TemporalUpscalerInputs> last_inputs;
};

// Answers the TemporalUpscalerExtension virtuals the way a script or
// GDExtension would, so every call goes through the GDVIRTUAL path.
class TestCopyUpscalerScriptInstance : public ScriptInstance {
	TestCopyUpscalerState *state = nullptr;

public:
	explicit TestCopyUpscalerScriptInstance(TestCopyUpscalerState *p_state) :
			state(p_state) {}

	bool set(const StringName &p_name, const Variant &p_value) override { return false; }
	bool get(const StringName &p_name, Variant &r_ret) const override { return false; }
	void get_property_list(List<PropertyInfo> *r_properties) const override {}
	Variant::Type get_property_type(const StringName &p_name, bool *r_is_valid) const override {
		if (r_is_valid) {
			*r_is_valid = false;
		}
		return Variant::NIL;
	}
	void validate_property(PropertyInfo &p_property) const override {}
	bool property_can_revert(const StringName &p_name) const override { return false; }
	bool property_get_revert(const StringName &p_name, Variant &r_ret) const override { return false; }
	void get_method_list(List<MethodInfo> *r_list) const override {}
	bool has_method(const StringName &p_method) const override {
		return p_method == SNAME("_get_name") || p_method == SNAME("_is_available") || p_method == SNAME("_get_scale_range") ||
				p_method == SNAME("_get_mip_bias_offset") || p_method == SNAME("_get_jitter_phase_count") || p_method == SNAME("_process") ||
				p_method == SNAME("_reset_history");
	}
	int get_method_argument_count(const StringName &p_method, bool *r_is_valid = nullptr) const override {
		if (r_is_valid) {
			*r_is_valid = has_method(p_method);
		}
		if (p_method == SNAME("_get_jitter_phase_count")) {
			return 2;
		}
		return p_method == SNAME("_process") ? 1 : 0;
	}
	Variant callp(const StringName &p_method, const Variant **p_args, int p_argcount, Callable::CallError &r_error) override {
		r_error.error = Callable::CallError::CALL_OK;
		if (p_method == SNAME("_get_name")) {
			return state->name;
		}
		if (p_method == SNAME("_is_available")) {
			return state->available;
		}
		if (p_method == SNAME("_get_scale_range")) {
			return state->scale_range;
		}
		if (p_method == SNAME("_get_mip_bias_offset")) {
			return state->mip_bias_offset;
		}
		if (p_method == SNAME("_get_jitter_phase_count") && p_argcount == 2) {
			state->last_jitter_target = *p_args[0];
			state->last_jitter_render = *p_args[1];
			return state->jitter_phases;
		}
		if (p_method == SNAME("_process") && p_argcount == 1) {
			state->process_calls++;
			state->last_inputs = *p_args[0];
			return Variant();
		}
		if (p_method == SNAME("_reset_history")) {
			state->reset_calls++;
			return Variant();
		}
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
		return Variant();
	}
	void notification(int p_notification, bool p_reversed = false) override {}
	Script *get_script() const override { return nullptr; }
	const Variant get_rpc_config() const override { return Variant(); }
	ScriptLanguage *get_language() override { return nullptr; }
};

// Test-only copy upscaler (#192 step 7). Its virtuals are answered through
// the GDVIRTUAL path, like the testbed's SampleCopy script.
class TestCopyUpscalerExtension : public TemporalUpscalerExtension {
	GDSOFTCLASS(TestCopyUpscalerExtension, TemporalUpscalerExtension);

public:
	TestCopyUpscalerState state;

	TestCopyUpscalerExtension() {
		set_script_instance(memnew(TestCopyUpscalerScriptInstance(&state)));
	}
};

const char *CUSTOM_UPSCALER_SETTING = "rendering/scaling_3d/custom_upscaler";

// Restores the registry and the selection setting when a test ends.
struct CustomUpscalerScope {
	String previous;

	explicit CustomUpscalerScope(const String &p_selected) {
		previous = GLOBAL_GET(CUSTOM_UPSCALER_SETTING);
		ProjectSettings::get_singleton()->set_setting(CUSTOM_UPSCALER_SETTING, p_selected);
		TemporalUpscalerRegistry::clear_extensions();
		TemporalUpscalerRegistry::reset_custom_warning_latches();
	}
	~CustomUpscalerScope() {
		TemporalUpscalerRegistry::clear_extensions();
		TemporalUpscalerRegistry::reset_custom_warning_latches();
		ProjectSettings::get_singleton()->set_setting(CUSTOM_UPSCALER_SETTING, previous);
	}
};

} // namespace

TEST_CASE("[Upscalers][Custom] Enum values, mode type and project setting") {
	CHECK_EQ(int(RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM), 8);
	CHECK_EQ(int(Viewport::SCALING_3D_MODE_CUSTOM), 8);
	CHECK_EQ(int(RSE::VIEWPORT_SCALING_3D_REASON_CUSTOM_UPSCALER_UNAVAILABLE), 12);
	CHECK(RSE::scaling_3d_mode_type(RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM) == RSE::VIEWPORT_SCALING_3D_TYPE_TEMPORAL);
	CHECK(TemporalUpscalerRegistry::build_mode_hint(false).contains("Custom:8"));
	REQUIRE(ProjectSettings::get_singleton()->has_setting(CUSTOM_UPSCALER_SETTING));
	CHECK_EQ(String(ProjectSettings::get_singleton()->property_get_revert(CUSTOM_UPSCALER_SETTING)), String());
}

TEST_CASE("[Upscalers][Custom] Virtuals dispatch through the GDVIRTUAL path and fall back to defaults") {
	Ref<TestCopyUpscalerExtension> ext;
	ext.instantiate();
	CHECK_EQ(ext->get_upscaler_name(), "TestCopy");
	CHECK(ext->is_available());
	CHECK(ext->get_scale_range().is_equal_approx(Vector2(0.5f, 1.0f)));
	CHECK(Math::is_equal_approx(ext->get_mip_bias_offset(), -0.75f));
	CHECK_EQ(ext->get_jitter_phase_count(Size2i(1920, 1080), Size2i(960, 540)), 13u);
	CHECK_EQ(ext->state.last_jitter_target, Vector2i(1920, 1080));
	CHECK_EQ(ext->state.last_jitter_render, Vector2i(960, 540));
	Ref<TemporalUpscalerInputs> inputs;
	inputs.instantiate();
	ext->process(inputs);
	ext->reset_history();
	CHECK_EQ(ext->state.process_calls, 1);
	CHECK_EQ(ext->state.reset_calls, 1);
	CHECK(ext->state.last_inputs == inputs);

	// A negative jitter phase count keeps the engine default.
	ext->state.jitter_phases = -1;
	CHECK_EQ(ext->get_jitter_phase_count(Size2i(2560, 1440), Size2i(1280, 720)), nextgi_temporal_jitter_phase_count(Size2i(2560, 1440), Size2i(1280, 720), true));

	// Inverted bounds are swapped, and a non-positive minimum is raised to 0.01.
	ext->state.scale_range = Vector2(1.0f, 0.5f);
	CHECK(ext->get_scale_range().is_equal_approx(Vector2(0.5f, 1.0f)));
	ext->state.scale_range = Vector2(-1.0f, 0.75f);
	CHECK(ext->get_scale_range().is_equal_approx(Vector2(0.01f, 0.75f)));

	// Without overrides: unavailable, TSR's range, -0.5 mip bias offset, default jitter.
	Ref<TemporalUpscalerExtension> plain;
	plain.instantiate();
	CHECK(plain->get_upscaler_name().is_empty());
	CHECK_FALSE(plain->is_available());
	CHECK(plain->get_scale_range().is_equal_approx(Vector2(0.25f, 2.0f)));
	CHECK(Math::is_equal_approx(plain->get_mip_bias_offset(), -0.5f));
	CHECK_EQ(plain->get_jitter_phase_count(Size2i(2560, 1440), Size2i(1280, 720)), nextgi_temporal_jitter_phase_count(Size2i(2560, 1440), Size2i(1280, 720), true));
}

TEST_CASE("[Upscalers][Custom] Inputs carry the §6.1 fields") {
	Ref<TemporalUpscalerInputs> inputs;
	inputs.instantiate();
	const RID color = RID::from_uint64(11);
	const RID output = RID::from_uint64(12);
	inputs->set_color(color);
	inputs->set_output(output);
	inputs->set_internal_size(Size2i(960, 540));
	inputs->set_output_size(Size2i(1920, 1080));
	inputs->set_jitter(Vector2(0.25f, -0.25f));
	inputs->set_z_near(0.05f);
	inputs->set_z_far(4000.0f);
	inputs->set_reset(true);
	inputs->set_view_index(1);
	CHECK(inputs->get_color() == color);
	CHECK(inputs->get_output() == output);
	CHECK_EQ(Size2i(inputs->get("internal_size")), Size2i(960, 540));
	CHECK_EQ(Size2i(inputs->get("output_size")), Size2i(1920, 1080));
	CHECK(Vector2(inputs->get("jitter")).is_equal_approx(Vector2(0.25f, -0.25f)));
	CHECK(Math::is_equal_approx(float(inputs->get("z_far")), 4000.0f));
	CHECK(bool(inputs->get("reset")));
	CHECK_EQ(int(inputs->get("view_index")), 1);
	for (const char *property : { "color", "depth", "velocity", "reactive", "output", "internal_size", "max_internal_size", "output_size", "motion_vector_scale", "jitter",
				 "world_to_view", "view_to_clip", "z_near", "z_far", "fovy", "delta_time", "reset", "view_index", "render_scene_buffers" }) {
		CHECK_MESSAGE(ClassDB::has_property("TemporalUpscalerInputs", property), property);
	}
}

TEST_CASE("[Upscalers][Custom] Register, select and unregister with a deferred free") {
	CustomUpscalerScope scope("TestCopy");
	Ref<TestCopyUpscalerExtension> ext;
	ext.instantiate();
	CHECK(TemporalUpscalerRegistry::get(RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM) == nullptr);
	CHECK_FALSE(TemporalUpscalerRegistry::is_custom_upscaler_available());
	const uint64_t revision = TemporalUpscalerRegistry::get_custom_upscaler_revision();

	TemporalUpscalerRegistry::register_extension(ext);
	CHECK(TemporalUpscalerRegistry::get_custom_upscaler_revision() > revision);
	TemporalUpscaler *adapter = TemporalUpscalerRegistry::get(RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM);
	REQUIRE(adapter != nullptr);
	CHECK(TemporalUpscalerRegistry::is_custom_upscaler_available());
	CHECK(TemporalUpscalerRegistry::get_selected_extension() == ext);
	CHECK_EQ(adapter->get_display_name(), "TestCopy");
	CHECK(adapter->get_scale_range().is_equal_approx(Vector2(0.5f, 1.0f)));
	CHECK(Math::is_equal_approx(adapter->mip_bias_offset(), -0.75f));
	CHECK_EQ(adapter->jitter_phase_count(Size2i(1920, 1080), Size2i(960, 540)), 13u);
	CHECK_FALSE(adapter->supports_multiview());

	// The setting selects by name; another name leaves CUSTOM empty.
	ProjectSettings::get_singleton()->set_setting(CUSTOM_UPSCALER_SETTING, "Other");
	TemporalUpscalerRegistry::sync_custom_upscaler();
	CHECK(TemporalUpscalerRegistry::get(RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM) == nullptr);
	CHECK(TemporalUpscalerRegistry::get_selected_extension().is_null());
	ProjectSettings::get_singleton()->set_setting(CUSTOM_UPSCALER_SETTING, "TestCopy");

	// _is_available false empties CUSTOM until it reports true again.
	ext->state.available = false;
	TemporalUpscalerRegistry::sync_custom_upscaler();
	CHECK(TemporalUpscalerRegistry::get(RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM) == nullptr);
	CHECK_FALSE(TemporalUpscalerRegistry::is_custom_upscaler_available());
	ext->state.available = true;
	TemporalUpscalerRegistry::sync_custom_upscaler();
	CHECK(TemporalUpscalerRegistry::get(RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM) == adapter);

	// Unregistering empties CUSTOM at once but keeps the extension alive until frame end.
	const int refs_registered = ext->get_reference_count();
	TemporalUpscalerRegistry::unregister_extension(ext);
	CHECK(TemporalUpscalerRegistry::get(RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM) == nullptr);
	CHECK_FALSE(TemporalUpscalerRegistry::is_custom_upscaler_available());
	CHECK_EQ(TemporalUpscalerRegistry::get_retired_extension_count(), 1u);
	CHECK_EQ(ext->get_reference_count(), refs_registered);
	TemporalUpscalerRegistry::flush_retired_extensions();
	CHECK_EQ(TemporalUpscalerRegistry::get_retired_extension_count(), 0u);
	CHECK_EQ(ext->get_reference_count(), 1);

	// Unregistering twice is a no-op; an unnamed extension is rejected.
	TemporalUpscalerRegistry::unregister_extension(ext);
	CHECK_EQ(TemporalUpscalerRegistry::get_retired_extension_count(), 0u);
	Ref<TemporalUpscalerExtension> unnamed;
	unnamed.instantiate();
	ERR_PRINT_OFF;
	TemporalUpscalerRegistry::register_extension(unnamed);
	ERR_PRINT_ON;
	CHECK_EQ(unnamed->get_reference_count(), 1);
}

TEST_CASE("[Upscalers][Custom] The CUSTOM slot only takes extensions") {
	CustomUpscalerScope scope("TestCopy");
	MockUpscaler mock;
	ERR_PRINT_OFF;
	TemporalUpscalerRegistry::register_upscaler(RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM, &mock);
	ERR_PRINT_ON;
	CHECK(TemporalUpscalerRegistry::get(RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM) == nullptr);
	CHECK_EQ(TemporalUpscalerRegistry::get_custom_upscaler_id(), 0u);

	// Ids are never reused, even when an adapter address could be.
	Ref<TestCopyUpscalerExtension> ext;
	ext.instantiate();
	TemporalUpscalerRegistry::register_extension(ext);
	const uint64_t first_id = TemporalUpscalerRegistry::get_custom_upscaler_id();
	CHECK(first_id != 0u);
	TemporalUpscalerRegistry::unregister_extension(ext);
	CHECK_EQ(TemporalUpscalerRegistry::get_custom_upscaler_id(), 0u);
	TemporalUpscalerRegistry::flush_retired_extensions();
	TemporalUpscalerRegistry::register_extension(ext);
	CHECK(TemporalUpscalerRegistry::get_custom_upscaler_id() > first_id);
	uint64_t id = 0;
	CHECK(TemporalUpscalerRegistry::get_custom_upscaler(&id) != nullptr);
	CHECK_EQ(id, TemporalUpscalerRegistry::get_custom_upscaler_id());
}

TEST_CASE("[Upscalers][Custom] Re-registering a name retires the previous extension") {
	CustomUpscalerScope scope("TestCopy");
	Ref<TestCopyUpscalerExtension> first;
	first.instantiate();
	Ref<TestCopyUpscalerExtension> second;
	second.instantiate();
	TemporalUpscalerRegistry::register_extension(first);
	TemporalUpscaler *first_adapter = TemporalUpscalerRegistry::get(RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM);
	REQUIRE(first_adapter != nullptr);
	TemporalUpscalerRegistry::register_extension(second);
	CHECK(TemporalUpscalerRegistry::get_selected_extension() == second);
	CHECK(TemporalUpscalerRegistry::get(RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM) != nullptr);
	CHECK_EQ(TemporalUpscalerRegistry::get_retired_extension_count(), 1u);
	TemporalUpscalerRegistry::flush_retired_extensions();
	CHECK_EQ(first->get_reference_count(), 1);
	CHECK(second->get_reference_count() > 1);

	// Re-registering an object under a new name replaces its old entry, so one
	// unregister releases it.
	second->state.name = "Renamed";
	TemporalUpscalerRegistry::register_extension(second);
	TemporalUpscalerRegistry::flush_retired_extensions();
	TemporalUpscalerRegistry::unregister_extension(second);
	TemporalUpscalerRegistry::flush_retired_extensions();
	CHECK_EQ(second->get_reference_count(), 1);
}

TEST_CASE("[Upscalers][Custom] Adapter forwards inputs and runs _reset_history once per reset") {
	CustomUpscalerScope scope("TestCopy");
	Ref<TestCopyUpscalerExtension> ext;
	ext.instantiate();
	TemporalUpscalerRegistry::register_extension(ext);
	TemporalUpscaler *adapter = TemporalUpscalerRegistry::get(RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM);
	REQUIRE(adapter != nullptr);

	RenderSceneBuffersRD *buffers = reinterpret_cast<RenderSceneBuffersRD *>(uintptr_t(1));
	CHECK_EQ(adapter->ensure_context(buffers, 0), RSE::VIEWPORT_SCALING_3D_REASON_NONE);
	// Multiview custom upscaling is out of scope for #192.
	CHECK_EQ(adapter->ensure_context(buffers, 1), RSE::VIEWPORT_SCALING_3D_REASON_CUSTOM_UPSCALER_UNAVAILABLE);

	TemporalUpscaler::Inputs inputs;
	inputs.color = RID::from_uint64(21);
	inputs.depth = RID::from_uint64(22);
	inputs.velocity = RID::from_uint64(23);
	inputs.reactive = RID::from_uint64(24);
	inputs.output = RID::from_uint64(25);
	inputs.internal_size = Size2i(1280, 720);
	inputs.max_internal_size = Size2i(1600, 900);
	inputs.output_size = Size2i(1920, 1080);
	inputs.motion_vector_scale = inputs.internal_size;
	inputs.jitter = Vector2(0.25f, -0.5f);
	inputs.z_near = 0.1f;
	inputs.z_far = 500.0f;
	inputs.fovy = 70.0f;
	inputs.delta_time = 1.0f / 60.0f;
	inputs.reset = true;
	Ref<RenderSceneBuffersRD> render_buffers;
	render_buffers.instantiate();
	adapter->process(render_buffers, 0, inputs);
	CHECK_EQ(ext->state.process_calls, 1);
	CHECK_EQ(ext->state.reset_calls, 1);
	const Ref<TemporalUpscalerInputs> seen = ext->state.last_inputs;
	REQUIRE(seen.is_valid());
	CHECK(seen->get_color() == inputs.color);
	CHECK(seen->get_depth() == inputs.depth);
	CHECK(seen->get_velocity() == inputs.velocity);
	CHECK(seen->get_reactive() == inputs.reactive);
	CHECK(seen->get_output() == inputs.output);
	CHECK_EQ(seen->get_internal_size(), Size2i(1280, 720));
	CHECK_EQ(seen->get_max_internal_size(), Size2i(1600, 900));
	CHECK_EQ(seen->get_output_size(), Size2i(1920, 1080));
	CHECK(seen->get_render_scene_buffers() == render_buffers);
	CHECK(seen->get_motion_vector_scale().is_equal_approx(Vector2(1280, 720)));
	CHECK(seen->get_jitter().is_equal_approx(Vector2(0.25f, -0.5f)));
	CHECK(Math::is_equal_approx(seen->get_z_far(), 500.0f));
	CHECK(Math::is_equal_approx(seen->get_fovy(), 70.0f));
	CHECK(seen->get_reset());
	CHECK_EQ(seen->get_view_index(), 0u);

	inputs.reset = false;
	adapter->process(Ref<RenderSceneBuffersRD>(), 0, inputs);
	adapter->process(Ref<RenderSceneBuffersRD>(), 0, inputs);
	CHECK_EQ(ext->state.process_calls, 3);
	CHECK_EQ(ext->state.reset_calls, 1);
	inputs.reset = true;
	adapter->process(Ref<RenderSceneBuffersRD>(), 0, inputs);
	CHECK_EQ(ext->state.reset_calls, 2);

	adapter->free_contexts(buffers);
}

TEST_CASE("[Upscalers][Custom] Selection keeps CUSTOM in range and falls back to TSR with one log line") {
	using R = RSE::ViewportScaling3DReason;
	const Vector2 range(0.5f, 1.0f);
	const CustomUpscalerResolution in_range = nextgi_resolve_custom_upscaler(true, 1, false, R::VIEWPORT_SCALING_3D_REASON_NONE, range, 0.75f);
	CHECK_EQ(in_range.mode, RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM);
	CHECK_EQ(in_range.reason, R::VIEWPORT_SCALING_3D_REASON_NONE);
	CHECK(Math::is_equal_approx(in_range.scale, 0.75f));
	CHECK_FALSE(in_range.scale_clamped);

	const CustomUpscalerResolution low = nextgi_resolve_custom_upscaler(true, 1, false, R::VIEWPORT_SCALING_3D_REASON_NONE, range, 0.25f);
	CHECK_EQ(low.mode, RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM);
	CHECK(Math::is_equal_approx(low.scale, 0.5f));
	CHECK(low.scale_clamped);
	const CustomUpscalerResolution high = nextgi_resolve_custom_upscaler(true, 1, false, R::VIEWPORT_SCALING_3D_REASON_NONE, range, 1.5f);
	CHECK(Math::is_equal_approx(high.scale, 1.0f));
	CHECK(high.scale_clamped);

	// No available extension or a latched context error resolve to TSR with
	// CUSTOM_UPSCALER_UNAVAILABLE; a stereo viewport the extension cannot run
	// resolves to TSR with VIEW_UNSUPPORTED.
	const CustomUpscalerResolution missing = nextgi_resolve_custom_upscaler(false, 1, false, R::VIEWPORT_SCALING_3D_REASON_NONE, range, 0.75f);
	CHECK_EQ(missing.mode, RSE::VIEWPORT_SCALING_3D_MODE_TSR);
	CHECK_EQ(missing.reason, R::VIEWPORT_SCALING_3D_REASON_CUSTOM_UPSCALER_UNAVAILABLE);
	CHECK(Math::is_equal_approx(missing.scale, 0.75f));
	const CustomUpscalerResolution stereo = nextgi_resolve_custom_upscaler(true, 2, false, R::VIEWPORT_SCALING_3D_REASON_NONE, range, 0.75f);
	CHECK_EQ(stereo.mode, RSE::VIEWPORT_SCALING_3D_MODE_TSR);
	CHECK_EQ(stereo.reason, R::VIEWPORT_SCALING_3D_REASON_VIEW_UNSUPPORTED);
	CHECK_EQ(nextgi_resolve_custom_upscaler(false, 2, false, R::VIEWPORT_SCALING_3D_REASON_NONE, range, 0.75f).reason, R::VIEWPORT_SCALING_3D_REASON_CUSTOM_UPSCALER_UNAVAILABLE);
	CHECK_EQ(nextgi_resolve_custom_upscaler(true, 2, true, R::VIEWPORT_SCALING_3D_REASON_NONE, range, 0.75f).mode, RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM);
	const CustomUpscalerResolution latched = nextgi_resolve_custom_upscaler(true, 1, false, R::VIEWPORT_SCALING_3D_REASON_CUSTOM_UPSCALER_UNAVAILABLE, range, 0.75f);
	CHECK_EQ(latched.mode, RSE::VIEWPORT_SCALING_3D_MODE_TSR);
	CHECK_EQ(latched.reason, R::VIEWPORT_SCALING_3D_REASON_CUSTOM_UPSCALER_UNAVAILABLE);

	// One log line per fallback kind, however many viewports or frames hit it.
	CustomUpscalerScope scope("TestCopy");
	{
		WarningCounter counter;
		for (int i = 0; i < 3; i++) {
			TemporalUpscalerRegistry::report_custom_unavailable();
		}
		CHECK_EQ(counter.warnings, 1);
		for (int i = 0; i < 3; i++) {
			TemporalUpscalerRegistry::report_custom_scale_clamped("TestCopy", range);
		}
		CHECK_EQ(counter.warnings, 2);
	}
}

TEST_CASE("[Upscalers][Custom] RenderSceneBuffersRD applies the extension mip bias and drops the fast path after a swap") {
	CustomUpscalerScope scope("TestCopy");
	Ref<TestCopyUpscalerExtension> ext;
	ext.instantiate();
	TemporalUpscalerRegistry::register_extension(ext);

	Ref<RenderSceneBuffersRD> rb;
	rb.instantiate();
	Ref<RendererSceneRenderImplementation::RenderForwardClustered::RenderBufferDataForwardClustered> rb_data;
	rb_data.instantiate();
	rb->set_custom_data(RB_SCOPE_FORWARD_CLUSTERED, rb_data);

	const float scale_bias = Math::log2(0.75f);
	RenderSceneBuffersConfiguration cfg;
	cfg.set_internal_size(Size2i(1440, 810));
	cfg.set_max_internal_size(Size2i(1440, 810));
	cfg.set_min_internal_size(Size2i(960, 540));
	cfg.set_target_size(Size2i(1920, 1080));
	cfg.set_view_count(1);
	cfg.set_scaling_3d_mode(RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM);
	cfg.set_texture_mipmap_bias(scale_bias);
	rb->configure(&cfg);
	CHECK(Math::is_equal_approx(rb->get_computed_mipmap_bias(), scale_bias - 0.75f));

	// Re-registering the same extension re-reads its mip bias offset: the fast
	// path is refused so the samplers get the new bias.
	ext->state.mip_bias_offset = -0.25f;
	TemporalUpscalerRegistry::register_extension(ext);
	CHECK_FALSE(rb->can_configure_internal_only(&cfg));
	rb->configure(&cfg);
	CHECK(Math::is_equal_approx(rb->get_computed_mipmap_bias(), scale_bias - 0.25f));

	// The same extension keeps the internal-only fast path; a new adapter does not.
	cfg.set_internal_size(Size2i(1200, 675));
	CHECK(rb->can_configure_internal_only(&cfg));
	Ref<TestCopyUpscalerExtension> replacement;
	replacement.instantiate();
	TemporalUpscalerRegistry::register_extension(replacement);
	CHECK_FALSE(rb->can_configure_internal_only(&cfg));
	rb->configure(&cfg);
	CHECK(rb->can_configure_internal_only(&cfg));

	rb->cleanup();
	TemporalUpscalerRegistry::flush_retired_extensions();
	CHECK_EQ(ext->get_reference_count(), 1);
}

TEST_CASE("[Upscalers][Custom] RenderingServer API, availability and 100 register/unregister cycles") {
	RenderingServer *rs = RenderingServer::get_singleton();
	REQUIRE(rs != nullptr);
	CustomUpscalerScope scope("TestCopy");
	CHECK_EQ(rs->scaling_3d_mode_get_availability(RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM), RSE::VIEWPORT_SCALING_3D_REASON_CUSTOM_UPSCALER_UNAVAILABLE);

	Ref<TestCopyUpscalerExtension> ext;
	ext.instantiate();
	rs->register_temporal_upscaler_extension(ext);
	CHECK_EQ(rs->scaling_3d_mode_get_availability(RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM), RSE::VIEWPORT_SCALING_3D_REASON_NONE);
	ext->state.available = false;
	CHECK_EQ(rs->scaling_3d_mode_get_availability(RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM), RSE::VIEWPORT_SCALING_3D_REASON_CUSTOM_UPSCALER_UNAVAILABLE);
	ext->state.available = true;

	const RID camera = rs->camera_create();
	const RID viewport = rs->viewport_create();
	rs->viewport_attach_camera(viewport, camera);
	rs->viewport_set_size(viewport, 640, 360, 1);
	rs->viewport_set_active(viewport, true);
	rs->viewport_set_scaling_3d_mode(viewport, RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM);
	rs->draw(false, 0.0);
	const Dictionary status = rs->viewport_get_scaling_3d_status(viewport);
	CHECK_EQ(int(status["requested_mode"]), int(RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM));
	// The dummy rasterizer creates no RD render buffers: nothing is configured
	// and no sampler bias applies. The testbed scene checks the values on Metal.
	CHECK_EQ(int(status["jitter_phase_count"]), 0);
	CHECK(Math::is_zero_approx(float(status["mipmap_bias"])));

	// Outside a frame, the server releases an unregistered extension at once.
	const int registered_refs = ext->get_reference_count();
	for (int i = 0; i < 100; i++) {
		rs->unregister_temporal_upscaler_extension(ext);
		CHECK_EQ(TemporalUpscalerRegistry::get_retired_extension_count(), 0u);
		// Only this test's reference is left: the registry entry and its adapter are gone.
		CHECK_EQ(ext->get_reference_count(), 1);
		rs->register_temporal_upscaler_extension(ext);
		rs->draw(false, 0.0);
	}
	CHECK_EQ(ext->get_reference_count(), registered_refs);
	CHECK(TemporalUpscalerRegistry::get(RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM) != nullptr);

	rs->unregister_temporal_upscaler_extension(ext);
	CHECK_EQ(ext->get_reference_count(), 1);
	CHECK(TemporalUpscalerRegistry::get(RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM) == nullptr);
	rs->free_rid(viewport);
	rs->free_rid(camera);
}
