/**************************************************************************/
/*  nextgi_reflect.glsl                                                   */
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
#extension GL_EXT_ray_query : require

#VERSION_DEFINES

// GodotNext: nextgi_hit_lighting (#129, plan §21.2 contract L7). Reflection Mark pass:
// traces primary reflection rays for pixels up to max_roughness_to_trace and writes compact per-ray
// hit records plus 16-bit material keys for sorted-deferred hit lighting. Misses keep WRC/sky radiance.
// Only compiled and dispatched when hit lighting for reflections is active; Surface Cache mode keeps
// nextgi_gather.glsl's MODE_REFLECT untouched for byte-identical output.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "../oct_inc.glsl"
#include "nextgi_hit_lighting_common_inc.glsl"
#include "nextgi_reflect_inc.glsl"
#include "nextgi_view_inc.glsl"

struct NextGIHitSurfaceMark {
	uint attribute_chunk;
	uint attribute_offset;
	uint material_id;
	uint flags;
};

struct NextGIHitMaterialMark {
	uint params_slot;
	uint texture_base;
	uint texture_count;
	uint shader_slot;
};

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1) uniform sampler2D depth_tex;
layout(set = 0, binding = 2) uniform sampler2D normal_roughness_tex;
layout(set = 0, binding = 3) uniform sampler2D hiz_tex;
layout(set = 0, binding = 4) uniform sampler2D last_frame_tex;
layout(set = 0, binding = 5) uniform sampler2D sky_tex;
layout(set = 0, binding = 6) uniform sampler2D blue_noise_tex;
layout(set = 0, binding = 7) uniform sampler2D nextgi_wrc_radiance;
layout(set = 0, binding = 8) uniform sampler2D nextgi_wrc_visibility;

layout(set = 0, binding = 12, std430) restrict readonly buffer MarkWRCParams {
	vec4 camera;
	vec4 sky;
	vec4 origin_spacing;
	uvec4 dispatch;
	uint page_rect_count;
	uint fresh_levels;
	uint pad1;
	uint pad2;
}
nextgi_wrc;

// Define NEXTGI_TLAS before nextgi_rt_inc.glsl so nextgi_wrc_sample performs exact HWRT
// wall-visibility ray tests (matching nextgi_gather.glsl) instead of unfiltered 8x8 visibility texels.
#define NEXTGI_TLAS tlas
#define NEXTGI_WRC
#include "nextgi_rt_inc.glsl"

layout(set = 0, binding = 9, std430) restrict readonly buffer MarkRecords {
	NextGIInstanceRecord data[];
}
records;

layout(set = 0, binding = 10, std430) restrict readonly buffer MarkHitSurfaces {
	NextGIHitSurfaceMark data[];
}
hit_surfaces;

layout(set = 0, binding = 11, std430) restrict readonly buffer MarkHitMaterials {
	NextGIHitMaterialMark data[];
}
hit_materials;

layout(set = 0, binding = 13, std430) restrict readonly buffer MarkParams {
	mat4 inv_view_proj;
	mat4 view_proj;
	vec4 camera_pos_radius; // xyz camera pos, w rt_scene_radius
	vec4 screen_size_origin; // xy full screen size, z max_origin_offset, w max_trace_roughness
	vec4 ambient_color_sky; // rgb ambient color, a sky_energy (>0 when sky enabled)
	vec4 fog_params0; // x enabled, y depth_mode, z density, w height
	vec4 fog_params1; // x height_density, y depth_begin, z depth_end, w depth_curve
	vec4 fog_light_color; // rgb color, a energy
	vec4 view_to_world[3];
	uvec4 counts0; // x frame, y record_count, z material_count, w max_rays
	uvec4 counts1; // x reflect_enabled, y reflect_debug, z flags (b0 blue_noise, b1 front_layer, b2 has_translucent), w hiz_max_mip
}
mark_params;

layout(set = 0, binding = 14, std430) restrict buffer MarkHitRecords {
	NextGIHitLightRecord data[];
}
hit_records_out;

layout(set = 0, binding = 15, std430) restrict buffer MarkCounters {
	NextGIHitLightCounters data;
}
hl_counters;

layout(set = 0, binding = 16, std430) restrict buffer MarkReflectCounters {
	uint traced;
	uint hits;
	uint misses;
	uint pad;
}
reflect_counters;

layout(rgba16f, set = 0, binding = 17) restrict uniform image2D reflection_image;
layout(r16f, set = 0, binding = 18) restrict uniform image2D specular_hit_image;
layout(rgba16f, set = 0, binding = 19) restrict uniform image2D specular_debug_image;

#define NEXTGI_SCREEN_HIZ_TEX hiz_tex
#define NEXTGI_SCREEN_LAST_FRAME_TEX last_frame_tex
#include "nextgi_screen_trace_inc.glsl"

vec2 mark_blue_noise(ivec2 p_pixel, uint p_frame) {
	vec2 offset = fract(vec2(p_frame) * vec2(0.7548776662, 0.5698402910)) * 64.0;
	ivec2 coord = (p_pixel + ivec2(offset)) & ivec2(63);
	return texelFetch(blue_noise_tex, coord, 0).rg;
}

vec3 mark_miss_radiance(vec3 p_direction) {
	if (mark_params.ambient_color_sky.a > 0.0) {
		return textureLod(sky_tex, vec3_to_oct(p_direction), 0.0).rgb * mark_params.ambient_color_sky.a;
	}
	return mark_params.ambient_color_sky.rgb;
}

void mark_write_debug(ivec2 p_pixel, float p_hit_distance) {
	if (mark_params.counts1.y != 0u) {
		float scaled = p_hit_distance >= 65504.0 ? 1.0 : clamp(log2(1.0 + p_hit_distance) / 8.0, 0.0, 1.0);
		imageStore(specular_debug_image, p_pixel, vec4(vec3(scaled), 1.0));
	}
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(reflection_image);
	if (pixel.x >= size.x || pixel.y >= size.y) {
		return;
	}
	ivec2 full_size = ivec2(mark_params.screen_size_origin.xy);
	vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
	ivec2 full = ivec2(clamp(uv * vec2(full_size), vec2(0.0), vec2(full_size) - 1.0));
	float depth = texelFetch(depth_tex, full, 0).r;
	if (depth <= 0.0 || mark_params.counts1.x == 0u) {
		imageStore(reflection_image, pixel, vec4(0.0));
		imageStore(specular_hit_image, pixel, vec4(65504.0));
		mark_write_debug(pixel, 65504.0);
		return;
	}

	uint traced_prev = atomicAdd(reflect_counters.traced, 1u);
	if (traced_prev == 0xFFFFFFFFu) {
		hl_counters.data.sink = traced_prev;
	}

	vec4 guide = texelFetch(normal_roughness_tex, full, 0);
	float roughness = nextgi_reflect_decode_roughness(guide.a);
	mat3 view_to_world = mat3(mark_params.view_to_world[0].xyz, mark_params.view_to_world[1].xyz, mark_params.view_to_world[2].xyz);
	vec3 normal = nextgi_view_world_normal(view_to_world, guide.xyz);
	vec3 world = nextgi_view_reconstruct(mark_params.inv_view_proj, (vec2(full) + 0.5) / mark_params.screen_size_origin.xy, depth);
	vec3 view_dir = normalize(mark_params.camera_pos_radius.xyz - world);
	vec3 origin = world + normal * mark_params.screen_size_origin.z;

	float max_trace_roughness = mark_params.screen_size_origin.w;
	if (roughness > max_trace_roughness) {
		vec3 dir = reflect(-view_dir, normal);
		vec3 radiance = nextgi_wrc_sample(origin, dir);
		if (!nextgi_wrc_hit_cache) {
			radiance = mark_miss_radiance(dir);
		}
		// Resolved by NextGI (WRC or sky): publish 1.0 confidence so dark interiors never leak unoccluded sky cubemap.
		imageStore(reflection_image, pixel, vec4(radiance, 1.0));
		imageStore(specular_hit_image, pixel, vec4(65504.0));
		mark_write_debug(pixel, 65504.0);
		return;
	}

	uint hash = nextgi_reflect_hash(uint(uv.x * 65535.0) ^ (uint(uv.y * 65535.0) << 16u) ^ mark_params.counts0.x);
	vec2 seed = vec2(float(hash & 0xFFFFu), float(hash >> 16u)) / 65535.0;
	if ((mark_params.counts1.z & 1u) != 0u) {
		seed = mark_blue_noise(pixel, mark_params.counts0.x);
	}

	vec3 direction = roughness < 0.02 ? reflect(-view_dir, normal) : nextgi_reflect_sample_ggx(normal, view_dir, roughness, seed);
	if (dot(direction, normal) <= 0.0) {
		direction = reflect(-view_dir, normal);
	}

	float world_tmax = mark_params.camera_pos_radius.w;
	bool front_layer = (mark_params.counts1.z & 2u) != 0u;
	bool has_translucent = (mark_params.counts1.z & 4u) != 0u;
	bool mirror_eligible = roughness < NEXTGI_HL_MIRROR_ROUGHNESS;

	// Glossy rays (roughness >= 0.1) may take a fast Hi-Z screen hit unless a translucent pane sits in front;
	// mirrors (roughness < 0.1) always route through hit lighting for sharp material detail, shadows, and multi-bounce.
	vec3 screen_color = vec3(0.0);
	vec3 march_end = origin;
	bool screen_hit = false;
	if (!mirror_eligible && !front_layer) {
		screen_hit = nextgi_screen_trace_hiz(
				mark_params.view_proj,
				mark_params.inv_view_proj,
				mark_params.camera_pos_radius.xyz,
				mark_params.screen_size_origin.xy,
				float(mark_params.counts1.w),
				origin,
				direction,
				world_tmax,
				screen_color,
				march_end);
	}
	float traveled = length(march_end - origin);
	if (screen_hit && has_translucent && traveled > 0.005) {
		// Check whether an alpha-blended pane lies between origin and the screen hit.
		rayQueryEXT trans_rq;
		rayQueryInitializeEXT(trans_rq, tlas, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, NEXTGI_HL_MASK_TRANSLUCENT, origin, 0.0, direction, traveled);
		rayQueryProceedEXT(trans_rq);
		if (rayQueryGetIntersectionTypeEXT(trans_rq, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
			screen_hit = false;
			march_end = origin;
			traveled = 0.0;
		}
	}
	if (screen_hit) {
		vec3 radiance = nextgi_hl_reflection_fog(screen_color, origin + direction * traveled, traveled, mark_params.fog_params0, mark_params.fog_params1, mark_params.fog_light_color);
		imageStore(reflection_image, pixel, vec4(radiance, 1.0));
		imageStore(specular_hit_image, pixel, vec4(traveled));
		mark_write_debug(pixel, traveled);
		uint hit_prev = atomicAdd(reflect_counters.hits, 1u);
		if (hit_prev == 0xFFFFFFFFu) {
			hl_counters.data.sink = hit_prev;
		}
		return;
	}

	vec3 trace_origin = origin;
	float trace_tmax = world_tmax;
	float resume = 0.0;
	if (!mirror_eligible && !front_layer && !has_translucent && traveled > 0.0) {
		resume = nextgi_screen_resume(origin, direction, traveled, world_tmax, mark_params.screen_size_origin.z, trace_origin, trace_tmax);
	}

	uint cull_mask = has_translucent ? (NEXTGI_HL_MASK_GI | NEXTGI_HL_MASK_TRANSLUCENT) : NEXTGI_HL_MASK_GI;
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, cull_mask, trace_origin, 0.0, direction, trace_tmax);
	rayQueryProceedEXT(rq);

	if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
		vec3 radiance = nextgi_wrc_sample(trace_origin + direction * trace_tmax, direction);
		if (!nextgi_wrc_hit_cache) {
			radiance = mark_miss_radiance(direction);
		}
		// Traced miss resolved by WRC or sky: publish 1.0 confidence so 3x3 specular upsampling never leaks
		// unoccluded REFLECTION_SOURCE_SKY onto adjacent occluded interior pixels.
		imageStore(reflection_image, pixel, vec4(radiance, 1.0));
		imageStore(specular_hit_image, pixel, vec4(65504.0));
		mark_write_debug(pixel, 65504.0);
		uint miss_prev = atomicAdd(reflect_counters.misses, 1u);
		if (miss_prev == 0xFFFFFFFFu) {
			hl_counters.data.sink = miss_prev;
		}
		return;
	}

	uint custom_index = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true));
	uint geometry_index = uint(rayQueryGetIntersectionGeometryIndexEXT(rq, true));
	uint primitive_index = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true));
	vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rq, true);
	float hit_t = rayQueryGetIntersectionTEXT(rq, true);
	bool front_face = rayQueryGetIntersectionFrontFaceEXT(rq, true);

	uint material_key = NEXTGI_HL_KEY_REFLECTION_FALLBACK;
	bool is_translucent = false;
	if (custom_index < mark_params.counts0.y) {
		NextGIInstanceRecord inst = records.data[custom_index];
		is_translucent = (inst.rt_flags & NEXTGI_HL_RT_FLAG_TRANSLUCENT) != 0u;
		if (geometry_index < inst.surface_count) {
			uint surf_idx = inst.surface_base + geometry_index;
			NextGIHitSurfaceMark hs = hit_surfaces.data[surf_idx];
			if (hs.material_id < mark_params.counts0.z) {
				NextGIHitMaterialMark hm = hit_materials.data[hs.material_id];
				material_key = nextgi_hit_light_material_key(hm.shader_slot, hs.material_id);
			}
		}
	}

	uint hit_prev = atomicAdd(reflect_counters.hits, 1u);
	if (hit_prev == 0xFFFFFFFFu) {
		hl_counters.data.sink = hit_prev;
	}

	uint slot = atomicAdd(hl_counters.data.marked_hits, 1u);
	if (slot < mark_params.counts0.w) {
		uint flags = (front_face ? NEXTGI_HL_RECORD_FLAG_FRONT_FACE : 0u) |
				(is_translucent ? NEXTGI_HL_RECORD_FLAG_TRANSLUCENT : 0u) |
				(mirror_eligible ? NEXTGI_HL_RECORD_FLAG_MIRROR : 0u);
		float cam_dist = length(world - mark_params.camera_pos_radius.xyz);
		float cone_width = max(cam_dist / max(mark_params.screen_size_origin.y, 1.0), 1e-4);
		float cone_spread = max(roughness * 0.5, 1.0 / max(mark_params.screen_size_origin.y, 1.0));

		NextGIHitLightRecord rec;
		rec.origin_t = vec4(trace_origin, hit_t);
		rec.id_pixel = uvec4(
				(custom_index & 0xFFFFFFu) | flags,
				(geometry_index & 0xFFFFu) | ((material_key & 0xFFFFu) << 16u),
				primitive_index,
				(uint(pixel.x) & 0xFFFFu) | ((uint(pixel.y) & 0xFFFFu) << 16u));
		rec.packed_geom = uvec4(
				packHalf2x16(bary),
				packSnorm2x16(nextgi_hl_oct_encode(direction)),
				packHalf2x16(vec2(cone_width, cone_spread)),
				floatBitsToUint(resume));
		hit_records_out.data[slot] = rec;
	} else {
		if (slot == 0xFFFFFFFFu) {
			hl_counters.data.sink = slot;
		}
		// Fallback if record buffer capacity was exceeded.
		vec3 radiance = nextgi_wrc_sample(trace_origin + direction * hit_t, direction);
		if (!nextgi_wrc_hit_cache) {
			radiance = mark_miss_radiance(direction);
		}
		imageStore(reflection_image, pixel, vec4(radiance, 0.5));
		imageStore(specular_hit_image, pixel, vec4(resume + hit_t));
		mark_write_debug(pixel, resume + hit_t);
	}
}
