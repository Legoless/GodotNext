#[compute]

#version 450

#VERSION_DEFINES

// GodotNext: nextgeo_rt_native (#156, plan §21.4 N13). Native Nanite ray tracing (EXPERIMENTAL).
//
// Two passes over one part of a native set, the second after a barrier:
//
// Mode 0 (extract) scatters a cut of resident NextGeo clusters from the page arena into the RT
// arena: float32 positions and uint32 indices that a BLAS is then built from with CPU-known
// counts. One workgroup per cluster. The CPU computed every output offset (a prefix sum over the
// cut), so the writes are disjoint and need no atomics.
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
// The words buffer, all uint:
//   extract jobs, 8 words per cluster:
//     x = first position word in the page arena, y = first triangle byte in the page arena,
//     z = float stride | vertex count << 8 | triangle count << 16 (the cluster table's word), w = 0;
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
	uint mode; // 0: extract, 1: cards.
	uint first; // 0: the first job; 1: the run's first seed word.
	uint count; // 0: the job count; 1: the run's triangle count.
	uint group_word; // 1: the run's card group table, in arena words.
	uint vertex_word; // 1: the part's first position word.
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

	for (uint v = gl_LocalInvocationID.x; v < vertex_count; v += 64u) {
		uint from = source.x + v * stride;
		uint to = target.x + v * 3u;
		arena.data[to + 0u] = pages.data[from + 0u];
		arena.data[to + 1u] = pages.data[from + 1u];
		arena.data[to + 2u] = pages.data[from + 2u];
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

void main() {
	if (params.mode == 0u) {
		extract();
	} else {
		cards();
	}
}
