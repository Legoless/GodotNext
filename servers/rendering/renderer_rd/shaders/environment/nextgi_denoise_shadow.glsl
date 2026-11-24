/**************************************************************************/
/*  nextgi_denoise_shadow.glsl                                            */
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

#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: rt_shadows (#141, plan §21.3 contract M9). Per-light shadow denoiser over the
// tile-sparse R8 shadow-mask atlas:
// - MODE_TEMPORAL: reprojects with prepass velocity (#37) or prev_view_proj into the twin
//   history atlas, invalidating history when a tile's light ID changes or on depth/normal disocclusion.
// - MODE_SPATIAL: 2 à-trous spatial passes guided by depth, normal, and penumbra width
//   (hit_distance / light_size), sampling seamlessly across 16x16 tile boundaries via tile_lookup.
// - MODE_COPY_PREV_GUIDE: stores view-space normal + linear depth for next frame's disocclusion check.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "nextgi_rt_shadows_inc.glsl"
#include "nextgi_view_inc.glsl"

#if defined(MODE_TEMPORAL)

layout(set = 0, binding = 0) uniform sampler2D raw_mask_tex;
layout(set = 0, binding = 1) uniform sampler2D history_mask_prev_tex;
layout(r8ui, set = 0, binding = 2) uniform restrict readonly uimage2D history_len_prev_img;
layout(set = 0, binding = 3) uniform sampler2D depth_tex;
layout(set = 0, binding = 4) uniform sampler2D normal_roughness_tex;
layout(set = 0, binding = 5) uniform sampler2D velocity_tex;
layout(set = 0, binding = 6) uniform sampler2D prev_depth_normal_tex;

layout(set = 0, binding = 7, std430) restrict readonly buffer RTShadowParamsBuf {
	RTShadowParams data;
}
params_buf;
layout(set = 0, binding = 8, std430) restrict readonly buffer RTShadowLightsBuf {
	RTShadowLight data[];
}
lights_buf;
layout(set = 0, binding = 9, std430) restrict readonly buffer RTShadowTileList {
	uvec4 data[];
}
tile_list;
layout(set = 0, binding = 10, std430) restrict readonly buffer RTShadowAllocState {
	uint tile_count;
	uint active_tiles;
	uint overflow_tiles;
	uint overflow_lights;
	uint sink;
	uint pad0;
	uint pad1;
	uint pad2;
}
alloc_state;
layout(set = 0, binding = 11, std430) restrict readonly buffer RTShadowTileLookupCur {
	uint data[];
}
cur_lookup;
layout(set = 0, binding = 12, std430) restrict readonly buffer RTShadowTileLookupPrev {
	uint data[];
}
prev_lookup;
layout(set = 0, binding = 13, std430) restrict readonly buffer RTShadowTileOwnerPrev {
	uvec2 data[];
}
prev_owner;

layout(r8, set = 0, binding = 14) uniform restrict writeonly image2D history_mask_cur_img;
layout(r8ui, set = 0, binding = 15) uniform restrict writeonly uimage2D history_len_cur_img;

uint rt_lookup_cur_tile(uint p_base, uvec2 p_st) {
	uint pmin = cur_lookup.data[p_base];
	uint pdim = cur_lookup.data[p_base + 1u];
	uvec2 min_t = uvec2(pmin & 0xFFFFu, pmin >> 16u);
	uvec2 dim_t = uvec2(pdim & 0xFFFFu, pdim >> 16u);
	if (p_st.x < min_t.x || p_st.y < min_t.y) {
		return RT_SHADOW_TILE_NONE;
	}
	uvec2 rel = p_st - min_t;
	if (rel.x >= dim_t.x || rel.y >= dim_t.y) {
		return RT_SHADOW_TILE_NONE;
	}
	uint entry = cur_lookup.data[p_base + 2u + rel.y * dim_t.x + rel.x];
	return entry > 0u ? (entry - 1u) : RT_SHADOW_TILE_NONE;
}

uint rt_lookup_prev_tile(uint p_prev_base, uvec2 p_st, uint p_stable_id, uint p_tile_budget) {
	if (p_prev_base == RT_SHADOW_TILE_NONE) {
		return RT_SHADOW_TILE_NONE;
	}
	uint pmin = prev_lookup.data[p_prev_base];
	uint pdim = prev_lookup.data[p_prev_base + 1u];
	uvec2 min_t = uvec2(pmin & 0xFFFFu, pmin >> 16u);
	uvec2 dim_t = uvec2(pdim & 0xFFFFu, pdim >> 16u);
	if (p_st.x < min_t.x || p_st.y < min_t.y) {
		return RT_SHADOW_TILE_NONE;
	}
	uvec2 rel = p_st - min_t;
	if (rel.x >= dim_t.x || rel.y >= dim_t.y) {
		return RT_SHADOW_TILE_NONE;
	}
	uint entry = prev_lookup.data[p_prev_base + 2u + rel.y * dim_t.x + rel.x];
	if (entry == 0u) {
		return RT_SHADOW_TILE_NONE;
	}
	uint prev_tile = entry - 1u;
	if (prev_tile >= p_tile_budget) {
		return RT_SHADOW_TILE_NONE;
	}
	// Invalidate history when a tile's light ID or screen tile changed.
	uvec2 owner = prev_owner.data[prev_tile];
	uint packed_st = p_st.x | (p_st.y << 16u);
	if (owner.x != p_stable_id || owner.y != packed_st) {
		return RT_SHADOW_TILE_NONE;
	}
	return prev_tile;
}

void main() {
	uint tile_id = gl_WorkGroupID.z * 256u + (gl_WorkGroupID.y >> 1u);
	if (tile_id >= alloc_state.active_tiles) {
		return;
	}
	uvec2 local_px = uvec2(gl_WorkGroupID.x * 8u + gl_LocalInvocationID.x, (gl_WorkGroupID.y & 1u) * 8u + gl_LocalInvocationID.y);
	ivec2 atlas_px = rt_shadow_atlas_coord(tile_id, local_px);

	RTShadowParams params = params_buf.data;
	uvec4 tile = tile_list.data[tile_id];
	uint light_idx = tile.x;
	uvec2 st = uvec2(tile.y & 0xFFFFu, tile.y >> 16u);
	uvec2 screen_px = st * RT_SHADOW_TILE_SIZE + local_px;

	float raw = texelFetch(raw_mask_tex, atlas_px, 0).r;
	if (screen_px.x >= params.screen_tiles.x || screen_px.y >= params.screen_tiles.y) {
		imageStore(history_mask_cur_img, atlas_px, vec4(raw, 0.0, 0.0, 0.0));
		imageStore(history_len_cur_img, atlas_px, uvec4(1u, 0u, 0u, 0u));
		return;
	}

	float depth = texelFetch(depth_tex, ivec2(screen_px), 0).r;
	if (nextgi_view_is_background(depth)) {
		imageStore(history_mask_cur_img, atlas_px, vec4(1.0, 0.0, 0.0, 0.0));
		imageStore(history_len_cur_img, atlas_px, uvec4(0u, 0u, 0u, 0u));
		return;
	}

	RTShadowLight light = lights_buf.data[light_idx];
	bool has_history = (params.counts.w & RT_SHADOW_PARAM_FLAG_HAS_HISTORY) != 0u;
	bool use_velocity = (params.counts.w & RT_SHADOW_PARAM_FLAG_VELOCITY) != 0u;

	if (!has_history || light.policy_lookup.w == RT_SHADOW_TILE_NONE) {
		imageStore(history_mask_cur_img, atlas_px, vec4(raw, 0.0, 0.0, 0.0));
		imageStore(history_len_cur_img, atlas_px, uvec4(1u, 0u, 0u, 0u));
		return;
	}

	vec2 uv = (vec2(screen_px) + 0.5) / vec2(params.screen_tiles.xy);
	vec4 guide = texelFetch(normal_roughness_tex, ivec2(screen_px), 0);
	vec3 view_normal = normalize(guide.xyz * 2.0 - 1.0);
	vec3 world = nextgi_view_reconstruct(params.inv_view_proj, uv, depth);
	vec3 prev_proj = nextgi_view_project(params.prev_view_proj, world);

	vec2 prev_uv = prev_proj.xy;
	bool prev_valid = prev_proj.z >= 0.0;
	if (use_velocity && guide.a > 0.5) {
		// Dynamic surfaces follow prepass motion vectors (#37; motion = prev_uv - cur_uv).
		vec2 motion = texelFetch(velocity_tex, ivec2(screen_px), 0).xy;
		prev_uv = uv + motion;
		prev_valid = true;
	}
	prev_valid = prev_valid && all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThan(prev_uv, vec2(1.0)));

	float expected_prev_z = prev_proj.z > 0.0 ? max(nextgi_view_linear_depth(params.prev_depth_coeffs, prev_proj.z), 1e-4) : max(nextgi_view_linear_depth(params.depth_coefficients, depth), 1e-4);

	float accum = raw;
	uint new_len = 1u;
	if (prev_valid) {
		ivec2 prev_px = clamp(ivec2(floor(prev_uv * vec2(params.screen_tiles.xy))), ivec2(0), ivec2(params.screen_tiles.xy) - ivec2(1));
		uvec2 prev_st = uvec2(prev_px) >> 4u;
		uint prev_tile = rt_lookup_prev_tile(light.policy_lookup.w, prev_st, light.rect_id.z, params.counts.y);
		if (prev_tile != RT_SHADOW_TILE_NONE) {
			vec4 prev_guide = texelFetch(prev_depth_normal_tex, prev_px, 0);
			float prev_z = prev_guide.w;
			float rel_depth = abs(prev_z - expected_prev_z) / max(expected_prev_z, 0.05);
			float ndot = dot(view_normal, prev_guide.xyz);
			if (prev_z > 0.0 && rel_depth <= 0.1 && ndot >= 0.85) {
				uvec2 prev_local = uvec2(prev_px) & uvec2(15u);
				ivec2 prev_atlas_px = rt_shadow_atlas_coord(prev_tile, prev_local);
				float history_val = texelFetch(history_mask_prev_tex, prev_atlas_px, 0).r;
				uint prev_len = imageLoad(history_len_prev_img, prev_atlas_px).r;

				// 3x3 neighborhood variance clamp over the current light's tiles to prevent ghost trails (Q10).
				float m1 = 0.0;
				float m2 = 0.0;
				float count = 0.0;
				ivec2 max_s = ivec2(params.screen_tiles.xy) - ivec2(1);
				for (int oy = -1; oy <= 1; oy++) {
					for (int ox = -1; ox <= 1; ox++) {
						ivec2 np = clamp(ivec2(screen_px) + ivec2(ox, oy), ivec2(0), max_s);
						uvec2 nst = uvec2(np) >> 4u;
						uint ntile = (nst == st) ? tile_id : rt_lookup_cur_tile(light.policy_lookup.z, nst);
						if (ntile != RT_SHADOW_TILE_NONE) {
							ivec2 nap = rt_shadow_atlas_coord(ntile, uvec2(np) & uvec2(15u));
							float s = texelFetch(raw_mask_tex, nap, 0).r;
							m1 += s;
							m2 += s * s;
							count += 1.0;
						}
					}
				}
				float mean = count > 0.0 ? (m1 / count) : raw;
				float variance = count > 0.0 ? max(m2 / count - mean * mean, 0.0) : 0.0;
				float sigma = sqrt(variance);
				// Hard shadows (source_radius == 0) need tighter clamping for responsive dynamic casters.
				float k_sigma = light.area_width_size.w > 1e-5 ? 1.5 : 1.0;
				float clamped_hist = clamp(history_val, mean - k_sigma * sigma, mean + k_sigma * sigma);

				uint max_len = light.area_width_size.w > 1e-5 ? 16u : 8u;
				new_len = min(prev_len + 1u, max_len);
				float alpha = max(1.0 / float(new_len), 1.0 / float(max_len));
				accum = mix(clamped_hist, raw, alpha);
			}
		}
	}

	imageStore(history_mask_cur_img, atlas_px, vec4(clamp(accum, 0.0, 1.0), 0.0, 0.0, 0.0));
	imageStore(history_len_cur_img, atlas_px, uvec4(new_len, 0u, 0u, 0u));
}

#elif defined(MODE_SPATIAL)

layout(set = 0, binding = 0) uniform sampler2D source_mask_tex;
layout(set = 0, binding = 1) uniform sampler2D hit_dist_tex;
layout(set = 0, binding = 2) uniform sampler2D depth_tex;
layout(set = 0, binding = 3) uniform sampler2D normal_roughness_tex;

layout(set = 0, binding = 4, std430) restrict readonly buffer RTShadowParamsBuf {
	RTShadowParams data;
}
params_buf;
layout(set = 0, binding = 5, std430) restrict readonly buffer RTShadowLightsBuf {
	RTShadowLight data[];
}
lights_buf;
layout(set = 0, binding = 6, std430) restrict readonly buffer RTShadowTileList {
	uvec4 data[];
}
tile_list;
layout(set = 0, binding = 7, std430) restrict readonly buffer RTShadowAllocState {
	uint tile_count;
	uint active_tiles;
	uint overflow_tiles;
	uint overflow_lights;
	uint sink;
	uint pad0;
	uint pad1;
	uint pad2;
}
alloc_state;
layout(set = 0, binding = 8, std430) restrict readonly buffer RTShadowTileLookupCur {
	uint data[];
}
cur_lookup;

layout(r8, set = 0, binding = 9) uniform restrict writeonly image2D output_mask_image;

layout(push_constant, std430) uniform Push {
	uint step_size;
	uint pad0;
	uint pad1;
	uint pad2;
}
push;

uint rt_lookup_cur_tile(uint p_base, uvec2 p_st) {
	uint pmin = cur_lookup.data[p_base];
	uint pdim = cur_lookup.data[p_base + 1u];
	uvec2 min_t = uvec2(pmin & 0xFFFFu, pmin >> 16u);
	uvec2 dim_t = uvec2(pdim & 0xFFFFu, pdim >> 16u);
	if (p_st.x < min_t.x || p_st.y < min_t.y) {
		return RT_SHADOW_TILE_NONE;
	}
	uvec2 rel = p_st - min_t;
	if (rel.x >= dim_t.x || rel.y >= dim_t.y) {
		return RT_SHADOW_TILE_NONE;
	}
	uint entry = cur_lookup.data[p_base + 2u + rel.y * dim_t.x + rel.x];
	return entry > 0u ? (entry - 1u) : RT_SHADOW_TILE_NONE;
}

void main() {
	uint tile_id = gl_WorkGroupID.z * 256u + (gl_WorkGroupID.y >> 1u);
	if (tile_id >= alloc_state.active_tiles) {
		return;
	}
	uvec2 local_px = uvec2(gl_WorkGroupID.x * 8u + gl_LocalInvocationID.x, (gl_WorkGroupID.y & 1u) * 8u + gl_LocalInvocationID.y);
	ivec2 atlas_px = rt_shadow_atlas_coord(tile_id, local_px);

	RTShadowParams params = params_buf.data;
	uvec4 tile = tile_list.data[tile_id];
	uint light_idx = tile.x;
	uvec2 st = uvec2(tile.y & 0xFFFFu, tile.y >> 16u);
	uvec2 screen_px = st * RT_SHADOW_TILE_SIZE + local_px;

	float center_val = texelFetch(source_mask_tex, atlas_px, 0).r;
	if (screen_px.x >= params.screen_tiles.x || screen_px.y >= params.screen_tiles.y) {
		imageStore(output_mask_image, atlas_px, vec4(center_val, 0.0, 0.0, 0.0));
		return;
	}

	float depth = texelFetch(depth_tex, ivec2(screen_px), 0).r;
	if (nextgi_view_is_background(depth)) {
		imageStore(output_mask_image, atlas_px, vec4(1.0, 0.0, 0.0, 0.0));
		return;
	}

	RTShadowLight light = lights_buf.data[light_idx];
	float light_size = light.area_width_size.w;
	// Hard shadow lights (size == 0) preserve razor-sharp edges without spatial blurring.
	if (!(light_size > 1e-5)) {
		imageStore(output_mask_image, atlas_px, vec4(center_val, 0.0, 0.0, 0.0));
		return;
	}

	float center_z = max(nextgi_view_linear_depth(params.depth_coefficients, depth), 1e-4);
	vec3 center_normal = normalize(texelFetch(normal_roughness_tex, ivec2(screen_px), 0).xyz * 2.0 - 1.0);
	ivec2 max_s = ivec2(params.screen_tiles.xy) - ivec2(1);

	// Estimate local blocker hit distance from center + 4-neighbor cross so penumbra edges blur smoothly.
	float min_hit = texelFetch(hit_dist_tex, atlas_px, 0).r;
	const ivec2 cross_ofs[4] = ivec2[4](ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1));
	for (int k = 0; k < 4; k++) {
		ivec2 np = clamp(ivec2(screen_px) + cross_ofs[k] * int(push.step_size), ivec2(0), max_s);
		uvec2 nst = uvec2(np) >> 4u;
		uint ntile = (nst == st) ? tile_id : rt_lookup_cur_tile(light.policy_lookup.z, nst);
		if (ntile != RT_SHADOW_TILE_NONE) {
			float nh = texelFetch(hit_dist_tex, rt_shadow_atlas_coord(ntile, uvec2(np) & uvec2(15u)), 0).r;
			if (nh > 0.0 && nh < min_hit) {
				min_hit = nh;
			}
		}
	}

	// Compute penumbra width in screen pixels from blocker hit distance and light_size.
	float penumbra_px = 0.0;
	if (min_hit < RT_SHADOW_HIT_MISS * 0.5) {
		uint ltype = light.rect_id.w & 3u;
		float penumbra_world;
		if (ltype == RT_SHADOW_TYPE_DIRECTIONAL) {
			penumbra_world = min_hit * light_size;
		} else {
			vec2 uv = (vec2(screen_px) + 0.5) / vec2(params.screen_tiles.xy);
			vec3 world = nextgi_view_reconstruct(params.inv_view_proj, uv, depth);
			float dist_to_light = max(length(light.pos_range.xyz - world), min_hit + 0.05);
			penumbra_world = light_size * min_hit / max(dist_to_light - min_hit, 0.05);
		}
		float footprint = max(center_z * params.footprint_misc.x + params.footprint_misc.y, 1e-4);
		penumbra_px = clamp(penumbra_world / footprint, 0.0, 16.0);
	} else {
		// Unoccluded region far from blockers: skip blur to keep lit surfaces clean.
		imageStore(output_mask_image, atlas_px, vec4(center_val, 0.0, 0.0, 0.0));
		return;
	}

	float sigma = clamp(penumbra_px * 0.5, 0.25, 4.0);
	float inv_two_sigma2 = 0.5 / (sigma * sigma);
	int step_px = int(push.step_size);
	float inv_depth_tol = 1.0 / max(center_z * 0.02, 0.01);

	float accum = center_val;
	float weight_sum = 1.0;

	for (int oy = -1; oy <= 1; oy++) {
		for (int ox = -1; ox <= 1; ox++) {
			if (ox == 0 && oy == 0) {
				continue;
			}
			ivec2 ofs = ivec2(ox, oy) * step_px;
			ivec2 np = ivec2(screen_px) + ofs;
			if (np.x < 0 || np.y < 0 || np.x > max_s.x || np.y > max_s.y) {
				continue;
			}
			uvec2 nst = uvec2(np) >> 4u;
			uint ntile = (nst == st) ? tile_id : rt_lookup_cur_tile(light.policy_lookup.z, nst);
			if (ntile == RT_SHADOW_TILE_NONE) {
				continue;
			}
			float ndepth = texelFetch(depth_tex, np, 0).r;
			if (nextgi_view_is_background(ndepth)) {
				continue;
			}
			float nz = max(nextgi_view_linear_depth(params.depth_coefficients, ndepth), 1e-4);
			vec3 nnorm = normalize(texelFetch(normal_roughness_tex, np, 0).xyz * 2.0 - 1.0);

			float dist2 = float(ofs.x * ofs.x + ofs.y * ofs.y);
			float w_spatial = exp(-dist2 * inv_two_sigma2);
			float w_depth = exp(-abs(nz - center_z) * inv_depth_tol);
			float ndot = max(dot(center_normal, nnorm), 0.0);
			float ndot2 = ndot * ndot;
			float ndot4 = ndot2 * ndot2;
			float w_normal = ndot4 * ndot4; // pow(ndot, 8) -> crisp edges at normal discontinuities.
			float w = w_spatial * w_depth * w_normal;

			float s = texelFetch(source_mask_tex, rt_shadow_atlas_coord(ntile, uvec2(np) & uvec2(15u)), 0).r;
			accum += s * w;
			weight_sum += w;
		}
	}

	imageStore(output_mask_image, atlas_px, vec4(clamp(accum / weight_sum, 0.0, 1.0), 0.0, 0.0, 0.0));
}

#elif defined(MODE_COPY_PREV_GUIDE)

layout(set = 0, binding = 0) uniform sampler2D depth_tex;
layout(set = 0, binding = 1) uniform sampler2D normal_roughness_tex;
layout(set = 0, binding = 2, std430) restrict readonly buffer RTShadowParamsBuf {
	RTShadowParams data;
}
params_buf;
layout(rgba16f, set = 0, binding = 3) uniform restrict writeonly image2D prev_depth_normal_img;

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	RTShadowParams params = params_buf.data;
	if (px.x >= int(params.screen_tiles.x) || px.y >= int(params.screen_tiles.y)) {
		return;
	}
	float d = texelFetch(depth_tex, px, 0).r;
	if (nextgi_view_is_background(d)) {
		imageStore(prev_depth_normal_img, px, vec4(0.0, 0.0, 0.0, 0.0));
		return;
	}
	vec3 view_n = normalize(texelFetch(normal_roughness_tex, px, 0).xyz * 2.0 - 1.0);
	float lin_z = max(nextgi_view_linear_depth(params.depth_coefficients, d), 1e-4);
	imageStore(prev_depth_normal_img, px, vec4(view_n, lin_z));
}

#endif
