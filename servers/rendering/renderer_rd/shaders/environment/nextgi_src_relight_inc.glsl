// GodotNext: ps_swrt_trace (#186). Not a standalone shader: the SRC world-space relight kernel
// (#31, D14, §2.1 line 2b), shared by the hardware relight (nextgi_src_relight.glsl) and the
// software sibling (nextgi_src_relight_swrt.glsl). The includer defines nextgi_shadow_ray()
// first; only the shadow ray, its origin offset and, on the software tier, the positional rays'
// aim from that offset origin differ between the tiers.

#ifndef NEXTGI_SRC_RELIGHT_INC_GLSL
#define NEXTGI_SRC_RELIGHT_INC_GLSL

#include "../light_function_inc.glsl"
#include "../area_lights_inc.glsl"
#include "../ies_inc.glsl"

#ifdef NEXTGI_RADIANCE_RGBA16F
layout(rgba16f, set = 0, binding = 1) uniform image2D radiance_image;
#else
layout(r11f_g11f_b10f, set = 0, binding = 1) uniform image2D radiance_image;
#endif
layout(r8ui, set = 0, binding = 2) uniform uimage2D mask_image;

layout(set = 0, binding = 3) uniform sampler2D albedo_tex;
layout(set = 0, binding = 4) uniform sampler2D normal_tex;
layout(set = 0, binding = 5) uniform sampler2D emission_tex;
layout(set = 0, binding = 6) uniform sampler2D depth_tex;
layout(set = 0, binding = 19) uniform sampler2D src_indirect_tex;
layout(set = 0, binding = 7) uniform sampler2DShadow directional_shadow_tex;
layout(set = 0, binding = 8) uniform sampler2DShadow positional_shadow_tex;

layout(set = 0, binding = 9, std430) restrict readonly buffer NextGICards {
	NextGICard data[];
}
cards;

layout(set = 0, binding = 10, std430) restrict readonly buffer NextGICardSets {
	NextGICardSet data[];
}
card_sets;

layout(set = 0, binding = 11, std430) restrict readonly buffer NextGIRecords {
	NextGIInstanceRecord data[];
}
records;

layout(set = 0, binding = 12, std430) restrict readonly buffer NextGILights {
	NextGILight data[];
}
lights;

struct NextGIShadowReuse {
	mat4 matrix;
	vec4 atlas;
	float bias;
	uint source;
	uint pad0;
	uint pad1;
	vec4 range_origin; // GodotNext: #258. xyz camera, w directional shadow distance (0: no range).
};

layout(set = 0, binding = 13, std430) restrict readonly buffer NextGIReuses {
	NextGIShadowReuse data[];
}
reuses;

struct NextGIRelightPage {
	uint record_index;
	uint card_index;
	uint radiance_origin;
	uint radiance_size;
	uint mask_origin;
	uint mask_size;
	uint light_offset;
	uint shadowed_count;
	uint light_count;
	uint stable_id;
	uint pad0;
	uint pad1;
};

layout(set = 0, binding = 14, std430) restrict readonly buffer NextGIPages {
	NextGIRelightPage data[];
}
pages;

layout(set = 0, binding = 15, std430) restrict readonly buffer NextGIPageLights {
	uint data[];
}
page_lights;

layout(set = 0, binding = 16, std430) buffer NextGIRelightCounters {
	uint traced;
	uint skipped;
	uint reserved;
	uint pad;
}
counters;

layout(set = 0, binding = 17, std430) buffer NextGIRelightProbes {
	vec4 data[];
}
probes;

layout(set = 0, binding = 18, std430) restrict readonly buffer NextGIRelightParams {
	uint frame;
	uint ray_budget;
	uint page_count;
	float rt_radius;
	uint page_offset;
	uint light_function_count; // GodotNext: light_function_atlas (#138)
}
params;

// GodotNext: light_function_atlas (#138, plan §21.3 contract M8).
layout(set = 0, binding = 29) uniform sampler2D light_function_atlas;
layout(set = 0, binding = 30, std430) restrict readonly buffer NextGILightFunctions {
	LightFunctionEntry data[];
}
light_function_data;
// GodotNext: light_source_parity (#140, plan §21.3 contract M8). Companion per-light parity record,
// area light texture atlas, and IES profile atlas (bound on both HWRT and SWRT).
struct NextGILightParity {
	vec4 axis_x_width;
	vec4 axis_y_height;
	vec4 area_tex_rect;
	uint ies_layer;
	float ies_scale;
	float barn_door_angle_rad;
	float barn_door_length;
};

layout(set = 0, binding = 31, std430) restrict readonly buffer NextGILightParities {
	NextGILightParity data[];
}
light_parities;

layout(set = 0, binding = 32) uniform texture2D area_light_atlas;
layout(set = 0, binding = 33) uniform texture2DArray ies_profile_atlas;
layout(set = 0, binding = 34) uniform sampler parity_linear_sampler;

const uint REUSE_NONE = 0xFFFFFFFFu;
const float PI = 3.14159265;

float nextgi_omni_attenuation(float distance, float inv_range, float decay) {
	float nd = distance * inv_range;
	nd *= nd;
	nd *= nd;
	nd = max(1.0 - nd, 0.0);
	nd *= nd;
	return nd * pow(max(distance, 0.0001), -decay);
}

vec3 nextgi_xform(NextGIInstanceRecord rec, vec3 p) {
	return vec3(
			dot(vec3(rec.object_to_world[0], rec.object_to_world[1], rec.object_to_world[2]), p) + rec.object_to_world[3],
			dot(vec3(rec.object_to_world[4], rec.object_to_world[5], rec.object_to_world[6]), p) + rec.object_to_world[7],
			dot(vec3(rec.object_to_world[8], rec.object_to_world[9], rec.object_to_world[10]), p) + rec.object_to_world[11]);
}

// GodotNext: nextgi_card_normal (#251). The rows were exact for rotation and uniform scale only;
// the shared inverse transpose also holds for non-uniform scale and mirroring.
vec3 nextgi_xform_n(NextGIInstanceRecord rec, vec3 n) {
	return nextgi_record_normal_to_world(rec, n);
}

// 1 lit, 0 occluded, -1 not inside a reused shadow map (the caller traces).
int nextgi_reuse_state(uint reuse_index, vec3 world) {
	for (uint j = 0u; j < 4u; j++) {
		NextGIShadowReuse entry = reuses.data[reuse_index + j];
		if (entry.source == REUSE_NONE) {
			break;
		}
		// GodotNext: nextgi_relight_shadow_range (#258). Beyond the directional shadow distance
		// the shadow pass culled the occluders, so the map cannot shadow this receiver: trace it.
		if (entry.range_origin.w > 0.0 && distance(world, entry.range_origin.xyz) > entry.range_origin.w) {
			continue;
		}
		vec4 clip = entry.matrix * vec4(world, 1.0);
		if (clip.w == 0.0) {
			continue;
		}
		clip.xyz /= clip.w;
		if (clip.x < entry.atlas.x || clip.y < entry.atlas.y || clip.x > entry.atlas.x + entry.atlas.z || clip.y > entry.atlas.y + entry.atlas.w || clip.z < 0.0 || clip.z > 1.0) {
			continue;
		}
		// Reverse-Z compare (GREATER): the receiver is lit when it is closer than the stored occluder.
		float ref_z = clamp(clip.z + entry.bias, 0.0, 1.0);
		float lit = entry.source == 0u ? texture(directional_shadow_tex, vec3(clip.xy, ref_z)) : texture(positional_shadow_tex, vec3(clip.xy, ref_z));
		return lit > 0.5 ? 1 : 0;
	}
	return -1;
}

void main() {
	uint slot = gl_WorkGroupID.z + params.page_offset;
	if (slot >= params.page_count) {
		return;
	}
	NextGIRelightPage page = pages.data[slot];
	uvec2 size = uvec2(page.radiance_size & 0xFFFFu, page.radiance_size >> 16u);
	uvec2 texel = gl_GlobalInvocationID.xy;
	if (texel.x >= size.x || texel.y >= size.y) {
		return;
	}
	uvec2 radiance_origin = uvec2(page.radiance_origin & 0xFFFFu, page.radiance_origin >> 16u);
	ivec2 radiance_coord = ivec2(radiance_origin + texel);

	NextGICard card = cards.data[page.card_index];
	vec2 card_size = max(vec2(card.atlas_rect.zw), vec2(1.0));
	ivec2 material = ivec2(card.atlas_rect.xy) + ivec2(clamp(((vec2(texel) + 0.5) / vec2(size)) * card_size, vec2(0.0), card_size - 1.0));
	float depth = texelFetch(depth_tex, material, 0).r;
	if (depth <= 0.0) {
		imageStore(radiance_image, radiance_coord, vec4(0.0));
		return;
	}

	NextGIInstanceRecord rec = records.data[page.record_index];
	NextGICardSet card_set = card_sets.data[rec.card_set];
	float xu = ((float(texel.x) + 0.5) / float(size.x) * 2.0 - 1.0) * card.axis_u.w;
	float yv = ((float(texel.y) + 0.5) / float(size.y) * 2.0 - 1.0) * card.axis_v.w;
	vec3 object_p = card.origin.xyz + card.axis_u.xyz * xu + card.axis_v.xyz * yv + card.axis_n.xyz * (-depth * card_set.capture_scale);
	vec3 world = nextgi_xform(rec, object_p);

	vec3 nview = normalize(texelFetch(normal_tex, material, 0).rgb * 2.0 - 1.0);
	vec3 nobj = normalize(card.axis_u.xyz * nview.x + card.axis_v.xyz * nview.y + card.axis_n.xyz * nview.z);
	vec3 normal = normalize(nextgi_xform_n(rec, nobj));

	uvec2 mask_size = uvec2(page.mask_size & 0xFFFFu, page.mask_size >> 16u);
	uvec2 mask_texel = min(texel / 2u, mask_size - 1u);
	uvec2 mask_origin = uvec2(page.mask_origin & 0xFFFFu, page.mask_origin >> 16u);
	ivec2 mask_coord = ivec2(mask_origin + mask_texel);
	uint mask = imageLoad(mask_image, mask_coord).r;
	bool owner = (texel.x & 1u) == 0u && (texel.y & 1u) == 0u && mask_size.x > 0u && mask_size.y > 0u;

	uint slots[2];
	uint slot_count = 0u;
	if (page.shadowed_count > 0u) {
		slots[0] = (params.frame * 2u) % page.shadowed_count;
		slot_count = 1u;
		uint second = (params.frame * 2u + 1u) % page.shadowed_count;
		if (second != slots[0]) {
			slots[1] = second;
			slot_count = 2u;
		}
	}

	if (owner) {
#ifdef NEXTGI_SHADOW_RETRACE_BINDING
		// GodotNext: nextgi_shadow_policy (#145). A mask texel appends at most one retrace record per frame, for the
		// first of its lights that needs one. Which light of the pair comes first flips on every visit of the pair
		// (next_gi_shadow_policy.h, nextgi_shadow_retrace_turn()): the pairs repeat every n / 2 frames for an even n
		// and every n frames for an odd n, so neither light waits for good.
		uint turn_period = (page.shadowed_count & 1u) != 0u ? page.shadowed_count : page.shadowed_count / 2u;
		uint slot_turn = nextgi_shadow_retrace_enabled() ? ((params.frame / max(turn_period, 1u)) & 1u) : 0u;
		bool retrace_recorded = false;
#else
		uint slot_turn = 0u;
#endif
		for (uint s = 0u; s < slot_count; s++) {
			uint light_slot = slots[(s + slot_turn) % slot_count];
			NextGILight light = lights.data[page_lights.data[page.light_offset + light_slot]];
			uint reuse = light.flags >> 8u;
			int reused = reuse != 0xFFFFFFu ? nextgi_reuse_state(reuse, world) : -1;
			bool lit = true;
			if (reused >= 0) {
				lit = reused == 1;
			} else {
				vec3 to_light;
				float tmax;
				uint light_type = uint(light.direction_type.w + 0.5);
				if (light_type == 0u) {
					to_light = -light.direction_type.xyz;
					tmax = params.rt_radius;
				} else {
					to_light = light.position_inv_range.xyz - world;
					tmax = length(to_light);
					if (tmax > 0.0) {
						to_light /= tmax;
					}
				}
				uint prev = atomicAdd(counters.reserved, 1u);
				if (prev < params.ray_budget) {
					atomicAdd(counters.traced, 1u);
#ifdef NEXTGI_TRACE_SDF
					// GodotNext: ps_swrt_trace (#186 step 6). 1.5 voxels of the level that holds the texel.
					vec3 origin = swrt_offset_origin(world, normal);
					// That offset grows with the level (0.15 m at level 0, 0.6 m at level 2), so a
					// positional light is aimed at from the offset origin: a ray parallel to the texel's
					// would end up to the offset beyond the light, inside the ceiling or wall the light
					// is mounted on.
					if (light_type != 0u) {
						to_light = light.position_inv_range.xyz - origin;
						tmax = length(to_light);
						if (tmax > 0.0) {
							to_light /= tmax;
						}
					}
					// #186 step 5: a positional light's ray marches the mesh SDFs of the instances
					// inside that light's range (nextgi_src_relight_swrt.glsl).
					lit = !nextgi_shadow_ray_to_light(origin, to_light, tmax, light_type, page_lights.data[page.light_offset + light_slot]);
#else
					vec3 origin = world + normal * rec.origin_offset;
					if (light_type != 0u) {
						to_light = light.position_inv_range.xyz - origin;
						tmax = length(to_light);
						if (tmax > 0.0) {
							to_light /= tmax;
						}
					}
					// GodotNext: nextgi_shadow_policy (#145). The light's caster mask and the alpha-tested casters apply.
					uint light_index = page_lights.data[page.light_offset + light_slot];
					lit = !nextgi_shadow_ray(origin, to_light, tmax, light_index);
#ifdef NEXTGI_SHADOW_RETRACE_BINDING
					if (nextgi_shadow_retrace) {
						// Only casters whose alpha comes from shader code stood in the way: the RT-pipeline retrace
						// decides, and the resolve pass writes the bit after it. Until then the bit keeps its last
						// answer, also when the texel already has a record this frame or the records are full. The
						// record's ray counts against the ray budget (R2) like any other.
						lit = (mask & (1u << light_slot)) != 0u;
						if (!retrace_recorded && atomicAdd(counters.reserved, 1u) < params.ray_budget) {
							retrace_recorded = nextgi_shadow_retrace_append(origin, to_light, tmax, light_shadows.data[light_index].x, uint(mask_coord.x) | (uint(mask_coord.y) << 16u), light_slot);
							if (retrace_recorded) {
								atomicAdd(counters.traced, 1u);
							}
						}
					}
#endif
#endif
				} else {
					atomicAdd(counters.skipped, 1u);
					lit = (mask & (1u << light_slot)) != 0u;
				}
			}
			if (lit) {
				mask |= 1u << light_slot;
			} else {
				mask &= ~(1u << light_slot);
			}
		}
		imageStore(mask_image, mask_coord, uvec4(mask, 0u, 0u, 0u));
	}

	vec3 albedo = min(texelFetch(albedo_tex, material, 0).rgb, vec3(0.95));
	vec3 emission = texelFetch(emission_tex, material, 0).rgb;
	vec3 direct = vec3(0.0);
	for (uint i = 0u; i < page.light_count; i++) {
		uint light_idx = page_lights.data[page.light_offset + i];
		NextGILight light = lights.data[light_idx];
		NextGILightParity parity = light_parities.data[light_idx];
		uint light_type = uint(light.direction_type.w + 0.5);
		float shadow = 1.0;
		if (i < page.shadowed_count) {
			shadow = (mask & (1u << i)) != 0u ? 1.0 : 0.0;
		}
		if (light_type == 3u) {
			// GodotNext: light_source_parity (#140). AreaLight3D: LTC + barn doors + area_texture + IES.
			vec3 rel = world - light.position_inv_range.xyz;
			if (dot(light.direction_type.xyz, rel) <= 0.0) {
				continue;
			}
			vec2 half_size = vec2(parity.axis_x_width.w, parity.axis_y_height.w) * 0.5;
			if (half_size.x <= 1e-5 || half_size.y <= 1e-5) {
				continue;
			}
			vec3 ax = parity.axis_x_width.xyz;
			vec3 ay = parity.axis_y_height.xyz;
			vec3 pos_local = vec3(dot(rel, ax), dot(rel, ay), dot(rel, -light.direction_type.xyz));
			vec2 clip_min;
			vec2 clip_max;
			if (!barn_door_clip_rect(pos_local, half_size, parity.barn_door_angle_rad, parity.barn_door_length, clip_min, clip_max)) {
				continue;
			}
			float ies = 1.0;
			if (parity.ies_layer != IES_LAYER_NONE) {
				ies = ies_sample_atlas(parity.ies_layer, parity.ies_scale, normalize(pos_local), ies_profile_atlas, parity_linear_sampler);
				if (ies <= 0.0) {
					continue;
				}
			}
			vec3 closest_local = vec3(clamp(pos_local.x, clip_min.x, clip_max.x), clamp(pos_local.y, clip_min.y, clip_max.y), 0.0);
			float dist = length(closest_local - pos_local);
			float atten_raw = nextgi_omni_attenuation(dist, light.position_inv_range.w, light.attenuation);
			float atten_ltc = atten_raw * dist * dist;
			vec3 rel_center = light.position_inv_range.xyz - world;
			vec3 p_min_x = ax * clip_min.x;
			vec3 p_max_x = ax * clip_max.x;
			vec3 p_min_y = ay * clip_min.y;
			vec3 p_max_y = ay * clip_max.y;
			vec3 points[4];
			points[0] = rel_center + p_min_x + p_min_y;
			points[1] = rel_center + p_max_x + p_min_y;
			points[2] = rel_center + p_max_x + p_max_y;
			points[3] = rel_center + p_min_x + p_max_y;
			vec4 clip_tex_rect = barn_door_clip_texture_rect(parity.area_tex_rect, half_size, clip_min, clip_max);
			float max_mipmap = 0.0;
			if (clip_tex_rect != vec4(0.0)) {
				vec2 atlas_size = vec2(textureSize(sampler2D(area_light_atlas, parity_linear_sampler), 0));
				vec2 tex_px = max(ceil(parity.area_tex_rect.zw * atlas_size), vec2(1.0));
				float atlas_mips = float(textureQueryLevels(sampler2D(area_light_atlas, parity_linear_sampler)));
				max_mipmap = max(min(floor(log2(min(tex_px.x, tex_px.y))), atlas_mips) - 1.0, 0.0);
			}
			float ltc_integral = 0.0;
			vec3 area_tex_color = vec3(1.0);
			ltc_evaluate_diff(normal, points, clip_tex_rect, max_mipmap, area_light_atlas, parity_linear_sampler, ltc_integral, area_tex_color);
			vec3 area_contrib = light.color_energy.rgb * light.color_energy.a * area_tex_color * (ltc_integral * 0.5) * atten_ltc * ies * shadow;
			if (params.light_function_count > 0u && light_idx < params.light_function_count) {
				area_contrib *= light_function_mask_combined(light_function_data.data[light_idx], light_function_atlas, world);
			}
			direct += area_contrib;
			continue;
		}
		vec3 to_light;
		float atten = 1.0;
		if (light_type == 0u) {
			to_light = -light.direction_type.xyz;
		} else {
			vec3 delta = light.position_inv_range.xyz - world;
			float dist = length(delta);
			to_light = dist > 0.0 ? delta / dist : vec3(0.0, 1.0, 0.0);
			atten = nextgi_omni_attenuation(dist, light.position_inv_range.w, light.attenuation);
			if (light_type == 2u) {
				float facing = dot(-to_light, light.direction_type.xyz);
				float scos = max(facing, light.cos_spot);
				float denom = max(1.0 - light.cos_spot, 1e-4);
				float spot_rim = max(1e-4, (1.0 - scos) / denom);
				float power = light.inv_spot_atten > 0.0 ? 1.0 / light.inv_spot_atten : 1.0;
				atten *= 1.0 - pow(spot_rim, power);
			}
			// GodotNext: light_source_parity (#140). IES profile attenuation in light local space.
			if (parity.ies_layer != IES_LAYER_NONE && atten > 0.0) {
				vec3 dir_ls = vec3(dot(-to_light, parity.axis_x_width.xyz), dot(-to_light, parity.axis_y_height.xyz), dot(-to_light, -light.direction_type.xyz));
				atten *= ies_sample_atlas(parity.ies_layer, parity.ies_scale, dir_ls, ies_profile_atlas, parity_linear_sampler);
			}
		}
		float ndotl = max(dot(normal, to_light), 0.0);
		vec3 light_contrib = light.color_energy.rgb * light.color_energy.a * atten * ndotl * shadow;
		if (params.light_function_count > 0u && light_idx < params.light_function_count) {
			light_contrib *= light_function_mask_combined(light_function_data.data[light_idx], light_function_atlas, world);
		}
		direct += light_contrib;
	}
	vec3 indirect = texelFetch(src_indirect_tex, radiance_coord / 8, 0).rgb;
	vec3 radiance = emission + (albedo / PI) * (direct + indirect);
	imageStore(radiance_image, radiance_coord, vec4(radiance, 1.0));

	uvec2 center = (size / 2u) & ~1u;
	if (texel == center) {
		probes.data[page.stable_id] = vec4(radiance, 1.0);
	}
}

#endif // NEXTGI_SRC_RELIGHT_INC_GLSL
