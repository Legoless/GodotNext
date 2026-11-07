#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_query : require

// HWRT diffuse gather (#25). 8x8, no workgroup memory. One opaque proceed per query,
// via nextgi_trace. Arena positions are read from the chunk storage buffers
// (chunk index + byte offset). SPIRV-Cross rejects physical buffer addresses.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT nextgi_tlas;
layout(set = 0, binding = 26) uniform sampler2D nextgi_wrc_radiance;
layout(set = 0, binding = 27) uniform sampler2D nextgi_wrc_visibility;
layout(set = 0, binding = 29, std430) restrict buffer NextGIGatherConfidence {
	float data[];
}
confidence_data;
layout(set = 0, binding = 30) uniform sampler2D hiz_tex;
layout(set = 0, binding = 31) uniform sampler2D last_frame_tex;
// GodotNext: nextgi_reflect (#60). The reflect variant reuses the gather binding set and writes
// the specular radiance plus its confidence into RB_TEX_REFLECTION.
layout(set = 0, binding = 32) uniform sampler2D normal_roughness_tex;
layout(rgba16f, set = 0, binding = 33) uniform image2D reflection_image;
// GodotNext: nextgi_firefly_clamp (#54). Clamped-energy counter (Q16): removed and total sample
// luminance in quarter units, summed with 32-bit integer atomics (order independent).
// GodotNext: nextgi_blue_noise (#54). 64x64 RG8 void-and-cluster tile.
layout(set = 0, binding = 38) uniform sampler2D blue_noise_tex;
layout(set = 0, binding = 37, std430) buffer NextGIClampTotals {
	uint removed;
	uint total;
}
clamp_totals;
layout(r16f, set = 0, binding = 35) uniform image2D specular_hit_image;
layout(rgba16f, set = 0, binding = 36) uniform image2D specular_debug_image;
layout(set = 0, binding = 34, std430) buffer NextGIReflectCounters {
	uint traced;
	uint hits;
	uint misses;
	uint pad;
}
reflect_counters;
layout(set = 0, binding = 28, std430) restrict readonly buffer NextGIGatherWRCParams {
	vec4 camera;
	vec4 sky;
	vec4 origin_spacing;
	uvec4 dispatch;
	uint page_rect_count;
	uint pad0;
	uint pad1;
	uint pad2;
}
nextgi_wrc;
#ifdef MODE_FAR_FIELD
layout(set = 0, binding = 39) uniform accelerationStructureEXT far_tlas;
#define NEXTGI_TLAS far_tlas
#else
#define NEXTGI_TLAS nextgi_tlas
#endif
#define NEXTGI_WRC
#include "nextgi_rt_inc.glsl"

// GodotNext: nextgi_blue_noise (#54). R2-rotated 64x64 tile lookup, wrapping by bitmask.
vec2 nextgi_blue_noise_sample(ivec2 p_pixel, uint p_frame) {
	vec2 offset = fract(vec2(p_frame) * vec2(0.7548776662, 0.5698402910)) * 64.0;
	ivec2 coord = (p_pixel + ivec2(offset)) & ivec2(63);
	return texelFetch(blue_noise_tex, coord, 0).rg;
}

layout(set = 0, binding = 1) uniform sampler2D depth_tex;
layout(set = 0, binding = 2) uniform sampler2D radiance_tex;
layout(set = 0, binding = 3) uniform sampler2D material_depth_tex;
layout(set = 0, binding = 4) uniform sampler2D sky_tex;
layout(set = 0, binding = 5) uniform sampler2D src_indirect_tex;

layout(set = 0, binding = 6, std430) restrict readonly buffer NextGIGatherCards {
	NextGICard data[];
}
cards;

layout(set = 0, binding = 7, std430) restrict readonly buffer NextGIGatherCardSets {
	NextGICardSet data[];
}
card_sets;

layout(set = 0, binding = 8, std430) restrict readonly buffer NextGIGatherRecords {
	NextGIInstanceRecord data[];
}
records;

layout(set = 0, binding = 9, std430) restrict readonly buffer NextGIGatherSurfaces {
	NextGISurfaceRecord data[];
}
surfaces;

layout(set = 0, binding = 10, std430) restrict readonly buffer NextGIChunk0 {
	uint data[];
}
chunk0;
layout(set = 0, binding = 11, std430) restrict readonly buffer NextGIChunk1 {
	uint data[];
}
chunk1;
layout(set = 0, binding = 12, std430) restrict readonly buffer NextGIChunk2 {
	uint data[];
}
chunk2;
layout(set = 0, binding = 13, std430) restrict readonly buffer NextGIChunk3 {
	uint data[];
}
chunk3;

layout(set = 0, binding = 14, std430) restrict readonly buffer NextGIPageRects {
	uvec2 data[];
}
page_rects;

layout(rgba16f, set = 0, binding = 15) uniform image2D probe_image;
layout(rgba16f, set = 0, binding = 16) uniform image2D ambient_image;
layout(rgba16f, set = 0, binding = 17) uniform image2D indirect_image;

layout(set = 0, binding = 18, std430) restrict readonly buffer NextGIGatherParams {
	mat4 inv_view_proj;
	mat4 prev_view_proj;
	vec4 camera_pos;
	vec4 prev_camera_pos;
	vec4 ambient_color;
	vec4 screen_jitter; // xy screen size, zw hammersley jitter in the tile
	uint frame;
	uint probes_x;
	uint probes_y;
	float rt_radius;
	float max_origin_offset;
	uint sky_enabled;
	uint feedback_count;
	uint feedback_stride;
	uint record_count;
	uint velocity_enabled;
	uint page_rect_count;
	uint pad0;
	uint pad1;
	uint pad2;
	float max_trace_roughness;
	float reflect_divisor;
	uint reflect_enabled;
	uint reflect_debug;
	uint sampling_mode;
	uint pad4;
	uint ray_stride;
	uint ray_count;
	// GodotNext: nextgi_reflection_behaviours (#132). Fog at reflection hits.
	uint fog_enabled;
	uint fog_depth_mode;
	float fog_density;
	float fog_height;
	float fog_height_density;
	float fog_depth_begin;
	float fog_depth_end;
	float fog_depth_curve;
	vec4 fog_light_color;
	float far_distance;
	uint far_occlusion_only;
	uint front_layer;
	uint front_pad0;
	uint front_pad1;
	uint front_pad2;
}
params;

layout(set = 0, binding = 19, std430) buffer NextGIGatherCounters {
	uint rays;
	uint misses;
	uint history_resets;
	uint uncovered;
	uint coverage_hits;
	uint coverage_rays;
	uint miss_radiance_bits; // packed as a scaled luminance
	uint wrc_samples;
	uint sky_samples;
	uint screen_hits;
}
counters;

layout(set = 0, binding = 20, std430) restrict readonly buffer NextGIFeedbackPages {
	uvec4 data[]; // x = indirect texel (x | y << 16), y = record, z = card
}
feedback_pages;

struct NextGIRefMaterial {
	vec4 albedo_metal;
	vec4 emission_roughness;
	vec4 specular_mode;
};

layout(set = 0, binding = 23, std430) restrict readonly buffer NextGIGatherRefMaterials {
	NextGIRefMaterial data[];
}
materials;

layout(set = 0, binding = 24, std430) restrict readonly buffer NextGIGatherRefLights {
	vec4 data[];
}
lights;

layout(set = 0, binding = 25, std430) restrict readonly buffer NextGIGatherRefParams {
	vec4 cam_origin_fov;
	vec4 cam_forward_aspect;
	vec4 cam_up;
	vec4 cam_right;
	uvec4 dispatch;
	uvec4 counts;
	vec4 sky_color;
	vec4 exposure_clear;
}
ref_params;

#ifdef MODE_HIT_RADIANCE_REFERENCE
#include "nextgi_reference_pt_inc.glsl"
#endif

#include "../oct_inc.glsl"

// D6 helpers. They stay in this shader: the RD header builder pastes an include once, and these
// functions need the buffers declared above. nextgi_trace stays in nextgi_rt_inc.glsl.
uint nextgi_chunk_uint(uint chunk, uint byte_offset) {
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

vec3 nextgi_chunk_vec3(uint chunk, uint byte_offset) {
	return vec3(uintBitsToFloat(nextgi_chunk_uint(chunk, byte_offset)), uintBitsToFloat(nextgi_chunk_uint(chunk, byte_offset + 4u)), uintBitsToFloat(nextgi_chunk_uint(chunk, byte_offset + 8u)));
}

// Arena float3 positions and uint32 indices. This is not a ray-query position fetch.
bool nextgi_hit_object_pos_normal(NextGIHit hit, out vec3 object_pos, out vec3 face_normal) {
	if (!hit.hit || hit.custom_index >= params.record_count) {
		return false;
	}
	NextGIInstanceRecord rec = records.data[hit.custom_index];
	if (hit.geometry_index >= rec.surface_count) {
		return false;
	}
	NextGISurfaceRecord surface = surfaces.data[rec.surface_base + hit.geometry_index];
	uint index_byte = surface.index_offset + hit.primitive * 12u;
	uint i0 = nextgi_chunk_uint(rec.arena_chunk, index_byte);
	uint i1 = nextgi_chunk_uint(rec.arena_chunk, index_byte + 4u);
	uint i2 = nextgi_chunk_uint(rec.arena_chunk, index_byte + 8u);
	vec3 p0 = nextgi_chunk_vec3(rec.arena_chunk, surface.vertex_offset + i0 * 12u);
	vec3 p1 = nextgi_chunk_vec3(rec.arena_chunk, surface.vertex_offset + i1 * 12u);
	vec3 p2 = nextgi_chunk_vec3(rec.arena_chunk, surface.vertex_offset + i2 * 12u);
	object_pos = p0 * (1.0 - hit.bary.x - hit.bary.y) + p1 * hit.bary.x + p2 * hit.bary.y;
	face_normal = normalize(cross(p1 - p0, p2 - p0));
	if (!hit.front_face) {
		face_normal = -face_normal;
	}
	return true;
}

// GodotNext: nextgi_skinned_actors (#59). D6 samples a skinned surface's bind-pose cards at
// the posed hit's triangle and barycentrics; bit 0 of flags marks the posed slot and bits
// 8-31 hold the bind-pose vertex offset in 16-byte units.
bool nextgi_hit_bind_pose_pos_normal(NextGIHit hit, out vec3 object_pos, out vec3 face_normal) {
	if (!hit.hit || hit.custom_index >= params.record_count) {
		return false;
	}
	NextGIInstanceRecord rec = records.data[hit.custom_index];
	if (hit.geometry_index >= rec.surface_count) {
		return false;
	}
	NextGISurfaceRecord surface = surfaces.data[rec.surface_base + hit.geometry_index];
	uint vertex_base = surface.vertex_offset;
	if ((surface.flags & 1u) != 0u) {
		vertex_base = (surface.flags >> 8u) * 16u;
	}
	uint index_byte = surface.index_offset + hit.primitive * 12u;
	uint i0 = nextgi_chunk_uint(rec.arena_chunk, index_byte);
	uint i1 = nextgi_chunk_uint(rec.arena_chunk, index_byte + 4u);
	uint i2 = nextgi_chunk_uint(rec.arena_chunk, index_byte + 8u);
	vec3 p0 = nextgi_chunk_vec3(rec.arena_chunk, vertex_base + i0 * 12u);
	vec3 p1 = nextgi_chunk_vec3(rec.arena_chunk, vertex_base + i1 * 12u);
	vec3 p2 = nextgi_chunk_vec3(rec.arena_chunk, vertex_base + i2 * 12u);
	object_pos = p0 * (1.0 - hit.bary.x - hit.bary.y) + p1 * hit.bary.x + p2 * hit.bary.y;
	face_normal = normalize(cross(p1 - p0, p2 - p0));
	if (!hit.front_face) {
		face_normal = -face_normal;
	}
	return true;
}

// D6. uncovered is set when only the best-axis card, without the depth test, qualified.
bool nextgi_src_lookup(NextGIHit hit, out vec3 radiance, out bool uncovered) {
	radiance = vec3(0.0);
	uncovered = false;
	vec3 object_pos;
	vec3 face_normal;
	if (!nextgi_hit_bind_pose_pos_normal(hit, object_pos, face_normal)) {
		return false;
	}
	NextGIInstanceRecord rec = records.data[hit.custom_index];
	if (rec.card_set == 0xFFFFFFFFu || rec.page_table == 0xFFFFFFFFu) {
		return false;
	}
	// Bit 31 marks a page that was evicted. D6 treats that as uncovered until it is resident again.
	if ((rec.page_table & 0x80000000u) != 0u) {
		uncovered = true;
		return false;
	}
	NextGICardSet card_set = card_sets.data[rec.card_set];
	float best_align = -2.0;
	float fallback_align = -2.0;
	vec3 best = vec3(0.0);
	vec3 fallback = vec3(0.0);
	bool found = false;
	bool found_fallback = false;
	for (uint i = 0u; i < card_set.card_count; i++) {
		NextGICard card = cards.data[card_set.first_card + i];
		vec3 rel = object_pos - card.origin.xyz;
		float u = dot(rel, card.axis_u.xyz);
		float v = dot(rel, card.axis_v.xyz);
		float depth = -dot(rel, card.axis_n.xyz);
		if (abs(u) > card.axis_u.w || abs(v) > card.axis_v.w) {
			continue;
		}
		float align = dot(face_normal, card.axis_n.xyz);
		vec2 card_size = max(vec2(card.atlas_rect.zw), vec2(1.0));
		vec2 texel = vec2((u / card.axis_u.w) * 0.5 + 0.5, (v / card.axis_v.w) * 0.5 + 0.5) * card_size;
		ivec2 material = ivec2(card.atlas_rect.xy) + ivec2(clamp(texel, vec2(0.0), card_size - 1.0));
		float card_depth = texelFetch(material_depth_tex, material, 0).r;
		uint page_index = rec.page_table + i;
		if (page_index >= params.page_rect_count) {
			continue;
		}
		uvec2 page = page_rects.data[page_index];
		uvec2 page_origin = uvec2(page.x & 0xFFFFu, page.x >> 16u);
		uvec2 page_size = max(uvec2(page.y & 0xFFFFu, page.y >> 16u), uvec2(1u));
		ivec2 radiance_coord = ivec2(page_origin) + ivec2(clamp(texel / card_size * vec2(page_size), vec2(0.0), vec2(page_size) - 1.0));
		vec3 sample_radiance = texelFetch(radiance_tex, radiance_coord, 0).rgb;
		if (align > fallback_align) {
			fallback_align = align;
			fallback = sample_radiance;
			found_fallback = true;
		}
		if (card_depth > 0.0 && abs(card_depth - depth) <= 2.0 * card_set.texel_size && align > best_align) {
			best_align = align;
			best = sample_radiance;
			found = true;
		}
	}
	if (found) {
		radiance = best;
		uncovered = false;
		return true;
	}
	return false;
}

const float PI = 3.14159265;

float nextgi_radical_inverse(uint bits) {
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return float(bits) * 2.3283064365386963e-10;
}

vec3 nextgi_unproject(vec2 pixel, float depth) {
	vec2 uv = (pixel + 0.5) / params.screen_jitter.xy;
	vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
	vec4 world = params.inv_view_proj * ndc;
	return world.xyz / world.w;
}

vec3 nextgi_geometric_normal(ivec2 pixel, float depth, vec3 world) {
	ivec2 size = ivec2(params.screen_jitter.xy);
	ivec2 px = pixel + ivec2(1, 0);
	ivec2 py = pixel + ivec2(0, 1);
	float dx = texelFetch(depth_tex, clamp(px, ivec2(0), size - 1), 0).r;
	float dy = texelFetch(depth_tex, clamp(py, ivec2(0), size - 1), 0).r;
	vec3 wx = nextgi_unproject(vec2(px), dx);
	vec3 wy = nextgi_unproject(vec2(py), dy);
	vec3 normal = normalize(cross(wx - world, wy - world));
	if (dot(normal, params.camera_pos.xyz - world) < 0.0) {
		normal = -normal;
	}
	return normal;
}

vec3 nextgi_orient(vec3 normal, vec3 local_dir) {
	vec3 up = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(up, normal));
	vec3 bitangent = cross(normal, tangent);
	return normalize(tangent * local_dir.x + bitangent * local_dir.y + normal * local_dir.z);
}

vec3 nextgi_miss_radiance(vec3 direction) {
	if (params.sky_enabled != 0u) {
		return textureLod(sky_tex, vec3_to_oct(direction), 0.0).rgb;
	}
	return params.ambient_color.rgb;
}

vec3 nextgi_shade_hit(NextGIHit hit, vec3 ray_dir, inout uint uncovered_count, out float confidence) {
	confidence = 0.0;
#ifdef MODE_HIT_RADIANCE_REFERENCE
	// 16 estimator samples at the hit. Misses stay on the sky path in the caller.
	vec3 object_pos;
	vec3 face_normal;
	if (!nextgi_hit_object_pos_normal(hit, object_pos, face_normal) || hit.custom_index >= params.record_count) {
		return nextgi_miss_radiance(ray_dir);
	}
	confidence = 1.0;
	NextGIInstanceRecord rec = records.data[hit.custom_index];
	vec3 world = nextgi_pt_mul_point(rec, object_pos);
	vec3 face = nextgi_pt_mul_normal(rec, face_normal);
	vec3 accum = vec3(0.0);
	uint rng = nextgi_pt_hash(hit.custom_index ^ params.frame ^ ref_params.counts.x);
	for (uint s = 0u; s < 16u; s++) {
		accum += nextgi_pt_radiance(world + face * rec.origin_offset, -ray_dir, rng);
	}
	return accum / 16.0;
#else
	vec3 radiance;
	bool uncovered = false;
	if (!nextgi_src_lookup(hit, radiance, uncovered)) {
		vec3 missed_pos;
		vec3 missed_normal;
		if (nextgi_hit_object_pos_normal(hit, missed_pos, missed_normal) && hit.custom_index < params.record_count) {
			NextGIInstanceRecord rec = records.data[hit.custom_index];
			vec3 world = vec3(
					dot(vec3(rec.object_to_world[0], rec.object_to_world[1], rec.object_to_world[2]), missed_pos) + rec.object_to_world[3],
					dot(vec3(rec.object_to_world[4], rec.object_to_world[5], rec.object_to_world[6]), missed_pos) + rec.object_to_world[7],
					dot(vec3(rec.object_to_world[8], rec.object_to_world[9], rec.object_to_world[10]), missed_pos) + rec.object_to_world[11]);
			vec3 cached = nextgi_wrc_sample(world, -ray_dir);
			if (nextgi_wrc_hit_cache) {
				confidence = nextgi_wrc_confidence;
				uncovered_count++;
				return cached;
			}
		}
		return nextgi_miss_radiance(ray_dir);
	}
	confidence = 1.0;
	if (uncovered) {
		uncovered_count++;
	}
	return radiance;
#endif
}

// Hi-Z march, at most 24 steps. A hit samples the last frame. A miss leaves march_end on the ray.
bool nextgi_screen_trace(vec3 origin, vec3 direction, float tmax, out vec3 color, out vec3 march_end) {
	color = vec3(0.0);
	march_end = origin;
	if (params.pad0 == 0u) {
		return false;
	}
	mat4 view_proj = inverse(params.inv_view_proj);
	vec3 far_pos = origin + direction * tmax;
	vec4 clip0 = view_proj * vec4(origin, 1.0);
	vec4 clip1 = view_proj * vec4(far_pos, 1.0);
	if (clip0.w <= 0.001) {
		return false;
	}
	vec3 ndc0 = clip0.xyz / clip0.w;
	vec3 ndc1 = clip1.w > 0.001 ? clip1.xyz / clip1.w : ndc0;
	vec2 uv0 = ndc0.xy * 0.5 + 0.5;
	vec2 uv1 = ndc1.xy * 0.5 + 0.5;
	vec2 screen = max(params.screen_jitter.xy, vec2(1.0));
	float mip = clamp(log2(max(length((uv1 - uv0) * screen) / 24.0, 1.0)), 0.0, float(params.pad1));
	bool left_screen = false;
	for (int step = 0; step < 24; step++) {
		float t = (float(step) + 0.5) / 24.0;
		vec2 uv = mix(uv0, uv1, t);
		march_end = mix(origin, far_pos, t);
		if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) {
			left_screen = true;
			break;
		}
		float scene_z = textureLod(hiz_tex, uv, mip).r;
		ivec2 pixel = ivec2(clamp(uv * screen, vec2(0.0), screen - 1.0));
		vec3 scene_pos = nextgi_unproject(vec2(pixel), scene_z);
		float scene_dist = max(length(scene_pos - params.camera_pos.xyz), 0.001);
		float ray_dist = length(march_end - params.camera_pos.xyz);
		if (ray_dist > scene_dist && ray_dist < scene_dist + 0.1 * scene_dist) {
			color = textureLod(last_frame_tex, uv, 0.0).rgb;
			return true;
		}
	}
	if (!left_screen) {
		march_end = origin;
	}
	return false;
}

void nextgi_gather_main() {
	uvec2 probe = gl_GlobalInvocationID.xy;
	if (probe.x >= params.probes_x || probe.y >= params.probes_y) {
		return;
	}
	vec2 pixel = vec2(probe) * 16.0 + params.screen_jitter.zw * 16.0;
	ivec2 ipixel = ivec2(clamp(pixel, vec2(0.0), params.screen_jitter.xy - 1.0));
	float depth = texelFetch(depth_tex, ipixel, 0).r;
	if (depth <= 0.0) {
		confidence_data.data[probe.y * params.probes_x + probe.x] = 0.0;
		return;
	}
	vec3 world = nextgi_unproject(vec2(ipixel), depth);
	vec3 normal = nextgi_geometric_normal(ipixel, depth, world);
	vec3 to_camera = normalize(params.camera_pos.xyz - world);
	float anchor_t = length(params.camera_pos.xyz - world);
	NextGIHit anchor;
	float origin_offset = params.max_origin_offset;
	if (nextgi_trace(params.camera_pos.xyz, -to_camera, anchor_t + 0.05, anchor)) {
		origin_offset = records.data[anchor.custom_index].origin_offset;
	}
	vec3 origin = world + normal * origin_offset;

	vec3 raw_radiance[64];
	float angle = float(params.frame) * 2.3999632;
	if (params.sampling_mode == 0u) {
		angle = nextgi_blue_noise_sample(ivec2(gl_GlobalInvocationID.xy), params.frame).x * 6.2831853;
	}
	float ca = cos(angle);
	float sa = sin(angle);
	uint uncovered_count = 0u;
	float confidence_sum = 0.0;
	for (uint i = 0u; i < params.ray_count; i++) {
		uint ix = i % params.ray_stride;
		uint iy = i / params.ray_stride;
		vec2 uv = (vec2(ix, iy) + 0.5) / float(params.ray_stride);
		uv = uv * 2.0 - 1.0;
		uv = vec2(ca * uv.x - sa * uv.y, sa * uv.x + ca * uv.y);
		vec3 dir = nextgi_orient(normal, oct_to_vec3(uv));
		if (dot(dir, normal) < 0.0) {
			dir = -dir;
		}
		NextGIHit hit;
		vec3 radiance;
		float ray_confidence = 0.0;
		atomicAdd(counters.rays, 1u);
		float handoff = nextgi_wrc_handoff(origin);
		float tmax = handoff > 0.0 ? handoff : params.rt_radius;
		vec3 screen_color;
		vec3 march_end;
		bool screen_hit = nextgi_screen_trace(origin, dir, tmax, screen_color, march_end);
		float traveled = length(march_end - origin);
		vec3 trace_origin = origin;
		float trace_tmax = tmax;
		if (traveled > 0.001 && traveled + 0.001 < tmax) {
			trace_origin = march_end;
			trace_tmax = tmax - traveled;
		}
		if (screen_hit) {
			radiance = screen_color;
			ray_confidence = 1.0;
			atomicAdd(counters.screen_hits, 1u);
		} else if (nextgi_trace(trace_origin, dir, trace_tmax, hit)) {
			radiance = nextgi_shade_hit(hit, dir, uncovered_count, ray_confidence);
		} else {
			radiance = nextgi_wrc_sample(trace_origin + dir * trace_tmax, dir);
			if (!nextgi_wrc_hit_cache) {
				radiance = nextgi_miss_radiance(dir);
				atomicAdd(counters.sky_samples, 1u);
			} else {
				ray_confidence = nextgi_wrc_confidence;
				atomicAdd(counters.wrc_samples, 1u);
				uncovered_count++;
			}
			atomicAdd(counters.misses, 1u);
			uint lum = uint(clamp(dot(radiance, vec3(0.2126, 0.7152, 0.0722)) * 64.0, 0.0, 4095.0));
			atomicAdd(counters.miss_radiance_bits, lum);
		}
		confidence_sum += ray_confidence;
		raw_radiance[i] = radiance;
	}

	// GodotNext: nextgi_firefly_clamp (#54). Clamp each raw sample's luminance to mean + 3 sigma of
	// this probe's 16 rays (emissive radiance included), then count the removed energy for Q16.
	const vec3 luminance_weights = vec3(0.2126, 0.7152, 0.0722);
	float lum_sum = 0.0;
	for (uint i = 0u; i < params.ray_count; i++) {
		lum_sum += dot(raw_radiance[i], luminance_weights);
	}
	const float mean_lum = lum_sum / float(params.ray_count);
	float variance = 0.0;
	for (uint i = 0u; i < params.ray_count; i++) {
		const float delta = dot(raw_radiance[i], luminance_weights) - mean_lum;
		variance += delta * delta;
	}
	const float clamp_limit = mean_lum + 3.0 * sqrt(variance / float(params.ray_count));
	float removed = 0.0;
	const uint texels = params.ray_stride * params.ray_stride;
	for (uint t = 0u; t < texels; t++) {
		vec3 radiance = raw_radiance[t % params.ray_count];
		const float luminance = dot(radiance, luminance_weights);
		if (luminance > clamp_limit && luminance > 0.0) {
			radiance *= clamp_limit / luminance;
			removed += luminance - clamp_limit;
		}
		// Raw radiance: the temporal pass in NextGIDenoise is the single accumulator (#53, D8).
		ivec2 texel = ivec2(probe) * int(params.ray_stride) + ivec2(t % params.ray_stride, t / params.ray_stride);
		imageStore(probe_image, texel, vec4(radiance, length(world - params.camera_pos.xyz)));
	}
	atomicAdd(clamp_totals.removed, uint(removed * 4.0));
	atomicAdd(clamp_totals.total, uint(lum_sum * 4.0));
	if (uncovered_count > 0u) {
		atomicAdd(counters.uncovered, uncovered_count);
	}
	confidence_data.data[probe.y * params.probes_x + probe.x] = confidence_sum / 16.0;
}

void nextgi_integrate_main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	ivec2 ambient_size = imageSize(ambient_image);
	if (pixel.x >= ambient_size.x || pixel.y >= ambient_size.y) {
		return;
	}
	vec2 screen = max(params.screen_jitter.xy, vec2(1.0));
	vec2 full = (vec2(pixel) + 0.5) * screen / vec2(max(ambient_size, ivec2(1))) - 0.5;
	ivec2 ipixel = ivec2(clamp(full, vec2(0.0), screen - 1.0));
	float depth = texelFetch(depth_tex, ipixel, 0).r;
	if (depth <= 0.0) {
		imageStore(ambient_image, pixel, vec4(0.0));
		return;
	}
	vec3 world = nextgi_unproject(full, depth);
	vec3 normal = nextgi_geometric_normal(ipixel, depth, world);
	vec2 probe_f = full / 16.0 - 0.5;
	ivec2 base = ivec2(floor(probe_f));
	vec2 fract_p = probe_f - vec2(base);
	vec3 accum = vec3(0.0);
	float weight = 0.0;
	float conf_accum = 0.0;
	for (int oy = 0; oy <= 1; oy++) {
		for (int ox = 0; ox <= 1; ox++) {
			ivec2 id = base + ivec2(ox, oy);
			if (id.x < 0 || id.y < 0 || id.x >= int(params.probes_x) || id.y >= int(params.probes_y)) {
				continue;
			}
			float bilinear = (ox == 0 ? 1.0 - fract_p.x : fract_p.x) * (oy == 0 ? 1.0 - fract_p.y : fract_p.y);
			vec4 probe = imageLoad(probe_image, id * 4);
			float depth_weight = probe.a <= 0.0 ? 0.0 : 1.0 / (1.0 + abs(length(world - params.camera_pos.xyz) - probe.a) * 4.0);
			vec3 irradiance = vec3(0.0);
			for (uint i = 0u; i < params.ray_stride * params.ray_stride; i++) {
				irradiance += imageLoad(probe_image, id * int(params.ray_stride) + ivec2(i % params.ray_stride, i / params.ray_stride)).rgb;
			}
			irradiance /= float(params.ray_stride * params.ray_stride);
			float w = bilinear * depth_weight * max(dot(normal, normal), 0.0);
			float probe_confidence = confidence_data.data[uint(id.y) * params.probes_x + uint(id.x)];
			accum += irradiance * w;
			conf_accum += probe_confidence * w;
			weight += w;
		}
	}
	vec3 out_irr = weight > 0.0 ? accum / weight : vec3(0.0);
	float out_confidence = weight > 0.0 ? conf_accum / weight : 0.0;
	imageStore(ambient_image, pixel, vec4(out_irr / PI, out_confidence));
}

void nextgi_feedback_main() {
	uint linear = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * params.feedback_stride;
	if (linear >= params.feedback_count) {
		return;
	}
	uvec4 block = feedback_pages.data[linear];
	uint record_index = block.y;
	if (record_index >= params.record_count) {
		return;
	}
	NextGIInstanceRecord rec = records.data[record_index];
	NextGICard card = cards.data[block.z];
	vec3 card_n = normalize(card.axis_n.xyz);
	vec3 normal = normalize(vec3(
			dot(vec3(rec.object_to_world[0], rec.object_to_world[4], rec.object_to_world[8]), card_n),
			dot(vec3(rec.object_to_world[1], rec.object_to_world[5], rec.object_to_world[9]), card_n),
			dot(vec3(rec.object_to_world[2], rec.object_to_world[6], rec.object_to_world[10]), card_n)));
	vec3 object_origin = card.origin.xyz + card.axis_n.xyz * rec.origin_offset;
	vec3 origin = vec3(
			dot(vec3(rec.object_to_world[0], rec.object_to_world[1], rec.object_to_world[2]), object_origin) + rec.object_to_world[3],
			dot(vec3(rec.object_to_world[4], rec.object_to_world[5], rec.object_to_world[6]), object_origin) + rec.object_to_world[7],
			dot(vec3(rec.object_to_world[8], rec.object_to_world[9], rec.object_to_world[10]), object_origin) + rec.object_to_world[11]);
	vec3 accum = vec3(0.0);
	uint uncovered_count = 0u;
	for (uint i = 0u; i < 16u; i++) {
		vec2 uv = vec2(float(i) / 16.0, nextgi_radical_inverse(i + params.frame));
		float radius = sqrt(uv.x);
		float phi = uv.y * 2.0 * PI + float(params.frame) * 0.1;
		vec3 local_dir = vec3(radius * cos(phi), radius * sin(phi), sqrt(max(1.0 - uv.x, 0.0)));
		vec3 dir = nextgi_orient(normal, local_dir);
		NextGIHit hit;
		vec3 radiance = nextgi_miss_radiance(dir);
		float handoff = nextgi_wrc_handoff(origin);
		float tmax = handoff > 0.0 ? handoff : params.rt_radius;
		if ((rec.rt_flags & 8u) != 0u) {
			radiance = nextgi_miss_radiance(dir);
		} else if (nextgi_trace(origin, dir, tmax, hit)) {
			float ignored_confidence = 0.0;
			radiance = nextgi_shade_hit(hit, dir, uncovered_count, ignored_confidence);
		} else {
			radiance = nextgi_wrc_sample(origin + dir * tmax, dir);
			if (!nextgi_wrc_hit_cache) {
				radiance = nextgi_miss_radiance(dir);
			} else {
				uncovered_count++;
			}
		}
		accum += radiance;
	}
	imageStore(indirect_image, ivec2(block.x & 0xFFFFu, block.x >> 16u), vec4(accum / 16.0 * PI, 1.0));
}

void nextgi_coverage_main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = ivec2(params.screen_jitter.xy) / 2;
	if (pixel.x >= size.x || pixel.y >= size.y) {
		return;
	}
	ivec2 full = pixel * 2;
	float depth = texelFetch(depth_tex, full, 0).r;
	if (depth <= 0.0) {
		return;
	}
	vec3 world = nextgi_unproject(vec2(full), depth);
	vec3 normal = nextgi_geometric_normal(full, depth, world);
	vec3 dir = normal;
	NextGIHit hit;
	if (!nextgi_trace(world + normal * params.max_origin_offset, dir, params.rt_radius, hit) || hit.t > 10.0) {
		return;
	}
	atomicAdd(counters.coverage_rays, 1u);
	vec3 radiance;
	bool uncovered = false;
	if (nextgi_src_lookup(hit, radiance, uncovered) && !uncovered) {
		atomicAdd(counters.coverage_hits, 1u);
	} else {
		// A WRC sample, or an SRC miss, is uncovered for Q2.
		atomicAdd(counters.uncovered, 1u);
	}
}

// GodotNext: nextgi_reflect (#60). One GGX VNDF reflection ray per trace pixel.
uint nextgi_reflect_hash(uint p_x) {
	p_x ^= p_x >> 16;
	p_x *= 0x7feb352du;
	p_x ^= p_x >> 15;
	p_x *= 0x846ca68bu;
	p_x ^= p_x >> 16;
	return p_x;
}

// GodotNext: nextgi_reflection_behaviours (#132). Mirrors the forward fog
// formula for a hit at p_distance/p_world; sky misses keep their radiance.
vec3 nextgi_reflection_fog(vec3 p_radiance, vec3 p_world, float p_distance) {
	if (params.fog_enabled == 0u || p_distance >= 65504.0) {
		return p_radiance;
	}
	float amount;
	if (params.fog_depth_mode != 0u) {
		float fog_z = smoothstep(params.fog_depth_begin, params.fog_depth_end, p_distance);
		amount = pow(fog_z, params.fog_depth_curve) * params.fog_density;
	} else {
		amount = 1.0 - exp(min(0.0, -p_distance * params.fog_density));
	}
	if (abs(params.fog_height_density) >= 0.0001) {
		float y_dist = p_world.y - params.fog_height;
		amount = max(1.0 - exp(min(0.0, y_dist * params.fog_height_density)), amount);
	}
	return mix(p_radiance, params.fog_light_color.rgb * params.fog_light_color.a, clamp(amount, 0.0, 1.0));
}

vec3 nextgi_reflect_sample_ggx(vec3 p_normal, vec3 p_view, float p_roughness, vec2 p_u) {
	float alpha = max(p_roughness * p_roughness, 1e-3);
	vec3 up = abs(p_normal.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 t1 = normalize(cross(up, p_normal));
	vec3 t2 = cross(p_normal, t1);
	vec3 v_local = vec3(dot(p_view, t1), dot(p_view, t2), dot(p_view, p_normal));
	float phi = 6.2831853 * p_u.x;
	float cos_theta = sqrt((1.0 - p_u.y) / (1.0 + (alpha * alpha - 1.0) * p_u.y));
	float sin_theta = sqrt(max(1.0 - cos_theta * cos_theta, 0.0));
	vec3 h_local = vec3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);
	vec3 h = normalize(t1 * h_local.x + t2 * h_local.y + p_normal * h_local.z);
	return reflect(-p_view, h);
}

void nextgi_reflect_main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	ivec2 full_size = ivec2(params.screen_jitter.xy);
	ivec2 size = ivec2(vec2(full_size) / max(params.reflect_divisor, 1.0));
	if (pixel.x >= size.x || pixel.y >= size.y) {
		return;
	}
	vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
	ivec2 full = ivec2(clamp(uv * vec2(full_size), vec2(0.0), vec2(full_size) - 1.0));
	float depth = texelFetch(depth_tex, full, 0).r;
	if (depth <= 0.0 || params.reflect_enabled == 0u) {
		imageStore(reflection_image, pixel, vec4(0.0));
		imageStore(specular_hit_image, pixel, vec4(65504.0));
		return;
	}
	atomicAdd(reflect_counters.traced, 1u);
	vec4 guide = texelFetch(normal_roughness_tex, full, 0);
	float roughness = guide.a;
	if (roughness > 0.5) {
		roughness = 1.0 - roughness;
	}
	roughness /= (127.0 / 255.0);
	vec3 normal = normalize(guide.xyz * 2.0 - 1.0);
	vec3 world = nextgi_unproject(vec2(full), depth);
	vec3 view_dir = normalize(params.camera_pos.xyz - world);
	vec3 origin = world + normal * params.max_origin_offset;

	uint hash = nextgi_reflect_hash(uint(uv.x * 65535.0) ^ (uint(uv.y * 65535.0) << 16) ^ params.frame);
	vec2 seed = vec2(float(hash & 0xFFFFu), float(hash >> 16)) / 65535.0;
	if (params.sampling_mode == 0u) {
		seed = nextgi_blue_noise_sample(pixel, params.frame);
	}

	vec3 radiance;
	float confidence = 0.0;
	float hit_distance = 65504.0;
	if (roughness <= params.max_trace_roughness) {
		vec3 direction = nextgi_reflect_sample_ggx(normal, view_dir, roughness, seed);
		if (dot(direction, normal) <= 0.0) {
			direction = reflect(-view_dir, normal);
		}
		float handoff = nextgi_wrc_handoff(origin);
		float tmax = handoff > 0.0 ? handoff : params.rt_radius;
		vec3 screen_color;
		vec3 march_end;
		bool screen_hit = nextgi_screen_trace(origin, direction, tmax, screen_color, march_end);
		if (params.front_layer != 0u) {
			screen_hit = false;
			march_end = origin;
		}
		float traveled = length(march_end - origin);
		vec3 trace_origin = origin;
		float trace_tmax = tmax;
		if (traveled > 0.001 && traveled + 0.001 < tmax) {
			trace_origin = march_end;
			trace_tmax = tmax - traveled;
		}
		NextGIHit hit;
#ifdef MODE_FAR_FIELD
		bool visibility_only = false;
#endif
		uint uncovered_count = 0u;
		if (screen_hit) {
			radiance = screen_color;
			confidence = 1.0;
			hit_distance = traveled;
			atomicAdd(reflect_counters.hits, 1u);
		} else {
			bool hit_result;
#ifdef MODE_FAR_FIELD
			hit = nextgi_trace_near_opaque(trace_origin, direction, trace_tmax);
			hit_result = hit.hit;
			if (!hit_result) {
				visibility_only = params.far_occlusion_only != 0u;
				hit_result = nextgi_trace(origin + direction * params.rt_radius, direction, max(params.far_distance - params.rt_radius, 0.0), hit);
				if (hit_result) {
					hit_distance = params.rt_radius + hit.t;
				}
			} else {
				hit_distance = traveled + hit.t;
			}
#else
			hit_result = nextgi_trace(trace_origin, direction, trace_tmax, hit);
			hit_distance = traveled + hit.t;
#endif
			if (hit_result) {
				atomicAdd(reflect_counters.hits, 1u);
#ifdef MODE_FAR_FIELD
				if (params.far_occlusion_only != 0u && (records.data[hit.custom_index].rt_flags & 8u) != 0u) {
					radiance = vec3(0.0);
					confidence = 1.0;
				} else {
					radiance = nextgi_shade_hit(hit, direction, uncovered_count, confidence);
				}
#else
				radiance = nextgi_shade_hit(hit, direction, uncovered_count, confidence);
#endif
			} else {
#ifdef MODE_FAR_FIELD
				if (params.far_occlusion_only != 0u) {
					radiance = vec3(1.0);
					confidence = 1.0;
				} else {
					radiance = nextgi_miss_radiance(direction);
				}
#else
				radiance = nextgi_wrc_sample(trace_origin + direction * trace_tmax, direction);
				if (!nextgi_wrc_hit_cache) {
					radiance = nextgi_miss_radiance(direction);
				} else {
					confidence = nextgi_wrc_confidence;
				}
#endif
				atomicAdd(reflect_counters.misses, 1u);
			}
		}
#ifdef MODE_FAR_FIELD
		if (!visibility_only) {
			radiance = nextgi_reflection_fog(radiance, origin + direction * hit_distance, hit_distance);
		}
#else
		radiance = nextgi_reflection_fog(radiance, origin + direction * hit_distance, hit_distance);
#endif
	} else {
		vec3 direction = reflect(-view_dir, normal);
		radiance = nextgi_wrc_sample(origin, direction);
		confidence = nextgi_wrc_confidence;
	}
	imageStore(reflection_image, pixel, vec4(radiance, clamp(confidence, 0.0, 1.0)));
	imageStore(specular_hit_image, pixel, vec4(hit_distance));
	if (params.reflect_debug != 0u) {
		float scaled = hit_distance >= 65504.0 ? 1.0 : clamp(log2(1.0 + hit_distance) / 8.0, 0.0, 1.0);
		imageStore(specular_debug_image, pixel, vec4(vec3(scaled), 1.0));
	}
}

void main() {
#ifdef MODE_INTEGRATE
	nextgi_integrate_main();
#elif defined(MODE_SRC_FEEDBACK)
	nextgi_feedback_main();
#elif defined(MODE_COVERAGE)
	nextgi_coverage_main();
#elif defined(MODE_REFLECT) || defined(MODE_FAR_FIELD)
	nextgi_reflect_main();
#else
	nextgi_gather_main();
#endif
}
