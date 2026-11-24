/**************************************************************************/
/*  oidn_denoiser_mock.cpp                                                */
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

#include "oidn_denoiser.h"

#include "core/math/math_funcs.h"
#include "core/os/mutex.h"
#include "core/templates/local_vector.h"

// GodotNext: path_tracer_denoise (#168). Deterministic SDK-free mock backend for CI and doctests
// (oidn_backend=mock, plan §19). Runs a guide-aware 3x3 cross-bilateral filter over RGB32F buffers so
// constant regions are preserved, edges across albedo/normal guides stay sharp, and flat-region noise drops.

namespace OIDNModule {

namespace {

Mutex g_mock_mutex;
MockStats g_mock_stats;
bool g_mock_fail_next = false;
String g_mock_fail_message;

float _sanitize(float p_value, bool p_non_negative) {
	if (!Math::is_finite(p_value)) {
		return 0.0f;
	}
	return p_non_negative && p_value < 0.0f ? 0.0f : p_value;
}

} // namespace

const char *backend_name() {
	return "mock";
}

bool is_mock_backend() {
	return true;
}

MockStats get_mock_stats() {
	MutexLock lock(g_mock_mutex);
	return g_mock_stats;
}

void reset_mock_stats() {
	MutexLock lock(g_mock_mutex);
	g_mock_stats = MockStats();
	g_mock_fail_next = false;
	g_mock_fail_message = String();
}

void set_mock_fail_next(bool p_fail, const String &p_message) {
	MutexLock lock(g_mock_mutex);
	g_mock_fail_next = p_fail;
	g_mock_fail_message = p_message;
}

Error denoise_impl(const RendererRD::PathTracerOIDNInput &p_input, String &r_error) {
	{
		MutexLock lock(g_mock_mutex);
		g_mock_stats.calls++;
		g_mock_stats.last_width = p_input.width;
		g_mock_stats.last_height = p_input.height;
		g_mock_stats.last_had_albedo = p_input.albedo != nullptr;
		g_mock_stats.last_had_normal = p_input.normal != nullptr;
		g_mock_stats.last_hdr = p_input.hdr;
		g_mock_stats.last_clean_aux = p_input.clean_aux;
		if (g_mock_fail_next) {
			g_mock_fail_next = false;
			r_error = g_mock_fail_message.is_empty() ? String("Simulated OIDN mock failure.") : g_mock_fail_message;
			return FAILED;
		}
	}

	const int width = int(p_input.width);
	const int height = int(p_input.height);
	const bool has_albedo = p_input.albedo != nullptr;
	const bool has_normal = p_input.normal != nullptr;
	// Support in-place filtering cleanly if caller aliases color and output.
	LocalVector<float> scratch;
	const float *src = p_input.color;
	if (p_input.color == p_input.output) {
		scratch.resize(uint32_t(uint64_t(width) * uint64_t(height) * 3u));
		memcpy(scratch.ptr(), p_input.color, scratch.size() * sizeof(float));
		src = scratch.ptr();
	}

	constexpr float INV_TWO_SIGMA_SPATIAL_SQ = 1.0f / (2.0f * 1.5f * 1.5f);
	constexpr float INV_TWO_SIGMA_ALBEDO_SQ = 1.0f / (2.0f * 0.15f * 0.15f);
	constexpr float INV_TWO_SIGMA_NORMAL_SQ = 1.0f / (2.0f * 0.1f * 0.1f);
	constexpr float EPSILON = 1e-6f;

	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			const uint64_t center_idx = (uint64_t(y) * uint64_t(width) + uint64_t(x)) * 3u;
			const float cr = _sanitize(src[center_idx + 0u], p_input.hdr);
			const float cg = _sanitize(src[center_idx + 1u], p_input.hdr);
			const float cb = _sanitize(src[center_idx + 2u], p_input.hdr);

			float ar = 0.0f, ag = 0.0f, ab = 0.0f;
			if (has_albedo) {
				ar = _sanitize(p_input.albedo[center_idx + 0u], false);
				ag = _sanitize(p_input.albedo[center_idx + 1u], false);
				ab = _sanitize(p_input.albedo[center_idx + 2u], false);
			}
			float nx = 0.0f, ny = 0.0f, nz = 0.0f;
			bool center_has_normal = false;
			if (has_normal) {
				nx = _sanitize(p_input.normal[center_idx + 0u], false);
				ny = _sanitize(p_input.normal[center_idx + 1u], false);
				nz = _sanitize(p_input.normal[center_idx + 2u], false);
				center_has_normal = (nx * nx + ny * ny + nz * nz) > EPSILON * EPSILON;
			}

			if (has_normal && !center_has_normal) {
				p_input.output[center_idx + 0u] = cr;
				p_input.output[center_idx + 1u] = cg;
				p_input.output[center_idx + 2u] = cb;
				continue;
			}

			float sum_r = 0.0f;
			float sum_g = 0.0f;
			float sum_b = 0.0f;
			float sum_w = 0.0f;

			for (int dy = -1; dy <= 1; dy++) {
				const int py = y + dy;
				if (py < 0 || py >= height) {
					continue;
				}
				for (int dx = -1; dx <= 1; dx++) {
					const int px = x + dx;
					if (px < 0 || px >= width) {
						continue;
					}
					const uint64_t idx = (uint64_t(py) * uint64_t(width) + uint64_t(px)) * 3u;
					float exponent = -float(dx * dx + dy * dy) * INV_TWO_SIGMA_SPATIAL_SQ;
					if (has_normal) {
						const float snx = _sanitize(p_input.normal[idx + 0u], false);
						const float sny = _sanitize(p_input.normal[idx + 1u], false);
						const float snz = _sanitize(p_input.normal[idx + 2u], false);
						if (snx * snx + sny * sny + snz * snz <= EPSILON * EPSILON) {
							continue;
						}
						const float dnx = nx - snx;
						const float dny = ny - sny;
						const float dnz = nz - snz;
						exponent -= (dnx * dnx + dny * dny + dnz * dnz) * INV_TWO_SIGMA_NORMAL_SQ;
					}
					if (has_albedo) {
						const float dar = ar - _sanitize(p_input.albedo[idx + 0u], false);
						const float dag = ag - _sanitize(p_input.albedo[idx + 1u], false);
						const float dab = ab - _sanitize(p_input.albedo[idx + 2u], false);
						exponent -= (dar * dar + dag * dag + dab * dab) * INV_TWO_SIGMA_ALBEDO_SQ;
					}
					const float w = Math::exp(exponent);
					sum_r += w * _sanitize(src[idx + 0u], p_input.hdr);
					sum_g += w * _sanitize(src[idx + 1u], p_input.hdr);
					sum_b += w * _sanitize(src[idx + 2u], p_input.hdr);
					sum_w += w;
				}
			}

			if (sum_w > EPSILON) {
				p_input.output[center_idx + 0u] = sum_r / sum_w;
				p_input.output[center_idx + 1u] = sum_g / sum_w;
				p_input.output[center_idx + 2u] = sum_b / sum_w;
			} else {
				p_input.output[center_idx + 0u] = cr;
				p_input.output[center_idx + 1u] = cg;
				p_input.output[center_idx + 2u] = cb;
			}
		}
	}

	return OK;
}

void shutdown() {
	reset_mock_stats();
}

} // namespace OIDNModule
