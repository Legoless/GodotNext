#[compute]
#version 450

#VERSION_DEFINES

// GodotNext: ps_voxel_lighting (#188). Compacts every valid visibility entry
// and resamples the relight atlas. Page rects are (origin.xy, size.xy) packed
// as two uints, the same layout as nextgi_src_lookup. There is no visibility
// buffer and no lookup self-test.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) buffer NextGIVoxelRadiance {
	uint data[];
}
voxel_radiance;

layout(set = 0, binding = 1, std430) buffer NextGIVoxelWeights {
	uint data[];
}
voxel_weights;

layout(set = 0, binding = 2, std430) buffer NextGIVoxelStaging {
	uint data[];
}
voxel_staging;

layout(set = 0, binding = 3, std430) buffer NextGIVoxelPageRects {
	uvec2 data[];
}
page_rects;

layout(set = 0, binding = 4) uniform sampler2D radiance_atlas;

#include "nextgi_voxel_lighting_inc.glsl"

const uint MODE_SHADE = 0u;
const uint MODE_CLEAR = 1u;
const uint SHADE_STRIDE = 4u;

layout(push_constant, std430) uniform Params {
	uint mode;
	uint count;
	uint page_rect_count;
	uint weight_words;
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

void main() {
	uint id = voxel_id();
	// Keep every binding live for the compiler, including the lookup include.
	if (params.weight_words == 0xffffffffu && id == 0u) {
		voxel_radiance.data[0] = page_rects.data[0].x ^ voxel_staging.data[0];
		voxel_weights.data[0] = uint(texelFetch(radiance_atlas, ivec2(0), 0).r);
		NextGIVoxelClipmap unused;
		unused.origin_size[0] = ivec4(0);
		unused.origin_size[1] = ivec4(0);
		unused.origin_size[2] = ivec4(0);
		unused.origin_size[3] = ivec4(0);
		float unused_weight = 0.0;
		voxel_radiance.data[1] = nextgi_voxel_pack_r11g11b10(nextgi_voxel_lighting_lookup(vec3(0.0), vec3(0.0), vec3(0.0), unused, unused_weight));
	}
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
	uint item = 1u + id * SHADE_STRIDE;
	uint sample_index = voxel_staging.data[item];
	uint page_table = voxel_staging.data[item + 1u];
	uint card_weight = voxel_staging.data[item + 2u];
	uint uv_bits = voxel_staging.data[item + 3u];
	uint card = card_weight & 0xfu;
	uint weight = (card_weight >> 8u) & 0xffu;
	uint voxel = sample_index / NEXTGI_VOXEL_DIRECTIONS;
	uint direction = sample_index - voxel * NEXTGI_VOXEL_DIRECTIONS;
	bool page_missing = page_table == 0xffffffffu || (page_table & 0x80000000u) != 0u || card >= 12u;
	uint page_index = page_missing ? 0u : page_table + card;
	if (page_missing || page_index >= params.page_rect_count) {
		voxel_radiance.data[sample_index] = 0u;
		store_weight(voxel, direction, 0u);
		return;
	}
	uvec2 page = page_rects.data[page_index];
	uvec2 page_origin = uvec2(page.x & 0xffffu, page.x >> 16u);
	uvec2 page_size = max(uvec2(page.y & 0xffffu, page.y >> 16u), uvec2(1u));
	float u = float(uv_bits & 0xffffu) / 65535.0;
	float v = float(uv_bits >> 16u) / 65535.0;
	ivec2 radiance_coord = ivec2(page_origin) + ivec2(clamp(vec2(u, v) * vec2(page_size), vec2(0.0), vec2(page_size) - vec2(1.0)));
	ivec2 atlas_size = textureSize(radiance_atlas, 0);
	radiance_coord = clamp(radiance_coord, ivec2(0), atlas_size - ivec2(1));
	vec3 radiance = texelFetch(radiance_atlas, radiance_coord, 0).rgb;
	uint packed = nextgi_voxel_pack_r11g11b10(radiance);
	voxel_radiance.data[sample_index] = packed;
	store_weight(voxel, direction, weight);
	if (packed != 0u) {
		atomicOr(voxel_staging.data[0], 1u);
	}
}
