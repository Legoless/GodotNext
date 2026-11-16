/**************************************************************************/
/*  virtual_shadow_map_storage.cpp                                        */
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

#include "virtual_shadow_map_storage.h"

#include "core/math/random_pcg.h"
#include "core/os/os.h"
#include "core/templates/hash_set.h"
#include "core/variant/array.h"
#include "servers/rendering/renderer_compositor.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"

#include <cmath>
#include <cstring>

namespace RendererRD {

// GodotNext: vsm_page_pool (#220).

VirtualShadowMapStorage *VirtualShadowMapStorage::singleton = nullptr;

bool VirtualShadowMapStorage::is_dev_switch_on(PoolQuality *r_quality, uint32_t *r_pool_pages) {
#ifdef DEBUG_ENABLED
	// -2: not read yet, -1: off, otherwise the pool quality.
	static int switch_state = -2;
	static uint32_t switch_pages = 0;
	if (switch_state == -2) {
		switch_state = -1;
		const String value = OS::get_singleton() != nullptr ? OS::get_singleton()->get_environment("GODOT_DEV_VSM").strip_edges().to_lower() : String();
		if (value == "1" || value == "high") {
			switch_state = POOL_QUALITY_HIGH;
		} else if (value == "low") {
			switch_state = POOL_QUALITY_LOW;
		} else if (value == "medium") {
			switch_state = POOL_QUALITY_MEDIUM;
		} else if (value == "epic") {
			switch_state = POOL_QUALITY_EPIC;
		} else if (value.ends_with("%") && value.trim_suffix("%").is_valid_int() && value.trim_suffix("%").to_int() > 0 && value.trim_suffix("%").to_int() <= 400) {
			// GodotNext: vsm_page_marking (#219). A share of the default High pool, for overflow tests.
			switch_state = POOL_QUALITY_HIGH;
			switch_pages = MAX(uint32_t(uint64_t(pool_quality_pages(POOL_QUALITY_HIGH)) * uint64_t(value.trim_suffix("%").to_int()) / 100u), 1u);
		} else if (!value.is_empty() && value != "0") {
			WARN_PRINT(vformat("GODOT_DEV_VSM=%s is not 1, low, medium, high, epic or a percentage of the High pool; virtual shadow maps stay off.", value));
		}
		if (switch_state >= 0 && switch_pages == 0) {
			switch_pages = pool_quality_pages(PoolQuality(switch_state));
		}
	}
	if (switch_state < 0) {
		return false;
	}
	if (r_quality != nullptr) {
		*r_quality = PoolQuality(switch_state);
	}
	if (r_pool_pages != nullptr) {
		*r_pool_pages = switch_pages;
	}
	return true;
#else
	return false;
#endif
}

VirtualShadowMapStorage::VirtualShadowMapStorage() {
	singleton = this;
	PoolQuality quality = POOL_QUALITY_HIGH;
	uint32_t pages = 0;
	if (is_dev_switch_on(&quality, &pages)) {
		active_quality = quality;
		active_cache_frames = cache_frames(quality);
		active = pool_create(pages);
		if (active) {
			print_line(vformat("VSM: GODOT_DEV_VSM is set: virtual shadow maps are active with the %s pool (%d pages, %d MiB).", pool_quality_name(quality), get_pool_page_count(), int64_t(pool_bytes(get_pool_page_count()) >> 20)));
		}
	}
}

VirtualShadowMapStorage::~VirtualShadowMapStorage() {
	free_all();
	_free_check_buffers();
	_free_marking_resources();
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr && table_buffer.is_valid()) {
		rd->free_rid(table_buffer);
	}
	if (rd != nullptr && map_data_buffer.is_valid()) {
		rd->free_rid(map_data_buffer);
	}
	if (rd != nullptr && map_state_buffer.is_valid()) {
		rd->free_rid(map_state_buffer);
	}
	map_data_buffer = RID();
	map_state_buffer = RID();
	map_buffers_maps = 0;
	table_buffer = RID();
	table_buffer_bytes = 0;
	if (mark_version.is_valid()) {
		mark_shader.version_free(mark_version);
		mark_version = RID();
	}
	if (allocate_version.is_valid()) {
		allocate_shader.version_free(allocate_version);
		allocate_version = RID();
	}
	if (shader_version.is_valid()) {
		shader.version_free(shader_version);
		shader_version = RID();
	}
	shader_ready = false;
	singleton = nullptr;
}

bool VirtualShadowMapStorage::_ensure_shader() {
	if (shader_ready) {
		return true;
	}
	if (shader_failed) {
		return false;
	}
	Vector<String> modes;
	modes.push_back("\n#define MODE_SCROLL\n");
	modes.push_back("\n#define MODE_LOOKUP_CHECK\n");
	shader.initialize(modes, get_glsl_defines());
	shader_version = shader.version_create();
	for (int mode = 0; mode < SHADER_MODE_MAX; mode++) {
		const RID shader_rid = shader.version_get_shader(shader_version, mode);
		if (shader_rid.is_null()) {
			// Freeing the version frees the pipelines made from it.
			shader.version_free(shader_version);
			shader_version = RID();
			for (RID &pipeline : pipelines) {
				pipeline = RID();
			}
			shader_failed = true;
			ERR_FAIL_V_MSG(false, "The virtual shadow map page-table shader did not compile; the scroll pass and the self-check stay off.");
		}
		pipelines[mode] = RD::get_singleton()->compute_pipeline_create(shader_rid);
	}
	shader_ready = true;
	return true;
}

RID VirtualShadowMapStorage::_shader(ShaderMode p_mode) {
	return shader.version_get_shader(shader_version, p_mode);
}

bool VirtualShadowMapStorage::_table_resize(uint32_t p_old_maps, uint32_t p_new_maps) {
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	const uint64_t old_bytes = table_bytes(p_old_maps);
	const uint64_t new_bytes = table_bytes(p_new_maps);
	ERR_FAIL_COND_V(new_bytes <= old_bytes || new_bytes > UINT32_MAX, false);
	const RID new_buffer = rd->storage_buffer_create(uint32_t(new_bytes));
	ERR_FAIL_COND_V(new_buffer.is_null(), false);
	rd->set_resource_name(new_buffer, "VSM Page Table");
	rd->buffer_clear(new_buffer, uint32_t(old_bytes), uint32_t(new_bytes - old_bytes));
	if (table_buffer.is_valid()) {
		rd->buffer_copy(table_buffer, new_buffer, 0, 0, uint32_t(old_bytes));
		rd->free_rid(table_buffer);
	}
	table_buffer = new_buffer;
	table_buffer_bytes = new_bytes;
	// GodotNext: vsm_page_marking (#219). The per-map buffers grow with the table.
	_resize_map_buffers(p_new_maps);
	return true;
}

void VirtualShadowMapStorage::_table_clear(uint32_t p_first_map, uint32_t p_map_count) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || table_buffer.is_null() || p_map_count == 0) {
		return;
	}
	// The physical pages these entries held go back to the free list in the allocation pass (#219),
	// which finds their entries cleared.
	rd->buffer_clear(table_buffer, uint32_t(table_bytes(p_first_map)), uint32_t(table_bytes(p_map_count)));
	// GodotNext: vsm_page_marking (#219). The maps no longer project anywhere, and the next light that
	// gets them starts without their overflow bias.
	for (uint32_t map = p_first_map; map < p_first_map + p_map_count && map < map_data.size(); map++) {
		_set_map_data(map, MapData());
	}
	if (map_state_buffer.is_valid() && p_first_map < map_buffers_maps) {
		const uint32_t count = MIN(p_map_count, map_buffers_maps - p_first_map);
		rd->buffer_clear(map_state_buffer, (MAP_STATE_BIAS * map_buffers_maps + p_first_map) * uint32_t(sizeof(uint32_t)), count * uint32_t(sizeof(uint32_t)));
	}
}

void VirtualShadowMapStorage::_table_scroll(uint32_t p_map, const ClipmapScroll &p_scroll) {
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL(rd);
	ERR_FAIL_COND(table_buffer.is_null());
	if (!_ensure_shader()) {
		return;
	}
	ScrollPushConstant push;
	push.map = p_map;
	for (uint32_t mip = 0; mip < MIP_COUNT; mip++) {
		const ScrollRange &range = p_scroll.mips[mip];
		push.mip_ranges[mip] = (range.col_start & 0xFFu) | ((range.col_count & 0xFFu) << 8) | ((range.row_start & 0xFFu) << 16) | ((range.row_count & 0xFFu) << 24);
	}
	const RID shader_rid = _shader(SHADER_MODE_SCROLL);
	RD::Uniform u_table(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, table_buffer);
	const RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache(shader_rid, 0, u_table);
	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, pipelines[SHADER_MODE_SCROLL]);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	rd->compute_list_set_push_constant(compute_list, &push, sizeof(ScrollPushConstant));
	rd->compute_list_dispatch_threads(compute_list, ENTRIES_PER_MAP, 1, 1);
	rd->compute_list_end();
}

bool VirtualShadowMapStorage::_pool_resize(uint32_t p_page_count) {
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	if (p_page_count == 0) {
		if (pool_texture.is_valid()) {
			rd->free_rid(pool_texture);
		}
		pool_texture = RID();
		pool_texture_size = Vector2i();
		pool_driver_bytes = 0;
		return true;
	}
	// The new pool is created before the old one goes, so that a failure keeps the old pool. The
	// old one costs no extra peak memory: RenderingDevice frees it only frames later anyway.
	const Vector2i grid = pool_page_grid(p_page_count);
	RD::TextureFormat format;
	format.format = RD::DATA_FORMAT_R32_UINT;
	format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	format.width = uint32_t(grid.x) * PAGE_SIZE;
	format.height = uint32_t(grid.y) * PAGE_SIZE;
	format.array_layers = POOL_LAYERS;
	format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_STORAGE_ATOMIC_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	ERR_FAIL_COND_V_MSG(!rd->texture_is_format_supported_for_usage(format.format, format.usage_bits), false, "The device cannot use R32_UINT storage images with atomics, which the virtual shadow map pool needs.");
	const uint64_t textures_before = rd->get_memory_usage(RD::MEMORY_TEXTURES);
	const RID new_texture = rd->texture_create(format, RD::TextureView());
	ERR_FAIL_COND_V_MSG(new_texture.is_null(), false, vformat("The virtual shadow map pool of %d pages could not be created; the previous pool stays.", p_page_count));
	const uint64_t new_driver_bytes = rd->get_memory_usage(RD::MEMORY_TEXTURES) - textures_before;
	if (pool_texture.is_valid()) {
		rd->free_rid(pool_texture);
	}
	pool_texture = new_texture;
	pool_driver_bytes = new_driver_bytes;
	pool_texture_size = Vector2i(int32_t(format.width), int32_t(format.height));
	rd->set_resource_name(pool_texture, "VSM Physical Pool");
	// Reversed-Z: 0 is the far plane, the value imageAtomicMax keeps when nothing is nearer.
	rd->texture_clear(pool_texture, Color(0, 0, 0, 0), 0, 1, 0, POOL_LAYERS);
	return true;
}

void VirtualShadowMapStorage::begin_light_update() {
	view_directional.clear();
}

void VirtualShadowMapStorage::update_light(RID p_light_instance, const LightUpdate &p_light, uint32_t &r_map_base, uint32_t &r_flags) {
	r_map_base = MAP_NONE;
	r_flags = 0;
	if (!active) {
		return;
	}
	// Stale lights are evicted by frame: every view of a frame stamps the lights it draws.
	if (RSG::rasterizer != nullptr && RSG::rasterizer->get_frame_number() != get_frame()) {
		set_frame(RSG::rasterizer->get_frame_number());
	}
	if (!p_light.casts_shadow) {
		map_free(p_light_instance);
		return;
	}
	uint32_t first = MAP_NONE;
	if (p_light.kind == MAP_KIND_DIRECTIONAL) {
		if (p_light.move_clipmaps) {
			first = directional_update(p_light_instance, p_light.transform.basis, p_light.camera_position, p_light.max_distance);
		} else {
			// Reflection probes read the maps of the main view's camera.
			const MapRange *range = map_touch(p_light_instance);
			if (range != nullptr && range->kind == MAP_KIND_DIRECTIONAL) {
				first = range->first;
			}
		}
	} else {
		first = map_allocate(p_light_instance, p_light.kind);
	}
	const MapRange *range = map_get(p_light_instance);
	if (first == MAP_NONE || range == nullptr) {
		return;
	}
	r_map_base = first;
	r_flags = light_flags(range->kind, range->count);

	// GodotNext: vsm_page_marking (#219). Where world positions land in each map.
	if (range->kind == MAP_KIND_DIRECTIONAL) {
		if (p_light.move_clipmaps) {
			for (uint32_t level = 0; level < range->count; level++) {
				_set_map_data(first + level, map_data_clipmap(p_light.transform.basis, level, get_clipmap_origin(p_light_instance, level), p_light.max_distance));
			}
		}
		if (view_directional.size() < MAX_DIRECTIONAL_LIGHTS) {
			DirectionalEntry entry;
			entry.map_base = r_map_base;
			entry.flags = r_flags;
			entry.max_distance = p_light.max_distance;
			entry.fog_energy = p_light.fog_energy;
			view_directional.push_back(entry);
		}
	} else {
		for (uint32_t face = 0; face < range->count; face++) {
			_set_map_data(first + face, map_data_face(range->kind, face, p_light.transform, p_light.range, p_light.spot_angle));
		}
	}
}

void VirtualShadowMapStorage::light_instance_freed(RID p_light_instance) {
	map_free(p_light_instance);
}

Dictionary VirtualShadowMapStorage::get_debug_info() const {
	Dictionary info;
	info["active"] = active;
	info["quality"] = active ? String(pool_quality_name(active_quality)) : String();
	info["pool_pages"] = int64_t(get_pool_page_count());
	info["pool_grid"] = pool_page_grid(get_pool_page_count());
	info["pool_texture_size"] = pool_texture_size;
	info["pool_layers"] = int64_t(POOL_LAYERS);
	info["pool_bytes"] = int64_t(pool_bytes(get_pool_page_count()));
	info["pool_driver_bytes"] = int64_t(pool_driver_bytes);
	info["pool_reallocations"] = int64_t(get_pool_reallocation_count());
	info["page_table_maps"] = int64_t(get_table_capacity());
	info["page_table_bytes"] = int64_t(table_buffer_bytes);
	info["maps_allocated"] = int64_t(get_allocated_map_count());
	info["maps_free"] = int64_t(get_free_map_count());
	info["free_ranges"] = int64_t(get_free_range_count());
	info["lights"] = int64_t(get_light_count());
	info["scroll_passes"] = int64_t(get_scroll_pass_count());
	info["failed_allocations"] = int64_t(get_failed_allocation_count());
	info["map_allocations"] = int64_t(get_map_allocation_count());
	info["evicted_lights"] = int64_t(get_evicted_light_count());
	info["evicted_maps"] = int64_t(get_evicted_map_count());
	info["frame"] = int64_t(get_frame());
	info["page_size"] = int64_t(PAGE_SIZE);
	info["virtual_size"] = int64_t(VIRTUAL_SIZE);
	info["mip_count"] = int64_t(MIP_COUNT);
	info["entries_per_map"] = int64_t(ENTRIES_PER_MAP);
	info["map_table_bytes"] = int64_t(MAP_TABLE_BYTES);
	info["table_growth_maps"] = int64_t(TABLE_GROWTH_MAPS);
	info["table_max_maps"] = int64_t(TABLE_MAX_MAPS);
	info["clipmap_level0_extent"] = CLIPMAP_LEVEL0_EXTENT;
	info["clipmap_max_levels"] = int64_t(CLIPMAP_MAX_LEVELS);
	Array quality_pages;
	for (uint32_t quality = 0; quality < POOL_QUALITY_MAX; quality++) {
		quality_pages.push_back(int64_t(pool_quality_pages(PoolQuality(quality))));
	}
	info["pool_quality_pages"] = quality_pages;
	// GodotNext: vsm_page_marking (#219). The last marked view's counters (read back: debugging only).
	info["marking_ready"] = marking_ready;
	info["marked_views"] = int64_t(marked_views);
	info["cache_frames"] = int64_t(active_cache_frames);
	info["lod_bias"] = Vector2i(lod_bias_directional, lod_bias_local);
	info["request_capacity"] = int64_t(request_buffer_capacity);
	if (state_buffer.is_valid() && RD::get_singleton() != nullptr) {
		const Vector<uint8_t> bytes = RD::get_singleton()->buffer_get_data(state_buffer, 0, STATE_SIZE * uint32_t(sizeof(uint32_t)));
		if (uint32_t(bytes.size()) == STATE_SIZE * sizeof(uint32_t)) {
			const uint32_t *state = reinterpret_cast<const uint32_t *>(bytes.ptr());
			Dictionary pages;
			pages["requests"] = int64_t(state[STATE_REQUESTS]);
			pages["requests_dropped"] = int64_t(state[STATE_REQUESTS_DROPPED]);
			pages["available"] = int64_t(state[STATE_AVAILABLE]);
			pages["overflow"] = int64_t(state[STATE_OVERFLOW]);
			pages["new"] = int64_t(state[STATE_NEW]);
			pages["kept"] = int64_t(state[STATE_KEPT]);
			pages["evicted"] = int64_t(state[STATE_EVICTED]);
			pages["orphaned"] = int64_t(state[STATE_ORPHANED]);
			pages["stolen"] = int64_t(state[STATE_STOLEN]);
			pages["dirty"] = int64_t(state[STATE_DIRTY]);
			pages["dirty_directional"] = int64_t(state[STATE_DIRTY_DIRECTIONAL]);
			pages["coarse"] = int64_t(state[STATE_COARSE]);
			pages["cached"] = int64_t(state[STATE_CACHED]);
			pages["max_map_requests"] = int64_t(state[STATE_MAX_MAP_REQUESTS]);
			pages["biased_maps"] = int64_t(state[STATE_BIASED_MAPS]);
			pages["total_overflow"] = int64_t(state[STATE_TOTAL_OVERFLOW]);
			pages["overflow_views"] = int64_t(state[STATE_OVERFLOW_VIEWS]);
			pages["total_dropped"] = int64_t(state[STATE_TOTAL_DROPPED]);
			pages["views"] = int64_t(state[STATE_VIEWS]);
			info["pages"] = pages;
		}
	}
	return info;
}

namespace {

// Light-instance ids for the self-check. RID_Owner ids keep bit 63 clear, so these never meet a
// real light instance.
RID _check_rid(uint64_t p_index) {
	return RID::from_uint64((uint64_t(1) << 63) | p_index);
}

uint32_t _float_bits(float p_value) {
	uint32_t bits = 0;
	memcpy(&bits, &p_value, sizeof(uint32_t));
	return bits;
}

float _bits_float(uint32_t p_bits) {
	float value = 0.0f;
	memcpy(&value, &p_bits, sizeof(float));
	return value;
}

} // namespace

Dictionary VirtualShadowMapStorage::run_self_check(uint32_t p_seed, uint32_t p_query_count, uint32_t p_cycles) {
	Dictionary result;
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, result);
	if (!_ensure_shader()) {
		result["error"] = "shader";
		return result;
	}
	RandomPCG rng(p_seed == 0 ? 0x76736d32ull : uint64_t(p_seed));
	uint64_t next_rid = 1;

	const uint32_t pool_pages_before = get_pool_page_count();
	const uint32_t capacity_before = get_table_capacity();
	const uint32_t allocated_before = get_allocated_map_count();
	const uint32_t lights_before = get_light_count();
	// The check's allocations may evict stale real lights (map_allocate()); the leak counts allow for them.
	const uint64_t evicted_lights_before = get_evicted_light_count();
	const uint64_t evicted_maps_before = get_evicted_map_count();
	result["active"] = active;
	result["page_table_maps_before"] = int64_t(capacity_before);

	// The check's lights. Every return path frees them and puts the pool back as it was.
	struct CheckLight {
		RID rid;
		MapKind kind = MAP_KIND_SPOT;
		uint32_t levels = 1;
		uint32_t first = MAP_NONE;
		uint32_t count = 0;
	};
	CheckLight check_lights[5];
	LocalVector<RID> live;
	auto restore = [&]() {
		for (const CheckLight &light : check_lights) {
			if (light.rid.is_valid()) {
				map_free(light.rid);
			}
		}
		for (const RID &rid : live) {
			map_free(rid);
		}
		live.clear();
		if (pool_pages_before == 0) {
			pool_free();
		} else {
			pool_create(pool_pages_before);
		}
	};

	// 1. The pool at each quality: page grid, texture size, computed bytes and the driver's bytes.
	Array pools;
	for (uint32_t quality = 0; quality < POOL_QUALITY_MAX; quality++) {
		const uint32_t pages = pool_quality_pages(PoolQuality(quality));
		Dictionary pool;
		pool["quality"] = String(pool_quality_name(PoolQuality(quality)));
		pool["pages"] = int64_t(pages);
		pool["created"] = pool_create(pages);
		pool["grid"] = pool_page_grid(pages);
		pool["texture_size"] = pool_texture_size;
		pool["layers"] = int64_t(POOL_LAYERS);
		pool["bytes"] = int64_t(pool_bytes(pages));
		pool["texture_bytes"] = int64_t(pool_texture_size.x) * pool_texture_size.y * POOL_TEXEL_BYTES * POOL_LAYERS;
		pool["driver_bytes"] = int64_t(pool_driver_bytes);
		pools.push_back(pool);
	}
	result["pools"] = pools;
	if (get_pool_page_count() == 0) {
		// No pool at any quality (no R32_UINT storage atomics, or no memory): the entries below would
		// have no page grid to point into.
		restore();
		result["error"] = "pool";
		return result;
	}

	// 2. Maps of every kind through the real allocator and page table, plus one map left empty.
	check_lights[0].kind = MAP_KIND_DIRECTIONAL;
	check_lights[0].levels = 8;
	check_lights[1].kind = MAP_KIND_SPOT;
	check_lights[2].kind = MAP_KIND_OMNI;
	check_lights[3].kind = MAP_KIND_AREA;
	check_lights[4].kind = MAP_KIND_SPOT; // Stays empty: every lookup misses.
	bool allocated_all = true;
	for (CheckLight &light : check_lights) {
		light.rid = _check_rid(next_rid++);
		light.first = map_allocate(light.rid, light.kind, light.levels);
		light.count = map_kind_map_count(light.kind, light.levels);
		allocated_all = allocated_all && light.first != MAP_NONE;
	}
	result["check_maps_allocated"] = allocated_all;
	if (!allocated_all) {
		restore();
		result["error"] = "allocation";
		return result;
	}

	// The CPU copy of the table. Only the check maps hold entries.
	const uint32_t capacity = get_table_capacity();
	LocalVector<uint32_t> cpu_table;
	cpu_table.resize(capacity * ENTRIES_PER_MAP);
	memset(cpu_table.ptr(), 0, cpu_table.size() * sizeof(uint32_t));
	const Vector2i pool_grid = pool_page_grid(get_pool_page_count());
	// Residency per mip for each profile: all, coarsest only, half, sparse fine with a full
	// coarsest mip, mip 0 only, and empty.
	static const float profiles[6][MIP_COUNT] = {
		{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
		{ 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f },
		{ 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f },
		{ 0.1f, 0.1f, 0.1f, 0.3f, 0.3f, 0.6f, 1.0f },
		{ 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
		{ 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
	};
	static const uint32_t extra_flags[7] = { ENTRY_REQUESTED, ENTRY_DIRTY_STATIC, ENTRY_DIRTY_DYNAMIC, ENTRY_COARSE, ENTRY_UNCACHED, ENTRY_SCROLLED, ENTRY_OVERFLOW };
	LocalVector<uint32_t> check_maps;
	uint32_t resident_entries = 0;
	for (uint32_t light_index = 0; light_index < 5; light_index++) {
		const CheckLight &light = check_lights[light_index];
		for (uint32_t i = 0; i < light.count; i++) {
			const uint32_t map = light.first + i;
			const uint32_t profile = light_index == 4 ? 5 : (map % 5);
			check_maps.push_back(map);
			uint32_t *entries = cpu_table.ptr() + uint64_t(map) * ENTRIES_PER_MAP;
			for (uint32_t mip = 0; mip < MIP_COUNT; mip++) {
				const uint32_t count = (PAGES_PER_SIDE >> mip) * (PAGES_PER_SIDE >> mip);
				for (uint32_t e = 0; e < count; e++) {
					if (rng.randf() >= profiles[profile][mip]) {
						continue;
					}
					uint32_t flags = ENTRY_ALLOCATED;
					for (uint32_t extra : extra_flags) {
						if (rng.randf() < 0.25f) {
							flags |= extra;
						}
					}
					entries[mip_entry_offset(mip) + e] = entry_pack(rng.rand(uint32_t(pool_grid.x)), rng.rand(uint32_t(pool_grid.y)), flags);
					resident_entries++;
				}
			}
		}
		rd->buffer_update(table_buffer, uint32_t(table_bytes(light.first)), uint32_t(table_bytes(light.count)), cpu_table.ptr() + uint64_t(light.first) * ENTRIES_PER_MAP);
	}
	result["check_maps"] = int64_t(check_maps.size());
	result["check_resident_entries"] = int64_t(resident_entries);

	// 3. Clipmap scrolls on the directional levels, on the GPU and in the CPU model, then the whole
	// check range read back. Origins include negative ones and moves of a whole level.
	static const int32_t scroll_cases[8][4] = {
		{ 0, 0, 1, 0 },
		{ -37, 5, -36, 3 },
		{ 10, 10, 10, 10 },
		{ -1, -1, -65, -1 },
		{ 100, -100, 131, -71 },
		{ -3, -5, -4, -6 },
		{ 5, 7, 70, 7 },
		{ -200, 300, -201, 333 },
	};
	uint32_t scroll_marked = 0;
	uint32_t scroll_passes_run = 0;
	for (uint32_t level = 0; level < check_lights[0].count; level++) {
		const int32_t *c = scroll_cases[level % 8];
		const ClipmapScroll scroll = clipmap_scroll(Vector2i(c[0], c[1]), Vector2i(c[2], c[3]));
		if (!scroll.any) {
			continue;
		}
		const uint32_t map = check_lights[0].first + level;
		_table_scroll(map, scroll);
		scroll_passes_run++;
		scroll_marked += clipmap_apply_scroll(cpu_table.ptr() + uint64_t(map) * ENTRIES_PER_MAP, scroll);
	}
	uint32_t scroll_checked = 0;
	uint32_t scroll_mismatches = 0;
	for (const CheckLight &light : check_lights) {
		const Vector<uint8_t> bytes = rd->buffer_get_data(table_buffer, uint32_t(table_bytes(light.first)), uint32_t(table_bytes(light.count)));
		ERR_CONTINUE(uint64_t(bytes.size()) != table_bytes(light.count));
		const uint32_t *gpu = reinterpret_cast<const uint32_t *>(bytes.ptr());
		const uint32_t *cpu = cpu_table.ptr() + uint64_t(light.first) * ENTRIES_PER_MAP;
		for (uint32_t i = 0; i < light.count * ENTRIES_PER_MAP; i++) {
			scroll_checked++;
			scroll_mismatches += gpu[i] != cpu[i] ? 1 : 0;
		}
	}
	result["scroll_passes"] = int64_t(scroll_passes_run);
	result["scroll_marked_entries"] = int64_t(scroll_marked);
	result["scroll_checked_entries"] = int64_t(scroll_checked);
	result["scroll_mismatched_entries"] = int64_t(scroll_mismatches);

	// 4. Lookups: the GPU runs the lookups on the exact query bits; the CPU models run on the same.
	// Three classes, each with its own counters:
	// - lookup_*: p_query_count vsm_lookup() queries on the check maps (the issue's 2^16);
	// - clipmap_*: as many vsm_lookup_clipmap() queries on the directional levels, with origins that
	//   are odd, negative, 63 modulo 64 or aligned, and texels near the window's far edge and outside;
	// - map_guard_*: vsm_lookup() on VSM_MAP_NONE, on maps at or past the table's map count (that hold
	//   entries, so a missing guard shows), and through vsm_light_map() with a VSM_MAP_NONE base or an
	//   index at or past the light's map count.
	const uint32_t query_count = MAX(p_query_count, 1u);
	const uint32_t clipmap_count = query_count;
	const uint32_t guard_count = MAX(query_count / 16, 64u);
	const uint32_t total_count = query_count + clipmap_count + guard_count;
	const uint32_t query_words = 12;
	LocalVector<uint32_t> queries;
	queries.resize(total_count * query_words);
	memset(queries.ptr(), 0, queries.size() * sizeof(uint32_t));
	enum QueryKind : uint32_t {
		QUERY_MAP,
		QUERY_LIGHT_MAP,
		QUERY_CLIPMAP,
	};
	auto set_query = [&](uint32_t p_q, QueryKind p_kind, uint32_t p_map, uint32_t p_a, uint32_t p_b, uint32_t p_mip, uint32_t p_flags, uint32_t p_index, const Vector2i &p_origin, uint32_t p_table_maps) {
		uint32_t *w = queries.ptr() + uint64_t(p_q) * query_words;
		w[0] = p_map;
		w[1] = p_a;
		w[2] = p_b;
		w[3] = (p_mip & 0xFFu) | (uint32_t(p_kind) << 8);
		w[4] = p_flags;
		w[5] = p_index;
		w[6] = uint32_t(p_origin.x);
		w[7] = uint32_t(p_origin.y);
		w[8] = p_table_maps;
	};
	auto random_uv = [&]() {
		float value = 0.0f;
		if (rng.randf() < 0.85f) {
			value = -0.05f + 1.1f * rng.randf();
		} else {
			const float edge = float(1 + rng.rand(PAGES_PER_SIDE - 1)) / float(PAGES_PER_SIDE);
			switch (rng.rand(9)) {
				case 0:
					value = 0.0f;
					break;
				case 1:
					value = -0.0f;
					break;
				case 2:
					value = 1.0f;
					break;
				case 3:
					value = std::nextafter(1.0f, 0.0f);
					break;
				case 4:
					value = edge;
					break;
				case 5:
					value = std::nextafter(edge, 0.0f);
					break;
				case 6:
					value = -1.0f;
					break;
				case 7:
					value = 1.5f;
					break;
				default:
					value = 1e-30f;
					break;
			}
		}
		return value;
	};
	uint32_t q = 0;
	for (uint32_t i = 0; i < query_count; i++, q++) {
		const uint32_t map = check_maps[rng.rand(check_maps.size())];
		const float u = random_uv();
		const float v = random_uv();
		set_query(q, QUERY_MAP, map, _float_bits(u), _float_bits(v), rng.rand(MIP_COUNT + 1), 0, 0, Vector2i(), capacity); // MIP_COUNT itself clamps to the last mip.
	}
	const CheckLight &sun = check_lights[0];
	const uint32_t sun_flags = light_flags(sun.kind, sun.count);
	for (uint32_t i = 0; i < clipmap_count; i++, q++) {
		Vector2i origin;
		for (int axis = 0; axis < 2; axis++) {
			const uint32_t pick = rng.rand(20);
			if (pick < 10) {
				origin[axis] = int32_t(rng.rand(601)) - 300;
			} else if (pick < 14) {
				origin[axis] = 64 * (int32_t(rng.rand(8)) - 4) + 63;
			} else if (pick < 17) {
				origin[axis] = 64 * (int32_t(rng.rand(8)) - 4);
			} else {
				origin[axis] = -31; // The review's case: one page right of the centered -32.
			}
		}
		Vector2i rel;
		for (int axis = 0; axis < 2; axis++) {
			const uint32_t pick = rng.rand(10);
			if (pick < 6) {
				rel[axis] = int32_t(rng.rand(VIRTUAL_SIZE));
			} else if (pick < 9) {
				rel[axis] = int32_t(VIRTUAL_SIZE - 1 - rng.rand(8 * PAGE_SIZE)); // The far edge's pages.
			} else {
				rel[axis] = rng.rand(2) ? int32_t(VIRTUAL_SIZE + rng.rand(200)) : -1 - int32_t(rng.rand(200));
			}
		}
		const Vector2i texel = origin * int32_t(PAGE_SIZE) + rel;
		const uint32_t mip = rng.rand(MIP_COUNT + 1);
		const uint32_t level = rng.rand(sun.count);
		set_query(q, QUERY_CLIPMAP, sun.first, uint32_t(texel.x), uint32_t(texel.y), mip, sun_flags, level, origin, capacity);
	}
	for (uint32_t i = 0; i < guard_count; i++, q++) {
		const uint32_t mip = rng.rand(MIP_COUNT + 1);
		const uint32_t u = _float_bits(random_uv());
		const uint32_t v = _float_bits(random_uv());
		const CheckLight &light = check_lights[rng.rand(5)];
		const uint32_t flags = light_flags(light.kind, light.count);
		switch (i % 4) {
			case 0: {
				const uint32_t pick = rng.rand(3);
				const uint32_t map = pick == 0 ? MAP_NONE : (pick == 1 ? MAP_NONE - 1 : capacity + rng.rand(1000));
				set_query(q, QUERY_MAP, map, u, v, mip, 0, 0, Vector2i(), capacity);
			} break;
			case 1: {
				// A map that holds entries, with the lookup told the table ends at or before it.
				const uint32_t map = check_maps[rng.rand(check_maps.size())];
				set_query(q, QUERY_MAP, map, u, v, mip, 0, 0, Vector2i(), rng.rand(map + 1));
			} break;
			case 2:
				set_query(q, QUERY_LIGHT_MAP, MAP_NONE, u, v, mip, flags, rng.rand(light.count), Vector2i(), capacity);
				break;
			default:
				set_query(q, QUERY_LIGHT_MAP, light.first, u, v, mip, flags, rng.rand(light.count + 3), Vector2i(), capacity);
				break;
		}
	}
	const uint32_t query_bytes = total_count * query_words * sizeof(uint32_t);
	const uint32_t result_bytes_expected = total_count * 4 * sizeof(uint32_t);
	const RID query_buffer = rd->storage_buffer_create(query_bytes, Span<uint8_t>(reinterpret_cast<const uint8_t *>(queries.ptr()), query_bytes));
	const RID result_buffer = rd->storage_buffer_create(result_bytes_expected);
	{
		CheckPushConstant push;
		push.count = total_count;
		const RID shader_rid = _shader(SHADER_MODE_LOOKUP_CHECK);
		RD::Uniform u_table(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, table_buffer);
		RD::Uniform u_queries(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, query_buffer);
		RD::Uniform u_results(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, result_buffer);
		const RID uniform_set = UniformSetCacheRD::get_singleton()->get_cache(shader_rid, 0, u_table, u_queries, u_results);
		RD::ComputeListID compute_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(compute_list, pipelines[SHADER_MODE_LOOKUP_CHECK]);
		rd->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
		rd->compute_list_set_push_constant(compute_list, &push, sizeof(CheckPushConstant));
		rd->compute_list_dispatch_threads(compute_list, total_count, 1, 1);
		rd->compute_list_end();
	}
	const Vector<uint8_t> result_bytes = rd->buffer_get_data(result_buffer);
	rd->free_rid(query_buffer);
	rd->free_rid(result_buffer);

	auto encode = [](const LookupResult &p_lookup) {
		return vformat("%d %d %x %x", p_lookup.texel_x, p_lookup.texel_y, p_lookup.mip | (p_lookup.valid ? 0x80000000u : 0u), p_lookup.entry);
	};
	auto same = [](const LookupResult &p_a, const LookupResult &p_b) {
		return p_a.valid == p_b.valid && p_a.mip == p_b.mip && p_a.entry == p_b.entry && p_a.texel_x == p_b.texel_x && p_a.texel_y == p_b.texel_y;
	};
	auto gpu_equals = [](const uint32_t *p_gpu, const LookupResult &p_lookup) {
		return p_gpu[0] == p_lookup.texel_x && p_gpu[1] == p_lookup.texel_y && p_gpu[2] == (p_lookup.mip | (p_lookup.valid ? 0x80000000u : 0u)) && p_gpu[3] == p_lookup.entry;
	};
	auto note_mismatch = [&](Dictionary &r_first, uint32_t p_q, const uint32_t *p_gpu, const LookupResult &p_cpu) {
		if (!r_first.is_empty()) {
			return;
		}
		const uint32_t *w = queries.ptr() + uint64_t(p_q) * query_words;
		r_first["query"] = int64_t(p_q);
		r_first["words"] = vformat("%x %x %x %x %x %x %x %x %x", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8]);
		r_first["gpu"] = vformat("%d %d %x %x", p_gpu[0], p_gpu[1], p_gpu[2], p_gpu[3]);
		r_first["cpu"] = encode(p_cpu);
	};

	uint32_t matches = 0;
	uint32_t mismatches = 0;
	uint32_t mutant_mismatches = 0;
	uint32_t at_requested_mip = 0;
	uint32_t misses = 0;
	uint32_t fallback_by[MIP_COUNT] = {};
	Dictionary first_mismatch;
	uint32_t clipmap_matches = 0;
	uint32_t clipmap_valid = 0;
	uint32_t clipmap_fallbacks = 0;
	uint32_t clipmap_outside = 0;
	uint32_t clipmap_window_stops = 0;
	uint32_t clipmap_window_mutant_mismatches = 0;
	Dictionary clipmap_first_mismatch;
	uint32_t guard_matches = 0;
	uint32_t guard_rejected = 0;
	uint32_t guard_valid = 0;
	uint32_t guard_mutant_compared = 0;
	uint32_t guard_mutant_mismatches = 0;
	Dictionary guard_first_mismatch;
	const bool have_results = uint32_t(result_bytes.size()) == result_bytes_expected;
	if (have_results) {
		const uint32_t *gpu = reinterpret_cast<const uint32_t *>(result_bytes.ptr());
		for (uint32_t i = 0; i < total_count; i++) {
			const uint32_t *w = queries.ptr() + uint64_t(i) * query_words;
			const uint32_t *g = gpu + uint64_t(i) * 4;
			const uint32_t mip = w[3] & 0xFFu;
			const QueryKind kind = QueryKind((w[3] >> 8) & 0xFFu);
			const uint32_t table_maps = w[8];
			const Vector2 uv(_bits_float(w[1]), _bits_float(w[2]));
			if (i < query_count) {
				const uint32_t map = w[0];
				const LookupResult cpu = lookup(cpu_table.ptr(), table_maps, map, uv, mip);
				// The CPU model without fallback must disagree with the GPU on exactly the fallback queries.
				const LookupResult mutant = lookup(cpu_table.ptr(), table_maps, map, uv, mip, false);
				if (gpu_equals(g, cpu)) {
					matches++;
				} else {
					note_mismatch(first_mismatch, i, g, cpu);
					mismatches++;
				}
				mutant_mismatches += gpu_equals(g, mutant) ? 0 : 1;
				if (!cpu.valid) {
					misses++;
				} else {
					const uint32_t requested = MIN(mip, MIP_COUNT - 1);
					if (cpu.mip == requested) {
						at_requested_mip++;
					} else {
						fallback_by[cpu.mip - requested]++;
					}
				}
			} else if (i < query_count + clipmap_count) {
				const uint32_t map = light_map(w[0], w[4], w[5]);
				const Vector2i origin = Vector2i(int32_t(w[6]), int32_t(w[7]));
				const Vector2i texel = Vector2i(int32_t(w[1]), int32_t(w[2]));
				const LookupResult cpu = lookup_clipmap(cpu_table.ptr(), table_maps, map, origin, texel, mip);
				// The lookup this replaces: vsm_lookup() on the wrapped texel, with no window check.
				const Vector2 wrapped_uv((float(texel.x & int32_t(VIRTUAL_SIZE - 1)) + 0.5f) / float(VIRTUAL_SIZE), (float(texel.y & int32_t(VIRTUAL_SIZE - 1)) + 0.5f) / float(VIRTUAL_SIZE));
				const LookupResult wrapped = lookup(cpu_table.ptr(), table_maps, map, wrapped_uv, mip);
				if (gpu_equals(g, cpu)) {
					clipmap_matches++;
				} else {
					note_mismatch(clipmap_first_mismatch, i, g, cpu);
				}
				const Vector2i rel = texel - origin * int32_t(PAGE_SIZE);
				clipmap_outside += (rel.x < 0 || rel.y < 0 || rel.x >= int32_t(VIRTUAL_SIZE) || rel.y >= int32_t(VIRTUAL_SIZE)) ? 1 : 0;
				clipmap_valid += cpu.valid ? 1 : 0;
				clipmap_fallbacks += (cpu.valid && cpu.mip > MIN(mip, MIP_COUNT - 1)) ? 1 : 0;
				clipmap_window_stops += same(cpu, wrapped) ? 0 : 1;
				clipmap_window_mutant_mismatches += gpu_equals(g, wrapped) ? 0 : 1;
			} else {
				const uint32_t map = kind == QUERY_LIGHT_MAP ? light_map(w[0], w[4], w[5]) : w[0];
				const LookupResult cpu = lookup(cpu_table.ptr(), table_maps, map, uv, mip);
				if (gpu_equals(g, cpu)) {
					guard_matches++;
				} else {
					note_mismatch(guard_first_mismatch, i, g, cpu);
				}
				guard_rejected += map >= table_maps ? 1 : 0;
				guard_valid += cpu.valid ? 1 : 0;
				// Without the guards: the index added to the base as is, and the whole table readable.
				const uint32_t unguarded = kind == QUERY_LIGHT_MAP ? w[0] + w[5] : w[0];
				if (map >= table_maps && unguarded < capacity) {
					guard_mutant_compared++;
					guard_mutant_mismatches += gpu_equals(g, lookup(cpu_table.ptr(), capacity, unguarded, uv, mip)) ? 0 : 1;
				}
			}
		}
	}
	uint32_t fallbacks = 0;
	Array fallback_histogram;
	for (uint32_t d = 1; d < MIP_COUNT; d++) {
		fallback_histogram.push_back(int64_t(fallback_by[d]));
		fallbacks += fallback_by[d];
	}
	result["lookup_queries"] = int64_t(query_count);
	result["lookup_matches"] = int64_t(matches);
	result["lookup_mismatches"] = int64_t(have_results ? mismatches : query_count);
	result["lookup_at_requested_mip"] = int64_t(at_requested_mip);
	result["lookup_fallbacks"] = int64_t(fallbacks);
	result["lookup_fallback_by_mips"] = fallback_histogram; // [1 mip coarser, 2, ..., 6]
	result["lookup_misses"] = int64_t(misses);
	result["lookup_mutant_mismatches"] = int64_t(mutant_mismatches);
	if (!first_mismatch.is_empty()) {
		result["lookup_first_mismatch"] = first_mismatch;
	}
	result["clipmap_queries"] = int64_t(clipmap_count);
	result["clipmap_matches"] = int64_t(clipmap_matches);
	result["clipmap_valid"] = int64_t(clipmap_valid);
	result["clipmap_fallbacks"] = int64_t(clipmap_fallbacks);
	result["clipmap_outside"] = int64_t(clipmap_outside);
	// Queries where the window check changes the answer of the wrapped lookup: the GPU must differ
	// from that lookup on exactly these.
	result["clipmap_window_stops"] = int64_t(clipmap_window_stops);
	result["clipmap_window_mutant_mismatches"] = int64_t(clipmap_window_mutant_mismatches);
	if (!clipmap_first_mismatch.is_empty()) {
		result["clipmap_first_mismatch"] = clipmap_first_mismatch;
	}
	result["map_guard_queries"] = int64_t(guard_count);
	result["map_guard_matches"] = int64_t(guard_matches);
	result["map_guard_rejected"] = int64_t(guard_rejected);
	result["map_guard_valid"] = int64_t(guard_valid);
	// Rejected queries whose unguarded map lies in the table: the GPU must miss where the unguarded
	// lookup finds entries.
	result["map_guard_mutant_compared"] = int64_t(guard_mutant_compared);
	result["map_guard_mutant_mismatches"] = int64_t(guard_mutant_mismatches);
	if (!guard_first_mismatch.is_empty()) {
		result["map_guard_first_mismatch"] = guard_first_mismatch;
	}

	for (const CheckLight &light : check_lights) {
		map_free(light.rid);
	}

	// 5. Allocate and free cycles of random kinds, each writing a sentinel entry into one of its maps,
	// with the pool recreated at every quality in between.
	const uint32_t cycles = p_cycles;
	uint32_t cycle_allocations = 0;
	uint32_t cycle_frees = 0;
	uint32_t cycle_failures = 0;
	uint32_t cycle_maps = 0;
	uint32_t pool_switches = 0;
	const uint32_t pool_period = MAX(cycles / POOL_QUALITY_MAX, 1u);
	for (uint32_t cycle = 0; cycle < cycles; cycle++) {
		if (cycle % pool_period == 0) {
			pool_create(pool_quality_pages(PoolQuality((cycle / pool_period) % POOL_QUALITY_MAX)));
			pool_switches++;
		}
		if (live.size() < 48 && (live.is_empty() || rng.randf() < 0.5f)) {
			const MapKind kind = MapKind(rng.rand(MAP_KIND_MAX));
			const uint32_t levels = 1 + rng.rand(CLIPMAP_MAX_LEVELS);
			const RID rid = _check_rid(next_rid++);
			const uint32_t first = map_allocate(rid, kind, levels);
			if (first == MAP_NONE) {
				cycle_failures++;
				continue;
			}
			const uint32_t count = map_kind_map_count(kind, levels);
			const uint32_t map = first + rng.rand(count);
			const uint32_t sentinel = entry_pack(rng.rand(PAGES_PER_SIDE), rng.rand(PAGES_PER_SIDE), ENTRY_ALLOCATED | ENTRY_DIRTY_STATIC);
			rd->buffer_update(table_buffer, uint32_t(table_bytes(map)) + rng.rand(ENTRIES_PER_MAP) * ENTRY_BYTES, ENTRY_BYTES, &sentinel);
			live.push_back(rid);
			cycle_allocations++;
			cycle_maps += count;
		} else {
			const uint32_t index = rng.rand(live.size());
			map_free(live[index]);
			live.remove_at_unordered(index);
			cycle_frees++;
		}
	}
	cycle_frees += live.size();
	restore();

	// 6. Every map no light owns must be all zero: the check's and the cycles' maps were freed, and
	// so were the maps of any stale light they evicted. Real lights' maps are not read: their
	// entries belong to the page allocation pass (#219).
	uint32_t nonzero_entries = 0;
	uint32_t free_maps_checked = 0;
	{
		const Vector<uint8_t> bytes = rd->buffer_get_data(table_buffer);
		const uint32_t *entries = reinterpret_cast<const uint32_t *>(bytes.ptr());
		const uint64_t entry_count = uint64_t(bytes.size()) / sizeof(uint32_t);
		for (uint32_t r = 0; r < get_free_range_count(); r++) {
			uint32_t first = 0;
			uint32_t count = 0;
			get_free_range(r, first, count);
			const uint64_t end = MIN(uint64_t(first + count) * ENTRIES_PER_MAP, entry_count);
			for (uint64_t i = uint64_t(first) * ENTRIES_PER_MAP; i < end; i++) {
				nonzero_entries += entries[i] != 0 ? 1 : 0;
			}
			free_maps_checked += count;
		}
		result["table_read_bytes"] = int64_t(bytes.size());
	}
	const uint32_t capacity_after = get_table_capacity();
	const int64_t evicted_lights_check = int64_t(get_evicted_light_count() - evicted_lights_before);
	const int64_t evicted_maps_check = int64_t(get_evicted_map_count() - evicted_maps_before);
	result["cycles"] = int64_t(cycles);
	result["cycle_allocations"] = int64_t(cycle_allocations);
	result["cycle_frees"] = int64_t(cycle_frees);
	result["cycle_failures"] = int64_t(cycle_failures);
	result["cycle_maps"] = int64_t(cycle_maps);
	result["pool_switches"] = int64_t(pool_switches);
	result["table_free_maps_checked"] = int64_t(free_maps_checked);
	result["table_nonzero_entries"] = int64_t(nonzero_entries);
	result["evicted_lights"] = evicted_lights_check;
	result["evicted_maps"] = evicted_maps_check;
	result["leaked_maps"] = int64_t(get_allocated_map_count()) - (int64_t(allocated_before) - evicted_maps_check);
	result["leaked_lights"] = int64_t(get_light_count()) - (int64_t(lights_before) - evicted_lights_check);
	result["free_maps"] = int64_t(get_free_map_count());
	result["free_maps_expected"] = int64_t(capacity_after) - (int64_t(allocated_before) - evicted_maps_check);
	result["page_table_maps_after"] = int64_t(capacity_after);
	result["page_table_bytes"] = int64_t(table_buffer_bytes);
	result["pool_pages_after"] = int64_t(get_pool_page_count());
	result["pool_pages_before"] = int64_t(pool_pages_before);
	// What RenderingDevice's counters must show once the freed resources are gone: the table's
	// growth, and no texture change (the pool is back at its size).
	// GodotNext: vsm_page_marking (#219). The per-map buffers grow with the table.
	result["expected_buffer_delta"] = int64_t(table_bytes(capacity_after)) - int64_t(table_bytes(capacity_before)) + (int64_t(capacity_after) - int64_t(capacity_before)) * int64_t(MAP_BUFFER_BYTES_PER_MAP);
	result["expected_texture_delta"] = int64_t(0);
	result["expected_buffer_count_delta"] = int64_t(capacity_before == 0 && capacity_after > 0 ? 3 : 0);
	return result;
}

/* PAGE MARKING AND ALLOCATION (#219) */

// GodotNext: vsm_page_marking (#219, plan §21.3 contract S3).

bool VirtualShadowMapStorage::_ensure_marking_shaders() {
	if (marking_ready) {
		return true;
	}
	if (marking_failed) {
		return false;
	}
	const String defines = get_glsl_defines();
	Vector<String> mark_modes;
	mark_modes.push_back("\n#define MODE_PIXEL\n");
	mark_modes.push_back("\n#define MODE_FROXEL\n");
	mark_modes.push_back("\n#define MODE_BOX\n");
	mark_shader.initialize(mark_modes, defines);
	mark_version = mark_shader.version_create();
	Vector<String> allocate_modes;
	allocate_modes.push_back("\n#define MODE_UPDATE\n");
	allocate_modes.push_back("\n#define MODE_SCAN\n");
	allocate_modes.push_back("\n#define MODE_SCATTER\n");
	allocate_modes.push_back("\n#define MODE_ASSIGN\n");
	allocate_modes.push_back("\n#define MODE_MAPS\n");
	allocate_modes.push_back("\n#define MODE_EMIT\n");
	allocate_modes.push_back("\n#define MODE_CLEAR\n");
	allocate_shader.initialize(allocate_modes, defines);
	allocate_version = allocate_shader.version_create();
	bool ok = true;
	for (int mode = 0; mode < MARK_MODE_MAX && ok; mode++) {
		const RID shader_rid = mark_shader.version_get_shader(mark_version, mode);
		ok = shader_rid.is_valid();
		if (ok) {
			mark_pipelines[mode] = RD::get_singleton()->compute_pipeline_create(shader_rid);
		}
	}
	for (int mode = 0; mode < ALLOCATE_MODE_MAX && ok; mode++) {
		const RID shader_rid = allocate_shader.version_get_shader(allocate_version, mode);
		ok = shader_rid.is_valid();
		if (ok) {
			allocate_pipelines[mode] = RD::get_singleton()->compute_pipeline_create(shader_rid);
		}
	}
	if (!ok) {
		// Freeing the versions frees the pipelines made from them.
		mark_shader.version_free(mark_version);
		allocate_shader.version_free(allocate_version);
		mark_version = RID();
		allocate_version = RID();
		for (RID &pipeline : mark_pipelines) {
			pipeline = RID();
		}
		for (RID &pipeline : allocate_pipelines) {
			pipeline = RID();
		}
		marking_failed = true;
		ERR_FAIL_V_MSG(false, "The virtual shadow map marking and allocation shaders did not compile; pages are not marked.");
	}
	marking_ready = true;
	return true;
}

void VirtualShadowMapStorage::_resize_map_buffers(uint32_t p_maps) {
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL(rd);
	if (p_maps == map_buffers_maps) {
		return;
	}
	const uint32_t old_maps = map_buffers_maps;
	map_data.resize(p_maps);
	for (uint32_t map = old_maps; map < p_maps; map++) {
		map_data[map] = MapData();
	}
	const RID new_data = rd->storage_buffer_create(p_maps * uint32_t(sizeof(MapData)));
	const RID new_state = rd->storage_buffer_create(p_maps * MAP_STATE_REGIONS * uint32_t(sizeof(uint32_t)));
	ERR_FAIL_COND(new_data.is_null() || new_state.is_null());
	rd->set_resource_name(new_data, "VSM Map Data");
	rd->set_resource_name(new_state, "VSM Map State");
	// The regions' offsets follow the map count, so the state (requests, dirty places and the overflow
	// bias) starts over; the data is uploaded again whole.
	rd->buffer_clear(new_state, 0, p_maps * MAP_STATE_REGIONS * uint32_t(sizeof(uint32_t)));
	if (map_data_buffer.is_valid()) {
		rd->free_rid(map_data_buffer);
	}
	if (map_state_buffer.is_valid()) {
		rd->free_rid(map_state_buffer);
	}
	map_data_buffer = new_data;
	map_state_buffer = new_state;
	map_buffers_maps = p_maps;
	map_data_dirty_begin = 0;
	map_data_dirty_end = p_maps;
}

void VirtualShadowMapStorage::_set_map_data(uint32_t p_map, const MapData &p_data) {
	ERR_FAIL_UNSIGNED_INDEX(p_map, map_data.size());
	if (map_data[p_map] == p_data) {
		return;
	}
	map_data[p_map] = p_data;
	if (map_data_dirty_begin >= map_data_dirty_end) {
		map_data_dirty_begin = p_map;
		map_data_dirty_end = p_map + 1;
	} else {
		map_data_dirty_begin = MIN(map_data_dirty_begin, p_map);
		map_data_dirty_end = MAX(map_data_dirty_end, p_map + 1);
	}
}

void VirtualShadowMapStorage::_upload_map_data() {
	if (map_data_dirty_begin >= map_data_dirty_end || map_data_buffer.is_null()) {
		return;
	}
	const uint32_t end = MIN(map_data_dirty_end, map_data.size());
	if (map_data_dirty_begin < end) {
		RD::get_singleton()->buffer_update(map_data_buffer, map_data_dirty_begin * uint32_t(sizeof(MapData)), (end - map_data_dirty_begin) * uint32_t(sizeof(MapData)), map_data.ptr() + map_data_dirty_begin);
	}
	map_data_dirty_begin = 0;
	map_data_dirty_end = 0;
}

void VirtualShadowMapStorage::_ensure_page_buffers() {
	RenderingDevice *rd = RD::get_singleton();
	const uint32_t page_count = get_pool_page_count();
	if (state_buffer.is_null()) {
		state_buffer = rd->storage_buffer_create(STATE_SIZE * uint32_t(sizeof(uint32_t)));
		rd->set_resource_name(state_buffer, "VSM Allocation State");
		rd->buffer_clear(state_buffer, 0, STATE_SIZE * uint32_t(sizeof(uint32_t)));
		view_buffer = rd->storage_buffer_create(sizeof(ViewParams));
		rd->set_resource_name(view_buffer, "VSM View Params");
		indirect_buffer = rd->storage_buffer_create(INDIRECT_WORDS * uint32_t(sizeof(uint32_t)), Span<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
		rd->set_resource_name(indirect_buffer, "VSM Dirty Page Indirect Args");
		rd->buffer_clear(indirect_buffer, 0, INDIRECT_WORDS * uint32_t(sizeof(uint32_t)));
		box_buffer = rd->storage_buffer_create(MAX_TRANSPARENT_BOXES * 2 * 4 * uint32_t(sizeof(float)));
		rd->set_resource_name(box_buffer, "VSM Transparent Boxes");
		capture_depth_words = 4;
		capture_depth_buffer = rd->storage_buffer_create(capture_depth_words * uint32_t(sizeof(uint32_t)));
		rd->set_resource_name(capture_depth_buffer, "VSM Check Depth");
	}
	if (page_buffers_pages != page_count) {
		for (RID *buffer : { &pages_buffer, &free_list_buffer, &dirty_buffer, &request_buffer }) {
			if (buffer->is_valid()) {
				rd->free_rid(*buffer);
			}
			*buffer = RID();
		}
		page_buffers_pages = page_count;
		request_buffer_capacity = request_capacity(page_count);
		pages_buffer = rd->storage_buffer_create(MAX(page_count, 1u) * 4 * uint32_t(sizeof(uint32_t)));
		rd->set_resource_name(pages_buffer, "VSM Pool Header");
		rd->buffer_clear(pages_buffer, 0, MAX(page_count, 1u) * 4 * uint32_t(sizeof(uint32_t)));
		free_list_buffer = rd->storage_buffer_create(MAX(page_count, 1u) * uint32_t(sizeof(uint32_t)));
		rd->set_resource_name(free_list_buffer, "VSM Free List");
		dirty_buffer = rd->storage_buffer_create(MAX(page_count, 1u) * 4 * uint32_t(sizeof(uint32_t)));
		rd->set_resource_name(dirty_buffer, "VSM Dirty Pages");
		request_buffer = rd->storage_buffer_create(request_buffer_capacity * uint32_t(sizeof(uint32_t)));
		rd->set_resource_name(request_buffer, "VSM Requests");
		// The pool changed size: no page of the old one is valid (mark_and_allocate() clears the table).
	}
}

void VirtualShadowMapStorage::_free_marking_resources() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return;
	}
	for (RID *buffer : { &pages_buffer, &free_list_buffer, &dirty_buffer, &request_buffer, &state_buffer, &view_buffer, &indirect_buffer, &box_buffer, &capture_depth_buffer }) {
		if (buffer->is_valid()) {
			rd->free_rid(*buffer);
		}
		*buffer = RID();
	}
	page_buffers_pages = 0;
	request_buffer_capacity = 0;
	capture_depth_words = 0;
}

void VirtualShadowMapStorage::_free_check_buffers() {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr) {
		return;
	}
	for (RID *buffer : { &check.cluster, &check.lights[0], &check.lights[1], &check.lights[2], &check.bias_before, &check.table_marked, &check.pages_before, &check.state_marked, &check.requests, &check.map_state_marked, &check.table_after, &check.pages_after, &check.state_after, &check.map_state_after, &check.dirty, &check.indirect, &check.pool_saved, &check.pool_cleared }) {
		if (buffer->is_valid()) {
			rd->free_rid(*buffer);
		}
		*buffer = RID();
	}
	check.captured = false;
}

RID VirtualShadowMapStorage::_snapshot(RID p_buffer, uint32_t p_offset, uint32_t p_bytes, const char *p_name) {
	RenderingDevice *rd = RD::get_singleton();
	if (p_buffer.is_null() || p_bytes == 0) {
		return RID();
	}
	const RID copy = rd->storage_buffer_create(p_bytes);
	ERR_FAIL_COND_V(copy.is_null(), RID());
	rd->set_resource_name(copy, String("VSM Check ") + p_name);
	rd->buffer_copy(p_buffer, copy, p_offset, 0, p_bytes);
	return copy;
}

RID VirtualShadowMapStorage::_pool_snapshot(const char *p_name) {
	RenderingDevice *rd = RD::get_singleton();
	if (pool_texture.is_null()) {
		return RID();
	}
	RD::TextureFormat format;
	format.format = RD::DATA_FORMAT_R32_UINT;
	format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	format.width = uint32_t(pool_texture_size.x);
	format.height = uint32_t(pool_texture_size.y);
	format.array_layers = POOL_LAYERS;
	// Sampling only so that the texture's view is valid (Vulkan needs a view usage); it is only copied.
	format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	const RID copy = rd->texture_create(format, RD::TextureView());
	ERR_FAIL_COND_V(copy.is_null(), RID());
	rd->set_resource_name(copy, String("VSM Check ") + p_name);
	_pool_copy(pool_texture, copy);
	return copy;
}

void VirtualShadowMapStorage::_pool_copy(RID p_from, RID p_to) {
	RenderingDevice *rd = RD::get_singleton();
	const Vector3 size(real_t(pool_texture_size.x), real_t(pool_texture_size.y), 1);
	for (uint32_t layer = 0; layer < POOL_LAYERS; layer++) {
		rd->texture_copy(p_from, p_to, Vector3(), Vector3(), size, 0, 0, layer, layer);
	}
}

bool VirtualShadowMapStorage::is_marking_enabled() {
	return active && get_pool_page_count() > 0 && pool_texture.is_valid() && _ensure_marking_shaders();
}

void VirtualShadowMapStorage::set_lod_bias(int32_t p_directional, int32_t p_local) {
	lod_bias_directional = CLAMP(p_directional, -int32_t(MIP_COUNT), int32_t(MIP_COUNT));
	lod_bias_local = CLAMP(p_local, -int32_t(MIP_COUNT), int32_t(MIP_COUNT));
}

void VirtualShadowMapStorage::request_check() {
	_free_check_buffers();
	check.armed = true;
}

void VirtualShadowMapStorage::mark_and_allocate(const MarkInput &p_input) {
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL(rd);
	if (!is_marking_enabled() || p_input.depth_texture.is_null() || p_input.cluster_buffer.is_null() || p_input.size.x <= 0 || p_input.size.y <= 0) {
		return;
	}
	const uint32_t table_maps = get_table_capacity();
	if (table_maps == 0 || table_buffer.is_null() || map_buffers_maps != table_maps) {
		return; // No light has maps yet.
	}
	_ensure_page_buffers();
	const uint32_t page_count = get_pool_page_count();
	const Vector2i grid = pool_page_grid(page_count);
	if (processed_pool_reallocations != get_pool_reallocation_count()) {
		// A new pool holds none of the old pages: every entry and every page record starts over.
		rd->buffer_clear(table_buffer, 0, uint32_t(table_buffer_bytes));
		rd->buffer_clear(pages_buffer, 0, page_count * 4 * uint32_t(sizeof(uint32_t)));
		processed_pool_reallocations = get_pool_reallocation_count();
	}
	_upload_map_data();
	const bool capture = check.armed;
	const uint32_t view_frame = RSG::rasterizer != nullptr ? uint32_t(RSG::rasterizer->get_frame_number()) : 0u;

	// The view's parameters.
	ViewParams view;
	for (int row = 0; row < 4; row++) {
		for (int column = 0; column < 4; column++) {
			view.inv_projection[row][column] = float(p_input.inv_projection.columns[column][row]);
		}
	}
	for (int row = 0; row < 3; row++) {
		view.view_to_world[row][0] = float(p_input.cam_transform.basis.rows[row].x);
		view.view_to_world[row][1] = float(p_input.cam_transform.basis.rows[row].y);
		view.view_to_world[row][2] = float(p_input.cam_transform.basis.rows[row].z);
		view.view_to_world[row][3] = float(p_input.cam_transform.origin[row]);
	}
	const uint32_t cluster_size = MAX(p_input.cluster_size, 1u);
	const uint32_t cluster_width = (uint32_t(p_input.size.x) + cluster_size - 1) / cluster_size;
	const uint32_t cluster_height = (uint32_t(p_input.size.y) + cluster_size - 1) / cluster_size;
	const uint32_t max_div_32 = p_input.cluster_max_elements / 32;
	view.screen[0] = uint32_t(p_input.size.x);
	view.screen[1] = uint32_t(p_input.size.y);
	view.screen[2] = capture ? 1u : 0u;
	view.screen[3] = request_buffer_capacity;
	view.footprint[0] = p_input.pixel_scale;
	view.footprint[1] = p_input.pixel_bias;
	view.footprint[2] = p_input.z_far;
	view.cluster[0] = uint32_t(Math::get_shift_from_power_of_2(cluster_size));
	view.cluster[1] = cluster_width;
	view.cluster[2] = cluster_width * cluster_height * (max_div_32 + 32);
	view.cluster[3] = max_div_32;
	view.counts[0] = view_directional.size();
	view.counts[1] = p_input.omni_count;
	view.counts[2] = p_input.spot_count;
	view.counts[3] = p_input.area_count;
	const uint32_t box_count = MIN(p_input.boxes.size(), MAX_TRANSPARENT_BOXES);
	view.misc[0] = table_maps;
	view.misc[1] = box_count;
	view.misc[2] = view_frame;
	view.bias[0] = lod_bias_directional;
	view.bias[1] = lod_bias_local;
	if (p_input.fog) {
		view.fog_size[0] = uint32_t(MAX(p_input.fog_size.x, 1));
		view.fog_size[1] = uint32_t(MAX(p_input.fog_size.y, 1));
		view.fog_size[2] = uint32_t(MAX(p_input.fog_size.z, 1));
	}
	view.fog_a[0] = p_input.fog_length;
	view.fog_a[1] = p_input.fog_spread;
	view.fog_a[2] = float(p_input.fog_near_size.x);
	view.fog_a[3] = float(p_input.fog_near_size.y);
	view.fog_b[0] = float(p_input.fog_far_size.x);
	view.fog_b[1] = float(p_input.fog_far_size.y);
	for (uint32_t i = 0; i < view_directional.size(); i++) {
		const DirectionalEntry &entry = view_directional[i];
		view.directional[i][0] = entry.map_base;
		view.directional[i][1] = entry.flags;
		memcpy(&view.directional[i][2], &entry.max_distance, sizeof(float));
		memcpy(&view.directional[i][3], &entry.fog_energy, sizeof(float));
	}
	rd->buffer_update(view_buffer, 0, sizeof(ViewParams), &view);

	// Transparent boxes; the ones past the limit merge into the last.
	LocalVector<AABB> boxes;
	if (box_count > 0) {
		boxes.resize(box_count);
		for (uint32_t i = 0; i < box_count; i++) {
			boxes[i] = p_input.boxes[i];
		}
		for (uint32_t i = box_count; i < p_input.boxes.size(); i++) {
			boxes[box_count - 1].merge_with(p_input.boxes[i]);
		}
		LocalVector<float> box_floats;
		box_floats.resize(box_count * 8);
		for (uint32_t i = 0; i < box_count; i++) {
			const Vector3 lo = boxes[i].position;
			const Vector3 hi = boxes[i].position + boxes[i].size;
			const float values[8] = { float(lo.x), float(lo.y), float(lo.z), 0.0f, float(hi.x), float(hi.y), float(hi.z), 0.0f };
			memcpy(box_floats.ptr() + i * 8, values, sizeof(values));
		}
		rd->buffer_update(box_buffer, 0, box_count * 8 * uint32_t(sizeof(float)), box_floats.ptr());
	}

	if (capture) {
		const uint32_t words = uint32_t(p_input.size.x) * uint32_t(p_input.size.y);
		if (capture_depth_words < words) {
			rd->free_rid(capture_depth_buffer);
			capture_depth_words = words;
			capture_depth_buffer = rd->storage_buffer_create(words * uint32_t(sizeof(uint32_t)));
			rd->set_resource_name(capture_depth_buffer, "VSM Check Depth");
		}
		check.view = view;
		check.table_maps = table_maps;
		check.pool_pages = page_count;
		check.grid_width = uint32_t(grid.x);
		check.request_capacity = request_buffer_capacity;
		check.frame = view_frame;
		check.cache_frames = active_cache_frames;
		check.omni_count = p_input.omni_count;
		check.spot_count = p_input.spot_count;
		check.area_count = p_input.area_count;
		check.fog = p_input.fog;
		check.boxes = boxes;
		check.map_data = map_data;
		check.cluster = _snapshot(p_input.cluster_buffer, 0, view.cluster[2] * 3 * uint32_t(sizeof(uint32_t)), "Cluster");
		const RID light_buffers[3] = { p_input.omni_buffer, p_input.spot_buffer, p_input.area_buffer };
		const uint32_t light_counts[3] = { p_input.omni_count, p_input.spot_count, p_input.area_count };
		for (int type = 0; type < 3; type++) {
			check.lights[type] = _snapshot(light_buffers[type], 0, light_counts[type] * LIGHT_DATA_BYTES, "Lights");
		}
		check.bias_before = _snapshot(map_state_buffer, MAP_STATE_BIAS * table_maps * uint32_t(sizeof(uint32_t)), table_maps * uint32_t(sizeof(uint32_t)), "Bias Before");
	}

	rd->draw_command_begin_label("VSM Page Marking and Allocation");
	// Per-view counters.
	rd->buffer_clear(state_buffer, 0, STATE_VIEW_END * uint32_t(sizeof(uint32_t)));
	rd->buffer_clear(state_buffer, STATE_BUCKETS * uint32_t(sizeof(uint32_t)), (STATE_SIZE - STATE_BUCKETS) * uint32_t(sizeof(uint32_t)));
	rd->buffer_clear(map_state_buffer, 0, 2 * table_maps * uint32_t(sizeof(uint32_t)));
	rd->buffer_clear(map_state_buffer, MAP_STATE_DIRTY_CURSOR * table_maps * uint32_t(sizeof(uint32_t)), table_maps * uint32_t(sizeof(uint32_t)));

	// Marking.
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, table_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, map_data_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, map_state_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, state_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, request_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, view_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, p_input.cluster_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, p_input.omni_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, p_input.spot_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, p_input.area_buffer));
	{
		Vector<RID> depth_ids;
		depth_ids.push_back(RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED));
		depth_ids.push_back(p_input.depth_texture);
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 10, depth_ids));
	}
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, capture_depth_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, box_buffer));
	{
		RD::ComputeListID list = rd->compute_list_begin();
		const RID pixel_shader = mark_shader.version_get_shader(mark_version, MARK_MODE_PIXEL);
		rd->compute_list_bind_compute_pipeline(list, mark_pipelines[MARK_MODE_PIXEL]);
		rd->compute_list_bind_uniform_set(list, UniformSetCacheRD::get_singleton()->get_cache_vec(pixel_shader, 0, uniforms), 0);
		rd->compute_list_dispatch_threads(list, uint32_t(p_input.size.x), uint32_t(p_input.size.y), 1);
		if (p_input.fog) {
			const RID froxel_shader = mark_shader.version_get_shader(mark_version, MARK_MODE_FROXEL);
			rd->compute_list_bind_compute_pipeline(list, mark_pipelines[MARK_MODE_FROXEL]);
			rd->compute_list_bind_uniform_set(list, UniformSetCacheRD::get_singleton()->get_cache_vec(froxel_shader, 0, uniforms), 0);
			rd->compute_list_dispatch_threads(list, view.fog_size[0] + 1, view.fog_size[1] + 1, view.fog_size[2] + 1);
		}
		if (box_count > 0) {
			const RID box_shader = mark_shader.version_get_shader(mark_version, MARK_MODE_BOX);
			rd->compute_list_bind_compute_pipeline(list, mark_pipelines[MARK_MODE_BOX]);
			rd->compute_list_bind_uniform_set(list, UniformSetCacheRD::get_singleton()->get_cache_vec(box_shader, 0, uniforms), 0);
			rd->compute_list_dispatch(list, box_count, 1, 1);
		}
		rd->compute_list_end();
	}

	if (capture) {
		check.table_marked = _snapshot(table_buffer, 0, uint32_t(table_buffer_bytes), "Table Marked");
		check.pages_before = _snapshot(pages_buffer, 0, page_count * 4 * uint32_t(sizeof(uint32_t)), "Pages Before");
		check.state_marked = _snapshot(state_buffer, 0, STATE_SIZE * uint32_t(sizeof(uint32_t)), "State Marked");
		check.requests = _snapshot(request_buffer, 0, request_buffer_capacity * uint32_t(sizeof(uint32_t)), "Requests");
		check.map_state_marked = _snapshot(map_state_buffer, 0, table_maps * MAP_STATE_REGIONS * uint32_t(sizeof(uint32_t)), "Map State Marked");
	}

	if (capture) {
		// The clear pass's check: every texel it does not clear keeps this non-zero value. The pool's
		// content comes back afterwards.
		check.pool_saved = _pool_snapshot("Pool Saved");
		if (check.pool_saved.is_valid()) {
			rd->texture_clear(pool_texture, Color(1, 1, 1, 1), 0, 1, 0, POOL_LAYERS);
		}
	}

	// Allocation.
	LocalVector<RD::Uniform> allocate_uniforms;
	allocate_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, table_buffer));
	allocate_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, map_data_buffer));
	allocate_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, map_state_buffer));
	allocate_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, state_buffer));
	allocate_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, request_buffer));
	allocate_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, pages_buffer));
	allocate_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, free_list_buffer));
	allocate_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, dirty_buffer));
	allocate_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, indirect_buffer));
	allocate_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, pool_texture));
	AllocatePushConstant push;
	push.frame = view_frame;
	push.cache_frames = active_cache_frames;
	push.table_maps = table_maps;
	push.pool_pages = page_count;
	push.grid_width = uint32_t(grid.x);
	push.request_capacity = request_buffer_capacity;
	auto bind = [&](RD::ComputeListID p_list, AllocateMode p_mode) {
		const RID shader_rid = allocate_shader.version_get_shader(allocate_version, p_mode);
		rd->compute_list_bind_compute_pipeline(p_list, allocate_pipelines[p_mode]);
		rd->compute_list_bind_uniform_set(p_list, UniformSetCacheRD::get_singleton()->get_cache_vec(shader_rid, 0, allocate_uniforms), 0);
		rd->compute_list_set_push_constant(p_list, &push, sizeof(AllocatePushConstant));
	};
	{
		RD::ComputeListID list = rd->compute_list_begin();
		bind(list, ALLOCATE_MODE_UPDATE);
		rd->compute_list_dispatch_threads(list, page_count, 1, 1);
		rd->compute_list_add_barrier(list);
		bind(list, ALLOCATE_MODE_SCAN);
		rd->compute_list_dispatch(list, 1, 1, 1);
		rd->compute_list_add_barrier(list);
		bind(list, ALLOCATE_MODE_SCATTER);
		rd->compute_list_dispatch_threads(list, page_count, 1, 1);
		rd->compute_list_add_barrier(list);
		for (uint32_t phase = 0; phase < 2; phase++) {
			push.phase = phase;
			bind(list, ALLOCATE_MODE_ASSIGN);
			rd->compute_list_dispatch_threads(list, request_buffer_capacity, 1, 1);
			rd->compute_list_add_barrier(list);
		}
		push.phase = 0;
		bind(list, ALLOCATE_MODE_MAPS);
		rd->compute_list_dispatch(list, 1, 1, 1);
		rd->compute_list_add_barrier(list);
		bind(list, ALLOCATE_MODE_EMIT);
		rd->compute_list_dispatch_threads(list, request_buffer_capacity, 1, 1);
		rd->compute_list_end();
	}
	auto clear_pages = [&]() {
		// One workgroup per dirty page, from the arguments MODE_MAPS wrote.
		RD::ComputeListID list = rd->compute_list_begin();
		bind(list, ALLOCATE_MODE_CLEAR);
		rd->compute_list_dispatch_indirect(list, indirect_buffer, INDIRECT_ALL * uint32_t(sizeof(uint32_t)));
		rd->compute_list_end();
	};
	clear_pages();
	if (capture && check.pool_saved.is_valid()) {
		// The pool as the clear pass left it, then the pool as it would be without the check.
		check.pool_cleared = _pool_snapshot("Pool Cleared");
		_pool_copy(check.pool_saved, pool_texture);
		clear_pages();
		rd->free_rid(check.pool_saved);
		check.pool_saved = RID();
	}
	rd->draw_command_end_label();

	if (capture) {
		check.table_after = _snapshot(table_buffer, 0, uint32_t(table_buffer_bytes), "Table After");
		check.pages_after = _snapshot(pages_buffer, 0, page_count * 4 * uint32_t(sizeof(uint32_t)), "Pages After");
		check.state_after = _snapshot(state_buffer, 0, STATE_SIZE * uint32_t(sizeof(uint32_t)), "State After");
		check.map_state_after = _snapshot(map_state_buffer, 0, table_maps * MAP_STATE_REGIONS * uint32_t(sizeof(uint32_t)), "Map State After");
		check.dirty = _snapshot(dirty_buffer, 0, page_count * 4 * uint32_t(sizeof(uint32_t)), "Dirty");
		check.indirect = _snapshot(indirect_buffer, 0, INDIRECT_WORDS * uint32_t(sizeof(uint32_t)), "Indirect");
		check.armed = false;
		check.captured = true;
	}
	marked_views++;
}

namespace {

LocalVector<uint32_t> _read_words(RID p_buffer) {
	LocalVector<uint32_t> words;
	if (p_buffer.is_null()) {
		return words;
	}
	const Vector<uint8_t> bytes = RD::get_singleton()->buffer_get_data(p_buffer);
	words.resize(uint32_t(bytes.size()) / sizeof(uint32_t));
	if (words.size() > 0) {
		memcpy(words.ptr(), bytes.ptr(), words.size() * sizeof(uint32_t));
	}
	return words;
}

float _word_float(uint32_t p_word) {
	float value = 0.0f;
	memcpy(&value, &p_word, sizeof(float));
	return value;
}

// Request flags per entry: bit 0 requested, bit 1 coarse.
void _add_requests(HashMap<uint32_t, uint32_t> &r_set, const LocalVector<uint64_t> &p_requests) {
	for (uint64_t request : p_requests) {
		uint32_t &flags = r_set[uint32_t(request)];
		flags |= 1u | ((request & RendererVirtualShadowMapStorage::REQUEST_COARSE) ? 2u : 0u);
	}
}

} // namespace

RendererVirtualShadowMapStorage::MarkLight VirtualShadowMapStorage::_mark_light(const uint32_t *p_light_words, MapKind p_kind) {
	MarkLight light;
	light.kind = p_kind;
	light.position = Vector3(_word_float(p_light_words[LIGHT_DATA_POSITION / 4]), _word_float(p_light_words[LIGHT_DATA_POSITION / 4 + 1]), _word_float(p_light_words[LIGHT_DATA_POSITION / 4 + 2]));
	light.direction = Vector3(_word_float(p_light_words[LIGHT_DATA_DIRECTION / 4]), _word_float(p_light_words[LIGHT_DATA_DIRECTION / 4 + 1]), _word_float(p_light_words[LIGHT_DATA_DIRECTION / 4 + 2]));
	light.inv_radius = _word_float(p_light_words[(p_kind == MAP_KIND_AREA ? LIGHT_DATA_AREA_INV_RANGE : LIGHT_DATA_INV_RADIUS) / 4]);
	light.cos_angle = _word_float(p_light_words[LIGHT_DATA_COS_SPOT_ANGLE / 4]);
	light.fog_energy = _word_float(p_light_words[LIGHT_DATA_FOG_ENERGY / 4]);
	light.map_base = p_light_words[LIGHT_DATA_VSM_MAP_BASE / 4];
	light.flags = p_light_words[LIGHT_DATA_VSM_FLAGS / 4];
	return light;
}

Dictionary VirtualShadowMapStorage::get_check_result() {
	Dictionary result;
	result["captured"] = check.captured;
	result["armed"] = check.armed;
	if (!check.captured) {
		return result;
	}
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, result);
	PackedStringArray failures;
	const ViewParams &view = check.view;
	const uint32_t table_maps = check.table_maps;
	const uint64_t table_entries = uint64_t(table_maps) * ENTRIES_PER_MAP;
	const uint32_t width = view.screen[0];
	const uint32_t height = view.screen[1];

	const LocalVector<uint32_t> depth = _read_words(capture_depth_buffer);
	const LocalVector<uint32_t> cluster = _read_words(check.cluster);
	LocalVector<uint32_t> light_words[3];
	for (int type = 0; type < 3; type++) {
		light_words[type] = _read_words(check.lights[type]);
	}
	const LocalVector<uint32_t> bias_before = _read_words(check.bias_before);
	const LocalVector<uint32_t> table_marked = _read_words(check.table_marked);
	const LocalVector<uint32_t> pages_before = _read_words(check.pages_before);
	const LocalVector<uint32_t> state_marked = _read_words(check.state_marked);
	const LocalVector<uint32_t> request_list = _read_words(check.requests);
	const LocalVector<uint32_t> map_state_marked = _read_words(check.map_state_marked);
	const LocalVector<uint32_t> table_after = _read_words(check.table_after);
	const LocalVector<uint32_t> pages_after = _read_words(check.pages_after);
	const LocalVector<uint32_t> state_after = _read_words(check.state_after);
	const LocalVector<uint32_t> map_state_after = _read_words(check.map_state_after);
	const LocalVector<uint32_t> dirty = _read_words(check.dirty);
	const LocalVector<uint32_t> indirect = _read_words(check.indirect);
	Vector<uint8_t> pool_layers[POOL_LAYERS];
	if (check.pool_cleared.is_valid()) {
		for (uint32_t layer = 0; layer < POOL_LAYERS; layer++) {
			pool_layers[layer] = rd->texture_get_data(check.pool_cleared, layer);
		}
	}
	_free_check_buffers();

	if (depth.size() < width * height || table_marked.size() != table_entries || table_after.size() != table_entries || bias_before.size() != table_maps || state_after.size() != STATE_SIZE || pages_before.size() != check.pool_pages * 4 || pages_after.size() != check.pool_pages * 4 || map_state_after.size() != table_maps * MAP_STATE_REGIONS || check.map_data.size() != table_maps) {
		failures.push_back("the capture is incomplete");
		result["failures"] = failures;
		return result;
	}
	const MapData *maps = check.map_data.ptr();
	const uint32_t *bias = bias_before.ptr();
	LocalVector<MarkLight> lights[3];
	const MapKind light_kinds[3] = { MAP_KIND_OMNI, MAP_KIND_SPOT, MAP_KIND_AREA };
	const uint32_t light_counts[3] = { check.omni_count, check.spot_count, check.area_count };
	for (int type = 0; type < 3; type++) {
		for (uint32_t i = 0; i < light_counts[type] && (i + 1) * (LIGHT_DATA_BYTES / 4) <= light_words[type].size(); i++) {
			lights[type].push_back(_mark_light(light_words[type].ptr() + i * (LIGHT_DATA_BYTES / 4), light_kinds[type]));
		}
	}
	const uint32_t directional_count = view.counts[0];

	// 1. Marking: the CPU models over every pixel, fog vertex and box, against the GPU's flags.
	// A point's requests at view-space position p_view (the perturbation is added by the caller).
	auto mark_point = [&](uint32_t p_screen_x, uint32_t p_screen_y, const Vector3 &p_view, bool p_coarse, float p_cell_size, LocalVector<uint64_t> &r_requests) {
		const float view_depth = -float(p_view.z);
		const float footprint = view_depth * view.footprint[0] + view.footprint[1];
		const Vector3 world(row_dot(view.view_to_world[0], p_view), row_dot(view.view_to_world[1], p_view), row_dot(view.view_to_world[2], p_view));
		for (uint32_t i = 0; i < directional_count; i++) {
			const float max_distance = _word_float(view.directional[i][2]);
			const float fog_energy = _word_float(view.directional[i][3]);
			if (view_depth > max_distance || (p_coarse && !(fog_energy > 0.0f))) {
				continue;
			}
			mark_directional_point(maps, table_maps, bias, view.directional[i][0], view.directional[i][1], world, footprint, view.bias[0], p_coarse, r_requests);
		}
		const uint32_t cluster_x = p_screen_x >> view.cluster[0];
		const uint32_t cluster_y = p_screen_y >> view.cluster[0];
		const uint32_t cluster_offset = (view.cluster[1] * cluster_y + cluster_x) * (view.cluster[3] + 32);
		const uint32_t cluster_z = uint32_t(CLAMP((view_depth / view.footprint[2]) * 32.0f, 0.0f, 31.0f));
		for (uint32_t type = 0; type < 3; type++) {
			const uint32_t type_offset = cluster_offset + view.cluster[2] * type;
			if (type_offset + view.cluster[3] + cluster_z >= cluster.size()) {
				continue;
			}
			const uint32_t item_min_max = cluster[type_offset + view.cluster[3] + cluster_z];
			const uint32_t item_min = item_min_max & 0xFFFFu;
			const uint32_t item_max = item_min_max >> 16;
			const uint32_t item_from = item_min >> 5;
			const uint32_t item_to = item_max == 0 ? 0 : ((item_max - 1) >> 5) + 1;
			for (uint32_t i = item_from; i < item_to; i++) {
				uint32_t mask = cluster[type_offset + i];
				const int32_t local_min = CLAMP(int32_t(item_min) - int32_t(i) * 32, 0, 31);
				const int32_t mask_width = MIN(int32_t(item_max) - int32_t(item_min), 32 - local_min);
				const uint32_t range_mask = mask_width <= 0 ? 0u : (mask_width >= 32 ? 0xFFFFFFFFu : (((1u << mask_width) - 1u) << local_min));
				mask &= range_mask;
				for (uint32_t bit = 0; bit < 32; bit++) {
					if (!(mask & (1u << bit))) {
						continue;
					}
					const uint32_t index = 32 * i + bit;
					if (index >= lights[type].size()) {
						continue;
					}
					const MarkLight &light = lights[type][index];
					if (light.map_base == MAP_NONE || (p_coarse && !(light.fog_energy > 0.0f))) {
						continue;
					}
					const Vector3 rel = Vector3(float(p_view.x) - float(light.position.x), float(p_view.y) - float(light.position.y), float(p_view.z) - float(light.position.z));
					bool all_faces = false;
					if (p_coarse) {
						if (!light_reaches_cell(light, rel, p_cell_size, all_faces)) {
							continue;
						}
					} else if (!light_reaches(light, rel)) {
						continue;
					}
					mark_local_point(maps, table_maps, bias, light, world, footprint, view.bias[1], p_coarse, all_faces, r_requests);
				}
			}
		}
	};
	auto pixel_view = [&](uint32_t p_x, uint32_t p_y, bool &r_valid) {
		const float depth_value = _word_float(depth[p_y * width + p_x]);
		r_valid = depth_value > 0.0f;
		const float ndc_x = (float(p_x) + 0.5f) / float(width) * 2.0f - 1.0f;
		const float ndc_y = (float(p_y) + 0.5f) / float(height) * 2.0f - 1.0f;
		const Vector3 clip(ndc_x, ndc_y, depth_value);
		const float w = row_dot(view.inv_projection[3], clip);
		return Vector3(row_dot(view.inv_projection[0], clip) / w, row_dot(view.inv_projection[1], clip) / w, row_dot(view.inv_projection[2], clip) / w);
	};
	const bool fog = check.fog;
	const Vector3i fog_size(int32_t(view.fog_size[0]), int32_t(view.fog_size[1]), int32_t(view.fog_size[2]));
	auto froxel_view = [&](int32_t p_x, int32_t p_y, int32_t p_z) {
		float unit_x = float(p_x) / float(fog_size.x);
		float unit_y = float(p_y) / float(fog_size.y);
		float unit_z = float(p_z) / float(fog_size.z);
		unit_z = std::pow(unit_z, view.fog_a[1]);
		const float size_x = view.fog_a[2] + (view.fog_b[0] - view.fog_a[2]) * unit_z;
		const float size_y = view.fog_a[3] + (view.fog_b[1] - view.fog_a[3]) * unit_z;
		Vector3 result_view;
		result_view.x = (unit_x * 2.0f - 1.0f) * size_x;
		result_view.y = -((unit_y * 2.0f - 1.0f) * size_y);
		result_view.z = -view.fog_a[0] * unit_z;
		return result_view;
	};
	// Every source of requests, with p_perturbation 0 for the exact inputs and 1 to 8 for the corners
	// of a small box around them: a decision within float rounding of a page, mip, face or light edge
	// can go either way on the GPU.
	auto gather = [&](uint32_t p_perturbation, HashMap<uint32_t, uint32_t> *r_union, HashMap<uint32_t, uint32_t> *r_robust, HashMap<uint32_t, uint32_t> *r_nominal) {
		const float delta = 1e-5f;
		auto offset = [&](const Vector3 &p_value, uint32_t p_corner) {
			if (p_corner == 0) {
				return p_value;
			}
			const float scale = delta * MAX(MAX(Math::abs(float(p_value.x)), Math::abs(float(p_value.y))), MAX(Math::abs(float(p_value.z)), 1e-3f));
			const uint32_t c = p_corner - 1;
			return p_value + Vector3((c & 1) ? scale : -scale, (c & 2) ? scale : -scale, (c & 4) ? scale : -scale);
		};
		LocalVector<uint64_t> requests;
		LocalVector<uint64_t> corner_requests;
		auto evaluate = [&](auto &&p_source) {
			requests.clear();
			p_source(0u, requests);
			if (r_nominal != nullptr) {
				_add_requests(*r_nominal, requests);
			}
			if (p_perturbation == 0) {
				return;
			}
			// Union and intersection over the exact inputs and the eight corners.
			HashMap<uint32_t, uint32_t> item_flags;
			_add_requests(item_flags, requests);
			HashMap<uint32_t, uint32_t> robust_flags(item_flags);
			for (uint32_t corner = 1; corner <= 8; corner++) {
				corner_requests.clear();
				p_source(corner, corner_requests);
				HashMap<uint32_t, uint32_t> corner_flags;
				_add_requests(corner_flags, corner_requests);
				for (const KeyValue<uint32_t, uint32_t> &E : corner_flags) {
					item_flags[E.key] |= E.value;
				}
				for (KeyValue<uint32_t, uint32_t> &E : robust_flags) {
					const uint32_t *flags = corner_flags.getptr(E.key);
					E.value &= flags != nullptr ? *flags : 0u;
				}
			}
			for (const KeyValue<uint32_t, uint32_t> &E : item_flags) {
				(*r_union)[E.key] |= E.value;
			}
			for (const KeyValue<uint32_t, uint32_t> &E : robust_flags) {
				if (E.value != 0) {
					(*r_robust)[E.key] |= E.value;
				}
			}
		};
		for (uint32_t y = 0; y < height; y++) {
			for (uint32_t x = 0; x < width; x++) {
				bool valid = false;
				const Vector3 base = pixel_view(x, y, valid);
				if (!valid) {
					continue;
				}
				evaluate([&](uint32_t p_corner, LocalVector<uint64_t> &r_out) {
					mark_point(x, y, offset(base, p_corner), false, 0.0f, r_out);
				});
			}
		}
		if (fog) {
			for (int32_t z = 0; z <= fog_size.z; z++) {
				for (int32_t y = 0; y <= fog_size.y; y++) {
					for (int32_t x = 0; x <= fog_size.x; x++) {
						const Vector3 base = froxel_view(x, y, z);
						const Vector3 next = froxel_view(x + 1, y + 1, z + 1);
						const Vector3 diagonal(float(next.x) - float(base.x), float(next.y) - float(base.y), float(next.z) - float(base.z));
						const float cell_size = std::sqrt(float(diagonal.x) * float(diagonal.x) + float(diagonal.y) * float(diagonal.y) + float(diagonal.z) * float(diagonal.z));
						const uint32_t screen_x = MIN(uint32_t(float(x) / float(fog_size.x) * float(width)), width - 1);
						const uint32_t screen_y = MIN(uint32_t(float(y) / float(fog_size.y) * float(height)), height - 1);
						evaluate([&](uint32_t p_corner, LocalVector<uint64_t> &r_out) {
							mark_point(screen_x, screen_y, offset(base, p_corner), true, cell_size, r_out);
						});
					}
				}
			}
		}
		for (const AABB &box : check.boxes) {
			evaluate([&](uint32_t p_corner, LocalVector<uint64_t> &r_out) {
				AABB grown = box;
				if (p_corner > 0) {
					const float scale = delta * MAX(float(box.get_longest_axis_size()), 1e-3f) + delta * MAX(float(box.position.length()), 1.0f);
					grown = (p_corner & 1) ? box.grow(scale) : box.grow(-MIN(scale, float(box.size[box.get_shortest_axis_index()]) * 0.25f));
				}
				for (uint32_t i = 0; i < directional_count; i++) {
					mark_directional_box(maps, table_maps, view.directional[i][0], view.directional[i][1], grown, r_out);
				}
				for (uint32_t type = 0; type < 3; type++) {
					for (const MarkLight &light : lights[type]) {
						if (light.map_base == MAP_NONE) {
							continue;
						}
						const Vector3 light_world(row_dot(view.view_to_world[0], light.position), row_dot(view.view_to_world[1], light.position), row_dot(view.view_to_world[2], light.position));
						mark_local_box(maps, table_maps, light, light_world, grown, r_out);
					}
				}
			});
		}
	};

	// The GPU's flags after marking.
	HashMap<uint32_t, uint32_t> gpu;
	for (uint64_t i = 0; i < table_entries; i++) {
		const uint32_t entry = table_marked[i];
		if (entry & ENTRY_REQUESTED) {
			gpu[uint32_t(i)] = 1u | ((entry & ENTRY_COARSE) ? 2u : 0u);
		}
	}
	HashMap<uint32_t, uint32_t> nominal;
	gather(0, nullptr, nullptr, &nominal);
	uint32_t matched = 0;
	uint32_t gpu_only = 0;
	uint32_t cpu_only = 0;
	uint32_t coarse_mismatch = 0;
	uint32_t coarse_cpu = 0;
	for (const KeyValue<uint32_t, uint32_t> &E : gpu) {
		const uint32_t *flags = nominal.getptr(E.key);
		if (flags == nullptr) {
			gpu_only++;
		} else if (*flags != E.value) {
			coarse_mismatch++;
		} else {
			matched++;
		}
	}
	for (const KeyValue<uint32_t, uint32_t> &E : nominal) {
		cpu_only += gpu.has(E.key) ? 0 : 1;
		coarse_cpu += (E.value & 2u) ? 1 : 0;
	}
	const bool exact = gpu_only == 0 && cpu_only == 0 && coarse_mismatch == 0;
	uint32_t unexplained_gpu = 0;
	uint32_t unexplained_cpu = 0;
	uint32_t unexplained_coarse = 0;
	if (!exact) {
		// Within float rounding: every GPU flag must come from some perturbed input, and every flag the
		// CPU sets for all of them must be on the GPU.
		HashMap<uint32_t, uint32_t> allowed;
		HashMap<uint32_t, uint32_t> required;
		gather(1, &allowed, &required, nullptr);
		for (const KeyValue<uint32_t, uint32_t> &E : gpu) {
			const uint32_t *flags = allowed.getptr(E.key);
			if (flags == nullptr) {
				unexplained_gpu++;
			} else if ((E.value & ~*flags) != 0) {
				unexplained_coarse++;
			}
		}
		for (const KeyValue<uint32_t, uint32_t> &E : required) {
			const uint32_t *flags = gpu.getptr(E.key);
			if (flags == nullptr) {
				unexplained_cpu++;
			} else if ((E.value & ~*flags) != 0) {
				unexplained_coarse++;
			}
		}
	}
	Dictionary mark;
	mark["pixels"] = int64_t(width) * height;
	mark["fog_vertices"] = fog ? int64_t(fog_size.x + 1) * (fog_size.y + 1) * (fog_size.z + 1) : int64_t(0);
	mark["boxes"] = int64_t(check.boxes.size());
	mark["directional_lights"] = int64_t(directional_count);
	mark["local_lights"] = int64_t(lights[0].size() + lights[1].size() + lights[2].size());
	mark["gpu_requested"] = int64_t(gpu.size());
	mark["cpu_requested"] = int64_t(nominal.size());
	mark["cpu_coarse"] = int64_t(coarse_cpu);
	mark["matched"] = int64_t(matched);
	mark["gpu_only"] = int64_t(gpu_only);
	mark["cpu_only"] = int64_t(cpu_only);
	mark["coarse_mismatch"] = int64_t(coarse_mismatch);
	mark["exact"] = exact;
	mark["unexplained_gpu"] = int64_t(unexplained_gpu);
	mark["unexplained_cpu"] = int64_t(unexplained_cpu);
	mark["unexplained_coarse"] = int64_t(unexplained_coarse);
	result["mark"] = mark;
	if (unexplained_gpu + unexplained_cpu + unexplained_coarse > 0) {
		failures.push_back(vformat("marking: %d GPU requests, %d CPU requests and %d coarse flags are not explained by float rounding", unexplained_gpu, unexplained_cpu, unexplained_coarse));
	}

	// 2. The request list: each requested entry once, and the per-map counts.
	const uint32_t listed = MIN(state_marked[STATE_REQUESTS], check.request_capacity);
	HashSet<uint32_t> list_set;
	uint32_t list_duplicates = 0;
	uint32_t list_unrequested = 0;
	LocalVector<uint32_t> list_per_map;
	list_per_map.resize(table_maps);
	memset(list_per_map.ptr(), 0, table_maps * sizeof(uint32_t));
	for (uint32_t i = 0; i < listed && i < request_list.size(); i++) {
		const uint32_t index = request_list[i];
		if (list_set.has(index)) {
			list_duplicates++;
			continue;
		}
		list_set.insert(index);
		if (uint64_t(index) >= table_entries || !gpu.has(index)) {
			list_unrequested++;
			continue;
		}
		list_per_map[index / ENTRIES_PER_MAP]++;
	}
	uint32_t list_missing = 0;
	for (const KeyValue<uint32_t, uint32_t> &E : gpu) {
		list_missing += list_set.has(E.key) ? 0 : 1;
	}
	uint32_t map_count_mismatches = 0;
	for (uint32_t m = 0; m < table_maps; m++) {
		map_count_mismatches += list_per_map[m] != map_state_marked[MAP_STATE_REQUESTS * table_maps + m] ? 1 : 0;
	}
	Dictionary list;
	list["listed"] = int64_t(listed);
	list["dropped"] = int64_t(state_marked[STATE_REQUESTS_DROPPED]);
	list["duplicates"] = int64_t(list_duplicates);
	list["unrequested"] = int64_t(list_unrequested);
	list["missing"] = int64_t(list_missing);
	list["map_count_mismatches"] = int64_t(map_count_mismatches);
	result["request_list"] = list;
	if (list_duplicates + list_unrequested + list_missing + map_count_mismatches > 0) {
		failures.push_back(vformat("request list: %d duplicates, %d unrequested, %d missing, %d per-map counts off", list_duplicates, list_unrequested, list_missing, map_count_mismatches));
	}

	// 3. Allocation: the CPU model on the state after marking, against the GPU's counters, and the
	// GPU's table and pool header after allocation against the invariants.
	LocalVector<uint32_t> model_table(table_marked);
	LocalVector<PageRecord> model_pages;
	model_pages.resize(check.pool_pages);
	for (uint32_t p = 0; p < check.pool_pages; p++) {
		model_pages[p].owner = pages_before[p * 4];
		model_pages[p].last_used = pages_before[p * 4 + 1];
		model_pages[p].bucket = pages_before[p * 4 + 2];
	}
	const AllocationCounts model = allocate_model(model_table.ptr(), table_entries, model_pages.ptr(), check.pool_pages, check.grid_width, request_list.ptr(), MIN(listed, request_list.size()), check.frame, check.cache_frames);
	Dictionary counts;
	const struct {
		const char *name;
		uint32_t model;
		uint32_t gpu;
	} pairs[] = {
		{ "kept", model.kept, state_after[STATE_KEPT] },
		{ "new", model.new_pages, state_after[STATE_NEW] },
		{ "stolen", model.stolen, state_after[STATE_STOLEN] },
		{ "overflow", model.overflow, state_after[STATE_OVERFLOW] },
		{ "evicted", model.evicted, state_after[STATE_EVICTED] },
		{ "orphaned", model.orphaned, state_after[STATE_ORPHANED] },
		{ "cached", model.cached, state_after[STATE_CACHED] },
		{ "available", model.available, state_after[STATE_AVAILABLE] },
		{ "dirty", model.dirty, state_after[STATE_DIRTY] },
		{ "coarse", model.coarse, state_after[STATE_COARSE] },
	};
	uint32_t count_mismatches = 0;
	for (const auto &pair : pairs) {
		counts[pair.name] = int64_t(pair.gpu);
		counts[String(pair.name) + "_model"] = int64_t(pair.model);
		count_mismatches += pair.model != pair.gpu ? 1 : 0;
	}
	counts["requests"] = int64_t(state_after[STATE_REQUESTS]);
	counts["dirty_directional"] = int64_t(state_after[STATE_DIRTY_DIRECTIONAL]);
	counts["max_map_requests"] = int64_t(state_after[STATE_MAX_MAP_REQUESTS]);
	counts["biased_maps"] = int64_t(state_after[STATE_BIASED_MAPS]);
	counts["total_overflow"] = int64_t(state_after[STATE_TOTAL_OVERFLOW]);
	counts["overflow_views"] = int64_t(state_after[STATE_OVERFLOW_VIEWS]);
	result["allocation"] = counts;
	if (count_mismatches > 0) {
		failures.push_back(vformat("allocation: %d counters differ from the CPU model", count_mismatches));
	}
	// The order of the free list (free pages, then cached pages least recently used first): the pages
	// handed out by their bucket in the update step, on the GPU and in the model. Within a bucket the
	// GPU's atomic order differs from the model's; the counts per bucket do not.
	auto handed_out_by_bucket = [&](const LocalVector<uint32_t> &p_owner_words, uint32_t p_stride, LocalVector<uint32_t> &r_histogram) {
		r_histogram.resize(BUCKET_COUNT);
		memset(r_histogram.ptr(), 0, BUCKET_COUNT * sizeof(uint32_t));
		uint32_t unbucketed = 0;
		for (uint32_t p = 0; p < check.pool_pages; p++) {
			const uint32_t owner = p_owner_words[p * p_stride];
			if (owner == 0 || uint64_t(owner - 1) >= table_entries) {
				continue;
			}
			// Handed out: the owner's entry did not map this page before allocation.
			const uint32_t entry_before = table_marked[owner - 1];
			if ((entry_before & ENTRY_ALLOCATED) && entry_page_y(entry_before) * check.grid_width + entry_page_x(entry_before) == p) {
				continue;
			}
			PageRecord before;
			before.owner = pages_before[p * 4];
			before.last_used = pages_before[p * 4 + 1];
			uint32_t bucket = BUCKET_NONE;
			page_update(before, p, check.grid_width, table_marked.ptr(), table_entries, check.frame, check.cache_frames, bucket);
			if (bucket < BUCKET_COUNT) {
				r_histogram[bucket]++;
			} else {
				unbucketed++;
			}
		}
		return unbucketed;
	};
	LocalVector<uint32_t> model_owner_words;
	model_owner_words.resize(check.pool_pages);
	for (uint32_t p = 0; p < check.pool_pages; p++) {
		model_owner_words[p] = model_pages[p].owner;
	}
	LocalVector<uint32_t> gpu_handed;
	LocalVector<uint32_t> model_handed;
	const uint32_t gpu_unbucketed = handed_out_by_bucket(pages_after, 4, gpu_handed);
	const uint32_t model_unbucketed = handed_out_by_bucket(model_owner_words, 1, model_handed);
	uint32_t lru_mismatches = 0;
	uint32_t handed_free = 0;
	uint32_t handed_cached = 0;
	for (uint32_t b = 0; b < BUCKET_COUNT; b++) {
		lru_mismatches += gpu_handed[b] != model_handed[b] ? 1 : 0;
		(b == 0 ? handed_free : handed_cached) += gpu_handed[b];
	}
	counts["handed_out_free"] = int64_t(handed_free);
	counts["handed_out_cached"] = int64_t(handed_cached);
	counts["lru_bucket_mismatches"] = int64_t(lru_mismatches);
	if (lru_mismatches > 0 || gpu_unbucketed > 0 || model_unbucketed > 0) {
		failures.push_back(vformat("allocation: the pages handed out differ from the model's least-recently-used order in %d buckets (%d GPU and %d model pages from no bucket)", lru_mismatches, gpu_unbucketed, model_unbucketed));
	}
	// Invariants after allocation.
	uint32_t owner_mismatches = 0; // A page whose owner does not map it.
	uint32_t dangling = 0; // An allocated entry whose page is not owned by it.
	uint32_t double_mapped = 0; // Pages mapped by two entries.
	uint32_t unserved = 0; // A requested entry with neither a page nor the overflow flag.
	uint32_t overflow_flagged = 0;
	uint32_t moved = 0; // A kept entry whose page changed.
	uint32_t flags_left = 0; // Request or dirty flags left after the emit pass.
	uint32_t evicted_kept = 0; // An entry the model drops that the GPU kept.
	uint32_t resident = 0;
	uint32_t stamp_mismatches = 0;
	uint32_t coarse_overflowed = 0; // Coarse requests left without a page.
	uint32_t fine_served_new = 0; // Fine requests that got a new page.
	LocalVector<uint32_t> page_users;
	page_users.resize(check.pool_pages);
	memset(page_users.ptr(), 0, check.pool_pages * sizeof(uint32_t));
	for (uint64_t i = 0; i < table_entries; i++) {
		const uint32_t entry = table_after[i];
		if (entry & (ENTRY_REQUESTED | ENTRY_COARSE | ENTRY_DIRTY_MASK | ENTRY_SCROLLED)) {
			flags_left++;
		}
		const uint32_t before = table_marked[i];
		if ((before & ENTRY_ALLOCATED) && !(model_table[i] & ENTRY_ALLOCATED) && (entry & ENTRY_ALLOCATED) && !(before & ENTRY_REQUESTED)) {
			// The model evicted or orphaned it; the GPU may only differ for pages it gave away (stolen).
			evicted_kept++;
		}
		if (before & ENTRY_REQUESTED) {
			if (entry & ENTRY_ALLOCATED) {
				if ((before & ENTRY_ALLOCATED) && (entry_page_x(before) != entry_page_x(entry) || entry_page_y(before) != entry_page_y(entry))) {
					moved++;
				}
			} else if (entry & ENTRY_OVERFLOW) {
				overflow_flagged++;
				coarse_overflowed += (before & ENTRY_COARSE) ? 1 : 0;
			} else {
				unserved++;
			}
		}
		if ((before & ENTRY_REQUESTED) && !(before & ENTRY_COARSE) && !(before & ENTRY_ALLOCATED) && (entry & ENTRY_ALLOCATED)) {
			fine_served_new++;
		}
		if (!(entry & ENTRY_ALLOCATED)) {
			continue;
		}
		resident++;
		const uint32_t p = entry_page_y(entry) * check.grid_width + entry_page_x(entry);
		if (p >= check.pool_pages || pages_after[p * 4] != uint32_t(i) + 1) {
			dangling++;
			continue;
		}
		page_users[p]++;
		if ((before & ENTRY_REQUESTED) && pages_after[p * 4 + 1] != check.frame) {
			stamp_mismatches++;
		}
	}
	for (uint32_t p = 0; p < check.pool_pages; p++) {
		double_mapped += page_users[p] > 1 ? 1 : 0;
		const uint32_t owner = pages_after[p * 4];
		if (owner != 0) {
			const uint64_t index = owner - 1;
			const uint32_t entry = index < table_entries ? table_after[index] : 0u;
			if (!(entry & ENTRY_ALLOCATED) || entry_page_y(entry) * check.grid_width + entry_page_x(entry) != p) {
				owner_mismatches++;
			}
		}
	}
	if (overflow_flagged != state_after[STATE_OVERFLOW]) {
		failures.push_back(vformat("allocation: %d requested entries carry the overflow flag, the counter says %d", overflow_flagged, state_after[STATE_OVERFLOW]));
	}
	// Evicted entries the GPU kept can only be ones the GPU did not steal and the model did: the
	// steal order inside a bucket differs. Their number cannot exceed the stolen count.
	// Coarse requests take pages first: none may go without while a fine request got a new page.
	if (coarse_overflowed > 0 && fine_served_new > 0) {
		failures.push_back(vformat("allocation: %d coarse requests have no page while %d fine requests got new ones", coarse_overflowed, fine_served_new));
	}
	if (evicted_kept > model.stolen) {
		failures.push_back(vformat("allocation: %d entries the model drops are still mapped on the GPU", evicted_kept));
	}
	Dictionary invariants;
	invariants["owner_mismatches"] = int64_t(owner_mismatches);
	invariants["dangling_entries"] = int64_t(dangling);
	invariants["double_mapped_pages"] = int64_t(double_mapped);
	invariants["unserved_requests"] = int64_t(unserved);
	invariants["overflow_flagged"] = int64_t(overflow_flagged);
	invariants["moved_pages"] = int64_t(moved);
	invariants["flags_left"] = int64_t(flags_left);
	invariants["evicted_but_mapped"] = int64_t(evicted_kept);
	invariants["resident_pages"] = int64_t(resident);
	invariants["stamp_mismatches"] = int64_t(stamp_mismatches);
	invariants["coarse_overflowed"] = int64_t(coarse_overflowed);
	invariants["fine_served_new"] = int64_t(fine_served_new);
	result["invariants"] = invariants;
	if (owner_mismatches + dangling + double_mapped + unserved + moved + flags_left + stamp_mismatches > 0) {
		failures.push_back(vformat("allocation invariants: %d owner mismatches, %d dangling entries, %d double-mapped pages, %d unserved requests, %d moved pages, %d flags left, %d stamps", owner_mismatches, dangling, double_mapped, unserved, moved, flags_left, stamp_mismatches));
	}

	// 4. The dirty list: exactly the requested entries whose page is new or must be re-rendered, each
	// map's pages together where MODE_MAPS placed them, directional maps first, and the arguments.
	const uint32_t dirty_count = state_after[STATE_DIRTY];
	const uint32_t dirty_directional = state_after[STATE_DIRTY_DIRECTIONAL];
	HashSet<uint32_t> expected_dirty;
	for (uint32_t i = 0; i < MIN(listed, request_list.size()); i++) {
		const uint32_t index = request_list[i];
		const uint32_t before = table_marked[index];
		const uint32_t after = table_after[index];
		if (!(after & ENTRY_ALLOCATED)) {
			continue;
		}
		if (!(before & ENTRY_ALLOCATED) || (before & (ENTRY_SCROLLED | ENTRY_UNCACHED | ENTRY_DIRTY_MASK))) {
			expected_dirty.insert(index);
		}
	}
	uint32_t dirty_wrong = 0;
	uint32_t dirty_misplaced = 0;
	HashSet<uint32_t> listed_dirty;
	for (uint32_t r = 0; r < dirty_count && (r + 1) * 4 <= dirty.size(); r++) {
		const uint32_t map = dirty[r * 4];
		const uint32_t packed = dirty[r * 4 + 1];
		const uint32_t mip = packed & 0xFFu;
		if (map >= table_maps || mip >= MIP_COUNT) {
			dirty_wrong++;
			continue;
		}
		const uint32_t index = entry_index(map, mip, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu);
		const uint32_t offset = map_state_after[MAP_STATE_DIRTY_OFFSET * table_maps + map];
		const uint32_t count = map_state_after[MAP_STATE_DIRTY_COUNT * table_maps + map];
		const bool directional = maps[map].kind == MAP_KIND_DIRECTIONAL;
		if (r < offset || r >= offset + count || directional != (r < dirty_directional)) {
			dirty_misplaced++;
		}
		if (!expected_dirty.has(index) || listed_dirty.has(index) || dirty[r * 4 + 2] != (table_after[index] & ~uint32_t(ENTRY_FLAGS_MASK))) {
			dirty_wrong++;
			continue;
		}
		listed_dirty.insert(index);
	}
	const uint32_t dirty_missing = expected_dirty.size() - listed_dirty.size();
	const uint32_t expected_args[INDIRECT_WORDS] = { dirty_directional, 1, 1, dirty_count - dirty_directional, 1, 1, dirty_count, 1, 1 };
	uint32_t args_wrong = 0;
	for (uint32_t i = 0; i < INDIRECT_WORDS; i++) {
		args_wrong += (i >= indirect.size() || indirect[i] != expected_args[i]) ? 1 : 0;
	}
	Dictionary dirty_info;
	dirty_info["pages"] = int64_t(dirty_count);
	dirty_info["directional_pages"] = int64_t(dirty_directional);
	dirty_info["expected"] = int64_t(expected_dirty.size());
	dirty_info["wrong"] = int64_t(dirty_wrong);
	dirty_info["misplaced"] = int64_t(dirty_misplaced);
	dirty_info["missing"] = int64_t(dirty_missing);
	dirty_info["indirect_args_wrong"] = int64_t(args_wrong);
	result["dirty"] = dirty_info;
	if (dirty_wrong + dirty_misplaced + dirty_missing + args_wrong > 0 || dirty_count != expected_dirty.size()) {
		failures.push_back(vformat("dirty list: %d pages for %d expected, %d wrong, %d misplaced, %d missing, %d indirect words wrong", dirty_count, expected_dirty.size(), dirty_wrong, dirty_misplaced, dirty_missing, args_wrong));
	}

	// 5. The page clear: the layers each dirty record names are 0, every other texel of the pool keeps
	// the value it was filled with before allocation (non-zero).
	{
		const uint32_t texture_width = check.grid_width * PAGE_SIZE;
		const uint64_t layer_bytes = uint64_t(texture_width) * uint64_t(pool_page_grid(check.pool_pages).y) * PAGE_SIZE * sizeof(uint32_t);
		LocalVector<uint8_t> cleared_layers; // Per page: bit 0 static layer, bit 1 dynamic layer.
		cleared_layers.resize(check.pool_pages);
		memset(cleared_layers.ptr(), 0, check.pool_pages);
		for (uint32_t r = 0; r < dirty_count && (r + 1) * 4 <= dirty.size(); r++) {
			const uint32_t p = entry_page_y(dirty[r * 4 + 2]) * check.grid_width + entry_page_x(dirty[r * 4 + 2]);
			if (p < check.pool_pages) {
				cleared_layers[p] = uint8_t(cleared_layers[p] | ((dirty[r * 4 + 3] & ENTRY_DIRTY_STATIC) ? 1u : 0u) | ((dirty[r * 4 + 3] & ENTRY_DIRTY_DYNAMIC) ? 2u : 0u));
			}
		}
		uint64_t not_cleared = 0; // Texels of a cleared layer that are not 0.
		uint64_t touched = 0; // Texels outside every cleared layer that lost the fill value.
		uint32_t fill = 0;
		bool fill_known = false;
		bool complete = check.pool_pages > 0;
		for (uint32_t layer = 0; layer < POOL_LAYERS; layer++) {
			complete = complete && uint64_t(pool_layers[layer].size()) == layer_bytes;
		}
		if (complete) {
			for (uint32_t layer = 0; layer < POOL_LAYERS; layer++) {
				const uint32_t *texels = reinterpret_cast<const uint32_t *>(pool_layers[layer].ptr());
				for (uint32_t p = 0; p < check.pool_pages; p++) {
					const bool cleared = ((cleared_layers[p] >> layer) & 1u) != 0;
					const uint64_t base = uint64_t(p / check.grid_width) * PAGE_SIZE * texture_width + uint64_t(p % check.grid_width) * PAGE_SIZE;
					for (uint32_t y = 0; y < PAGE_SIZE; y++) {
						const uint32_t *row = texels + base + uint64_t(y) * texture_width;
						for (uint32_t x = 0; x < PAGE_SIZE; x++) {
							if (cleared) {
								not_cleared += row[x] != 0 ? 1 : 0;
							} else if (!fill_known) {
								fill = row[x];
								fill_known = true;
							} else {
								touched += row[x] != fill ? 1 : 0;
							}
						}
					}
				}
			}
		}
		uint32_t cleared_pages = 0;
		for (uint32_t p = 0; p < check.pool_pages; p++) {
			cleared_pages += cleared_layers[p] != 0 ? 1 : 0;
		}
		Dictionary clear_info;
		clear_info["checked"] = complete;
		clear_info["cleared_pages"] = int64_t(cleared_pages);
		clear_info["not_cleared_texels"] = int64_t(not_cleared);
		clear_info["touched_texels"] = int64_t(touched);
		clear_info["fill"] = int64_t(fill);
		result["clear"] = clear_info;
		if (!complete) {
			failures.push_back("page clear: the pool was not captured");
		} else if (not_cleared > 0 || touched > 0 || (fill_known && fill == 0)) {
			failures.push_back(vformat("page clear: %d texels of cleared pages are not 0, %d other texels changed (fill value %d)", int64_t(not_cleared), int64_t(touched), int64_t(fill)));
		}
	}

	// 6. The overflow bias of every map. It moves once per frame: the view is the frame's first when the
	// last bias update was in another frame.
	uint32_t max_requests = 0;
	for (uint32_t m = 0; m < table_maps; m++) {
		max_requests = MAX(max_requests, map_state_marked[MAP_STATE_REQUESTS * table_maps + m]);
	}
	const bool new_frame = state_marked[STATE_BIAS_FRAME] != check.frame + 1;
	uint32_t bias_mismatches = 0;
	uint32_t biased = 0;
	uint32_t unowned_biased = 0; // Maps no light owns with a bias state: a freed light's bias left behind.
	for (uint32_t m = 0; m < table_maps; m++) {
		const uint32_t expected = overflow_bias_update(bias_before[m], map_state_marked[MAP_STATE_REQUESTS * table_maps + m], max_requests, state_after[STATE_OVERFLOW], new_frame);
		bias_mismatches += expected != map_state_after[MAP_STATE_BIAS * table_maps + m] ? 1 : 0;
		biased += overflow_bias(expected) != 0 ? 1 : 0;
		if (maps[m].kind == MAP_KIND_MAX && (bias_before[m] != 0 || map_state_after[MAP_STATE_BIAS * table_maps + m] != 0)) {
			unowned_biased++;
		}
	}
	Dictionary bias_info;
	bias_info["mismatches"] = int64_t(bias_mismatches);
	bias_info["biased_maps"] = int64_t(biased);
	bias_info["max_map_requests"] = int64_t(max_requests);
	bias_info["new_frame"] = new_frame;
	bias_info["unowned_biased"] = int64_t(unowned_biased);
	result["bias"] = bias_info;
	if (bias_mismatches > 0 || max_requests != state_after[STATE_MAX_MAP_REQUESTS] || biased != state_after[STATE_BIASED_MAPS]) {
		failures.push_back(vformat("overflow bias: %d maps differ from the CPU model", bias_mismatches));
	}
	if (unowned_biased > 0) {
		failures.push_back(vformat("overflow bias: %d maps no light owns keep a bias state", unowned_biased));
	}

	result["table_maps"] = int64_t(table_maps);
	result["pool_pages"] = int64_t(check.pool_pages);
	result["frame"] = int64_t(check.frame);
	result["size"] = Vector2i(int32_t(width), int32_t(height));
	result["failures"] = failures;
	result["pass"] = failures.is_empty();
	return result;
}

} // namespace RendererRD
