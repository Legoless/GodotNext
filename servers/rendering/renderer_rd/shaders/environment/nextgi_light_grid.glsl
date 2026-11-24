/**************************************************************************/
/*  nextgi_light_grid.glsl                                                */
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

#version 460

#VERSION_DEFINES

// GodotNext: nextgi_hit_lighting (#129, plan §21.2 contract L7). World-space 64^3 light grid builder:
// covers [-rt_scene_radius, +rt_scene_radius]^3 around the camera from the D14 scenario light set,
// keeping <= 32 lights per cell with deterministic priority-based overflow keeping.

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

#include "nextgi_hit_lighting_common_inc.glsl"

layout(set = 0, binding = 0, std430) restrict readonly buffer LightGridLights {
	NextGIHitLightGPU data[];
}
lights;

layout(set = 0, binding = 1, std430) restrict writeonly buffer LightGridOut {
	uint data[];
}
light_grid;

layout(set = 0, binding = 2, std430) restrict buffer LightGridCounters {
	NextGIHitLightCounters data;
}
counters;

layout(push_constant, std430) uniform LightGridPush {
	vec4 grid_min_cell_size; // xyz = grid_min, w = cell_size
	uvec4 counts; // x = light_count, y/z/w = pad
}
grid_push;

// Deterministic light priority at a cell AABB: directional lights sort ahead of positional lights,
// shadowed lights break ties ahead of unshadowed lights, higher effective energy wins, and lower light
// index breaks exact ties.
float cell_light_priority(NextGIHitLightGPU p_light, vec3 p_box_min, vec3 p_box_max, out bool r_intersects) {
	uint ltype = uint(p_light.direction_type.w + 0.5);
	vec3 rgb = p_light.color_energy.rgb * p_light.color_energy.a;
	float base_energy = max(rgb.r, max(rgb.g, rgb.b));
	if (!(base_energy > 0.0)) {
		r_intersects = false;
		return 0.0;
	}
	if (ltype == 0u) {
		// Directional light affects every cell; bias above positional so suns are never evicted.
		r_intersects = true;
		return 1e12 + base_energy;
	}
	float inv_range = p_light.position_inv_range.w;
	if (!(inv_range > 0.0)) {
		r_intersects = false;
		return 0.0;
	}
	float range = 1.0 / inv_range;
	vec3 pos = p_light.position_inv_range.xyz;
	vec3 closest = clamp(pos, p_box_min, p_box_max);
	vec3 delta = closest - pos;
	float dist_sq = dot(delta, delta);
	if (dist_sq > range * range) {
		r_intersects = false;
		return 0.0;
	}
	float dist = sqrt(dist_sq);
	if (ltype == 2u) {
		// Conservative cone-sphere cull against the cell bounding sphere.
		vec3 box_center = (p_box_min + p_box_max) * 0.5;
		float box_radius = length(p_box_max - box_center);
		vec3 to_center = box_center - pos;
		float center_dist = length(to_center);
		if (center_dist > box_radius + 1e-4) {
			float sin_half = sqrt(max(1.0 - p_light.spot_atten_radius.y * p_light.spot_atten_radius.y, 0.0));
			float cos_expand = p_light.spot_atten_radius.y;
			float axial = dot(to_center, p_light.direction_type.xyz);
			if (axial < -box_radius) {
				r_intersects = false;
				return 0.0;
			}
			float perp_sq = max(center_dist * center_dist - axial * axial, 0.0);
			float cone_r = max(axial, 0.0) * (sin_half / max(cos_expand, 1e-4)) + box_radius / max(cos_expand, 1e-4);
			if (cos_expand > 0.0 && perp_sq > cone_r * cone_r) {
				r_intersects = false;
				return 0.0;
			}
		}
	}
	r_intersects = true;
	float atten = nextgi_hl_omni_attenuation(max(dist, 0.1), inv_range, p_light.spot_atten_radius.x);
	float shadow_bias = (p_light.shadow_flags.x & NEXTGI_HL_LIGHT_FLAG_SHADOW) != 0u ? 1.001 : 1.0;
	return base_energy * max(atten, 1e-6) * shadow_bias;
}

void main() {
	uvec3 cell = gl_GlobalInvocationID.xyz;
	if (any(greaterThanEqual(cell, uvec3(NEXTGI_LIGHT_GRID_DIM)))) {
		return;
	}
	float cell_size = grid_push.grid_min_cell_size.w;
	vec3 box_min = grid_push.grid_min_cell_size.xyz + vec3(cell) * cell_size;
	vec3 box_max = box_min + vec3(cell_size);

	uint kept_idx[NEXTGI_LIGHT_GRID_CELL_CAP];
	float kept_prio[NEXTGI_LIGHT_GRID_CELL_CAP];
	uint count = 0u;
	uint overflow_count = 0u;

	uint light_count = min(grid_push.counts.x, 65535u);
	for (uint i = 0u; i < light_count; i++) {
		bool intersects = false;
		float prio = cell_light_priority(lights.data[i], box_min, box_max, intersects);
		if (!intersects) {
			continue;
		}
		if (count < NEXTGI_LIGHT_GRID_CELL_CAP) {
			// Insert in descending priority order (ties: lower light index first).
			uint pos = count;
			while (pos > 0u && prio > kept_prio[pos - 1u]) {
				kept_idx[pos] = kept_idx[pos - 1u];
				kept_prio[pos] = kept_prio[pos - 1u];
				pos--;
			}
			kept_idx[pos] = i;
			kept_prio[pos] = prio;
			count++;
		} else {
			overflow_count++;
			if (prio > kept_prio[NEXTGI_LIGHT_GRID_CELL_CAP - 1u]) {
				uint pos = NEXTGI_LIGHT_GRID_CELL_CAP - 1u;
				while (pos > 0u && prio > kept_prio[pos - 1u]) {
					kept_idx[pos] = kept_idx[pos - 1u];
					kept_prio[pos] = kept_prio[pos - 1u];
					pos--;
				}
				kept_idx[pos] = i;
				kept_prio[pos] = prio;
			}
		}
	}

	uint cell_index = cell.x + cell.y * NEXTGI_LIGHT_GRID_DIM + cell.z * NEXTGI_LIGHT_GRID_DIM * NEXTGI_LIGHT_GRID_DIM;
	uint base = cell_index * NEXTGI_LIGHT_GRID_CELL_WORDS;
	light_grid.data[base] = count;
	for (uint w = 0u; w < 16u; w++) {
		uint i0 = w * 2u;
		uint i1 = i0 + 1u;
		uint lo = i0 < count ? (kept_idx[i0] & 0xFFFFu) : 0xFFFFu;
		uint hi = i1 < count ? (kept_idx[i1] & 0xFFFFu) : 0xFFFFu;
		light_grid.data[base + 1u + w] = lo | (hi << 16u);
	}

	if (overflow_count > 0u) {
		uint prev = atomicAdd(counters.data.light_grid_overflow, overflow_count);
		if (prev == 0xFFFFFFFFu) {
			counters.data.sink = prev;
		}
	}
}
