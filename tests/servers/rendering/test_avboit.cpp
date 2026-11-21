/**************************************************************************/
/*  test_avboit.cpp                                                       */
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

TEST_FORCE_LINK(test_avboit)

#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "servers/rendering/avboit_policy.h"
#include "servers/rendering/renderer_rd/effects/avboit.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_forward_clustered.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"

// GodotNext: avboit (#295, plan §21.2.1).

namespace {

using AVBOITRD = RendererRD::AVBOIT;
using BlendMode = RendererRD::MaterialStorage::ShaderData::BlendMode;

AVBOITPolicy::ViewInputs _avboit_inputs() {
	AVBOITPolicy::ViewInputs inputs;
	inputs.method = AVBOITPolicy::METHOD_AVBOIT;
	inputs.renderer = AVBOITPolicy::RENDERER_FORWARD_PLUS;
	inputs.device_features = true;
	inputs.msaa = false;
	inputs.view_count = 1;
	return inputs;
}

// One translucent layer of a pixel, for the CPU model of the passes.
struct Layer {
	float depth;
	float alpha;
	float color;
};

// The sorted pass: back to front with the mix blend.
float _sorted(const Layer *p_layers, int p_count, float p_background) {
	float color = p_background;
	for (int i = 0; i < p_count; i++) {
		color = p_layers[i].color * p_layers[i].alpha + color * (1.0f - p_layers[i].alpha);
	}
	return color;
}

// The AVBOIT passes on one cell with every layer covering it, drawn in p_order: the bounds and
// occlusion pre-passes, the composite's blend and the resolve, as the shaders and blend states do.
float _avboit(const Layer *p_layers, int p_count, const int *p_order, float p_background) {
	float nearest = AVBOITRD::BOUNDS_CLEAR_NEAR;
	float farthest = 0.0f;
	for (int i = 0; i < p_count; i++) {
		nearest = MIN(nearest, p_layers[p_order[i]].depth);
		farthest = MAX(farthest, p_layers[p_order[i]].depth);
	}
	float slices[AVBOITRD::SLICES] = {};
	for (int i = 0; i < p_count; i++) {
		const Layer &layer = p_layers[p_order[i]];
		slices[AVBOITRD::slice(layer.depth, nearest, farthest)] += AVBOITRD::optical_depth(layer.alpha);
	}
	float sum = 0.0f;
	float coverage = 0.0f;
	for (int i = 0; i < p_count; i++) {
		const Layer &layer = p_layers[p_order[i]];
		const float t = AVBOITRD::transmittance_front(slices, nearest, farthest, layer.depth, AVBOITRD::optical_depth(layer.alpha));
		sum += layer.color * layer.alpha * t;
		coverage = layer.alpha + coverage * (1.0f - layer.alpha);
	}
	return sum + p_background * (1.0f - coverage);
}

} // namespace

TEST_CASE("[AVBOIT] The transparency method setting is registered and Sorted by default") {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	REQUIRE(settings != nullptr);
	REQUIRE(settings->has_setting(AVBOITPolicy::SETTING_METHOD));
	CHECK_EQ(String(AVBOITPolicy::SETTING_METHOD), "rendering/transparency/method");
	CHECK_EQ(int(settings->property_get_revert(AVBOITPolicy::SETTING_METHOD)), int(AVBOITPolicy::METHOD_SORTED));
	CHECK_EQ(AVBOITPolicy::method_from_setting(0), AVBOITPolicy::METHOD_SORTED);
	CHECK_EQ(AVBOITPolicy::method_from_setting(1), AVBOITPolicy::METHOD_AVBOIT);
	// Out-of-range values keep the default.
	CHECK_EQ(AVBOITPolicy::method_from_setting(2), AVBOITPolicy::METHOD_SORTED);
	CHECK_EQ(AVBOITPolicy::method_from_setting(-1), AVBOITPolicy::METHOD_SORTED);
}

TEST_CASE("[AVBOIT] Every fallback reason, in the order they are checked") {
	AVBOITPolicy::ViewInputs inputs = _avboit_inputs();
	CHECK_EQ(AVBOITPolicy::evaluate(inputs), AVBOITPolicy::REASON_NONE);

	inputs = _avboit_inputs();
	inputs.method = AVBOITPolicy::METHOD_SORTED;
	CHECK_EQ(AVBOITPolicy::evaluate(inputs), AVBOITPolicy::REASON_SETTING);

	inputs = _avboit_inputs();
	inputs.renderer = AVBOITPolicy::RENDERER_MOBILE;
	CHECK_EQ(AVBOITPolicy::evaluate(inputs), AVBOITPolicy::REASON_MOBILE_RENDERER);

	inputs = _avboit_inputs();
	inputs.renderer = AVBOITPolicy::RENDERER_COMPATIBILITY;
	CHECK_EQ(AVBOITPolicy::evaluate(inputs), AVBOITPolicy::REASON_COMPATIBILITY_RENDERER);

	inputs = _avboit_inputs();
	inputs.device_features = false;
	CHECK_EQ(AVBOITPolicy::evaluate(inputs), AVBOITPolicy::REASON_DEVICE_FEATURES);

	inputs = _avboit_inputs();
	inputs.msaa = true;
	CHECK_EQ(AVBOITPolicy::evaluate(inputs), AVBOITPolicy::REASON_MSAA);

	inputs = _avboit_inputs();
	inputs.view_count = 2;
	CHECK_EQ(AVBOITPolicy::evaluate(inputs), AVBOITPolicy::REASON_MULTIVIEW);

	// The setting wins over everything, the renderer over the device, the device over the view.
	inputs.method = AVBOITPolicy::METHOD_SORTED;
	inputs.renderer = AVBOITPolicy::RENDERER_MOBILE;
	inputs.device_features = false;
	inputs.msaa = true;
	CHECK_EQ(AVBOITPolicy::evaluate(inputs), AVBOITPolicy::REASON_SETTING);
	inputs.method = AVBOITPolicy::METHOD_AVBOIT;
	CHECK_EQ(AVBOITPolicy::evaluate(inputs), AVBOITPolicy::REASON_MOBILE_RENDERER);
	inputs.renderer = AVBOITPolicy::RENDERER_FORWARD_PLUS;
	CHECK_EQ(AVBOITPolicy::evaluate(inputs), AVBOITPolicy::REASON_DEVICE_FEATURES);
	inputs.device_features = true;
	CHECK_EQ(AVBOITPolicy::evaluate(inputs), AVBOITPolicy::REASON_MSAA);

	// Each reason has a name and a line the checks can find.
	CHECK(AVBOITPolicy::describe(AVBOITPolicy::REASON_NONE).begins_with("AVBOIT: on (EXPERIMENTAL)"));
	for (int reason = AVBOITPolicy::REASON_SETTING; reason < AVBOITPolicy::REASON_MAX; reason++) {
		const AVBOITPolicy::Reason r = AVBOITPolicy::Reason(reason);
		CHECK_NE(String(AVBOITPolicy::reason_name(r)), "UNKNOWN");
		CHECK(AVBOITPolicy::describe(r).begins_with(vformat("AVBOIT: sorted (reason %s)", AVBOITPolicy::reason_name(r))));
	}
	CHECK_EQ(String(AVBOITPolicy::reason_name(AVBOITPolicy::REASON_MSAA)), "MSAA");
	CHECK_EQ(String(AVBOITPolicy::reason_name(AVBOITPolicy::REASON_MOBILE_RENDERER)), "MOBILE_RENDERER");
	CHECK_EQ(String(AVBOITPolicy::reason_name(AVBOITPolicy::REASON_COMPATIBILITY_RENDERER)), "COMPATIBILITY_RENDERER");
	CHECK_EQ(String(AVBOITPolicy::reason_name(AVBOITPolicy::REASON_DEVICE_FEATURES)), "DEVICE_FEATURES");
}

TEST_CASE("[AVBOIT] The device features AVBOIT needs") {
	AVBOITPolicy::DeviceFeatures features;
	features.rgba16f_attachment = true;
	features.rgba16f_storage = true;
	features.max_color_attachments = AVBOITPolicy::OCCLUSION_LAYERS;
	CHECK(AVBOITPolicy::device_features_met(features));
	features.max_color_attachments = 8;
	CHECK(AVBOITPolicy::device_features_met(features));

	AVBOITPolicy::DeviceFeatures missing = features;
	missing.rgba16f_attachment = false;
	CHECK_FALSE(AVBOITPolicy::device_features_met(missing));
	missing = features;
	missing.rgba16f_storage = false;
	CHECK_FALSE(AVBOITPolicy::device_features_met(missing));
	missing = features;
	missing.max_color_attachments = AVBOITPolicy::OCCLUSION_LAYERS - 1;
	CHECK_FALSE(AVBOITPolicy::device_features_met(missing));
	CHECK_FALSE(AVBOITPolicy::device_features_met(AVBOITPolicy::DeviceFeatures()));

	// Without a device nothing is reported, so the view keeps sorted translucency.
	CHECK_FALSE(AVBOITPolicy::device_features_met(AVBOITRD::query_device_features(nullptr)));
	CHECK_EQ(AVBOITRD::LAYERS, AVBOITPolicy::OCCLUSION_LAYERS);
	CHECK_EQ(AVBOITRD::SLICES, AVBOITRD::LAYERS * 4);
}

TEST_CASE("[AVBOIT] Each reason is printed once until the method changes") {
	AVBOITPolicy::ReasonLog log;
	// The default prints nothing.
	CHECK(log.note(AVBOITPolicy::METHOD_SORTED, AVBOITPolicy::REASON_SETTING).is_empty());
	CHECK(log.note(AVBOITPolicy::METHOD_SORTED, AVBOITPolicy::REASON_SETTING).is_empty());

	const String on = log.note(AVBOITPolicy::METHOD_AVBOIT, AVBOITPolicy::REASON_NONE);
	CHECK(on.begins_with("AVBOIT: on"));
	CHECK(log.note(AVBOITPolicy::METHOD_AVBOIT, AVBOITPolicy::REASON_NONE).is_empty());
	// Two views, one with MSAA, alternate every frame: each line once.
	CHECK(log.note(AVBOITPolicy::METHOD_AVBOIT, AVBOITPolicy::REASON_MSAA).begins_with("AVBOIT: sorted (reason MSAA)"));
	CHECK(log.note(AVBOITPolicy::METHOD_AVBOIT, AVBOITPolicy::REASON_NONE).is_empty());
	CHECK(log.note(AVBOITPolicy::METHOD_AVBOIT, AVBOITPolicy::REASON_MSAA).is_empty());

	// Back to sorted: said once. AVBOIT again: everything is said again.
	CHECK(log.note(AVBOITPolicy::METHOD_SORTED, AVBOITPolicy::REASON_SETTING).begins_with("AVBOIT: sorted (reason SETTING)"));
	CHECK(log.note(AVBOITPolicy::METHOD_SORTED, AVBOITPolicy::REASON_SETTING).is_empty());
	CHECK_FALSE(log.note(AVBOITPolicy::METHOD_AVBOIT, AVBOITPolicy::REASON_NONE).is_empty());
	CHECK_FALSE(log.note(AVBOITPolicy::METHOD_AVBOIT, AVBOITPolicy::REASON_MSAA).is_empty());

	// The Mobile and Compatibility renderers say theirs once.
	AVBOITPolicy::ReasonLog mobile;
	AVBOITPolicy::ViewInputs inputs = _avboit_inputs();
	inputs.renderer = AVBOITPolicy::RENDERER_MOBILE;
	CHECK(mobile.note(inputs.method, AVBOITPolicy::evaluate(inputs)).begins_with("AVBOIT: sorted (reason MOBILE_RENDERER)"));
	CHECK(mobile.note(inputs.method, AVBOITPolicy::evaluate(inputs)).is_empty());
	AVBOITPolicy::ReasonLog compatibility;
	inputs.renderer = AVBOITPolicy::RENDERER_COMPATIBILITY;
	CHECK(compatibility.note(inputs.method, AVBOITPolicy::evaluate(inputs)).begins_with("AVBOIT: sorted (reason COMPATIBILITY_RENDERER)"));
}

TEST_CASE("[AVBOIT] Which pass draws which blend mode") {
	// Mix, premultiplied alpha and alpha to coverage occlude; add and subtract only add light; the
	// rest (mul, blending disabled) is drawn sorted after the resolve.
	CHECK_EQ(AVBOITRD::surface_class(BlendMode::BLEND_MODE_MIX), AVBOITRD::SURFACE_OCCLUDING);
	CHECK_EQ(AVBOITRD::surface_class(BlendMode::BLEND_MODE_PREMULTIPLIED_ALPHA), AVBOITRD::SURFACE_OCCLUDING);
	CHECK_EQ(AVBOITRD::surface_class(BlendMode::BLEND_MODE_ALPHA_TO_COVERAGE), AVBOITRD::SURFACE_OCCLUDING);
	CHECK_EQ(AVBOITRD::surface_class(BlendMode::BLEND_MODE_ADD), AVBOITRD::SURFACE_UNRECORDED);
	CHECK_EQ(AVBOITRD::surface_class(BlendMode::BLEND_MODE_SUB), AVBOITRD::SURFACE_UNRECORDED);
	CHECK_EQ(AVBOITRD::surface_class(BlendMode::BLEND_MODE_MUL), AVBOITRD::SURFACE_REMAINDER);
	CHECK_EQ(AVBOITRD::surface_class(BlendMode::BLEND_MODE_DISABLED), AVBOITRD::SURFACE_REMAINDER);
	// shadow_to_opacity takes its alpha from lighting, which the pre-passes do not run: a surface that
	// would occlude is composited without being recorded. The others keep their class.
	CHECK_EQ(AVBOITRD::surface_class(BlendMode::BLEND_MODE_MIX, true), AVBOITRD::SURFACE_UNRECORDED);
	CHECK_EQ(AVBOITRD::surface_class(BlendMode::BLEND_MODE_PREMULTIPLIED_ALPHA, true), AVBOITRD::SURFACE_UNRECORDED);
	CHECK_EQ(AVBOITRD::surface_class(BlendMode::BLEND_MODE_ALPHA_TO_COVERAGE, true), AVBOITRD::SURFACE_UNRECORDED);
	CHECK_EQ(AVBOITRD::surface_class(BlendMode::BLEND_MODE_ADD, true), AVBOITRD::SURFACE_UNRECORDED);
	CHECK_EQ(AVBOITRD::surface_class(BlendMode::BLEND_MODE_MUL, true), AVBOITRD::SURFACE_REMAINDER);
	CHECK_EQ(AVBOITRD::surface_class(BlendMode::BLEND_MODE_DISABLED, true), AVBOITRD::SURFACE_REMAINDER);

	const BlendMode modes[] = { BlendMode::BLEND_MODE_MIX, BlendMode::BLEND_MODE_ADD, BlendMode::BLEND_MODE_SUB, BlendMode::BLEND_MODE_MUL, BlendMode::BLEND_MODE_ALPHA_TO_COVERAGE, BlendMode::BLEND_MODE_PREMULTIPLIED_ALPHA, BlendMode::BLEND_MODE_DISABLED };
	for (const bool shadow_to_opacity : { false, true }) {
		for (const BlendMode mode : modes) {
			const AVBOITRD::SurfaceClass surface = AVBOITRD::surface_class(mode, shadow_to_opacity);
			// Every surface is drawn by exactly one of the composite and the remainder pass.
			CHECK_NE(AVBOITRD::pass_draws(AVBOITRD::PASS_COMPOSITE, mode, shadow_to_opacity), AVBOITRD::pass_draws(AVBOITRD::PASS_REMAINDER, mode, shadow_to_opacity));
			// The pre-passes record exactly the occluding ones, and both record the same set.
			CHECK_EQ(AVBOITRD::pass_draws(AVBOITRD::PASS_BOUNDS, mode, shadow_to_opacity), surface == AVBOITRD::SURFACE_OCCLUDING);
			CHECK_EQ(AVBOITRD::pass_draws(AVBOITRD::PASS_OCCLUSION, mode, shadow_to_opacity), surface == AVBOITRD::SURFACE_OCCLUDING);
			CHECK(AVBOITRD::pass_draws(AVBOITRD::PASS_NONE, mode, shadow_to_opacity));
			// The composite weights what it draws and nothing else.
			const AVBOITRD::CompositeMode composite = AVBOITRD::composite_mode(mode, shadow_to_opacity);
			CHECK_EQ(composite != AVBOITRD::COMPOSITE_OFF, AVBOITRD::pass_draws(AVBOITRD::PASS_COMPOSITE, mode, shadow_to_opacity));
			// The GLSL takes a surface's own optical depth off its slice for modes 1 and 2 only
			// (avboit_mode <= 2u): exactly the recorded ones.
			if (composite != AVBOITRD::COMPOSITE_OFF) {
				CHECK_EQ(uint32_t(composite) <= 2u, surface == AVBOITRD::SURFACE_OCCLUDING);
			}
			// The mode fits the three specialization bits.
			CHECK_LE(uint32_t(composite), 7u);
		}
	}
	CHECK_EQ(AVBOITRD::composite_mode(BlendMode::BLEND_MODE_MIX), AVBOITRD::COMPOSITE_MIX);
	CHECK_EQ(AVBOITRD::composite_mode(BlendMode::BLEND_MODE_ALPHA_TO_COVERAGE), AVBOITRD::COMPOSITE_MIX);
	CHECK_EQ(AVBOITRD::composite_mode(BlendMode::BLEND_MODE_PREMULTIPLIED_ALPHA), AVBOITRD::COMPOSITE_PREMULTIPLIED);
	CHECK_EQ(AVBOITRD::composite_mode(BlendMode::BLEND_MODE_ADD), AVBOITRD::COMPOSITE_ADD);
	CHECK_EQ(AVBOITRD::composite_mode(BlendMode::BLEND_MODE_SUB), AVBOITRD::COMPOSITE_SUBTRACT);
	CHECK_EQ(AVBOITRD::composite_mode(BlendMode::BLEND_MODE_MIX, true), AVBOITRD::COMPOSITE_MIX_UNRECORDED);
	CHECK_EQ(AVBOITRD::composite_mode(BlendMode::BLEND_MODE_ALPHA_TO_COVERAGE, true), AVBOITRD::COMPOSITE_MIX_UNRECORDED);
	CHECK_EQ(AVBOITRD::composite_mode(BlendMode::BLEND_MODE_PREMULTIPLIED_ALPHA, true), AVBOITRD::COMPOSITE_PREMULTIPLIED_UNRECORDED);
	CHECK_EQ(AVBOITRD::composite_mode(BlendMode::BLEND_MODE_ADD, true), AVBOITRD::COMPOSITE_ADD);
	CHECK_EQ(AVBOITRD::composite_mode(BlendMode::BLEND_MODE_SUB, true), AVBOITRD::COMPOSITE_SUBTRACT);
	// The GLSL's coverage (avboit_mode <= 2u || avboit_mode >= 5u) and its premultiplied weighting
	// (avboit_mode == 2u || avboit_mode == 6u).
	CHECK_LE(uint32_t(AVBOITRD::COMPOSITE_PREMULTIPLIED), 2u);
	CHECK_GT(uint32_t(AVBOITRD::COMPOSITE_ADD), 2u);
	CHECK_LT(uint32_t(AVBOITRD::COMPOSITE_SUBTRACT), 5u);
	CHECK_EQ(uint32_t(AVBOITRD::COMPOSITE_MIX_UNRECORDED), 5u);
	CHECK_EQ(uint32_t(AVBOITRD::COMPOSITE_PREMULTIPLIED_UNRECORDED), 6u);
}

TEST_CASE("[AVBOIT] The grid, its adaptive slices and the transmittance in front") {
	CHECK_EQ(AVBOITRD::grid_size(Size2i(1920, 1080)), Size2i(480, 270));
	CHECK_EQ(AVBOITRD::grid_size(Size2i(1281, 721)), Size2i(321, 181));
	CHECK_EQ(AVBOITRD::grid_size(Size2i(1, 1)), Size2i(1, 1));
	CHECK_EQ(AVBOITRD::cell_of_pixel(Vector2(0.5, 0.5), Size2i(1920, 1080), Size2i(480, 270)), Vector2i(0, 0));
	CHECK_EQ(AVBOITRD::cell_of_pixel(Vector2(5.5, 9.5), Size2i(1920, 1080), Size2i(480, 270)), Vector2i(1, 2));
	CHECK_EQ(AVBOITRD::cell_of_pixel(Vector2(1919.5, 1079.5), Size2i(1920, 1080), Size2i(480, 270)), Vector2i(479, 269));
	// A cell's center: two pixels in for a 4-pixel cell, and in the cell it belongs to.
	CHECK(AVBOITRD::cell_center(Vector2i(1, 2), Size2i(1920, 1080), Size2i(480, 270)).is_equal_approx(Vector2(6.0, 10.0)));
	const Vector2 odd_center = AVBOITRD::cell_center(Vector2i(320, 180), Size2i(1281, 721), Size2i(321, 181));
	CHECK_EQ(AVBOITRD::cell_of_pixel(odd_center, Size2i(1281, 721), Size2i(321, 181)), Vector2i(320, 180));

	CHECK(Math::is_equal_approx(AVBOITRD::optical_depth(0.5f), float(Math::LN2)));
	CHECK_EQ(AVBOITRD::optical_depth(0.0f), 0.0f);
	CHECK(Math::is_equal_approx(AVBOITRD::optical_depth(1.0f), -Math::log(1.0f - AVBOITRD::MAX_ALPHA)));

	// Logarithmic slices between the cell's nearest and farthest depth.
	CHECK_EQ(AVBOITRD::slice(2.0f, 2.0f, 8.0f), 0u);
	CHECK_EQ(AVBOITRD::slice(8.0f, 2.0f, 8.0f), AVBOITRD::SLICES - 1);
	// 4.1 m is a little past the middle of 2 to 8 m on a log scale.
	CHECK_EQ(AVBOITRD::slice(4.1f, 2.0f, 8.0f), AVBOITRD::SLICES / 2);
	CHECK_EQ(AVBOITRD::slice(1.0f, 2.0f, 8.0f), 0u);
	CHECK_EQ(AVBOITRD::slice(100.0f, 2.0f, 8.0f), AVBOITRD::SLICES - 1);
	// One depth in the cell: everything is slice 0.
	CHECK_EQ(AVBOITRD::slice(3.0f, 3.0f, 3.0f), 0u);

	float slices[AVBOITRD::SLICES] = {};
	// An empty cell lets everything through.
	CHECK_EQ(AVBOITRD::transmittance_front(slices, AVBOITRD::BOUNDS_CLEAR_NEAR, 0.0f, 5.0f, 0.0f), 1.0f);
	// Two layers: the near one sees nothing in front of it, the far one sees the near one.
	slices[0] = AVBOITRD::optical_depth(0.5f);
	slices[AVBOITRD::SLICES - 1] = AVBOITRD::optical_depth(0.25f);
	CHECK(Math::is_equal_approx(AVBOITRD::transmittance_front(slices, 2.0f, 8.0f, 2.0f, AVBOITRD::optical_depth(0.5f)), 1.0f));
	CHECK(Math::is_equal_approx(AVBOITRD::transmittance_front(slices, 2.0f, 8.0f, 8.0f, AVBOITRD::optical_depth(0.25f)), 0.5f));
	// A surface the cell's center missed (its depth there is in a slice with nothing of its own) is
	// still dimmed by the layer in front of it: its own share does not come off another slice.
	float single[AVBOITRD::SLICES] = {};
	single[0] = AVBOITRD::optical_depth(0.5f);
	CHECK(Math::is_equal_approx(AVBOITRD::transmittance_front(single, 2.0f, 8.0f, 6.0f, AVBOITRD::optical_depth(0.5f)), 0.5f));
	// An additive surface (no optical depth of its own) behind the layer is dimmed by it.
	CHECK(Math::is_equal_approx(AVBOITRD::transmittance_front(single, 2.0f, 8.0f, 6.0f, 0.0f), 0.5f));
	// A surface nearer than the cell's nearest occluder is not dimmed by the occluder behind it, though
	// its depth falls in slice 0 (an additive surface in front of glass, or a layer's edge).
	CHECK_EQ(AVBOITRD::slice(1.5f, 2.0f, 8.0f), 0u);
	CHECK_EQ(AVBOITRD::transmittance_front(single, 2.0f, 8.0f, 1.5f, 0.0f), 1.0f);
	CHECK_EQ(AVBOITRD::transmittance_front(single, 2.0f, 8.0f, 1.5f, AVBOITRD::optical_depth(0.25f)), 1.0f);
	// Two surfaces in one slice dim each other, but neither dims itself.
	float shared[AVBOITRD::SLICES] = {};
	shared[7] = AVBOITRD::optical_depth(0.5f) + AVBOITRD::optical_depth(0.25f);
	CHECK_EQ(AVBOITRD::slice(3.9f, 2.0f, 8.0f), 7u);
	CHECK(Math::is_equal_approx(AVBOITRD::transmittance_front(shared, 2.0f, 8.0f, 3.9f, AVBOITRD::optical_depth(0.5f)), 0.75f));
	CHECK(Math::is_equal_approx(AVBOITRD::transmittance_front(shared, 2.0f, 8.0f, 3.9f, AVBOITRD::optical_depth(0.25f)), 0.5f));

	// Two close layers, 3.0 and 3.1 m at the cell's center, each with alpha 0.5: the near one is in
	// slice 0, the far one in the last slice. Classified by their depths at the cell's center, the far
	// one is dimmed by the near one and the near one by nothing, whatever their depths at the pixel.
	float close[AVBOITRD::SLICES] = {};
	close[0] = AVBOITRD::optical_depth(0.5f);
	close[AVBOITRD::SLICES - 1] = AVBOITRD::optical_depth(0.5f);
	CHECK(Math::is_equal_approx(AVBOITRD::transmittance_front(close, 3.0f, 3.1f, 3.1f, AVBOITRD::optical_depth(0.5f)), 0.5f));
	CHECK(Math::is_equal_approx(AVBOITRD::transmittance_front(close, 3.0f, 3.1f, 3.0f, AVBOITRD::optical_depth(0.5f)), 1.0f));
	// Behind every occluder the cell recorded (its own sample missed): both layers are in front of it.
	CHECK(Math::is_equal_approx(AVBOITRD::transmittance_front(close, 3.0f, 3.1f, 3.2f, AVBOITRD::optical_depth(0.5f)), 0.25f));
	// Within the half-float rounding of the stored farthest depth, it is the farthest layer itself.
	CHECK(Math::is_equal_approx(AVBOITRD::transmittance_front(close, 3.0f, 3.1f, 3.1f * 1.001f, AVBOITRD::optical_depth(0.5f)), 0.5f));
	CHECK_GT(AVBOITRD::FAR_MARGIN, 1.0f + 2.0f / 1024.0f);

	// A surface on a slice boundary: the pre-passes put its own sample just in front of the boundary,
	// the composite's depth for it lands just behind. It still does not dim itself.
	float boundary[AVBOITRD::SLICES] = {};
	boundary[0] = AVBOITRD::optical_depth(0.5f);
	boundary[AVBOITRD::SLICES - 1] = AVBOITRD::optical_depth(0.5f);
	const float first_boundary = 3.0f * Math::pow(3.1f / 3.0f, 1.0f / float(AVBOITRD::SLICES));
	CHECK_EQ(AVBOITRD::slice(first_boundary * (1.0f - 1e-5f), 3.0f, 3.1f), 0u);
	CHECK_EQ(AVBOITRD::slice(first_boundary * (1.0f + 1e-5f), 3.0f, 3.1f), 1u);
	CHECK(Math::is_equal_approx(AVBOITRD::transmittance_front(boundary, 3.0f, 3.1f, first_boundary * (1.0f + 1e-5f), AVBOITRD::optical_depth(0.5f)), 1.0f));
	// The far layer is still dimmed by it.
	CHECK(Math::is_equal_approx(AVBOITRD::transmittance_front(boundary, 3.0f, 3.1f, 3.1f, AVBOITRD::optical_depth(0.5f)), 0.5f));
	// The tolerance is below the bounds' own half-float rounding (2^-11 of the depth): it groups only
	// depths the bounds cannot tell apart either.
	CHECK_LT(AVBOITRD::SLICE_TOLERANCE, 1.0f / 2048.0f);
}

TEST_CASE("[AVBOIT] The depth at a cell's center, from the window depth") {
	// The composite extrapolates a fragment's window depth to its cell's center and classifies by the
	// view depth there. Window depth is linear in screen space across a flat triangle, so this is
	// exact under perspective, where view depth is not (its reciprocal is), and under an orthographic
	// projection, where view depth is linear too.
	Projection correction;
	correction.set_depth_correction(true);
	const Projection projections[2] = {
		correction * Projection::create_perspective(50.0, 16.0 / 9.0, 0.05, 4000.0),
		correction * Projection::create_orthogonal(-4.0, 4.0, -2.25, 2.25, 0.05, 100.0),
	};
	for (int p = 0; p < 2; p++) {
		const Projection &projection = projections[p];
		const Projection inverse = projection.inverse();
		// Two points of a steep plane, 3 m and 5 m away.
		const Vector3 a(-0.4, 0.1, -3.0);
		const Vector3 b(0.6, 0.2, -5.0);
		const Vector4 clip_a = projection.xform(Vector4(a.x, a.y, a.z, 1.0));
		const Vector4 clip_b = projection.xform(Vector4(b.x, b.y, b.z, 1.0));
		const Vector3 ndc_a = Vector3(clip_a.x, clip_a.y, clip_a.z) / clip_a.w;
		const Vector3 ndc_b = Vector3(clip_b.x, clip_b.y, clip_b.z) / clip_b.w;
		CHECK(Math::is_equal_approx(AVBOITRD::view_depth(inverse, Vector2(ndc_a.x, ndc_a.y), ndc_a.z), 3.0f));
		CHECK(Math::is_equal_approx(AVBOITRD::view_depth(inverse, Vector2(ndc_b.x, ndc_b.y), ndc_b.z), 5.0f));
		// From point a, a quarter and one and a half of the way to b on the screen: inside and past.
		for (const float t : { 0.25f, 1.5f }) {
			const Vector2 ndc = Vector2(ndc_a.x, ndc_a.y).lerp(Vector2(ndc_b.x, ndc_b.y), t);
			const float window_depth = Math::lerp(ndc_a.z, ndc_b.z, t);
			// The true depth on the line through a and b: its reciprocal is linear on the screen under
			// perspective, the depth itself under an orthographic projection.
			const float expected = p == 0 ? 1.0f / Math::lerp(1.0f / 3.0f, 1.0f / 5.0f, t) : Math::lerp(3.0f, 5.0f, t);
			CHECK(Math::is_equal_approx(AVBOITRD::view_depth(inverse, ndc, window_depth), expected, 1e-3f));
		}
	}
	// Kept to the bounds' half-float range: no infinite or zero depth reaches the slices.
	CHECK_EQ(AVBOITRD::grid_depth(1.0e6f), AVBOITRD::MAX_DEPTH);
	CHECK_EQ(AVBOITRD::grid_depth(-2.0f), AVBOITRD::MIN_DEPTH);
	CHECK_EQ(Math::half_to_float(Math::make_half_float(AVBOITRD::MAX_DEPTH)), AVBOITRD::MAX_DEPTH);
	CHECK_EQ(AVBOITRD::slice(AVBOITRD::grid_depth(1.0e6f), 100.0f, AVBOITRD::grid_depth(2.0e6f)), AVBOITRD::SLICES - 1);
	float far_slices[AVBOITRD::SLICES] = {};
	far_slices[0] = AVBOITRD::optical_depth(0.5f);
	const float far_t = AVBOITRD::transmittance_front(far_slices, 100.0f, AVBOITRD::MAX_DEPTH, AVBOITRD::grid_depth(1.0e6f), 0.0f);
	CHECK(Math::is_equal_approx(far_t, 0.5f));
}

TEST_CASE("[AVBOIT] The passes give the same pixel in every draw order") {
	// Three layers, each in its own slice: the result matches the sorted pass.
	const Layer layers[3] = { { 2.0f, 0.5f, 1.0f }, { 4.0f, 0.4f, 0.25f }, { 8.0f, 0.6f, 0.5f } };
	const Layer back_to_front[3] = { layers[2], layers[1], layers[0] };
	const float sorted = _sorted(back_to_front, 3, 0.1f);
	const int orders[6][3] = { { 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 } };
	const float first = _avboit(layers, 3, orders[0], 0.1f);
	for (const int *order : orders) {
		const float result = _avboit(layers, 3, order, 0.1f);
		CHECK(Math::abs(result - first) < 1e-6f);
		CHECK(Math::abs(result - sorted) < 1e-5f);
	}
	// The sorted pass itself is not order independent: front to back gives another pixel.
	CHECK(Math::abs(_sorted(layers, 3, 0.1f) - sorted) > 0.05f);
	// One layer matches the sorted pass: it does not dim itself.
	const int one[1] = { 0 };
	CHECK(Math::abs(_avboit(layers, 1, one, 0.1f) - _sorted(layers, 1, 0.1f)) < 1e-6f);
}

TEST_CASE("[AVBOIT] Blend states of the pre-passes and the composite") {
	const RD::PipelineColorBlendState bounds = AVBOITRD::bounds_blend_state();
	REQUIRE_EQ(bounds.attachments.size(), 1);
	CHECK(bounds.attachments[0].enable_blend);
	CHECK_EQ(bounds.attachments[0].color_blend_op, RD::BLEND_OP_MINIMUM);
	CHECK_EQ(bounds.attachments[0].alpha_blend_op, RD::BLEND_OP_MAXIMUM);
	CHECK_EQ(AVBOITRD::bounds_clear_color(), Color(AVBOITRD::BOUNDS_CLEAR_NEAR, AVBOITRD::BOUNDS_CLEAR_NEAR, AVBOITRD::BOUNDS_CLEAR_NEAR, 0.0));
	// The clear's nearest depth is the largest finite half float, so the RGBA16F target keeps it.
	CHECK_EQ(Math::half_to_float(Math::make_half_float(AVBOITRD::BOUNDS_CLEAR_NEAR)), AVBOITRD::BOUNDS_CLEAR_NEAR);

	const RD::PipelineColorBlendState occlusion = AVBOITRD::occlusion_blend_state();
	REQUIRE_EQ(occlusion.attachments.size(), int(AVBOITRD::LAYERS));
	for (int i = 0; i < occlusion.attachments.size(); i++) {
		CHECK(occlusion.attachments[i].enable_blend);
		CHECK_EQ(occlusion.attachments[i].color_blend_op, RD::BLEND_OP_ADD);
		CHECK_EQ(occlusion.attachments[i].src_color_blend_factor, RD::BLEND_FACTOR_ONE);
		CHECK_EQ(occlusion.attachments[i].dst_color_blend_factor, RD::BLEND_FACTOR_ONE);
		CHECK_EQ(occlusion.attachments[i].src_alpha_blend_factor, RD::BLEND_FACTOR_ONE);
		CHECK_EQ(occlusion.attachments[i].dst_alpha_blend_factor, RD::BLEND_FACTOR_ONE);
	}

	const RD::PipelineColorBlendState::Attachment composite = AVBOITRD::composite_blend_attachment();
	CHECK(composite.enable_blend);
	CHECK_EQ(composite.src_color_blend_factor, RD::BLEND_FACTOR_ONE);
	CHECK_EQ(composite.dst_color_blend_factor, RD::BLEND_FACTOR_ONE);
	CHECK_EQ(composite.color_blend_op, RD::BLEND_OP_ADD);
	CHECK_EQ(composite.src_alpha_blend_factor, RD::BLEND_FACTOR_ONE);
	CHECK_EQ(composite.dst_alpha_blend_factor, RD::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
}

TEST_CASE("[AVBOIT] Shader variants, the specialization bits and the pipeline key") {
	using Shader = RendererSceneRenderImplementation::SceneShaderForwardClustered;
	using Version = Shader::ShaderVersion;
	// Appended after every other variant (the last are #218's VSM depth variants), so no earlier index
	// moves, with ubershader twins.
	CHECK_EQ(int(Version::SHADER_VERSION_AVBOIT_BASE), int(Version::SHADER_VERSION_VSM_BASE) + int(Version::SHADER_VERSION_VSM_COUNT));
	CHECK_EQ(int(Version::SHADER_VERSION_AVBOIT_COUNT), 4);
	CHECK_EQ(int(Shader::ShaderData::VERTEX_INPUT_MASKS_SIZE), int(Version::SHADER_VERSION_AVBOIT_BASE) + int(Version::SHADER_VERSION_AVBOIT_COUNT));
	CHECK_EQ(int(Shader::SHADER_GROUP_AVBOIT), int(Shader::SHADER_GROUP_VSM) + 1);
	CHECK_EQ(int(Shader::PIPELINE_VERSION_AVBOIT_OCCLUSION) + 1, int(Shader::PIPELINE_VERSION_MAX));

	// sc_avboit_composite() reads bits 8 to 10 of packed_1, above the eight flags before it.
	Shader::ShaderSpecialization specialization = {};
	CHECK_EQ(specialization.packed_1, 0u);
	specialization.avboit_composite = AVBOITRD::COMPOSITE_SUBTRACT;
	CHECK_EQ(specialization.packed_1, uint32_t(AVBOITRD::COMPOSITE_SUBTRACT) << 8);
	specialization.material_feedback = 1;
	CHECK_EQ((specialization.packed_1 >> 8) & 7u, uint32_t(AVBOITRD::COMPOSITE_SUBTRACT));
	CHECK_EQ(specialization.packed_1 & 0xFFu, 1u << 7);

	// A key without AVBOIT hashes as it did before the field existed; the field changes the hash.
	Shader::ShaderData::PipelineKey key;
	key.vertex_format_id = 3;
	key.framebuffer_format_id = 5;
	key.cull_mode = RD::POLYGON_CULL_BACK;
	key.primitive_type = RSE::PRIMITIVE_TRIANGLES;
	key.version = Shader::PIPELINE_VERSION_COLOR_PASS;
	key.color_pass_flags = Shader::PIPELINE_COLOR_PASS_FLAG_TRANSPARENT;
	uint32_t h = hash_murmur3_one_64(key.vertex_format_id);
	h = hash_murmur3_one_32(key.framebuffer_format_id, h);
	h = hash_murmur3_one_32(key.cull_mode, h);
	h = hash_murmur3_one_32(key.primitive_type, h);
	h = hash_murmur3_one_32(key.version, h);
	h = hash_murmur3_one_32(key.color_pass_flags, h);
	h = hash_murmur3_one_32(key.shader_specialization.packed_0, h);
	h = hash_murmur3_one_32(key.shader_specialization.packed_1, h);
	h = hash_murmur3_one_32(key.shader_specialization.packed_2, h);
	h = hash_murmur3_one_32(key.wireframe, h);
	h = hash_murmur3_one_32(key.ubershader, h);
	h = hash_murmur3_one_32(key.zero_velocity_write, h);
	CHECK_EQ(key.hash(), hash_fmix32(h));
	Shader::ShaderData::PipelineKey avboit_key = key;
	avboit_key.avboit = true;
	CHECK_NE(avboit_key.hash(), key.hash());
	// The ubershader key keeps the AVBOIT field (the render list clears only the specialization).
	avboit_key.ubershader = 1;
	avboit_key.shader_specialization = {};
	Shader::ShaderData::PipelineKey uber_key = key;
	uber_key.ubershader = 1;
	CHECK_NE(avboit_key.hash(), uber_key.hash());
}
