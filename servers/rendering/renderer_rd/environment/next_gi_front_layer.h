/**************************************************************************/
/*  next_gi_front_layer.h                                                 */
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
#include "core/templates/rid.h"
#include "core/variant/dictionary.h"

class RenderDataRD;

namespace RendererRD {
class NextGI;
class NextGIGather;
class CopyEffects;

_FORCE_INLINE_ bool nextgi_front_layer_fragment_visible(float p_alpha, float p_fragment_depth, float p_opaque_depth) {
	return p_alpha >= 0.1f && p_fragment_depth >= p_opaque_depth;
}

_FORCE_INLINE_ bool nextgi_front_layer_depth_matches(float p_depth_a, float p_depth_b) {
	return Math::abs(p_depth_a - p_depth_b) <= 1.0e-4f;
}

_FORCE_INLINE_ bool nextgi_front_layer_allowed(bool p_project_allow, bool p_environment_enabled, bool p_hwrt) {
	return p_project_allow && p_environment_enabled && p_hwrt;
}

class NextGIFrontLayer {
	NextGI *owner = nullptr;
	NextGIGather *gather = nullptr;
	RID framebuffer;
	RID depth;
	RID normal_roughness;
	RID reflection;
	RID hit_distance;
	RID opaque_depth;
	Size2i size;
	uint32_t dispatch_count = 0;

public:
	void set_owner(NextGI *p_owner);
	bool ensure_targets(const Size2i &p_size);
	bool copy_opaque_depth(RID p_source, CopyEffects *p_copy_effects);
	void trace(RenderDataRD *p_render_data);
	void free_resources();
	void fill_info(Dictionary &r_info) const;

	RID get_framebuffer() const { return framebuffer; }
	RID get_depth() const { return depth; }
	RID get_normal_roughness() const { return normal_roughness; }
	RID get_reflection() const { return reflection; }
	RID get_opaque_depth() const { return opaque_depth; }

	~NextGIFrontLayer();
};

} // namespace RendererRD
