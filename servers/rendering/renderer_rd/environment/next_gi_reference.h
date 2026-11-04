/**************************************************************************/
/*  next_gi_reference.h                                                   */
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

#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"
#include "servers/rendering/renderer_scene_render.h"

class RenderDataRD;

namespace RendererRD {

uint32_t nextgi_reference_sample_count();

// Closed box, Lambertian faces, uniform emission, no lights: every pixel is Le / (1 - rho).
_FORCE_INLINE_ float nextgi_reference_furnace_equilibrium(float p_le, float p_rho) {
	return p_le / (1.0f - p_rho);
}

// The reference ShaderRD is created only under the HWRT tier.
_FORCE_INLINE_ bool nextgi_reference_shader_allowed(bool p_hwrt) {
	return p_hwrt;
}

// GodotNext: nextgi_reference
// One sample per frame into an RGBA32F sum/count image, then a composite into the HDR color.
class NextGIReference {
	struct ShaderState;
	ShaderState *shader = nullptr;
	RID accumulation;
	RID material_buffer;
	RID light_buffer;
	RID params_buffer;
	RID uniform_set;
	RID dummy_buffer;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t samples = 0;
	uint64_t scene_hash = 0;
	uint64_t read_hash = 0;
	float read_min = 0.0f;
	float read_max = 0.0f;
	float read_mean = 0.0f;
	PackedFloat32Array read_luminance;
	bool scene_invalid = false;
	bool logged_inactive = false;
	bool tables_current = false;
	uint32_t cached_surface_count = 0;
	uint32_t cached_light_count = 0;
	RID estimator_buffer;

	bool _ensure_shader();
	void _free_uniform_set();
	// Logs the inactive-tier error once and reports that the shader must not be created.
	bool _refuse_without_hwrt();
	bool _build_tables(const RenderDataRD *p_render_data, uint32_t &r_surface_count, uint32_t &r_light_count);
	uint64_t _scene_hash(const RenderDataRD *p_render_data) const;
	void _readback();

public:
	void free();
	// Drops the material tables so the next prepare or render rebuilds them.
	void invalidate_tables() { tables_current = false; }
	// Builds the material and light tables and the estimator params. Gather's diagnostic variant reads these.
	bool prepare_estimator(const RenderDataRD *p_render_data, RID &r_materials, RID &r_lights, RID &r_params);
	// Accumulates one camera sample when reference_mode is not off. Runs after the TLAS rebuild.
	void render(const RenderDataRD *p_render_data);
	// Writes sum/count times exposure into the internal HDR color. No-op when the mode is off.
	void composite(RID p_color);
	void fill_info(Dictionary &r_info) const;
	uint32_t get_samples() const { return samples; }
	bool is_active() const;

	NextGIReference() {}
	~NextGIReference() {}
};

} // namespace RendererRD
