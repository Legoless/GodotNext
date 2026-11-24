/**************************************************************************/
/*  light_source_parity.h                                                 */
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

// GodotNext: light_source_parity (#140, plan §21.3 contract M8, GM-11).
// Pure CPU constants and helpers for IES profile atlas layer allocation/recycling,
// barn-door rect clipping (matching area_lights_inc.glsl / ies_inc.glsl and #139 culling),
// and per-light contact shadow parameter resolution and world-space depth conversion.

#include "core/math/math_funcs.h"
#include "core/math/rect2.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/light_grid_math.h"
#include "servers/rendering/rendering_server_enums.h"

#include <cmath>
#include <cstdint>

namespace RendererRD {

namespace LightSourceParity {

static constexpr uint32_t IES_TABLE_WIDTH = 256u;
static constexpr uint32_t IES_TABLE_HEIGHT = 64u;
static constexpr uint32_t IES_MAX_ATLAS_LAYERS = 256u;
static constexpr uint32_t IES_LAYER_NONE = 0xFFFFFFFFu;

static constexpr uint32_t MAX_SSCS_LIGHTS_PER_VIEW = 32u;
static constexpr float DEFAULT_CONTACT_SHADOW_LENGTH = 0.0f;
static constexpr float DEFAULT_CONTACT_SHADOW_CASTING_INTENSITY = 1.0f;
static constexpr float DEFAULT_CONTACT_SHADOW_NON_CASTING_INTENSITY = 0.0f;

// Project-level ScreenSpaceContactShadowsLength enum mapped to screen-height fraction
// (roughly matching upstream READ_COUNT 2/3/4 = 63/127/191 px at 1080p).
static constexpr float CONTACT_SHADOW_PROJECT_LENGTH_SHORT = 0.06f;
static constexpr float CONTACT_SHADOW_PROJECT_LENGTH_MEDIUM = 0.12f;
static constexpr float CONTACT_SHADOW_PROJECT_LENGTH_LONG = 0.18f;

static constexpr uint32_t CONTACT_SHADOW_FLAG_WORLD_SPACE = 1u << 0u;

// Ref-counted fixed-capacity (up to 256 layers) layer allocator for the IES Texture2DArray atlas.
// Layers are assigned lazily when referenced by at least one light and recycled as soon as the
// last referencing light detaches or frees the profile.
class IESAtlasAllocator {
	struct Slot {
		uint32_t layer = IES_LAYER_NONE;
		uint32_t ref_count = 0;
	};

	uint32_t capacity = IES_MAX_ATLAS_LAYERS;
	HashMap<uint64_t, Slot> key_to_slot;
	LocalVector<uint32_t> free_layers;
	uint32_t next_layer = 0;
	uint32_t active_layers = 0;

public:
	explicit IESAtlasAllocator(uint32_t p_capacity = IES_MAX_ATLAS_LAYERS) :
			capacity(MIN(p_capacity, IES_MAX_ATLAS_LAYERS)) {}

	uint32_t get_capacity() const { return capacity; }
	uint32_t get_active_layer_count() const { return active_layers; }

	// Increments the ref count for p_key, allocating a layer if needed.
	// Sets r_newly_allocated to true if this call assigned a fresh layer (requiring GPU upload).
	// Returns IES_LAYER_NONE if the atlas is full.
	uint32_t acquire(uint64_t p_key, bool *r_newly_allocated = nullptr) {
		if (r_newly_allocated != nullptr) {
			*r_newly_allocated = false;
		}
		Slot *existing = key_to_slot.getptr(p_key);
		if (existing != nullptr) {
			existing->ref_count++;
			return existing->layer;
		}
		uint32_t layer = IES_LAYER_NONE;
		if (!free_layers.is_empty()) {
			layer = free_layers[free_layers.size() - 1];
			free_layers.resize(free_layers.size() - 1);
		} else if (next_layer < capacity) {
			layer = next_layer++;
		} else {
			return IES_LAYER_NONE;
		}
		Slot slot;
		slot.layer = layer;
		slot.ref_count = 1;
		key_to_slot.insert(p_key, slot);
		active_layers++;
		if (r_newly_allocated != nullptr) {
			*r_newly_allocated = true;
		}
		return layer;
	}

	// Decrements the ref count for p_key; returns true if the layer was recycled.
	bool release(uint64_t p_key) {
		Slot *existing = key_to_slot.getptr(p_key);
		if (existing == nullptr) {
			return false;
		}
		if (existing->ref_count > 1) {
			existing->ref_count--;
			return false;
		}
		const uint32_t recycled = existing->layer;
		key_to_slot.erase(p_key);
		free_layers.push_back(recycled);
		if (active_layers > 0) {
			active_layers--;
		}
		return true;
	}

	// Force-removes a key regardless of ref count (used on profile destruction).
	bool evict(uint64_t p_key) {
		Slot *existing = key_to_slot.getptr(p_key);
		if (existing == nullptr) {
			return false;
		}
		const uint32_t recycled = existing->layer;
		key_to_slot.erase(p_key);
		free_layers.push_back(recycled);
		if (active_layers > 0) {
			active_layers--;
		}
		return true;
	}

	uint32_t get_layer(uint64_t p_key) const {
		const Slot *existing = key_to_slot.getptr(p_key);
		return existing != nullptr ? existing->layer : IES_LAYER_NONE;
	}

	uint32_t get_ref_count(uint64_t p_key) const {
		const Slot *existing = key_to_slot.getptr(p_key);
		return existing != nullptr ? existing->ref_count : 0u;
	}

	void clear() {
		key_to_slot.clear();
		free_layers.clear();
		next_layer = 0;
		active_layers = 0;
	}
};

// Converts a unit direction in light local space (forward = -Z, right = +X, up = +Y)
// into exact half-texel-centered UV coordinates for the 256 x 64 IES table.
_FORCE_INLINE_ Vector2 ies_direction_to_uv(const Vector3 &p_dir_ls) {
	const float len = float(p_dir_ls.length());
	if (len <= 1e-8f) {
		return Vector2(0.5f / float(IES_TABLE_WIDTH), 0.5f / float(IES_TABLE_HEIGHT));
	}
	const Vector3 d = p_dir_ls / len;
	const float cos_theta = CLAMP(float(-d.z), -1.0f, 1.0f);
	const float theta = std::acos(cos_theta);
	float phi = std::atan2(float(d.y), float(d.x));
	if (phi < 0.0f) {
		phi += float(Math::TAU);
	}
	const float u = (theta / float(Math::PI)) * (float(IES_TABLE_WIDTH - 1u) / float(IES_TABLE_WIDTH)) + (0.5f / float(IES_TABLE_WIDTH));
	const float v = (phi / float(Math::TAU)) * (float(IES_TABLE_HEIGHT - 1u) / float(IES_TABLE_HEIGHT)) + (0.5f / float(IES_TABLE_HEIGHT));
	return Vector2(u, v);
}

// Clips an area light's emitter rectangle [-half_size, +half_size] by the two barn-door plane pairs
// as seen from shading point p_local = (x, y, z) in light local space where forward emission is -Z
// (so p_local.z < 0 is in front of the emitter; forward depth f = -p_local.z > 0).
// Matches barn_door_clip_rect() in ies_inc.glsl and the culling frustum in light_grid_math.h (#139).
// Returns false if the emitter is completely occluded by the barn doors.
_FORCE_INLINE_ bool barn_door_clip_rect(const Vector3 &p_local, const Vector2 &p_half_size, float p_angle_rad, float p_length, Vector2 &r_min, Vector2 &r_max) {
	r_min = -p_half_size;
	r_max = p_half_size;

	const float f = float(-p_local.z);
	if (f <= 0.0f) {
		return false;
	}
	if (!LightGrid::is_barn_door_active_rad(p_angle_rad, p_length)) {
		return true;
	}

	const float clamped_angle = CLAMP(p_angle_rad, 0.0f, float(Math::PI * 0.5) - 1e-4f);
	const float sin_a = std::sin(clamped_angle);
	const float cos_a = std::cos(clamped_angle);
	const float z_d = p_length * cos_a;
	if (z_d <= 1e-6f) {
		return true;
	}

	if (f <= z_d) {
		// Inside the door-length slab: point is either inside the convex shroud (full rect visible)
		// or outside behind a flap (completely occluded).
		const float tan_a = sin_a / cos_a;
		const float lim_x = float(p_half_size.x) + f * tan_a;
		const float lim_y = float(p_half_size.y) + f * tan_a;
		return std::fabs(float(p_local.x)) <= lim_x && std::fabs(float(p_local.y)) <= lim_y;
	}

	const float inv_dz = 1.0f / (f - z_d);
	const float x_d = float(p_half_size.x) + p_length * sin_a;
	const float y_d = float(p_half_size.y) + p_length * sin_a;

	const float x_min = MAX(float(-p_half_size.x), (-f * x_d - z_d * float(p_local.x)) * inv_dz);
	const float x_max = MIN(float(p_half_size.x), (f * x_d - z_d * float(p_local.x)) * inv_dz);
	if (x_min >= x_max) {
		return false;
	}

	const float y_min = MAX(float(-p_half_size.y), (-f * y_d - z_d * float(p_local.y)) * inv_dz);
	const float y_max = MIN(float(p_half_size.y), (f * y_d - z_d * float(p_local.y)) * inv_dz);
	if (y_min >= y_max) {
		return false;
	}

	r_min = Vector2(x_min, y_min);
	r_max = Vector2(x_max, y_max);
	return true;
}

// Adjusts an area light's atlas texture rect to correspond to a barn-door clipped sub-rectangle.
// Matches fetch_ltc_lod's (1.0 - uv) convention in area_lights_inc.glsl.
_FORCE_INLINE_ Rect2 barn_door_clip_texture_rect(const Rect2 &p_tex_rect, const Vector2 &p_half_size, const Vector2 &p_clip_min, const Vector2 &p_clip_max) {
	if (p_tex_rect == Rect2() || p_half_size.x <= 1e-6f || p_half_size.y <= 1e-6f) {
		return p_tex_rect;
	}
	const float inv_w = 0.5f / float(p_half_size.x);
	const float inv_h = 0.5f / float(p_half_size.y);
	const float u_lo = CLAMP((float(p_clip_min.x) + float(p_half_size.x)) * inv_w, 0.0f, 1.0f);
	const float u_hi = CLAMP((float(p_clip_max.x) + float(p_half_size.x)) * inv_w, 0.0f, 1.0f);
	const float v_lo = CLAMP((float(p_clip_min.y) + float(p_half_size.y)) * inv_h, 0.0f, 1.0f);
	const float v_hi = CLAMP((float(p_clip_max.y) + float(p_half_size.y)) * inv_h, 0.0f, 1.0f);
	return Rect2(
			p_tex_rect.position.x + (1.0f - u_hi) * p_tex_rect.size.x,
			p_tex_rect.position.y + (1.0f - v_hi) * p_tex_rect.size.y,
			(u_hi - u_lo) * p_tex_rect.size.x,
			(v_hi - v_lo) * p_tex_rect.size.y);
}

// Maps project ScreenSpaceContactShadowsLength enum (Short/Medium/Long) to screen-height fraction.
_FORCE_INLINE_ float project_contact_shadow_length_fraction(int p_project_length_enum) {
	switch (p_project_length_enum) {
		case RSE::SCREEN_SPACE_CONTACT_SHADOWS_LENGTH_SHORT:
			return CONTACT_SHADOW_PROJECT_LENGTH_SHORT;
		case RSE::SCREEN_SPACE_CONTACT_SHADOWS_LENGTH_LONG:
			return CONTACT_SHADOW_PROJECT_LENGTH_LONG;
		case RSE::SCREEN_SPACE_CONTACT_SHADOWS_LENGTH_MEDIUM:
		default:
			return CONTACT_SHADOW_PROJECT_LENGTH_MEDIUM;
	}
}

struct ResolvedContactShadow {
	float length = 0.0f;
	bool world_space = false;
	bool active = false;
};

// Resolves a light's effective contact shadow length and mode:
// - allow == false or light_length == 0.0 -> off (byte-identical to disabled).
// - light_length < 0.0 (e.g. -1.0) -> inherits project enabled switch and project length (screen-space).
// - light_length > 0.0 -> explicit per-light override (screen-space fraction or world-space meters).
// - Both casting and non-casting intensities <= 0.0 -> inactive.
_FORCE_INLINE_ ResolvedContactShadow resolve_contact_shadow(
		float p_light_length,
		bool p_light_world_space,
		bool p_allow_contact_shadows,
		float p_casting_intensity,
		float p_non_casting_intensity,
		bool p_project_enabled,
		int p_project_length_enum) {
	ResolvedContactShadow out;
	if (!p_allow_contact_shadows) {
		return out;
	}
	if (p_casting_intensity <= 0.0f && p_non_casting_intensity <= 0.0f) {
		return out;
	}
	if (p_light_length == 0.0f) {
		return out;
	}
	if (p_light_length < 0.0f) {
		if (!p_project_enabled) {
			return out;
		}
		out.length = project_contact_shadow_length_fraction(p_project_length_enum);
		out.world_space = false;
		out.active = out.length > 0.0f;
		return out;
	}
	out.length = p_light_length;
	out.world_space = p_light_world_space;
	out.active = true;
	return out;
}

// Converts a contact shadow length to screen-space pixels at linear view depth p_view_depth.
// Matches screen_space_contact_shadows.glsl.
_FORCE_INLINE_ float contact_shadow_length_to_pixels(float p_length, bool p_world_space, float p_screen_height, float p_proj_scale_y, float p_view_depth, bool p_orthogonal) {
	if (p_length <= 0.0f || p_screen_height <= 0.0f) {
		return 0.0f;
	}
	if (!p_world_space) {
		return p_length * p_screen_height;
	}
	const float half_h_scale = std::fabs(p_proj_scale_y) * 0.5f * p_screen_height;
	if (p_orthogonal) {
		return p_length * half_h_scale;
	}
	return p_length * half_h_scale / MAX(p_view_depth, 1e-4f);
}

// Selects the minimum SSCS compute pipeline variant (READ_COUNT 2/3/4 -> max 63/127/191 px)
// sufficient for a light's contact shadow length.
_FORCE_INLINE_ RSE::ScreenSpaceContactShadowsLength select_contact_shadow_quality(float p_length, bool p_world_space, int p_screen_height, int p_project_length_enum) {
	if (p_world_space) {
		// World-space rays vary with depth per pixel; use medium/long cap so close geometry isn't truncated.
		return p_length > 0.5f ? RSE::SCREEN_SPACE_CONTACT_SHADOWS_LENGTH_LONG : RSE::SCREEN_SPACE_CONTACT_SHADOWS_LENGTH_MEDIUM;
	}
	const float px = p_length * float(MAX(p_screen_height, 1));
	if (px <= 63.0f) {
		return RSE::SCREEN_SPACE_CONTACT_SHADOWS_LENGTH_SHORT;
	}
	if (px <= 127.0f) {
		return RSE::SCREEN_SPACE_CONTACT_SHADOWS_LENGTH_MEDIUM;
	}
	return RSE::SCREEN_SPACE_CONTACT_SHADOWS_LENGTH_LONG;
}

} // namespace LightSourceParity

} // namespace RendererRD
