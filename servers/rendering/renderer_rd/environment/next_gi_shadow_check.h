/**************************************************************************/
/*  next_gi_shadow_check.h                                                */
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

#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/renderer_rd/environment/next_gi_shadow_policy.h"

namespace RendererRD {

class NextGIRTScene;

// GodotNext: nextgi_shadow_policy (#145). GM-4's caster-mask matrix on the GPU: the debug setting
// rendering/global_illumination/nextgi/debug/shadow_ray_check traces NEXTGI_SHADOW_CHECK_RAYS rays (8 caster masks
// over the scene's 20 layers) through nextgi_shadow_ray_inc.glsl's trace_shadow() every frame, against this
// frame's TLAS and caster-mask allocation, and counts the rays whose occlusion matches the raster caster rule
// (next_gi_shadow_policy.h). RenderingServer.nextgi_get_counters() reports the counts as shadow_check_*, a frame
// or more later.

enum NextGIShadowCheckCounter {
	NEXTGI_SHADOW_CHECK_COUNTER_RAYS,
	NEXTGI_SHADOW_CHECK_COUNTER_AGREE,
	NEXTGI_SHADOW_CHECK_COUNTER_DISAGREE,
	NEXTGI_SHADOW_CHECK_COUNTER_EXPECTED_OCCLUDED,
	NEXTGI_SHADOW_CHECK_COUNTER_TRACED_OCCLUDED,
	NEXTGI_SHADOW_CHECK_COUNTER_FAST_RAYS, // Lights on the opaque fast path.
	NEXTGI_SHADOW_CHECK_COUNTER_LOOP_RAYS, // Allocated lights with an alpha-tested caster: the candidate loop.
	NEXTGI_SHADOW_CHECK_COUNTER_FALLBACK_RAYS, // Lights without an allocated bit.
	NEXTGI_SHADOW_CHECK_COUNTER_FAST_DISAGREE,
	NEXTGI_SHADOW_CHECK_COUNTER_LOOP_DISAGREE,
	NEXTGI_SHADOW_CHECK_COUNTER_FALLBACK_DISAGREE,
	NEXTGI_SHADOW_CHECK_COUNTER_ALPHA_SAMPLES,
	NEXTGI_SHADOW_CHECK_COUNTER_FIRST_DISAGREEMENT, // The lowest ray index that disagrees (UINT32_MAX: none).
	NEXTGI_SHADOW_CHECK_COUNTER_MAX = 16,
};

const char *nextgi_shadow_check_counter_name(NextGIShadowCheckCounter p_counter);

class NextGIShadowCheck {
	struct Internal;
	Internal *internal = nullptr;

	bool _ensure_shader();
	static void _counters_received(const Vector<uint8_t> &p_data, uint64_t p_token);

public:
	// After this frame's TLAS phase (the TLAS must be traceable). Rebuilds the rays when the scene changed.
	void run(NextGIRTScene *p_rt_scene);
	void free();
	// The last results that came back (shadow_check_* keys), with the allocation they ran against.
	static void fill_counters(Dictionary &r_counters);

	NextGIShadowCheck();
	~NextGIShadowCheck();
};

} // namespace RendererRD
