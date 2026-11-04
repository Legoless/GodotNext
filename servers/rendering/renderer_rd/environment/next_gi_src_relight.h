/**************************************************************************/
/*  next_gi_src_relight.h                                                 */
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

#pragma once

#include "core/math/math_funcs.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_scene.h"
#include "servers/rendering/rendering_server_enums.h"

namespace RendererRD {

// World-space SRC relight (#31, D14). The types and functions above NextGIRelight are device-free.

enum {
	NEXTGI_SRC_RELIGHT_TEXEL_CAP = 256 * 1024, // High. Medium and Epic are #55.
	NEXTGI_SRC_RELIGHT_RAY_CAP = 256 * 1024,
	NEXTGI_SRC_MAX_SHADOWED_LIGHTS = 8,
	NEXTGI_SRC_MASK_EVALS_PER_FRAME = 2,
	NEXTGI_SRC_RADIANCE_SIZE = 4096,
	NEXTGI_SRC_MASK_SIZE = 2048,
};

enum NextGILightFlags {
	NEXTGI_LIGHT_FLAG_SHADOW = 1u << 0,
	NEXTGI_LIGHT_FLAG_STATIC = 1u << 1,
	NEXTGI_LIGHT_REUSE_SHIFT = 8u,
	NEXTGI_LIGHT_REUSE_NONE = 0xFFFFFFu,
};

enum NextGIShadowReuseSource {
	NEXTGI_REUSE_DIRECTIONAL = 0u,
	NEXTGI_REUSE_POSITIONAL = 1u,
	NEXTGI_REUSE_NONE = 0xFFFFFFFFu,
};

// std430, 64 bytes. color_energy.w is the energy. position_inv_range.w is 1/range (0 for a directional).
// direction_type.xyz is the direction light travels; w is RSE::LightType. flags b0 has shadow, b1 static,
// b8-31 is the first NextGIShadowReuse (0xFFFFFF when the light has no reused shadow map).
struct NextGILight {
	float color_energy[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float position_inv_range[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float direction_type[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float attenuation = 0.0f;
	float cos_spot = 0.0f;
	float inv_spot_atten = 1.0f;
	uint32_t flags = NEXTGI_LIGHT_REUSE_NONE << NEXTGI_LIGHT_REUSE_SHIFT;
};
static_assert(sizeof(NextGILight) == 64, "NextGILight must stay 64 bytes (plan D14).");

// std430, 96 bytes. matrix is column-major, matching GLSL mat4. atlas is xy origin and zw size in UV.
struct NextGIShadowReuse {
	float matrix[16] = {};
	float atlas[4] = {};
	float bias = 0.0f;
	uint32_t source = NEXTGI_REUSE_NONE;
	uint32_t pad0 = 0;
	uint32_t pad1 = 0;
};
static_assert(sizeof(NextGIShadowReuse) == 96, "NextGIShadowReuse must stay 96 bytes.");

enum NextGIPositionalLightList {
	NEXTGI_POSITIONAL_LIGHT_NONE = 0,
	NEXTGI_POSITIONAL_LIGHT_DYNAMIC,
	NEXTGI_POSITIONAL_LIGHT_STATIC,
};

// Which scenario list a non-directional light belongs in. DISABLED and directionals belong in neither.
// Hidden lights and lights outside a scenario belong in neither, so leaving a scenario drops the light
// and entering one puts it back.
_FORCE_INLINE_ NextGIPositionalLightList nextgi_positional_light_list(bool p_visible, bool p_in_scenario, bool p_directional, RenderingServerEnums::LightBakeMode p_bake) {
	if (!p_visible || !p_in_scenario || p_directional) {
		return NEXTGI_POSITIONAL_LIGHT_NONE;
	}
	if (p_bake == RenderingServerEnums::LIGHT_BAKE_DYNAMIC) {
		return NEXTGI_POSITIONAL_LIGHT_DYNAMIC;
	}
	if (p_bake == RenderingServerEnums::LIGHT_BAKE_STATIC) {
		return NEXTGI_POSITIONAL_LIGHT_STATIC;
	}
	return NEXTGI_POSITIONAL_LIGHT_NONE;
}

// Erases the id from both lists, then inserts it into the one p_list names.
template <typename T, typename Id>
_FORCE_INLINE_ void nextgi_sync_positional_light(T &r_dynamic, T &r_static, const Id &p_id, NextGIPositionalLightList p_list) {
	r_dynamic.erase(p_id);
	r_static.erase(p_id);
	if (p_list == NEXTGI_POSITIONAL_LIGHT_DYNAMIC) {
		r_dynamic.push_back(p_id);
	} else if (p_list == NEXTGI_POSITIONAL_LIGHT_STATIC) {
		r_static.push_back(p_id);
	}
}

// STATIC lights are skipped for lightmapped receivers. DYNAMIC lights still reach them.
_FORCE_INLINE_ bool nextgi_light_reaches_receiver(bool p_is_static, uint32_t p_rt_flags) {
	return !(p_is_static && (p_rt_flags & NEXTGI_RT_FLAG_LIGHTMAPPED) != 0);
}

// Godot's omni falloff: nd^4 window times inverse-distance decay.
_FORCE_INLINE_ float nextgi_omni_attenuation(float p_distance, float p_inv_range, float p_decay) {
	float nd = p_distance * p_inv_range;
	nd *= nd;
	nd *= nd;
	nd = MAX(1.0f - nd, 0.0f);
	nd *= nd;
	return nd * Math::pow(MAX(p_distance, 0.0001f), -p_decay);
}

// p_facing is dot(direction from the light to the point, the spot direction). p_inv_spot_atten is 1/spot attenuation.
_FORCE_INLINE_ float nextgi_spot_attenuation(float p_distance, float p_inv_range, float p_decay, float p_facing, float p_cos_spot, float p_inv_spot_atten) {
	float atten = nextgi_omni_attenuation(p_distance, p_inv_range, p_decay);
	const float scos = MAX(p_facing, p_cos_spot);
	const float denom = MAX(1.0f - p_cos_spot, 1e-4f);
	const float spot_rim = MAX(1e-4f, (1.0f - scos) / denom);
	const float power = p_inv_spot_atten > 0.0f ? 1.0f / p_inv_spot_atten : 1.0f;
	atten *= 1.0f - Math::pow(spot_rim, power);
	return MAX(atten, 0.0f);
}

struct NextGIPageLightCandidate {
	uint32_t light_index = 0;
	float energy = 0.0f;
	bool has_shadow = false;
};

// Rank by energy, highest first. The first p_max_shadowed lights that cast shadows are the shadowed set.
// Every other light is unshadowed. Ties keep the lower light index first.
inline void nextgi_select_page_lights(const NextGIPageLightCandidate *p_lights, uint32_t p_count, uint32_t p_max_shadowed, LocalVector<uint32_t> &r_shadowed, LocalVector<uint32_t> &r_unshadowed) {
	r_shadowed.clear();
	r_unshadowed.clear();
	LocalVector<uint32_t> order;
	order.resize(p_count);
	for (uint32_t i = 0; i < p_count; i++) {
		order[i] = i;
	}
	for (uint32_t i = 1; i < p_count; i++) {
		const uint32_t key = order[i];
		uint32_t j = i;
		while (j > 0) {
			const NextGIPageLightCandidate &a = p_lights[order[j - 1]];
			const NextGIPageLightCandidate &b = p_lights[key];
			const bool out_of_order = a.energy < b.energy || (a.energy == b.energy && a.light_index > b.light_index);
			if (!out_of_order) {
				break;
			}
			order[j] = order[j - 1];
			j--;
		}
		order[j] = key;
	}
	for (uint32_t i = 0; i < p_count; i++) {
		const NextGIPageLightCandidate &light = p_lights[order[i]];
		if (light.has_shadow && r_shadowed.size() < p_max_shadowed) {
			r_shadowed.push_back(light.light_index);
		} else {
			r_unshadowed.push_back(light.light_index);
		}
	}
}

// The two mask slots updated this frame. Both are UINT32_MAX when the page has no shadowed lights.
inline void nextgi_mask_slots(uint32_t p_frame, uint32_t p_shadowed_count, uint32_t &r_slot_a, uint32_t &r_slot_b) {
	if (p_shadowed_count == 0) {
		r_slot_a = UINT32_MAX;
		r_slot_b = UINT32_MAX;
		return;
	}
	r_slot_a = (p_frame * NEXTGI_SRC_MASK_EVALS_PER_FRAME) % p_shadowed_count;
	r_slot_b = (p_frame * NEXTGI_SRC_MASK_EVALS_PER_FRAME + 1) % p_shadowed_count;
}

// Whole pages, round-robin, up to p_texel_cap. A page larger than the cap is taken by itself so it still relights.
// Returns how many indices were written. Advances p_cursor to the first page that did not fit, or wraps when all fit.
inline uint32_t nextgi_schedule_relight(const uint32_t *p_texels, uint32_t p_page_count, uint32_t &p_cursor, uint32_t p_texel_cap, uint32_t *r_indices, uint32_t p_index_capacity) {
	if (p_page_count == 0 || p_texels == nullptr || r_indices == nullptr || p_index_capacity == 0) {
		return 0;
	}
	if (p_cursor >= p_page_count) {
		p_cursor = 0;
	}
	uint32_t scheduled = 0;
	uint32_t used = 0;
	const uint32_t start = p_cursor;
	for (uint32_t n = 0; n < p_page_count && scheduled < p_index_capacity; n++) {
		const uint32_t index = (start + n) % p_page_count;
		const uint32_t texels = p_texels[index];
		if (texels == 0) {
			p_cursor = (index + 1) % p_page_count;
			continue;
		}
		if (used > 0 && used + texels > p_texel_cap) {
			p_cursor = index;
			break;
		}
		r_indices[scheduled++] = index;
		used += texels;
		p_cursor = (index + 1) % p_page_count;
		if (used >= p_texel_cap) {
			break;
		}
	}
	return scheduled;
}

// Medium, High, Epic texel and shadow-ray caps. Auto and unknown settings are High until #55.
inline uint32_t nextgi_src_budget_for_quality(int p_quality) {
	if (p_quality == 1) {
		return 128u * 1024u;
	}
	if (p_quality == 3) {
		return 512u * 1024u;
	}
	return 256u * 1024u;
}

inline uint32_t nextgi_src_page_ray_estimate(uint32_t p_texels, uint32_t p_shadowed_lights, float p_ray_fraction) {
	const uint32_t lights = MIN(p_shadowed_lights, 2u);
	const float fraction = CLAMP(p_ray_fraction, 0.0f, 1.0f);
	return uint32_t(float(p_texels / 4u) * float(lights) * fraction);
}

inline bool nextgi_src_scale_changed(float p_captured, float p_current) {
	if (p_captured <= 0.0f) {
		return false;
	}
	return Math::abs(p_current - p_captured) > 0.01f * p_captured;
}

inline AABB nextgi_src_event_bounds(const AABB &p_old, bool p_has_old, const AABB &p_new, bool p_has_new) {
	if (p_has_old && p_has_new) {
		return p_old.merge(p_new);
	}
	if (p_has_new) {
		return p_new;
	}
	return p_old;
}

// S ∪ (S − L·R). L is the direction the light travels.
inline AABB nextgi_src_directional_sweep(const AABB &p_swept, const Vector3 &p_travel, float p_radius) {
	Vector3 direction = p_travel;
	if (direction.length_squared() > 0.000001f) {
		direction = direction.normalized();
	}
	AABB shifted = p_swept;
	shifted.position -= direction * p_radius;
	return p_swept.merge(shifted);
}

inline bool nextgi_src_sphere_meets_aabb(const Vector3 &p_center, float p_radius, const AABB &p_aabb) {
	const Vector3 end = p_aabb.position + p_aabb.size;
	const Vector3 closest(
			CLAMP(p_center.x, p_aabb.position.x, end.x),
			CLAMP(p_center.y, p_aabb.position.y, end.y),
			CLAMP(p_center.z, p_aabb.position.z, end.z));
	return closest.distance_squared_to(p_center) <= p_radius * p_radius;
}

// The swept volume meets the light's range, and the page is inside that range and meets the volume extruded away from the light.
inline bool nextgi_src_local_light_dirties_page(const AABB &p_page, const AABB &p_swept, const Vector3 &p_light, float p_range) {
	if (p_range <= 0.0f || !nextgi_src_sphere_meets_aabb(p_light, p_range, p_swept) || !nextgi_src_sphere_meets_aabb(p_light, p_range, p_page)) {
		return false;
	}
	Vector3 away = p_page.get_center() - p_light;
	const float length = away.length();
	if (length <= 0.0001f) {
		return p_page.intersects(p_swept);
	}
	away /= length;
	AABB shifted = p_swept;
	shifted.position += away * MAX(p_range - length, 0.0f);
	return p_page.intersects(p_swept.merge(shifted));
}

struct NextGISrcSchedulePage {
	uint32_t texels = 0;
	uint32_t ray_estimate = 0;
	uint32_t last_relit_frame = 0;
	float priority = 0.0f;
	bool dirty_direct = false;
	bool near_dirty = false;
};

// Stream 2 (dirty, highest priority first), then stream 3 (near a dirty region, at most 1/16 of the pages), then the oldest pages. Stops at either cap.
uint32_t nextgi_src_schedule_streams(const NextGISrcSchedulePage *p_pages, uint32_t p_count, uint32_t p_texel_cap, uint32_t p_ray_cap, uint32_t *r_indices, uint32_t p_capacity);

} // namespace RendererRD
