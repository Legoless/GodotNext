/**************************************************************************/
/*  test_megalights.cpp                                                   */
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

TEST_FORCE_LINK(test_megalights)

#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "scene/3d/light_3d.h"
#include "servers/rendering/renderer_rd/effects/mega_lights.h"
#include "servers/rendering/rendering_server.h"

#include <cmath>
#include <cstdint>

using ML = RendererRD::MegaLights;

TEST_CASE("[Renderer][MegaLights] Project settings defaults and validation (#142)") {
	const ML::Settings defaults = ML::read_settings();
	CHECK_EQ(defaults.samples_per_pixel, ML::DEFAULT_SAMPLES_PER_PIXEL);
	CHECK_EQ(defaults.downsample, ML::DOWNSAMPLE_HALF);
	CHECK_EQ(defaults.directional_lights, false);
	CHECK(Math::is_equal_approx(defaults.falloff_cull_threshold, ML::DEFAULT_FALLOFF_CULL_THRESHOLD));
	CHECK_EQ(defaults.reference_mode, false);

	// Validation clamps out-of-range settings safely.
	ML::Settings s;
	s.samples_per_pixel = 0;
	s.downsample = static_cast<ML::DownsampleMode>(99);
	s.falloff_cull_threshold = -0.5f;
	const ML::Settings clamped_lo = ML::validate_settings(s);
	CHECK_EQ(clamped_lo.samples_per_pixel, ML::MIN_SAMPLES_PER_PIXEL);
	CHECK_EQ(clamped_lo.downsample, ML::DOWNSAMPLE_HALF);
	CHECK(Math::is_equal_approx(clamped_lo.falloff_cull_threshold, ML::DEFAULT_FALLOFF_CULL_THRESHOLD));

	s.samples_per_pixel = 100;
	s.downsample = ML::DOWNSAMPLE_FULL;
	s.falloff_cull_threshold = NAN;
	const ML::Settings clamped_hi = ML::validate_settings(s);
	CHECK_EQ(clamped_hi.samples_per_pixel, ML::MAX_SAMPLES_PER_PIXEL);
	CHECK_EQ(clamped_hi.downsample, ML::DOWNSAMPLE_FULL);
	CHECK(Math::is_equal_approx(clamped_hi.falloff_cull_threshold, ML::DEFAULT_FALLOFF_CULL_THRESHOLD));

	s.falloff_cull_threshold = 0.0f;
	CHECK_EQ(ML::validate_settings(s).falloff_cull_threshold, 0.0f);

	// String and integer variant parsing for downsample setting.
	CHECK_EQ(ML::parse_downsample_variant(0), ML::DOWNSAMPLE_FULL);
	CHECK_EQ(ML::parse_downsample_variant(1), ML::DOWNSAMPLE_HALF);
	CHECK_EQ(ML::parse_downsample_variant(String("full")), ML::DOWNSAMPLE_FULL);
	CHECK_EQ(ML::parse_downsample_variant(String("Full")), ML::DOWNSAMPLE_FULL);
	CHECK_EQ(ML::parse_downsample_variant(String("half")), ML::DOWNSAMPLE_HALF);
	CHECK_EQ(ML::parse_downsample_variant(String("Half")), ML::DOWNSAMPLE_HALF);

	// Provisional quality preset mapping (Low=2, Medium=2, High=4, Epic=8).
	CHECK_EQ(ML::samples_for_quality_preset(0), 2u);
	CHECK_EQ(ML::samples_for_quality_preset(1), 2u);
	CHECK_EQ(ML::samples_for_quality_preset(2), 4u);
	CHECK_EQ(ML::samples_for_quality_preset(3), 8u);
	CHECK_EQ(ML::samples_for_quality_preset(99), 8u);
}

TEST_CASE("[Renderer][MegaLights] GPU struct layouts and packing contracts (#142)") {
	CHECK_EQ(sizeof(ML::SampleGPU), 16u);
	CHECK_EQ(sizeof(ML::ParamsUBO), 256u);
	CHECK_EQ(sizeof(ML::GuidingPushConstant), 32u);

	// Light type + index bit-packing roundtrip.
	for (uint32_t t = 0; t <= 3; t++) {
		for (uint32_t idx : { 0u, 1u, 63u, 4095u, 65535u }) {
			const uint32_t packed = ML::pack_sample_light(t, idx);
			CHECK_NE(packed, ML::SAMPLE_INVALID);
			CHECK_EQ(ML::unpack_sample_light_type(packed), t);
			CHECK_EQ(ML::unpack_sample_light_index(packed), idx);
		}
	}

	// Guiding entry packing roundtrip.
	const uint32_t lid = ML::pack_sample_light(ML::LIGHT_TYPE_AREA, 1234u);
	const uint32_t entry = ML::pack_guiding_entry(lid, 77u);
	CHECK_EQ(ML::unpack_guiding_light(entry), lid);
	CHECK_EQ(ML::unpack_guiding_count(entry), 77u);

	// Count saturation at GUIDING_COUNT_MAX.
	const uint32_t sat = ML::pack_guiding_entry(lid, 999999u);
	CHECK_EQ(ML::unpack_guiding_light(sat), lid);
	CHECK_EQ(ML::unpack_guiding_count(sat), ML::GUIDING_COUNT_MAX);

	// Octahedral snorm16 unit-vector encode/decode angular accuracy across sphere.
	const Vector3 dirs[] = {
		Vector3(1, 0, 0),
		Vector3(-1, 0, 0),
		Vector3(0, 1, 0),
		Vector3(0, -1, 0),
		Vector3(0, 0, 1),
		Vector3(0, 0, -1),
		Vector3(1, 1, 1).normalized(),
		Vector3(-0.37f, 0.81f, -0.45f).normalized(),
		Vector3(0.12f, -0.95f, 0.29f).normalized(),
		Vector3(-0.71f, -0.52f, -0.47f).normalized(),
	};
	for (const Vector3 &d : dirs) {
		const uint32_t word = ML::pack_oct_snorm16(d);
		const Vector3 u = ML::unpack_oct_snorm16(word);
		const double chord = double(d.distance_to(u));
		const double deg_err = 2.0 * Math::rad_to_deg(std::asin(CLAMP(chord * 0.5, 0.0, 1.0)));
		CHECK(deg_err < 0.01);
	}
}

TEST_CASE("[Renderer][MegaLights] Downsample 2x2 rotation and grid dimensions (#142)") {
	// 2x2 Bayer rotation visits all 4 pixels within every 4-frame period.
	bool visited[2][2] = {};
	for (uint32_t f = 0; f < 4; f++) {
		const Vector2i off = ML::downsample_offset(f);
		CHECK(off.x >= 0);
		CHECK(off.x <= 1);
		CHECK(off.y >= 0);
		CHECK(off.y <= 1);
		visited[off.y][off.x] = true;
		// Periodic every 4 frames.
		CHECK_EQ(ML::downsample_offset(f + 4u), off);
		CHECK_EQ(ML::downsample_offset(f + 100u), off);
	}
	CHECK(visited[0][0]);
	CHECK(visited[0][1]);
	CHECK(visited[1][0]);
	CHECK(visited[1][1]);

	// Even and odd viewport dimensions.
	CHECK_EQ(ML::compute_sample_grid_size(Size2i(1920, 1080), ML::DOWNSAMPLE_FULL), Size2i(1920, 1080));
	CHECK_EQ(ML::compute_sample_grid_size(Size2i(1920, 1080), ML::DOWNSAMPLE_HALF), Size2i(960, 540));
	CHECK_EQ(ML::compute_sample_grid_size(Size2i(127, 65), ML::DOWNSAMPLE_HALF), Size2i(64, 33));
	CHECK_EQ(ML::compute_guiding_tile_dims(Size2i(1920, 1080)), Size2i(240, 135));
	CHECK_EQ(ML::compute_guiding_tile_dims(Size2i(127, 65)), Size2i(16, 9));

	// Edge clamping on odd viewports never steps out of bounds.
	const Size2i odd_size(127, 65);
	const Size2i odd_grid = ML::compute_sample_grid_size(odd_size, ML::DOWNSAMPLE_HALF);
	for (uint32_t f = 0; f < 4; f++) {
		const Vector2i px = ML::sample_to_full_pixel(Vector2i(odd_grid.x - 1, odd_grid.y - 1), odd_size, ML::DOWNSAMPLE_HALF, f);
		CHECK(px.x >= 0);
		CHECK(px.x < odd_size.x);
		CHECK(px.y >= 0);
		CHECK(px.y < odd_size.y);
	}

	// VRAM budget check for default 1080p half-res 4 spp sample + visibility + ping-pong guiding buffers.
	const Size2i hd(1920, 1080);
	const Size2i half_grid = ML::compute_sample_grid_size(hd, ML::DOWNSAMPLE_HALF);
	const Size2i tiles = ML::compute_guiding_tile_dims(hd);
	const uint64_t sample_bytes = uint64_t(half_grid.x) * uint64_t(half_grid.y) * 4ull * sizeof(ML::SampleGPU);
	const uint64_t vis_bytes = uint64_t(half_grid.x) * uint64_t(half_grid.y) * 4ull * sizeof(uint32_t);
	const uint64_t guiding_bytes = 2ull * uint64_t(tiles.x) * uint64_t(tiles.y) * ML::GUIDING_TILE_WORDS * sizeof(uint32_t);
	const uint64_t total_bytes = sample_bytes + vis_bytes + guiding_bytes;
	// Must stay within plan §21.3.4 64 MiB MegaLights per-view memory budget.
	CHECK(total_bytes < 64ull * 1024ull * 1024ull);
}

TEST_CASE("[Renderer][MegaLights] Falloff early cull boundary (#142)") {
	const float inv_range = 0.1f; // range = 10 m
	const float decay = 2.0f;

	const float att_near = ML::omni_attenuation(1.0f, inv_range, decay);
	const float att_mid = ML::omni_attenuation(5.0f, inv_range, decay);
	const float att_edge = ML::omni_attenuation(9.8f, inv_range, decay);
	const float att_out = ML::omni_attenuation(10.0f, inv_range, decay);

	CHECK(att_near > att_mid);
	CHECK(att_mid > att_edge);
	CHECK(att_edge > 0.0f);
	CHECK_EQ(att_out, 0.0f);

	// Threshold 0 disables early cull for any strictly positive attenuation.
	CHECK(ML::passes_falloff_cull(att_edge, 0.0f));
	CHECK_FALSE(ML::passes_falloff_cull(0.0f, 0.0f));

	// Positive threshold culls negligible edge attenuation while keeping near/mid lights.
	CHECK(ML::passes_falloff_cull(att_near, 0.001f));
	CHECK(ML::passes_falloff_cull(att_mid, 0.001f));
	CHECK_FALSE(ML::passes_falloff_cull(att_edge, att_edge + 1e-4f));
	CHECK(ML::passes_falloff_cull(att_edge, att_edge * 0.5f));
}

TEST_CASE("[Renderer][MegaLights] Visible-light guiding weight floor and top-32 tile aggregator (#142)") {
	// Without prior frame history, all lights receive neutral weight 1.0.
	CHECK(Math::is_equal_approx(ML::guiding_weight(0u, 0u, false), 1.0f));
	CHECK(Math::is_equal_approx(ML::guiding_weight(0u, 10u, false), 1.0f));

	// With history, occluded/unlisted lights keep strictly positive floor (never 0 -> unbiased!).
	const float w_occ = ML::guiding_weight(0u, 20u, true);
	CHECK(w_occ > 0.0f);
	CHECK(Math::is_equal_approx(w_occ, ML::DEFAULT_GUIDING_FLOOR));

	// Visible lights receive monotonically higher weight up to 1.0 at max_count.
	const float w_half = ML::guiding_weight(10u, 20u, true);
	const float w_max = ML::guiding_weight(20u, 20u, true);
	CHECK(w_half > w_occ);
	CHECK(w_max > w_half);
	CHECK(Math::is_equal_approx(w_max, 1.0f));

	// Top-32 tile aggregation: 48 distinct lights + invalid sentinel -> deterministic top 32 descending.
	LocalVector<uint32_t> hits;
	hits.push_back(ML::SAMPLE_INVALID);
	for (uint32_t i = 0; i < 48; i++) {
		const uint32_t lid = ML::pack_sample_light(ML::LIGHT_TYPE_OMNI, i);
		const uint32_t reps = (i % 12) + 1;
		for (uint32_t r = 0; r < reps; r++) {
			hits.push_back(lid);
		}
	}
	hits.push_back(ML::SAMPLE_INVALID);

	LocalVector<ML::GuidingEntry> top32;
	ML::aggregate_guiding_tile_cpu(hits.ptr(), hits.size(), top32);
	CHECK_EQ(top32.size(), ML::GUIDING_MAX_LIGHTS);

	// Must be sorted descending by count, ascending by light_packed on ties, and contain no SAMPLE_INVALID.
	for (uint32_t i = 0; i < top32.size(); i++) {
		CHECK_NE(top32[i].light_packed, ML::SAMPLE_INVALID);
		CHECK(top32[i].count >= 1u);
		if (i > 0) {
			const bool valid_order = (top32[i - 1].count > top32[i].count) ||
					(top32[i - 1].count == top32[i].count && top32[i - 1].light_packed < top32[i].light_packed);
			CHECK(valid_order);
		}
	}
	// The 4 lights with count=12 must occupy the first 4 slots in ascending index order (11, 23, 35, 47).
	CHECK_EQ(top32[0].count, 12u);
	CHECK_EQ(ML::unpack_sample_light_index(top32[0].light_packed), 11u);
	CHECK_EQ(ML::unpack_sample_light_index(top32[1].light_packed), 23u);
	CHECK_EQ(ML::unpack_sample_light_index(top32[2].light_packed), 35u);
	CHECK_EQ(ML::unpack_sample_light_index(top32[3].light_packed), 47u);
}

TEST_CASE("[Renderer][MegaLights] Weighted reservoir sampling unbiasedness with 64 lights and guiding floor (#142)") {
	constexpr uint32_t LIGHT_COUNT = 64u;
	constexpr uint32_t K_SAMPLES = 4u;
	constexpr uint32_t TRIALS = 40000u;

	// True per-light radiances (unequal, spanning 2 orders of magnitude).
	float radiance[LIGHT_COUNT];
	float unguided_weights[LIGHT_COUNT];
	float guided_weights[LIGHT_COUNT];
	double exact_sum = 0.0;

	for (uint32_t i = 0; i < LIGHT_COUNT; i++) {
		radiance[i] = 0.05f + 2.5f * float((i * 29u + 7u) % LIGHT_COUNT) / float(LIGHT_COUNT);
		// Proposal weight can differ from exact radiance (e.g., guiding boosts first 32 and floors last 32 at 0.25).
		unguided_weights[i] = radiance[i];
		const float guide = (i < 32u) ? ML::guiding_weight(32u - i, 32u, true) : ML::guiding_weight(0u, 32u, true);
		guided_weights[i] = radiance[i] * guide;
		exact_sum += double(radiance[i]);
	}

	auto run_estimator = [&](const float *p_weights, uint64_t p_seed) -> double {
		uint64_t rng = p_seed;
		LocalVector<ML::ReservoirSample> samples;
		double accum = 0.0;
		for (uint32_t t = 0; t < TRIALS; t++) {
			ML::sample_reservoirs_cpu(p_weights, LIGHT_COUNT, K_SAMPLES, rng, samples);
			CHECK_EQ(samples.size(), K_SAMPLES);
			double frame_est = 0.0;
			for (uint32_t k = 0; k < K_SAMPLES; k++) {
				CHECK(samples[k].light_index < LIGHT_COUNT);
				CHECK(samples[k].inv_pdf > 0.0f);
				frame_est += double(radiance[samples[k].light_index]) * double(samples[k].inv_pdf);
			}
			accum += frame_est / double(K_SAMPLES);
		}
		return accum / double(TRIALS);
	};

	const double est_unguided = run_estimator(unguided_weights, 0x142A11CEu);
	const double est_guided = run_estimator(guided_weights, 0x142B0B55u);
	const double rel_err_unguided = std::abs(est_unguided - exact_sum) / exact_sum;
	const double rel_err_guided = std::abs(est_guided - exact_sum) / exact_sum;

	// Both unguided and guiding-floor estimators must be unbiased (well within 1%, typically < 0.2%).
	CHECK(rel_err_unguided < 0.005);
	CHECK(rel_err_guided < 0.005);
}

TEST_CASE("[Renderer][MegaLights] Welford relative standard error scaling (#142)") {
	// Edge cases: N < 2 or near-zero mean return 0 cleanly.
	CHECK_EQ(ML::welford_relative_std_error(1.0f, 1.0f, 0u), 0.0f);
	CHECK_EQ(ML::welford_relative_std_error(1.0f, 1.0f, 1u), 0.0f);
	CHECK_EQ(ML::welford_relative_std_error(0.0f, 1.0f, 100u), 0.0f);

	// Known Bernoulli(0.5) sequence (values 0 and 2 -> mean = 1.0, variance = 1.0, std = 1.0).
	// For N=1024 samples, theoretical RSE = sigma / (mean * sqrt(N)) = 1 / 32 = 0.03125.
	double mean = 0.0;
	double m2 = 0.0;
	for (uint32_t i = 1; i <= 1024u; i++) {
		const double x = (i & 1u) ? 2.0 : 0.0;
		const double delta = x - mean;
		mean += delta / double(i);
		m2 += delta * (x - mean);
	}
	const float rse_1024 = ML::welford_relative_std_error(float(mean), float(m2), 1024u);
	CHECK(Math::abs(rse_1024 - (1.0f / 32.0f)) < 0.001f);

	// 4x samples (4096) halves RSE.
	for (uint32_t i = 1025u; i <= 4096u; i++) {
		const double x = (i & 1u) ? 2.0 : 0.0;
		const double delta = x - mean;
		mean += delta / double(i);
		m2 += delta * (x - mean);
	}
	const float rse_4096 = ML::welford_relative_std_error(float(mean), float(m2), 4096u);
	CHECK(Math::abs(rse_4096 / rse_1024 - 0.5f) < 0.01f);
}

TEST_CASE("[Renderer][MegaLights] Spatio-temporal blue-noise 64x64x32 table quality (#142)") {
	const uint8_t *data = ML::get_stbn_data();
	const uint32_t byte_size = ML::get_stbn_byte_size();
	REQUIRE(data != nullptr);
	CHECK_EQ(byte_size, ML::STBN_WIDTH * ML::STBN_HEIGHT * ML::STBN_DEPTH * ML::STBN_CHANNELS);
	CHECK_EQ(byte_size, 262144u);

	constexpr uint32_t W = ML::STBN_WIDTH;
	constexpr uint32_t H = ML::STBN_HEIGHT;
	constexpr uint32_t D = ML::STBN_DEPTH;
	constexpr uint32_t SLICE_PIXELS = W * H;

	// 1. Every time slice t = 0..31 and both channels must have exact uniform 256-bin stratification
	// (4096 / 256 = 16 occurrences per byte value 0..255).
	for (uint32_t ch = 0; ch < 2u; ch++) {
		for (uint32_t t = 0; t < D; t++) {
			uint32_t hist[256] = {};
			for (uint32_t p = 0; p < SLICE_PIXELS; p++) {
				const uint8_t v = data[(t * SLICE_PIXELS + p) * 2u + ch];
				hist[v]++;
			}
			for (uint32_t b = 0; b < 256u; b++) {
				CHECK_EQ(hist[b], SLICE_PIXELS / 256u);
			}
		}
	}

	// 2. Verify blue-noise negative lag-1 autocorrelation both spatially (dx=1, dy=1) and temporally (dt=1),
	// plus decorrelation between channel 0 and channel 1.
	auto at = [&](uint32_t t, uint32_t y, uint32_t x, uint32_t ch) -> double {
		const uint32_t idx = ((t % D) * SLICE_PIXELS + (y % H) * W + (x % W)) * 2u + ch;
		return (double(data[idx]) + 0.5) / 256.0 - 0.5;
	};

	const double var = (256.0 * 256.0 - 1.0) / (12.0 * 256.0 * 256.0);
	const double inv_n = 1.0 / double(D * SLICE_PIXELS);
	double cross_sum = 0.0;

	for (uint32_t ch = 0; ch < 2u; ch++) {
		double sum_dx = 0.0;
		double sum_dy = 0.0;
		double sum_dt = 0.0;
		for (uint32_t t = 0; t < D; t++) {
			for (uint32_t y = 0; y < H; y++) {
				for (uint32_t x = 0; x < W; x++) {
					const double v = at(t, y, x, ch);
					sum_dx += v * at(t, y, x + 1u, ch);
					sum_dy += v * at(t, y + 1u, x, ch);
					sum_dt += v * at(t + 1u, y, x, ch);
					if (ch == 0u) {
						cross_sum += v * at(t, y, x, 1u);
					}
				}
			}
		}
		const double r_dx = (sum_dx * inv_n) / var;
		const double r_dy = (sum_dy * inv_n) / var;
		const double r_dt = (sum_dt * inv_n) / var;
		// Blue noise suppresses low frequencies -> strong negative lag-1 correlation.
		CHECK(r_dx < -0.15);
		CHECK(r_dy < -0.15);
		CHECK(r_dt < -0.05);
	}

	const double r_cross = (cross_sum * inv_n) / var;
	CHECK(std::abs(r_cross) < 0.02);
}

TEST_CASE("[Renderer][MegaLights] Visibility chain settings, structs, and packing (#146)") {
	const ML::Settings defaults = ML::read_settings();
	CHECK_EQ(defaults.screen_traces, ML::DEFAULT_SCREEN_TRACES);
	CHECK_EQ(defaults.screen_traces, true);
	CHECK(Math::is_equal_approx(defaults.distant_screen_traces_length, ML::DEFAULT_DISTANT_SCREEN_TRACES_LENGTH));
	CHECK_EQ(defaults.far_field, false);
	CHECK_EQ(defaults.swrt_continuation, false);

	// Validation clamps invalid distant screen trace length back to default and preserves 0.0.
	ML::Settings s = defaults;
	s.distant_screen_traces_length = -3.0f;
	CHECK(Math::is_equal_approx(ML::validate_settings(s).distant_screen_traces_length, ML::DEFAULT_DISTANT_SCREEN_TRACES_LENGTH));
	s.distant_screen_traces_length = NAN;
	CHECK(Math::is_equal_approx(ML::validate_settings(s).distant_screen_traces_length, ML::DEFAULT_DISTANT_SCREEN_TRACES_LENGTH));
	s.distant_screen_traces_length = 0.0f;
	CHECK_EQ(ML::validate_settings(s).distant_screen_traces_length, 0.0f);
	s.distant_screen_traces_length = 12.5f;
	CHECK(Math::is_equal_approx(ML::validate_settings(s).distant_screen_traces_length, 12.5f));

	// GPU struct sizes & alignment contracts.
	CHECK_EQ(sizeof(ML::VisParamsUBO), 272u);
	CHECK_EQ(sizeof(ML::VisVSMProjectionParams), 64u);
	CHECK_EQ(ML::VSM_SMRT_RAYS, 2u);

	// Shader variant counts match bake registration.
	CHECK_EQ(ML::get_screen_trace_shader_variant_defines().size(), 4);
	CHECK_EQ(ML::get_trace_rays_shader_variant_defines().size(), 4);
	CHECK_EQ(ML::get_trace_rays_swrt_shader_variant_defines().size(), 1);
	CHECK_EQ(ML::get_trace_vsm_shader_variant_defines().size(), 1);

	// RGB transmittance RGBA8 packing roundtrip.
	const uint32_t packed_zero = ML::pack_visibility_rgba8(0.0f, 0.0f, 0.0f, 1.0f);
	const Color c_zero = ML::unpack_visibility_rgba8(packed_zero);
	CHECK(Math::is_equal_approx(c_zero.r, 0.0f));
	CHECK(Math::is_equal_approx(c_zero.g, 0.0f));
	CHECK(Math::is_equal_approx(c_zero.b, 0.0f));
	CHECK(Math::is_equal_approx(c_zero.a, 1.0f));

	const uint32_t packed_one = ML::pack_visibility_rgba8(1.0f, 1.0f, 1.0f, 1.0f);
	CHECK_EQ(packed_one, 0xFFFFFFFFu);

	const uint32_t packed_rgb = ML::pack_visibility_rgba8(0.25f, 0.5f, 0.75f, 1.0f);
	const Color c_rgb = ML::unpack_visibility_rgba8(packed_rgb);
	CHECK(Math::abs(c_rgb.r - 0.25f) < (1.0f / 255.0f));
	CHECK(Math::abs(c_rgb.g - 0.50f) < (1.0f / 255.0f));
	CHECK(Math::abs(c_rgb.b - 0.75f) < (1.0f / 255.0f));
}

TEST_CASE("[Renderer][MegaLights] Per-light MegaLightsShadowMethod and VSM routing (#146)") {
	// Light3D enum parity with RenderingServer enums.
	CHECK_EQ(int(Light3D::MEGALIGHTS_SHADOW_METHOD_AUTO), int(RSE::LIGHT_MEGALIGHTS_SHADOW_METHOD_AUTO));
	CHECK_EQ(int(Light3D::MEGALIGHTS_SHADOW_METHOD_RAY_TRACED), int(RSE::LIGHT_MEGALIGHTS_SHADOW_METHOD_RAY_TRACED));
	CHECK_EQ(int(Light3D::MEGALIGHTS_SHADOW_METHOD_VIRTUAL), int(RSE::LIGHT_MEGALIGHTS_SHADOW_METHOD_VIRTUAL));

	// VSM routing helper: only VIRTUAL lights route to VSM, and only when VSM is available.
	CHECK_FALSE(ML::light_uses_vsm(RSE::LIGHT_MEGALIGHTS_SHADOW_METHOD_AUTO, true));
	CHECK_FALSE(ML::light_uses_vsm(RSE::LIGHT_MEGALIGHTS_SHADOW_METHOD_RAY_TRACED, true));
	CHECK(ML::light_uses_vsm(RSE::LIGHT_MEGALIGHTS_SHADOW_METHOD_VIRTUAL, true));
	CHECK_FALSE(ML::light_uses_vsm(RSE::LIGHT_MEGALIGHTS_SHADOW_METHOD_VIRTUAL, false));

	// Node property getter/setter roundtrip on DirectionalLight3D and OmniLight3D.
	DirectionalLight3D *sun = memnew(DirectionalLight3D);
	CHECK_EQ(sun->get_megalights_shadow_method(), Light3D::MEGALIGHTS_SHADOW_METHOD_AUTO);
	sun->set_megalights_shadow_method(Light3D::MEGALIGHTS_SHADOW_METHOD_VIRTUAL);
	CHECK_EQ(sun->get_megalights_shadow_method(), Light3D::MEGALIGHTS_SHADOW_METHOD_VIRTUAL);
	sun->set_megalights_shadow_method(Light3D::MEGALIGHTS_SHADOW_METHOD_RAY_TRACED);
	CHECK_EQ(sun->get_megalights_shadow_method(), Light3D::MEGALIGHTS_SHADOW_METHOD_RAY_TRACED);
	memdelete(sun);

	OmniLight3D *omni = memnew(OmniLight3D);
	CHECK_EQ(omni->get_megalights_shadow_method(), Light3D::MEGALIGHTS_SHADOW_METHOD_AUTO);
	omni->set_megalights_shadow_method(Light3D::MEGALIGHTS_SHADOW_METHOD_VIRTUAL);
	CHECK_EQ(omni->get_megalights_shadow_method(), Light3D::MEGALIGHTS_SHADOW_METHOD_VIRTUAL);
	memdelete(omni);
}

TEST_CASE("[Renderer][MegaLights] Screen-trace step presets, distant length, and surface thickness (#146)") {
	// Quality preset step caps increase monotonically (Low=8, Medium=12, High=16, Epic=24).
	CHECK_EQ(ML::screen_trace_steps_for_quality(0), 8u);
	CHECK_EQ(ML::screen_trace_steps_for_quality(1), 12u);
	CHECK_EQ(ML::screen_trace_steps_for_quality(2), 16u);
	CHECK_EQ(ML::screen_trace_steps_for_quality(3), 24u);
	CHECK_EQ(ML::screen_trace_steps_for_quality(-5), 8u);
	CHECK_EQ(ML::screen_trace_steps_for_quality(10), 24u);

	// Near pixels use short near length; far pixels ramp smoothly across [0.5*start, 2*start] up to distant_screen_traces_length.
	const float d_near = ML::screen_trace_max_distance(4.0f, 1.0f, 4.0f, 16.0f);
	const float d_mid = ML::screen_trace_max_distance(16.0f, 1.0f, 4.0f, 16.0f);
	const float d_far = ML::screen_trace_max_distance(120.0f, 1.0f, 4.0f, 16.0f);
	CHECK(Math::is_equal_approx(d_near, 1.0f));
	CHECK(d_mid > d_near);
	CHECK(d_mid < d_far);
	CHECK(Math::is_equal_approx(d_far, 4.0f));

	// Setting distant_length = 0 disables distant screen trace for far pixels cleanly.
	CHECK(Math::is_equal_approx(ML::screen_trace_max_distance(100.0f, 1.0f, 0.0f, 16.0f), 0.0f));

	// Surface thickness scales linearly with view depth while keeping a 2 cm minimum floor.
	CHECK(Math::is_equal_approx(ML::screen_trace_surface_thickness(0.5f, 0.01f), 0.02f));
	CHECK(Math::is_equal_approx(ML::screen_trace_surface_thickness(10.0f, 0.01f), 0.10f));
	CHECK(Math::is_equal_approx(ML::screen_trace_surface_thickness(10.0f, 0.05f), 0.50f));
	CHECK(Math::is_equal_approx(ML::screen_trace_surface_thickness(10.0f, 0.0f), 0.02f));
}

TEST_CASE("[Renderer][MegaLights] 2x2 block ray deduplication and stream compaction (#146)") {
	// 1-texel depth tolerance keeps a 1 cm floor near camera and scales linearly with depth, grazing slope, and half-res stride.
	const float tol_near = ML::dedupe_depth_tolerance(2.0f, 1.0f, 1.732f, 1080u, false, false);
	const float tol_mid = ML::dedupe_depth_tolerance(20.0f, 1.0f, 1.732f, 1080u, false, false);
	const float tol_far = ML::dedupe_depth_tolerance(120.0f, 1.0f, 1.732f, 1080u, false, false);
	const float tol_half = ML::dedupe_depth_tolerance(20.0f, 1.0f, 1.732f, 1080u, true, false);
	const float tol_sloped = ML::dedupe_depth_tolerance(20.0f, 0.35f, 1.732f, 1080u, false, false);
	CHECK(Math::is_equal_approx(tol_near, 0.01f));
	CHECK(tol_mid > tol_near);
	CHECK(tol_far > tol_mid * 5.0f);
	CHECK(tol_half > tol_mid * 1.5f);
	CHECK(tol_sloped > tol_mid * 1.5f);

	const uint32_t light0 = ML::pack_sample_light(ML::LIGHT_TYPE_OMNI, 7u);
	const uint32_t light1 = ML::pack_sample_light(ML::LIGHT_TYPE_OMNI, 8u);
	const Vector3 n_up(0.0f, 1.0f, 0.0f);
	const Vector3 n_side(1.0f, 0.0f, 0.0f);
	const Vector3 dir_a = Vector3(0.2f, 0.95f, 0.24f).normalized();
	const Vector3 dir_b = Vector3(0.21f, 0.95f, 0.23f).normalized();
	const Vector3 dir_far = Vector3(-0.8f, 0.6f, 0.0f).normalized();

	// Matching light, depth within 1 texel, matching normal and direction -> deduplicate.
	CHECK(ML::can_dedupe_ray(light0, light0, 10.0f, 10.01f, 0.02f, 0.02f, n_up, n_up, dir_a, dir_b));
	// Reject different light, depth jump > 1 texel, normal discontinuity, or divergent ray direction.
	CHECK_FALSE(ML::can_dedupe_ray(light0, light1, 10.0f, 10.01f, 0.02f, 0.02f, n_up, n_up, dir_a, dir_b));
	CHECK_FALSE(ML::can_dedupe_ray(light0, light0, 10.0f, 10.50f, 0.02f, 0.02f, n_up, n_up, dir_a, dir_b));
	CHECK_FALSE(ML::can_dedupe_ray(light0, light0, 10.0f, 10.01f, 0.02f, 0.02f, n_up, n_side, dir_a, dir_b));
	CHECK_FALSE(ML::can_dedupe_ray(light0, light0, 10.0f, 10.01f, 0.02f, 0.02f, n_up, n_up, dir_a, dir_far));
	CHECK_FALSE(ML::can_dedupe_ray(ML::SAMPLE_INVALID, ML::SAMPLE_INVALID, 10.0f, 10.0f, 0.02f, 0.02f, n_up, n_up, dir_a, dir_a));

	// Full 2x2 block (4 pixels x 2 spp = 8 samples):
	//  - Slot k=0 across all 4 pixels picks light0 on same flat surface (depth diff <= tol) -> 1 ray + 3 deduped!
	//    Pixel 2 has smaller t_resume (0.35 vs 0.85) -> merged ray keeps conservative min t_resume = 0.35!
	//  - Slot k=1:
	//    pixel 0: screen hit -> screen_hit_count = 1 (0 rays)
	//    pixel 1: VSM light -> vsm_count = 1 (0 rays)
	//    pixel 2: unshadowed light -> unshadowed_count = 1 (0 rays)
	//    pixel 3: unresolved light1 at different depth -> 1 separate ray.
	ML::DedupeBlockSample block[8];
	for (uint32_t p = 0; p < 4u; p++) {
		ML::DedupeBlockSample &s0 = block[p * 2u + 0u];
		s0.light_packed = light0;
		s0.linear_depth = 10.0f + float(p) * 0.004f;
		s0.depth_tol = 0.025f;
		s0.world_normal = n_up;
		s0.ray_dir = dir_a;
		s0.t_resume = (p == 2u) ? 0.35f : 0.85f;
		s0.casts_shadow = true;
		s0.use_vsm = false;
		s0.screen_hit = false;
	}
	block[0 * 2u + 1u] = { light1, 10.0f, 0.025f, n_up, dir_a, 0.5f, true, false, true }; // screen hit
	block[1 * 2u + 1u] = { light1, 10.0f, 0.025f, n_up, dir_a, 0.5f, true, true, false }; // VSM
	block[2 * 2u + 1u] = { light1, 10.0f, 0.025f, n_up, dir_a, 0.5f, false, false, false }; // unshadowed
	block[3 * 2u + 1u] = { light1, 14.0f, 0.025f, n_up, dir_far, 0.9f, true, false, false }; // separate ray

	const ML::DedupeBlockResult with_dedupe = ML::compact_and_dedupe_2x2_cpu(block, 2u, true);
	CHECK_EQ(with_dedupe.ray_count, 2u);
	CHECK_EQ(with_dedupe.deduped_count, 3u);
	CHECK_EQ(with_dedupe.screen_hit_count, 1u);
	CHECK_EQ(with_dedupe.vsm_count, 1u);
	CHECK_EQ(with_dedupe.unshadowed_count, 1u);
	REQUIRE_EQ(with_dedupe.ray_min_t_resume.size(), 2u);
	CHECK(Math::is_equal_approx(with_dedupe.ray_min_t_resume[0], 0.35f));
	CHECK(Math::is_equal_approx(with_dedupe.ray_min_t_resume[1], 0.90f));
	// All 4 pixels' slot-0 samples map to ray 0; pixel 1 slot 1 gets VSM tag (0x80000000 | 0); pixel 3 slot 1 maps to ray 1.
	for (uint32_t p = 0; p < 4u; p++) {
		CHECK_EQ(with_dedupe.sample_ray_map[p * 2u + 0u], 0u);
	}
	CHECK_EQ(with_dedupe.sample_ray_map[0 * 2u + 1u], ML::SAMPLE_INVALID);
	CHECK_EQ(with_dedupe.sample_ray_map[1 * 2u + 1u], 0x80000000u);
	CHECK_EQ(with_dedupe.sample_ray_map[2 * 2u + 1u], ML::SAMPLE_INVALID);
	CHECK_EQ(with_dedupe.sample_ray_map[3 * 2u + 1u], 1u);

	// Disabling dedupe keeps all 5 unresolved non-VSM rays distinct.
	const ML::DedupeBlockResult no_dedupe = ML::compact_and_dedupe_2x2_cpu(block, 2u, false);
	CHECK_EQ(no_dedupe.ray_count, 5u);
	CHECK_EQ(no_dedupe.deduped_count, 0u);
	CHECK_EQ(no_dedupe.vsm_count, 1u);
	CHECK_EQ(no_dedupe.screen_hit_count, 1u);
	CHECK_EQ(no_dedupe.unshadowed_count, 1u);
}

TEST_CASE("[Renderer][MegaLights] Visibility tier gating (HWRT, Mac M2+ Experimental, SWRT extension, VSM) (#146)") {
	// 1. Desktop Vulkan/D3D12 HWRT with RT pipeline -> full HWRT with retrace & optional far field.
	ML::TierInputs desktop_hwrt;
	desktop_hwrt.forward_plus = true;
	desktop_hwrt.supports_ray_query = true;
	desktop_hwrt.supports_ray_tracing_pipeline = true;
	desktop_hwrt.is_metal = false;
	desktop_hwrt.nextgi_hwrt_available = true;
	desktop_hwrt.vsm_available = true;
	desktop_hwrt.far_field_requested = true;
	const ML::TierStatus st_desktop = ML::resolve_visibility_tier(desktop_hwrt);
	CHECK_EQ(st_desktop.tier, ML::VISIBILITY_TIER_HWRT);
	CHECK_FALSE(st_desktop.experimental);
	CHECK_FALSE(st_desktop.inline_ray_query_only);
	CHECK(st_desktop.retrace_supported);
	CHECK(st_desktop.far_field_supported);
	CHECK_FALSE(st_desktop.swrt_extension);
	CHECK(st_desktop.vsm_supported);
	CHECK_EQ(String(st_desktop.tier_name), String("HWRT"));

	// 2. Mac Apple M2+ Metal (#171): inline ray query only, cleanly labelled Experimental, no RT pipeline retrace.
	ML::TierInputs mac_m2;
	mac_m2.forward_plus = true;
	mac_m2.supports_ray_query = true;
	mac_m2.supports_ray_tracing_pipeline = false;
	mac_m2.is_metal = true;
	mac_m2.is_apple_m2_or_newer = true;
	mac_m2.nextgi_hwrt_available = true;
	mac_m2.vsm_available = true;
	const ML::TierStatus st_mac_m2 = ML::resolve_visibility_tier(mac_m2);
	CHECK_EQ(st_mac_m2.tier, ML::VISIBILITY_TIER_HWRT_INLINE_ONLY);
	CHECK(st_mac_m2.experimental);
	CHECK(st_mac_m2.inline_ray_query_only);
	CHECK_FALSE(st_mac_m2.retrace_supported);
	CHECK_FALSE(st_mac_m2.far_field_supported);
	CHECK(st_mac_m2.vsm_supported);
	CHECK_EQ(String(st_mac_m2.tier_name), String("HWRT_INLINE_ONLY"));
	CHECK(String(st_mac_m2.experimental_label).contains("Experimental"));
	CHECK(String(st_mac_m2.experimental_label).contains("Apple M2+"));

	// 3. Mac Apple M1 (no M2+ inline RT) with SWRT continuation enabled -> labeled SWRT extension.
	ML::TierInputs mac_m1_swrt;
	mac_m1_swrt.forward_plus = true;
	mac_m1_swrt.supports_ray_query = false;
	mac_m1_swrt.is_metal = true;
	mac_m1_swrt.is_apple_m2_or_newer = false;
	mac_m1_swrt.df_sdf_available = true;
	mac_m1_swrt.swrt_continuation_requested = true;
	mac_m1_swrt.vsm_available = true;
	const ML::TierStatus st_swrt = ML::resolve_visibility_tier(mac_m1_swrt);
	CHECK_EQ(st_swrt.tier, ML::VISIBILITY_TIER_SWRT_EXTENSION);
	CHECK(st_swrt.swrt_extension);
	CHECK_FALSE(st_swrt.experimental);
	CHECK_FALSE(st_swrt.retrace_supported);
	CHECK(st_swrt.vsm_supported);
	CHECK_EQ(String(st_swrt.tier_name), String("SWRT_EXTENSION"));

	// 4. Non-RT tier with SWRT continuation off (default) -> screen-only tier.
	ML::TierInputs non_rt_default = mac_m1_swrt;
	non_rt_default.swrt_continuation_requested = false;
	const ML::TierStatus st_screen = ML::resolve_visibility_tier(non_rt_default);
	CHECK_EQ(st_screen.tier, ML::VISIBILITY_TIER_SCREEN_ONLY);
	CHECK_FALSE(st_screen.swrt_extension);
	CHECK_EQ(String(st_screen.tier_name), String("SCREEN_ONLY"));

	// 5. Non-Forward+ renderer -> unavailable cleanly.
	ML::TierInputs mobile;
	mobile.forward_plus = false;
	mobile.supports_ray_query = true;
	const ML::TierStatus st_mobile = ML::resolve_visibility_tier(mobile);
	CHECK_EQ(st_mobile.tier, ML::VISIBILITY_TIER_DISABLED);
	CHECK_EQ(String(st_mobile.tier_name), String("UNAVAILABLE"));

	// 6. RenderingServer.megalights_get_tier_info() exposes all expected inspection keys cleanly.
	const Dictionary info = RenderingServer::get_singleton()->megalights_get_tier_info();
	CHECK(info.has("tier"));
	CHECK(info.has("backend"));
	CHECK(info.has("experimental"));
	CHECK(info.has("experimental_label"));
	CHECK(info.has("inline_ray_query_only"));
	CHECK(info.has("retrace_supported"));
	CHECK(info.has("far_field_enabled"));
	CHECK(info.has("swrt_extension"));
	CHECK(info.has("vsm_supported"));
	CHECK(info.has("screen_traces_enabled"));
	CHECK(info.has("distant_screen_traces_length"));
	CHECK(info.has("reason"));
}
