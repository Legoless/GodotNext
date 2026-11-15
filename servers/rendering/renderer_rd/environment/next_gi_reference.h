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

// GodotNext: nextgi_vulkan (#49). The shader's mode word (dispatch.w): 0 full, 1 direct only,
// 2 reflections only. The reference_mode setting puts Off first (0 Off, 1 Full, 2 Direct Only,
// 3 Reflections Only), so it is one higher. Passing the setting through unchanged ran Full as
// direct only, which the closed-box furnace caught on Vulkan.
_FORCE_INLINE_ uint32_t nextgi_reference_shader_mode(int p_reference_mode) {
	return p_reference_mode > 1 ? uint32_t(p_reference_mode - 1) : 0u;
}

// GodotNext: nextgi_reference_capture (#263). The accumulation restarts when this key changes. A scene
// reload keeps the seed, the mode, the instance count and the camera origin, so the key also carries an
// explicit epoch (debug/reference_epoch), the whole camera transform and projection, the sky, and a hash
// of the instance transforms and the material and light tables the estimator reads.
struct NextGIReferenceResetKey {
	uint32_t seed = 1;
	uint32_t mode = 0;
	uint32_t epoch = 0;
	uint32_t instance_count = 0;
	Transform3D camera;
	Projection projection;
	Color sky;
	uint64_t tables = 0;
};

// FNV-1a, one byte at a time.
_FORCE_INLINE_ uint64_t nextgi_reference_hash_bytes(uint64_t p_hash, const void *p_data, size_t p_size) {
	const uint8_t *bytes = static_cast<const uint8_t *>(p_data);
	for (size_t i = 0; i < p_size; i++) {
		p_hash = (p_hash ^ bytes[i]) * 1099511628211ull;
	}
	return p_hash;
}

_FORCE_INLINE_ uint64_t nextgi_reference_hash_real(uint64_t p_hash, double p_value) {
	return nextgi_reference_hash_bytes(p_hash, &p_value, sizeof(p_value));
}

_FORCE_INLINE_ uint64_t nextgi_reference_hash_transform(uint64_t p_hash, const Transform3D &p_transform) {
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			p_hash = nextgi_reference_hash_real(p_hash, p_transform.basis.rows[i][j]);
		}
		p_hash = nextgi_reference_hash_real(p_hash, p_transform.origin[i]);
	}
	return p_hash;
}

_FORCE_INLINE_ uint64_t nextgi_reference_reset_hash(const NextGIReferenceResetKey &p_key) {
	uint64_t hash = 1469598103934665603ull;
	const uint32_t words[4] = { p_key.seed, p_key.mode, p_key.epoch, p_key.instance_count };
	hash = nextgi_reference_hash_bytes(hash, words, sizeof(words));
	hash = nextgi_reference_hash_transform(hash, p_key.camera);
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			hash = nextgi_reference_hash_real(hash, p_key.projection.columns[i][j]);
		}
	}
	hash = nextgi_reference_hash_real(hash, p_key.sky.r);
	hash = nextgi_reference_hash_real(hash, p_key.sky.g);
	hash = nextgi_reference_hash_real(hash, p_key.sky.b);
	return nextgi_reference_hash_bytes(hash, &p_key.tables, sizeof(p_key.tables));
}

// GodotNext: nextgi_reference
// One sample per frame into an RGBA32F sum/count image, then a composite into the HDR color.
class NextGIReference {
	struct ShaderState;
	ShaderState *shader = nullptr;
	RID accumulation;
	// GodotNext: nextgi_reference_reflections (#60 step 6).
	RID reflection_guides;
	RID reflection_distance;
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
	// GodotNext: nextgi_reference_capture (#263). Accumulation restarts so far, the hash of the
	// instance transforms and the material and light tables, the exposure the composite applies,
	// and the raw RGBA32F accumulation (radiance sum, sample count) the last readback saw.
	uint32_t resets = 0;
	uint64_t tables_hash = 0;
	float exposure = 1.0f;
	PackedByteArray read_accumulation;
	// GodotNext: rr_guides (#205). The raw RGBA32F reflection guides of the reflections-only
	// mode (hit distance sum, hit count, sample count) the last readback saw.
	PackedByteArray read_reflection_guides;

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
	// GodotNext: nextgi_vulkan (#50). The variant defines the runtime ShaderRD is initialized with, shared
	// with the shader baker's bake-only instance (NextGIBakeShaders) so both resolve to the same cache path.
	static Vector<String> get_shader_variant_defines();
	uint32_t get_samples() const { return samples; }
	// Mean first-hit distance (r) and hit fraction (g) of the reflection-only reference.
	RID get_reflection_distance() const { return reflection_distance; }
	bool is_active() const;

	NextGIReference() {}
	~NextGIReference() {}
};

} // namespace RendererRD
