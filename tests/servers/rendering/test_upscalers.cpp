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

#include "scene/main/viewport.h"
#include "servers/rendering/renderer_rd/effects/temporal_upscaler.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_enums.h"

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
	// GodotNext: pu_dlss_scaling_mode (#103), GU-1.
	CHECK_EQ(int(RSE::VIEWPORT_SCALING_3D_MODE_NEAREST), 5);
	CHECK_EQ(int(RSE::VIEWPORT_SCALING_3D_MODE_DLSS), 6);
	CHECK_EQ(int(RSE::VIEWPORT_SCALING_3D_MODE_MAX), 7);
	CHECK_EQ(int(Viewport::SCALING_3D_MODE_DLSS), 6);
	CHECK_EQ(int(Viewport::SCALING_3D_MODE_MAX), 7);
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
