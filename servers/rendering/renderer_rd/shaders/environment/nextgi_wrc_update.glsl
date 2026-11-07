#[compute]

#version 460

#extension GL_EXT_ray_query : require

#VERSION_DEFINES

// World Radiance Cache update (#44, D15). 8x8, one probe per workgroup, one opaque proceed per ray.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT nextgi_tlas;
#define NEXTGI_TLAS nextgi_tlas
#include "nextgi_rt_inc.glsl"

#if defined(MODE_TRANSLUCENCY_TRACE) || defined(MODE_TRANSLUCENCY_PROJECT)
layout(rgba16f, set = 0, binding = 1) uniform image2D radiance_image;
#else
layout(r11f_g11f_b10f, set = 0, binding = 1) uniform image2D radiance_image;
#endif
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
	mat4 inv_view_proj; // GodotNext: nextgi_translucency_volume (#61).
	vec4 volume; // x width, y height, z near, w far.
	mat4 previous_view_proj;
	vec4 history; // blend, Lite, valid, sky lighting enabled.
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
	uint translucent_probes;
	uint translucent_rays;
	uint translucent_hits;
	uint translucent_sky_misses;
	uint translucent_sky_energy;
}
counters;

#if defined(MODE_TRANSLUCENCY_MARK) || defined(MODE_TRANSLUCENCY_TRACE) || defined(MODE_TRANSLUCENCY_PROJECT) || defined(MODE_TRANSLUCENCY_VOLUME)
layout(set = 0, binding = 22) uniform texture2D translucency_mark_tex;
layout(set = 0, binding = 23, std430) buffer NextGITranslucencyMarks {
	uint data[];
}
translucency_marks;
layout(set = 0, binding = 24, std430) buffer NextGITranslucencyIndices {
	uint count;
	uint data[];
}
translucency_indices;
layout(set = 0, binding = 34) uniform sampler translucency_sampler;
#endif

shared uint wrc_backfaces;
shared uint wrc_probe;
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
#ifdef MODE_TRANSLUCENCY_TRACE
		wrc_probe = 0xFFFFFFFFu;
		uint candidate = gl_WorkGroupID.x;
		uint mask = 1u << (candidate % 32u);
		if ((translucency_marks.data[candidate / 32u] & mask) != 0u) {
			uint slot = atomicAdd(translucency_indices.count, 1u);
			if (slot < 4096u) {
				translucency_indices.data[slot] = candidate;
				wrc_probe = candidate;
				atomicAdd(counters.translucent_probes, 1u);
			} else {
				atomicAnd(translucency_marks.data[candidate / 32u], ~mask);
			}
		}
#else
		wrc_probe = (params.dispatch.x + gl_WorkGroupID.x) % 16384u;
#endif
	}
	wrc_escape_samples[gl_LocalInvocationIndex] = vec3(0.0);
	barrier();
#ifdef MODE_TRANSLUCENCY_TRACE
	if (wrc_probe == 0xFFFFFFFFu) {
		return;
	}
#else
	if (gl_WorkGroupID.x >= params.dispatch.y) {
		return;
	}
#endif
	uint probe = wrc_probe;
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
#ifdef MODE_TRANSLUCENCY_TRACE
	atomicAdd(counters.translucent_rays, 1u);
#else
	atomicAdd(counters.rays, 1u);
#endif
	vec3 sample_radiance = params.sky.rgb;
	bool back_face = false;
	if (nextgi_trace(probe_pos, direction, 1.0e6, hit)) {
#ifdef MODE_TRANSLUCENCY_TRACE
		atomicAdd(counters.translucent_hits, 1u);
#else
		atomicAdd(counters.hits, 1u);
#endif
		bool uncovered = false;
		vec3 looked_up = vec3(0.0);
		if (!nextgi_src_lookup(hit, looked_up, uncovered)) {
			looked_up = previous;
		}
		sample_radiance = looked_up;
		back_face = !hit.front_face && hit.t < 0.5 * spacing;
	} else {
#ifdef MODE_TRANSLUCENCY_TRACE
		atomicAdd(counters.translucent_sky_misses, 1u);
		if (params.history.w < 0.5) {
			sample_radiance = vec3(0.0);
		}
		atomicAdd(counters.translucent_sky_energy, uint(clamp(dot(sample_radiance, vec3(0.2126, 0.7152, 0.0722)) * 64.0, 0.0, 4095.0)));
#else
		atomicAdd(counters.sky_misses, 1u);
#endif
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
	if (gl_LocalInvocationIndex != 0u) {
		return;
	}
#ifdef MODE_TRANSLUCENCY_PROJECT
	if (gl_WorkGroupID.x >= min(translucency_indices.count, 4096u)) {
		return;
	}
	uint probe = translucency_indices.data[gl_WorkGroupID.x];
#else
	if (gl_WorkGroupID.x >= params.dispatch.y) {
		return;
	}
	uint probe = (params.dispatch.x + gl_WorkGroupID.x) % 16384u;
#endif
	vec3 sum = vec3(0.0);
#ifdef MODE_TRANSLUCENCY_PROJECT
	vec3 l1r = vec3(0.0);
	vec3 l1g = vec3(0.0);
	vec3 l1b = vec3(0.0);
#endif
	for (uint y = 0u; y < 8u; y++) {
		for (uint x = 0u; x < 8u; x++) {
			vec3 sample_radiance = imageLoad(radiance_image, nextgi_wrc_radiance_texel(probe, ivec2(x, y))).rgb;
			sum += sample_radiance;
#ifdef MODE_TRANSLUCENCY_PROJECT
			vec3 direction = nextgi_wrc_oct_dir(ivec2(x, y));
			l1r += direction * sample_radiance.r;
			l1g += direction * sample_radiance.g;
			l1b += direction * sample_radiance.b;
#endif
		}
	}
	sum /= 64.0;
	ivec3 cell = nextgi_wrc_probe_cell(probe);
#ifdef MODE_TRANSLUCENCY_PROJECT
	imageStore(sh_r_image, cell, vec4(sum.r, l1r / 64.0));
	imageStore(sh_g_image, cell, vec4(sum.g, l1g / 64.0));
	imageStore(sh_b_image, cell, vec4(sum.b, l1b / 64.0));
#else
	imageStore(sh_r_image, cell, vec4(sum.r, 0.0, 0.0, 0.0));
	imageStore(sh_g_image, cell, vec4(sum.g, 0.0, 0.0, 0.0));
	imageStore(sh_b_image, cell, vec4(sum.b, 0.0, 0.0, 0.0));
#endif
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

#ifdef MODE_TRANSLUCENCY_MARK
void nextgi_translucency_mark_main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = ivec2(params.volume.xy);
	if (pixel.x >= size.x || pixel.y >= size.y) {
		return;
	}
	vec4 world = texelFetch(sampler2D(translucency_mark_tex, translucency_sampler), pixel, 0);
	if (world.a <= 0.0) {
		return;
	}
	float distance = length(world.xyz - params.camera.xyz);
	if (distance > 16.0) {
		return;
	}
	uint level = distance <= 8.0 ? 0u : 1u;
	float spacing = params.origin_spacing.w * float(1u << level);
	vec3 level_origin = floor(params.origin_spacing.xyz / spacing) * spacing;
	ivec3 cell = clamp(ivec3(round((world.xyz - level_origin) / spacing + 7.5)), ivec3(0), ivec3(15));
	uint probe = level * 4096u + uint(cell.x + cell.y * 16 + cell.z * 256);
	atomicOr(translucency_marks.data[probe / 32u], 1u << (probe % 32u));
}
#endif

#ifdef MODE_TRANSLUCENCY_VOLUME
layout(rgba16f, set = 0, binding = 25) uniform image3D translucency_r_image;
layout(rgba16f, set = 0, binding = 26) uniform image3D translucency_g_image;
layout(rgba16f, set = 0, binding = 27) uniform image3D translucency_b_image;
layout(rgba16f, set = 0, binding = 28) uniform image3D translucency_sh_r_image;
layout(rgba16f, set = 0, binding = 29) uniform image3D translucency_sh_g_image;
layout(rgba16f, set = 0, binding = 30) uniform image3D translucency_sh_b_image;
layout(set = 0, binding = 31) uniform texture3D translucency_history_r;
layout(set = 0, binding = 32) uniform texture3D translucency_history_g;
layout(set = 0, binding = 33) uniform texture3D translucency_history_b;

void nextgi_translucency_volume_main() {
	ivec3 froxel = ivec3(gl_GlobalInvocationID.xyz);
	ivec2 size = ivec2(params.volume.xy);
	if (froxel.x >= size.x || froxel.y >= size.y || froxel.z >= 32) {
		return;
	}
	vec2 uv = (vec2(froxel.xy) + 0.5) / vec2(size);
	float slice = (float(froxel.z) + 0.5) / 32.0;
	float depth = params.volume.z * pow(params.volume.w / params.volume.z, slice);
	vec4 clip = params.inv_view_proj * vec4(uv * 2.0 - 1.0, depth, 1.0);
	vec3 world = clip.xyz / clip.w;
	float cam_distance = length(world - params.camera.xyz);
	float base_level = clamp(floor(log2(max(cam_distance / 8.0, 1.0))), 0.0, 3.0);
	float level = min(base_level + (params.history.y > 0.5 ? 1.0 : 0.0), 3.0);
	float spacing = params.origin_spacing.w * exp2(level);
	vec3 level_origin = floor(params.origin_spacing.xyz / spacing) * spacing;
	vec3 rel = (world - level_origin) / spacing + 7.5;
	ivec3 cell = clamp(ivec3(round(rel)), ivec3(0), ivec3(15));
	uint probe = uint(level) * 4096u + uint(cell.x + cell.y * 16 + cell.z * 256);
	bool traced = params.history.y < 0.5 && level < 2.0 && (translucency_marks.data[probe / 32u] & (1u << (probe % 32u))) != 0u;
	vec4 red;
	vec4 green;
	vec4 blue;
	if (traced) {
		cell.z += int(level) * 16;
		red = imageLoad(translucency_sh_r_image, cell);
		green = imageLoad(translucency_sh_g_image, cell);
		blue = imageLoad(translucency_sh_b_image, cell);
	} else {
		cell.z += int(level) * 16;
		red = vec4(imageLoad(sh_r_image, cell).r, 0.0, 0.0, 0.0);
		green = vec4(imageLoad(sh_g_image, cell).r, 0.0, 0.0, 0.0);
		blue = vec4(imageLoad(sh_b_image, cell).r, 0.0, 0.0, 0.0);
	}
	vec4 previous_clip = params.previous_view_proj * vec4(world, 1.0);
	vec3 history_coord = vec3(0.0);
	if (previous_clip.w > 0.0) {
		vec2 previous_uv = previous_clip.xy / previous_clip.w * 0.5 + 0.5;
		float previous_depth = previous_clip.z / previous_clip.w;
		float previous_slice = clamp(log(max(previous_depth, params.volume.z) / params.volume.z) / log(params.volume.w / params.volume.z), 0.0, 1.0);
		history_coord = vec3(previous_uv, previous_slice);
	}
	float weight = params.history.z > 0.5 && all(greaterThanEqual(history_coord.xy, vec2(0.0))) && all(lessThanEqual(history_coord.xy, vec2(1.0))) ? params.history.x : 0.0;
	vec4 history_r = texture(sampler3D(translucency_history_r, translucency_sampler), history_coord);
	vec4 history_g = texture(sampler3D(translucency_history_g, translucency_sampler), history_coord);
	vec4 history_b = texture(sampler3D(translucency_history_b, translucency_sampler), history_coord);
	imageStore(translucency_r_image, froxel, mix(red, history_r, weight));
	imageStore(translucency_g_image, froxel, mix(green, history_g, weight));
	imageStore(translucency_b_image, froxel, mix(blue, history_b, weight));
}
#endif

void main() {
#ifdef MODE_SH
	nextgi_wrc_project();
#elif defined(MODE_TRANSLUCENCY_VOLUME)
	nextgi_translucency_volume_main();
#elif defined(MODE_TRANSLUCENCY_MARK)
	nextgi_translucency_mark_main();
#elif defined(MODE_TRANSLUCENCY_PROJECT)
	nextgi_wrc_project();
#else
	nextgi_wrc_trace();
#endif
}
