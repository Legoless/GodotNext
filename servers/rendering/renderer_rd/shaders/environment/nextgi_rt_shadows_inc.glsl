/**************************************************************************/
/*  nextgi_rt_shadows_inc.glsl                                            */
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

// GodotNext: rt_shadows (#141, plan §21.3 contract M9). Shared layout and helpers for
// standalone ray-traced shadow tile allocation, ray tracing, and per-light denoising.
// Must match NextGIRTShadows::LightGPU / ParamsGPU in next_gi_rt_shadows.h.

#ifndef NEXTGI_RT_SHADOWS_INC_GLSL
#define NEXTGI_RT_SHADOWS_INC_GLSL

const uint RT_SHADOW_TILE_SIZE = 16u;
const uint RT_SHADOW_ATLAS_TILES_X = 256u;
const uint RT_SHADOW_TILE_NONE = 0xFFFFFFFFu;
const float RT_SHADOW_HIT_MISS = 65504.0;

const uint RT_SHADOW_TYPE_DIRECTIONAL = 0u;
const uint RT_SHADOW_TYPE_OMNI = 1u;
const uint RT_SHADOW_TYPE_SPOT = 2u;
const uint RT_SHADOW_TYPE_AREA = 3u;

const uint RT_SHADOW_PARAM_FLAG_HAS_HISTORY = 1u << 0u;
const uint RT_SHADOW_PARAM_FLAG_VELOCITY = 1u << 1u;
const uint RT_SHADOW_PARAM_FLAG_ORTHOGONAL = 1u << 2u;

// 144 bytes = 9 x vec4, std430.
struct RTShadowLight {
	vec4 pos_range; // xyz: world position (local), w: range (local) or max trace distance (directional).
	vec4 dir_cos; // xyz: world direction (directional: toward light; spot/area: shine dir), w: cos_spot_angle or -1.
	vec4 area_width_size; // xyz: world area width vector, w: source_radius (LIGHT_PARAM_SIZE / directional tan half angle).
	vec4 area_height_trans; // xyz: world area height vector, w: transmittance_bias.
	vec4 view_pos_radius; // xyz: view-space position, w: bounding cull radius (local).
	vec4 view_dir_sin; // xyz: view-space shine direction, w: sin_spot_angle (spot) or 0.
	uvec4 policy_lookup; // x: light_mask_mode (#145), y: caster_mask (#145), z: cur_lookup_base, w: prev_lookup_base.
	uvec4 rect_id; // x: min_tx | (min_ty << 16), y: rect_w | (rect_h << 16), z: stable_id, w: type | (seed << 8).
	vec4 area_cull; // xyz: view-space area half-extents, w: penumbra_scale.
};

// 336 bytes = 21 x vec4, std430.
struct RTShadowParams {
	mat4 inv_view_proj;
	mat4 prev_view_proj;
	mat4 inv_projection;
	vec4 view_to_world[3];
	vec4 depth_coefficients;
	vec4 prev_depth_coeffs;
	vec4 camera_pos_offset; // xyz: camera world pos, w: max_origin_offset (<= 0.05 m per §8.2).
	vec4 footprint_misc; // x: footprint_scale, y: footprint_bias, z: rt_scene_radius, w: 0.0.
	uvec4 screen_tiles; // x: screen_w, y: screen_h, z: tiles_x, w: tiles_y.
	uvec4 counts; // x: rt_light_count, y: tile_budget, z: frame_index, w: flags.
};

ivec2 rt_shadow_tile_atlas_origin(uint p_tile_id) {
	return ivec2(int((p_tile_id & 0xFFu) * RT_SHADOW_TILE_SIZE), int((p_tile_id >> 8u) * RT_SHADOW_TILE_SIZE));
}

ivec2 rt_shadow_atlas_coord(uint p_tile_id, uvec2 p_local_px) {
	return rt_shadow_tile_atlas_origin(p_tile_id) + ivec2(p_local_px);
}

void rt_shadow_build_basis(vec3 p_n, out vec3 r_t, out vec3 r_b) {
	vec3 up = abs(p_n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
	r_t = normalize(cross(up, p_n));
	r_b = cross(p_n, r_t);
}

#endif // NEXTGI_RT_SHADOWS_INC_GLSL
