/**************************************************************************/
/*  nextgi_rt_shadows.glsl                                                */
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

#VERSION_DEFINES

#extension GL_EXT_ray_query : require

// GodotNext: rt_shadows (#141, plan §21.3 contract M9). Standalone ray-traced shadow trace pass.
// One thread per pixel of each allocated 16x16 shadow tile (8x8 workgroups, 0 shared memory).
// Traces 1 blue-noise-rotated ray per pixel toward the light's disk/sphere/rectangle via trace_shadow()
// from #145 (20-layer caster masks, alpha-tested casters, translucent-as-opaque LQ6, #130 material retrace).
// Ray origin offset obeys §8.2 (4 * eps_rt * scale, clamped <= 5 cm).

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT nextgi_tlas;

#define NEXTGI_SHADOW_TLAS nextgi_tlas
#define NEXTGI_SHADOW_SET 0
#define NEXTGI_SHADOW_INSTANCE_BINDING 1
#define NEXTGI_SHADOW_ALPHA_BINDING 2
#ifdef NEXTGI_SHADOW_ALPHA_TEXTURES
#define NEXTGI_SHADOW_CHUNK0_BINDING 12
#define NEXTGI_SHADOW_CHUNK1_BINDING 13
#define NEXTGI_SHADOW_CHUNK2_BINDING 14
#define NEXTGI_SHADOW_CHUNK3_BINDING 15
#define NEXTGI_SHADOW_SAMPLER_BINDING 16
#define NEXTGI_SHADOW_RETRACE_BINDING 17
#define NEXTGI_SHADOW_TEXTURE_SET 1
#endif
#include "nextgi_shadow_ray_inc.glsl"
#include "nextgi_rt_shadows_inc.glsl"
#include "nextgi_view_inc.glsl"

layout(set = 0, binding = 3, std430) restrict readonly buffer RTShadowParamsBuf {
	RTShadowParams data;
}
params_buf;

layout(set = 0, binding = 4, std430) restrict readonly buffer RTShadowLightsBuf {
	RTShadowLight data[];
}
lights_buf;

layout(set = 0, binding = 5, std430) restrict readonly buffer RTShadowTileList {
	uvec4 data[];
}
tile_list;

layout(set = 0, binding = 6, std430) restrict readonly buffer RTShadowAllocState {
	uint tile_count;
	uint active_tiles;
	uint overflow_tiles;
	uint overflow_lights;
	uint sink;
	uint pad0;
	uint pad1;
	uint pad2;
}
alloc_state;

layout(set = 0, binding = 7) uniform sampler2D depth_tex;
layout(set = 0, binding = 8) uniform sampler2D normal_roughness_tex;
layout(set = 0, binding = 9) uniform sampler2D blue_noise_tex;

layout(r8, set = 0, binding = 10) uniform restrict writeonly image2D raw_mask_image;
layout(r16f, set = 0, binding = 11) uniform restrict writeonly image2D hit_dist_image;

// #54 blue-noise tile rotated temporally by the R2 sequence and decorrelated per light.
vec2 rt_shadow_blue_noise(uvec2 p_pixel, uint p_frame, uint p_light_seed) {
	float idx = float((p_frame + p_light_seed * 17u) & 1023u);
	vec2 offset = fract(vec2(idx) * vec2(0.7548776662, 0.5698402910)) * 64.0;
	ivec2 coord = (ivec2(p_pixel) + ivec2(offset)) & ivec2(63);
	return texelFetch(blue_noise_tex, coord, 0).rg;
}

vec2 rt_shadow_disk(vec2 p_u) {
	float r = sqrt(clamp(p_u.x, 0.0, 1.0));
	float theta = 6.28318530718 * p_u.y;
	return vec2(cos(theta) * r, sin(theta) * r);
}

void main() {
	uint tile_id = gl_WorkGroupID.z * 256u + (gl_WorkGroupID.y >> 1u);
	if (tile_id >= alloc_state.active_tiles) {
		return;
	}

	uvec2 local_px = uvec2(gl_WorkGroupID.x * 8u + gl_LocalInvocationID.x, (gl_WorkGroupID.y & 1u) * 8u + gl_LocalInvocationID.y);
	ivec2 atlas_px = rt_shadow_atlas_coord(tile_id, local_px);

	RTShadowParams params = params_buf.data;
	uvec4 tile = tile_list.data[tile_id];
	uint light_idx = tile.x;
	uvec2 st = uvec2(tile.y & 0xFFFFu, tile.y >> 16u);
	uvec2 screen_px = st * RT_SHADOW_TILE_SIZE + local_px;

	if (screen_px.x >= params.screen_tiles.x || screen_px.y >= params.screen_tiles.y) {
		imageStore(raw_mask_image, atlas_px, vec4(1.0, 0.0, 0.0, 0.0));
		imageStore(hit_dist_image, atlas_px, vec4(RT_SHADOW_HIT_MISS, 0.0, 0.0, 0.0));
		return;
	}

	float depth = texelFetch(depth_tex, ivec2(screen_px), 0).r;
	if (nextgi_view_is_background(depth)) {
		imageStore(raw_mask_image, atlas_px, vec4(1.0, 0.0, 0.0, 0.0));
		imageStore(hit_dist_image, atlas_px, vec4(RT_SHADOW_HIT_MISS, 0.0, 0.0, 0.0));
		return;
	}

	vec2 uv = (vec2(screen_px) + 0.5) / vec2(params.screen_tiles.xy);
	vec3 world = nextgi_view_reconstruct(params.inv_view_proj, uv, depth);
	mat3 view_to_world = mat3(params.view_to_world[0].xyz, params.view_to_world[1].xyz, params.view_to_world[2].xyz);
	vec3 world_normal = nextgi_view_world_normal(view_to_world, texelFetch(normal_roughness_tex, ivec2(screen_px), 0).rgb);

	RTShadowLight light = lights_buf.data[light_idx];
	uint ltype = light.rect_id.w & 3u;
	uint light_seed = light.rect_id.w >> 8u;
	vec2 u = rt_shadow_blue_noise(screen_px, params.counts.z, light_seed);

	float linear_z = max(nextgi_view_linear_depth(params.depth_coefficients, depth), 1e-4);
	// §8.2 ray origin offset: 4 * eps_rt * scale, clamped <= 5 cm.
	float max_offset = min(params.camera_pos_offset.w, 0.05);
	float origin_offset = clamp(0.004 + linear_z * 0.002, 0.004, max(max_offset, 0.004));

	vec3 center_to_light;
	float center_dist;
	if (ltype == RT_SHADOW_TYPE_DIRECTIONAL) {
		center_to_light = normalize(light.dir_cos.xyz);
		center_dist = max(light.pos_range.w, params.footprint_misc.z);
	} else {
		vec3 diff = light.pos_range.xyz - world;
		center_dist = length(diff);
		if (ltype != RT_SHADOW_TYPE_AREA && center_dist > light.pos_range.w) {
			imageStore(raw_mask_image, atlas_px, vec4(1.0, 0.0, 0.0, 0.0));
			imageStore(hit_dist_image, atlas_px, vec4(RT_SHADOW_HIT_MISS, 0.0, 0.0, 0.0));
			return;
		}
		if (ltype == RT_SHADOW_TYPE_AREA && dot(light.dir_cos.xyz, world - light.pos_range.xyz) <= 0.0) {
			imageStore(raw_mask_image, atlas_px, vec4(1.0, 0.0, 0.0, 0.0));
			imageStore(hit_dist_image, atlas_px, vec4(RT_SHADOW_HIT_MISS, 0.0, 0.0, 0.0));
			return;
		}
		center_to_light = center_dist > 1e-5 ? (diff / center_dist) : vec3(0.0, 1.0, 0.0);
		if (ltype == RT_SHADOW_TYPE_SPOT && dot(-center_to_light, light.dir_cos.xyz) < light.dir_cos.w - 0.02) {
			imageStore(raw_mask_image, atlas_px, vec4(1.0, 0.0, 0.0, 0.0));
			imageStore(hit_dist_image, atlas_px, vec4(RT_SHADOW_HIT_MISS, 0.0, 0.0, 0.0));
			return;
		}
	}

	float ndotl = dot(world_normal, center_to_light);
	bool backface_transmittance = false;
	vec3 origin;
	if (ndotl <= -0.1) {
		if (light.area_height_trans.w > 0.0) {
			// Back-facing surface with LIGHT_PARAM_TRANSMITTANCE_BIAS: trace inward to measure thickness.
			origin = world - world_normal * min(light.area_height_trans.w, 0.25);
			backface_transmittance = true;
		} else {
			imageStore(raw_mask_image, atlas_px, vec4(0.0, 0.0, 0.0, 0.0));
			imageStore(hit_dist_image, atlas_px, vec4(0.0, 0.0, 0.0, 0.0));
			return;
		}
	} else {
		origin = world + world_normal * origin_offset;
	}

	vec3 ray_dir = center_to_light;
	float tmax = center_dist;
	float source_radius = light.area_width_size.w;

	if (ltype == RT_SHADOW_TYPE_DIRECTIONAL) {
		if (source_radius > 0.0) {
			vec3 t_axis;
			vec3 b_axis;
			rt_shadow_build_basis(center_to_light, t_axis, b_axis);
			vec2 disk = rt_shadow_disk(u) * source_radius;
			ray_dir = normalize(center_to_light + t_axis * disk.x + b_axis * disk.y);
		}
		tmax = max(light.pos_range.w, params.footprint_misc.z);
	} else if (ltype == RT_SHADOW_TYPE_AREA) {
		vec3 sample_pos = light.pos_range.xyz + light.area_width_size.xyz * (u.x - 0.5) + light.area_height_trans.xyz * (u.y - 0.5);
		vec3 to_sample = sample_pos - origin;
		float d = length(to_sample);
		if (d <= 1e-4) {
			imageStore(raw_mask_image, atlas_px, vec4(1.0, 0.0, 0.0, 0.0));
			imageStore(hit_dist_image, atlas_px, vec4(RT_SHADOW_HIT_MISS, 0.0, 0.0, 0.0));
			return;
		}
		ray_dir = to_sample / d;
		tmax = max(d - 0.005, 0.0);
	} else {
		vec3 to_center = light.pos_range.xyz - origin;
		float d_center = length(to_center);
		if (d_center <= 1e-4) {
			imageStore(raw_mask_image, atlas_px, vec4(1.0, 0.0, 0.0, 0.0));
			imageStore(hit_dist_image, atlas_px, vec4(RT_SHADOW_HIT_MISS, 0.0, 0.0, 0.0));
			return;
		}
		vec3 dir_center = to_center / d_center;
		if (source_radius > 0.0) {
			vec3 t_axis;
			vec3 b_axis;
			rt_shadow_build_basis(dir_center, t_axis, b_axis);
			vec2 disk = rt_shadow_disk(u) * min(source_radius, d_center * 0.95);
			vec3 sample_pos = light.pos_range.xyz + t_axis * disk.x + b_axis * disk.y;
			vec3 to_sample = sample_pos - origin;
			float d = length(to_sample);
			ray_dir = d > 1e-4 ? (to_sample / d) : dir_center;
			tmax = max(d - 0.005, 0.0);
		} else {
			ray_dir = dir_center;
			tmax = max(d_center - 0.005, 0.0);
		}
	}

	uint light_mode = light.policy_lookup.x;
	uint caster_mask = light.policy_lookup.y;
	float vis = trace_shadow(origin, ray_dir, tmax, light_mode, caster_mask);
	float hit_dist = nextgi_shadow_hit_t >= 0.0 ? min(nextgi_shadow_hit_t, RT_SHADOW_HIT_MISS) : RT_SHADOW_HIT_MISS;

#ifdef NEXTGI_SHADOW_RETRACE_BINDING
	if (nextgi_shadow_retrace && !backface_transmittance) {
		uint packed_coord = uint(atlas_px.x) | (uint(atlas_px.y) << 16u);
		if (!nextgi_shadow_retrace_append(origin, ray_dir, tmax, light_mode, packed_coord, tile_id)) {
			// Retrace buffer full: shadow as opaque (plan §21.3 M4).
			vis = 0.0;
			hit_dist = min(tmax * 0.5, 2.0);
		}
	}
#endif

	if (backface_transmittance) {
		vis = 0.0;
	}

	imageStore(raw_mask_image, atlas_px, vec4(vis, 0.0, 0.0, 0.0));
	imageStore(hit_dist_image, atlas_px, vec4(hit_dist, 0.0, 0.0, 0.0));
}
