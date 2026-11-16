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
#include "core/variant/array.h"
#include "servers/rendering/renderer_compositor.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"

#include <cmath>
#include <cstring>

namespace RendererRD {

// GodotNext: vsm_page_pool (#220).

VirtualShadowMapStorage *VirtualShadowMapStorage::singleton = nullptr;

bool VirtualShadowMapStorage::is_dev_switch_on(PoolQuality *r_quality) {
#ifdef DEBUG_ENABLED
	// -2: not read yet, -1: off, otherwise the pool quality.
	static int switch_state = -2;
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
		} else if (!value.is_empty() && value != "0") {
			WARN_PRINT(vformat("GODOT_DEV_VSM=%s is not 1, low, medium, high or epic; virtual shadow maps stay off.", value));
		}
	}
	if (switch_state < 0) {
		return false;
	}
	if (r_quality != nullptr) {
		*r_quality = PoolQuality(switch_state);
	}
	return true;
#else
	return false;
#endif
}

VirtualShadowMapStorage::VirtualShadowMapStorage() {
	singleton = this;
	PoolQuality quality = POOL_QUALITY_HIGH;
	if (is_dev_switch_on(&quality)) {
		active_quality = quality;
		active = pool_create(pool_quality_pages(quality));
		if (active) {
			print_line(vformat("VSM: GODOT_DEV_VSM is set: virtual shadow maps are active with the %s pool (%d pages, %d MiB).", pool_quality_name(quality), get_pool_page_count(), int64_t(pool_bytes(get_pool_page_count()) >> 20)));
		}
	}
}

VirtualShadowMapStorage::~VirtualShadowMapStorage() {
	free_all();
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr && table_buffer.is_valid()) {
		rd->free_rid(table_buffer);
	}
	table_buffer = RID();
	table_buffer_bytes = 0;
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
	return true;
}

void VirtualShadowMapStorage::_table_clear(uint32_t p_first_map, uint32_t p_map_count) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd == nullptr || table_buffer.is_null() || p_map_count == 0) {
		return;
	}
	// The physical pages these entries held are not tracked here yet: the allocation pass (#219)
	// reclaims them.
	rd->buffer_clear(table_buffer, uint32_t(table_bytes(p_first_map)), uint32_t(table_bytes(p_map_count)));
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

void VirtualShadowMapStorage::update_light(RID p_light_instance, MapKind p_kind, bool p_casts_shadow, const Basis &p_light_basis, const Vector3 &p_camera_position, float p_max_distance, bool p_move_clipmaps, uint32_t &r_map_base, uint32_t &r_flags) {
	r_map_base = MAP_NONE;
	r_flags = 0;
	if (!active) {
		return;
	}
	// Stale lights are evicted by frame: every view of a frame stamps the lights it draws.
	if (RSG::rasterizer != nullptr && RSG::rasterizer->get_frame_number() != get_frame()) {
		set_frame(RSG::rasterizer->get_frame_number());
	}
	if (!p_casts_shadow) {
		map_free(p_light_instance);
		return;
	}
	uint32_t first = MAP_NONE;
	if (p_kind == MAP_KIND_DIRECTIONAL) {
		if (p_move_clipmaps) {
			first = directional_update(p_light_instance, p_light_basis, p_camera_position, p_max_distance);
		} else {
			// Reflection probes read the maps of the main view's camera.
			const MapRange *range = map_touch(p_light_instance);
			if (range != nullptr && range->kind == MAP_KIND_DIRECTIONAL) {
				first = range->first;
			}
		}
	} else {
		first = map_allocate(p_light_instance, p_kind);
	}
	const MapRange *range = map_get(p_light_instance);
	if (first == MAP_NONE || range == nullptr) {
		return;
	}
	r_map_base = first;
	r_flags = light_flags(range->kind, range->count);
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
	result["expected_buffer_delta"] = int64_t(table_bytes(capacity_after)) - int64_t(table_bytes(capacity_before));
	result["expected_texture_delta"] = int64_t(0);
	result["expected_buffer_count_delta"] = int64_t(capacity_before == 0 && capacity_after > 0 ? 1 : 0);
	return result;
}

} // namespace RendererRD
