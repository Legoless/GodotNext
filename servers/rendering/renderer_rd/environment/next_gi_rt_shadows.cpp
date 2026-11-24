/**************************************************************************/
/*  next_gi_rt_shadows.cpp                                                */
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

#include "next_gi_rt_shadows.h"

#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/templates/hashfuncs.h"
#include "servers/rendering/renderer_compositor.h"
#include "servers/rendering/renderer_rd/effects/denoise.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_hit_materials.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_scene.h"
#include "servers/rendering/renderer_rd/environment/next_gi_shadow_policy.h"
#include "servers/rendering/renderer_rd/environment/next_gi_view.h"
#include "servers/rendering/renderer_rd/environment/nextgi_blue_noise.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_rt_shadow_alloc.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_rt_shadows.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_globals.h"

#include <cmath>
#include <cstring>

using namespace RendererRD;

// GodotNext: rt_shadows (#141, plan §21.3 contract M9, GM-3, GM-4, budget M-7).

static const StringName &_rt_shadows_rb_scope() {
	static const StringName scope("nextgi_rt_shadows");
	return scope;
}

static_assert(nextgi_rt_shadow_preset_tile_budget(RendererVirtualShadowMapStorage::POOL_QUALITY_HIGH) == 65536u, "High preset holds 65,536 16x16 shadow tiles.");

/* PURE CPU MODELS */

float RendererRD::nextgi_rt_shadow_origin_offset(float p_eps_rt, float p_scale, float p_linear_depth) {
	const float eps_term = 4.0f * MAX(p_eps_rt, 0.0f) * MAX(p_scale, 0.0f);
	const float depth_term = 0.004f + MAX(p_linear_depth, 0.0f) * 0.002f;
	return CLAMP(MAX(eps_term, depth_term), 0.004f, NEXTGI_RT_SHADOW_MAX_ORIGIN_OFFSET);
}

NextGIRTShadowTileRect RendererRD::nextgi_rt_shadow_project_rect(
		NextGIRTShadowLightType p_type,
		const Vector3 &p_world_pos,
		float p_cull_radius,
		const Projection &p_view_proj,
		const Vector3 &p_cam_pos,
		float p_z_near,
		uint32_t p_tiles_x,
		uint32_t p_tiles_y) {
	NextGIRTShadowTileRect rect;
	if (p_tiles_x == 0u || p_tiles_y == 0u) {
		return rect;
	}
	if (p_type == NEXTGI_RT_SHADOW_TYPE_DIRECTIONAL) {
		rect.min_tx = 0u;
		rect.min_ty = 0u;
		rect.width = p_tiles_x;
		rect.height = p_tiles_y;
		return rect;
	}
	if (!(p_cull_radius > 0.0f)) {
		return rect;
	}
	const float cam_dist = float(p_cam_pos.distance_to(p_world_pos));
	const float safe_radius = p_cull_radius + MAX(p_z_near * 2.0f, 0.25f);
	if (cam_dist <= safe_radius) {
		rect.min_tx = 0u;
		rect.min_ty = 0u;
		rect.width = p_tiles_x;
		rect.height = p_tiles_y;
		return rect;
	}

	// Project the 8 corners of the world-space bounding box of the light sphere.
	float min_u = 1e30f;
	float max_u = -1e30f;
	float min_v = 1e30f;
	float max_v = -1e30f;
	bool any_behind = false;
	bool any_in_front = false;
	for (int corner = 0; corner < 8; corner++) {
		const Vector3 offset(
				(corner & 1) ? p_cull_radius : -p_cull_radius,
				(corner & 2) ? p_cull_radius : -p_cull_radius,
				(corner & 4) ? p_cull_radius : -p_cull_radius);
		const Vector3 wp = p_world_pos + offset;
		const Vector4 clip = p_view_proj.xform(Vector4(wp.x, wp.y, wp.z, 1.0f));
		if (clip.w <= 1e-4f) {
			any_behind = true;
		} else {
			any_in_front = true;
			const float u = (clip.x / clip.w) * 0.5f + 0.5f;
			const float v = (clip.y / clip.w) * 0.5f + 0.5f;
			min_u = MIN(min_u, u);
			max_u = MAX(max_u, u);
			min_v = MIN(min_v, v);
			max_v = MAX(max_v, v);
		}
	}
	if (!any_in_front) {
		return rect;
	}
	if (any_behind) {
		// Box straddles the camera near plane: stay conservative and let GPU tile depth culling trim.
		rect.min_tx = 0u;
		rect.min_ty = 0u;
		rect.width = p_tiles_x;
		rect.height = p_tiles_y;
		return rect;
	}
	if (max_u < 0.0f || min_u > 1.0f || max_v < 0.0f || min_v > 1.0f) {
		return rect;
	}

	const float cu0 = CLAMP(min_u, 0.0f, 1.0f);
	const float cu1 = CLAMP(max_u, 0.0f, 1.0f);
	const float cv0 = CLAMP(min_v, 0.0f, 1.0f);
	const float cv1 = CLAMP(max_v, 0.0f, 1.0f);

	const uint32_t tx0 = MIN(uint32_t(Math::floor(cu0 * float(p_tiles_x))), p_tiles_x - 1u);
	const uint32_t ty0 = MIN(uint32_t(Math::floor(cv0 * float(p_tiles_y))), p_tiles_y - 1u);
	const uint32_t tx1 = MIN(uint32_t(Math::floor(cu1 * float(p_tiles_x))), p_tiles_x - 1u);
	const uint32_t ty1 = MIN(uint32_t(Math::floor(cv1 * float(p_tiles_y))), p_tiles_y - 1u);
	rect.min_tx = tx0;
	rect.min_ty = ty0;
	rect.width = (tx1 >= tx0) ? (tx1 - tx0 + 1u) : 0u;
	rect.height = (ty1 >= ty0) ? (ty1 - ty0 + 1u) : 0u;
	return rect;
}

void NextGIRTShadowBudget::reset(uint32_t p_budget) {
	tile_budget = p_budget;
	tiles_used = 0u;
	lights_accepted = 0u;
	overflow_tiles = 0u;
	overflow_lights = 0u;
	fallback_lights = 0u;
}

bool NextGIRTShadowBudget::try_allocate_light(uint32_t p_light_tiles) {
	if (p_light_tiles == 0u) {
		lights_accepted++;
		return true;
	}
	if (uint64_t(tiles_used) + uint64_t(p_light_tiles) <= uint64_t(tile_budget)) {
		tiles_used += p_light_tiles;
		lights_accepted++;
		return true;
	}
	overflow_tiles += p_light_tiles;
	overflow_lights++;
	fallback_lights++;
	return false;
}

bool RendererRD::nextgi_rt_shadow_history_valid(
		uint32_t p_cur_stable_id,
		uint32_t p_prev_stable_id,
		uint32_t p_cur_packed_st,
		uint32_t p_prev_packed_st,
		float p_expected_z,
		float p_prev_z,
		const Vector3 &p_cur_normal,
		const Vector3 &p_prev_normal) {
	if (p_cur_stable_id == 0u || p_cur_stable_id != p_prev_stable_id) {
		return false;
	}
	if (p_cur_packed_st != p_prev_packed_st) {
		return false;
	}
	if (!(p_prev_z > 0.0f) || !(p_expected_z > 0.0f)) {
		return false;
	}
	const float rel_depth = std::fabs(p_prev_z - p_expected_z) / MAX(p_expected_z, 0.05f);
	if (rel_depth > 0.1f) {
		return false;
	}
	const float ndot = float(p_cur_normal.dot(p_prev_normal));
	return ndot >= 0.85f;
}

float RendererRD::nextgi_rt_shadow_transmittance_thickness(float p_hit_dist, float p_transmittance_bias) {
	if (!(p_hit_dist >= 0.0f) || p_hit_dist >= NEXTGI_RT_SHADOW_HIT_MISS * 0.5f) {
		return -1.0f;
	}
	return MAX(p_hit_dist + p_transmittance_bias, 0.0f);
}

float RendererRD::nextgi_rt_shadow_penumbra_pixels(
		bool p_directional,
		float p_hit_dist,
		float p_light_size,
		float p_dist_to_light,
		float p_pixel_footprint) {
	if (!(p_light_size > 1e-5f) || !(p_hit_dist >= 0.0f) || p_hit_dist >= NEXTGI_RT_SHADOW_HIT_MISS * 0.5f) {
		return 0.0f;
	}
	float penumbra_world = 0.0f;
	if (p_directional) {
		penumbra_world = p_hit_dist * p_light_size;
	} else {
		const float dist = MAX(p_dist_to_light, p_hit_dist + 0.05f);
		penumbra_world = p_light_size * p_hit_dist / MAX(dist - p_hit_dist, 0.05f);
	}
	return CLAMP(penumbra_world / MAX(p_pixel_footprint, 1e-4f), 0.0f, 16.0f);
}

/* PER-VIEW GPU STATE */

static void _release_readback_state(NextGIRTShadowsView::ReadbackState *p_state) {
	if (p_state != nullptr && p_state->refs.unref()) {
		memdelete(p_state);
	}
}

static void _rt_shadow_alloc_state_received(const Vector<uint8_t> &p_data, uint64_t p_state_ptr) {
	NextGIRTShadowsView::ReadbackState *state = reinterpret_cast<NextGIRTShadowsView::ReadbackState *>(p_state_ptr);
	if (state == nullptr) {
		return;
	}
	if (state->alive.is_set()) {
		if (p_data.size() >= int64_t(4 * sizeof(uint32_t))) {
			const uint32_t *words = reinterpret_cast<const uint32_t *>(p_data.ptr());
			state->tiles_allocated = words[1];
			state->overflow_tiles = words[2];
			state->overflow_lights = words[3];
			state->has_result = true;
		}
		state->pending.clear();
	}
	_release_readback_state(state);
}

static void _rt_shadow_overflow_lights_received(const Vector<uint8_t> &p_data, uint64_t p_state_ptr) {
	NextGIRTShadowsView::ReadbackState *state = reinterpret_cast<NextGIRTShadowsView::ReadbackState *>(p_state_ptr);
	if (state == nullptr) {
		return;
	}
	if (state->alive.is_set()) {
		state->overflow_set.clear();
		const uint32_t count = MIN(uint32_t(p_data.size() / sizeof(uint32_t)), state->submitted_lights.size());
		const uint32_t *flags = reinterpret_cast<const uint32_t *>(p_data.ptr());
		for (uint32_t i = 0; i < count; i++) {
			if (flags[i] != 0u) {
				state->overflow_set.insert(state->submitted_lights[i]);
			}
		}
	}
	_release_readback_state(state);
}

NextGIRTShadowsView::NextGIRTShadowsView() {
	readback = memnew(ReadbackState);
	readback->refs.init(1);
	readback->alive.set();
}

NextGIRTShadowsView::~NextGIRTShadowsView() {
	free_resources();
	if (readback != nullptr) {
		readback->alive.clear();
		_release_readback_state(readback);
		readback = nullptr;
	}
}

void NextGIRTShadowsView::invalidate_trace_set() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr && trace_uniform_set.is_valid() && rd->uniform_set_is_valid(trace_uniform_set)) {
		rd->free_rid(trace_uniform_set);
	}
	trace_uniform_set = RID();
	trace_uniform_key = 0;
}

void NextGIRTShadowsView::free_resources() {
	invalidate_trace_set();
	RenderingDevice *rd = RD::get_singleton();
	auto free_rid = [&](RID &r_rid) {
		if (rd != nullptr && r_rid.is_valid()) {
			rd->free_rid(r_rid);
		}
		r_rid = RID();
	};
	for (int i = 0; i < 3; i++) {
		free_rid(mask_atlas[i]);
	}
	for (int i = 0; i < 2; i++) {
		free_rid(history_len[i]);
		free_rid(lookup_buffer[i]);
		free_rid(tile_owner_buffer[i]);
		lookup_capacity[i] = 0;
	}
	free_rid(hit_dist_atlas);
	free_rid(prev_depth_normal);
	free_rid(tile_depth_minmax_buffer);
	free_rid(tile_list_buffer);
	free_rid(lights_buffer);
	free_rid(params_buffer);
	free_rid(alloc_state_buffer);
	free_rid(indirect_buffer);
	free_rid(light_overflow_buffer);
	lights_capacity = 0;
	light_overflow_capacity = 0;
	screen_size = Size2i();
	tile_budget = 0;
	has_history = false;
	vram_bytes = 0;
	prev_lookup_bases.clear();
	cur_lookup_bases.clear();
	overflow_fallback_lights.clear();
}

void NextGIRTShadowsView::ensure_resources(const Size2i &p_screen_size, uint32_t p_tile_budget) {
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL(rd);
	const uint32_t clamped_budget = MAX(p_tile_budget, 256u);
	const bool budget_changed = tile_budget != clamped_budget || mask_atlas[0].is_null();
	const bool screen_changed = screen_size != p_screen_size || prev_depth_normal.is_null();

	if (budget_changed) {
		invalidate_trace_set();
		auto free_rid = [&](RID &r_rid) {
			if (r_rid.is_valid()) {
				rd->free_rid(r_rid);
			}
			r_rid = RID();
		};
		for (int i = 0; i < 3; i++) {
			free_rid(mask_atlas[i]);
		}
		for (int i = 0; i < 2; i++) {
			free_rid(history_len[i]);
			free_rid(tile_owner_buffer[i]);
		}
		free_rid(hit_dist_atlas);
		free_rid(tile_list_buffer);

		const Size2i atlas_size = nextgi_rt_shadow_atlas_size(clamped_budget);
		RD::TextureFormat mask_fmt;
		mask_fmt.format = RD::DATA_FORMAT_R8_UNORM;
		mask_fmt.texture_type = RD::TEXTURE_TYPE_2D;
		mask_fmt.width = uint32_t(atlas_size.x);
		mask_fmt.height = uint32_t(atlas_size.y);
		mask_fmt.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		for (int i = 0; i < 3; i++) {
			mask_atlas[i] = rd->texture_create(mask_fmt, RD::TextureView());
			rd->set_resource_name(mask_atlas[i], vformat("NextGI RT Shadow Mask Atlas %d", i));
			rd->texture_clear(mask_atlas[i], Color(1, 1, 1, 1), 0, 1, 0, 1);
		}

		RD::TextureFormat len_fmt;
		len_fmt.format = RD::DATA_FORMAT_R8_UINT;
		len_fmt.texture_type = RD::TEXTURE_TYPE_2D;
		len_fmt.width = uint32_t(atlas_size.x);
		len_fmt.height = uint32_t(atlas_size.y);
		len_fmt.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		for (int i = 0; i < 2; i++) {
			history_len[i] = rd->texture_create(len_fmt, RD::TextureView());
			rd->set_resource_name(history_len[i], vformat("NextGI RT Shadow History Len %d", i));
			rd->texture_clear(history_len[i], Color(0, 0, 0, 0), 0, 1, 0, 1);
		}

		RD::TextureFormat hit_fmt;
		hit_fmt.format = RD::DATA_FORMAT_R16_SFLOAT;
		hit_fmt.texture_type = RD::TEXTURE_TYPE_2D;
		hit_fmt.width = uint32_t(atlas_size.x);
		hit_fmt.height = uint32_t(atlas_size.y);
		hit_fmt.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		hit_dist_atlas = rd->texture_create(hit_fmt, RD::TextureView());
		rd->set_resource_name(hit_dist_atlas, "NextGI RT Shadow Hit Distance Atlas");
		rd->texture_clear(hit_dist_atlas, Color(NEXTGI_RT_SHADOW_HIT_MISS, 0, 0, 0), 0, 1, 0, 1);

		tile_list_buffer = rd->storage_buffer_create(clamped_budget * 16u);
		rd->set_resource_name(tile_list_buffer, "NextGI RT Shadow Tile List");
		for (int i = 0; i < 2; i++) {
			tile_owner_buffer[i] = rd->storage_buffer_create(clamped_budget * 8u);
			rd->set_resource_name(tile_owner_buffer[i], vformat("NextGI RT Shadow Tile Owner %d", i));
			rd->buffer_clear(tile_owner_buffer[i], 0, clamped_budget * 8u);
		}

		tile_budget = clamped_budget;
		has_history = false;
		prev_lookup_bases.clear();
		// 3x R8 mask + 2x R8UI history len + R16F hit dist.
		const uint64_t r8_bytes = uint64_t(atlas_size.x) * uint64_t(atlas_size.y);
		vram_bytes = r8_bytes * 3u + r8_bytes * 2u + r8_bytes * 2u;
	}

	if (screen_changed) {
		if (prev_depth_normal.is_valid()) {
			rd->free_rid(prev_depth_normal);
		}
		if (tile_depth_minmax_buffer.is_valid()) {
			rd->free_rid(tile_depth_minmax_buffer);
		}
		RD::TextureFormat guide_fmt;
		guide_fmt.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		guide_fmt.texture_type = RD::TEXTURE_TYPE_2D;
		guide_fmt.width = uint32_t(MAX(p_screen_size.x, 1));
		guide_fmt.height = uint32_t(MAX(p_screen_size.y, 1));
		guide_fmt.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		prev_depth_normal = rd->texture_create(guide_fmt, RD::TextureView());
		rd->set_resource_name(prev_depth_normal, "NextGI RT Shadow Prev Depth Normal");
		rd->texture_clear(prev_depth_normal, Color(0, 0, 0, 0), 0, 1, 0, 1);

		const uint32_t tiles_x = (uint32_t(MAX(p_screen_size.x, 1)) + NEXTGI_RT_SHADOW_TILE_SIZE - 1u) / NEXTGI_RT_SHADOW_TILE_SIZE;
		const uint32_t tiles_y = (uint32_t(MAX(p_screen_size.y, 1)) + NEXTGI_RT_SHADOW_TILE_SIZE - 1u) / NEXTGI_RT_SHADOW_TILE_SIZE;
		tile_depth_minmax_buffer = rd->storage_buffer_create(MAX(tiles_x * tiles_y, 1u) * 8u);
		rd->set_resource_name(tile_depth_minmax_buffer, "NextGI RT Shadow Tile Depth MinMax");

		screen_size = p_screen_size;
		has_history = false;
		prev_lookup_bases.clear();
	}

	if (params_buffer.is_null()) {
		params_buffer = rd->storage_buffer_create(sizeof(NextGIRTShadows::ParamsGPU));
		rd->set_resource_name(params_buffer, "NextGI RT Shadow Params");
	}
	if (alloc_state_buffer.is_null()) {
		alloc_state_buffer = rd->storage_buffer_create(8u * sizeof(uint32_t));
		rd->set_resource_name(alloc_state_buffer, "NextGI RT Shadow Alloc State");
		rd->buffer_clear(alloc_state_buffer, 0, 8u * sizeof(uint32_t));
	}
	if (indirect_buffer.is_null()) {
		indirect_buffer = rd->storage_buffer_create(4u * sizeof(uint32_t), Span<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
		rd->set_resource_name(indirect_buffer, "NextGI RT Shadow Indirect Args");
		rd->buffer_clear(indirect_buffer, 0, 4u * sizeof(uint32_t));
	}
	// Ensure both lookup buffers exist with at least 16 bytes so prev_lookup binding is always valid.
	for (int i = 0; i < 2; i++) {
		if (lookup_buffer[i].is_null()) {
			lookup_capacity[i] = 64u;
			lookup_buffer[i] = rd->storage_buffer_create(lookup_capacity[i] * sizeof(uint32_t));
			rd->set_resource_name(lookup_buffer[i], vformat("NextGI RT Shadow Tile Lookup %d", i));
			rd->buffer_clear(lookup_buffer[i], 0, lookup_capacity[i] * sizeof(uint32_t));
		}
	}
}

void NextGIRTShadowsView::ensure_dynamic_buffers(uint32_t p_light_count, uint32_t p_lookup_words, uint32_t p_cur_slot) {
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL(rd);
	const uint32_t needed_lights = MAX(p_light_count, 16u);
	if (lights_buffer.is_null() || needed_lights > lights_capacity) {
		invalidate_trace_set();
		if (lights_buffer.is_valid()) {
			rd->free_rid(lights_buffer);
		}
		lights_capacity = MAX(needed_lights, lights_capacity * 2u);
		lights_buffer = rd->storage_buffer_create(lights_capacity * sizeof(NextGIRTShadows::LightGPU));
		rd->set_resource_name(lights_buffer, "NextGI RT Shadow Lights");
	}
	if (light_overflow_buffer.is_null() || needed_lights > light_overflow_capacity) {
		if (light_overflow_buffer.is_valid()) {
			rd->free_rid(light_overflow_buffer);
		}
		light_overflow_capacity = MAX(needed_lights, light_overflow_capacity * 2u);
		light_overflow_buffer = rd->storage_buffer_create(light_overflow_capacity * sizeof(uint32_t));
		rd->set_resource_name(light_overflow_buffer, "NextGI RT Shadow Light Overflow");
	}
	const uint32_t needed_words = MAX(p_lookup_words, 64u);
	if (lookup_buffer[p_cur_slot].is_null() || needed_words > lookup_capacity[p_cur_slot]) {
		if (lookup_buffer[p_cur_slot].is_valid()) {
			rd->free_rid(lookup_buffer[p_cur_slot]);
		}
		lookup_capacity[p_cur_slot] = MAX(needed_words, lookup_capacity[p_cur_slot] * 2u);
		lookup_buffer[p_cur_slot] = rd->storage_buffer_create(lookup_capacity[p_cur_slot] * sizeof(uint32_t));
		rd->set_resource_name(lookup_buffer[p_cur_slot], vformat("NextGI RT Shadow Tile Lookup %d", p_cur_slot));
	}
}

void NextGIRTShadowsView::poll_readback() {
	if (readback == nullptr || !readback->has_result) {
		return;
	}
	last_tiles_allocated = readback->tiles_allocated;
	last_overflow_tiles = readback->overflow_tiles;
	last_overflow_lights = readback->overflow_lights;
	if (!readback->overflow_set.is_empty()) {
		for (const RID &li : readback->overflow_set) {
			overflow_fallback_lights.insert(li);
		}
		overflow_retry_countdown = 30u;
	} else if (overflow_retry_countdown > 0u) {
		overflow_retry_countdown--;
		if (overflow_retry_countdown == 0u) {
			overflow_fallback_lights.clear();
		}
	}
	readback->has_result = false;
}

/* NEXTGI RT SHADOWS SYSTEM */

struct NextGIRTShadows::Shaders {
	enum AllocMode {
		ALLOC_MODE_DEPTH_MINMAX = 0,
		ALLOC_MODE_INIT_LOOKUP = 1,
		ALLOC_MODE_ALLOC_TILES = 2,
		ALLOC_MODE_FINALIZE = 3,
		ALLOC_MODE_RETRACE_RESOLVE = 4,
		ALLOC_MODE_MAX,
	};
	NextgiRtShadowAllocShaderRD alloc_shader;
	RID alloc_version;
	RID alloc_pipelines[ALLOC_MODE_MAX];

	NextgiRtShadowsShaderRD trace_shader;
	RID trace_version;
	RID trace_pipelines[2]; // 0: base opaque/cut; 1: NEXTGI_SHADOW_ALPHA_TEXTURES + retrace.
};

Vector<String> NextGIRTShadows::get_alloc_shader_variant_defines() {
	Vector<String> modes;
	modes.push_back("\n#define MODE_DEPTH_MINMAX\n");
	modes.push_back("\n#define MODE_INIT_LOOKUP\n");
	modes.push_back("\n#define MODE_ALLOC_TILES\n");
	modes.push_back("\n#define MODE_FINALIZE\n");
	modes.push_back("\n#define MODE_RETRACE_RESOLVE\n");
	return modes;
}

Vector<String> NextGIRTShadows::get_trace_shader_variant_defines() {
	Vector<String> variants;
	variants.push_back("");
	variants.push_back("\n#define NEXTGI_SHADOW_ALPHA_TEXTURES\n");
	return variants;
}

NextGIRTShadows::NextGIRTShadows() {}

NextGIRTShadows::~NextGIRTShadows() {
	_free_resources();
}

void NextGIRTShadows::_free_resources() {
	RenderingDevice *rd = RD::get_singleton();
	if (shaders != nullptr) {
		if (shaders->alloc_version.is_valid()) {
			for (int i = 0; i < Shaders::ALLOC_MODE_MAX; i++) {
				if (rd != nullptr && shaders->alloc_pipelines[i].is_valid()) {
					rd->free_rid(shaders->alloc_pipelines[i]);
				}
				shaders->alloc_pipelines[i] = RID();
			}
			shaders->alloc_shader.version_free(shaders->alloc_version);
			shaders->alloc_version = RID();
		}
		if (shaders->trace_version.is_valid()) {
			for (int i = 0; i < 2; i++) {
				if (rd != nullptr && shaders->trace_pipelines[i].is_valid()) {
					rd->free_rid(shaders->trace_pipelines[i]);
				}
				shaders->trace_pipelines[i] = RID();
			}
			shaders->trace_shader.version_free(shaders->trace_version);
			shaders->trace_version = RID();
		}
		memdelete(shaders);
		shaders = nullptr;
	}
	if (denoiser != nullptr) {
		memdelete(denoiser);
		denoiser = nullptr;
	}
	if (rd != nullptr) {
		if (shadow_texture_set.is_valid() && rd->uniform_set_is_valid(shadow_texture_set)) {
			rd->free_rid(shadow_texture_set);
		}
		if (retrace_buffer.is_valid()) {
			rd->free_rid(retrace_buffer);
		}
		if (blue_noise_texture.is_valid()) {
			rd->free_rid(blue_noise_texture);
		}
	}
	shadow_texture_set = RID();
	shadow_texture_set_table = RID();
	retrace_buffer = RID();
	blue_noise_texture = RID();
	alloc_shader_ready = false;
	trace_shader_ready = false;
	active_view.unref();
}

bool NextGIRTShadows::_ensure_alloc_shaders() {
	if (alloc_shader_ready) {
		return true;
	}
	if (alloc_shader_failed) {
		return false;
	}
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	if (shaders == nullptr) {
		shaders = memnew(Shaders);
	}
	shaders->alloc_shader.initialize(get_alloc_shader_variant_defines());
	shaders->alloc_version = shaders->alloc_shader.version_create(false);
	if (shaders->alloc_version.is_null()) {
		alloc_shader_failed = true;
		ERR_FAIL_V_MSG(false, "NextGIRTShadows: could not create tile-allocation shader version.");
	}
	for (int i = 0; i < Shaders::ALLOC_MODE_MAX; i++) {
		const RID s = shaders->alloc_shader.version_get_shader(shaders->alloc_version, i);
		if (s.is_null()) {
			for (int j = 0; j < i; j++) {
				if (shaders->alloc_pipelines[j].is_valid()) {
					rd->free_rid(shaders->alloc_pipelines[j]);
					shaders->alloc_pipelines[j] = RID();
				}
			}
			shaders->alloc_shader.version_free(shaders->alloc_version);
			shaders->alloc_version = RID();
			alloc_shader_failed = true;
			ERR_FAIL_V_MSG(false, "NextGIRTShadows: tile-allocation shader failed to compile.");
		}
		shaders->alloc_pipelines[i] = rd->compute_pipeline_create(s);
	}
	alloc_shader_ready = true;
	return true;
}

bool NextGIRTShadows::_ensure_trace_shaders(NextGI *p_next_gi) {
	if (trace_shader_ready) {
		return true;
	}
	if (trace_shader_failed) {
		return false;
	}
	if (p_next_gi == nullptr || !p_next_gi->can_initialize_ray_query_shaders()) {
		return false;
	}
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	if (shaders == nullptr) {
		shaders = memnew(Shaders);
	}
	shaders->trace_shader.initialize(get_trace_shader_variant_defines());
	shaders->trace_version = shaders->trace_shader.version_create(false);
	if (shaders->trace_version.is_null()) {
		trace_shader_failed = true;
		ERR_FAIL_V_MSG(false, "NextGIRTShadows: could not create RT shadow trace shader version.");
	}
	for (int i = 0; i < 2; i++) {
		const RID s = shaders->trace_shader.version_get_shader(shaders->trace_version, i);
		if (s.is_null()) {
			for (int j = 0; j < i; j++) {
				if (shaders->trace_pipelines[j].is_valid()) {
					rd->free_rid(shaders->trace_pipelines[j]);
					shaders->trace_pipelines[j] = RID();
				}
			}
			shaders->trace_shader.version_free(shaders->trace_version);
			shaders->trace_version = RID();
			trace_shader_failed = true;
			ERR_FAIL_V_MSG(false, "NextGIRTShadows: RT shadow trace shader failed to compile.");
		}
		shaders->trace_pipelines[i] = rd->compute_pipeline_create(s);
	}
	p_next_gi->notify_ray_query_shader_initialized();
	trace_shader_ready = true;
	return true;
}

void NextGIRTShadows::_ensure_blue_noise() {
	if (blue_noise_texture.is_valid()) {
		return;
	}
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL(rd);
	RD::TextureFormat fmt;
	fmt.format = RD::DATA_FORMAT_R8G8_UNORM;
	fmt.texture_type = RD::TEXTURE_TYPE_2D;
	fmt.width = 64;
	fmt.height = 64;
	fmt.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	Vector<uint8_t> data;
	data.resize(64 * 64 * 2);
	memcpy(data.ptrw(), nextgi_blue_noise_tile, 64 * 64 * 2);
	Vector<Vector<uint8_t>> slices;
	slices.push_back(data);
	blue_noise_texture = rd->texture_create(fmt, RD::TextureView(), slices);
	rd->set_resource_name(blue_noise_texture, "NextGI RT Shadow Blue Noise");
}

bool NextGIRTShadows::is_light_in_overflow_fallback(const Ref<RenderSceneBuffersRD> &p_render_buffers, RID p_light_instance) {
	if (p_render_buffers.is_null() || p_light_instance.is_null()) {
		return false;
	}
	if (!p_render_buffers->has_custom_data(_rt_shadows_rb_scope())) {
		return false;
	}
	const Ref<NextGIRTShadowsView> view = p_render_buffers->get_custom_data(_rt_shadows_rb_scope());
	return view.is_valid() && view->overflow_fallback_lights.has(p_light_instance);
}

void NextGIRTShadows::begin_view(RenderDataRD *p_render_data, NextGIRTScene *p_rt_scene, uint32_t p_tile_budget) {
	view_active = false;
	view_tlas_ready = false;
	active_view.unref();
	active_rt_scene = p_rt_scene;
	active_lights.clear();
	active_light_rids.clear();
	active_lookup_cursor = 0;
	active_max_rect_w = 0;
	active_max_rect_h = 0;
	active_dir_count = 0;
	active_local_count = 0;
	active_fallback_count = 0;
	bound_mask_atlas = RID();
	bound_tile_lookup = RID();
	bound_hit_dist_atlas = RID();

	if (p_render_data == nullptr || p_render_data->scene_data == nullptr || p_render_data->reflection_probe.is_valid()) {
		return;
	}
	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	if (rb.is_null() || rb->get_view_count() != 1) {
		return;
	}
	const Size2i screen_size = rb->get_internal_size();
	if (screen_size.x <= 0 || screen_size.y <= 0) {
		return;
	}

	Ref<NextGIRTShadowsView> view;
	if (rb->has_custom_data(_rt_shadows_rb_scope())) {
		view = rb->get_custom_data(_rt_shadows_rb_scope());
	}
	if (view.is_null()) {
		view.instantiate();
		rb->set_custom_data(_rt_shadows_rb_scope(), view);
	}
	view->poll_readback();

	active_view = view;
	active_screen_size = screen_size;
	active_tiles_x = (uint32_t(screen_size.x) + NEXTGI_RT_SHADOW_TILE_SIZE - 1u) / NEXTGI_RT_SHADOW_TILE_SIZE;
	active_tiles_y = (uint32_t(screen_size.y) + NEXTGI_RT_SHADOW_TILE_SIZE - 1u) / NEXTGI_RT_SHADOW_TILE_SIZE;
	active_tile_budget = MAX(p_tile_budget, 256u);
	active_view_proj = NextGIView::view_projection(*p_render_data->scene_data);
	active_cam_pos = p_render_data->scene_data->cam_transform.origin;
	active_z_near = p_render_data->scene_data->z_near;
	view_tlas_ready = p_rt_scene != nullptr && p_rt_scene->is_tlas_traceable() && p_rt_scene->get_tlas().is_valid();
	view->cur_lookup_bases.clear();
	view_active = true;
}

uint32_t NextGIRTShadows::register_light(const LightRegistration &p_light, uint32_t &r_flags) {
	r_flags = 0u;
	if (!view_active || active_view.is_null() || p_light.light_instance.is_null()) {
		return NEXTGI_RT_SHADOW_TILE_NONE;
	}
	if (active_view->overflow_fallback_lights.has(p_light.light_instance)) {
		active_fallback_count++;
		return NEXTGI_RT_SHADOW_TILE_NONE;
	}

	const NextGIRTShadowTileRect rect = nextgi_rt_shadow_project_rect(
			p_light.type,
			p_light.world_pos,
			p_light.cull_radius,
			active_view_proj,
			active_cam_pos,
			active_z_near,
			active_tiles_x,
			active_tiles_y);
	if (rect.is_empty()) {
		// Culled off-screen: still sampled through RT (0 tiles needed -> unshadowed).
		return NEXTGI_RT_SHADOW_TILE_NONE;
	}

	// Assign a unique non-zero stable ID per light instance for temporal tile history invalidation.
	uint32_t *stable_ptr = active_view->light_stable_ids.getptr(p_light.light_instance);
	uint32_t stable_id = 0u;
	if (stable_ptr != nullptr) {
		stable_id = *stable_ptr;
	} else {
		stable_id = active_view->next_stable_id++;
		if (stable_id == 0u) {
			stable_id = active_view->next_stable_id++;
		}
		active_view->light_stable_ids.insert(p_light.light_instance, stable_id);
	}

	const uint32_t lookup_base = active_lookup_cursor;
	const uint32_t lookup_words = 2u + rect.tile_count();
	active_lookup_cursor += lookup_words;
	active_max_rect_w = MAX(active_max_rect_w, rect.width);
	active_max_rect_h = MAX(active_max_rect_h, rect.height);

	const uint32_t *prev_base_ptr = active_view->prev_lookup_bases.getptr(stable_id);
	const uint32_t prev_lookup_base = (active_view->has_history && prev_base_ptr != nullptr) ? *prev_base_ptr : NEXTGI_RT_SHADOW_TILE_NONE;
	active_view->cur_lookup_bases.insert(stable_id, lookup_base);

	const uint32_t light_mode = active_rt_scene != nullptr ? active_rt_scene->get_shadow_light_mode(p_light.caster_mask) : 0u;

	LightGPU lg;
	lg.pos_range[0] = float(p_light.world_pos.x);
	lg.pos_range[1] = float(p_light.world_pos.y);
	lg.pos_range[2] = float(p_light.world_pos.z);
	lg.pos_range[3] = p_light.range;

	lg.dir_cos[0] = float(p_light.world_dir.x);
	lg.dir_cos[1] = float(p_light.world_dir.y);
	lg.dir_cos[2] = float(p_light.world_dir.z);
	lg.dir_cos[3] = p_light.cos_spot_angle;

	float effective_size = p_light.source_radius;
	if (p_light.type == NEXTGI_RT_SHADOW_TYPE_AREA) {
		effective_size = MAX(effective_size, 0.5f * MIN(float(p_light.area_size.x), float(p_light.area_size.y)));
	}
	lg.area_width_size[0] = float(p_light.area_width_world.x);
	lg.area_width_size[1] = float(p_light.area_width_world.y);
	lg.area_width_size[2] = float(p_light.area_width_world.z);
	lg.area_width_size[3] = effective_size;

	lg.area_height_trans[0] = float(p_light.area_height_world.x);
	lg.area_height_trans[1] = float(p_light.area_height_world.y);
	lg.area_height_trans[2] = float(p_light.area_height_world.z);
	lg.area_height_trans[3] = p_light.transmittance_bias;

	lg.view_pos_radius[0] = float(p_light.view_pos.x);
	lg.view_pos_radius[1] = float(p_light.view_pos.y);
	lg.view_pos_radius[2] = float(p_light.view_pos.z);
	lg.view_pos_radius[3] = p_light.cull_radius;

	lg.view_dir_sin[0] = float(p_light.view_dir.x);
	lg.view_dir_sin[1] = float(p_light.view_dir.y);
	lg.view_dir_sin[2] = float(p_light.view_dir.z);
	lg.view_dir_sin[3] = p_light.sin_spot_angle;

	lg.policy_lookup[0] = light_mode;
	lg.policy_lookup[1] = p_light.caster_mask;
	lg.policy_lookup[2] = lookup_base;
	lg.policy_lookup[3] = prev_lookup_base;

	lg.rect_id[0] = (rect.min_tx & 0xFFFFu) | ((rect.min_ty & 0xFFFFu) << 16u);
	lg.rect_id[1] = (rect.width & 0xFFFFu) | ((rect.height & 0xFFFFu) << 16u);
	lg.rect_id[2] = stable_id;
	lg.rect_id[3] = (uint32_t(p_light.type) & 3u) | ((stable_id & 0x3FFFFFFFu) << 8u);

	lg.area_cull[0] = float(p_light.area_size.x) * 0.5f;
	lg.area_cull[1] = float(p_light.area_size.y) * 0.5f;
	lg.area_cull[2] = p_light.range;
	lg.area_cull[3] = 1.0f;

	active_lights.push_back(lg);
	active_light_rids.push_back(p_light.light_instance);
	if (p_light.type == NEXTGI_RT_SHADOW_TYPE_DIRECTIONAL) {
		active_dir_count++;
	} else {
		active_local_count++;
	}

	r_flags = NEXTGI_RT_SHADOW_FLAG_SAMPLE;
	return lookup_base;
}

void NextGIRTShadows::render(RenderDataRD *p_render_data, NextGI *p_next_gi, RID p_normal_roughness, RID p_velocity, bool p_velocity_enabled) {
	last_stats = ViewStats();
	last_stats.tile_budget = active_tile_budget;
	last_stats.fallback_lights = active_fallback_count;
	if (!view_active || active_view.is_null() || p_render_data == nullptr || p_render_data->scene_data == nullptr || p_next_gi == nullptr) {
		return;
	}
	if (active_lights.is_empty()) {
		last_stats.atlas_vram_bytes = active_view->vram_bytes;
		return;
	}

	Ref<RenderSceneBuffersRD> rb = p_render_data->render_buffers;
	if (rb.is_null()) {
		return;
	}
	const RID depth_tex = rb->get_depth_texture(0);
	if (depth_tex.is_null() || p_normal_roughness.is_null()) {
		return;
	}

	RenderingDevice *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_COND(rd == nullptr || uniform_set_cache == nullptr || material_storage == nullptr);

	if (!_ensure_alloc_shaders() || !_ensure_trace_shaders(p_next_gi)) {
		return;
	}
	_ensure_blue_noise();

	NextGIRTShadowsView *view = active_view.ptr();
	view->ensure_resources(active_screen_size, active_tile_budget);
	const uint32_t prev_slot = view->history_index & 1u;
	const uint32_t cur_slot = prev_slot ^ 1u;
	view->ensure_dynamic_buffers(active_lights.size(), active_lookup_cursor, cur_slot);

	// Clear current lookup table to 0 (unallocated) so MODE_INIT_LOOKUP only writes 2 header words per light.
	rd->buffer_clear(view->lookup_buffer[cur_slot], 0, active_lookup_cursor * uint32_t(sizeof(uint32_t)));
	rd->buffer_clear(view->alloc_state_buffer, 0, 8u * uint32_t(sizeof(uint32_t)));

	// If TLAS is not traceable yet (e.g. Frame 1 BLAS warmup or empty scene), expose zeroed lookup (lit = 1.0).
	NextGIRTScene *rt_scene = p_next_gi->get_rt_scene();
	if (!view_tlas_ready || rt_scene == nullptr || !rt_scene->is_tlas_traceable() || rt_scene->get_tlas().is_null() ||
			rt_scene->get_shadow_instance_buffer().is_null() || rt_scene->get_shadow_alpha_buffer().is_null()) {
		// Initialize lookup headers so shading shader bounds checks read clean empty tiles.
		rd->buffer_update(view->lights_buffer, 0, active_lights.size() * uint32_t(sizeof(LightGPU)), active_lights.ptr());
		ParamsGPU params;
		params.screen_tiles[0] = uint32_t(active_screen_size.x);
		params.screen_tiles[1] = uint32_t(active_screen_size.y);
		params.screen_tiles[2] = active_tiles_x;
		params.screen_tiles[3] = active_tiles_y;
		params.counts[0] = active_lights.size();
		params.counts[1] = view->tile_budget;
		params.counts[2] = view->frame_counter;
		params.counts[3] = 0u;
		rd->buffer_update(view->params_buffer, 0, sizeof(ParamsGPU), &params);

		const RID init_shader = shaders->alloc_shader.version_get_shader(shaders->alloc_version, Shaders::ALLOC_MODE_INIT_LOOKUP);
		RD::Uniform u_p(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, view->params_buffer);
		RD::Uniform u_l(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, view->lights_buffer);
		RD::Uniform u_lk(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, view->lookup_buffer[cur_slot]);
		RD::Uniform u_ov(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, view->light_overflow_buffer);
		const RID set = uniform_set_cache->get_cache(init_shader, 0, u_p, u_l, u_lk, u_ov);
		if (set.is_valid()) {
			const RD::ComputeListID cl = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(cl, shaders->alloc_pipelines[Shaders::ALLOC_MODE_INIT_LOOKUP]);
			rd->compute_list_bind_uniform_set(cl, set, 0);
			rd->compute_list_dispatch(cl, (active_lights.size() + 63u) / 64u, 1u, 1u);
			rd->compute_list_end();
		}

		bound_mask_atlas = view->mask_atlas[prev_slot];
		bound_tile_lookup = view->lookup_buffer[cur_slot];
		bound_hit_dist_atlas = view->hit_dist_atlas;
		last_stats.lights_traced = active_lights.size();
		last_stats.directional_lights = active_dir_count;
		last_stats.local_lights = active_local_count;
		last_stats.atlas_vram_bytes = view->vram_bytes;
		return;
	}

	// Build ParamsGPU.
	const RenderSceneDataRD &sd = *p_render_data->scene_data;
	const Projection depth_proj = NextGIView::depth_projection(sd);
	const Projection inv_view_proj = NextGIView::inv_view_projection(sd);
	const Projection prev_view_proj = NextGIView::prev_view_projection(sd);
	const Projection inv_proj = NextGIView::inv_projection(sd);
	const Projection prev_depth_proj = NextGIViewMath::depth_projection(sd.prev_cam_projection, sd.flip_y, sd.prev_taa_jitter);
	const Vector4 cur_depth_coeffs = NextGIViewMath::depth_coefficients(depth_proj);
	const Vector4 prev_depth_coeffs = NextGIViewMath::depth_coefficients(prev_depth_proj);

	const float size_x = 2.0f / (Math::abs(float(sd.cam_projection.columns[0][0])) * float(MAX(active_screen_size.x, 1)));
	const float size_y = 2.0f / (Math::abs(float(sd.cam_projection.columns[1][1])) * float(MAX(active_screen_size.y, 1)));
	float footprint_scale = 0.0f;
	float footprint_bias = 0.0f;
	if (sd.cam_projection.is_orthogonal()) {
		footprint_bias = MIN(size_x, size_y);
	} else {
		footprint_scale = MIN(size_x, size_y);
	}
	const float rt_scene_radius = float(GLOBAL_GET("rendering/global_illumination/nextgi/rt_scene_radius"));

	ParamsGPU params;
	NextGIViewMath::store(inv_view_proj, params.inv_view_proj);
	NextGIViewMath::store(prev_view_proj, params.prev_view_proj);
	NextGIViewMath::store(inv_proj, params.inv_projection);
	NextGIViewMath::store_basis(sd.cam_transform.basis, params.view_to_world);
	params.depth_coefficients[0] = float(cur_depth_coeffs.x);
	params.depth_coefficients[1] = float(cur_depth_coeffs.y);
	params.depth_coefficients[2] = float(cur_depth_coeffs.z);
	params.depth_coefficients[3] = float(cur_depth_coeffs.w);
	params.prev_depth_coeffs[0] = float(prev_depth_coeffs.x);
	params.prev_depth_coeffs[1] = float(prev_depth_coeffs.y);
	params.prev_depth_coeffs[2] = float(prev_depth_coeffs.z);
	params.prev_depth_coeffs[3] = float(prev_depth_coeffs.w);
	params.camera_pos_offset[0] = float(sd.cam_transform.origin.x);
	params.camera_pos_offset[1] = float(sd.cam_transform.origin.y);
	params.camera_pos_offset[2] = float(sd.cam_transform.origin.z);
	params.camera_pos_offset[3] = NEXTGI_RT_SHADOW_MAX_ORIGIN_OFFSET;
	params.footprint_misc[0] = footprint_scale;
	params.footprint_misc[1] = footprint_bias;
	params.footprint_misc[2] = MAX(rt_scene_radius, 64.0f);
	params.footprint_misc[3] = 0.0f;
	params.screen_tiles[0] = uint32_t(active_screen_size.x);
	params.screen_tiles[1] = uint32_t(active_screen_size.y);
	params.screen_tiles[2] = active_tiles_x;
	params.screen_tiles[3] = active_tiles_y;
	params.counts[0] = active_lights.size();
	params.counts[1] = view->tile_budget;
	params.counts[2] = view->frame_counter++;
	uint32_t pflags = 0u;
	if (view->has_history) {
		pflags |= 1u << 0u; // RT_SHADOW_PARAM_FLAG_HAS_HISTORY
	}
	if (p_velocity_enabled && p_velocity.is_valid()) {
		pflags |= 1u << 1u; // RT_SHADOW_PARAM_FLAG_VELOCITY
	}
	if (sd.cam_orthogonal) {
		pflags |= 1u << 2u; // RT_SHADOW_PARAM_FLAG_ORTHOGONAL
	}
	params.counts[3] = pflags;

	rd->buffer_update(view->params_buffer, 0, sizeof(ParamsGPU), &params);
	rd->buffer_update(view->lights_buffer, 0, active_lights.size() * uint32_t(sizeof(LightGPU)), active_lights.ptr());

	const RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	rd->draw_command_begin_label("NextGI: Standalone RT Shadows");

	// Pass 1: Per-16x16-tile depth min/max.
	{
		const RID s = shaders->alloc_shader.version_get_shader(shaders->alloc_version, Shaders::ALLOC_MODE_DEPTH_MINMAX);
		RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, depth_tex }));
		RD::Uniform u_params(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, view->params_buffer);
		RD::Uniform u_zmm(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, view->tile_depth_minmax_buffer);
		const RID set = uniform_set_cache->get_cache(s, 0, u_depth, u_params, u_zmm);
		if (set.is_valid()) {
			const RD::ComputeListID cl = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(cl, shaders->alloc_pipelines[Shaders::ALLOC_MODE_DEPTH_MINMAX]);
			rd->compute_list_bind_uniform_set(cl, set, 0);
			rd->compute_list_dispatch(cl, active_tiles_x, active_tiles_y, 1u);
			rd->compute_list_end();
		}
	}

	// Pass 2: Initialize lookup headers and clear per-light overflow flags.
	{
		const RID s = shaders->alloc_shader.version_get_shader(shaders->alloc_version, Shaders::ALLOC_MODE_INIT_LOOKUP);
		RD::Uniform u_params(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, view->params_buffer);
		RD::Uniform u_lights(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, view->lights_buffer);
		RD::Uniform u_lookup(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, view->lookup_buffer[cur_slot]);
		RD::Uniform u_ov(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, view->light_overflow_buffer);
		const RID set = uniform_set_cache->get_cache(s, 0, u_params, u_lights, u_lookup, u_ov);
		if (set.is_valid()) {
			const RD::ComputeListID cl = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(cl, shaders->alloc_pipelines[Shaders::ALLOC_MODE_INIT_LOOKUP]);
			rd->compute_list_bind_uniform_set(cl, set, 0);
			rd->compute_list_dispatch(cl, (active_lights.size() + 63u) / 64u, 1u, 1u);
			rd->compute_list_end();
		}
	}

	// Pass 3: Allocate active 16x16 tiles within tile_budget.
	if (active_max_rect_w > 0u && active_max_rect_h > 0u) {
		const RID s = shaders->alloc_shader.version_get_shader(shaders->alloc_version, Shaders::ALLOC_MODE_ALLOC_TILES);
		RD::Uniform u_params(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, view->params_buffer);
		RD::Uniform u_lights(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, view->lights_buffer);
		RD::Uniform u_zmm(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, view->tile_depth_minmax_buffer);
		RD::Uniform u_state(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, view->alloc_state_buffer);
		RD::Uniform u_lookup(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, view->lookup_buffer[cur_slot]);
		RD::Uniform u_list(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, view->tile_list_buffer);
		RD::Uniform u_owner(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, view->tile_owner_buffer[cur_slot]);
		RD::Uniform u_ov(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, view->light_overflow_buffer);
		const RID set = uniform_set_cache->get_cache(s, 0, u_params, u_lights, u_zmm, u_state, u_lookup, u_list, u_owner, u_ov);
		if (set.is_valid()) {
			const RD::ComputeListID cl = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(cl, shaders->alloc_pipelines[Shaders::ALLOC_MODE_ALLOC_TILES]);
			rd->compute_list_bind_uniform_set(cl, set, 0);
			rd->compute_list_dispatch(cl, (active_max_rect_w + 7u) / 8u, (active_max_rect_h + 7u) / 8u, active_lights.size());
			rd->compute_list_end();
		}
	}

	// Pass 4: Finalize indirect dispatch arguments.
	{
		const RID s = shaders->alloc_shader.version_get_shader(shaders->alloc_version, Shaders::ALLOC_MODE_FINALIZE);
		RD::Uniform u_params(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, view->params_buffer);
		RD::Uniform u_state(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, view->alloc_state_buffer);
		RD::Uniform u_ind(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, view->indirect_buffer);
		const RID set = uniform_set_cache->get_cache(s, 0, u_params, u_state, u_ind);
		if (set.is_valid()) {
			const RD::ComputeListID cl = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(cl, shaders->alloc_pipelines[Shaders::ALLOC_MODE_FINALIZE]);
			rd->compute_list_bind_uniform_set(cl, set, 0);
			rd->compute_list_dispatch(cl, 1u, 1u, 1u);
			rd->compute_list_end();
		}
	}

	// Pass 5: Ray-query shadow trace (variant 1 when alpha-tested casters need bindless texture / #130 retrace).
	const bool alpha_variant = rt_scene->wants_shadow_alpha_variant() && rt_scene->ensure_shadow_texture_table().is_valid();
	NextGIHitMaterials *hit_materials = alpha_variant && p_next_gi->get_hit_availability_for(NEXTGI_HIT_CONSUMER_MATERIAL_RETRACE) == NEXTGI_HIT_AVAILABLE ? p_next_gi->get_hit_materials() : nullptr;
	const uint32_t retrace_capacity = hit_materials != nullptr && rt_scene->wants_shadow_retrace() && hit_materials->has_current_records() ? uint32_t(NEXTGI_SHADOW_RETRACE_MAX_RECORDS) : 0u;
	if (alpha_variant && retrace_buffer.is_null()) {
		retrace_buffer = rd->storage_buffer_create(nextgi_shadow_retrace_buffer_size(NEXTGI_SHADOW_RETRACE_MAX_RECORDS));
		rd->set_resource_name(retrace_buffer, "NextGI RT Shadow Retrace Records");
		view->invalidate_trace_set();
	}
	if (alpha_variant) {
		const uint32_t header[4] = { 0u, retrace_capacity, 0u, 0u };
		rd->buffer_update(retrace_buffer, 0, sizeof(header), header);
	}

	const int trace_variant = alpha_variant ? 1 : 0;
	const RID trace_shader_rid = shaders->trace_shader.version_get_shader(shaders->trace_version, trace_variant);
	LocalVector<RD::Uniform> alpha_uniforms;
	const uint64_t alpha_key = alpha_variant ? rt_scene->append_shadow_alpha_uniforms(12, 16, alpha_uniforms) : 0u;
	if (alpha_variant) {
		const RID table = rt_scene->ensure_shadow_texture_table();
		if (shadow_texture_set.is_null() || shadow_texture_set_table != table || !rd->uniform_set_is_valid(shadow_texture_set)) {
			if (shadow_texture_set.is_valid() && rd->uniform_set_is_valid(shadow_texture_set)) {
				rd->free_rid(shadow_texture_set);
			}
			RD::Uniform table_uniform(RD::UNIFORM_TYPE_BINDLESS_TABLE, 0, table);
			shadow_texture_set = rd->uniform_set_create(table_uniform, trace_shader_rid, 1);
			shadow_texture_set_table = table;
		}
	}

	uint64_t trace_key = hash_murmur3_one_64(uint64_t(trace_variant));
	trace_key = hash_murmur3_one_64(rt_scene->get_tlas().get_id(), trace_key);
	trace_key = hash_murmur3_one_64(rt_scene->get_shadow_instance_buffer().get_id(), trace_key);
	trace_key = hash_murmur3_one_64(rt_scene->get_shadow_alpha_buffer().get_id(), trace_key);
	trace_key = hash_murmur3_one_64(view->params_buffer.get_id(), trace_key);
	trace_key = hash_murmur3_one_64(view->lights_buffer.get_id(), trace_key);
	trace_key = hash_murmur3_one_64(view->tile_list_buffer.get_id(), trace_key);
	trace_key = hash_murmur3_one_64(view->alloc_state_buffer.get_id(), trace_key);
	trace_key = hash_murmur3_one_64(depth_tex.get_id(), trace_key);
	trace_key = hash_murmur3_one_64(p_normal_roughness.get_id(), trace_key);
	trace_key = hash_murmur3_one_64(blue_noise_texture.get_id(), trace_key);
	trace_key = hash_murmur3_one_64(view->mask_atlas[2].get_id(), trace_key);
	trace_key = hash_murmur3_one_64(view->hit_dist_atlas.get_id(), trace_key);
	trace_key = hash_murmur3_one_64(alpha_key, trace_key);
	if (alpha_variant) {
		trace_key = hash_murmur3_one_64(retrace_buffer.get_id(), trace_key);
	}

	if (view->trace_uniform_set.is_null() || view->trace_uniform_key != trace_key || !rd->uniform_set_is_valid(view->trace_uniform_set)) {
		view->invalidate_trace_set();
		LocalVector<RD::Uniform> u;
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, rt_scene->get_tlas()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, rt_scene->get_shadow_instance_buffer()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, rt_scene->get_shadow_alpha_buffer()));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, view->params_buffer));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, view->lights_buffer));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, view->tile_list_buffer));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, view->alloc_state_buffer));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ nearest_sampler, depth_tex })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ nearest_sampler, p_normal_roughness })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 9, Vector<RID>({ nearest_sampler, blue_noise_texture })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 10, Vector<RID>({ view->mask_atlas[2] })));
		u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 11, Vector<RID>({ view->hit_dist_atlas })));
		if (alpha_variant) {
			for (const RD::Uniform &au : alpha_uniforms) {
				u.push_back(au);
			}
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 17, retrace_buffer));
		}
		view->trace_uniform_set = rd->uniform_set_create(u, trace_shader_rid, 0);
		view->trace_uniform_key = trace_key;
	}

	if (view->trace_uniform_set.is_valid()) {
		const RD::ComputeListID cl = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(cl, shaders->trace_pipelines[trace_variant]);
		rd->compute_list_bind_uniform_set(cl, view->trace_uniform_set, 0);
		if (alpha_variant && shadow_texture_set.is_valid()) {
			rd->compute_list_bind_uniform_set(cl, shadow_texture_set, 1);
		}
		rd->compute_list_dispatch_indirect(cl, view->indirect_buffer, 0);
		rd->compute_list_end();
	}

	// Optional Pass 5b: #145/#130 material retrace for custom shader alpha/discard casters.
	bool retrace_ran = false;
	if (retrace_capacity > 0u && hit_materials != nullptr &&
			hit_materials->trace_shadow_retrace(rt_scene, retrace_buffer, retrace_capacity, float(sd.time))) {
		const RID resolve_shader = shaders->alloc_shader.version_get_shader(shaders->alloc_version, Shaders::ALLOC_MODE_RETRACE_RESOLVE);
		RD::Uniform u_raw(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ view->mask_atlas[2] }));
		RD::Uniform u_hit(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ view->hit_dist_atlas }));
		RD::Uniform u_ret(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, retrace_buffer);
		const RID set = uniform_set_cache->get_cache(resolve_shader, 0, u_raw, u_hit, u_ret);
		if (set.is_valid()) {
			const RD::ComputeListID cl = rd->compute_list_begin();
			rd->compute_list_bind_compute_pipeline(cl, shaders->alloc_pipelines[Shaders::ALLOC_MODE_RETRACE_RESOLVE]);
			rd->compute_list_bind_uniform_set(cl, set, 0);
			rd->compute_list_dispatch(cl, (retrace_capacity + 63u) / 64u, 1u, 1u);
			rd->compute_list_end();
			retrace_ran = true;
		}
	}

	// Pass 6: Per-light shadow denoiser (temporal + 2x spatial + prev guide copy).
	if (denoiser == nullptr) {
		denoiser = memnew(NextGIDenoise(false));
	}
	NextGIDenoise::ShadowDenoiseParams dp;
	dp.raw_mask = view->mask_atlas[2];
	dp.history_mask_prev = view->mask_atlas[prev_slot];
	dp.history_mask_cur = view->mask_atlas[cur_slot];
	dp.history_len_prev = view->history_len[prev_slot];
	dp.history_len_cur = view->history_len[cur_slot];
	dp.hit_dist = view->hit_dist_atlas;
	dp.depth = depth_tex;
	dp.normal_roughness = p_normal_roughness;
	dp.velocity = (p_velocity_enabled && p_velocity.is_valid()) ? p_velocity : p_normal_roughness;
	dp.prev_depth_normal = view->prev_depth_normal;
	dp.params_buffer = view->params_buffer;
	dp.lights_buffer = view->lights_buffer;
	dp.tile_list_buffer = view->tile_list_buffer;
	dp.alloc_state_buffer = view->alloc_state_buffer;
	dp.cur_lookup_buffer = view->lookup_buffer[cur_slot];
	dp.prev_lookup_buffer = view->lookup_buffer[prev_slot];
	dp.prev_owner_buffer = view->tile_owner_buffer[prev_slot];
	dp.indirect_buffer = view->indirect_buffer;
	dp.screen_size = active_screen_size;
	denoiser->shadow_denoise(dp);

	rd->draw_command_end_label();

	// Final spatially denoised output resides in mask_atlas[prev_slot], while mask_atlas[cur_slot]
	// preserves clean pre-spatial temporal accumulation for next frame's history!
	bound_mask_atlas = view->mask_atlas[prev_slot];
	bound_tile_lookup = view->lookup_buffer[cur_slot];
	bound_hit_dist_atlas = view->hit_dist_atlas;

	// Flip history index so cur_slot becomes next frame's prev_slot.
	view->history_index = cur_slot;
	view->has_history = true;
	view->prev_lookup_bases = view->cur_lookup_bases;
	view->last_frame = RSG::rasterizer != nullptr ? RSG::rasterizer->get_frame_number() : 0u;

	// Async readback for tile allocation / overflow telemetry and overflow light fallback feedback.
	if (view->readback != nullptr && view->readback->pending.set_if_clear()) {
		view->readback->submitted_lights = active_light_rids;
		const uint64_t state_ptr = reinterpret_cast<uint64_t>(view->readback);
		view->readback->refs.ref();
		if (rd->buffer_get_data_async(view->light_overflow_buffer, callable_mp_static(&_rt_shadow_overflow_lights_received).bind(state_ptr), 0, active_lights.size() * uint32_t(sizeof(uint32_t))) != OK) {
			_release_readback_state(view->readback);
		}
		view->readback->refs.ref();
		if (rd->buffer_get_data_async(view->alloc_state_buffer, callable_mp_static(&_rt_shadow_alloc_state_received).bind(state_ptr), 0, 4u * uint32_t(sizeof(uint32_t))) != OK) {
			view->readback->pending.clear();
			_release_readback_state(view->readback);
		}
	}

	last_stats.lights_traced = active_lights.size();
	last_stats.directional_lights = active_dir_count;
	last_stats.local_lights = active_local_count;
	last_stats.tiles_allocated = view->last_tiles_allocated;
	last_stats.tile_budget = view->tile_budget;
	last_stats.overflow_tiles = view->last_overflow_tiles;
	last_stats.overflow_lights = view->last_overflow_lights;
	last_stats.fallback_lights = active_fallback_count;
	last_stats.atlas_vram_bytes = view->vram_bytes;
	last_stats.retrace_active = retrace_ran;
}

void NextGIRTShadows::end_view() {
	view_active = false;
	active_view.unref();
	active_rt_scene = nullptr;
}
