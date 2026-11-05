/**************************************************************************/
/*  temporal_upscaler.cpp                                                 */
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

#include "servers/rendering/renderer_rd/effects/temporal_upscaler.h"

TemporalUpscaler *TemporalUpscalerRegistry::upscalers[RSE::VIEWPORT_SCALING_3D_MODE_MAX] = {};

void TemporalUpscalerRegistry::register_upscaler(RSE::ViewportScaling3DMode p_mode, TemporalUpscaler *p_upscaler) {
	ERR_FAIL_INDEX(p_mode, RSE::VIEWPORT_SCALING_3D_MODE_MAX);
	upscalers[p_mode] = p_upscaler;
}

void TemporalUpscalerRegistry::unregister_upscaler(RSE::ViewportScaling3DMode p_mode) {
	ERR_FAIL_INDEX(p_mode, RSE::VIEWPORT_SCALING_3D_MODE_MAX);
	upscalers[p_mode] = nullptr;
}

TemporalUpscaler *TemporalUpscalerRegistry::get(RSE::ViewportScaling3DMode p_mode) {
	ERR_FAIL_INDEX_V(p_mode, RSE::VIEWPORT_SCALING_3D_MODE_MAX, nullptr);
	return upscalers[p_mode];
}

void TemporalUpscalerRegistry::clear() {
	for (int mode = 0; mode < RSE::VIEWPORT_SCALING_3D_MODE_MAX; mode++) {
		upscalers[mode] = nullptr;
	}
}

String TemporalUpscalerRegistry::build_mode_hint(bool p_metal) {
	Vector<String> hints = { "Nearest (Fastest):5", "Bilinear (Fastest):0", "FSR 1.0 (Fast):1", "FSR 2.2 (Slow):2" };
	if (p_metal) {
		hints.push_back("MetalFX (Spatial - Fast):3");
		hints.push_back("MetalFX (Temporal - Slow):4");
	}
	if (get(RSE::VIEWPORT_SCALING_3D_MODE_DLSS) != nullptr) {
		hints.push_back("DLSS (Slow):6");
	}
	return String(",").join(hints);
}
