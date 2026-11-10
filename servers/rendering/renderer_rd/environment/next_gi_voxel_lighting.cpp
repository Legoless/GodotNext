/**************************************************************************/
/*  next_gi_voxel_lighting.cpp                                            */
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

#include "next_gi_voxel_lighting.h"

#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "core/string/print_string.h"
#include "servers/rendering/renderer_geometry_instance.h"
#include "servers/rendering/renderer_rd/environment/next_gi_global_sdf.h"
#include "servers/rendering/renderer_rd/environment/next_gi_relight.h"
#include "servers/rendering/renderer_rd/environment/next_gi_sdf.h"
#include "servers/rendering/renderer_rd/environment/next_gi_src.h"
#include "servers/rendering/renderer_rd/shader_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_voxel_lighting.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_scene_render.h"
#include "servers/rendering/rendering_device.h"

#include <cmath>
#include <cstring>

static_assert(NextGIVoxelLighting::allocated_volume_bytes() == 33554432ull, "Voxel lighting buffers must stay within 32 MiB.");
static_assert(NextGIVoxelLighting::visibility_buffer_bytes() == 0, "Voxel lighting does not allocate a visibility buffer.");
static_assert(NextGIVoxelLighting::radiance_buffer_bytes() + NextGIVoxelLighting::weight_buffer_bytes() + NextGIVoxelLighting::staging_buffer_bytes() == NextGIVoxelLighting::allocated_volume_bytes(), "volume_bytes must be the sum of the real buffers.");

static constexpr float VOXEL_LIGHTING_HUGE = 1.0e20f;
// Same values as nextgi_voxel_lighting.glsl.
static constexpr uint32_t GPU_MODE_SHADE = 0;
static constexpr uint32_t GPU_MODE_CLEAR = 1;
static constexpr uint32_t GPU_MODE_PROBE = 2;
static constexpr uint32_t GPU_SHADE_STRIDE = 7;
static constexpr uint32_t GPU_PROBE_STRIDE = 4;
static constexpr uint32_t GPU_PROBE_HEADER = 16;
static constexpr uint32_t GPU_FLAG_COUNT = 1;
static constexpr const char *VOXEL_LIGHTING_READBACK = "rendering/global_illumination/nextgi/debug/voxel_lighting_readback";

static_assert(NextGIVoxelLighting::debug_buffer_bytes() == 16u + NextGIVoxelLighting::MAX_DEBUG_PROBES * GPU_PROBE_STRIDE * sizeof(uint32_t), "One vec4 per probe after the four counters.");
static_assert(1u + GPU_PROBE_HEADER + NextGIVoxelLighting::MAX_DEBUG_PROBES * GPU_PROBE_STRIDE <= NextGIVoxelLighting::staging_buffer_bytes() / sizeof(uint32_t), "Probes fit the staging buffer in one dispatch.");

struct NextGIVoxelLighting::GPUState {
	enum Bound {
		BOUND_ATLAS,
		BOUND_PAGES,
		BOUND_MATERIAL_DEPTH,
		BOUND_CARDS,
		BOUND_CARD_SETS,
		BOUND_DEBUG,
		BOUND_MAX,
	};
	NextgiVoxelLightingShaderRD shader;
	RID version;
	RID pipeline;
	RID radiance;
	RID weights;
	RID staging;
	RID dummy;
	RID debug;
	RID fallback_texture;
	RID sampler;
	RID uniform_set;
	RID bound[BOUND_MAX];
	bool failed = false;
	bool volumes_cleared = false;
};

static uint32_t _pack_positive_float(float p_value, uint32_t p_mantissa_bits) {
	if (!(p_value > 0.0f) || p_mantissa_bits == 0 || p_mantissa_bits > 6) {
		return 0;
	}
	uint32_t bits = 0;
	memcpy(&bits, &p_value, sizeof(bits));
	const int unbiased = int((bits >> 23) & 255u) - 127;
	const uint32_t mantissa = bits & 0x7fffffu;
	int stored = unbiased + NextGIVoxelLighting::R11G11B10_BIAS;
	const uint32_t max_mantissa = (1u << p_mantissa_bits) - 1u;
	if (stored <= 0) {
		const float scale = std::ldexp(1.0f, 1 - NextGIVoxelLighting::R11G11B10_BIAS - int(p_mantissa_bits));
		if (!(scale > 0.0f)) {
			return 0;
		}
		const float rounded = Math::floor(p_value / scale + 0.5f);
		if (!(rounded > 0.0f)) {
			return 0;
		}
		return MIN(uint32_t(rounded), max_mantissa);
	}
	if (stored > NextGIVoxelLighting::R11G11B10_MAX_STORED) {
		return (uint32_t(NextGIVoxelLighting::R11G11B10_MAX_STORED) << p_mantissa_bits) | max_mantissa;
	}
	const uint32_t shift = 23 - p_mantissa_bits;
	uint32_t packed_mantissa = mantissa >> shift;
	if (shift > 0 && ((mantissa >> (shift - 1)) & 1u) != 0) {
		packed_mantissa += 1u;
		if (packed_mantissa > max_mantissa) {
			packed_mantissa = 0;
			stored += 1;
			if (stored > NextGIVoxelLighting::R11G11B10_MAX_STORED) {
				return (uint32_t(NextGIVoxelLighting::R11G11B10_MAX_STORED) << p_mantissa_bits) | max_mantissa;
			}
		}
	}
	return (uint32_t(stored) << p_mantissa_bits) | packed_mantissa;
}

static float _unpack_positive_float(uint32_t p_packed, uint32_t p_mantissa_bits) {
	const uint32_t mantissa_mask = (1u << p_mantissa_bits) - 1u;
	const uint32_t mantissa = p_packed & mantissa_mask;
	const uint32_t exponent = (p_packed >> p_mantissa_bits) & 31u;
	if (exponent == 0) {
		if (mantissa == 0) {
			return 0.0f;
		}
		return std::ldexp(float(mantissa) / float(1u << p_mantissa_bits), 1 - NextGIVoxelLighting::R11G11B10_BIAS);
	}
	const float significand = 1.0f + float(mantissa) / float(1u << p_mantissa_bits);
	return std::ldexp(significand, int(exponent) - NextGIVoxelLighting::R11G11B10_BIAS);
}

float NextGIVoxelLighting::level_voxel_size(uint32_t p_level) {
	return 2.0f * NextGIGlobalSDF::level_voxel_size(p_level);
}

Vector3 NextGIVoxelLighting::voxel_center(uint32_t p_level, const Vector3i &p_sdf_origin, const Vector3i &p_voxel) {
	const float sdf_voxel = NextGIGlobalSDF::level_voxel_size(p_level);
	const float lighting = level_voxel_size(p_level);
	const Vector3 origin_world = Vector3(p_sdf_origin) * sdf_voxel;
	return origin_world + (Vector3(p_voxel) + Vector3(0.5f, 0.5f, 0.5f)) * lighting;
}

uint32_t NextGIVoxelLighting::voxel_index(const Vector3i &p_voxel) {
	return uint32_t(p_voxel.x + p_voxel.y * int(LEVEL_SIZE) + p_voxel.z * int(LEVEL_SIZE * LEVEL_SIZE));
}

Vector3i NextGIVoxelLighting::voxel_coord(uint32_t p_index) {
	const uint32_t slice = LEVEL_SIZE * LEVEL_SIZE;
	const uint32_t z = p_index / slice;
	const uint32_t rest = p_index % slice;
	return Vector3i(int(rest % LEVEL_SIZE), int(rest / LEVEL_SIZE), int(z));
}

uint32_t NextGIVoxelLighting::direction_index(uint32_t p_level, uint32_t p_voxel, uint32_t p_direction) {
	return ((p_level * VOXELS_PER_LEVEL) + p_voxel) * DIRECTIONS + p_direction;
}

Vector3 NextGIVoxelLighting::direction_vector(uint32_t p_direction) {
	switch (p_direction) {
		case 0:
			return Vector3(1.0f, 0.0f, 0.0f);
		case 1:
			return Vector3(0.0f, 1.0f, 0.0f);
		case 2:
			return Vector3(0.0f, 0.0f, 1.0f);
		case 3:
			return Vector3(-1.0f, 0.0f, 0.0f);
		case 4:
			return Vector3(0.0f, -1.0f, 0.0f);
		default:
			return Vector3(0.0f, 0.0f, -1.0f);
	}
}

int NextGIVoxelLighting::wrap_coord(int p_value) {
	int wrapped = p_value % int(LEVEL_SIZE);
	if (wrapped < 0) {
		wrapped += int(LEVEL_SIZE);
	}
	return wrapped;
}

uint32_t NextGIVoxelLighting::voxels_for_sdf_brick(const Vector3i &p_brick, Vector3i *r_voxels, uint32_t p_max) {
	if (r_voxels == nullptr || p_max == 0) {
		return 0;
	}
	const Vector3i base = p_brick * int(SDF_BRICK_VOXELS);
	uint32_t count = 0;
	for (int z = 0; z < int(SDF_BRICK_VOXELS); z++) {
		for (int y = 0; y < int(SDF_BRICK_VOXELS); y++) {
			for (int x = 0; x < int(SDF_BRICK_VOXELS); x++) {
				const Vector3i voxel = base + Vector3i(x, y, z);
				if (voxel.x < 0 || voxel.y < 0 || voxel.z < 0 || voxel.x >= int(LEVEL_SIZE) || voxel.y >= int(LEVEL_SIZE) || voxel.z >= int(LEVEL_SIZE)) {
					continue;
				}
				if (count < p_max) {
					r_voxels[count] = voxel;
				}
				count++;
			}
		}
	}
	return MIN(count, p_max);
}

uint32_t NextGIVoxelLighting::facing_direction(uint32_t p_axis, bool p_positive) {
	return p_positive ? p_axis + 3u : p_axis;
}

uint32_t NextGIVoxelLighting::pack_visibility(uint32_t p_instance, uint32_t p_card, uint32_t p_hit_t) {
	return (p_instance & 0xFFFFFu) | ((p_card & 0xFu) << 20) | ((p_hit_t & 0xFFu) << 24);
}

void NextGIVoxelLighting::unpack_visibility(uint32_t p_packed, uint32_t &r_instance, uint32_t &r_card, uint32_t &r_hit_t) {
	r_instance = p_packed & 0xFFFFFu;
	r_card = (p_packed >> 20) & 0xFu;
	r_hit_t = (p_packed >> 24) & 0xFFu;
}

uint32_t NextGIVoxelLighting::atomic_min_visibility(uint32_t p_current, uint32_t p_candidate) {
	return MIN(p_current, p_candidate);
}

uint32_t NextGIVoxelLighting::quantize_hit_t(float p_distance, float p_max_distance) {
	if (!(p_max_distance > 0.0f) || !(p_distance > 0.0f)) {
		return 0;
	}
	const float clamped = CLAMP(p_distance / p_max_distance, 0.0f, 1.0f);
	return MIN(uint32_t(Math::floor(clamped * float(MAX_HIT_T))), MAX_HIT_T);
}

uint32_t NextGIVoxelLighting::pack_r11g11b10(const Color &p_color) {
	const uint32_t red = _pack_positive_float(p_color.r, 6);
	const uint32_t green = _pack_positive_float(p_color.g, 6);
	const uint32_t blue = _pack_positive_float(p_color.b, 5);
	return red | (green << 11) | (blue << 22);
}

Color NextGIVoxelLighting::unpack_r11g11b10(uint32_t p_packed) {
	Color color;
	color.r = _unpack_positive_float(p_packed & 0x7FFu, 6);
	color.g = _unpack_positive_float((p_packed >> 11) & 0x7FFu, 6);
	color.b = _unpack_positive_float((p_packed >> 22) & 0x3FFu, 5);
	color.a = 1.0f;
	return color;
}

uint32_t NextGIVoxelLighting::pack_normal(const Vector3 &p_normal) {
	const Vector3 normal = p_normal.length_squared() > 1e-12f ? p_normal.normalized() : Vector3(0.0f, 0.0f, 1.0f);
	const Vector2 oct = normal.octahedron_encode();
	const uint32_t x = uint32_t(CLAMP(oct.x, 0.0f, 1.0f) * 65535.0f + 0.5f);
	const uint32_t y = uint32_t(CLAMP(oct.y, 0.0f, 1.0f) * 65535.0f + 0.5f);
	return (x & 0xFFFFu) | ((y & 0xFFFFu) << 16);
}

Vector3 NextGIVoxelLighting::unpack_normal(uint32_t p_packed) {
	return Vector3::octahedron_decode(Vector2(float(p_packed & 0xFFFFu) / 65535.0f, float(p_packed >> 16) / 65535.0f));
}

NextGIVoxelLighting::CardChoice NextGIVoxelLighting::choose_card(const CardQuery *p_cards, uint32_t p_count, const Vector3 &p_position, const Vector3 &p_normal) {
	CardChoice best;
	if (p_cards == nullptr || p_count == 0) {
		return best;
	}
	const uint32_t count = MIN(p_count, MAX_CARDS);
	bool found = false;
	float best_align = -2.0f;
	for (uint32_t index = 0; index < count; index++) {
		const CardQuery &card = p_cards[index];
		const float texel = card.texel_size > 0.0f ? card.texel_size : 1.0f;
		const float tolerance = CARD_DEPTH_TEXELS * texel;
		const Vector3 relative = p_position - card.origin;
		const float u = card.axis_u.dot(relative);
		const float v = card.axis_v.dot(relative);
		if (Math::abs(u) > card.half_u || Math::abs(v) > card.half_v) {
			continue;
		}
		const float u_norm = card.half_u > 0.0f ? u / card.half_u : 0.0f;
		const float v_norm = card.half_v > 0.0f ? v / card.half_v : 0.0f;
		const float u01 = CLAMP(u_norm * 0.5f + 0.5f, 0.0f, 1.0f);
		const float v01 = CLAMP(v_norm * 0.5f + 0.5f, 0.0f, 1.0f);
		// Distance below the card plane; the capture camera looks along -axis_n.
		const float depth = -card.axis_n.dot(relative);
		float depth_error = 0.0f;
		if (card.depth != nullptr && card.depth_width > 0 && card.depth_height > 0) {
			// D6: the captured material depth at the hit's card texel.
			const uint32_t tx = MIN(uint32_t(u01 * float(card.depth_width)), card.depth_width - 1);
			const uint32_t ty = MIN(uint32_t(v01 * float(card.depth_height)), card.depth_height - 1);
			const float card_depth = card.depth[ty * card.depth_width + tx];
			if (!(card_depth > 0.0f) || Math::abs(card_depth - depth) > tolerance) {
				continue;
			}
			depth_error = Math::abs(card_depth - depth);
		} else {
			if (depth < -tolerance || depth > card.depth_range + tolerance) {
				continue;
			}
			depth_error = MAX(MAX(-depth, depth - card.depth_range), 0.0f);
		}
		const float align = card.axis_n.dot(p_normal);
		if (found && align <= best_align) {
			continue;
		}
		found = true;
		best_align = align;
		best.card = index;
		best.missing = false;
		best.projection = align;
		best.depth_error = depth_error / texel;
		best.u = u01;
		best.v = v01;
	}
	return best;
}

float NextGIVoxelLighting::shade_weight(bool p_missing, float p_projection_cosine, float p_depth_error_texels) {
	if (p_missing || p_depth_error_texels > CARD_DEPTH_TEXELS) {
		return 0.0f;
	}
	return CLAMP(p_projection_cosine, 0.0f, 1.0f);
}

Vector3 NextGIVoxelLighting::gradient_from_taps(const float p_taps[6], float p_step) {
	const float denom = Math::abs(p_step) > 1e-8f ? (2.0f * p_step) : 1.0f;
	return Vector3(p_taps[0] - p_taps[1], p_taps[2] - p_taps[3], p_taps[4] - p_taps[5]) / denom;
}

// Pre-weighted axis blend. The 0.5 cutoff is applied once by the caller, matching
// nextgi_voxel_lighting_lookup, so a trilinear corner below 0.5 still contributes.
struct VoxelAxisBlend {
	Color preweighted = Color(0.0f, 0.0f, 0.0f, 0.0f);
	float weight = 0.0f;
};

static VoxelAxisBlend _axis_blend(const Vector3 &p_gradient, const NextGIVoxelLighting::DirectionSample p_samples[NextGIVoxelLighting::DIRECTIONS]) {
	VoxelAxisBlend blend;
	if (p_samples == nullptr || p_gradient.length_squared() <= 1e-12f) {
		return blend;
	}
	const Vector3 normal = p_gradient.normalized();
	float axis_x = Math::abs(normal.x);
	float axis_y = Math::abs(normal.y);
	float axis_z = Math::abs(normal.z);
	const float axis_sum = axis_x + axis_y + axis_z;
	if (!(axis_sum > 1e-6f)) {
		return blend;
	}
	axis_x /= axis_sum;
	axis_y /= axis_sum;
	axis_z /= axis_sum;
	// Slot d stores a surface facing -d.
	const uint32_t dir_x = NextGIVoxelLighting::facing_direction(0, normal.x >= 0.0f);
	const uint32_t dir_y = NextGIVoxelLighting::facing_direction(1, normal.y >= 0.0f);
	const uint32_t dir_z = NextGIVoxelLighting::facing_direction(2, normal.z >= 0.0f);
	blend.weight = axis_x * p_samples[dir_x].weight + axis_y * p_samples[dir_y].weight + axis_z * p_samples[dir_z].weight;
	blend.preweighted = p_samples[dir_x].radiance * (axis_x * p_samples[dir_x].weight) + p_samples[dir_y].radiance * (axis_y * p_samples[dir_y].weight) + p_samples[dir_z].radiance * (axis_z * p_samples[dir_z].weight);
	return blend;
}

NextGIVoxelLighting::LookupResult NextGIVoxelLighting::lookup(const Vector3 &p_gradient, const DirectionSample p_samples[DIRECTIONS], const Color &p_wrc) {
	LookupResult result;
	result.radiance = p_wrc;
	result.weight = 0.0f;
	result.used_wrc = true;
	const VoxelAxisBlend blend = _axis_blend(p_gradient, p_samples);
	result.weight = blend.weight;
	if (blend.weight < WEIGHT_FALLBACK) {
		return result;
	}
	result.radiance = blend.preweighted / blend.weight;
	result.used_wrc = false;
	return result;
}

namespace {

// One instance seen along one axis ray, in the instance's local space. A world step of t moves
// the local point by direction * t, so |direction| converts local distances to world ones.
struct VoxelInstanceRay {
	const NextGIVoxelLighting::TraceInstance *instance = nullptr;
	Vector3 origin;
	Vector3 direction;
	float local_per_world = 1.0f;
	float eps = 0.0f; // Local units.
	float min_step = 0.0f; // World units.
	bool is_signed = true;

	float sample(float p_t) const {
		return instance->field->sample_distance(origin + direction * p_t);
	}
	// World distance the ray can safely advance from a local distance.
	float safe_step(float p_local_distance) const {
		return MAX(Math::abs(p_local_distance) / local_per_world, min_step);
	}
};

struct VoxelMarchEvent {
	bool found = false;
	bool exit = false;
	// Projected onto the surface (a touch or a nearest approach), not refined at a crossing.
	bool projected = false;
	float t = 0.0f;
	Vector3 local;
	Vector3 local_normal;
};

bool _voxel_field_ok(const NextGIVoxelLighting::TraceInstance &p_instance) {
	return p_instance.field != nullptr && p_instance.field->is_valid();
}

float _voxel_local_eps(const MeshSDFData &p_field) {
	return MAX(p_field.voxel_size * 0.5f, 1e-4f);
}

Vector3 _voxel_local_normal(const MeshSDFData &p_field, const Vector3 &p_local, const Vector3 &p_fallback) {
	const float step = _voxel_local_eps(p_field);
	const float taps[6] = {
		p_field.sample_distance(p_local + Vector3(step, 0.0f, 0.0f)),
		p_field.sample_distance(p_local - Vector3(step, 0.0f, 0.0f)),
		p_field.sample_distance(p_local + Vector3(0.0f, step, 0.0f)),
		p_field.sample_distance(p_local - Vector3(0.0f, step, 0.0f)),
		p_field.sample_distance(p_local + Vector3(0.0f, 0.0f, step)),
		p_field.sample_distance(p_local - Vector3(0.0f, 0.0f, step)),
	};
	const Vector3 gradient = NextGIVoxelLighting::gradient_from_taps(taps, step);
	return gradient.length_squared() > 1e-12f ? gradient.normalized() : p_fallback;
}

// Normals transform by the inverse transpose of the instance basis.
Vector3 _voxel_world_normal(const NextGIVoxelLighting::TraceInstance &p_instance, const Vector3 &p_local_normal) {
	const Vector3 normal = p_instance.inverse.basis.xform_inv(p_local_normal);
	return normal.length_squared() > 1e-12f ? normal.normalized() : p_local_normal;
}

Vector3 _voxel_local_direction(const NextGIVoxelLighting::TraceInstance &p_instance, const Vector3 &p_world) {
	const Vector3 local = p_instance.transform.basis.xform_inv(p_world);
	return local.length_squared() > 1e-12f ? local.normalized() : p_world;
}

// Bisects the zero crossing between p_t_a (value p_d_a) and p_t_b, which lies on the other side.
float _voxel_refine(const VoxelInstanceRay &p_ray, float p_t_a, float p_d_a, float p_t_b) {
	const bool inside_a = p_d_a < 0.0f;
	float a = p_t_a;
	float b = p_t_b;
	for (uint32_t step = 0; step < NextGIVoxelLighting::REFINE_STEPS; step++) {
		const float mid = (a + b) * 0.5f;
		if ((p_ray.sample(mid) < 0.0f) == inside_a) {
			a = mid;
		} else {
			b = mid;
		}
	}
	return (a + b) * 0.5f;
}

VoxelMarchEvent _voxel_event(const VoxelInstanceRay &p_ray, float p_t, bool p_exit, const Vector3 &p_world_direction) {
	VoxelMarchEvent event;
	event.found = true;
	event.exit = p_exit;
	event.t = p_t;
	event.local = p_ray.origin + p_ray.direction * p_t;
	// An entry faces against the ray, an exit along it.
	const Vector3 fallback = _voxel_local_direction(*p_ray.instance, p_exit ? p_world_direction : -p_world_direction);
	event.local_normal = _voxel_local_normal(*p_ray.instance->field, event.local, fallback);
	return event;
}

// First surface entered along the ray from p_t (local distance p_d, outside). A signed field
// is refined to its zero crossing. A ray that only grazes the surface, or an unsigned field,
// takes its nearest approach within eps, projected onto the surface, unless p_crossings_only
// (a signed ray that starts within eps of a surface it runs along).
VoxelMarchEvent _voxel_march_entry(const VoxelInstanceRay &p_ray, float p_t, float p_d, float p_max, const Vector3 &p_world_direction, bool p_crossings_only = false) {
	float t = p_t;
	float d = p_d;
	float near_t = -1.0f;
	float near_d = VOXEL_LIGHTING_HUGE;
	for (uint32_t step = 0; step < NextGIVoxelLighting::MAX_MARCH_STEPS; step++) {
		float advance = p_ray.safe_step(d);
		if (!p_ray.is_signed && d <= p_ray.eps) {
			// Unsigned fields converge on the sheet without crossing it.
			advance = MAX(d / p_ray.local_per_world, p_ray.min_step * 0.1f);
		}
		const float t_next = t + advance;
		if (t_next > p_max) {
			break;
		}
		const float d_next = p_ray.sample(t_next);
		if (d_next >= VOXEL_LIGHTING_HUGE) {
			break;
		}
		if (p_ray.is_signed && d_next <= 0.0f) {
			return _voxel_event(p_ray, _voxel_refine(p_ray, t, d, t_next), false, p_world_direction);
		}
		// Running along a touched surface keeps the ray within eps; that is no graze.
		if (!p_crossings_only && d_next <= p_ray.eps && d_next < near_d) {
			near_t = t_next;
			near_d = d_next;
		} else if (near_t >= 0.0f && d_next > near_d) {
			break; // Moving away again after the nearest approach.
		}
		t = t_next;
		d = d_next;
	}
	if (near_t < 0.0f) {
		return VoxelMarchEvent();
	}
	VoxelMarchEvent event = _voxel_event(p_ray, near_t, false, p_world_direction);
	event.local -= event.local_normal * near_d;
	event.projected = true;
	return event;
}

// Surface left along the ray from inside a signed field.
VoxelMarchEvent _voxel_march_exit(const VoxelInstanceRay &p_ray, float p_d, float p_max, const Vector3 &p_world_direction) {
	float t = 0.0f;
	float d = p_d;
	for (uint32_t step = 0; step < NextGIVoxelLighting::MAX_MARCH_STEPS; step++) {
		const float t_next = t + p_ray.safe_step(d);
		if (t_next > p_max) {
			break;
		}
		const float d_next = p_ray.sample(t_next);
		if (d_next >= VOXEL_LIGHTING_HUGE) {
			break;
		}
		if (d_next >= 0.0f) {
			return _voxel_event(p_ray, _voxel_refine(p_ray, t, d, t_next), true, p_world_direction);
		}
		t = t_next;
		d = d_next;
	}
	return VoxelMarchEvent();
}

// Stores the event in slot p_slot when it is nearer than what the slot holds and faces -p_slot.
// A crossing only has to face that way; a projected surface has to face it clearly, so that a
// surface running along the axis (a floor beside a voxel center) stays out of that axis's slots.
void _voxel_keep(NextGIVoxelLighting::AxisHit &r_slot, uint32_t p_slot, const VoxelInstanceRay &p_ray, const VoxelMarchEvent &p_event, float p_max_distance) {
	const uint32_t id = p_ray.instance->id;
	if (r_slot.hit && (p_event.t > r_slot.distance || (p_event.t == r_slot.distance && id >= r_slot.instance))) {
		return;
	}
	const NextGIVoxelLighting::TraceInstance &instance = *p_ray.instance;
	const Vector3 world_normal = _voxel_world_normal(instance, p_event.local_normal);
	const float facing_min = p_event.projected ? NextGIVoxelLighting::SLOT_FACING_MIN : 0.0f;
	if (!(world_normal.dot(NextGIVoxelLighting::direction_vector(p_slot)) < -facing_min)) {
		return;
	}
	r_slot.hit = true;
	r_slot.exit = p_event.exit;
	r_slot.instance = id;
	r_slot.distance = p_event.t;
	r_slot.hit_t = NextGIVoxelLighting::quantize_hit_t(p_event.t, p_max_distance);
	// The card is chosen by the GPU shade (D6 needs the captured material depth).
	r_slot.packed = NextGIVoxelLighting::pack_visibility(id, NextGIVoxelLighting::NO_CARD, r_slot.hit_t);
	r_slot.object_position = p_event.local;
	r_slot.object_normal = p_event.local_normal;
	r_slot.position = instance.transform.xform(p_event.local);
	r_slot.normal = world_normal;
}

// From inside a solid, the exit found along the ray faces along it and goes to the opposite
// slot. Lookups read a voxel from both sides of its center, so inside a wall that slot is read
// from both rooms. The exit is kept only when the solid's exit along the opposite ray is not
// nearer: the voxel serves the side its center is nearer to. The far face would light the near
// room, because a surface just past the near face that faces the wall reads that slot.
void _voxel_keep_exit(NextGIVoxelLighting::AxisHit *r_slots, uint32_t p_direction, const VoxelInstanceRay &p_ray, float p_d, float p_max_distance, const Vector3 &p_world_direction) {
	const VoxelMarchEvent leave = _voxel_march_exit(p_ray, p_d, p_max_distance, p_world_direction);
	if (!leave.found) {
		return;
	}
	VoxelInstanceRay back = p_ray;
	back.direction = -p_ray.direction;
	const VoxelMarchEvent other = _voxel_march_exit(back, p_d, p_max_distance, -p_world_direction);
	if (other.found && other.t < leave.t) {
		return;
	}
	const uint32_t slot = NextGIVoxelLighting::opposite_direction(p_direction);
	_voxel_keep(r_slots[slot], slot, p_ray, leave, p_max_distance);
}

} // namespace

uint32_t NextGIVoxelLighting::trace_voxel(const TraceInstance *p_instances, uint32_t p_count, const Vector3 &p_center, float p_max_distance, AxisHit r_slots[DIRECTIONS]) {
	for (uint32_t direction = 0; direction < DIRECTIONS; direction++) {
		r_slots[direction] = AxisHit();
	}
	if (p_instances == nullptr || p_count == 0 || !(p_max_distance > 0.0f)) {
		return 0;
	}
	LocalVector<VoxelInstanceRay> rays;
	LocalVector<float> start;
	// Outward normal (instance-local) of the surface a center touches; zero without a gradient.
	LocalVector<Vector3> touch_normal;
	rays.resize(p_count);
	start.resize(p_count);
	touch_normal.resize(p_count);
	bool inside = false;
	for (uint32_t index = 0; index < p_count; index++) {
		VoxelInstanceRay &ray = rays[index];
		ray.instance = &p_instances[index];
		start[index] = VOXEL_LIGHTING_HUGE;
		touch_normal[index] = Vector3();
		if (!_voxel_field_ok(p_instances[index])) {
			continue;
		}
		const MeshSDFData &field = *p_instances[index].field;
		ray.origin = p_instances[index].inverse.xform(p_center);
		ray.eps = _voxel_local_eps(field);
		ray.is_signed = (field.flags & MeshSDFData::FLAG_UNSIGNED) == 0;
		start[index] = field.sample_distance(ray.origin);
		// A center inside any solid sees only the surfaces that leave it.
		if (ray.is_signed && start[index] < -ray.eps) {
			inside = true;
		} else if (Math::abs(start[index]) <= ray.eps) {
			touch_normal[index] = _voxel_local_normal(field, ray.origin, Vector3());
		}
	}
	// A center inside a solid but within eps of its surface is inside all the same. At the default
	// mesh-SDF density that is every center in a 10-25 cm wall: its field never drops below -eps.
	// Along an axis the touched surface faces, the ray leaves the solid at once, and what it meets
	// next lies on the far side of that surface (the next room). Nothing found along such an axis
	// goes to its slot. The solid's far face, the exit along the opposite axis, does not either: it
	// is farther than the touched face, and that slot is read from both rooms. The slot stays empty.
	uint32_t blocked = 0;
	if (!inside) {
		for (uint32_t index = 0; index < p_count; index++) {
			if (!rays[index].is_signed || !(start[index] < 0.0f) || touch_normal[index] == Vector3()) {
				continue;
			}
			const Vector3 normal = _voxel_world_normal(p_instances[index], touch_normal[index]);
			for (uint32_t direction = 0; direction < DIRECTIONS; direction++) {
				if (normal.dot(direction_vector(direction)) > SLOT_FACING_MIN) {
					blocked |= 1u << direction;
				}
			}
		}
	}
	for (uint32_t direction = 0; direction < DIRECTIONS; direction++) {
		const Vector3 world_direction = direction_vector(direction);
		const bool slot_blocked = (blocked & (1u << direction)) != 0;
		for (uint32_t index = 0; index < p_count; index++) {
			if (!(start[index] < VOXEL_LIGHTING_HUGE)) {
				continue;
			}
			VoxelInstanceRay ray = rays[index];
			ray.direction = p_instances[index].inverse.basis.xform(world_direction);
			ray.local_per_world = ray.direction.length();
			if (!(ray.local_per_world > 1e-8f)) {
				continue;
			}
			ray.min_step = ray.eps * 0.5f / ray.local_per_world;
			const float d0 = start[index];
			if (inside) {
				if (ray.is_signed && d0 < -ray.eps) {
					_voxel_keep_exit(r_slots, direction, ray, d0, p_max_distance, world_direction);
				}
				continue;
			}
			if (Math::abs(d0) <= ray.eps) {
				// The center touches this surface. From outside, it goes to slot d only when it
				// clearly faces -d. One that runs along the ray is in neither slot. One that faces
				// along the ray is being left (the opposite ray files it), so the ray continues to
				// the next entry.
				const Vector3 local_normal = touch_normal[index] != Vector3() ? touch_normal[index] : _voxel_local_direction(p_instances[index], -world_direction);
				const float facing = _voxel_world_normal(p_instances[index], local_normal).dot(world_direction);
				if (ray.is_signed && d0 < 0.0f) {
					// The center is inside this solid, so its surfaces are exits, refined to the
					// crossing. Along an axis the touched surface faces, the ray leaves through
					// it, and that exit, which faces d, goes to slot -d. The opposite ray
					// crosses the solid to its far face, which stays out of slot d: the touched
					// face is nearer (_voxel_keep_exit). Along the surface no exit is near enough
					// to matter, and a shallow one would file the surface in a slot it hardly
					// faces.
					if (Math::abs(facing) > SLOT_FACING_MIN) {
						_voxel_keep_exit(r_slots, direction, ray, d0, p_max_distance, world_direction);
					}
					continue;
				}
				if (slot_blocked) {
					continue;
				}
				if (facing < -SLOT_FACING_MIN) {
					VoxelMarchEvent touch;
					touch.found = true;
					touch.projected = true;
					touch.t = 0.0f;
					touch.local_normal = local_normal;
					touch.local = ray.origin - local_normal * d0;
					_voxel_keep(r_slots[direction], direction, ray, touch, p_max_distance);
					continue;
				}
				if (facing <= SLOT_FACING_MIN) {
					// The ray runs along the touched surface. A signed field still finds a later
					// entry into the same mesh (a wall rising from a touched floor) at a crossing
					// that clearly faces -d; a shallow crossing is the touched surface itself.
					if (ray.is_signed) {
						const VoxelMarchEvent entry = _voxel_march_entry(ray, 0.0f, d0, p_max_distance, world_direction, true);
						if (entry.found && _voxel_world_normal(p_instances[index], entry.local_normal).dot(world_direction) < -SLOT_FACING_MIN) {
							_voxel_keep(r_slots[direction], direction, ray, entry, p_max_distance);
						}
					}
					continue;
				}
				const float t0 = 2.0f * ray.eps / ray.local_per_world;
				const float d1 = ray.sample(t0);
				if (t0 >= p_max_distance || d1 <= ray.eps) {
					continue;
				}
				const VoxelMarchEvent entry = _voxel_march_entry(ray, t0, d1, p_max_distance, world_direction);
				if (entry.found) {
					_voxel_keep(r_slots[direction], direction, ray, entry, p_max_distance);
				}
				continue;
			}
			if (slot_blocked) {
				continue;
			}
			const VoxelMarchEvent entry = _voxel_march_entry(ray, 0.0f, d0, p_max_distance, world_direction);
			if (entry.found) {
				_voxel_keep(r_slots[direction], direction, ray, entry, p_max_distance);
			}
		}
	}
	uint32_t filled = 0;
	for (uint32_t direction = 0; direction < DIRECTIONS; direction++) {
		filled += r_slots[direction].hit ? 1u : 0u;
	}
	return filled;
}

NextGIVoxelLighting::NextGIVoxelLighting() {
}

NextGIVoxelLighting::~NextGIVoxelLighting() {
	_free_gpu();
}

void NextGIVoxelLighting::_uniform_set_invalidated(void *p_userdata) {
	GPUState *state = static_cast<GPUState *>(p_userdata);
	if (state == nullptr) {
		return;
	}
	// A dependency free already destroyed the set. Drop the id so shutdown does not free it again.
	state->uniform_set = RID();
	for (uint32_t index = 0; index < GPUState::BOUND_MAX; index++) {
		state->bound[index] = RID();
	}
}

void NextGIVoxelLighting::_free_gpu() {
	if (gpu == nullptr) {
		return;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd != nullptr) {
		if (gpu->uniform_set.is_valid()) {
			rd->uniform_set_set_invalidation_callback(gpu->uniform_set, nullptr, nullptr);
			rd->free_rid(gpu->uniform_set);
			gpu->uniform_set = RID();
		}
		if (gpu->pipeline.is_valid()) {
			rd->free_rid(gpu->pipeline);
		}
		if (gpu->version.is_valid()) {
			gpu->shader.version_free(gpu->version);
		}
		const RID owned[] = { gpu->radiance, gpu->weights, gpu->staging, gpu->dummy, gpu->debug, gpu->fallback_texture, gpu->sampler };
		for (const RID &rid : owned) {
			if (rid.is_valid()) {
				rd->free_rid(rid);
			}
		}
	}
	memdelete(gpu);
	gpu = nullptr;
	allocated_bytes = 0;
}

void NextGIVoxelLighting::reset() {
	hits.clear();
	queue.clear();
	fill_begin = 0;
	queued.clear();
	for (uint32_t level = 0; level < LEVELS; level++) {
		origins[level] = Vector3i();
		origin_set[level] = false;
	}
	levels_to_fill = 0;
	seen_fields.clear();
	retrace_rays = 0;
	shaded_hits = 0;
	unpaged_hits = 0;
	covered_hits = -1;
	gpu_shaded_hits = -1;
	resampled_nonzero = -1;
	field_cache.clear();
	field_generation = UINT64_MAX;
	stable_ids.clear();
	next_stable_id = 1;
	gpu_clear_voxels.clear();
	gpu_shade_words.clear();
	debug_probe_positions.clear();
	debug_probe_normals.clear();
	debug_probe_results.clear();
	_free_gpu();
}

uint32_t NextGIVoxelLighting::_brick_key(uint32_t p_level, const Vector3i &p_brick) {
	return p_level | (uint32_t(p_brick.x) << 4) | (uint32_t(p_brick.y) << 10) | (uint32_t(p_brick.z) << 16);
}

Vector3i NextGIVoxelLighting::_lighting_origin(const Vector3i &p_sdf_origin) {
	return Vector3i(p_sdf_origin.x / 2, p_sdf_origin.y / 2, p_sdf_origin.z / 2);
}

AABB NextGIVoxelLighting::_brick_aabb(uint32_t p_level, const Vector3i &p_brick) const {
	const Vector3i base = p_brick * int(SDF_BRICK_VOXELS);
	const float size = level_voxel_size(p_level);
	const Vector3 corner = voxel_center(p_level, origins[p_level], base) - Vector3(size, size, size) * 0.5f;
	AABB box(corner, Vector3(size, size, size) * float(SDF_BRICK_VOXELS));
	box.grow_by(size * float(TRACE_REACH_VOXELS));
	return box;
}

Vector3i NextGIVoxelLighting::_physical_voxel(uint32_t p_level, const Vector3i &p_local) const {
	const Vector3i world = _lighting_origin(origins[p_level]) + p_local;
	return Vector3i(wrap_coord(world.x), wrap_coord(world.y), wrap_coord(world.z));
}

uint32_t NextGIVoxelLighting::_stable_id(RenderGeometryInstance *p_geometry, uint32_t p_element) {
	const uint64_t key = uint64_t(uintptr_t(p_geometry)) ^ (uint64_t(p_element) * 0x9E3779B97F4A7C15ULL);
	if (uint32_t *found = stable_ids.getptr(key)) {
		return *found;
	}
	uint32_t id = next_stable_id;
	if (id == 0 || id > 0xFFFFFu) {
		id = 0xFFFFFu;
	} else {
		next_stable_id++;
	}
	stable_ids.insert(key, id);
	return id;
}

const MeshSDFData *NextGIVoxelLighting::_cached_field(const RID &p_mesh) {
	if (!p_mesh.is_valid()) {
		return nullptr;
	}
	if (MeshSDFData *cached = field_cache.getptr(p_mesh)) {
		return cached;
	}
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	if (mesh_storage == nullptr) {
		return nullptr;
	}
	const NextGISDFAtlas::MeshPages *pages = mesh_storage->get_sdf_atlas().get_pages(p_mesh);
	if (pages == nullptr || pages->field.is_null()) {
		return nullptr;
	}
	field_cache.insert(p_mesh, pages->field->get_data());
	return field_cache.getptr(p_mesh);
}

void NextGIVoxelLighting::set_clipmap_origins(const Vector3i *p_origins, uint32_t p_count) {
	if (p_origins == nullptr) {
		return;
	}
	const uint32_t count = MIN(p_count, LEVELS);
	for (uint32_t level = 0; level < count; level++) {
		if (!origin_set[level]) {
			// A new level has no hits. refresh_instances queues the geometry already in it.
			origins[level] = p_origins[level];
			origin_set[level] = true;
			levels_to_fill |= 1u << level;
			continue;
		}
		if (origins[level] == p_origins[level]) {
			continue;
		}
		const Vector3i delta = p_origins[level] - origins[level];
		const Vector3i brick_delta = NextGIGlobalSDF::scroll_brick_delta(origins[level], p_origins[level]);
		const Vector3i aligned = brick_delta * int(NextGIGlobalSDF::BRICK_SIZE);
		if (delta != aligned) {
			_drop_level(level);
			origins[level] = p_origins[level];
		} else {
			_scroll_level(level, p_origins[level]);
		}
	}
}

void NextGIVoxelLighting::set_clipmap_origins_from_camera(const Vector3 &p_camera) {
	Vector3i level_origins[LEVELS];
	for (uint32_t level = 0; level < LEVELS; level++) {
		level_origins[level] = NextGIGlobalSDF::scrolled_origin(p_camera, level);
	}
	set_clipmap_origins(level_origins, LEVELS);
}

void NextGIVoxelLighting::_enqueue_brick(uint32_t p_level, const Vector3i &p_brick, bool p_fill) {
	if (p_level >= LEVELS) {
		return;
	}
	const int page = int(NextGIGlobalSDF::PAGE_SIZE);
	if (p_brick.x < 0 || p_brick.y < 0 || p_brick.z < 0 || p_brick.x >= page || p_brick.y >= page || p_brick.z >= page) {
		return;
	}
	const uint32_t key = _brick_key(p_level, p_brick);
	if (bool *queued_fill = queued.getptr(key)) {
		if (p_fill || !*queued_fill) {
			return;
		}
		// A dirty brick already waiting as fill moves up to the dirty bricks.
		for (uint32_t index = fill_begin; index < queue.size(); index++) {
			if (queue[index].level == p_level && queue[index].brick == p_brick) {
				queue.remove_at(index);
				break;
			}
		}
		*queued_fill = false;
	} else {
		queued.insert(key, p_fill);
	}
	DirtyBrick item;
	item.level = p_level;
	item.brick = p_brick;
	item.fill = p_fill;
	if (p_fill) {
		queue.push_back(item);
	} else {
		queue.insert(fill_begin, item);
		fill_begin++;
	}
}

void NextGIVoxelLighting::_count_fill_begin() {
	fill_begin = 0;
	while (fill_begin < queue.size() && !queue[fill_begin].fill) {
		fill_begin++;
	}
}

void NextGIVoxelLighting::_enqueue_aabb(const AABB &p_aabb, uint32_t p_level_mask, bool p_fill) {
	if (p_aabb.size.x < 0.0f || p_aabb.size.y < 0.0f || p_aabb.size.z < 0.0f) {
		return;
	}
	for (uint32_t level = 0; level < LEVELS; level++) {
		if (!origin_set[level] || (p_level_mask & (1u << level)) == 0) {
			continue;
		}
		const float brick_world = NextGIGlobalSDF::level_voxel_size(level) * float(NextGIGlobalSDF::BRICK_SIZE);
		if (!(brick_world > 0.0f)) {
			continue;
		}
		AABB grown = p_aabb;
		grown.grow_by(level_voxel_size(level) * float(TRACE_REACH_VOXELS));
		const Vector3 end = grown.position + grown.size;
		const int origin_brick[3] = {
			origins[level].x / int(NextGIGlobalSDF::BRICK_SIZE),
			origins[level].y / int(NextGIGlobalSDF::BRICK_SIZE),
			origins[level].z / int(NextGIGlobalSDF::BRICK_SIZE),
		};
		const int min_brick[3] = {
			int(Math::floor(grown.position.x / brick_world)) - origin_brick[0],
			int(Math::floor(grown.position.y / brick_world)) - origin_brick[1],
			int(Math::floor(grown.position.z / brick_world)) - origin_brick[2],
		};
		const int max_brick[3] = {
			int(Math::floor(end.x / brick_world)) - origin_brick[0],
			int(Math::floor(end.y / brick_world)) - origin_brick[1],
			int(Math::floor(end.z / brick_world)) - origin_brick[2],
		};
		if (max_brick[0] < 0 || max_brick[1] < 0 || max_brick[2] < 0) {
			continue;
		}
		if (min_brick[0] >= int(NextGIGlobalSDF::PAGE_SIZE) || min_brick[1] >= int(NextGIGlobalSDF::PAGE_SIZE) || min_brick[2] >= int(NextGIGlobalSDF::PAGE_SIZE)) {
			continue;
		}
		const int x0 = MAX(min_brick[0], 0);
		const int y0 = MAX(min_brick[1], 0);
		const int z0 = MAX(min_brick[2], 0);
		const int x1 = MIN(max_brick[0], int(NextGIGlobalSDF::PAGE_SIZE) - 1);
		const int y1 = MIN(max_brick[1], int(NextGIGlobalSDF::PAGE_SIZE) - 1);
		const int z1 = MIN(max_brick[2], int(NextGIGlobalSDF::PAGE_SIZE) - 1);
		for (int z = z0; z <= z1; z++) {
			for (int y = y0; y <= y1; y++) {
				for (int x = x0; x <= x1; x++) {
					_enqueue_brick(level, Vector3i(x, y, z), p_fill);
				}
			}
		}
	}
}

void NextGIVoxelLighting::enqueue_dirty_volumes(const AABB *p_volumes, uint32_t p_count) {
	if (p_volumes == nullptr || p_count == 0) {
		return;
	}
	for (uint32_t index = 0; index < p_count; index++) {
		_enqueue_aabb(p_volumes[index]);
	}
}

void NextGIVoxelLighting::refresh_instances(const InstanceBounds *p_instances, uint32_t p_count) {
	const uint32_t fill = levels_to_fill;
	levels_to_fill = 0;
	if (p_instances == nullptr || p_count == 0) {
		return;
	}
	// Registry events only cover changes. A created level and a mesh SDF that finished after its
	// bricks were traced have no event, so their instances are queued here.
	for (uint32_t index = 0; index < p_count; index++) {
		const InstanceBounds &instance = p_instances[index];
		if (instance.field == nullptr || !(instance.world_aabb.size.x > 0.0f)) {
			continue;
		}
		uint32_t mask = fill;
		const void *const *seen = seen_fields.getptr(instance.mesh);
		if (seen == nullptr || *seen != instance.field) {
			mask = (1u << LEVELS) - 1u;
		}
		if (mask != 0) {
			_enqueue_aabb(instance.world_aabb, mask, true);
		}
	}
	for (uint32_t index = 0; index < p_count; index++) {
		seen_fields[p_instances[index].mesh] = p_instances[index].field;
	}
}

bool NextGIVoxelLighting::begin_frame(uint64_t p_field_generation) {
	const bool fields_changed = p_field_generation != field_generation;
	if (fields_changed) {
		field_cache.clear();
		field_generation = p_field_generation;
	}
	return levels_to_fill != 0 || fields_changed;
}

uint32_t NextGIVoxelLighting::pending_bricks_at_level(uint32_t p_level) const {
	uint32_t count = 0;
	for (uint32_t index = 0; index < queue.size(); index++) {
		if (queue[index].level == p_level) {
			count++;
		}
	}
	return count;
}

void NextGIVoxelLighting::_drop_level(uint32_t p_level) {
	LocalVector<DirtyBrick> kept;
	for (uint32_t index = 0; index < queue.size(); index++) {
		if (queue[index].level != p_level) {
			kept.push_back(queue[index]);
		}
	}
	queue = kept;
	_rebuild_queue_index();
	_count_fill_begin();
	LocalVector<uint32_t> keys;
	for (const KeyValue<uint32_t, AxisHit> &entry : hits) {
		if (entry.key / DIRECTIONS / VOXELS_PER_LEVEL == p_level) {
			keys.push_back(entry.key);
		}
	}
	for (uint32_t key : keys) {
		hits.erase(key);
	}
	for (uint32_t voxel = 0; voxel < VOXELS_PER_LEVEL; voxel++) {
		gpu_clear_voxels.push_back(p_level * VOXELS_PER_LEVEL + voxel);
	}
	// The level restarts empty at its new origin, like a new level.
	levels_to_fill |= 1u << p_level;
}

void NextGIVoxelLighting::_scroll_level(uint32_t p_level, const Vector3i &p_new_origin) {
	const Vector3i delta = NextGIGlobalSDF::scroll_brick_delta(origins[p_level], p_new_origin);
	const int page = int(NextGIGlobalSDF::PAGE_SIZE);
	LocalVector<DirtyBrick> kept;
	HashMap<uint32_t, bool> seen;
	for (uint32_t index = 0; index < queue.size(); index++) {
		DirtyBrick item = queue[index];
		if (item.level == p_level) {
			const Vector3i moved = item.brick - delta;
			if (moved.x < 0 || moved.y < 0 || moved.z < 0 || moved.x >= page || moved.y >= page || moved.z >= page) {
				continue;
			}
			item.brick = moved;
		}
		const uint32_t key = _brick_key(item.level, item.brick);
		if (seen.has(key)) {
			continue;
		}
		seen.insert(key, item.fill);
		kept.push_back(item);
	}
	// Order is kept, so the dirty bricks still lead.
	queue = kept;
	queued = seen;
	_count_fill_begin();
	origins[p_level] = p_new_origin;
	LocalVector<Vector3i> edges;
	edges.resize(NextGIGlobalSDF::BRICKS_PER_LEVEL);
	const uint32_t edge_count = NextGIGlobalSDF::scrolled_edge_bricks(delta, edges.ptr(), uint32_t(edges.size()));
	for (uint32_t edge = 0; edge < edge_count; edge++) {
		_enqueue_brick(p_level, edges[edge]);
		Vector3i voxels[SDF_BRICK_VOXELS * SDF_BRICK_VOXELS * SDF_BRICK_VOXELS];
		const uint32_t voxel_count = voxels_for_sdf_brick(edges[edge], voxels, SDF_BRICK_VOXELS * SDF_BRICK_VOXELS * SDF_BRICK_VOXELS);
		for (uint32_t voxel = 0; voxel < voxel_count; voxel++) {
			_clear_voxel(p_level, _physical_voxel(p_level, voxels[voxel]));
		}
	}
}

void NextGIVoxelLighting::_clear_voxel(uint32_t p_level, const Vector3i &p_physical) {
	if (p_level >= LEVELS || p_physical.x < 0 || p_physical.y < 0 || p_physical.z < 0 || p_physical.x >= int(LEVEL_SIZE) || p_physical.y >= int(LEVEL_SIZE) || p_physical.z >= int(LEVEL_SIZE)) {
		return;
	}
	const uint32_t voxel = voxel_index(p_physical);
	for (uint32_t direction = 0; direction < DIRECTIONS; direction++) {
		hits.erase(direction_index(p_level, voxel, direction));
	}
	gpu_clear_voxels.push_back(p_level * VOXELS_PER_LEVEL + voxel);
}

void NextGIVoxelLighting::_note_voxel(uint32_t p_voxel_key, bool p_any_hit) {
	(void)p_any_hit;
	gpu_clear_voxels.push_back(p_voxel_key);
}

void NextGIVoxelLighting::_rebuild_queue_index() {
	queued.clear();
	for (uint32_t index = 0; index < queue.size(); index++) {
		queued.insert(_brick_key(queue[index].level, queue[index].brick), queue[index].fill);
	}
}

void NextGIVoxelLighting::select_brick_instances(uint32_t p_level, const Vector3i &p_brick, const TraceInstance *p_instances, uint32_t p_count, LocalVector<uint32_t> &r_indices, uint32_t &r_overlapping) const {
	struct Candidate {
		float distance = 0.0f;
		uint32_t index = 0;
		bool operator<(const Candidate &p_other) const {
			return distance < p_other.distance || (distance == p_other.distance && index < p_other.index);
		}
	};
	r_indices.clear();
	r_overlapping = 0;
	if (p_instances == nullptr || p_level >= LEVELS) {
		return;
	}
	const AABB brick_aabb = _brick_aabb(p_level, p_brick);
	const Vector3 center = brick_aabb.get_center();
	LocalVector<Candidate> overlapping;
	for (uint32_t index = 0; index < p_count; index++) {
		const TraceInstance &instance = p_instances[index];
		if (!_voxel_field_ok(instance) || !instance.world_aabb.intersects(brick_aabb)) {
			continue;
		}
		const Vector3 low = instance.world_aabb.position;
		const Vector3 high = low + instance.world_aabb.size;
		const Vector3 closest(CLAMP(center.x, low.x, high.x), CLAMP(center.y, low.y, high.y), CLAMP(center.z, low.z, high.z));
		Candidate candidate;
		candidate.distance = center.distance_squared_to(closest);
		candidate.index = index;
		overlapping.push_back(candidate);
	}
	r_overlapping = overlapping.size();
	// The cap keeps the instances nearest the brick: a dropped far one cannot hide a kept near one.
	overlapping.sort();
	const uint32_t kept = MIN(overlapping.size(), MAX_TRACE_INSTANCES);
	r_indices.resize(kept);
	for (uint32_t index = 0; index < kept; index++) {
		r_indices[index] = overlapping[index].index;
	}
}

void NextGIVoxelLighting::_trace_brick(const DirtyBrick &p_brick, const TraceInstance *p_instances, uint32_t p_count, uint32_t &r_overlapping) {
	Vector3i voxels[SDF_BRICK_VOXELS * SDF_BRICK_VOXELS * SDF_BRICK_VOXELS];
	const uint32_t voxel_count = voxels_for_sdf_brick(p_brick.brick, voxels, SDF_BRICK_VOXELS * SDF_BRICK_VOXELS * SDF_BRICK_VOXELS);
	LocalVector<uint32_t> kept;
	select_brick_instances(p_brick.level, p_brick.brick, p_instances, p_count, kept, r_overlapping);
	LocalVector<TraceInstance> traced;
	traced.resize(kept.size());
	for (uint32_t index = 0; index < kept.size(); index++) {
		traced[index] = p_instances[kept[index]];
	}
	const float max_distance = float(TRACE_REACH_VOXELS) * level_voxel_size(p_brick.level);
	for (uint32_t voxel_i = 0; voxel_i < voxel_count; voxel_i++) {
		const Vector3i physical = _physical_voxel(p_brick.level, voxels[voxel_i]);
		const uint32_t voxel = voxel_index(physical);
		const uint32_t voxel_key = p_brick.level * VOXELS_PER_LEVEL + voxel;
		const Vector3 center = voxel_center(p_brick.level, origins[p_brick.level], voxels[voxel_i]);
		AxisHit slots[DIRECTIONS];
		const uint32_t filled = trace_voxel(traced.ptr(), traced.size(), center, max_distance, slots);
		retrace_rays += DIRECTIONS;
		for (uint32_t direction = 0; direction < DIRECTIONS; direction++) {
			const uint32_t sample_index = direction_index(p_brick.level, voxel, direction);
			hits.erase(sample_index);
			if (slots[direction].hit) {
				hits.insert(sample_index, slots[direction]);
			}
		}
		_note_voxel(voxel_key, filled > 0);
	}
}

uint32_t NextGIVoxelLighting::retrace(const TraceInstance *p_instances, uint32_t p_count) {
	retrace_rays = 0;
	bool one_brick = false;
	uint32_t bricks_done = 0;
	const uint32_t brick_rays = SDF_BRICK_VOXELS * SDF_BRICK_VOXELS * SDF_BRICK_VOXELS * DIRECTIONS;
	while (queue.size() > 0) {
		if (bricks_done > 0 && (retrace_rays + brick_rays > RAYS_PER_FRAME || one_brick)) {
			break;
		}
		const DirtyBrick brick = queue[0];
		queue.remove_at(0);
		if (fill_begin > 0) {
			fill_begin--;
		}
		queued.erase(_brick_key(brick.level, brick.brick));
		if (brick.level >= LEVELS || !origin_set[brick.level]) {
			continue;
		}
		uint32_t overlapping = 0;
		_trace_brick(brick, p_instances, p_count, overlapping);
		if (overlapping > 16) {
			one_brick = true;
		}
		bricks_done++;
	}
	return retrace_rays;
}

bool NextGIVoxelLighting::get_stored_hit(uint32_t p_level, const Vector3i &p_local, uint32_t p_direction, AxisHit &r_hit) const {
	if (p_level >= LEVELS || p_direction >= DIRECTIONS || !origin_set[p_level]) {
		return false;
	}
	const uint32_t voxel = voxel_index(_physical_voxel(p_level, p_local));
	const AxisHit *hit = hits.getptr(direction_index(p_level, voxel, p_direction));
	if (hit == nullptr) {
		return false;
	}
	r_hit = *hit;
	return true;
}

void NextGIVoxelLighting::shade(const TraceInstance *p_instances, uint32_t p_count) {
	shaded_hits = 0;
	unpaged_hits = 0;
	gpu_shade_words.clear();
	HashMap<uint32_t, const TraceInstance *> by_id;
	if (p_instances != nullptr) {
		for (uint32_t index = 0; index < p_count; index++) {
			by_id.insert(p_instances[index].id, &p_instances[index]);
		}
	}
	gpu_shade_words.reserve(hits.size() * GPU_SHADE_STRIDE);
	for (const KeyValue<uint32_t, AxisHit> &entry : hits) {
		const TraceInstance *const *found = by_id.getptr(entry.value.instance);
		const TraceInstance *instance = found != nullptr ? *found : nullptr;
		// A hit whose instance left the feed still shades, to weight 0.
		if (instance == nullptr || instance->card_set == UINT32_MAX || instance->page_table == EMPTY_VISIBILITY || (instance->page_table & PAGE_EVICTED_BIT) != 0) {
			unpaged_hits++;
		}
		gpu_shade_words.push_back(entry.key);
		gpu_shade_words.push_back(instance != nullptr ? instance->page_table : EMPTY_VISIBILITY);
		gpu_shade_words.push_back(instance != nullptr ? instance->card_set : UINT32_MAX);
		uint32_t bits[3];
		const float position[3] = { entry.value.object_position.x, entry.value.object_position.y, entry.value.object_position.z };
		memcpy(bits, position, sizeof(bits));
		gpu_shade_words.push_back(bits[0]);
		gpu_shade_words.push_back(bits[1]);
		gpu_shade_words.push_back(bits[2]);
		gpu_shade_words.push_back(pack_normal(entry.value.object_normal));
		shaded_hits++;
	}
}

float NextGIVoxelLighting::get_coverage() const {
	if (covered_hits < 0 || gpu_shaded_hits < 0) {
		return -1.0f;
	}
	if (gpu_shaded_hits == 0) {
		return 0.0f;
	}
	return float(covered_hits) / float(gpu_shaded_hits);
}

int64_t NextGIVoxelLighting::get_coverage_ppm() const {
	if (covered_hits < 0 || gpu_shaded_hits < 0) {
		return -1;
	}
	if (gpu_shaded_hits == 0) {
		return 0;
	}
	const double ppm = (double(covered_hits) * 1000000.0) / double(gpu_shaded_hits);
	return CLAMP(int64_t(ppm + 0.5), int64_t(0), int64_t(1000000));
}

void NextGIVoxelLighting::copy_clipmap(ClipmapLevelGPU *r_levels, uint32_t p_count) const {
	if (r_levels == nullptr) {
		return;
	}
	const uint32_t count = MIN(p_count, LEVELS);
	for (uint32_t level = 0; level < count; level++) {
		r_levels[level].origin_x = origins[level].x;
		r_levels[level].origin_y = origins[level].y;
		r_levels[level].origin_z = origins[level].z;
		const float size = level_voxel_size(level);
		uint32_t bits = 0;
		memcpy(&bits, &size, sizeof(bits));
		r_levels[level].voxel_size_bits = bits;
	}
}

RID NextGIVoxelLighting::get_radiance_buffer() const {
	return gpu != nullptr ? gpu->radiance : RID();
}

RID NextGIVoxelLighting::get_weight_buffer() const {
	return gpu != nullptr ? gpu->weights : RID();
}

void NextGIVoxelLighting::fill_info(Dictionary &r_info) const {
	r_info["voxel_lighting_retrace"] = int64_t(retrace_rays);
	r_info["voxel_lighting_coverage"] = get_coverage();
	r_info["voxel_lighting_coverage_ppm"] = get_coverage_ppm();
	r_info["voxel_lighting_volume_bytes"] = int64_t(allocated_bytes);
	r_info["voxel_lighting_stored_hits"] = int64_t(hits.size());
	r_info["voxel_lighting_shaded_hits"] = int64_t(shaded_hits);
	r_info["voxel_lighting_unpaged_hits"] = int64_t(unpaged_hits);
	r_info["voxel_lighting_resampled_nonzero"] = resampled_nonzero;
	r_info["voxel_lighting_readbacks"] = int64_t(debug_readbacks);
	if (debug_probe_results.size() == debug_probe_positions.size() && !debug_probe_results.is_empty()) {
		PackedVector3Array positions;
		PackedVector3Array normals;
		PackedColorArray results;
		positions.resize(debug_probe_positions.size());
		normals.resize(debug_probe_positions.size());
		results.resize(debug_probe_positions.size());
		for (uint32_t index = 0; index < debug_probe_positions.size(); index++) {
			positions.set(index, debug_probe_positions[index]);
			normals.set(index, debug_probe_normals[index]);
			results.set(index, debug_probe_results[index]);
		}
		r_info["voxel_lighting_probe_positions"] = positions;
		r_info["voxel_lighting_probe_normals"] = normals;
		r_info["voxel_lighting_probe_radiance"] = results;
	}
}

void NextGIVoxelLighting::select_debug_probes(const HashMap<uint32_t, AxisHit> &p_hits, uint32_t p_max, LocalVector<uint32_t> &r_keys) {
	r_keys.clear();
	if (p_hits.is_empty() || p_max == 0) {
		return;
	}
	// Whole voxels, picked by a hash of the voxel key. A voxel's slots are stored in direction
	// order, so every n-th stored hit would keep one direction of each voxel with n hits.
	uint32_t stride = (p_hits.size() + p_max - 1) / p_max;
	for (uint32_t attempt = 0; attempt < 8; attempt++) {
		r_keys.clear();
		for (const KeyValue<uint32_t, AxisHit> &entry : p_hits) {
			if (stride > 1 && hash_fmix32(entry.key / DIRECTIONS) % stride != 0) {
				continue;
			}
			r_keys.push_back(entry.key);
		}
		if (r_keys.size() <= p_max) {
			return;
		}
		stride++;
	}
	r_keys.resize(p_max);
}

void NextGIVoxelLighting::_collect_debug_probes() {
	debug_probe_positions.clear();
	debug_probe_normals.clear();
	debug_probe_results.clear();
	LocalVector<uint32_t> keys;
	select_debug_probes(hits, MAX_DEBUG_PROBES, keys);
	for (uint32_t key : keys) {
		const AxisHit &hit = hits[key];
		debug_probe_positions.push_back(hit.position);
		debug_probe_normals.push_back(hit.normal);
	}
}

bool NextGIVoxelLighting::_upload_and_dispatch(uint32_t p_mode, const uint32_t *p_words, uint32_t p_count, uint32_t p_stride, const ShadeBindings &p_bindings, uint32_t p_flags, uint32_t p_header_words) {
	if (gpu == nullptr || p_words == nullptr || p_count == 0 || p_stride == 0 || !gpu->staging.is_valid() || !gpu->pipeline.is_valid() || !gpu->uniform_set.is_valid()) {
		return false;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr) {
		return false;
	}
	const uint32_t payload = uint32_t(staging_buffer_bytes() / sizeof(uint32_t)) - 1u;
	const uint32_t batch = payload / p_stride;
	// A header (the probe clipmap) precedes the items, so that payload has to fit one batch.
	if (batch == 0 || (p_header_words > 0 && p_header_words + uint64_t(p_count) * p_stride > payload)) {
		return false;
	}
	struct Params {
		uint32_t mode;
		uint32_t count;
		uint32_t page_rect_count;
		uint32_t flags;
		uint32_t card_set_count;
		uint32_t card_count;
		uint32_t pad[2];
	};
	static_assert(sizeof(Params) == 32, "Voxel lighting push constants must be 32 bytes.");
	for (uint32_t offset = 0; offset < p_count; offset += batch) {
		const uint32_t items = MIN(batch, p_count - offset);
		const uint32_t bytes = (p_header_words + items * p_stride) * uint32_t(sizeof(uint32_t));
		const Error err = rd->buffer_update(gpu->staging, sizeof(uint32_t), bytes, p_words + uint64_t(offset) * p_stride);
		if (err != OK) {
			return false;
		}
		Params params = {};
		params.mode = p_mode;
		params.count = items;
		params.page_rect_count = p_bindings.page_rect_count;
		params.flags = p_flags;
		params.card_set_count = p_bindings.card_set_count;
		params.card_count = p_bindings.card_count;
		const RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, gpu->pipeline);
		rd->compute_list_bind_uniform_set(list, gpu->uniform_set, 0);
		rd->compute_list_set_push_constant(list, &params, sizeof(params));
		const uint32_t groups = (items + 63u) / 64u;
		const uint32_t groups_x = MIN(groups, 256u);
		const uint32_t groups_y = MAX((groups + groups_x - 1u) / groups_x, 1u);
		rd->compute_list_dispatch(list, groups_x, groups_y, 1);
		rd->compute_list_end();
	}
	return true;
}

bool NextGIVoxelLighting::_dispatch_gpu(const ShadeBindings &p_bindings, bool p_readback) {
	if (gpu_clear_voxels.is_empty() && gpu_shade_words.is_empty()) {
		return false;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr) {
		return false;
	}
	if (gpu == nullptr) {
		gpu = memnew(GPUState);
	}
	if (gpu->failed) {
		return false;
	}
	if (!gpu->pipeline.is_valid()) {
		gpu->shader.initialize(Vector<String>({ "" }));
		gpu->version = gpu->shader.version_create();
		if (!gpu->version.is_valid()) {
			gpu->failed = true;
			WARN_PRINT("NextGI voxel lighting shader failed to compile; global-SDF hits fall back to the WRC.");
			return false;
		}
		gpu->pipeline = rd->compute_pipeline_create(gpu->shader.version_get_shader(gpu->version, 0));
		if (!gpu->pipeline.is_valid()) {
			gpu->shader.version_free(gpu->version);
			gpu->version = RID();
			gpu->failed = true;
			WARN_PRINT("NextGI voxel lighting pipeline failed to compile; global-SDF hits fall back to the WRC.");
			return false;
		}
	}
	if (!gpu->radiance.is_valid()) {
		gpu->radiance = rd->storage_buffer_create(uint32_t(radiance_buffer_bytes()));
		gpu->weights = rd->storage_buffer_create(uint32_t(weight_buffer_bytes()));
		gpu->staging = rd->storage_buffer_create(uint32_t(staging_buffer_bytes()));
		// Stands in for the page rects, cards, card sets and debug buffer: one card record.
		gpu->dummy = rd->storage_buffer_create(sizeof(RendererRD::NextGICard));
		if (!gpu->radiance.is_valid() || !gpu->weights.is_valid() || !gpu->staging.is_valid() || !gpu->dummy.is_valid()) {
			gpu->failed = true;
			WARN_PRINT("NextGI voxel lighting volumes failed to allocate; global-SDF hits fall back to the WRC.");
			return false;
		}
		rd->set_resource_name(gpu->radiance, "NextGI Voxel Radiance");
		rd->set_resource_name(gpu->weights, "NextGI Voxel Weights");
		rd->set_resource_name(gpu->staging, "NextGI Voxel Staging");
		allocated_bytes = allocated_volume_bytes();
		gpu->volumes_cleared = false;
	}
	// The debug buffer exists only while the readback setting is on.
	if (p_readback && !gpu->debug.is_valid()) {
		gpu->debug = rd->storage_buffer_create(uint32_t(debug_buffer_bytes()));
		if (gpu->debug.is_valid()) {
			rd->set_resource_name(gpu->debug, "NextGI Voxel Debug");
		}
	} else if (!p_readback && gpu->debug.is_valid()) {
		if (gpu->uniform_set.is_valid()) {
			rd->uniform_set_set_invalidation_callback(gpu->uniform_set, nullptr, nullptr);
			rd->free_rid(gpu->uniform_set);
			gpu->uniform_set = RID();
		}
		rd->free_rid(gpu->debug);
		gpu->debug = RID();
	}
	const bool readback = p_readback && gpu->debug.is_valid();
	if (!gpu->fallback_texture.is_valid()) {
		RD::TextureFormat format;
		format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
		format.width = 1;
		format.height = 1;
		format.texture_type = RD::TEXTURE_TYPE_2D;
		format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		gpu->fallback_texture = rd->texture_create(format, RD::TextureView());
		if (gpu->fallback_texture.is_valid()) {
			Vector<uint8_t> pixels;
			pixels.resize(4);
			pixels.fill(0);
			rd->texture_update(gpu->fallback_texture, 0, pixels);
			rd->set_resource_name(gpu->fallback_texture, "NextGI Voxel Lighting Fallback");
		}
	}
	if (!gpu->sampler.is_valid()) {
		RD::SamplerState state;
		state.mag_filter = RD::SAMPLER_FILTER_NEAREST;
		state.min_filter = RD::SAMPLER_FILTER_NEAREST;
		state.mip_filter = RD::SAMPLER_FILTER_NEAREST;
		state.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		state.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
		gpu->sampler = rd->sampler_create(state);
	}
	if (!gpu->fallback_texture.is_valid() || !gpu->sampler.is_valid()) {
		return false;
	}
	// Missing SRC or relight resources bind stand-ins, and the counts keep the shade from
	// reading them: every hit then shades to weight 0.
	ShadeBindings bindings = p_bindings;
	const bool src_ok = bindings.cards.is_valid() && bindings.card_sets.is_valid() && bindings.material_depth.is_valid();
	if (!src_ok) {
		bindings.card_count = 0;
		bindings.card_set_count = 0;
	}
	if (!bindings.page_rects.is_valid() || !bindings.radiance_atlas.is_valid()) {
		bindings.page_rect_count = 0;
	}
	RID bound[GPUState::BOUND_MAX];
	bound[GPUState::BOUND_ATLAS] = bindings.radiance_atlas.is_valid() ? bindings.radiance_atlas : gpu->fallback_texture;
	bound[GPUState::BOUND_PAGES] = bindings.page_rects.is_valid() ? bindings.page_rects : gpu->dummy;
	bound[GPUState::BOUND_MATERIAL_DEPTH] = src_ok ? bindings.material_depth : gpu->fallback_texture;
	bound[GPUState::BOUND_CARDS] = src_ok ? bindings.cards : gpu->dummy;
	bound[GPUState::BOUND_CARD_SETS] = src_ok ? bindings.card_sets : gpu->dummy;
	bound[GPUState::BOUND_DEBUG] = readback ? gpu->debug : gpu->dummy;
	bool rebind = !gpu->uniform_set.is_valid();
	for (uint32_t index = 0; index < GPUState::BOUND_MAX; index++) {
		rebind = rebind || bound[index] != gpu->bound[index];
	}
	if (rebind) {
		if (gpu->uniform_set.is_valid()) {
			rd->uniform_set_set_invalidation_callback(gpu->uniform_set, nullptr, nullptr);
			rd->free_rid(gpu->uniform_set);
			gpu->uniform_set = RID();
		}
		Vector<RD::Uniform> uniforms;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, gpu->radiance));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, gpu->weights));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, gpu->staging));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, bound[GPUState::BOUND_PAGES]));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ gpu->sampler, bound[GPUState::BOUND_ATLAS] })));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ gpu->sampler, bound[GPUState::BOUND_MATERIAL_DEPTH] })));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, bound[GPUState::BOUND_CARDS]));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, bound[GPUState::BOUND_CARD_SETS]));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, bound[GPUState::BOUND_DEBUG]));
		const RID shader_rid = gpu->shader.version_get_shader(gpu->version, 0);
		gpu->uniform_set = rd->uniform_set_create(uniforms, shader_rid, 0);
		if (!gpu->uniform_set.is_valid()) {
			return false;
		}
		rd->uniform_set_set_invalidation_callback(gpu->uniform_set, _uniform_set_invalidated, gpu);
		for (uint32_t index = 0; index < GPUState::BOUND_MAX; index++) {
			gpu->bound[index] = bound[index];
		}
	}
	if (!gpu->volumes_cleared) {
		const uint32_t payload = uint32_t(staging_buffer_bytes() / sizeof(uint32_t)) - 1u;
		LocalVector<uint32_t> keys;
		keys.resize(payload);
		for (uint32_t base = 0; base < VOXEL_COUNT; base += payload) {
			const uint32_t count = MIN(payload, VOXEL_COUNT - base);
			for (uint32_t index = 0; index < count; index++) {
				keys[index] = base + index;
			}
			if (!_upload_and_dispatch(GPU_MODE_CLEAR, keys.ptr(), count, 1, bindings, 0)) {
				return false;
			}
		}
		gpu->volumes_cleared = true;
	}
	if (gpu_clear_voxels.size() > 0) {
		HashSet<uint32_t> seen;
		LocalVector<uint32_t> unique;
		for (uint32_t voxel : gpu_clear_voxels) {
			if (seen.has(voxel)) {
				continue;
			}
			seen.insert(voxel);
			unique.push_back(voxel);
		}
		if (!_upload_and_dispatch(GPU_MODE_CLEAR, unique.ptr(), unique.size(), 1, bindings, 0)) {
			return false;
		}
	}
	if (readback) {
		const uint32_t zero[4] = {};
		if (rd->buffer_update(gpu->debug, 0, sizeof(zero), zero) != OK) {
			return false;
		}
	}
	const uint32_t shade_count = gpu_shade_words.size() / GPU_SHADE_STRIDE;
	if (shade_count > 0 && !_upload_and_dispatch(GPU_MODE_SHADE, gpu_shade_words.ptr(), shade_count, GPU_SHADE_STRIDE, bindings, readback ? GPU_FLAG_COUNT : 0)) {
		return false;
	}
	if (!readback) {
		return true;
	}
	// Debug only: probe the lookup at stored hits, then read the counters and probes back.
	const uint32_t probe_count = debug_probe_positions.size();
	if (probe_count > 0) {
		LocalVector<uint32_t> words;
		words.resize(GPU_PROBE_HEADER + probe_count * GPU_PROBE_STRIDE);
		ClipmapLevelGPU clipmap[LEVELS];
		copy_clipmap(clipmap, LEVELS);
		memcpy(words.ptr(), clipmap, sizeof(clipmap));
		for (uint32_t index = 0; index < probe_count; index++) {
			const float position[3] = { debug_probe_positions[index].x, debug_probe_positions[index].y, debug_probe_positions[index].z };
			uint32_t *probe = words.ptr() + GPU_PROBE_HEADER + index * GPU_PROBE_STRIDE;
			memcpy(probe, position, sizeof(position));
			probe[3] = pack_normal(debug_probe_normals[index]);
		}
		if (!_upload_and_dispatch(GPU_MODE_PROBE, words.ptr(), probe_count, GPU_PROBE_STRIDE, bindings, 0, GPU_PROBE_HEADER)) {
			return false;
		}
	}
	const Vector<uint8_t> data = rd->buffer_get_data(gpu->debug, 0, uint32_t(16u + probe_count * 16u));
	debug_readbacks++;
	if (data.size() < int(16u + probe_count * 16u)) {
		return false;
	}
	uint32_t counters[4];
	memcpy(counters, data.ptr(), sizeof(counters));
	resampled_nonzero = counters[0] != 0 ? 1 : 0;
	covered_hits = int64_t(counters[1]);
	gpu_shaded_hits = int64_t(counters[2]);
	debug_probe_results.resize(probe_count);
	for (uint32_t index = 0; index < probe_count; index++) {
		float values[4];
		memcpy(values, data.ptr() + 16u + index * 16u, sizeof(values));
		debug_probe_results[index] = Color(values[0], values[1], values[2], values[3]);
	}
	return true;
}

void NextGIVoxelLighting::update_render(RenderDataRD *p_render_data, RendererRD::NextGIRelight *p_relight, RendererRD::NextGISRC *p_src) {
	// Cached: re-read only when a project setting changes, so a runtime toggle still applies.
	const bool readback = GLOBAL_GET_CACHED(bool, VOXEL_LIGHTING_READBACK);
	covered_hits = -1;
	gpu_shaded_hits = -1;
	resampled_nonzero = -1;
	gpu_clear_voxels.clear();
	gpu_shade_words.clear();
	debug_probe_positions.clear();
	debug_probe_normals.clear();
	debug_probe_results.clear();
	const Vector3 camera = p_render_data != nullptr && p_render_data->scene_data != nullptr ? p_render_data->scene_data->cam_transform.origin : Vector3();
	set_clipmap_origins_from_camera(camera);
	const RendererSceneRender::RenderSDFGIUpdateData *update = p_render_data != nullptr ? p_render_data->sdfgi_update_data : nullptr;
	if (update != nullptr) {
		enqueue_dirty_volumes(update->nextgi_dirty_volumes, update->nextgi_dirty_volume_count);
	}
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	const bool refresh = mesh_storage != nullptr ? begin_frame(mesh_storage->get_sdf_atlas().get_generation()) : levels_to_fill != 0;
	if (!refresh && queue.size() == 0 && hits.size() == 0 && gpu_clear_voxels.size() == 0) {
		retrace_rays = 0;
		shaded_hits = 0;
		return;
	}

	// The scene cull always publishes the feed with the update data.
	LocalVector<RuntimeRef> candidates;
	const LocalVector<RendererSceneRender::NextGIRTInstance> *feed = update != nullptr ? update->nextgi_rt_instances : nullptr;
	if (feed != nullptr) {
		for (uint32_t index = 0; index < feed->size(); index++) {
			const RendererSceneRender::NextGIRTInstance &fed = (*feed)[index];
			if (fed.gpu_source.is_valid() || fed.geometry_instance == nullptr) {
				continue;
			}
			RenderGeometryInstanceBase *geometry = static_cast<RenderGeometryInstanceBase *>(fed.geometry_instance);
			if (geometry->data == nullptr) {
				continue;
			}
			const RID mesh = fed.mesh_override.is_valid() ? fed.mesh_override : geometry->data->base;
			if (!mesh.is_valid()) {
				continue;
			}
			const Transform3D transform = fed.has_rt_transform ? fed.rt_transform : geometry->transform;
			if (!(Math::abs(transform.basis.determinant()) > 1e-8f)) {
				continue;
			}
			RuntimeRef item;
			item.id = _stable_id(fed.geometry_instance, fed.element_index);
			item.transform = transform;
			item.inverse = transform.affine_inverse();
			item.mesh = mesh;
			item.geometry = fed.geometry_instance;
			item.element = fed.element_index;
			if (mesh_storage != nullptr) {
				const NextGISDFAtlas::MeshPages *pages = mesh_storage->get_sdf_atlas().get_pages(mesh);
				if (pages != nullptr && pages->field.is_valid()) {
					const AABB bounds = pages->field->get_bounds();
					if (bounds.size.x > 0.0f && bounds.size.y > 0.0f && bounds.size.z > 0.0f) {
						item.world_aabb = transform.xform(bounds);
						item.field = pages->field.ptr();
					}
				}
			}
			candidates.push_back(item);
		}
	}
	if (refresh) {
		LocalVector<InstanceBounds> bounds;
		bounds.resize(candidates.size());
		for (uint32_t index = 0; index < candidates.size(); index++) {
			bounds[index].mesh = candidates[index].mesh.get_id();
			bounds[index].world_aabb = candidates[index].world_aabb;
			bounds[index].field = candidates[index].field;
		}
		refresh_instances(bounds.ptr(), bounds.size());
	}

	const uint32_t brick_rays = SDF_BRICK_VOXELS * SDF_BRICK_VOXELS * SDF_BRICK_VOXELS * DIRECTIONS;
	LocalVector<DirtyBrick> planned;
	int first_index = -1;
	for (uint32_t index = 0; index < queue.size(); index++) {
		const DirtyBrick &brick = queue[index];
		if (brick.level < LEVELS && origin_set[brick.level]) {
			first_index = int(index);
			break;
		}
	}
	if (first_index >= 0) {
		const DirtyBrick &first = queue[first_index];
		const AABB first_aabb = _brick_aabb(first.level, first.brick);
		uint32_t overlap = 0;
		for (uint32_t index = 0; index < candidates.size(); index++) {
			if (candidates[index].field == nullptr) {
				continue;
			}
			if (candidates[index].world_aabb.intersects(first_aabb)) {
				overlap++;
			}
		}
		const bool one_brick = overlap > 16;
		planned.push_back(first);
		uint32_t rays = brick_rays;
		if (!one_brick) {
			for (uint32_t index = uint32_t(first_index) + 1; index < queue.size(); index++) {
				const DirtyBrick &brick = queue[index];
				if (brick.level >= LEVELS || !origin_set[brick.level]) {
					continue;
				}
				if (rays + brick_rays > RAYS_PER_FRAME) {
					break;
				}
				planned.push_back(brick);
				rays += brick_rays;
			}
		}
	}

	HashSet<uint32_t> hit_ids;
	for (const KeyValue<uint32_t, AxisHit> &entry : hits) {
		hit_ids.insert(entry.value.instance);
	}
	const uint32_t card_set_count = p_src != nullptr ? p_src->get_card_set_count() : 0;
	LocalVector<TraceInstance> traces;
	LocalVector<uint32_t> field_slots;
	for (uint32_t index = 0; index < candidates.size(); index++) {
		const RuntimeRef &candidate = candidates[index];
		bool overlaps = false;
		if (candidate.field != nullptr) {
			for (uint32_t brick_index = 0; brick_index < planned.size(); brick_index++) {
				if (candidate.world_aabb.intersects(_brick_aabb(planned[brick_index].level, planned[brick_index].brick))) {
					overlaps = true;
					break;
				}
			}
		}
		if (!overlaps && !hit_ids.has(candidate.id)) {
			continue;
		}
		TraceInstance trace;
		trace.id = candidate.id;
		trace.transform = candidate.transform;
		trace.inverse = candidate.inverse;
		trace.world_aabb = candidate.world_aabb;
		trace.page_table = p_relight != nullptr ? p_relight->get_instance_page_table(candidate.geometry, candidate.element) : EMPTY_VISIBILITY;
		const uint32_t card_set = p_src != nullptr ? RendererRD::NextGISRC::get_instance_card_set(candidate.geometry) : UINT32_MAX;
		trace.card_set = card_set < card_set_count ? card_set : UINT32_MAX;
		traces.push_back(trace);
		field_slots.push_back(overlaps ? index : UINT32_MAX);
	}
	for (uint32_t index = 0; index < traces.size(); index++) {
		if (field_slots[index] == UINT32_MAX) {
			continue;
		}
		traces[index].field = _cached_field(candidates[field_slots[index]].mesh);
	}
	if (queue.size() > 0) {
		retrace(traces.ptr(), traces.size());
	} else {
		retrace_rays = 0;
	}
	shade(traces.ptr(), traces.size());
	if (readback) {
		_collect_debug_probes();
	}
	if (gpu_clear_voxels.size() == 0 && gpu_shade_words.size() == 0) {
		return;
	}
	ShadeBindings bindings;
	if (p_relight != nullptr) {
		bindings.page_rects = p_relight->get_page_rect_buffer();
		bindings.page_rect_count = bindings.page_rects.is_valid() ? p_relight->get_page_rect_count() : 0;
		bindings.radiance_atlas = p_relight->get_radiance_texture();
	}
	if (p_src != nullptr) {
		bindings.material_depth = p_src->get_atlas_texture(RendererRD::NextGISRC::ATLAS_DEPTH);
		bindings.cards = p_src->get_card_buffer();
		bindings.card_sets = p_src->get_card_set_buffer();
		bindings.card_count = p_src->get_card_count();
		bindings.card_set_count = card_set_count;
	}
	_dispatch_gpu(bindings, readback);
}
