/**************************************************************************/
/*  avboit_policy.h                                                       */
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

#include "core/string/ustring.h"
#include "core/typedefs.h"
#include "core/variant/variant.h"

// GodotNext: avboit (#295, plan §21.2.1). Which transparency method a view gets, and why.
//
// AVBOIT (Adaptive Voxel-Based Order Independent Transparency, UE 5.9 Preview, EXPERIMENTAL) blends
// overlapping translucent surfaces the same way whatever order they are drawn in. GodotNext builds it
// on Forward+ only, with Vulkan, D3D12 and Metal. Every other case keeps the sorted translucency
// Godot always had, and says why once: the project setting (the default), the Mobile and
// Compatibility renderers, a device without the features below, a view with MSAA (UE turns AVBOIT
// off under MSAA too) and a multiview (XR) view. Nothing here touches a device, so the doctests run
// headless; RendererRD::AVBOIT fills DeviceFeatures from the rendering device.
namespace AVBOITPolicy {

// The project setting. 0 Sorted (the default), 1 AVBOIT (EXPERIMENTAL).
static constexpr const char *SETTING_METHOD = "rendering/transparency/method";

enum Method {
	METHOD_SORTED,
	METHOD_AVBOIT,
};

enum Renderer {
	RENDERER_FORWARD_PLUS,
	RENDERER_MOBILE,
	RENDERER_COMPATIBILITY,
};

// In the order evaluate() checks them.
enum Reason {
	REASON_NONE, // AVBOIT draws the view's translucency.
	REASON_SETTING,
	REASON_MOBILE_RENDERER,
	REASON_COMPATIBILITY_RENDERER,
	REASON_DEVICE_FEATURES,
	REASON_MSAA,
	REASON_MULTIVIEW,
	REASON_MAX,
};

// The occlusion pre-pass writes its depth slices to this many RGBA16F color attachments at once.
static constexpr uint32_t OCCLUSION_LAYERS = 4;

// What the AVBOIT passes use beyond Forward+ itself. No raster ordered views, atomics, subgroup
// operations or 64-bit features: the pre-pass and the composite only blend.
struct DeviceFeatures {
	// RGBA16F color attachments that can be sampled and that blend with the add, min and max
	// operations: the occlusion grid and the accumulation target.
	bool rgba16f_attachment = false;
	// RGBA16F storage images in compute: the resolve writes the view's color buffer.
	bool rgba16f_storage = false;
	// The device's limit on color attachments in one pass; the occlusion pre-pass needs OCCLUSION_LAYERS.
	uint32_t max_color_attachments = 0;
};

inline bool device_features_met(const DeviceFeatures &p_features) {
	return p_features.rgba16f_attachment && p_features.rgba16f_storage && p_features.max_color_attachments >= OCCLUSION_LAYERS;
}

inline Method method_from_setting(int p_value) {
	return p_value == METHOD_AVBOIT ? METHOD_AVBOIT : METHOD_SORTED;
}

struct ViewInputs {
	Method method = METHOD_SORTED;
	Renderer renderer = RENDERER_FORWARD_PLUS;
	bool device_features = false;
	bool msaa = false;
	uint32_t view_count = 1;
};

inline Reason evaluate(const ViewInputs &p_inputs) {
	if (p_inputs.method != METHOD_AVBOIT) {
		return REASON_SETTING;
	}
	if (p_inputs.renderer == RENDERER_MOBILE) {
		return REASON_MOBILE_RENDERER;
	}
	if (p_inputs.renderer == RENDERER_COMPATIBILITY) {
		return REASON_COMPATIBILITY_RENDERER;
	}
	if (!p_inputs.device_features) {
		return REASON_DEVICE_FEATURES;
	}
	if (p_inputs.msaa) {
		return REASON_MSAA;
	}
	if (p_inputs.view_count != 1) {
		return REASON_MULTIVIEW;
	}
	return REASON_NONE;
}

inline const char *reason_name(Reason p_reason) {
	switch (p_reason) {
		case REASON_NONE:
			return "NONE";
		case REASON_SETTING:
			return "SETTING";
		case REASON_MOBILE_RENDERER:
			return "MOBILE_RENDERER";
		case REASON_COMPATIBILITY_RENDERER:
			return "COMPATIBILITY_RENDERER";
		case REASON_DEVICE_FEATURES:
			return "DEVICE_FEATURES";
		case REASON_MSAA:
			return "MSAA";
		case REASON_MULTIVIEW:
			return "MULTIVIEW";
		case REASON_MAX:
			break;
	}
	return "UNKNOWN";
}

// The line the renderers print. Checks look for "AVBOIT: on" and "AVBOIT: sorted (reason <NAME>)".
inline String describe(Reason p_reason) {
	switch (p_reason) {
		case REASON_NONE:
			return "AVBOIT: on (EXPERIMENTAL): translucency is blended the same in any draw order.";
		case REASON_SETTING:
			return vformat("AVBOIT: sorted (reason SETTING): %s selects sorted translucency.", SETTING_METHOD);
		case REASON_MOBILE_RENDERER:
			return "AVBOIT: sorted (reason MOBILE_RENDERER): AVBOIT needs the Forward+ renderer; the Mobile renderer keeps sorted translucency.";
		case REASON_COMPATIBILITY_RENDERER:
			return "AVBOIT: sorted (reason COMPATIBILITY_RENDERER): AVBOIT needs the Forward+ renderer; the Compatibility renderer keeps sorted translucency.";
		case REASON_DEVICE_FEATURES:
			return vformat("AVBOIT: sorted (reason DEVICE_FEATURES): the device lacks blendable RGBA16F color attachments, RGBA16F storage images or %d color attachments in one pass.", OCCLUSION_LAYERS);
		case REASON_MSAA:
			return "AVBOIT: sorted (reason MSAA): a view with MSAA keeps sorted translucency.";
		case REASON_MULTIVIEW:
			return "AVBOIT: sorted (reason MULTIVIEW): a multiview (XR) view keeps sorted translucency.";
		case REASON_MAX:
			break;
	}
	return String();
}

// Says each reason once: note() returns the line to print the first time a reason comes up after
// the method last changed, and an empty string otherwise, so a frame loop never repeats it. The
// default (the setting selects sorted) prints nothing until AVBOIT was selected and deselected.
class ReasonLog {
	Method method = METHOD_SORTED;
	uint32_t logged = 1u << REASON_SETTING;

public:
	String note(Method p_method, Reason p_reason) {
		if (p_method != method) {
			method = p_method;
			logged = 0;
		}
		const uint32_t bit = 1u << uint32_t(p_reason);
		if ((logged & bit) != 0) {
			return String();
		}
		logged |= bit;
		return describe(p_reason);
	}
};

} // namespace AVBOITPolicy
