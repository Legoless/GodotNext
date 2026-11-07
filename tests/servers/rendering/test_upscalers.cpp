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

	String get_display_name() const override { return "Mock DLSS"; }
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
