/**************************************************************************/
/*  nextgi_world_cache_resolve.glsl                                       */
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

// GodotNext: nextgi_world_cache (#294, plan §21.2.1 / §26.1 / §26.2).
// Variant 0: Temporal resolve, fixed-point accumulation clear, and LRU/age-based eviction for
//            the sparse 8-way set-associative world-space lighting cache.
// Variant 1 (MODE_DEBUG_VIS): Renders the World Cache cells on screen-space geometry for
//            VIEWPORT_DEBUG_DRAW_NEXTGI_WORLD_CACHE (DEBUG_VIEW_WORLD_CACHE).

#include "nextgi_world_cache_common_inc.glsl"

#ifdef MODE_DEBUG_VIS
#include "nextgi_view_inc.glsl"
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict readonly buffer WCDebugParams {
	NextGIWorldCacheParams data;
}
wc_params;

layout(set = 0, binding = 1, std430) restrict readonly buffer WCDebugKeys {
	uint data[];
}
wc_keys;

layout(set = 0, binding = 2, std430) restrict readonly buffer WCDebugRadiance {
	vec4 data[];
}
wc_radiance;

layout(set = 0, binding = 3) uniform sampler2D wc_depth_tex;
layout(set = 0, binding = 4) uniform sampler2D wc_normal_roughness_tex;
layout(rgba16f, set = 0, binding = 5) restrict writeonly uniform image2D wc_debug_out;

vec3 wc_level_border_tint(uint p_level) {
	if (p_level == 0u) {
		return vec3(0.2, 0.95, 0.45);
	}
	if (p_level == 1u) {
		return vec3(0.2, 0.75, 1.0);
	}
	if (p_level == 2u) {
		return vec3(1.0, 0.85, 0.2);
	}
	if (p_level == 3u) {
		return vec3(1.0, 0.45, 0.15);
	}
	return vec3(0.95, 0.25, 0.75);
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	ivec2 out_size = imageSize(wc_debug_out);
	if (pixel.x >= out_size.x || pixel.y >= out_size.y) {
		return;
	}
	ivec2 screen_size = ivec2(wc_params.data.counts0.xy);
	vec2 uv = (vec2(pixel) + 0.5) / vec2(out_size);
	ivec2 src_pixel = clamp(ivec2(uv * vec2(screen_size)), ivec2(0), screen_size - ivec2(1));
	float depth = texelFetch(wc_depth_tex, src_pixel, 0).r;
	if (depth <= 0.0) {
		imageStore(wc_debug_out, pixel, vec4(0.05, 0.06, 0.08, 1.0));
		return;
	}
	vec3 world_pos = nextgi_view_reconstruct(wc_params.data.inv_view_proj, uv, depth);
	vec4 guide = texelFetch(wc_normal_roughness_tex, src_pixel, 0);
	mat3 v2w = mat3(wc_params.data.view_to_world[0].xyz, wc_params.data.view_to_world[1].xyz, wc_params.data.view_to_world[2].xyz);
	vec3 normal = nextgi_view_world_normal(v2w, guide.xyz);
	vec3 cam_pos = wc_params.data.camera_pos_radius.xyz;
	if (dot(normal, cam_pos - world_pos) < 0.0) {
		normal = -normal;
	}

	float dist = length(world_pos - cam_pos);
	uint base_level = nextgi_wc_level_for_distance(dist);
	uint face = nextgi_wc_normal_face(normal);
	vec3 found_rad = vec3(0.0);
	uint found_level = base_level;
	bool found = false;
	for (uint l = 0u; l < 2u && !found; l++) {
		uint level = base_level + l;
		if (level >= NEXTGI_WC_LEVELS) {
			break;
		}
		ivec3 cell = nextgi_wc_quantize(world_pos, level);
		uint key = nextgi_wc_make_key(cell, level, face);
		uint base_slot = nextgi_wc_hash_bucket(cell, level, face) * NEXTGI_WC_SLOTS_PER_BUCKET;
		for (uint w = 0u; w < NEXTGI_WC_SLOTS_PER_BUCKET; w++) {
			uint k = wc_keys.data[base_slot + w];
			if (k == key) {
				vec4 rad = wc_radiance.data[base_slot + w];
				if (rad.a > 0.0) {
					found_rad = rad.rgb;
					found_level = level;
					found = true;
				}
				break;
			}
		}
	}

	vec3 color;
	if (found) {
		float cell_sz = nextgi_wc_cell_size(found_level);
		vec3 f = fract(world_pos / cell_sz);
		vec3 edge = min(f, 1.0 - f);
		float min_edge;
		if (face < 2u) {
			min_edge = min(edge.y, edge.z);
		} else if (face < 4u) {
			min_edge = min(edge.x, edge.z);
		} else {
			min_edge = min(edge.x, edge.y);
		}
		float grid_line = 1.0 - smoothstep(0.02, 0.05, min_edge);
		color = mix(found_rad, wc_level_border_tint(found_level), grid_line * 0.35);
	} else {
		// Unpopulated cell: muted magenta tint so holes are clearly visible during debugging.
		color = vec3(0.18, 0.05, 0.14);
	}
	imageStore(wc_debug_out, pixel, vec4(color, 1.0));
}

#else // Resolve & evict pass.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict buffer WCResolveKeys {
	uint data[];
}
wc_keys;

layout(set = 0, binding = 1, std430) restrict buffer WCResolveRadiance {
	vec4 data[];
}
wc_radiance;

layout(set = 0, binding = 2, std430) restrict buffer WCResolveAccum {
	uvec4 data[];
}
wc_accum;

layout(set = 0, binding = 3, std430) restrict buffer WCResolveMeta {
	uint data[];
}
wc_meta;

layout(set = 0, binding = 4, std430) restrict buffer WCResolveCounters {
	NextGIWorldCacheCounters data;
}
wc_counters;

layout(push_constant, std430) uniform WCResolvePush {
	uint frame;
	uint max_age;
	uint clear_all;
	uint pad;
}
push;

void main() {
	uint slot = gl_GlobalInvocationID.x;
	if (slot >= NEXTGI_WC_CAPACITY) {
		return;
	}
	if (push.clear_all != 0u) {
		wc_keys.data[slot] = NEXTGI_WC_EMPTY_KEY;
		wc_radiance.data[slot] = vec4(0.0);
		wc_accum.data[slot] = uvec4(0u);
		wc_meta.data[slot] = 0u;
		return;
	}
	uvec4 acc = wc_accum.data[slot];
	uint key = wc_keys.data[slot];
	if (acc.w > 0u && key != NEXTGI_WC_EMPTY_KEY) {
		vec3 sample_rad = vec3(acc.xyz) / (NEXTGI_WC_FIXED_SCALE * float(acc.w));
		vec4 prev = wc_radiance.data[slot];
		float new_hist = min(prev.a + 1.0, NEXTGI_WC_MAX_HISTORY);
		float alpha = 1.0 / new_hist;
		vec3 blended = prev.a > 0.0 ? mix(prev.rgb, sample_rad, alpha) : sample_rad;
		wc_radiance.data[slot] = vec4(blended, new_hist);
		wc_meta.data[slot] = push.frame;
		wc_accum.data[slot] = uvec4(0u);
		uint uprev = atomicAdd(wc_counters.data.cells_updated, 1u);
		uint aprev = atomicAdd(wc_counters.data.cells_active, 1u);
		if ((uprev | aprev) == 0xFFFFFFFFu) {
			wc_counters.data.sink = uprev;
		}
		return;
	}
	if (acc.w > 0u) {
		wc_accum.data[slot] = uvec4(0u);
	}
	if (key == NEXTGI_WC_EMPTY_KEY) {
		return;
	}
	uint last_frame = wc_meta.data[slot];
	uint age = push.frame >= last_frame ? (push.frame - last_frame) : 0u;
	vec4 rad = wc_radiance.data[slot];
	if (age > push.max_age || (rad.a <= 0.0 && age > 2u)) {
		wc_keys.data[slot] = NEXTGI_WC_EMPTY_KEY;
		wc_radiance.data[slot] = vec4(0.0);
		wc_meta.data[slot] = 0u;
		uint eprev = atomicAdd(wc_counters.data.cells_evicted, 1u);
		if (eprev == 0xFFFFFFFFu) {
			wc_counters.data.sink = eprev;
		}
	} else {
		uint aprev = atomicAdd(wc_counters.data.cells_active, 1u);
		if (aprev == 0xFFFFFFFFu) {
			wc_counters.data.sink = aprev;
		}
	}
}

#endif // MODE_DEBUG_VIS
