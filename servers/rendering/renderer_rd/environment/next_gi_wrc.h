/**************************************************************************/
/*  next_gi_wrc.h                                                         */
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

#include "core/math/projection.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "core/math/vector3i.h"
#include "core/math/vector4.h"
#include "core/templates/rid.h"
#include "core/variant/dictionary.h"

class RenderDataRD;
class RenderSceneDataRD;

namespace RendererRD {

// World Radiance Cache (D15). 4 clipmaps of 16³ probes. The byte total is the packed layout
// without history: 896 B/probe * 16384 probes = 14 MiB.
enum {
	NEXTGI_WRC_LEVELS = 4,
	NEXTGI_WRC_GRID = 16,
	NEXTGI_WRC_PROBE_COUNT = NEXTGI_WRC_LEVELS * NEXTGI_WRC_GRID * NEXTGI_WRC_GRID * NEXTGI_WRC_GRID,
	NEXTGI_WRC_PROBE_BYTES = 896,
	NEXTGI_WRC_PROBES_PER_FRAME = 512,
};

static_assert(NEXTGI_WRC_PROBE_COUNT == 16384, "The WRC is 4 * 16^3 probes.");
static_assert(uint64_t(NEXTGI_WRC_PROBE_BYTES) * NEXTGI_WRC_PROBE_COUNT == 14ull * 1024ull * 1024ull, "The WRC layout is 14 MiB.");

_FORCE_INLINE_ uint64_t nextgi_wrc_bytes() {
	return uint64_t(NEXTGI_WRC_PROBE_BYTES) * NEXTGI_WRC_PROBE_COUNT;
}

_FORCE_INLINE_ float nextgi_wrc_spacing(uint32_t p_level) {
	return float(1u << MIN(p_level, uint32_t(NEXTGI_WRC_LEVELS - 1)));
}

// Finest clipmap whose half-extent (8 * spacing) contains the distance from the camera.
uint32_t nextgi_wrc_level_for_distance(float p_distance);

// Diffuse rays stop at 1.5x the spacing of that level.
_FORCE_INLINE_ float nextgi_wrc_handoff_distance(float p_distance) {
	return 1.5f * nextgi_wrc_spacing(nextgi_wrc_level_for_distance(p_distance));
}

// Each level origin snaps to its spacing.
Vector3 nextgi_wrc_snap_origin(const Vector3 &p_camera, float p_spacing);

// GodotNext: nextgi_wrc_shift (#254). Probes are addressed by cell from each level's snapped
// origin, so a level whose origin moved holds history traced at other positions. Its probes take
// their next trace without blending ("fresh") for one full update cycle (FRESH_FRAMES), in which
// the update cursor reaches every probe once; the cursor is not moved, so continuous motion cannot
// starve the coarser levels.
static constexpr uint32_t NEXTGI_WRC_FRESH_FRAMES = 16384 / 512;
// Bit L is set when level L's snapped origin (level 0's origin snapped to 2^L m) moved.
uint32_t nextgi_wrc_shifted_levels(const Vector3 &p_previous_origin, const Vector3 &p_origin, bool p_had_previous);
void nextgi_wrc_mark_fresh(uint32_t p_shifted_levels, uint32_t r_fresh_frames[4]);
uint32_t nextgi_wrc_fresh_mask(const uint32_t p_fresh_frames[4]);
void nextgi_wrc_age_fresh(uint32_t r_fresh_frames[4]);

// A probe with more than 25% back-face hits moves along the mean escape direction, at most 0.4x spacing.
Vector3 nextgi_wrc_relocate(const Vector3 &p_probe, const Vector3 &p_mean_escape, float p_backface_fraction, float p_spacing);

// Translucency probes occupy the two fine WRC levels, with a bounded per-frame trace set.
static const uint32_t NEXTGI_TRANSLUCENCY_PROBE_CAP = 4096;
int32_t nextgi_translucency_probe_index(const Vector3 &p_world, const Vector3 &p_camera, const Vector3 &p_origin);
uint32_t nextgi_translucency_volume_level(float p_distance, bool p_lite);
float nextgi_translucency_history_weight(bool p_valid, const Vector2 &p_previous_uv);

// One cache per scenario. The frame block's "WRC update" slot traces a budget of probes.
class NextGIWRC {
	static NextGIWRC *singleton;

	struct ShaderState;
	ShaderState *shader = nullptr;
	// GodotNext: ps_swrt_trace (#186). The software tier's sibling (nextgi_wrc_update_swrt.glsl).
	struct SWRTShaderState;
	SWRTShaderState *swrt_shader = nullptr;
	RID swrt_trace_set;

	RID radiance;
	RID mip4;
	RID mip2;
	RID visibility;
	RID sh_r;
	RID sh_g;
	RID sh_b;
	RID offsets;
	RID params_buffer;
	// GodotNext: nextgi_translucency_volume (#61). 1/16-screen x 32-slice SH volume.
	RID translucency_r;
	RID translucency_g;
	RID translucency_b;
	RID translucency_history_r;
	RID translucency_history_g;
	RID translucency_history_b;
	RID translucency_radiance;
	RID translucency_mip4;
	RID translucency_mip2;
	RID translucency_visibility;
	RID translucency_sh_r;
	RID translucency_sh_g;
	RID translucency_sh_b;
	RID translucency_offsets;
	RID translucency_mark_texture;
	RID translucency_mark_framebuffer;
	RID translucency_marks;
	RID translucency_indices;
	RID translucency_uniform_set;
	RID translucency_mark_uniform_set;
	Size2i translucency_mark_size;
	Vector3i translucency_size;
	RID counter_buffer;
	RID uniform_set;
	RID volume_uniform_set;
	RID sampler;
	RID dummy_tex;
	RID dummy_buffer;
	bool packed_radiance = true;
	uint32_t cursor = 0;
	// GodotNext: nextgi_wrc_shift (#254). The level-0 origin of the last update, and the frames each
	// level stays fresh after its origin moved.
	Vector3 previous_origin;
	bool has_previous_origin = false;
	uint32_t fresh_frames[4] = {};
	uint32_t frame = 0;
	uint32_t last_rays = 0;
	uint32_t last_hits = 0;
	uint32_t last_sky = 0;
	uint32_t last_energy_bits = 0;
	uint32_t fog_samples = 0;
	uint32_t fog_ambient_bits = 0;
	uint32_t translucency_marked = 0;
	uint32_t translucency_rays = 0;
	uint32_t translucency_hits = 0;
	float translucency_sky_energy = 0.0f;
	uint32_t last_sdf_hits = 0;
	uint32_t last_sdf_exhausted = 0;
	uint32_t last_sdf_near = 0;
	bool volume_history_valid = false;
	bool has_previous_camera = false;
	bool previous_sky_read = false;
	// GodotNext: ps_lumen_lite (#183). The camera the probe grid snapped to at the last params
	// upload: the frame's first view, since secondary views reuse the scenario's update.
	Vector3 grid_camera;
	bool grid_camera_valid = false;
	Projection previous_view_projection;
	Vector3 previous_camera_position;
	Vector3 previous_camera_forward;

	bool _ensure_textures();
	bool _ensure_translucency_cache();
	bool _ensure_shader();
	bool _ensure_swrt_shader();
	void _free_uniform_set();
	// GodotNext: nextgi_async_counters (#186). One asynchronous read of the previous frame's
	// counters (fog included) before the slot zeroes them; a synchronous read inside the frame
	// block restarts the device frame and drops the block's timestamps.
	void _request_counters();
	static void _counters_received(const Vector<uint8_t> &p_data, uint64_t p_token);
	RID readback_buffer;
	uint64_t counter_token = 0;
	bool counters_pending = false;

public:
	static NextGIWRC *get_singleton() { return singleton; }

	// GodotNext: nextgi_view_math. The view the froxels and the history use, from NextGIView:
	// the depth prepass's view-projection, its inverse, and the depth coefficients
	// (NextGIViewMath::depth_coefficients()) that turn a froxel's linear depth into the
	// depth texture's value.
	struct ViewParams {
		Projection view_proj;
		Projection inv_view_proj;
		Vector4 depth_coefficients;
	};
	static ViewParams view_params(const RenderSceneDataRD &p_scene_data);
	// The expanded update shader source of one variant, for the layout doctest. No GPU compile.
	static String build_update_shader_source(int p_variant = 0);
	// GodotNext: ps_swrt_trace (#186). The software sibling's expanded source, for the doctests.
	static String build_update_swrt_shader_source(int p_variant = 0);
	// GodotNext: nextgi_vulkan (#50). The variant defines the runtime ShaderRD is initialized with, shared
	// with the shader baker's bake-only instance (NextGIBakeShaders) so both resolve to the same cache path.
	static Vector<String> get_shader_variant_defines();

	void update(const RenderDataRD *p_render_data);
	void fill_info(Dictionary &r_info) const;

	RID get_radiance() const { return radiance; }
	RID get_visibility() const { return visibility; }
	RID get_offsets() const { return offsets; }
	RID get_mip2() const { return mip2; }
	// GodotNext: ps_lumen_lite (#183). Lite's rough specular reads mip 4 and mip 2.
	RID get_mip4() const { return mip4; }
	// Every level's origin is nextgi_wrc_snap_origin() of this camera. Lite views other than the
	// first one of a frame read the probes on this grid, not on one snapped to their own camera.
	bool has_grid_camera() const { return grid_camera_valid; }
	Vector3 get_grid_camera() const { return grid_camera; }
	RID get_sh_r() const { return sh_r; }
	RID get_sh_g() const { return sh_g; }
	RID get_sh_b() const { return sh_b; }
	RID get_params_buffer() const { return params_buffer; }
	RID get_translucency_r() const { return translucency_r; }
	RID get_translucency_g() const { return translucency_g; }
	RID get_translucency_b() const { return translucency_b; }
	Vector3i get_translucency_size() const { return translucency_size; }
	RID get_counter_buffer() const { return counter_buffer; }
	bool is_ready() const { return radiance.is_valid(); }
	RID get_translucency_mark_target(const Size2i &p_size);
	RID get_translucency_mark_texture() const { return translucency_mark_texture; }
	bool uses_translucency_probes() const;

	void free_resources();

	NextGIWRC();
	~NextGIWRC();
};

} // namespace RendererRD
