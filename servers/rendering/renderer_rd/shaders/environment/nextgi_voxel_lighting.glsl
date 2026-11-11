#[compute]
#version 450

#VERSION_DEFINES

// GodotNext: ps_voxel_lighting (#188). Resamples the SRC into the voxel
// lighting volume. Each stored hit carries its instance-local position and
// normal, its instance's page table and its card set. The shade applies D6
// like nextgi_src_lookup: among the cards that contain the hit, the
// best-aligned one whose captured material depth matches the hit's depth
// within 2 card texels supplies radiance. The texel tolerance is the one of
// that lookup's Stage-2 path (see resample_src). A stored hit has no
// primitive, so a Stage-2 card set is scanned whole instead of through its
// group lookup. Page rects are (origin.xy, size.xy) packed as two uints.
// There is no visibility buffer.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) buffer NextGIVoxelRadiance {
	uint data[];
}
voxel_radiance;

layout(set = 0, binding = 1, std430) buffer NextGIVoxelWeights {
	uint data[];
}
voxel_weights;

layout(set = 0, binding = 2, std430) restrict readonly buffer NextGIVoxelStaging {
	uint data[];
}
voxel_staging;

layout(set = 0, binding = 3, std430) restrict readonly buffer NextGIVoxelPageRects {
	uvec2 data[];
}
page_rects;

layout(set = 0, binding = 4) uniform sampler2D radiance_atlas;
layout(set = 0, binding = 5) uniform sampler2D material_depth_tex;

// Mirrors of NextGICard and NextGICardSet (next_gi_src.h, std430).
struct NextGIVoxelCard {
	vec4 origin;
	vec4 axis_u; // w = half extent along u.
	vec4 axis_v; // w = half extent along v.
	vec4 axis_n; // w = depth range.
	uvec4 atlas_rect;
};

struct NextGIVoxelCardSet {
	uint first_card;
	uint card_count;
	float texel_size;
	float capture_scale;
};

layout(set = 0, binding = 6, std430) restrict readonly buffer NextGIVoxelCards {
	NextGIVoxelCard data[];
}
cards;

layout(set = 0, binding = 7, std430) restrict readonly buffer NextGIVoxelCardSets {
	NextGIVoxelCardSet data[];
}
card_sets;

// Debug readback only: [0] any non-zero radiance, [1] covered hits, [2] shaded hits, [3] unused,
// then one vec4 per probe (radiance, weight).
layout(set = 0, binding = 8, std430) buffer NextGIVoxelDebug {
	uint data[];
}
voxel_debug;

#include "nextgi_voxel_lighting_inc.glsl"

const uint MODE_SHADE = 0u;
const uint MODE_CLEAR = 1u;
const uint MODE_PROBE = 2u;
const uint SHADE_STRIDE = 7u;
const uint PROBE_STRIDE = 4u;
const uint PROBE_HEADER = 16u;
const uint FLAG_COUNT = 1u;
const uint PAGE_EVICTED_BIT = 0x80000000u;
const float CARD_DEPTH_TEXELS = 2.0;

layout(push_constant, std430) uniform Params {
	uint mode;
	uint count;
	uint page_rect_count;
	uint flags;
	uint card_set_count;
	uint card_count;
	uint pad0;
	uint pad1;
}
params;

uint voxel_id() {
	return gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * gl_NumWorkGroups.x * gl_WorkGroupSize.x;
}

void store_weight(uint voxel, uint direction, uint weight) {
	uint byte_index = voxel * NEXTGI_VOXEL_DIRECTIONS + direction;
	uint word_index = byte_index >> 2u;
	uint shift = (byte_index & 3u) * 8u;
	uint mask = 0xffu << shift;
	uint value = (weight & 0xffu) << shift;
	atomicAnd(voxel_weights.data[word_index], ~mask);
	atomicOr(voxel_weights.data[word_index], value);
}

// Octahedral unit normal from two unorm16 components (Vector3::octahedron_decode).
vec3 decode_normal(uint packed) {
	vec2 f = unpackUnorm2x16(packed) * 2.0 - 1.0;
	vec3 n = vec3(f, 1.0 - abs(f.x) - abs(f.y));
	float t = clamp(-n.z, 0.0, 1.0);
	n.x += n.x >= 0.0 ? -t : t;
	n.y += n.y >= 0.0 ? -t : t;
	return normalize(n);
}

// D6 (nextgi_src_d6_local_inc.glsl) on this shader's resources.
#define NEXTGI_D6_CARDS cards
#define NEXTGI_D6_CARD_SETS card_sets
#define NEXTGI_D6_PAGE_RECTS page_rects
#define NEXTGI_D6_CARD_T NextGIVoxelCard
#define NEXTGI_D6_CARD_SET_T NextGIVoxelCardSet
#define NEXTGI_D6_MATERIAL_DEPTH material_depth_tex
#define NEXTGI_D6_RADIANCE radiance_atlas
#define NEXTGI_D6_CARD_COUNT params.card_count
#define NEXTGI_D6_CARD_SET_COUNT params.card_set_count
#define NEXTGI_D6_PAGE_RECT_COUNT params.page_rect_count
#include "nextgi_src_d6_local_inc.glsl"

void main() {
	uint id = voxel_id();
	if (id >= params.count) {
		return;
	}
	if (params.mode == MODE_CLEAR) {
		uint voxel = voxel_staging.data[1u + id];
		uint base = voxel * NEXTGI_VOXEL_DIRECTIONS;
		for (uint direction = 0u; direction < NEXTGI_VOXEL_DIRECTIONS; direction++) {
			voxel_radiance.data[base + direction] = 0u;
			store_weight(voxel, direction, 0u);
		}
		return;
	}
	if (params.mode == MODE_PROBE) {
		NextGIVoxelClipmap clipmap;
		for (uint level = 0u; level < NEXTGI_VOXEL_LEVELS; level++) {
			uint word = 1u + level * 4u;
			clipmap.origin_size[level] = ivec4(int(voxel_staging.data[word]), int(voxel_staging.data[word + 1u]), int(voxel_staging.data[word + 2u]), int(voxel_staging.data[word + 3u]));
		}
		uint probe = 1u + PROBE_HEADER + id * PROBE_STRIDE;
		vec3 world = uintBitsToFloat(uvec3(voxel_staging.data[probe], voxel_staging.data[probe + 1u], voxel_staging.data[probe + 2u]));
		vec3 normal = decode_normal(voxel_staging.data[probe + 3u]);
		float weight = 0.0;
		vec3 radiance = nextgi_voxel_lighting_lookup(world, normal, vec3(0.0), clipmap, weight);
		uint result = 4u + id * 4u;
		voxel_debug.data[result] = floatBitsToUint(radiance.r);
		voxel_debug.data[result + 1u] = floatBitsToUint(radiance.g);
		voxel_debug.data[result + 2u] = floatBitsToUint(radiance.b);
		voxel_debug.data[result + 3u] = floatBitsToUint(weight);
		return;
	}
	uint item = 1u + id * SHADE_STRIDE;
	uint sample_index = voxel_staging.data[item];
	uint page_table = voxel_staging.data[item + 1u];
	uint card_set_index = voxel_staging.data[item + 2u];
	vec3 position = uintBitsToFloat(uvec3(voxel_staging.data[item + 3u], voxel_staging.data[item + 4u], voxel_staging.data[item + 5u]));
	vec3 normal = decode_normal(voxel_staging.data[item + 6u]);
	uint voxel = sample_index / NEXTGI_VOXEL_DIRECTIONS;
	uint direction = sample_index - voxel * NEXTGI_VOXEL_DIRECTIONS;
	vec3 radiance;
	float projection;
	bool covered = resample_src(position, normal, page_table, card_set_index, radiance, projection);
	// The weight counts a missing card as 0 and otherwise the projection cosine.
	uint weight = covered ? uint(projection * 255.0 + 0.5) : 0u;
	uint packed = covered ? nextgi_voxel_pack_r11g11b10(radiance) : 0u;
	voxel_radiance.data[sample_index] = packed;
	store_weight(voxel, direction, weight);
	if ((params.flags & FLAG_COUNT) != 0u) {
		atomicAdd(voxel_debug.data[2], 1u);
		if (float(weight) / 255.0 >= NEXTGI_VOXEL_WEIGHT_FALLBACK) {
			atomicAdd(voxel_debug.data[1], 1u);
		}
		if (packed != 0u) {
			atomicOr(voxel_debug.data[0], 1u);
		}
	}
}
