// GodotNext: ps_swrt_trace (#186). Not a standalone shader: the WRC update kernels (probe trace,
// SH projection, translucency mark, trace, project and volume), shared by the hardware update
// (nextgi_wrc_update.glsl) and the software sibling (nextgi_wrc_update_swrt.glsl). The includer
// includes nextgi_hit_inc.glsl and nextgi_records_inc.glsl first and defines nextgi_trace().
// The ray-query D6 lookup (bindings 9-17) is the hardware tier's; the sibling defines its own
// nextgi_src_lookup() with NEXTGI_TRACE_SDF.

#ifndef NEXTGI_WRC_UPDATE_INC_GLSL
#define NEXTGI_WRC_UPDATE_INC_GLSL

// GodotNext: nextgi_view_math. One reconstruction for every NextGI pass.
#include "nextgi_view_inc.glsl"

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

#ifndef NEXTGI_TRACE_SDF
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
#endif // !NEXTGI_TRACE_SDF
layout(set = 0, binding = 18) uniform sampler2D src_radiance_tex;
layout(set = 0, binding = 19) uniform sampler2D material_depth_tex;
// GodotNext: ps_lumen_lite (#183). Sky radiance for misses; params.sky.w = 1 when bound and the
// Environment reads sky light.
layout(set = 0, binding = 35) uniform sampler2D sky_radiance_tex;

layout(set = 0, binding = 20, std430) restrict readonly buffer NextGIWRCParams {
	vec4 camera; // xyz, w = 1 when the cache is valid
	vec4 sky; // Background color; w = 1 when misses read sky_radiance_tex.
	vec4 origin_spacing; // xyz snapped level-0 origin, w = 1 m spacing
	uvec4 dispatch; // probe_base, probe_count, frame, record_count
	uint page_rect_count;
	uint fresh_levels; // GodotNext: nextgi_wrc_shift (#254). Bit L: level L takes its trace without history.
	uint pad1;
	uint pad2;
	mat4 inv_view_proj; // GodotNext: nextgi_translucency_volume (#61).
	vec4 volume; // x width, y height, z near, w far.
	vec4 depth_coefficients; // GodotNext: nextgi_view_math. NextGIViewMath::depth_coefficients().
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
#ifdef NEXTGI_TRACE_SDF
	// GodotNext: ps_swrt_trace (#186). Probe-ray SDF hits, and the step-limit and near-origin ones.
	uint sdf_hits;
	uint sdf_exhausted;
	uint sdf_near;
#endif
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

#if defined(NEXTGI_TRACE_SDF) && !defined(MODE_TRANSLUCENCY_TRACE)
// GodotNext: ps_swrt_trace (#186). The software tier's probe-ray counters (nextgi_sdf_hit_inc.glsl).
void nextgi_sdf_note_wrc() {
	if ((nextgi_sdf_last.flags & SWRT_HIT_SURFACE) == 0u) {
		return;
	}
	atomicAdd(counters.sdf_hits, 1u);
	if ((nextgi_sdf_last.flags & SWRT_HIT_EXHAUSTED) != 0u) {
		atomicAdd(counters.sdf_exhausted, 1u);
	}
	if (nextgi_sdf_last_near()) {
		atomicAdd(counters.sdf_near, 1u);
	}
}
#endif

#ifndef NEXTGI_TRACE_SDF
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
	NextGISurfaceRecord surface = surfaces.data[rec.surface_base + hit.geometry_index];
	if ((surface.flags & 4u) != 0u) {
		// GodotNext: nextgi_nextgeo (#77). Outside-mask surface on a Stage-2 mesh falls back to WRC.
		return false;
	}
	NextGICardSet card_set = card_sets.data[rec.card_set];
	if ((surface.flags & 2u) != 0u) {
		// GodotNext: nextgi_nextgeo (#77). Stage-2 cards keyed by proxy-cut cluster group + dominant axis.
		uint group_byte = (surface.flags & ~15u) + hit.primitive * 2u;
		uint word = nextgi_wrc_chunk_uint(rec.arena_chunk, group_byte & ~3u);
		uint hit_group = ((group_byte & 2u) != 0u) ? (word >> 16u) : (word & 0xFFFFu);
		uint target_tag = hit_group + 1u;
		uint lo = 0u;
		uint hi = card_set.card_count;
		for (uint bstep = 0u; bstep < 20u && lo < hi; bstep++) {
			uint mid = (lo + hi) >> 1u;
			uint mid_tag = uint(cards.data[card_set.first_card + mid].origin.w + 0.5);
			if (mid_tag < target_tag) {
				lo = mid + 1u;
			} else {
				hi = mid;
			}
		}
		float best_align = 0.0;
		vec3 best = vec3(0.0);
		bool found = false;
		for (uint k = 0u; k < 6u && (lo + k) < card_set.card_count; k++) {
			uint i = lo + k;
			NextGICard card = cards.data[card_set.first_card + i];
			if (uint(card.origin.w + 0.5) != target_tag) {
				break;
			}
			float align = dot(face_normal, card.axis_n.xyz);
			if (align <= best_align) {
				continue;
			}
			vec3 rel = object_pos - card.origin.xyz;
			float u = dot(rel, card.axis_u.xyz);
			float v = dot(rel, card.axis_v.xyz);
			if (abs(u) > card.axis_u.w + 1e-4 || abs(v) > card.axis_v.w + 1e-4) {
				continue;
			}
			float depth = -dot(rel, card.axis_n.xyz);
			vec2 card_size = max(vec2(card.atlas_rect.zw), vec2(1.0));
			vec2 texel = vec2((u / card.axis_u.w) * 0.5 + 0.5, (v / card.axis_v.w) * 0.5 + 0.5) * card_size;
			ivec2 material = ivec2(card.atlas_rect.xy) + ivec2(clamp(texel, vec2(0.0), card_size - 1.0));
			float card_depth = texelFetch(material_depth_tex, material, 0).r;
			float depth_tol = 2.0 * max(card_set.texel_size, max(2.0 * card.axis_u.w / card_size.x, 2.0 * card.axis_v.w / card_size.y));
			if (!(card_depth > 0.0 && abs(card_depth - depth) <= depth_tol)) {
				continue;
			}
			uint page_index = rec.page_table + i;
			if (page_index >= params.page_rect_count) {
				continue;
			}
			uvec2 page = page_rects.data[page_index];
			uvec2 page_origin = uvec2(page.x & 0xFFFFu, page.x >> 16u);
			uvec2 page_size = max(uvec2(page.y & 0xFFFFu, page.y >> 16u), uvec2(1u));
			ivec2 radiance_coord = ivec2(page_origin) + ivec2(clamp(texel / card_size * vec2(page_size), vec2(0.0), vec2(page_size) - 1.0));
			best_align = align;
			best = texelFetch(src_radiance_tex, radiance_coord, 0).rgb;
			found = true;
		}
		radiance = best;
		return found;
	}
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
#endif // !NEXTGI_TRACE_SDF

// GodotNext: ps_lumen_lite (#183). The gather's miss term (nextgi_miss_radiance), octahedral sky.
vec3 nextgi_wrc_miss_radiance(vec3 direction) {
	if (params.sky.w < 0.5) {
		return params.sky.rgb;
	}
	vec3 n = direction / max(abs(direction.x) + abs(direction.y) + abs(direction.z), 1e-6);
	if (n.z < 0.0) {
		n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
	}
	return textureLod(sky_radiance_tex, n.xy * 0.5 + 0.5, 0.0).rgb;
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
	// GodotNext: nextgi_wrc_shift (#254). Since the level's origin moved, this cell's offset and
	// radiance belong to another position: no relocation, no blend and no stale fallback.
	bool fresh = (params.fresh_levels & (1u << level)) != 0u;
	vec4 previous_offset = fresh ? vec4(0.0) : imageLoad(offset_image, cell);
	probe_pos += previous_offset.xyz;
	ivec2 radiance_texel = nextgi_wrc_radiance_texel(probe, local);
	vec3 previous = fresh ? vec3(0.0) : imageLoad(radiance_image, radiance_texel).rgb;
	NextGIHit hit;
#ifdef MODE_TRANSLUCENCY_TRACE
	atomicAdd(counters.translucent_rays, 1u);
#else
	atomicAdd(counters.rays, 1u);
#endif
	vec3 sample_radiance = nextgi_wrc_miss_radiance(direction);
	bool back_face = false;
#if defined(NEXTGI_TRACE_SDF) && !defined(MODE_TRANSLUCENCY_TRACE)
	bool probe_hit = nextgi_trace(probe_pos, direction, 1.0e6, hit);
	nextgi_sdf_note_wrc();
	if (probe_hit) {
#else
	if (nextgi_trace(probe_pos, direction, 1.0e6, hit)) {
#endif
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
		// GodotNext: ps_lumen_lite (#183). w > 1.5 marks a probe inside geometry. 1.0 still counts as history.
		imageStore(offset_image, cell, vec4(moved - level_origin - (vec3(cell.xy, cell.z % 16) - vec3(7.5)) * spacing, fraction > 0.30 ? 2.0 : 1.0));
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
	// GodotNext: ps_lumen_lite (#183). Each mip texel averages the four texels it covers, so
	// mip 2 is a box filter of mip 4 (Lite's rough specular reads both).
	vec3 mip4_texels[16];
	for (uint y = 0u; y < 4u; y++) {
		for (uint x = 0u; x < 4u; x++) {
			vec3 block = imageLoad(radiance_image, nextgi_wrc_radiance_texel(probe, ivec2(x * 2u, y * 2u))).rgb;
			block += imageLoad(radiance_image, nextgi_wrc_radiance_texel(probe, ivec2(x * 2u + 1u, y * 2u))).rgb;
			block += imageLoad(radiance_image, nextgi_wrc_radiance_texel(probe, ivec2(x * 2u, y * 2u + 1u))).rgb;
			block += imageLoad(radiance_image, nextgi_wrc_radiance_texel(probe, ivec2(x * 2u + 1u, y * 2u + 1u))).rgb;
			mip4_texels[y * 4u + x] = block * 0.25;
			imageStore(mip4_image, ivec2(tile) * 6 + ivec2(1, 1) + ivec2(x, y), vec4(block * 0.25, 1.0));
		}
	}
	for (uint y = 0u; y < 2u; y++) {
		for (uint x = 0u; x < 2u; x++) {
			uint corner = y * 8u + x * 2u;
			vec3 block = (mip4_texels[corner] + mip4_texels[corner + 1u] + mip4_texels[corner + 4u] + mip4_texels[corner + 5u]) * 0.25;
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
	// GodotNext: nextgi_view_math. The slice is a view distance; reconstruct through its stored depth.
	vec3 world = nextgi_view_reconstruct(params.inv_view_proj, uv, nextgi_view_depth_from_linear(params.depth_coefficients, depth));
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
	vec3 previous_screen = nextgi_view_project(params.previous_view_proj, world);
	vec3 history_coord = vec3(0.0);
	if (previous_screen.z >= 0.0) {
		vec2 previous_uv = previous_screen.xy;
		float previous_depth = nextgi_view_linear_depth(params.depth_coefficients, previous_screen.z);
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

#endif // NEXTGI_WRC_UPDATE_INC_GLSL
