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

#include "core/math/math_funcs.h"
#include "core/string/print_string.h"
#include "servers/rendering/renderer_geometry_instance.h"
#include "servers/rendering/renderer_rd/environment/next_gi_global_sdf.h"
#include "servers/rendering/renderer_rd/environment/next_gi_relight.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_scene.h"
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
static constexpr uint32_t GPU_MODE_SHADE = 0;
static constexpr uint32_t GPU_MODE_CLEAR = 1;
static constexpr uint32_t GPU_SHADE_STRIDE = 4;

struct NextGIVoxelLighting::GPUState {
	NextgiVoxelLightingShaderRD shader;
	RID version;
	RID pipeline;
	RID radiance;
	RID weights;
	RID staging;
	RID page_dummy;
	RID fallback_texture;
	RID sampler;
	RID uniform_set;
	RID bound_atlas;
	RID bound_pages;
	uint32_t page_rect_count = 0;
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

static float _sample_instance(const NextGIVoxelLighting::TraceInstance &p_instance, const Vector3 &p_world) {
	if (p_instance.field == nullptr || !p_instance.field->is_valid() || !(p_instance.world_scale > 0.0f)) {
		return VOXEL_LIGHTING_HUGE;
	}
	return p_instance.field->sample_distance(p_instance.inverse.xform(p_world)) * p_instance.world_scale;
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
		const Vector3 relative = p_position - card.origin;
		const float u = card.axis_u.dot(relative);
		const float v = card.axis_v.dot(relative);
		if (Math::abs(u) > card.half_u || Math::abs(v) > card.half_v) {
			continue;
		}
		// Plane distance stands in for the material-depth atlas read on the CPU.
		const float plane = Math::abs(card.axis_n.dot(relative));
		if (plane > CARD_DEPTH_TEXELS * texel) {
			continue;
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
		best.depth_error = plane / texel;
		const float u_norm = card.half_u > 0.0f ? u / card.half_u : 0.0f;
		const float v_norm = card.half_v > 0.0f ? v / card.half_v : 0.0f;
		best.u = CLAMP(u_norm * 0.5f + 0.5f, 0.0f, 1.0f);
		best.v = CLAMP(v_norm * 0.5f + 0.5f, 0.0f, 1.0f);
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
	// Direction d stores the surface found by tracing along +d, so it faces -d.
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

NextGIVoxelLighting::AxisHit NextGIVoxelLighting::trace_axis(const TraceInstance *p_instances, uint32_t p_count, const Vector3 &p_origin, const Vector3 &p_direction, float p_max_distance) {
	AxisHit best;
	if (p_instances == nullptr || p_count == 0 || !(p_max_distance > 0.0f) || p_direction.length_squared() <= 1e-12f) {
		return best;
	}
	const Vector3 direction = p_direction.normalized();
	for (uint32_t index = 0; index < p_count; index++) {
		const TraceInstance &instance = p_instances[index];
		if (instance.field == nullptr || !instance.field->is_valid()) {
			continue;
		}
		const float field_step = instance.field->voxel_size * MAX(instance.world_scale, 0.0f);
		const float eps = MAX(0.02f, field_step * 0.5f);
		const float min_step = eps * 0.5f;
		float traveled = 0.0f;
		float previous = _sample_instance(instance, p_origin);
		if (previous >= VOXEL_LIGHTING_HUGE) {
			continue;
		}
		bool found = false;
		float hit_distance = 0.0f;
		if (Math::abs(previous) <= eps) {
			found = true;
			hit_distance = 0.0f;
		}
		for (uint32_t step = 0; step < MAX_MARCH_STEPS && !found; step++) {
			const float advance = MAX(Math::abs(previous), min_step);
			const float next_distance = traveled + advance;
			if (next_distance > p_max_distance) {
				break;
			}
			const float next = _sample_instance(instance, p_origin + direction * next_distance);
			if (next >= VOXEL_LIGHTING_HUGE) {
				break;
			}
			if ((previous > 0.0f && next <= 0.0f) || (previous < 0.0f && next >= 0.0f) || Math::abs(next) <= eps) {
				found = true;
				hit_distance = next_distance;
				break;
			}
			traveled = next_distance;
			previous = next;
		}
		if (!found) {
			continue;
		}
		AxisHit hit;
		hit.hit = true;
		hit.distance = hit_distance;
		hit.position = p_origin + direction * hit_distance;
		hit.instance = instance.id;
		const float tap_step = MAX(eps, 0.02f);
		const Vector3 tap_axes[6] = {
			Vector3(1, 0, 0),
			Vector3(-1, 0, 0),
			Vector3(0, 1, 0),
			Vector3(0, -1, 0),
			Vector3(0, 0, 1),
			Vector3(0, 0, -1),
		};
		float taps[6];
		for (uint32_t tap = 0; tap < 6; tap++) {
			taps[tap] = _sample_instance(instance, hit.position + tap_axes[tap] * tap_step);
		}
		const Vector3 gradient = gradient_from_taps(taps, tap_step);
		hit.normal = gradient.length_squared() > 1e-8f ? gradient.normalized() : -direction;
		const CardChoice choice = choose_card(instance.cards, instance.card_count, hit.position, hit.normal);
		hit.card = choice.card;
		hit.missing = choice.missing ? 1u : 0u;
		hit.depth_error = choice.depth_error;
		hit.projection = choice.projection;
		hit.card_u = choice.u;
		hit.card_v = choice.v;
		hit.hit_t = quantize_hit_t(hit.distance, p_max_distance);
		hit.packed = pack_visibility(instance.id, hit.card, hit.hit_t);
		if (!best.hit || hit.packed < best.packed) {
			best = hit;
		}
	}
	return best;
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
	state->bound_atlas = RID();
	state->bound_pages = RID();
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
		if (gpu->radiance.is_valid()) {
			rd->free_rid(gpu->radiance);
		}
		if (gpu->weights.is_valid()) {
			rd->free_rid(gpu->weights);
		}
		if (gpu->staging.is_valid()) {
			rd->free_rid(gpu->staging);
		}
		if (gpu->page_dummy.is_valid()) {
			rd->free_rid(gpu->page_dummy);
		}
		if (gpu->fallback_texture.is_valid()) {
			rd->free_rid(gpu->fallback_texture);
		}
		if (gpu->sampler.is_valid()) {
			rd->free_rid(gpu->sampler);
		}
	}
	memdelete(gpu);
	gpu = nullptr;
	allocated_bytes = 0;
}

void NextGIVoxelLighting::reset() {
	hits.clear();
	queue.clear();
	queued.clear();
	for (uint32_t level = 0; level < LEVELS; level++) {
		origins[level] = Vector3i();
		origin_set[level] = false;
	}
	retrace_rays = 0;
	shaded_hits = 0;
	covered_hits = 0;
	resampled_nonzero = 0;
	field_cache.clear();
	field_generation = UINT64_MAX;
	stable_ids.clear();
	next_stable_id = 1;
	gpu_clear_voxels.clear();
	gpu_shade_words.clear();
	shaded_voxels.clear();
	shade_page_rect_count = 0;
	shade_reject_pages = false;
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

void NextGIVoxelLighting::_fill_cards(RendererRD::NextGISRC *p_src, RenderGeometryInstance *p_geometry, const RID &p_mesh, const Transform3D &p_transform, float p_world_scale, TraceInstance &r_trace) const {
	r_trace.card_count = 0;
	(void)p_mesh;
	if (p_src == nullptr || p_geometry == nullptr) {
		return;
	}
	const uint32_t set_index = RendererRD::NextGISRC::get_instance_card_set(p_geometry);
	RendererRD::NextGICardSet card_set;
	if (set_index == UINT32_MAX || !p_src->copy_card_set(set_index, card_set) || card_set.card_count == 0) {
		return;
	}
	const uint32_t count = MIN(card_set.card_count, MAX_CARDS);
	for (uint32_t card_index = 0; card_index < count; card_index++) {
		RendererRD::NextGICard card;
		if (!p_src->copy_card(card_set.first_card + card_index, card)) {
			break;
		}
		CardQuery &query = r_trace.cards[r_trace.card_count];
		Vector3 axis_n(card.axis_n[0], card.axis_n[1], card.axis_n[2]);
		Vector3 axis_u(card.axis_u[0], card.axis_u[1], card.axis_u[2]);
		Vector3 axis_v(card.axis_v[0], card.axis_v[1], card.axis_v[2]);
		axis_n = p_transform.basis.xform(axis_n);
		axis_u = p_transform.basis.xform(axis_u);
		axis_v = p_transform.basis.xform(axis_v);
		if (axis_n.length_squared() > 1e-8f) {
			axis_n = axis_n.normalized();
		}
		if (axis_u.length_squared() > 1e-8f) {
			axis_u = axis_u.normalized();
		}
		if (axis_v.length_squared() > 1e-8f) {
			axis_v = axis_v.normalized();
		}
		query.axis_n = axis_n;
		query.axis_u = axis_u;
		query.axis_v = axis_v;
		query.origin = p_transform.xform(Vector3(card.origin[0], card.origin[1], card.origin[2]));
		query.half_u = card.axis_u[3] * p_world_scale;
		query.half_v = card.axis_v[3] * p_world_scale;
		query.texel_size = card_set.texel_size * p_world_scale;
		r_trace.card_count++;
	}
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
			origins[level] = p_origins[level];
			origin_set[level] = true;
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

void NextGIVoxelLighting::_enqueue_brick(uint32_t p_level, const Vector3i &p_brick) {
	if (p_level >= LEVELS) {
		return;
	}
	const int page = int(NextGIGlobalSDF::PAGE_SIZE);
	if (p_brick.x < 0 || p_brick.y < 0 || p_brick.z < 0 || p_brick.x >= page || p_brick.y >= page || p_brick.z >= page) {
		return;
	}
	const uint32_t key = _brick_key(p_level, p_brick);
	if (queued.has(key)) {
		return;
	}
	queued.insert(key);
	DirtyBrick item;
	item.level = p_level;
	item.brick = p_brick;
	queue.push_back(item);
}

void NextGIVoxelLighting::_enqueue_aabb(const AABB &p_aabb) {
	if (p_aabb.size.x < 0.0f || p_aabb.size.y < 0.0f || p_aabb.size.z < 0.0f) {
		return;
	}
	for (uint32_t level = 0; level < LEVELS; level++) {
		if (!origin_set[level]) {
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
					_enqueue_brick(level, Vector3i(x, y, z));
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
	LocalVector<uint32_t> keys;
	for (const KeyValue<uint32_t, StoredHit> &entry : hits) {
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
}

void NextGIVoxelLighting::_scroll_level(uint32_t p_level, const Vector3i &p_new_origin) {
	const Vector3i delta = NextGIGlobalSDF::scroll_brick_delta(origins[p_level], p_new_origin);
	const int page = int(NextGIGlobalSDF::PAGE_SIZE);
	LocalVector<DirtyBrick> kept;
	HashSet<uint32_t> seen;
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
		seen.insert(key);
		kept.push_back(item);
	}
	queue = kept;
	queued = seen;
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
		queued.insert(_brick_key(queue[index].level, queue[index].brick));
	}
}

void NextGIVoxelLighting::_apply_stored_hit(StoredHit &r_hit, const TraceInstance *p_instance) {
	uint32_t packed_instance = 0;
	uint32_t packed_card = 0;
	uint32_t hit_t = 0;
	unpack_visibility(r_hit.packed, packed_instance, packed_card, hit_t);
	if (p_instance == nullptr) {
		r_hit.weight = 0.0f;
		r_hit.missing = 1;
		r_hit.page_table = EMPTY_VISIBILITY;
		r_hit.cpu_radiance = Color();
		return;
	}
	const CardChoice choice = choose_card(p_instance->cards, p_instance->card_count, r_hit.position, r_hit.normal);
	r_hit.card = choice.card;
	r_hit.card_u = choice.u;
	r_hit.card_v = choice.v;
	r_hit.projection = choice.projection;
	r_hit.depth_error = choice.depth_error;
	r_hit.missing = choice.missing ? 1u : 0u;
	r_hit.page_table = p_instance->page_table;
	float weight = shade_weight(choice.missing, choice.projection, choice.depth_error);
	const bool evicted = (p_instance->page_table & PAGE_EVICTED_BIT) != 0;
	const bool no_page = p_instance->page_table == EMPTY_VISIBILITY || evicted || choice.card >= MAX_CARDS;
	bool out_of_range = false;
	if (shade_reject_pages && !no_page) {
		const uint64_t page_index = uint64_t(p_instance->page_table) + choice.card;
		if (page_index >= shade_page_rect_count) {
			out_of_range = true;
		}
	}
	if (no_page || out_of_range) {
		weight = 0.0f;
	}
	r_hit.weight = weight;
	r_hit.cpu_radiance = weight > 0.0f ? p_instance->debug_radiance : Color();
	r_hit.packed = pack_visibility(p_instance->id, choice.card, hit_t);
}

void NextGIVoxelLighting::_trace_brick(const DirtyBrick &p_brick, const TraceInstance *p_instances, uint32_t p_count, uint32_t &r_overlapping) {
	r_overlapping = 0;
	Vector3i voxels[SDF_BRICK_VOXELS * SDF_BRICK_VOXELS * SDF_BRICK_VOXELS];
	const uint32_t voxel_count = voxels_for_sdf_brick(p_brick.brick, voxels, SDF_BRICK_VOXELS * SDF_BRICK_VOXELS * SDF_BRICK_VOXELS);
	const AABB brick_aabb = _brick_aabb(p_brick.level, p_brick.brick);
	LocalVector<uint32_t> kept;
	if (p_instances != nullptr) {
		for (uint32_t index = 0; index < p_count; index++) {
			const TraceInstance &instance = p_instances[index];
			if (instance.field == nullptr || !instance.field->is_valid()) {
				continue;
			}
			if (!instance.world_aabb.intersects(brick_aabb)) {
				continue;
			}
			r_overlapping++;
			if (kept.size() < MAX_TRACE_INSTANCES) {
				kept.push_back(index);
			}
		}
	}
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
		bool any = false;
		for (uint32_t direction = 0; direction < DIRECTIONS; direction++) {
			const uint32_t sample_index = direction_index(p_brick.level, voxel, direction);
			hits.erase(sample_index);
			const AxisHit hit = trace_axis(traced.ptr(), traced.size(), center, direction_vector(direction), max_distance);
			retrace_rays++;
			if (!hit.hit || hit.packed == EMPTY_VISIBILITY) {
				continue;
			}
			StoredHit stored;
			stored.packed = hit.packed;
			stored.instance = hit.instance;
			stored.position = hit.position;
			stored.normal = hit.normal;
			const TraceInstance *match = nullptr;
			for (uint32_t index = 0; index < traced.size(); index++) {
				if (traced[index].id == hit.instance) {
					match = &traced[index];
					break;
				}
			}
			_apply_stored_hit(stored, match);
			hits.insert(sample_index, stored);
			any = true;
		}
		_note_voxel(voxel_key, any);
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

void NextGIVoxelLighting::shade(const TraceInstance *p_instances, uint32_t p_count) {
	shaded_hits = 0;
	covered_hits = 0;
	gpu_shade_words.clear();
	HashMap<uint32_t, const TraceInstance *> by_id;
	if (p_instances != nullptr) {
		for (uint32_t index = 0; index < p_count; index++) {
			by_id.insert(p_instances[index].id, &p_instances[index]);
		}
	}
	for (KeyValue<uint32_t, StoredHit> &entry : hits) {
		const TraceInstance *const *found = by_id.getptr(entry.value.instance);
		_apply_stored_hit(entry.value, found != nullptr ? *found : nullptr);
		shaded_hits++;
		if (entry.value.weight >= WEIGHT_FALLBACK) {
			covered_hits++;
		}
		const uint32_t weight_unorm = uint32_t(CLAMP(entry.value.weight * 255.0f + 0.5f, 0.0f, 255.0f));
		const uint32_t card = entry.value.card >= MAX_CARDS ? NO_CARD : entry.value.card;
		const uint32_t u = uint32_t(CLAMP(entry.value.card_u, 0.0f, 1.0f) * 65535.0f + 0.5f);
		const uint32_t v = uint32_t(CLAMP(entry.value.card_v, 0.0f, 1.0f) * 65535.0f + 0.5f);
		gpu_shade_words.push_back(entry.key);
		gpu_shade_words.push_back(entry.value.page_table);
		gpu_shade_words.push_back((card & 0xFu) | (weight_unorm << 8));
		gpu_shade_words.push_back((u & 0xFFFFu) | ((v & 0xFFFFu) << 16));
	}
}

float NextGIVoxelLighting::get_coverage() const {
	if (shaded_hits == 0) {
		return 0.0f;
	}
	return float(covered_hits) / float(shaded_hits);
}

int64_t NextGIVoxelLighting::get_coverage_ppm() const {
	if (shaded_hits == 0) {
		return 0;
	}
	const double ppm = (double(covered_hits) * 1000000.0) / double(shaded_hits);
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

NextGIVoxelLighting::LookupResult NextGIVoxelLighting::lookup_at(const Vector3 &p_world, const Vector3 &p_gradient, const Color &p_wrc) const {
	LookupResult result;
	result.radiance = p_wrc;
	result.weight = 0.0f;
	result.used_wrc = true;
	int selected = -1;
	Vector3 coord;
	for (uint32_t level = 0; level < LEVELS; level++) {
		if (!origin_set[level]) {
			continue;
		}
		const float lighting = level_voxel_size(level);
		if (!(lighting > 0.0f)) {
			continue;
		}
		const Vector3 origin_world = Vector3(origins[level]) * (lighting * 0.5f);
		const Vector3 level_coord = (p_world - origin_world) / lighting - Vector3(0.5f, 0.5f, 0.5f);
		if (level_coord.x < 0.0f || level_coord.y < 0.0f || level_coord.z < 0.0f || level_coord.x >= float(LEVEL_SIZE - 1) || level_coord.y >= float(LEVEL_SIZE - 1) || level_coord.z >= float(LEVEL_SIZE - 1)) {
			continue;
		}
		selected = int(level);
		coord = level_coord;
		break;
	}
	if (selected < 0) {
		return result;
	}
	const Vector3 base_f(Math::floor(coord.x), Math::floor(coord.y), Math::floor(coord.z));
	const Vector3i base(int(base_f.x), int(base_f.y), int(base_f.z));
	const Vector3 fract = coord - base_f;
	Color accum(0.0f, 0.0f, 0.0f, 0.0f);
	float blend = 0.0f;
	for (uint32_t corner = 0; corner < 8; corner++) {
		const Vector3i delta(int(corner & 1u), int((corner >> 1u) & 1u), int((corner >> 2u) & 1u));
		const Vector3i local = base + delta;
		const Vector3i world = _lighting_origin(origins[selected]) + local;
		const Vector3i physical(wrap_coord(world.x), wrap_coord(world.y), wrap_coord(world.z));
		DirectionSample samples[DIRECTIONS];
		const uint32_t voxel = voxel_index(physical);
		for (uint32_t direction = 0; direction < DIRECTIONS; direction++) {
			const StoredHit *hit = hits.getptr(direction_index(uint32_t(selected), voxel, direction));
			if (hit == nullptr) {
				continue;
			}
			samples[direction].radiance = hit->cpu_radiance;
			samples[direction].weight = hit->weight;
		}
		// Same order as the include: accumulate pre-weighted corners, then one cutoff.
		const VoxelAxisBlend resolved = _axis_blend(p_gradient, samples);
		const float corner_weight = (delta.x == 1 ? fract.x : 1.0f - fract.x) * (delta.y == 1 ? fract.y : 1.0f - fract.y) * (delta.z == 1 ? fract.z : 1.0f - fract.z);
		blend += corner_weight * resolved.weight;
		accum += resolved.preweighted * corner_weight;
	}
	result.weight = blend;
	if (!(blend >= WEIGHT_FALLBACK)) {
		return result;
	}
	result.radiance = accum / blend;
	result.used_wrc = false;
	return result;
}

bool NextGIVoxelLighting::_upload_and_dispatch(uint32_t p_mode, const uint32_t *p_words, uint32_t p_count) {
	if (gpu == nullptr || p_words == nullptr || p_count == 0 || !gpu->staging.is_valid() || !gpu->pipeline.is_valid() || !gpu->uniform_set.is_valid()) {
		return false;
	}
	RenderingDevice *rd = RenderingDevice::get_singleton();
	if (rd == nullptr) {
		return false;
	}
	const uint32_t stride = p_mode == GPU_MODE_SHADE ? GPU_SHADE_STRIDE : 1u;
	const uint32_t payload = uint32_t(staging_buffer_bytes() / sizeof(uint32_t)) - 1u;
	const uint32_t batch = payload / stride;
	if (batch == 0) {
		return false;
	}
	struct Params {
		uint32_t mode;
		uint32_t count;
		uint32_t page_rect_count;
		uint32_t weight_words;
	};
	static_assert(sizeof(Params) == 16, "Voxel lighting push constants must be 16 bytes.");
	for (uint32_t offset = 0; offset < p_count; offset += batch) {
		const uint32_t items = MIN(batch, p_count - offset);
		const uint32_t bytes = items * stride * uint32_t(sizeof(uint32_t));
		const Error err = rd->buffer_update(gpu->staging, sizeof(uint32_t), bytes, p_words + offset * stride);
		if (err != OK) {
			return false;
		}
		Params params;
		params.mode = p_mode;
		params.count = items;
		params.page_rect_count = gpu->page_rect_count;
		params.weight_words = uint32_t(weight_buffer_bytes() / sizeof(uint32_t));
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

bool NextGIVoxelLighting::_dispatch_gpu(const RID &p_radiance_atlas, RID p_page_rects, uint32_t p_page_rect_count) {
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
			WARN_PRINT("NextGI voxel lighting shader failed to compile; CPU results stay in use.");
			return false;
		}
		gpu->pipeline = rd->compute_pipeline_create(gpu->shader.version_get_shader(gpu->version, 0));
		if (!gpu->pipeline.is_valid()) {
			gpu->shader.version_free(gpu->version);
			gpu->version = RID();
			gpu->failed = true;
			WARN_PRINT("NextGI voxel lighting pipeline failed to compile; CPU results stay in use.");
			return false;
		}
	}
	if (!gpu->radiance.is_valid()) {
		gpu->radiance = rd->storage_buffer_create(uint32_t(radiance_buffer_bytes()));
		gpu->weights = rd->storage_buffer_create(uint32_t(weight_buffer_bytes()));
		gpu->staging = rd->storage_buffer_create(uint32_t(staging_buffer_bytes()));
		gpu->page_dummy = rd->storage_buffer_create(sizeof(uint32_t) * 2);
		if (!gpu->radiance.is_valid() || !gpu->weights.is_valid() || !gpu->staging.is_valid() || !gpu->page_dummy.is_valid()) {
			gpu->failed = true;
			WARN_PRINT("NextGI voxel lighting volumes failed to allocate; CPU results stay in use.");
			return false;
		}
		rd->set_resource_name(gpu->radiance, "NextGI Voxel Radiance");
		rd->set_resource_name(gpu->weights, "NextGI Voxel Weights");
		rd->set_resource_name(gpu->staging, "NextGI Voxel Staging");
		allocated_bytes = allocated_volume_bytes();
		gpu->volumes_cleared = false;
	}
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
	const RID atlas = p_radiance_atlas.is_valid() ? p_radiance_atlas : gpu->fallback_texture;
	const RID pages = p_page_rects.is_valid() ? p_page_rects : gpu->page_dummy;
	if (!atlas.is_valid() || !pages.is_valid() || !gpu->sampler.is_valid()) {
		return false;
	}
	gpu->page_rect_count = p_page_rects.is_valid() ? p_page_rect_count : 0;
	if (!gpu->uniform_set.is_valid() || atlas != gpu->bound_atlas || pages != gpu->bound_pages) {
		if (gpu->uniform_set.is_valid()) {
			rd->uniform_set_set_invalidation_callback(gpu->uniform_set, nullptr, nullptr);
			rd->free_rid(gpu->uniform_set);
			gpu->uniform_set = RID();
		}
		Vector<RD::Uniform> uniforms;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, gpu->radiance));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, gpu->weights));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, gpu->staging));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, pages));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ gpu->sampler, atlas })));
		const RID shader_rid = gpu->shader.version_get_shader(gpu->version, 0);
		gpu->uniform_set = rd->uniform_set_create(uniforms, shader_rid, 0);
		if (!gpu->uniform_set.is_valid()) {
			return false;
		}
		rd->uniform_set_set_invalidation_callback(gpu->uniform_set, _uniform_set_invalidated, gpu);
		gpu->bound_atlas = atlas;
		gpu->bound_pages = pages;
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
			if (!_upload_and_dispatch(GPU_MODE_CLEAR, keys.ptr(), count)) {
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
		if (!_upload_and_dispatch(GPU_MODE_CLEAR, unique.ptr(), unique.size())) {
			return false;
		}
	}
	const uint32_t shade_count = gpu_shade_words.size() / GPU_SHADE_STRIDE;
	if (shade_count > 0) {
		const uint32_t zero = 0;
		if (rd->buffer_update(gpu->staging, 0, sizeof(uint32_t), &zero) != OK) {
			return false;
		}
		if (!_upload_and_dispatch(GPU_MODE_SHADE, gpu_shade_words.ptr(), shade_count)) {
			return false;
		}
		const Vector<uint8_t> probe = rd->buffer_get_data(gpu->staging, 0, sizeof(uint32_t));
		uint32_t probe_bits = 0;
		if (probe.size() >= int(sizeof(uint32_t))) {
			memcpy(&probe_bits, probe.ptr(), sizeof(uint32_t));
		}
		if (probe_bits != 0) {
			resampled_nonzero = 1;
		}
	}
	return true;
}

void NextGIVoxelLighting::update_render(RenderDataRD *p_render_data, RendererRD::NextGIRTScene *p_rt_scene, RendererRD::NextGIRelight *p_relight, RendererRD::NextGISRC *p_src) {
	resampled_nonzero = 0;
	gpu_clear_voxels.clear();
	gpu_shade_words.clear();
	const Vector3 camera = p_render_data != nullptr && p_render_data->scene_data != nullptr ? p_render_data->scene_data->cam_transform.origin : Vector3();
	set_clipmap_origins_from_camera(camera);
	const RendererSceneRender::RenderSDFGIUpdateData *update = p_render_data != nullptr ? p_render_data->sdfgi_update_data : nullptr;
	if (update != nullptr) {
		enqueue_dirty_volumes(update->nextgi_dirty_volumes, update->nextgi_dirty_volume_count);
	}
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	if (mesh_storage != nullptr) {
		const uint64_t generation = mesh_storage->get_sdf_atlas().get_generation();
		if (generation != field_generation) {
			field_cache.clear();
			field_generation = generation;
		}
	}
	if (queue.size() == 0 && hits.size() == 0 && gpu_clear_voxels.size() == 0) {
		retrace_rays = 0;
		shaded_hits = 0;
		covered_hits = 0;
		return;
	}

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
			const Vector3 scale = transform.basis.get_scale();
			item.world_scale = MAX(scale.x, MAX(scale.y, scale.z));
			if (!(item.world_scale > 0.0f)) {
				item.world_scale = 1.0f;
			}
			item.mesh = mesh;
			item.geometry = fed.geometry_instance;
			item.element = fed.element_index;
			if (mesh_storage != nullptr) {
				const NextGISDFAtlas::MeshPages *pages = mesh_storage->get_sdf_atlas().get_pages(mesh);
				if (pages != nullptr && pages->field.is_valid()) {
					const AABB bounds = pages->field->get_bounds();
					if (bounds.size.x > 0.0f && bounds.size.y > 0.0f && bounds.size.z > 0.0f) {
						item.world_aabb = transform.xform(bounds);
					}
				}
			}
			candidates.push_back(item);
		}
	} else if (p_rt_scene != nullptr && mesh_storage != nullptr) {
		const NextGISDFInstances &table = p_rt_scene->get_sdf_instances();
		for (uint32_t index = 0; index < table.get_count(); index++) {
			const RID mesh = table.get_mesh(index);
			const NextGISDFAtlas::MeshPages *pages = mesh_storage->get_sdf_atlas().get_pages(mesh);
			if (pages == nullptr || pages->field.is_null()) {
				continue;
			}
			const AABB bounds = pages->field->get_bounds();
			if (bounds.size.x <= 0.0f || bounds.size.y <= 0.0f || bounds.size.z <= 0.0f) {
				continue;
			}
			const Transform3D transform = table.get_transform(index);
			if (!(Math::abs(transform.basis.determinant()) > 1e-8f)) {
				continue;
			}
			RuntimeRef item;
			const uint64_t key = uint64_t(mesh.get_id()) ^ (uint64_t(index + 1u) * 0x9E3779B97F4A7C15ULL);
			if (uint32_t *found = stable_ids.getptr(key)) {
				item.id = *found;
			} else {
				item.id = next_stable_id > 0xFFFFFu ? 0xFFFFFu : next_stable_id++;
				stable_ids.insert(key, item.id);
			}
			item.transform = transform;
			item.inverse = transform.affine_inverse();
			const Vector3 scale = transform.basis.get_scale();
			item.world_scale = MAX(scale.x, MAX(scale.y, scale.z));
			if (!(item.world_scale > 0.0f)) {
				item.world_scale = 1.0f;
			}
			item.mesh = mesh;
			item.world_aabb = transform.xform(bounds);
			candidates.push_back(item);
		}
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
			if (!candidates[index].mesh.is_valid() || candidates[index].world_aabb.size.x <= 0.0f) {
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
	for (const KeyValue<uint32_t, StoredHit> &entry : hits) {
		hit_ids.insert(entry.value.instance);
	}
	LocalVector<TraceInstance> traces;
	LocalVector<uint32_t> field_slots;
	for (uint32_t index = 0; index < candidates.size(); index++) {
		const RuntimeRef &candidate = candidates[index];
		bool overlaps = false;
		if (candidate.world_aabb.size.x > 0.0f) {
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
		trace.world_scale = candidate.world_scale;
		trace.world_aabb = candidate.world_aabb;
		trace.page_table = EMPTY_VISIBILITY;
		if (p_relight != nullptr && candidate.geometry != nullptr) {
			trace.page_table = p_relight->get_instance_page_table(candidate.geometry, candidate.element);
		}
		_fill_cards(p_src, candidate.geometry, candidate.mesh, candidate.transform, candidate.world_scale, trace);
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
	uint32_t page_count = 0;
	RID pages;
	RID atlas;
	if (p_relight != nullptr) {
		pages = p_relight->get_page_rect_buffer();
		if (pages.is_valid()) {
			page_count = p_relight->get_page_rect_count();
		}
		atlas = p_relight->get_radiance_texture();
	}
	shade_page_rect_count = page_count;
	shade_reject_pages = true;
	shade(traces.ptr(), traces.size());
	if (gpu_clear_voxels.size() == 0 && gpu_shade_words.size() == 0) {
		return;
	}
	_dispatch_gpu(atlas, pages, page_count);
}
