/**************************************************************************/
/*  tsr_presets.h                                                         */
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

#include "core/math/math_funcs.h"
#include "core/math/vector2i.h"
#include "core/string/ustring.h"
#include "servers/rendering/rendering_server_enums.h"

// GodotNext: pt_tsr_mode_presets (#197). Per-preset TSR feature split matching
// UE's public sg.AntiAliasingQuality 0-4 scalability description.
struct TSRPreset {
	float history_scale = 1.0f;
	bool resurrection = false;
	bool reprojection_field = false;
	bool flicker_analysis = false;
	bool rejection_aa = false;
};

inline constexpr TSRPreset TSR_PRESETS[5] = {
	// Low (0): 1.0, off, off, off, off.
	{ 1.0f, false, false, false, false },
	// Medium (1): 1.0, on, off, off, on.
	{ 1.0f, true, false, false, true },
	// High (2): 1.0, on, on, on, on.
	{ 1.0f, true, true, true, true },
	// Epic (3): 2.0, on, on, on, on.
	{ 2.0f, true, true, true, true },
	// Cinematic (4): 2.0, on, on, on, on.
	{ 2.0f, true, true, true, true },
};

static_assert(sizeof(TSR_PRESETS) / sizeof(TSR_PRESETS[0]) == RSE::VIEWPORT_TSR_QUALITY_MAX);

inline constexpr float NEXTGI_TSR_MIN_SCALE = 0.25f;
inline constexpr float NEXTGI_TSR_MAX_SCALE = 2.0f;

inline constexpr float nextgi_clamp_tsr_scale(float p_scale, bool &r_clamped) {
	const float clamped = CLAMP(p_scale, NEXTGI_TSR_MIN_SCALE, NEXTGI_TSR_MAX_SCALE);
	r_clamped = clamped != p_scale;
	return clamped;
}

struct TSRScalingResolution {
	RSE::ViewportScaling3DMode mode = RSE::VIEWPORT_SCALING_3D_MODE_BILINEAR;
	float scale = 1.0f;
	bool use_taa = false;
	RSE::ViewportMSAA msaa_3d = RSE::VIEWPORT_MSAA_DISABLED;
	bool scale_clamped = false;
	bool msaa_disabled = false;
	bool taa_disabled = false;
};

// Pure helper shared between RendererViewport::_configure_3d_render_buffers and
// [Upscalers][TSR] CPU doctests.
inline TSRScalingResolution nextgi_resolve_tsr_scaling(
		RSE::ViewportScaling3DMode p_requested_mode,
		float p_requested_scale,
		const String &p_rendering_method,
		bool p_upscaler_available,
		bool p_use_taa,
		RSE::ViewportMSAA p_msaa_3d) {
	const float EPSILON = 0.0001f;
	TSRScalingResolution resolved;
	resolved.mode = p_requested_mode;
	resolved.scale = p_requested_scale;
	resolved.use_taa = p_use_taa;
	resolved.msaa_3d = p_msaa_3d;

	RSE::ViewportScaling3DType scaling_type = RSE::scaling_3d_mode_type(resolved.mode);
	if ((!p_upscaler_available || scaling_type == RSE::VIEWPORT_SCALING_3D_TYPE_SPATIAL) && resolved.scale >= (1.0f - EPSILON) && resolved.scale <= (1.0f + EPSILON)) {
		resolved.scale = 1.0f;
		resolved.mode = RSE::VIEWPORT_SCALING_3D_MODE_OFF;
		scaling_type = RSE::scaling_3d_mode_type(resolved.mode);
	}

	if (resolved.mode != RSE::VIEWPORT_SCALING_3D_MODE_OFF && resolved.mode != RSE::VIEWPORT_SCALING_3D_MODE_BILINEAR && resolved.mode != RSE::VIEWPORT_SCALING_3D_MODE_NEAREST && p_rendering_method == "gl_compatibility") {
		resolved.mode = RSE::VIEWPORT_SCALING_3D_MODE_BILINEAR;
		scaling_type = RSE::scaling_3d_mode_type(resolved.mode);
	}

	if ((resolved.mode == RSE::VIEWPORT_SCALING_3D_MODE_FSR || resolved.mode == RSE::VIEWPORT_SCALING_3D_MODE_FSR2 || resolved.mode == RSE::VIEWPORT_SCALING_3D_MODE_METALFX_TEMPORAL || resolved.mode == RSE::VIEWPORT_SCALING_3D_MODE_DLSS || resolved.mode == RSE::VIEWPORT_SCALING_3D_MODE_TSR || resolved.mode == RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM) && p_rendering_method == "mobile") {
		resolved.mode = RSE::VIEWPORT_SCALING_3D_MODE_BILINEAR;
		scaling_type = RSE::scaling_3d_mode_type(resolved.mode);
	}

	if (resolved.mode == RSE::VIEWPORT_SCALING_3D_MODE_TSR && p_upscaler_available) {
		resolved.scale = nextgi_clamp_tsr_scale(resolved.scale, resolved.scale_clamped);
	}

	bool is_not_bilinear = resolved.mode != RSE::VIEWPORT_SCALING_3D_MODE_OFF && resolved.mode != RSE::VIEWPORT_SCALING_3D_MODE_BILINEAR;
	// GodotNext: pt_temporal_upscaler_extension (#192). CUSTOM, like TSR, may
	// supersample and disables 3D MSAA; nextgi_resolve_custom_upscaler() then
	// applies the extension's range or the TSR fallback.
	if (is_not_bilinear && resolved.mode != RSE::VIEWPORT_SCALING_3D_MODE_TSR && resolved.mode != RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM && resolved.scale >= (1.0f + EPSILON)) {
		resolved.mode = RSE::VIEWPORT_SCALING_3D_MODE_BILINEAR;
		scaling_type = RSE::scaling_3d_mode_type(resolved.mode);
		is_not_bilinear = false;
	}

	if (is_not_bilinear && resolved.mode != RSE::VIEWPORT_SCALING_3D_MODE_NEAREST && !p_upscaler_available) {
		resolved.mode = RSE::VIEWPORT_SCALING_3D_MODE_BILINEAR;
		scaling_type = RSE::scaling_3d_mode_type(resolved.mode);
	}

	if ((resolved.mode == RSE::VIEWPORT_SCALING_3D_MODE_TSR || resolved.mode == RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM) && resolved.msaa_3d != RSE::VIEWPORT_MSAA_DISABLED) {
		resolved.msaa_3d = RSE::VIEWPORT_MSAA_DISABLED;
		resolved.msaa_disabled = true;
	}

	if (resolved.use_taa && scaling_type == RSE::VIEWPORT_SCALING_3D_TYPE_TEMPORAL) {
		resolved.use_taa = false;
		resolved.taa_disabled = true;
	}

	return resolved;
}

// GodotNext: pt_tsr_core (#194). Step 4 jitter phase count = round(8 * (output/internal)^2), minimum 8.
inline uint32_t nextgi_tsr_jitter_phase_count(const Size2i &p_target_size, const Size2i &p_render_size) {
	if (p_render_size.width <= 0 || p_target_size.width <= 0) {
		return 8u;
	}
	const float ratio = float(p_target_size.width) / float(p_render_size.width);
	const uint32_t rounded = uint32_t(Math::floor(8.0f * ratio * ratio + 0.5f));
	return MAX(8u, rounded);
}

// Provisional mip bias offset from {-1.0, -0.5, 0.0}; final value is selected by the GT-2 sweep in PQ (§22).
inline constexpr float nextgi_tsr_mip_bias_offset() {
	return -0.5f;
}
