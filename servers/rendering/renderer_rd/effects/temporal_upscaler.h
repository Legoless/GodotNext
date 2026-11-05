/**************************************************************************/
/*  temporal_upscaler.h                                                   */
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
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,         */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/string/ustring.h"
#include "servers/rendering/rendering_server_enums.h"

// GodotNext: pu_dlss_scaling_mode (#103, plan §3.2)
//
// Optional temporal upscalers register here (modules/dlss is the first one).
// A DLSS request always resolves to a working backend: DLSS, then MetalFX
// temporal, then FSR2, with one ViewportScaling3DReason and one log line.
class TemporalUpscaler {
public:
	virtual ~TemporalUpscaler() = default;

	virtual String get_display_name() const = 0;

	// NONE when usable. Otherwise the reason the mode cannot run on this
	// device/view; r_effective_scale may be clamped by the implementation.
	virtual RSE::ViewportScaling3DReason get_availability(uint32_t p_view_count, float p_scale, float &r_effective_scale) const {
		r_effective_scale = p_scale;
		return RSE::VIEWPORT_SCALING_3D_REASON_NONE;
	}
};

// GodotNext: pu_dlss_scaling_mode (#103). The §3.2 precedence in one pure
// helper so the doctests and the viewport resolver agree.
inline RSE::ViewportScaling3DReason nextgi_resolve_dlss_reason(bool p_api_supported, bool p_module_registered, RSE::ViewportScaling3DReason p_module_reason, float p_scale, uint32_t p_view_count, int p_width, int p_height) {
	if (!p_api_supported) {
		return RSE::VIEWPORT_SCALING_3D_REASON_API_UNSUPPORTED;
	}
	if (!p_module_registered) {
		return RSE::VIEWPORT_SCALING_3D_REASON_MODULE_DISABLED;
	}
	if (p_module_reason != RSE::VIEWPORT_SCALING_3D_REASON_NONE) {
		return p_module_reason;
	}
	if (p_scale < 0.30f || p_scale > 1.0f || p_width < 32 || p_height < 32) {
		return RSE::VIEWPORT_SCALING_3D_REASON_SCALE_UNSUPPORTED;
	}
	if (p_view_count > 1) {
		return RSE::VIEWPORT_SCALING_3D_REASON_VIEW_UNSUPPORTED;
	}
	return RSE::VIEWPORT_SCALING_3D_REASON_NONE;
}

class TemporalUpscalerRegistry {
public:
	static void register_upscaler(RSE::ViewportScaling3DMode p_mode, TemporalUpscaler *p_upscaler);
	static void unregister_upscaler(RSE::ViewportScaling3DMode p_mode);
	static TemporalUpscaler *get(RSE::ViewportScaling3DMode p_mode);
	static void clear();

	// Builds the ProjectSettings enum hint. DLSS is appended only while a
	// DLSS upscaler is registered, so a default build's settings are unchanged.
	static String build_mode_hint(bool p_metal);

private:
	static TemporalUpscaler *upscalers[RSE::VIEWPORT_SCALING_3D_MODE_MAX];
};
