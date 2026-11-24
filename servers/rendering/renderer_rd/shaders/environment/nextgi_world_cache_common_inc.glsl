/**************************************************************************/
/*  nextgi_world_cache_common_inc.glsl                                    */
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

// GodotNext: nextgi_world_cache (#294, plan §21.2.1 / §26.1 / §26.2). Not a standalone shader.
//
// Shared sparse world-space cell grid constants, cascade sizing, 32-bit key encoding, 8-way
// set-associative hash table layout, and parameter/counter structs used by the World Cache
// path-trace raygen (nextgi_world_cache_trace.glsl), resolve/evict/debug compute shader
// (nextgi_world_cache_resolve.glsl), diffuse gather & reflections (nextgi_gather.glsl),
// WRC & translucency updates (nextgi_wrc_update.glsl), and hit lighting (nextgi_hit_lighting.glsl).
// Mirrors the CPU reference in next_gi_world_cache.{h,cpp} bit-for-bit.

#ifndef NEXTGI_WORLD_CACHE_COMMON_INC_GLSL
#define NEXTGI_WORLD_CACHE_COMMON_INC_GLSL

const uint NEXTGI_WC_BUCKETS = 16384u; // 2^14 buckets.
const uint NEXTGI_WC_BUCKET_MASK = 16383u;
const uint NEXTGI_WC_SLOTS_PER_BUCKET = 8u; // 8-way set-associative buckets.
const uint NEXTGI_WC_CAPACITY = 131072u; // 16384 * 8 slots.
const uint NEXTGI_WC_LEVELS = 5u; // 0.5m, 1m, 2m, 4m, 8m camera-distance cascades.
const uint NEXTGI_WC_EMPTY_KEY = 0u;
const uint NEXTGI_WC_MAX_AGE = 32u; // Evict untouched cells after 32 frames.
const float NEXTGI_WC_MAX_HISTORY = 16.0;
const float NEXTGI_WC_FIXED_SCALE = 1024.0;
const float NEXTGI_WC_MAX_RADIANCE = 64.0;
const uint NEXTGI_WC_MAX_SHADOWED_LIGHTS = 2u;
const uint NEXTGI_WC_MASK_GI = 0x80u;

// World Cache params buffer (192 bytes, std430). Matches NextGIWorldCacheParamsGPU.
struct NextGIWorldCacheParams {
	mat4 inv_view_proj;
	vec4 camera_pos_radius; // xyz camera pos, w rt_scene_radius
	vec4 grid_min_inv_cell; // xyz 64^3 world light grid min, w = 64 / (2 * rt_scene_radius)
	vec4 ambient_color_sky; // rgb ambient color, w sky_enabled (0/1)
	vec4 view_to_world[3]; // Camera basis (view-space normal -> world-space normal)
	uvec4 counts0; // x screen_w, y screen_h, z probe_grid_w, w probe_grid_h
	uvec4 counts1; // x frame, y light_count, z probe_stride, w flags
};

// World Cache GPU counters (64 bytes, std430). Matches NextGIWorldCacheCountersGPU.
struct NextGIWorldCacheCounters {
	uint paths_traced;
	uint primary_hits;
	uint bounce_hits;
	uint cells_inserted;
	uint cells_updated;
	uint cells_active;
	uint cells_evicted;
	uint hash_collisions;
	uint shadow_rays;
	uint lookup_hits;
	uint lookup_misses;
	uint sink;
	uint pad0;
	uint pad1;
	uint pad2;
	uint pad3;
};

// Maps a world-space normal to one of 6 axis-aligned faces [0..5]: +X, -X, +Y, -Y, +Z, -Z.
uint nextgi_wc_normal_face(vec3 p_normal) {
	vec3 a = abs(p_normal);
	if (a.x >= a.y && a.x >= a.z) {
		return p_normal.x >= 0.0 ? 0u : 1u;
	}
	if (a.y >= a.z) {
		return p_normal.y >= 0.0 ? 2u : 3u;
	}
	return p_normal.z >= 0.0 ? 4u : 5u;
}

// Camera-distance cascade level [0..4]:
//   L0: < 12m (0.5m), L1: < 28m (1m), L2: < 64m (2m), L3: < 140m (4m), L4: >= 140m (8m).
uint nextgi_wc_level_for_distance(float p_dist) {
	if (p_dist < 12.0) {
		return 0u;
	}
	if (p_dist < 28.0) {
		return 1u;
	}
	if (p_dist < 64.0) {
		return 2u;
	}
	if (p_dist < 140.0) {
		return 3u;
	}
	return 4u;
}

float nextgi_wc_cell_size(uint p_level) {
	return 0.5 * float(1u << min(p_level, NEXTGI_WC_LEVELS - 1u));
}

ivec3 nextgi_wc_quantize(vec3 p_world, uint p_level) {
	return ivec3(floor(p_world / nextgi_wc_cell_size(p_level)));
}

// Packs (cell.xyz low 8 bits each, level, face) into a guaranteed non-zero 32-bit key.
// Because every level's active diameter is strictly < 256 cells, low-8-bit cell coordinates
// uniquely identify every active cell in that level without wrap collisions.
uint nextgi_wc_make_key(ivec3 p_cell, uint p_level, uint p_face) {
	uint lvl = min(p_level, NEXTGI_WC_LEVELS - 1u);
	uint face = min(p_face, 5u);
	uint tag = 1u + lvl * 6u + face; // [1..30], strictly non-zero.
	uint cx = uint(p_cell.x) & 0xFFu;
	uint cy = uint(p_cell.y) & 0xFFu;
	uint cz = uint(p_cell.z) & 0xFFu;
	return cx | (cy << 8u) | (cz << 16u) | (tag << 24u);
}

// Spatial hash mapping (cell, level, face) to bucket index [0..NEXTGI_WC_BUCKETS - 1].
uint nextgi_wc_hash_bucket(ivec3 p_cell, uint p_level, uint p_face) {
	uint h = uint(p_cell.x) * 73856093u ^ uint(p_cell.y) * 19349663u ^ uint(p_cell.z) * 83492791u ^ (p_level * 2654435761u) ^ (p_face * 2246822519u);
	h ^= h >> 16u;
	h *= 0x7feb352du;
	h ^= h >> 15u;
	h *= 0x846ca68bu;
	h ^= h >> 16u;
	return h & NEXTGI_WC_BUCKET_MASK;
}

#endif // NEXTGI_WORLD_CACHE_COMMON_INC_GLSL
