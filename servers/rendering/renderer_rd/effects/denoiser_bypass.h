/**************************************************************************/
/*  denoiser_bypass.h                                                     */
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

// GodotNext: pu_dlss_rr (#206, plan §21.5 U4).
// Vendor-neutral denoiser bypass contract: built-in denoisers (NextGI line 8,
// MegaLights denoiser, and future PL/PM denoisers) register themselves here and
// skip their temporal/spatial filtering passes for any RenderSceneBuffersRD
// currently bypassed by an external ray-reconstruction upscaler.

#include "core/string/string_name.h"
#include "core/templates/vector.h"

class RenderSceneBuffersRD;

class DenoiserBypass {
public:
	static void register_denoiser(const StringName &p_name);
	static Vector<StringName> get_registered_denoisers();
	static void set_bypassed(const RenderSceneBuffersRD *p_rb, bool p_bypassed);
	static bool is_bypassed(const RenderSceneBuffersRD *p_rb);
	static void clear_rb(const RenderSceneBuffersRD *p_rb);
	static void clear_for_tests();
};

namespace RendererRD {
using ::DenoiserBypass;
} // namespace RendererRD
