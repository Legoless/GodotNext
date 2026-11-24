/**************************************************************************/
/*  nextgi_rt_shadow_alloc.glsl                                           */
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

// GodotNext: rt_shadows (#141, plan §21.3 contract M9). Tile-sparse shadow-mask allocation
// and helper passes for standalone ray-traced shadows:
// - MODE_DEPTH_MINMAX: computes per-16x16-tile linear depth min/max from prepass depth.
// - MODE_INIT_LOOKUP: initializes per-light lookup table headers and clears overflow flags.
// - MODE_ALLOC_TILES: walks screen tiles of each RT-shadowed light, culls against tile depth AABB,
//   and atomically allocates 16x16 atlas tiles within the preset budget (#277-safe atomic sinks).
// - MODE_FINALIZE: computes indirect dispatch dimensions for trace and denoise passes.
// - MODE_RETRACE_RESOLVE: applies #145/#130 material-retrace results to the raw mask and hit distance.

#include "nextgi_rt_shadows_inc.glsl"
#include "nextgi_view_inc.glsl"

#if defined(MODE_DEPTH_MINMAX)

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depth_tex;
layout(set = 0, binding = 1, std430) restrict readonly buffer RTShadowParamsBuf {
	RTShadowParams data;
}
params_buf;
layout(set = 0, binding = 2, std430) restrict writeonly buffer RTShadowTileDepthMinMax {
	vec2 data[];
}
tile_depth_minmax;

shared float s_min_z[64];
shared float s_max_z[64];

void main() {
	uvec2 st = gl_WorkGroupID.xy;
	RTShadowParams params = params_buf.data;
	if (st.x >= params.screen_tiles.z || st.y >= params.screen_tiles.w) {
		return;
	}
	uvec2 base_px = st * RT_SHADOW_TILE_SIZE + gl_LocalInvocationID.xy * 2u;
	ivec2 max_px = ivec2(params.screen_tiles.xy) - ivec2(1);

	float local_min = 1e30;
	float local_max = 0.0;
	for (uint dy = 0u; dy < 2u; dy++) {
		for (uint dx = 0u; dx < 2u; dx++) {
			ivec2 px = ivec2(base_px + uvec2(dx, dy));
			if (px.x > max_px.x || px.y > max_px.y) {
				continue;
			}
			float d = texelFetch(depth_tex, px, 0).r;
			if (!nextgi_view_is_background(d)) {
				float lin_z = max(nextgi_view_linear_depth(params.depth_coefficients, d), 1e-4);
				local_min = min(local_min, lin_z);
				local_max = max(local_max, lin_z);
			}
		}
	}

	uint tid = gl_LocalInvocationIndex;
	s_min_z[tid] = local_min;
	s_max_z[tid] = local_max;
	barrier();

	for (uint stride = 32u; stride > 0u; stride >>= 1u) {
		if (tid < stride) {
			s_min_z[tid] = min(s_min_z[tid], s_min_z[tid + stride]);
			s_max_z[tid] = max(s_max_z[tid], s_max_z[tid + stride]);
		}
		barrier();
	}

	if (tid == 0u) {
		tile_depth_minmax.data[st.y * params.screen_tiles.z + st.x] = vec2(s_min_z[0], s_max_z[0]);
	}
}

#elif defined(MODE_INIT_LOOKUP)

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict readonly buffer RTShadowParamsBuf {
	RTShadowParams data;
}
params_buf;
layout(set = 0, binding = 1, std430) restrict readonly buffer RTShadowLightsBuf {
	RTShadowLight data[];
}
lights_buf;
layout(set = 0, binding = 2, std430) restrict writeonly buffer RTShadowTileLookupCur {
	uint data[];
}
tile_lookup;
layout(set = 0, binding = 3, std430) restrict writeonly buffer RTShadowLightOverflow {
	uint data[];
}
light_overflow;

void main() {
	uint idx = gl_GlobalInvocationID.x;
	if (idx >= params_buf.data.counts.x) {
		return;
	}
	RTShadowLight light = lights_buf.data[idx];
	uint base = light.policy_lookup.z;
	tile_lookup.data[base] = light.rect_id.x;
	tile_lookup.data[base + 1u] = light.rect_id.y;
	light_overflow.data[idx] = 0u;
}

#elif defined(MODE_ALLOC_TILES)

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict readonly buffer RTShadowParamsBuf {
	RTShadowParams data;
}
params_buf;
layout(set = 0, binding = 1, std430) restrict readonly buffer RTShadowLightsBuf {
	RTShadowLight data[];
}
lights_buf;
layout(set = 0, binding = 2, std430) restrict readonly buffer RTShadowTileDepthMinMax {
	vec2 data[];
}
tile_depth_minmax;
layout(set = 0, binding = 3, std430) restrict buffer RTShadowAllocState {
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
layout(set = 0, binding = 4, std430) restrict writeonly buffer RTShadowTileLookupCur {
	uint data[];
}
tile_lookup;
layout(set = 0, binding = 5, std430) restrict writeonly buffer RTShadowTileList {
	uvec4 data[];
}
tile_list;
layout(set = 0, binding = 6, std430) restrict writeonly buffer RTShadowTileOwnerCur {
	uvec2 data[];
}
tile_owner;
layout(set = 0, binding = 7, std430) restrict buffer RTShadowLightOverflow {
	uint data[];
}
light_overflow;

vec3 rt_unproject_view(mat4 p_inv_proj, vec2 p_uv, float p_depth) {
	vec4 v = p_inv_proj * vec4(p_uv * 2.0 - 1.0, p_depth, 1.0);
	return v.xyz / max(abs(v.w), 1e-6) * (v.w < 0.0 ? -1.0 : 1.0);
}

bool rt_tile_intersects_local_light(RTShadowParams p_params, RTShadowLight p_light, uvec2 p_st, vec2 p_zmm) {
	vec2 inv_screen = 1.0 / vec2(max(p_params.screen_tiles.xy, uvec2(1u)));
	vec2 uv0 = vec2(p_st * RT_SHADOW_TILE_SIZE) * inv_screen;
	vec2 uv1 = min(vec2((p_st + uvec2(1u)) * RT_SHADOW_TILE_SIZE) * inv_screen, vec2(1.0));
	float d_near = nextgi_view_depth_from_linear(p_params.depth_coefficients, max(p_zmm.x * 0.995, 1e-4));
	float d_far = nextgi_view_depth_from_linear(p_params.depth_coefficients, p_zmm.y * 1.005);

	vec3 c0 = rt_unproject_view(p_params.inv_projection, vec2(uv0.x, uv0.y), d_near);
	vec3 c1 = rt_unproject_view(p_params.inv_projection, vec2(uv1.x, uv0.y), d_near);
	vec3 c2 = rt_unproject_view(p_params.inv_projection, vec2(uv0.x, uv1.y), d_near);
	vec3 c3 = rt_unproject_view(p_params.inv_projection, vec2(uv1.x, uv1.y), d_near);
	vec3 c4 = rt_unproject_view(p_params.inv_projection, vec2(uv0.x, uv0.y), d_far);
	vec3 c5 = rt_unproject_view(p_params.inv_projection, vec2(uv1.x, uv0.y), d_far);
	vec3 c6 = rt_unproject_view(p_params.inv_projection, vec2(uv0.x, uv1.y), d_far);
	vec3 c7 = rt_unproject_view(p_params.inv_projection, vec2(uv1.x, uv1.y), d_far);

	vec3 bmin = min(min(min(c0, c1), min(c2, c3)), min(min(c4, c5), min(c6, c7)));
	vec3 bmax = max(max(max(c0, c1), max(c2, c3)), max(max(c4, c5), max(c6, c7)));

	// Sphere vs AABB test.
	vec3 light_pos = p_light.view_pos_radius.xyz;
	float r = p_light.view_pos_radius.w;
	vec3 closest = clamp(light_pos, bmin, bmax);
	vec3 delta = closest - light_pos;
	if (dot(delta, delta) > r * r) {
		return false;
	}

	uint ltype = p_light.rect_id.w & 3u;
	if (ltype == RT_SHADOW_TYPE_SPOT) {
		// Conservative cone vs AABB bounding sphere test.
		vec3 box_center = 0.5 * (bmin + bmax);
		float box_radius = length(0.5 * (bmax - bmin));
		vec3 v = box_center - light_pos;
		float v_len_sq = dot(v, v);
		float v_proj = dot(v, p_light.view_dir_sin.xyz);
		if (v_proj + box_radius < 0.0 || v_proj - box_radius > r) {
			return false;
		}
		float sin_a = p_light.view_dir_sin.w;
		float cos_a = p_light.dir_cos.w;
		if (cos_a > 0.0) {
			float perp_sq = max(v_len_sq - v_proj * v_proj, 0.0);
			float dist_to_axis = sqrt(perp_sq);
			float cone_r = max(v_proj, 0.0) * (sin_a / max(cos_a, 1e-4));
			// Perpendicular distance from sphere center to cone surface:
			float signed_dist = cos_a * dist_to_axis - sin_a * v_proj;
			if (signed_dist > box_radius && dist_to_axis > cone_r + box_radius) {
				return false;
			}
		}
	} else if (ltype == RT_SHADOW_TYPE_AREA) {
		// Half-space test: at least one corner of the tile AABB must lie in front of the area light plane.
		vec3 n = p_light.view_dir_sin.xyz;
		vec3 support = mix(bmin, bmax, greaterThan(n, vec3(0.0)));
		if (dot(n, support - light_pos) < -0.05) {
			return false;
		}
	}
	return true;
}

void main() {
	uint light_idx = gl_WorkGroupID.z;
	RTShadowParams params = params_buf.data;
	if (light_idx >= params.counts.x) {
		return;
	}
	RTShadowLight light = lights_buf.data[light_idx];
	uvec2 min_t = uvec2(light.rect_id.x & 0xFFFFu, light.rect_id.x >> 16u);
	uvec2 dim_t = uvec2(light.rect_id.y & 0xFFFFu, light.rect_id.y >> 16u);
	uvec2 local_t = gl_GlobalInvocationID.xy;
	if (local_t.x >= dim_t.x || local_t.y >= dim_t.y) {
		return;
	}

	uvec2 st = min_t + local_t;
	if (st.x >= params.screen_tiles.z || st.y >= params.screen_tiles.w) {
		return;
	}

	vec2 zmm = tile_depth_minmax.data[st.y * params.screen_tiles.z + st.x];
	if (!(zmm.y > 0.0)) {
		return; // Background/sky tile: needs no shadow tile.
	}

	uint ltype = light.rect_id.w & 3u;
	if (ltype != RT_SHADOW_TYPE_DIRECTIONAL) {
		if (!rt_tile_intersects_local_light(params, light, st, zmm)) {
			return;
		}
	}

	uint slot = atomicAdd(alloc_state.tile_count, 1u);
	if (slot == 0xFFFFFFFFu) {
		alloc_state.sink = slot;
	}
	if (slot < params.counts.y) {
		tile_lookup.data[light.policy_lookup.z + 2u + local_t.y * dim_t.x + local_t.x] = slot + 1u;
		uint packed_st = st.x | (st.y << 16u);
		tile_list.data[slot] = uvec4(light_idx, packed_st, light.rect_id.z, 0u);
		tile_owner.data[slot] = uvec2(light.rect_id.z, packed_st);
	} else {
		uint ov = atomicAdd(alloc_state.overflow_tiles, 1u);
		uint prev_flag = atomicOr(light_overflow.data[light_idx], 1u);
		uint ol = 0u;
		if (prev_flag == 0u) {
			ol = atomicAdd(alloc_state.overflow_lights, 1u);
		}
		if ((ov | prev_flag | ol) == 0xFFFFFFFFu) {
			alloc_state.sink = ov ^ prev_flag ^ ol;
		}
	}
}

#elif defined(MODE_FINALIZE)

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict readonly buffer RTShadowParamsBuf {
	RTShadowParams data;
}
params_buf;
layout(set = 0, binding = 1, std430) restrict buffer RTShadowAllocState {
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
layout(set = 0, binding = 2, std430) restrict writeonly buffer RTShadowIndirect {
	uvec4 data; // xyz: dispatch groups for 8x8 workgroups (2, 2*min(active,256), ceil(active/256)), w: active_tiles.
}
indirect_buf;

void main() {
	uint active_tiles = min(alloc_state.tile_count, params_buf.data.counts.y);
	alloc_state.active_tiles = active_tiles;
	if (active_tiles == 0u) {
		indirect_buf.data = uvec4(0u, 0u, 0u, 0u);
	} else {
		uint rows = min(active_tiles, 256u);
		uint gz = (active_tiles + 255u) >> 8u;
		indirect_buf.data = uvec4(2u, rows * 2u, gz, active_tiles);
	}
}

#elif defined(MODE_RETRACE_RESOLVE)

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(r8, set = 0, binding = 0) uniform restrict writeonly image2D raw_mask_image;
layout(r16f, set = 0, binding = 1) uniform restrict writeonly image2D hit_dist_image;
layout(set = 0, binding = 2, std430) restrict readonly buffer NextGIShadowRetrace {
	uvec4 header;
	uvec4 data[];
}
retrace_records;

void main() {
	uint index = gl_GlobalInvocationID.x;
	uint count = min(retrace_records.header.x, retrace_records.header.y);
	if (index >= count) {
		return;
	}
	uvec4 info = retrace_records.data[index * 3u];
	if (info.w == 0u) {
		return;
	}
	ivec2 atlas_px = ivec2(int(info.x & 0xFFFFu), int(info.x >> 16u));
	if (info.w == 2u) {
		imageStore(raw_mask_image, atlas_px, vec4(1.0, 0.0, 0.0, 0.0));
		imageStore(hit_dist_image, atlas_px, vec4(RT_SHADOW_HIT_MISS, 0.0, 0.0, 0.0));
	} else {
		float tmax = uintBitsToFloat(retrace_records.data[index * 3u + 1u].w);
		imageStore(raw_mask_image, atlas_px, vec4(0.0, 0.0, 0.0, 0.0));
		imageStore(hit_dist_image, atlas_px, vec4(min(tmax * 0.5, 2.0), 0.0, 0.0, 0.0));
	}
}

#endif
