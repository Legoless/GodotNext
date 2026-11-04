#[compute]

#version 460

#extension GL_EXT_ray_query : require

#VERSION_DEFINES

// World Radiance Cache update (#44, D15). 8x8, one probe per workgroup, one opaque proceed per ray.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT nextgi_tlas;
#define NEXTGI_TLAS nextgi_tlas
#include "nextgi_rt_inc.glsl"

layout(r11f_g11f_b10f, set = 0, binding = 1) uniform image2D radiance_image;
layout(rg16f, set = 0, binding = 2) uniform image2D visibility_image;
layout(rgba16f, set = 0, binding = 3) uniform image3D sh_r_image;
layout(rgba16f, set = 0, binding = 4) uniform image3D sh_g_image;
layout(rgba16f, set = 0, binding = 5) uniform image3D sh_b_image;
layout(rgba16f, set = 0, binding = 6) uniform image3D offset_image;
layout(rgba16f, set = 0, binding = 7) uniform image2D mip4_image;
layout(rgba16f, set = 0, binding = 8) uniform image2D mip2_image;

layout(set = 0, binding = 9, std430) restrict readonly buffer NextGIWRCRecords {
	NextGIInstanceRecord data[];
}
records;
layout(set = 0, binding = 10, std430) restrict readonly buffer NextGIWRCSurfaces {
	NextGISurfaceRecord data[];
}
surfaces;
layout(set = 0, binding = 11, std430) restrict readonly buffer NextGIWRCChunk0 {
	uint data[];
}
chunk0;
layout(set = 0, binding = 12, std430) restrict readonly buffer NextGIWRCChunk1 {
	uint data[];
}
chunk1;
layout(set = 0, binding = 13, std430) restrict readonly buffer NextGIWRCChunk2 {
	uint data[];
}
chunk2;
layout(set = 0, binding = 14, std430) restrict readonly buffer NextGIWRCChunk3 {
	uint data[];
}
chunk3;
layout(set = 0, binding = 15, std430) restrict readonly buffer NextGIWRCCards {
	NextGICard data[];
}
cards;
layout(set = 0, binding = 16, std430) restrict readonly buffer NextGIWRCCardSets {
	NextGICardSet data[];
}
card_sets;
layout(set = 0, binding = 17, std430) restrict readonly buffer NextGIWRCPageRects {
	uvec2 data[];
}
page_rects;
layout(set = 0, binding = 18) uniform sampler2D src_radiance_tex;
layout(set = 0, binding = 19) uniform sampler2D material_depth_tex;

layout(set = 0, binding = 20, std430) restrict readonly buffer NextGIWRCParams {
	vec4 camera; // xyz, w = 1 when the cache is valid
	vec4 sky;
	vec4 origin_spacing; // xyz snapped level-0 origin, w = 1 m spacing
	uvec4 dispatch; // probe_base, probe_count, frame, record_count
	uint page_rect_count;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

layout(set = 0, binding = 21, std430) buffer NextGIWRCCounters {
	uint rays;
	uint hits;
	uint sky_misses;
	uint energy_bits;
	uint fog_samples;
	uint fog_ambient_bits;
	uint pad_a;
	uint pad_b;
}
counters;

shared uint wrc_backfaces;
shared vec3 wrc_escape_samples[64];

uint nextgi_wrc_chunk_uint(uint chunk, uint byte_offset) {
	uint word = byte_offset >> 2u;
	if (chunk == 0u) {
		return chunk0.data[word];
	}
	if (chunk == 1u) {
		return chunk1.data[word];
	}
	if (chunk == 2u) {
		return chunk2.data[word];
	}
	return chunk3.data[word];
}

vec3 nextgi_wrc_chunk_vec3(uint chunk, uint byte_offset) {
	return vec3(uintBitsToFloat(nextgi_wrc_chunk_uint(chunk, byte_offset)), uintBitsToFloat(nextgi_wrc_chunk_uint(chunk, byte_offset + 4u)), uintBitsToFloat(nextgi_wrc_chunk_uint(chunk, byte_offset + 8u)));
}

bool nextgi_wrc_hit_pos_normal(NextGIHit hit, out vec3 object_pos, out vec3 face_normal) {
	if (!hit.hit || hit.custom_index >= params.dispatch.w) {
		return false;
	}
	NextGIInstanceRecord rec = records.data[hit.custom_index];
	if (hit.geometry_index >= rec.surface_count) {
		return false;
	}
	NextGISurfaceRecord surface = surfaces.data[rec.surface_base + hit.geometry_index];
	uint index_byte = surface.index_offset + hit.primitive * 12u;
	uint i0 = nextgi_wrc_chunk_uint(rec.arena_chunk, index_byte);
	uint i1 = nextgi_wrc_chunk_uint(rec.arena_chunk, index_byte + 4u);
	uint i2 = nextgi_wrc_chunk_uint(rec.arena_chunk, index_byte + 8u);
	vec3 p0 = nextgi_wrc_chunk_vec3(rec.arena_chunk, surface.vertex_offset + i0 * 12u);
	vec3 p1 = nextgi_wrc_chunk_vec3(rec.arena_chunk, surface.vertex_offset + i1 * 12u);
	vec3 p2 = nextgi_wrc_chunk_vec3(rec.arena_chunk, surface.vertex_offset + i2 * 12u);
	object_pos = p0 * (1.0 - hit.bary.x - hit.bary.y) + p1 * hit.bary.x + p2 * hit.bary.y;
	face_normal = normalize(cross(p1 - p0, p2 - p0));
	if (!hit.front_face) {
		face_normal = -face_normal;
	}
	return true;
}

// D6 card lookup. A failure falls through to the previous WRC sample.
bool nextgi_src_lookup(NextGIHit hit, out vec3 radiance, out bool uncovered) {
	radiance = vec3(0.0);
	uncovered = false;
	vec3 object_pos;
	vec3 face_normal;
	if (!nextgi_wrc_hit_pos_normal(hit, object_pos, face_normal)) {
		return false;
	}
	NextGIInstanceRecord rec = records.data[hit.custom_index];
	if (rec.card_set == 0xFFFFFFFFu || rec.page_table == 0xFFFFFFFFu || (rec.page_table & 0x80000000u) != 0u) {
		uncovered = true;
		return false;
	}
	NextGICardSet card_set = card_sets.data[rec.card_set];
	float best_align = -2.0;
	vec3 best = vec3(0.0);
	bool found = false;
	for (uint i = 0u; i < card_set.card_count; i++) {
		NextGICard card = cards.data[card_set.first_card + i];
		vec3 rel = object_pos - card.origin.xyz;
		float u = dot(rel, card.axis_u.xyz);
		float v = dot(rel, card.axis_v.xyz);
		if (abs(u) > card.axis_u.w || abs(v) > card.axis_v.w) {
			continue;
		}
		float align = dot(face_normal, card.axis_n.xyz);
		vec2 card_size = max(vec2(card.atlas_rect.zw), vec2(1.0));
		vec2 texel = vec2((u / card.axis_u.w) * 0.5 + 0.5, (v / card.axis_v.w) * 0.5 + 0.5) * card_size;
		uint page_index = rec.page_table + i;
		if (page_index >= params.page_rect_count) {
			continue;
		}
		uvec2 page = page_rects.data[page_index];
		uvec2 page_origin = uvec2(page.x & 0xFFFFu, page.x >> 16u);
		uvec2 page_size = max(uvec2(page.y & 0xFFFFu, page.y >> 16u), uvec2(1u));
		ivec2 radiance_coord = ivec2(page_origin) + ivec2(clamp(texel / card_size * vec2(page_size), vec2(0.0), vec2(page_size) - 1.0));
		vec3 sample_radiance = texelFetch(src_radiance_tex, radiance_coord, 0).rgb;
		if (align > best_align) {
			best_align = align;
			best = sample_radiance;
			found = true;
		}
	}
	radiance = best;
	return found;
}

vec3 nextgi_wrc_oct_dir(ivec2 texel) {
	vec2 uv = (vec2(texel) + 0.5) / 8.0 * 2.0 - 1.0;
	vec3 direction = vec3(uv.x, uv.y, 1.0 - abs(uv.x) - abs(uv.y));
	if (direction.z < 0.0) {
		float x = (1.0 - abs(direction.y)) * sign(direction.x);
		float y = (1.0 - abs(direction.x)) * sign(direction.y);
		direction.x = x;
		direction.y = y;
	}
	return normalize(direction);
}

ivec2 nextgi_wrc_radiance_texel(uint probe, ivec2 local) {
	uvec2 tile = uvec2(probe % 128u, probe / 128u);
	return ivec2(tile) * 10 + ivec2(1, 1) + local;
}

ivec2 nextgi_wrc_visibility_texel(uint probe, ivec2 local) {
	uvec2 tile = uvec2(probe % 128u, probe / 128u);
	return ivec2(tile) * 8 + local;
}

ivec3 nextgi_wrc_probe_cell(uint probe) {
	uint in_level = probe % 4096u;
	uint x = in_level % 16u;
	uint y = (in_level / 16u) % 16u;
	uint z = in_level / 256u;
	uint level = probe / 4096u;
	return ivec3(x, y, z + level * 16u);
}

void nextgi_wrc_trace() {
	if (gl_LocalInvocationIndex == 0u) {
		wrc_backfaces = 0u;
	}
	wrc_escape_samples[gl_LocalInvocationIndex] = vec3(0.0);
	barrier();
	if (gl_WorkGroupID.x >= params.dispatch.y) {
		return;
	}
	uint probe = (params.dispatch.x + gl_WorkGroupID.x) % 16384u;
	ivec2 local = ivec2(gl_LocalInvocationID.xy);
	vec3 direction = nextgi_wrc_oct_dir(local);
	ivec3 cell = nextgi_wrc_probe_cell(probe);
	uint level = probe / 4096u;
	float spacing = params.origin_spacing.w * float(1u << level);
	vec3 level_origin = floor(params.origin_spacing.xyz / spacing) * spacing;
	vec3 probe_pos = level_origin + (vec3(cell.xy, cell.z % 16) - vec3(7.5)) * spacing;
	vec4 previous_offset = imageLoad(offset_image, cell);
	probe_pos += previous_offset.xyz;
	ivec2 radiance_texel = nextgi_wrc_radiance_texel(probe, local);
	vec3 previous = imageLoad(radiance_image, radiance_texel).rgb;
	NextGIHit hit;
	atomicAdd(counters.rays, 1u);
	vec3 sample_radiance = params.sky.rgb;
	bool back_face = false;
	if (nextgi_trace(probe_pos, direction, 1.0e6, hit)) {
		atomicAdd(counters.hits, 1u);
		bool uncovered = false;
		vec3 looked_up = vec3(0.0);
		if (!nextgi_src_lookup(hit, looked_up, uncovered)) {
			looked_up = previous;
		}
		sample_radiance = looked_up;
		back_face = !hit.front_face && hit.t < 0.5 * spacing;
	} else {
		atomicAdd(counters.sky_misses, 1u);
	}
	float blend = previous_offset.w > 0.5 ? 0.1 : 1.0;
	vec3 blended = mix(previous, sample_radiance, blend);
	imageStore(radiance_image, radiance_texel, vec4(blended, 1.0));
	imageStore(visibility_image, nextgi_wrc_visibility_texel(probe, local), vec4(hit.hit ? hit.t : 0.0, hit.hit ? hit.t * hit.t : 0.0, 0.0, 0.0));
	uint energy = uint(clamp(dot(blended, vec3(0.2126, 0.7152, 0.0722)) * 64.0, 0.0, 4095.0));
	atomicAdd(counters.energy_bits, energy);
	if (back_face) {
		atomicAdd(wrc_backfaces, 1u);
		wrc_escape_samples[gl_LocalInvocationIndex] = direction;
	}
	barrier();
	if (gl_LocalInvocationIndex == 0u) {
		vec3 escape_sum = vec3(0.0);
		for (uint i = 0u; i < 64u; i++) {
			escape_sum += wrc_escape_samples[i];
		}
		float fraction = float(wrc_backfaces) / 64.0;
		vec3 moved = probe_pos;
		if (fraction > 0.25 && dot(escape_sum, escape_sum) > 0.0) {
			float travel = min(length(escape_sum), 0.4 * spacing);
			moved = probe_pos + normalize(escape_sum) * travel;
		}
		imageStore(offset_image, cell, vec4(moved - level_origin - (vec3(cell.xy, cell.z % 16) - vec3(7.5)) * spacing, 1.0));
	}
}

void nextgi_wrc_project() {
	if (gl_LocalInvocationIndex != 0u || gl_WorkGroupID.x >= params.dispatch.y) {
		return;
	}
	uint probe = (params.dispatch.x + gl_WorkGroupID.x) % 16384u;
	vec3 sum = vec3(0.0);
	for (uint y = 0u; y < 8u; y++) {
		for (uint x = 0u; x < 8u; x++) {
			sum += imageLoad(radiance_image, nextgi_wrc_radiance_texel(probe, ivec2(x, y))).rgb;
		}
	}
	sum /= 64.0;
	ivec3 cell = nextgi_wrc_probe_cell(probe);
	imageStore(sh_r_image, cell, vec4(sum.r, 0.0, 0.0, 0.0));
	imageStore(sh_g_image, cell, vec4(sum.g, 0.0, 0.0, 0.0));
	imageStore(sh_b_image, cell, vec4(sum.b, 0.0, 0.0, 0.0));
	uvec2 tile = uvec2(probe % 128u, probe / 128u);
	for (uint y = 0u; y < 4u; y++) {
		for (uint x = 0u; x < 4u; x++) {
			vec3 block = imageLoad(radiance_image, nextgi_wrc_radiance_texel(probe, ivec2(x * 2u, y * 2u))).rgb;
			block += imageLoad(radiance_image, nextgi_wrc_radiance_texel(probe, ivec2(x * 2u + 1u, y * 2u))).rgb;
			block += imageLoad(radiance_image, nextgi_wrc_radiance_texel(probe, ivec2(x * 2u, y * 2u + 1u))).rgb;
			block += imageLoad(radiance_image, nextgi_wrc_radiance_texel(probe, ivec2(x * 2u + 1u, y * 2u + 1u))).rgb;
			imageStore(mip4_image, ivec2(tile) * 6 + ivec2(1, 1) + ivec2(x, y), vec4(block * 0.25, 1.0));
		}
	}
	for (uint y = 0u; y < 2u; y++) {
		for (uint x = 0u; x < 2u; x++) {
			vec3 block = imageLoad(mip4_image, ivec2(tile) * 6 + ivec2(1, 1) + ivec2(x * 2u, y * 2u)).rgb;
			imageStore(mip2_image, ivec2(tile) * 4 + ivec2(1, 1) + ivec2(x, y), vec4(block, 1.0));
		}
	}
	ivec2 origin = ivec2(tile) * 10;
	for (int i = 0; i < 8; i++) {
		imageStore(radiance_image, origin + ivec2(0, 1 + i), imageLoad(radiance_image, origin + ivec2(1, 1 + i)));
		imageStore(radiance_image, origin + ivec2(9, 1 + i), imageLoad(radiance_image, origin + ivec2(8, 1 + i)));
		imageStore(radiance_image, origin + ivec2(1 + i, 0), imageLoad(radiance_image, origin + ivec2(1 + i, 1)));
		imageStore(radiance_image, origin + ivec2(1 + i, 9), imageLoad(radiance_image, origin + ivec2(1 + i, 8)));
	}
	imageStore(radiance_image, origin, imageLoad(radiance_image, origin + ivec2(1, 1)));
	imageStore(radiance_image, origin + ivec2(9, 0), imageLoad(radiance_image, origin + ivec2(8, 1)));
	imageStore(radiance_image, origin + ivec2(0, 9), imageLoad(radiance_image, origin + ivec2(1, 8)));
	imageStore(radiance_image, origin + ivec2(9, 9), imageLoad(radiance_image, origin + ivec2(8, 8)));
}

void main() {
#ifdef MODE_SH
	nextgi_wrc_project();
#else
	nextgi_wrc_trace();
#endif
}
