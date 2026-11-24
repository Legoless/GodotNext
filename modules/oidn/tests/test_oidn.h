/**************************************************************************/
/*  test_oidn.h                                                           */
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

// GodotNext: path_tracer_denoise (#168). Unit tests for modules/oidn (runs under oidn_backend=mock in CI
// and also exercises the shared validation/dispatch contract when built with oidn_backend=oidn).

#include "../oidn_denoiser.h"

#include "core/math/math_funcs.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/effects/path_tracer.h"
#include "tests/test_macros.h"

#include <limits>

namespace TestOIDN {

TEST_CASE("[OIDN] Module registers backend on PathTracer and reports expected name") {
	CHECK(RendererRD::PathTracer::is_oidn_available());
	CHECK_EQ(String(RendererRD::PathTracer::get_oidn_backend_name()), String(OIDNModule::backend_name()));
	if (OIDNModule::is_mock_backend()) {
		CHECK_EQ(String(OIDNModule::backend_name()), String("mock"));
	} else {
		CHECK_EQ(String(OIDNModule::backend_name()), String("oidn"));
	}
}

TEST_CASE("[OIDN] Invalid inputs are rejected cleanly with error messages") {
	String err;
	RendererRD::PathTracerOIDNInput input;
	CHECK_EQ(OIDNModule::denoise(input, err), ERR_INVALID_PARAMETER);
	CHECK_FALSE(err.is_empty());

	float pixel[3] = { 0.5f, 0.5f, 0.5f };
	float out[3] = {};
	input.width = 0;
	input.height = 1;
	input.color = pixel;
	input.output = out;
	err = String();
	CHECK_EQ(OIDNModule::denoise(input, err), ERR_INVALID_PARAMETER);
	CHECK_FALSE(err.is_empty());

	input.width = 1;
	input.height = 1;
	input.color = nullptr;
	err = String();
	CHECK_EQ(OIDNModule::denoise(input, err), ERR_INVALID_PARAMETER);
	CHECK_FALSE(err.is_empty());

	input.color = pixel;
	input.output = nullptr;
	err = String();
	CHECK_EQ(OIDNModule::denoise(input, err), ERR_INVALID_PARAMETER);
	CHECK_FALSE(err.is_empty());
}

TEST_CASE("[OIDN] Denoises flat noisy input, preserves guide edges, and sanitizes non-finite values") {
	constexpr uint32_t W = 8;
	constexpr uint32_t H = 8;
	LocalVector<float> color;
	LocalVector<float> albedo;
	LocalVector<float> normal;
	LocalVector<float> output;
	color.resize(W * H * 3u);
	albedo.resize(W * H * 3u);
	normal.resize(W * H * 3u);
	output.resize(W * H * 3u);

	// Left half: red surface facing +Z with high-frequency checkerboard noise around 0.8.
	// Right half: blue surface facing +X with constant radiance 0.2.
	for (uint32_t y = 0; y < H; y++) {
		for (uint32_t x = 0; x < W; x++) {
			const uint32_t i = (y * W + x) * 3u;
			if (x < W / 2u) {
				const float noise = ((x + y) % 2u == 0u) ? 0.3f : -0.3f;
				color[i + 0u] = 0.8f + noise;
				color[i + 1u] = 0.1f;
				color[i + 2u] = 0.1f;
				albedo[i + 0u] = 0.9f;
				albedo[i + 1u] = 0.1f;
				albedo[i + 2u] = 0.1f;
				normal[i + 0u] = 0.0f;
				normal[i + 1u] = 0.0f;
				normal[i + 2u] = 1.0f;
			} else {
				color[i + 0u] = 0.05f;
				color[i + 1u] = 0.05f;
				color[i + 2u] = 0.2f;
				albedo[i + 0u] = 0.1f;
				albedo[i + 1u] = 0.1f;
				albedo[i + 2u] = 0.9f;
				normal[i + 0u] = 1.0f;
				normal[i + 1u] = 0.0f;
				normal[i + 2u] = 0.0f;
			}
		}
	}

	if (OIDNModule::is_mock_backend()) {
		OIDNModule::reset_mock_stats();
	}

	RendererRD::PathTracerOIDNInput input;
	input.width = W;
	input.height = H;
	input.color = color.ptr();
	input.albedo = albedo.ptr();
	input.normal = normal.ptr();
	input.output = output.ptr();
	input.hdr = true;
	input.clean_aux = true;

	String err;
	REQUIRE_EQ(RendererRD::PathTracer::run_oidn_denoise(input, err), OK);
	CHECK(err.is_empty());

	// All output values are finite and non-negative.
	for (uint32_t i = 0; i < output.size(); i++) {
		CHECK(Math::is_finite(output[i]));
		CHECK(output[i] >= 0.0f);
	}

	// Interior of noisy left half has strictly lower variance than the raw input.
	double raw_var = 0.0;
	double den_var = 0.0;
	for (uint32_t y = 1; y < H - 1; y++) {
		for (uint32_t x = 1; x < W / 2u - 1; x++) {
			const uint32_t i = (y * W + x) * 3u;
			const double dr = double(color[i]) - 0.8;
			const double dd = double(output[i]) - 0.8;
			raw_var += dr * dr;
			den_var += dd * dd;
		}
	}
	CHECK(den_var < raw_var * 0.25);

	// Right-half pixel adjacent to the guide seam (x = W/2, y = H/2) does not bleed red from the left half.
	const uint32_t seam_right = ((H / 2u) * W + (W / 2u)) * 3u;
	CHECK(output[seam_right + 0u] < 0.15f);
	CHECK(output[seam_right + 2u] > 0.15f);

	if (OIDNModule::is_mock_backend()) {
		const OIDNModule::MockStats stats = OIDNModule::get_mock_stats();
		CHECK_EQ(stats.calls, 1u);
		CHECK_EQ(stats.last_width, W);
		CHECK_EQ(stats.last_height, H);
		CHECK(stats.last_had_albedo);
		CHECK(stats.last_had_normal);
		CHECK(stats.last_hdr);
		CHECK(stats.last_clean_aux);

		// NaN/Inf sanitization and in-place operation.
		color[0] = std::numeric_limits<float>::quiet_NaN();
		color[1] = std::numeric_limits<float>::infinity();
		color[2] = -5.0f;
		input.output = color.ptr();
		REQUIRE_EQ(RendererRD::PathTracer::run_oidn_denoise(input, err), OK);
		for (uint32_t i = 0; i < color.size(); i++) {
			CHECK(Math::is_finite(color[i]));
			CHECK(color[i] >= 0.0f);
		}

		// Simulated error propagation (for fallback testing).
		OIDNModule::set_mock_fail_next(true, "injected test failure");
		input.output = output.ptr();
		err = String();
		CHECK_EQ(RendererRD::PathTracer::run_oidn_denoise(input, err), FAILED);
		CHECK_EQ(err, String("injected test failure"));
		// Error injection is one-shot; the next call succeeds.
		err = String();
		CHECK_EQ(RendererRD::PathTracer::run_oidn_denoise(input, err), OK);
		OIDNModule::reset_mock_stats();
	}
}

} // namespace TestOIDN
