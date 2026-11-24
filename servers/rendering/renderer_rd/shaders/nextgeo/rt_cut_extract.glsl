/**************************************************************************/
/*  rt_cut_extract.glsl                                                   */
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

// GodotNext: nextgeo_rt_native (#156, plan §21.4 N13). Native Nanite ray tracing (EXPERIMENTAL).
//
// Three passes over one part of a native set, separated by barriers:
//
// Mode 0 (extract) scatters a cut of resident NextGeo clusters from the page arena into the RT
// arena: float32 positions and uint32 indices that a BLAS is then built from with CPU-known
// counts. One workgroup per cluster. The CPU computed every output offset (a prefix sum over the
// cut), so the position/index writes are disjoint and need no atomics. When params.group_word != 0
// (GodotNext: #303), Mode 0 also reduces ordered-uint UV0/UV2 min/max into an 8-word scratch block
// at params.group_word for Mode 2.
//
// Mode 1 (cards) writes the u16 Stage-2 card group of each triangle of one surface run (#77), one
// invocation per 32-bit word, so no two invocations share a word. A triangle the cut shares with
// the fixed proxy keeps the proxy's group (the CPU passes it). Any other triangle takes, among the
// cards on its dominant axis, the one whose box (grown by the tolerance) is nearest its centroid:
// first the cards of the proxy-level groups above its cluster, then, when none holds it, every card
// of the mesh. Ties take the thinner box along the axis, then the smaller one. The proxy's cards
// are depth layers of whole DAG groups, so only a per-triangle choice finds the right layer.
// nextgeo_rt_native_triangle_card() in next_gi_rt_native.cpp is the CPU copy of this rule.
//
// Mode 2 (attributes, GodotNext: #303) writes the #130 attribute stream (32-byte NextGIHitStreamHeader
// + 20-byte NextGIHitAttributes per vertex) at params.vertex_word from the page arena using the UV
// ranges reduced into params.group_word.
//
// The words buffer, all uint:
//   extract jobs, 8 words per cluster:
//     x = first position word in the page arena, y = first triangle byte in the page arena,
//     z = float stride | vertex count << 8 | triangle count << 16 | vertex layout << 24, w = 0;
//     x = first position word in the RT-arena chunk, y = first index word in the chunk,
//     z = the cluster's first vertex in its part (added to every index), w = 0.
//   choices, 4 words each: first candidate word, candidate count, default card group, 0.
//   candidates: card descriptor indices.
//   seeds, one per triangle of the part: bit 31 clear = the card group itself, set = a choice.
//   card descriptors, 8 words each (plan §6.3): group | axis << 16 | flags << 24, center xyz,
//   half extents xyz, resolution.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict readonly buffer Pages {
	uint data[];
}
pages;

layout(set = 0, binding = 1, std430) restrict readonly buffer Words {
	uint data[];
}
words;

layout(set = 0, binding = 2, std430) restrict buffer Arena {
	uint data[];
}
arena;

layout(push_constant, std430) uniform Params {
	uint mode; // 0: extract, 1: cards, 2: attributes (#303).
	uint first; // 0/2: the first job; 1: the run's first seed word.
	uint count; // 0/2: the job count; 1: the run's triangle count.
	uint group_word; // 0/2: UV scratch word (0 = disabled in mode 0); 1: the run's card group table.
	uint vertex_word; // 1: the part's first position word; 2: the #130 attribute stream word.
	uint index_word; // 1: the run's first index word.
	uint choice_word; // 1: the first choice word.
	uint card_word; // 1: the first card descriptor word.
	uint card_count;
	uint search_all_limit; // Every card is searched only when the mesh has at most this many.
	float tolerance;
	uint pad;
}
params;

#define NO_CARD 0xFFFFu
#define FAR 3.4e38

shared uint wg_uv_bounds[8];

uint resolve_vertex_layout(uint p_layout, uint p_stride) {
	if (p_layout != 0u) {
		return p_layout;
	}
	if (p_stride == 3u) {
		return 0x01u;
	} else if (p_stride == 5u) {
		return 0x09u;
	} else if (p_stride == 6u) {
		return 0x03u;
	} else if (p_stride == 7u) {
		return 0x21u;
	} else if (p_stride == 8u) {
		return 0x0Bu;
	} else if (p_stride == 10u) {
		return 0x07u;
	} else if (p_stride == 12u) {
		return 0x0Fu;
	} else if (p_stride == 14u) {
		return 0x1Fu;
	} else if (p_stride == 16u) {
		return 0x2Fu;
	}
	return p_stride >= 18u ? 0x3Fu : 0x01u;
}

uint float_to_ordered_uint(float p_value) {
	uint u = floatBitsToUint(p_value);
	return (u & 0x80000000u) != 0u ? ~u : (u ^ 0x80000000u);
}

float ordered_uint_to_float(uint p_ordered) {
	uint u = (p_ordered & 0x80000000u) != 0u ? (p_ordered ^ 0x80000000u) : ~p_ordered;
	return uintBitsToFloat(u);
}

vec2 oct_encode_snorm(vec3 p_vec) {
	float len = length(p_vec);
	vec3 n = len > 0.0 ? (p_vec / len) : vec3(0.0, 0.0, 1.0);
	n /= (abs(n.x) + abs(n.y) + abs(n.z));
	vec2 e = n.xy;
	if (n.z < 0.0) {
		vec2 s = vec2(e.x >= 0.0 ? 1.0 : -1.0, e.y >= 0.0 ? 1.0 : -1.0);
		e = (1.0 - abs(e.yx)) * s;
	}
	return clamp(e, vec2(-1.0), vec2(1.0));
}

void extract() {
	uint job = params.first + gl_WorkGroupID.x;
	if (gl_WorkGroupID.x >= params.count) {
		return;
	}
	uvec4 source = uvec4(words.data[job * 8u + 0u], words.data[job * 8u + 1u], words.data[job * 8u + 2u], words.data[job * 8u + 3u]);
	uvec4 target = uvec4(words.data[job * 8u + 4u], words.data[job * 8u + 5u], words.data[job * 8u + 6u], words.data[job * 8u + 7u]);
	uint stride = source.z & 0xFFu;
	uint vertex_count = (source.z >> 8u) & 0xFFu;
	uint triangle_count = (source.z >> 16u) & 0xFFu;
	if (stride < 3u || vertex_count == 0u) {
		return;
	}

	uint vertex_layout = resolve_vertex_layout((source.z >> 24u) & 0xFFu, stride);
	uint off_cursor = 3u;
	if ((vertex_layout & (1u << 1u)) != 0u && off_cursor + 3u <= stride) {
		off_cursor += 3u;
	}
	if ((vertex_layout & (1u << 2u)) != 0u && off_cursor + 4u <= stride) {
		off_cursor += 4u;
	}
	bool has_uv0 = (vertex_layout & (1u << 3u)) != 0u && off_cursor + 2u <= stride;
	uint off_uv0 = off_cursor;
	if (has_uv0) {
		off_cursor += 2u;
	}
	bool has_uv2 = (vertex_layout & (1u << 4u)) != 0u && off_cursor + 2u <= stride;
	uint off_uv2 = off_cursor;

	bool track_uv = params.group_word != 0u && (has_uv0 || has_uv2);
	if (track_uv) {
		if (gl_LocalInvocationID.x < 8u) {
			wg_uv_bounds[gl_LocalInvocationID.x] = ((gl_LocalInvocationID.x & 2u) == 0u) ? 0xFFFFFFFFu : 0u;
		}
		barrier();
	}

	for (uint v = gl_LocalInvocationID.x; v < vertex_count; v += 64u) {
		uint from = source.x + v * stride;
		uint to = target.x + v * 3u;
		arena.data[to + 0u] = pages.data[from + 0u];
		arena.data[to + 1u] = pages.data[from + 1u];
		arena.data[to + 2u] = pages.data[from + 2u];
		if (params.group_word != 0u && has_uv0) {
			float ux = uintBitsToFloat(pages.data[from + off_uv0 + 0u]);
			float uy = uintBitsToFloat(pages.data[from + off_uv0 + 1u]);
			if (!isnan(ux) && !isinf(ux)) {
				uint ox = float_to_ordered_uint(ux);
				atomicMin(wg_uv_bounds[0], ox);
				atomicMax(wg_uv_bounds[2], ox);
			}
			if (!isnan(uy) && !isinf(uy)) {
				uint oy = float_to_ordered_uint(uy);
				atomicMin(wg_uv_bounds[1], oy);
				atomicMax(wg_uv_bounds[3], oy);
			}
		}
		if (params.group_word != 0u && has_uv2) {
			float u2x = uintBitsToFloat(pages.data[from + off_uv2 + 0u]);
			float u2y = uintBitsToFloat(pages.data[from + off_uv2 + 1u]);
			if (!isnan(u2x) && !isinf(u2x)) {
				uint o2x = float_to_ordered_uint(u2x);
				atomicMin(wg_uv_bounds[4], o2x);
				atomicMax(wg_uv_bounds[6], o2x);
			}
			if (!isnan(u2y) && !isinf(u2y)) {
				uint o2y = float_to_ordered_uint(u2y);
				atomicMin(wg_uv_bounds[5], o2y);
				atomicMax(wg_uv_bounds[7], o2y);
			}
		}
	}

	if (track_uv) {
		barrier();
		if (gl_LocalInvocationID.x < 8u) {
			uint val = wg_uv_bounds[gl_LocalInvocationID.x];
			if ((gl_LocalInvocationID.x & 2u) == 0u) {
				if (val != 0xFFFFFFFFu) {
					atomicMin(arena.data[params.group_word + gl_LocalInvocationID.x], val);
				}
			} else {
				if (val != 0u) {
					atomicMax(arena.data[params.group_word + gl_LocalInvocationID.x], val);
				}
			}
		}
	}

	uint index_count = triangle_count * 3u;
	for (uint i = gl_LocalInvocationID.x; i < index_count; i += 64u) {
		uint byte_address = source.y + i;
		uint local = (pages.data[byte_address >> 2u] >> ((byte_address & 3u) * 8u)) & 0xFFu;
		// A corrupt index never leaves the cluster's own vertices.
		arena.data[target.y + i] = target.z + min(local, vertex_count - 1u);
	}
}

vec3 arena_position(uint p_index) {
	uint word = params.vertex_word + p_index * 3u;
	return uintBitsToFloat(uvec3(arena.data[word + 0u], arena.data[word + 1u], arena.data[word + 2u]));
}

// Keeps the card when it beats the best one so far: nearer the centroid (squared distance outside
// the grown box), then thinner along the axis, then smaller.
void test_card(uint p_card, uint p_axis, vec3 p_centroid, inout uint r_group, inout vec3 r_key) {
	uint base = params.card_word + p_card * 8u;
	uint head = words.data[base];
	if (((head >> 16u) & 0xFFu) != p_axis) {
		return;
	}
	vec3 center = uintBitsToFloat(uvec3(words.data[base + 1u], words.data[base + 2u], words.data[base + 3u]));
	vec3 half_extents = uintBitsToFloat(uvec3(words.data[base + 4u], words.data[base + 5u], words.data[base + 6u]));
	vec3 outside = max(abs(p_centroid - center) - half_extents - vec3(params.tolerance), vec3(0.0));
	uint n = p_axis >> 1u;
	vec3 key = vec3(dot(outside, outside), half_extents[n], half_extents[(n + 1u) % 3u] * half_extents[(n + 2u) % 3u]);
	if (key.x < r_key.x || (key.x == r_key.x && (key.y < r_key.y || (key.y == r_key.y && key.z < r_key.z)))) {
		r_key = key;
		r_group = head & 0xFFFFu;
	}
}

uint triangle_card(uint p_triangle) {
	uint seed = words.data[params.first + p_triangle];
	if ((seed & 0x80000000u) == 0u) {
		return seed & 0xFFFFu;
	}
	uint choice = params.choice_word + (seed & 0x7FFFFFFFu) * 4u;
	uint candidate_first = words.data[choice + 0u];
	uint candidate_count = words.data[choice + 1u];
	uint group = words.data[choice + 2u] & 0xFFFFu;

	uint index = params.index_word + p_triangle * 3u;
	vec3 a = arena_position(arena.data[index + 0u]);
	vec3 b = arena_position(arena.data[index + 1u]);
	vec3 c = arena_position(arena.data[index + 2u]);
	// The proxy's rule (nextgeo_rt_proxy.cpp): Godot front faces wind clockwise, so the outward
	// normal is (c - a) x (b - a).
	vec3 normal = cross(c - a, b - a);
	if (!(dot(normal, normal) > 1e-24)) {
		return group;
	}
	vec3 magnitude = abs(normal);
	uint axis;
	if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z) {
		axis = normal.x >= 0.0 ? 0u : 1u;
	} else if (magnitude.y >= magnitude.z) {
		axis = normal.y >= 0.0 ? 2u : 3u;
	} else {
		axis = normal.z >= 0.0 ? 4u : 5u;
	}
	vec3 mid_point = (a + b + c) / 3.0;
	vec3 key = vec3(FAR);
	for (uint k = 0u; k < candidate_count; k++) {
		test_card(words.data[candidate_first + k], axis, mid_point, group, key);
	}
	if (key.x > 0.0 && params.card_count <= params.search_all_limit) {
		for (uint k = 0u; k < params.card_count; k++) {
			test_card(k, axis, mid_point, group, key);
		}
	}
	return group;
}

void cards() {
	uint word = gl_GlobalInvocationID.x;
	uint triangle = word * 2u;
	if (triangle >= params.count) {
		return;
	}
	uint low = triangle_card(triangle);
	uint high = triangle + 1u < params.count ? triangle_card(triangle + 1u) : NO_CARD;
	arena.data[params.group_word + word] = (low & 0xFFFFu) | (high << 16u);
}

void attributes() {
	uint job = params.first + gl_WorkGroupID.x;
	if (gl_WorkGroupID.x >= params.count) {
		return;
	}
	uint s_min_x = arena.data[params.group_word + 0u];
	uint s_min_y = arena.data[params.group_word + 1u];
	uint s_max_x = arena.data[params.group_word + 2u];
	uint s_max_y = arena.data[params.group_word + 3u];
	uint s_min2_x = arena.data[params.group_word + 4u];
	uint s_min2_y = arena.data[params.group_word + 5u];
	uint s_max2_x = arena.data[params.group_word + 6u];
	uint s_max2_y = arena.data[params.group_word + 7u];

	vec2 uv_min = vec2(0.0);
	vec2 uv_ext = vec2(0.0);
	if (s_min_x <= s_max_x) {
		uv_min.x = ordered_uint_to_float(s_min_x);
		uv_ext.x = max(ordered_uint_to_float(s_max_x) - uv_min.x, 0.0);
	}
	if (s_min_y <= s_max_y) {
		uv_min.y = ordered_uint_to_float(s_min_y);
		uv_ext.y = max(ordered_uint_to_float(s_max_y) - uv_min.y, 0.0);
	}
	vec2 uv2_min = vec2(0.0);
	vec2 uv2_ext = vec2(0.0);
	if (s_min2_x <= s_max2_x) {
		uv2_min.x = ordered_uint_to_float(s_min2_x);
		uv2_ext.x = max(ordered_uint_to_float(s_max2_x) - uv2_min.x, 0.0);
	}
	if (s_min2_y <= s_max2_y) {
		uv2_min.y = ordered_uint_to_float(s_min2_y);
		uv2_ext.y = max(ordered_uint_to_float(s_max2_y) - uv2_min.y, 0.0);
	}

	if (params.first == 0u && gl_WorkGroupID.x == 0u && gl_LocalInvocationID.x == 0u) {
		arena.data[params.vertex_word + 0u] = floatBitsToUint(uv_min.x);
		arena.data[params.vertex_word + 1u] = floatBitsToUint(uv_min.y);
		arena.data[params.vertex_word + 2u] = floatBitsToUint(uv_ext.x);
		arena.data[params.vertex_word + 3u] = floatBitsToUint(uv_ext.y);
		arena.data[params.vertex_word + 4u] = floatBitsToUint(uv2_min.x);
		arena.data[params.vertex_word + 5u] = floatBitsToUint(uv2_min.y);
		arena.data[params.vertex_word + 6u] = floatBitsToUint(uv2_ext.x);
		arena.data[params.vertex_word + 7u] = floatBitsToUint(uv2_ext.y);
	}

	uvec4 source = uvec4(words.data[job * 8u + 0u], words.data[job * 8u + 1u], words.data[job * 8u + 2u], words.data[job * 8u + 3u]);
	uvec4 target = uvec4(words.data[job * 8u + 4u], words.data[job * 8u + 5u], words.data[job * 8u + 6u], words.data[job * 8u + 7u]);
	uint stride = source.z & 0xFFu;
	uint vertex_count = (source.z >> 8u) & 0xFFu;
	if (stride < 3u || vertex_count == 0u) {
		return;
	}

	uint vertex_layout = resolve_vertex_layout((source.z >> 24u) & 0xFFu, stride);
	uint off_cursor = 3u;
	bool has_nrm = (vertex_layout & (1u << 1u)) != 0u && off_cursor + 3u <= stride;
	uint off_nrm = off_cursor;
	if (has_nrm) {
		off_cursor += 3u;
	}
	bool has_tan = (vertex_layout & (1u << 2u)) != 0u && off_cursor + 4u <= stride;
	uint off_tan = off_cursor;
	if (has_tan) {
		off_cursor += 4u;
	}
	bool has_uv0 = (vertex_layout & (1u << 3u)) != 0u && off_cursor + 2u <= stride;
	uint off_uv0 = off_cursor;
	if (has_uv0) {
		off_cursor += 2u;
	}
	bool has_uv2 = (vertex_layout & (1u << 4u)) != 0u && off_cursor + 2u <= stride;
	uint off_uv2 = off_cursor;
	if (has_uv2) {
		off_cursor += 2u;
	}
	bool has_col = (vertex_layout & (1u << 5u)) != 0u && off_cursor + 4u <= stride;
	uint off_col = off_cursor;

	for (uint v = gl_LocalInvocationID.x; v < vertex_count; v += 64u) {
		uint from = source.x + v * stride;
		vec3 nrm = has_nrm
				? vec3(uintBitsToFloat(pages.data[from + off_nrm + 0u]), uintBitsToFloat(pages.data[from + off_nrm + 1u]), uintBitsToFloat(pages.data[from + off_nrm + 2u]))
				: vec3(0.0, 0.0, 1.0);
		vec3 tan_v = has_tan
				? vec3(uintBitsToFloat(pages.data[from + off_tan + 0u]), uintBitsToFloat(pages.data[from + off_tan + 1u]), uintBitsToFloat(pages.data[from + off_tan + 2u]))
				: vec3(1.0, 0.0, 0.0);
		float tan_s = has_tan ? uintBitsToFloat(pages.data[from + off_tan + 3u]) : 1.0;
		vec2 uv0 = has_uv0
				? vec2(uintBitsToFloat(pages.data[from + off_uv0 + 0u]), uintBitsToFloat(pages.data[from + off_uv0 + 1u]))
				: vec2(0.0);
		vec2 uv2 = has_uv2
				? vec2(uintBitsToFloat(pages.data[from + off_uv2 + 0u]), uintBitsToFloat(pages.data[from + off_uv2 + 1u]))
				: vec2(0.0);
		vec4 col = has_col
				? vec4(uintBitsToFloat(pages.data[from + off_col + 0u]), uintBitsToFloat(pages.data[from + off_col + 1u]), uintBitsToFloat(pages.data[from + off_col + 2u]), uintBitsToFloat(pages.data[from + off_col + 3u]))
				: vec4(1.0);

		vec2 te = oct_encode_snorm(tan_v);
		float ty_abs = max(te.y * 0.5 + 0.5, 1.0 / 32767.0);
		float ty = tan_s < 0.0 ? -ty_abs : ty_abs;

		vec2 uv0_unit = vec2(
				uv_ext.x > 0.0 ? clamp((uv0.x - uv_min.x) / uv_ext.x, 0.0, 1.0) : 0.0,
				uv_ext.y > 0.0 ? clamp((uv0.y - uv_min.y) / uv_ext.y, 0.0, 1.0) : 0.0);
		vec2 uv2_unit = vec2(
				uv2_ext.x > 0.0 ? clamp((uv2.x - uv2_min.x) / uv2_ext.x, 0.0, 1.0) : 0.0,
				uv2_ext.y > 0.0 ? clamp((uv2.y - uv2_min.y) / uv2_ext.y, 0.0, 1.0) : 0.0);

		uint dst = params.vertex_word + 8u + (target.z + v) * 5u;
		arena.data[dst + 0u] = packSnorm2x16(oct_encode_snorm(nrm));
		arena.data[dst + 1u] = packSnorm2x16(vec2(te.x, ty));
		arena.data[dst + 2u] = packUnorm2x16(uv0_unit);
		arena.data[dst + 3u] = packUnorm2x16(uv2_unit);
		arena.data[dst + 4u] = packUnorm4x8(clamp(col, vec4(0.0), vec4(1.0)));
	}
}

void main() {
	if (params.mode == 0u) {
		extract();
	} else if (params.mode == 1u) {
		cards();
	} else {
		attributes();
	}
}
