/**************************************************************************/
/*  nextgi_hit_lighting.glsl                                              */
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

#[raygen]

#version 460
#extension GL_EXT_ray_query : require

#VERSION_DEFINES

#if defined(NEXTGI_HIT_SER_EXT)
#extension GL_EXT_shader_invocation_reorder : require
#elif defined(NEXTGI_HIT_SER_NV)
#extension GL_NV_shader_invocation_reorder : require
#endif

// GodotNext: nextgi_hit_lighting (#129, plan §21.2 contract L7). Raygen stage 2 of the NextGI hit pipeline
// (NEXTGI_HIT_RAYGEN_HIT_LIGHTING): traces sorted reflection hits into #130 material hit groups (with SER
// reorder hint by 16-bit material key when supported), skips up to 2 alpha-blended translucent segments
// accumulating tinted transmittance, shades hits with Godot's PBR BRDF over the 64^3 world light grid and
// up to 4 inline ray-query area shadows (#145 trace_shadow), adds SRC/WRC indirect light, and traces
// multi-bounce mirror reflections (roughness < 0.1) up to max_reflection_bounces.

#include "../oct_inc.glsl"
#include "nextgi_hit_layout_inc.glsl"

layout(set = 2, binding = 0, std430) restrict readonly buffer HitMaterialParams {
	uint data[];
}
hit_material_params[];

// Keeps set 2's parameter table a run-time array for pipeline library layout matching (#130).
uint nextgi_hit_params_probe(uint p_slot) {
	return hit_material_params[nonuniformEXT(p_slot)].data[0];
}

layout(location = 0) rayPayloadEXT NextGIHitPayload payload;

// Reuse #145's shared shadow visibility policy (inline ray query + bindless texture alpha).
#define NEXTGI_SHADOW_TLAS hit_tlas
#define NEXTGI_SHADOW_SET 0
#define NEXTGI_SHADOW_INSTANCE_BINDING 43
#define NEXTGI_SHADOW_ALPHA_BINDING 44
#define NEXTGI_SHADOW_ALPHA_TEXTURES
#define NEXTGI_SHADOW_USE_HIT_LAYOUT
#include "nextgi_shadow_ray_inc.glsl"

vec2 hl_blue_noise(ivec2 p_pixel, uint p_frame) {
	vec2 offset = fract(vec2(p_frame) * vec2(0.7548776662, 0.5698402910)) * 64.0;
	ivec2 coord = (p_pixel + ivec2(offset)) & ivec2(63);
	return texelFetch(hit_light_blue_noise, coord, 0).rg;
}

vec2 hl_wrc_oct_uv(vec3 p_dir) {
	vec3 d = normalize(p_dir);
	float sum = abs(d.x) + abs(d.y) + abs(d.z);
	vec2 oct = d.xy / max(sum, 0.0001);
	if (d.z < 0.0) {
		oct = (vec2(1.0) - abs(oct.yx)) * vec2(oct.x >= 0.0 ? 1.0 : -1.0, oct.y >= 0.0 ? 1.0 : -1.0);
	}
	return oct * 0.5 + 0.5;
}

vec3 hl_wrc_sample(vec3 p_world, vec3 p_dir, out bool r_hit_cache) {
	r_hit_cache = false;
	if (hit_light_params.wrc_camera.w < 0.5) {
		return vec3(0.0);
	}
	float dist = length(p_world - hit_light_params.wrc_camera.xyz);
	float spacing = 1.0;
	uint level = 0u;
	if (dist > 8.0) {
		spacing = 2.0;
		level = 1u;
	}
	if (dist > 16.0) {
		spacing = 4.0;
		level = 2u;
	}
	if (dist > 32.0) {
		spacing = 8.0;
		level = 3u;
	}
	vec3 level_origin = floor(hit_light_params.wrc_origin_spacing.xyz / spacing) * spacing;
	vec3 local = (p_world - level_origin) / spacing + vec3(7.5);
	vec3 base = floor(local);
	vec3 frac = local - base;
	vec2 oct = hl_wrc_oct_uv(p_dir);
	vec3 accum = vec3(0.0);
	float weight_sum = 0.0;
	for (uint i = 0u; i < 8u; i++) {
		ivec3 offset = ivec3(i & 1u, (i >> 1u) & 1u, (i >> 2u) & 1u);
		ivec3 cell = ivec3(base) + offset;
		if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, ivec3(16)))) {
			continue;
		}
		uint probe = uint(cell.x) + uint(cell.y) * 16u + uint(cell.z) * 256u + level * 4096u;
		uvec2 tile = uvec2(probe % 128u, probe / 128u);
		vec2 uv = (vec2(tile) * 10.0 + vec2(1.0) + oct * 8.0 + vec2(0.5)) / vec2(1280.0);
		vec2 vis_uv = (vec2(tile) * 8.0 + oct * 7.0 + vec2(0.5)) / vec2(1024.0);
		vec3 rad = textureLod(sampler2D(hit_light_wrc_radiance, SAMPLER_LINEAR_CLAMP), uv, 0.0).rgb;
		vec2 moments = textureLod(sampler2D(hit_light_wrc_visibility, SAMPLER_LINEAR_CLAMP), vis_uv, 0.0).rg;
		float variance = max(moments.g - moments.r * moments.r, 0.0);
		float chebyshev = variance / (variance + 0.05);
		vec3 trilinear = mix(vec3(1.0) - frac, frac, vec3(offset));
		float weight = trilinear.x * trilinear.y * trilinear.z * max(chebyshev, 0.05);
		accum += rad * weight;
		weight_sum += weight;
	}
	if (weight_sum <= 0.0) {
		return vec3(0.0);
	}
	r_hit_cache = true;
	return accum / weight_sum;
}

vec3 hl_miss_radiance(vec3 p_end_pos, vec3 p_dir) {
	bool wrc_ok = false;
	vec3 wrc = hl_wrc_sample(p_end_pos, p_dir, wrc_ok);
	if (wrc_ok) {
		return wrc;
	}
	if (hit_light_params.ambient_color_sky.a > 0.5) {
		return textureLod(sampler2D(hit_light_sky, SAMPLER_LINEAR_CLAMP), vec3_to_oct(p_dir), 0.0).rgb;
	}
	return hit_light_params.ambient_color_sky.rgb;
}

// D6 SRC indirect irradiance lookup at a hit point. Returns true when a valid covered SRC card texel is found.
bool hl_src_indirect_lookup(uint p_custom_index, vec3 p_world_pos, vec3 p_world_normal, out vec3 r_irradiance) {
	r_irradiance = vec3(0.0);
	if ((hit_light_params.counts1.w & NEXTGI_HL_PARAM_FLAG_SRC_INDIRECT) == 0u || p_custom_index >= hit_params.record_count) {
		return false;
	}
	NextGIInstanceRecord rec = hit_records.data[p_custom_index];
	if (rec.card_set == 0xFFFFFFFFu || rec.page_table == 0xFFFFFFFFu || (rec.page_table & 0x80000000u) != 0u) {
		return false;
	}
	mat3 basis = mat3(
			vec3(rec.object_to_world[0], rec.object_to_world[4], rec.object_to_world[8]),
			vec3(rec.object_to_world[1], rec.object_to_world[5], rec.object_to_world[9]),
			vec3(rec.object_to_world[2], rec.object_to_world[6], rec.object_to_world[10]));
	float det = determinant(basis);
	if (abs(det) < 1e-8) {
		return false;
	}
	vec3 origin = vec3(rec.object_to_world[3], rec.object_to_world[7], rec.object_to_world[11]);
	vec3 object_pos = inverse(basis) * (p_world_pos - origin);
	vec3 object_normal = normalize(transpose(basis) * p_world_normal);

	NextGICardSet card_set = hit_light_card_sets.data[rec.card_set];
	uint count = min(card_set.card_count, 24u);
	float best_align = -2.0;
	vec3 best_irr = vec3(0.0);
	bool found = false;
	for (uint i = 0u; i < count; i++) {
		NextGICard card = hit_check_cards.data[card_set.first_card + i];
		vec3 rel = object_pos - card.origin.xyz;
		float u = dot(rel, card.axis_u.xyz);
		float v = dot(rel, card.axis_v.xyz);
		if (abs(u) > card.axis_u.w || abs(v) > card.axis_v.w) {
			continue;
		}
		float depth = -dot(rel, card.axis_n.xyz);
		float align = dot(object_normal, card.axis_n.xyz);
		vec2 card_size = max(vec2(card.atlas_rect.zw), vec2(1.0));
		vec2 texel = vec2((u / max(card.axis_u.w, 1e-5)) * 0.5 + 0.5, (v / max(card.axis_v.w, 1e-5)) * 0.5 + 0.5) * card_size;
		ivec2 mat_coord = ivec2(card.atlas_rect.xy) + ivec2(clamp(texel, vec2(0.0), card_size - 1.0));
		float card_depth = texelFetch(hit_check_depth, mat_coord, 0).r;
		if (!(card_depth > 0.0 && abs(card_depth - depth) <= 2.0 * max(card_set.texel_size, 0.02))) {
			continue;
		}
		uint page_index = rec.page_table + i;
		if (page_index >= hit_light_params.counts1.z) {
			continue;
		}
		uvec2 page = hit_light_page_rects.data[page_index];
		uvec2 page_origin = uvec2(page.x & 0xFFFFu, page.x >> 16u);
		uvec2 page_size = max(uvec2(page.y & 0xFFFFu, page.y >> 16u), uvec2(1u));
		ivec2 rad_coord = ivec2(page_origin) + ivec2(clamp(texel / card_size * vec2(page_size), vec2(0.0), vec2(page_size) - 1.0));
		if (align > best_align) {
			best_align = align;
			best_irr = texelFetch(hit_light_src_indirect, rad_coord / 8, 0).rgb;
			found = true;
		}
	}
	if (found) {
		r_irradiance = best_irr;
		return true;
	}
	return false;
}

// Primary hit re-trace using SER when supported, reordering by hit object + 16-bit material key.
void hl_trace_primary(vec3 p_origin, vec3 p_dir, float p_tmax, uint p_cull_mask, uint p_material_key, vec2 p_cone) {
	payload.shading = uvec4(0u);
	payload.hit = uvec4(floatBitsToUint(p_cone.x), floatBitsToUint(p_cone.y), 0u, 0u);
#if defined(NEXTGI_HIT_SER_EXT)
	hitObjectEXT ho;
	hitObjectTraceRayEXT(ho, hit_tlas, gl_RayFlagsOpaqueEXT, p_cull_mask, 0u, 1u, 0u, p_origin, 0.0, p_dir, p_tmax, 0);
	reorderThreadEXT(ho, p_material_key, 16u);
	hitObjectExecuteShaderEXT(ho, 0);
#elif defined(NEXTGI_HIT_SER_NV)
	hitObjectNV ho;
	hitObjectTraceRayNV(ho, hit_tlas, gl_RayFlagsOpaqueEXT, p_cull_mask, 0u, 1u, 0u, p_origin, 0.0, p_dir, p_tmax, 0);
	reorderThreadNV(ho, p_material_key, 16u);
	hitObjectExecuteShaderNV(ho, 0);
#else
	traceRayEXT(hit_tlas, gl_RayFlagsOpaqueEXT, p_cull_mask, 0u, 1u, 0u, p_origin, 0.0, p_dir, p_tmax, 0);
#endif
}

// Secondary trace (translucent skip continuation or multi-bounce mirror reflection).
void hl_trace_secondary(vec3 p_origin, vec3 p_dir, float p_tmax, uint p_cull_mask, vec2 p_cone) {
	payload.shading = uvec4(0u);
	payload.hit = uvec4(floatBitsToUint(p_cone.x), floatBitsToUint(p_cone.y), 0u, 0u);
	traceRayEXT(hit_tlas, gl_RayFlagsOpaqueEXT, p_cull_mask, 0u, 1u, 0u, p_origin, 0.0, p_dir, p_tmax, 0);
}

// Shades a hit payload (direct lighting from 64^3 world light grid + up to 4 area shadows + SRC/WRC indirect + emission).
vec3 hl_shade_surface(
		vec3 p_hit_pos,
		vec3 p_view_dir,
		vec3 p_albedo,
		vec3 p_normal,
		float p_roughness,
		float p_metallic,
		float p_ao,
		vec3 p_emission,
		uint p_custom_index,
		float p_hit_eps,
		ivec2 p_pixel,
		uint p_bounce,
		bool p_add_specular_fallback) {
	vec3 direct = vec3(0.0);
	uint cell = nextgi_light_grid_world_to_cell(p_hit_pos, hit_light_params.grid_min_inv_cell.xyz, hit_light_params.grid_min_inv_cell.w);
	uint light_count_total = hit_light_params.counts0.y;
	if (cell != 0xFFFFFFFFu && light_count_total > 0u) {
		uint cell_base = cell * NEXTGI_LIGHT_GRID_CELL_WORDS;
		uint cell_count = min(hit_light_grid.data[cell_base], NEXTGI_LIGHT_GRID_CELL_CAP);
		uint shadowed_used = 0u;
		uint unshadowed_extra = 0u;
		uint traced_shadows = 0u;
		vec3 shadow_origin = p_hit_pos + p_normal * max(p_hit_eps, 0.01);

		for (uint i = 0u; i < cell_count; i++) {
			uint packed = hit_light_grid.data[cell_base + 1u + (i >> 1u)];
			uint light_idx = ((i & 1u) == 0u) ? (packed & 0xFFFFu) : (packed >> 16u);
			if (light_idx >= light_count_total) {
				continue;
			}
			NextGIHitLightGPU light = hit_light_lights.data[light_idx];
			uint ltype = uint(light.direction_type.w + 0.5);
			vec3 to_light;
			float dist = hit_light_params.camera_pos_radius.w;
			float atten = 1.0;
			bool is_dir = (ltype == 0u);
			if (is_dir) {
				to_light = -light.direction_type.xyz;
			} else {
				vec3 delta = light.position_inv_range.xyz - p_hit_pos;
				dist = length(delta);
				if (!(dist > 1e-4)) {
					continue;
				}
				to_light = delta / dist;
				if (ltype == 2u) {
					atten = nextgi_hl_spot_attenuation(dist, light.position_inv_range.w, light.spot_atten_radius.x, dot(-to_light, light.direction_type.xyz), light.spot_atten_radius.y, light.spot_atten_radius.z);
				} else {
					atten = nextgi_hl_omni_attenuation(dist, light.position_inv_range.w, light.spot_atten_radius.x);
				}
			}
			float n_dot_l = dot(p_normal, to_light);
			if (!(n_dot_l > 0.0) || !(atten > 0.0)) {
				continue;
			}

			float vis = 1.0;
			if ((light.shadow_flags.x & NEXTGI_HL_LIGHT_FLAG_SHADOW) != 0u) {
				if (shadowed_used < NEXTGI_HL_MAX_SHADOWED_LIGHTS) {
					vec2 bn = hl_blue_noise(p_pixel, hit_light_params.counts0.w + shadowed_used * 17u + p_bounce * 53u);
					vec3 shadow_dir;
					float shadow_tmax;
					nextgi_hl_area_shadow_ray(to_light, max(dist - max(p_hit_eps, 0.01), 0.0), light.spot_atten_radius.w, is_dir, bn, shadow_dir, shadow_tmax);
					vis = trace_shadow(shadow_origin, shadow_dir, shadow_tmax, light.shadow_flags.y, light.shadow_flags.z);
					shadowed_used++;
					traced_shadows++;
				} else {
					unshadowed_extra++;
				}
			}
			if (vis > 0.0) {
				vec3 brdf = nextgi_hl_eval_brdf(p_albedo, p_metallic, p_roughness, p_normal, p_view_dir, to_light);
				direct += brdf * (light.color_energy.rgb * light.color_energy.a * atten * n_dot_l * vis);
			}
		}

		if (traced_shadows > 0u) {
			uint prev = atomicAdd(hit_light_counters.data.shadow_rays, traced_shadows);
			if (prev == 0xFFFFFFFFu) {
				hit_light_counters.data.sink = prev;
			}
		}
		if (unshadowed_extra > 0u) {
			uint prev = atomicAdd(hit_light_counters.data.unshadowed_lights, unshadowed_extra);
			if (prev == 0xFFFFFFFFu) {
				hit_light_counters.data.sink = prev;
			}
		}
	}

	// Indirect light at the hit from SRC indirect texture (D6) with WRC / ambient fallback.
	vec3 diffuse_color = p_albedo * (1.0 - p_metallic);
	vec3 indirect = vec3(0.0);
	vec3 src_irr;
	if (hl_src_indirect_lookup(p_custom_index, p_hit_pos, p_normal, src_irr) && max(src_irr.r, max(src_irr.g, src_irr.b)) > 0.0) {
		indirect = (diffuse_color * (1.0 / NEXTGI_HL_PI)) * src_irr * p_ao;
	} else {
		bool wrc_ok = false;
		vec3 wrc_rad = hl_wrc_sample(p_hit_pos, p_normal, wrc_ok);
		if (wrc_ok) {
			indirect = diffuse_color * wrc_rad * p_ao;
		} else {
			indirect = diffuse_color * hit_light_params.ambient_color_sky.rgb * p_ao;
		}
	}

	// On a mirror's final bounce (or single-bounce mirror hit), add rough/WRC specular fallback so metals stay lit.
	if (p_add_specular_fallback) {
		vec3 refl = reflect(-p_view_dir, p_normal);
		vec3 env_spec = hl_miss_radiance(p_hit_pos, refl);
		vec3 f0 = mix(vec3(0.04), p_albedo, p_metallic);
		float n_dot_v = max(dot(p_normal, p_view_dir), 1e-4);
		vec3 fresnel = f0 + (1.0 - f0) * pow(clamp(1.0 - n_dot_v, 0.0, 1.0), 5.0);
		indirect += fresnel * env_spec * (1.0 - p_roughness) * p_ao;
	}

	return p_emission + direct + indirect;
}

void main() {
	uint launch_idx = gl_LaunchIDEXT.x;
	uint active_count = min(hit_light_counters.data.marked_hits, hit_light_params.counts0.x);
	if (launch_idx >= active_count) {
		return;
	}

	uint rec_idx = hit_light_sorted_indices.data[launch_idx];
	NextGIHitLightRecord rec = hit_light_records.data[rec_idx];

	ivec2 pixel = ivec2(rec.id_pixel.w & 0xFFFFu, rec.id_pixel.w >> 16u);
	if (pixel.x >= int(hit_light_params.counts1.x) || pixel.y >= int(hit_light_params.counts1.y)) {
		return;
	}

	vec3 cur_origin = rec.origin_t.xyz;
	float mark_t = rec.origin_t.w;
	uint inst_idx = rec.id_pixel.x & 0xFFFFFFu;
	uint material_key = rec.id_pixel.y >> 16u;
	bool first_translucent = (rec.id_pixel.x & NEXTGI_HL_RECORD_FLAG_TRANSLUCENT) != 0u;
	vec3 ray_dir = nextgi_hl_oct_decode(unpackSnorm2x16(rec.packed_geom.y));
	vec2 cone = unpackHalf2x16(rec.packed_geom.z);
	float resume_dist = uintBitsToFloat(rec.packed_geom.w);
	bool has_translucent = (hit_light_params.counts1.w & NEXTGI_HL_PARAM_FLAG_TRANSLUCENT) != 0u;
	uint max_bounces = clamp(hit_light_params.counts0.z, 1u, 4u);
	float world_tmax = hit_light_params.camera_pos_radius.w;

	float mark_eps = inst_idx < hit_params.record_count ? hit_records.data[inst_idx].origin_offset : 0.005;
	float primary_tmax = mark_t + max(2.0 * mark_eps, 0.01);
	uint primary_mask = first_translucent ? (NEXTGI_HL_MASK_GI | NEXTGI_HL_MASK_TRANSLUCENT) : NEXTGI_HL_MASK_GI;

	vec3 accum_radiance = vec3(0.0);
	vec3 throughput = vec3(1.0);
	float first_hit_distance = resume_dist + mark_t;
	vec3 first_hit_world = cur_origin + ray_dir * mark_t;
	bool got_any_hit = false;

	for (uint bounce = 0u; bounce < max_bounces; bounce++) {
		uint translucent_skips = 0u;
		bool segment_done = false;
		bool bounce_continue = false;
		vec3 next_origin = cur_origin;
		vec3 next_dir = ray_dir;
		float seg_remaining = world_tmax;

		for (uint seg = 0u; seg <= NEXTGI_HL_MAX_TRANSLUCENT_SKIPS; seg++) {
			if (bounce == 0u && seg == 0u) {
				hl_trace_primary(cur_origin, ray_dir, primary_tmax, primary_mask, material_key, cone);
			} else {
				uint mask = (has_translucent && translucent_skips < NEXTGI_HL_MAX_TRANSLUCENT_SKIPS)
						? (NEXTGI_HL_MASK_GI | NEXTGI_HL_MASK_TRANSLUCENT)
						: NEXTGI_HL_MASK_GI;
				hl_trace_secondary(cur_origin, ray_dir, seg_remaining, mask, cone);
			}

			uint pflags = payload.shading.z >> 24u;
			if ((pflags & NEXTGI_HIT_PAYLOAD_HIT) == 0u) {
				accum_radiance += throughput * hl_miss_radiance(cur_origin + ray_dir * seg_remaining, ray_dir);
				if (!got_any_hit) {
					first_hit_distance = 65504.0;
				}
				segment_done = true;
				break;
			}

			float hit_t = uintBitsToFloat(payload.hit.x);
			uint hit_custom = payload.hit.w;
			vec3 hit_pos = cur_origin + ray_dir * hit_t;
			float hit_eps = hit_custom < hit_params.record_count ? hit_records.data[hit_custom].origin_offset : 0.005;

			vec4 albedo_alpha = unpackUnorm4x8(payload.shading.x);
			vec3 albedo = albedo_alpha.rgb;
			float opacity = albedo_alpha.a;
			vec3 normal = nextgi_hit_oct_decode(unpackSnorm2x16(payload.shading.y));
			if (dot(normal, -ray_dir) < 0.0) {
				normal = -normal;
			}
			float roughness = float(payload.shading.z & 0xFFu) / 255.0;
			float metallic = float((payload.shading.z >> 8u) & 0xFFu) / 255.0;
			float ao = float((payload.shading.z >> 16u) & 0xFFu) / 255.0;
			vec3 emission = nextgi_hit_unpack_r11g11b10(payload.shading.w);

			bool hit_is_translucent = hit_custom < hit_params.record_count &&
					(hit_records.data[hit_custom].rt_flags & NEXTGI_HL_RT_FLAG_TRANSLUCENT) != 0u;

			if (hit_is_translucent && translucent_skips < NEXTGI_HL_MAX_TRANSLUCENT_SKIPS) {
				vec3 layer_rad = hl_shade_surface(hit_pos, -ray_dir, albedo, normal, roughness, metallic, ao, emission, hit_custom, hit_eps, pixel, bounce, false);
				accum_radiance += throughput * layer_rad * clamp(opacity, 0.0, 1.0);
				throughput *= nextgi_hl_translucent_transmittance(albedo, opacity);
				translucent_skips++;
				uint skip_prev = atomicAdd(hit_light_counters.data.translucent_skips, 1u);
				if (skip_prev == 0xFFFFFFFFu) {
					hit_light_counters.data.sink = skip_prev;
				}
				if (max(throughput.r, max(throughput.g, throughput.b)) < 1e-3) {
					if (!got_any_hit) {
						got_any_hit = true;
						first_hit_distance = resume_dist + hit_t;
						first_hit_world = hit_pos;
					}
					segment_done = true;
					break;
				}
				float step_bias = max(hit_eps, 0.005);
				cur_origin = hit_pos + ray_dir * step_bias;
				seg_remaining = max(seg_remaining - hit_t - step_bias, 0.0);
				cone.x += cone.y * hit_t;
				if (!(seg_remaining > 1e-3)) {
					accum_radiance += throughput * hl_miss_radiance(cur_origin, ray_dir);
					segment_done = true;
					break;
				}
				continue;
			}

			if (!got_any_hit) {
				got_any_hit = true;
				if (bounce == 0u) {
					first_hit_distance = resume_dist + length(hit_pos - rec.origin_t.xyz);
					first_hit_world = hit_pos;
				}
			}

			bool will_bounce = (bounce + 1u < max_bounces) && (roughness < NEXTGI_HL_MIRROR_ROUGHNESS);
			vec3 surf_rad = hl_shade_surface(hit_pos, -ray_dir, albedo, normal, roughness, metallic, ao, emission, hit_custom, hit_eps, pixel, bounce, !will_bounce && roughness < 0.25);
			accum_radiance += throughput * surf_rad;

			if (will_bounce) {
				vec3 f0 = mix(vec3(0.04), albedo, metallic);
				float n_dot_v = max(dot(normal, -ray_dir), 1e-4);
				vec3 fresnel = f0 + (1.0 - f0) * pow(clamp(1.0 - n_dot_v, 0.0, 1.0), 5.0);
				throughput *= fresnel;
				if (max(throughput.r, max(throughput.g, throughput.b)) >= 1e-3) {
					next_dir = reflect(ray_dir, normal);
					next_origin = hit_pos + normal * max(hit_eps, 0.01);
					cone.x += cone.y * hit_t;
					bounce_continue = true;
					uint b_prev = atomicAdd(hit_light_counters.data.bounce_rays, 1u);
					if (b_prev == 0xFFFFFFFFu) {
						hit_light_counters.data.sink = b_prev;
					}
				}
			}
			segment_done = true;
			break;
		}

		if (!bounce_continue || !segment_done) {
			break;
		}
		cur_origin = next_origin;
		ray_dir = next_dir;
	}

	vec3 final_radiance = nextgi_hl_reflection_fog(
			accum_radiance,
			first_hit_world,
			first_hit_distance,
			hit_light_params.fog_params0,
			hit_light_params.fog_params1,
			hit_light_params.fog_light_color);
	float confidence = got_any_hit ? 1.0 : 0.0;
	imageStore(hit_light_reflection_out, pixel, vec4(final_radiance, confidence));
	imageStore(hit_light_hit_distance_out, pixel, vec4(first_hit_distance));
	if ((hit_light_params.counts1.w & NEXTGI_HL_PARAM_FLAG_REFLECT_DEBUG) != 0u) {
		float scaled = first_hit_distance >= 65504.0 ? 1.0 : clamp(log2(1.0 + first_hit_distance) / 8.0, 0.0, 1.0);
		imageStore(hit_light_debug_out, pixel, vec4(vec3(scaled), 1.0));
	}
}
