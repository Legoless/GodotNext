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

#include "core/error/error_macros.h"
#include "core/math/math_funcs.h"
#include "core/variant/variant.h"

// GodotNext: vsm_page_pool (#220).

static_assert(RendererVirtualShadowMapStorage::ENTRIES_PER_MAP == 5461, "64 x 64 pages, 7 mips.");
static_assert(RendererVirtualShadowMapStorage::PAGES_PER_SIDE <= (1u << RendererVirtualShadowMapStorage::ENTRY_PAGE_BITS), "A page coordinate fits its entry field.");
static_assert(RendererVirtualShadowMapStorage::POOL_MAX_PAGES_PER_SIDE <= (1u << RendererVirtualShadowMapStorage::ENTRY_PAGE_BITS), "Every pool page is addressable.");
static_assert(RendererVirtualShadowMapStorage::CLIPMAP_MAX_LEVELS <= RendererVirtualShadowMapStorage::LIGHT_FLAGS_COUNT_MASK, "The level count fits vsm_flags.");
static_assert(RendererVirtualShadowMapStorage::TABLE_MAX_MAPS % RendererVirtualShadowMapStorage::TABLE_GROWTH_MAPS == 0, "The table grows to its cap.");
static_assert(RendererVirtualShadowMapStorage::table_bytes(512) <= 16ull * 1024 * 1024, "Budget V6: the page table of 64 lights and a directional light stays within 16 MiB.");
static_assert(RendererVirtualShadowMapStorage::pool_bytes(RendererVirtualShadowMapStorage::pool_quality_pages(RendererVirtualShadowMapStorage::POOL_QUALITY_LOW)) == 64ull * 1024 * 1024, "Budget V6, Low.");
static_assert(RendererVirtualShadowMapStorage::pool_bytes(RendererVirtualShadowMapStorage::pool_quality_pages(RendererVirtualShadowMapStorage::POOL_QUALITY_MEDIUM)) == 128ull * 1024 * 1024, "Budget V6, Medium.");
static_assert(RendererVirtualShadowMapStorage::pool_bytes(RendererVirtualShadowMapStorage::pool_quality_pages(RendererVirtualShadowMapStorage::POOL_QUALITY_HIGH)) == 256ull * 1024 * 1024, "Budget V6, High.");
static_assert(RendererVirtualShadowMapStorage::pool_bytes(RendererVirtualShadowMapStorage::pool_quality_pages(RendererVirtualShadowMapStorage::POOL_QUALITY_EPIC)) == 512ull * 1024 * 1024, "Budget V6, Epic.");

// Floor division by a power of two, also for negative values.
static _FORCE_INLINE_ int32_t _floor_shift(int32_t p_value, uint32_t p_shift) {
	return p_value >= 0 ? (p_value >> p_shift) : -((-p_value + int32_t((1u << p_shift) - 1)) >> p_shift);
}

Vector2i RendererVirtualShadowMapStorage::pool_page_grid(uint32_t p_page_count) {
	if (p_page_count == 0) {
		return Vector2i();
	}
	uint32_t width = 1;
	while (width * width < p_page_count) {
		width <<= 1;
	}
	const uint32_t height = (p_page_count + width - 1) / width;
	return Vector2i(int32_t(width), int32_t(height));
}

const char *RendererVirtualShadowMapStorage::pool_quality_name(PoolQuality p_quality) {
	switch (p_quality) {
		case POOL_QUALITY_LOW:
			return "low";
		case POOL_QUALITY_MEDIUM:
			return "medium";
		case POOL_QUALITY_HIGH:
			return "high";
		case POOL_QUALITY_EPIC:
			return "epic";
		default:
			return "invalid";
	}
}

uint32_t RendererVirtualShadowMapStorage::map_kind_map_count(MapKind p_kind, uint32_t p_levels) {
	switch (p_kind) {
		case MAP_KIND_DIRECTIONAL:
			return CLAMP(p_levels, 1u, CLIPMAP_MAX_LEVELS);
		case MAP_KIND_SPOT:
			return 1;
		case MAP_KIND_OMNI:
			return 6;
		case MAP_KIND_AREA:
			return 5;
		default:
			return 0;
	}
}

/* CLIPMAPS */

uint32_t RendererVirtualShadowMapStorage::clipmap_level_count(float p_max_distance) {
	// The coarsest level must reach p_max_distance from the camera on every side. The window is
	// centered on the camera's page, so its near edge can be one page closer than half its extent.
	uint32_t count = 1;
	while (count < CLIPMAP_MAX_LEVELS) {
		const uint32_t level = count - 1;
		const float reach = clipmap_level_extent(level) * 0.5f - clipmap_page_world_size(level);
		if (reach >= p_max_distance) {
			break;
		}
		count++;
	}
	return count;
}

float RendererVirtualShadowMapStorage::clipmap_level_extent(uint32_t p_level) {
	return CLIPMAP_LEVEL0_EXTENT * float(1u << MIN(p_level, CLIPMAP_MAX_LEVELS - 1));
}

float RendererVirtualShadowMapStorage::clipmap_page_world_size(uint32_t p_level) {
	return clipmap_level_extent(p_level) / float(PAGES_PER_SIDE);
}

Vector2i RendererVirtualShadowMapStorage::clipmap_level_origin(const Vector2 &p_center, uint32_t p_level) {
	const double page = double(clipmap_page_world_size(p_level));
	const int32_t center_x = int32_t(Math::floor(double(p_center.x) / page));
	const int32_t center_y = int32_t(Math::floor(double(p_center.y) / page));
	const int32_t half = int32_t(PAGES_PER_SIDE / 2);
	return Vector2i(center_x - half, center_y - half);
}

Vector2i RendererVirtualShadowMapStorage::clipmap_mip_origin(const Vector2i &p_origin, uint32_t p_mip) {
	return Vector2i(_floor_shift(p_origin.x, p_mip), _floor_shift(p_origin.y, p_mip));
}

RendererVirtualShadowMapStorage::ClipmapScroll RendererVirtualShadowMapStorage::clipmap_scroll(const Vector2i &p_old_origin, const Vector2i &p_new_origin) {
	ClipmapScroll scroll;
	for (uint32_t mip = 0; mip < MIP_COUNT; mip++) {
		const int32_t pages = int32_t(PAGES_PER_SIDE >> mip);
		const uint32_t mask = uint32_t(pages - 1);
		const Vector2i from = clipmap_mip_origin(p_old_origin, mip);
		const Vector2i to = clipmap_mip_origin(p_new_origin, mip);
		ScrollRange &range = scroll.mips[mip];
		for (int axis = 0; axis < 2; axis++) {
			const int64_t delta = int64_t(to[axis]) - int64_t(from[axis]);
			uint32_t start = 0;
			uint32_t count = 0;
			if (delta >= pages || delta <= -pages) {
				count = uint32_t(pages);
			} else if (delta > 0) {
				// Absolute pages [from + pages, to + pages) enter; they wrap onto the slots of [from, to).
				start = uint32_t(from[axis]) & mask;
				count = uint32_t(delta);
			} else if (delta < 0) {
				// Absolute pages [to, from) enter.
				start = uint32_t(to[axis]) & mask;
				count = uint32_t(-delta);
			}
			if (axis == 0) {
				range.col_start = start;
				range.col_count = count;
			} else {
				range.row_start = start;
				range.row_count = count;
			}
			scroll.any = scroll.any || count > 0;
		}
	}
	return scroll;
}

RendererVirtualShadowMapStorage::ClipmapScroll RendererVirtualShadowMapStorage::clipmap_scroll_all() {
	ClipmapScroll scroll;
	for (uint32_t mip = 0; mip < MIP_COUNT; mip++) {
		scroll.mips[mip].col_count = PAGES_PER_SIDE >> mip;
	}
	scroll.any = true;
	return scroll;
}

bool RendererVirtualShadowMapStorage::clipmap_scroll_hits(const ClipmapScroll &p_scroll, uint32_t p_mip, uint32_t p_x, uint32_t p_y) {
	ERR_FAIL_UNSIGNED_INDEX_V(p_mip, MIP_COUNT, false);
	const ScrollRange &range = p_scroll.mips[p_mip];
	const uint32_t mask = (PAGES_PER_SIDE >> p_mip) - 1;
	return ((p_x - range.col_start) & mask) < range.col_count || ((p_y - range.row_start) & mask) < range.row_count;
}

uint32_t RendererVirtualShadowMapStorage::clipmap_apply_scroll(uint32_t *r_map_entries, const ClipmapScroll &p_scroll) {
	ERR_FAIL_NULL_V(r_map_entries, 0);
	uint32_t marked = 0;
	for (uint32_t mip = 0; mip < MIP_COUNT; mip++) {
		const uint32_t pages = PAGES_PER_SIDE >> mip;
		uint32_t *entries = r_map_entries + mip_entry_offset(mip);
		for (uint32_t y = 0; y < pages; y++) {
			for (uint32_t x = 0; x < pages; x++) {
				uint32_t &entry = entries[y * pages + x];
				if ((entry & ENTRY_ALLOCATED) && clipmap_scroll_hits(p_scroll, mip, x, y)) {
					entry |= ENTRY_SCROLLED;
					marked++;
				}
			}
		}
	}
	return marked;
}

/* LOOKUP */

uint32_t RendererVirtualShadowMapStorage::entry_index(uint32_t p_map, uint32_t p_mip, uint32_t p_page_x, uint32_t p_page_y) {
	return p_map * ENTRIES_PER_MAP + mip_entry_offset(p_mip) + p_page_y * (PAGES_PER_SIDE >> p_mip) + p_page_x;
}

RendererVirtualShadowMapStorage::LookupResult RendererVirtualShadowMapStorage::lookup(const uint32_t *p_table, uint32_t p_table_maps, uint32_t p_map, const Vector2 &p_uv, uint32_t p_mip, bool p_fallback) {
	LookupResult result;
	ERR_FAIL_NULL_V(p_table, result);
	if (p_map >= p_table_maps) {
		// MAP_NONE included: a light without maps, or a map past the table, is a miss, as on the GPU.
		return result;
	}

	// Integer virtual texels first, so that every mip and both sides agree exactly.
	const float u = CLAMP(float(p_uv.x), 0.0f, 1.0f);
	const float v = CLAMP(float(p_uv.y), 0.0f, 1.0f);
	const uint32_t texel_x = MIN(uint32_t(u * float(VIRTUAL_SIZE)), VIRTUAL_SIZE - 1);
	const uint32_t texel_y = MIN(uint32_t(v * float(VIRTUAL_SIZE)), VIRTUAL_SIZE - 1);

	const uint32_t first_mip = MIN(p_mip, MIP_COUNT - 1);
	const uint32_t last_mip = p_fallback ? MIP_COUNT - 1 : first_mip;
	for (uint32_t mip = first_mip; mip <= last_mip; mip++) {
		const uint32_t mip_x = texel_x >> mip;
		const uint32_t mip_y = texel_y >> mip;
		const uint32_t entry = p_table[entry_index(p_map, mip, mip_x >> PAGE_SIZE_SHIFT, mip_y >> PAGE_SIZE_SHIFT)];
		if (entry & ENTRY_ALLOCATED) {
			result.texel_x = (entry_page_x(entry) << PAGE_SIZE_SHIFT) | (mip_x & (PAGE_SIZE - 1));
			result.texel_y = (entry_page_y(entry) << PAGE_SIZE_SHIFT) | (mip_y & (PAGE_SIZE - 1));
			result.mip = mip;
			result.entry = entry;
			result.valid = true;
			return result;
		}
	}
	return result;
}

RendererVirtualShadowMapStorage::LookupResult RendererVirtualShadowMapStorage::lookup_clipmap(const uint32_t *p_table, uint32_t p_table_maps, uint32_t p_map, const Vector2i &p_level_origin, const Vector2i &p_texel, uint32_t p_mip, bool p_fallback) {
	LookupResult result;
	ERR_FAIL_NULL_V(p_table, result);
	if (p_map >= p_table_maps) {
		return result;
	}
	// The texel's offset in the level's mip 0 window [origin, origin + PAGES_PER_SIDE) pages.
	const int64_t rel_x = int64_t(p_texel.x) - int64_t(p_level_origin.x) * int64_t(PAGE_SIZE);
	const int64_t rel_y = int64_t(p_texel.y) - int64_t(p_level_origin.y) * int64_t(PAGE_SIZE);
	if (rel_x < 0 || rel_y < 0 || rel_x >= int64_t(VIRTUAL_SIZE) || rel_y >= int64_t(VIRTUAL_SIZE)) {
		return result;
	}
	const uint32_t window_page_x = uint32_t(rel_x) >> PAGE_SIZE_SHIFT;
	const uint32_t window_page_y = uint32_t(rel_y) >> PAGE_SIZE_SHIFT;
	// The toroidal slot: the absolute texel modulo VIRTUAL_SIZE (two's complement, also below zero).
	const uint32_t texel_x = uint32_t(p_texel.x) & (VIRTUAL_SIZE - 1);
	const uint32_t texel_y = uint32_t(p_texel.y) & (VIRTUAL_SIZE - 1);

	const uint32_t first_mip = MIN(p_mip, MIP_COUNT - 1);
	const uint32_t last_mip = p_fallback ? MIP_COUNT - 1 : first_mip;
	for (uint32_t mip = first_mip; mip <= last_mip; mip++) {
		// At mip m the page is floor(origin / 2^m) + ((origin mod 2^m) + window page) >> m, inside the
		// mip's window while (origin mod 2^m) + window page < PAGES_PER_SIDE. Once outside, it stays
		// outside at every coarser mip.
		const uint32_t remainder_mask = (1u << mip) - 1;
		if ((uint32_t(p_level_origin.x) & remainder_mask) + window_page_x >= PAGES_PER_SIDE || (uint32_t(p_level_origin.y) & remainder_mask) + window_page_y >= PAGES_PER_SIDE) {
			break;
		}
		const uint32_t mip_x = texel_x >> mip;
		const uint32_t mip_y = texel_y >> mip;
		const uint32_t entry = p_table[entry_index(p_map, mip, mip_x >> PAGE_SIZE_SHIFT, mip_y >> PAGE_SIZE_SHIFT)];
		if (entry & ENTRY_ALLOCATED) {
			result.texel_x = (entry_page_x(entry) << PAGE_SIZE_SHIFT) | (mip_x & (PAGE_SIZE - 1));
			result.texel_y = (entry_page_y(entry) << PAGE_SIZE_SHIFT) | (mip_y & (PAGE_SIZE - 1));
			result.mip = mip;
			result.entry = entry;
			result.valid = true;
			return result;
		}
	}
	return result;
}

String RendererVirtualShadowMapStorage::get_glsl_defines() {
	String defines;
	defines += "\n#define VSM_PAGE_SIZE_SHIFT " + itos(PAGE_SIZE_SHIFT) + "u\n";
	defines += "#define VSM_PAGE_SIZE " + itos(PAGE_SIZE) + "u\n";
	defines += "#define VSM_VIRTUAL_SIZE " + itos(VIRTUAL_SIZE) + "u\n";
	defines += "#define VSM_PAGES_PER_SIDE " + itos(PAGES_PER_SIDE) + "u\n";
	defines += "#define VSM_MIP_COUNT " + itos(MIP_COUNT) + "u\n";
	defines += "#define VSM_ENTRIES_PER_MAP " + itos(ENTRIES_PER_MAP) + "u\n";
	defines += "#define VSM_ENTRY_PAGE_BITS " + itos(ENTRY_PAGE_BITS) + "u\n";
	defines += "#define VSM_ENTRY_PAGE_MASK " + itos(ENTRY_PAGE_MASK) + "u\n";
	defines += "#define VSM_ENTRY_ALLOCATED " + itos(ENTRY_ALLOCATED) + "u\n";
	defines += "#define VSM_ENTRY_REQUESTED " + itos(ENTRY_REQUESTED) + "u\n";
	defines += "#define VSM_ENTRY_DIRTY_STATIC " + itos(ENTRY_DIRTY_STATIC) + "u\n";
	defines += "#define VSM_ENTRY_DIRTY_DYNAMIC " + itos(ENTRY_DIRTY_DYNAMIC) + "u\n";
	defines += "#define VSM_ENTRY_COARSE " + itos(ENTRY_COARSE) + "u\n";
	defines += "#define VSM_ENTRY_UNCACHED " + itos(ENTRY_UNCACHED) + "u\n";
	defines += "#define VSM_ENTRY_SCROLLED " + itos(ENTRY_SCROLLED) + "u\n";
	defines += "#define VSM_ENTRY_OVERFLOW " + itos(ENTRY_OVERFLOW) + "u\n";
	defines += "#define VSM_MAP_NONE " + itos(MAP_NONE) + "u\n";
	defines += "#define VSM_LIGHT_FLAGS_KIND_MASK " + itos(LIGHT_FLAGS_KIND_MASK) + "u\n";
	defines += "#define VSM_LIGHT_FLAGS_COUNT_SHIFT " + itos(LIGHT_FLAGS_COUNT_SHIFT) + "u\n";
	defines += "#define VSM_LIGHT_FLAGS_COUNT_MASK " + itos(LIGHT_FLAGS_COUNT_MASK) + "u\n";
	defines += "#define VSM_MAP_KIND_DIRECTIONAL " + itos(MAP_KIND_DIRECTIONAL) + "u\n";
	defines += "#define VSM_MAP_KIND_SPOT " + itos(MAP_KIND_SPOT) + "u\n";
	defines += "#define VSM_MAP_KIND_OMNI " + itos(MAP_KIND_OMNI) + "u\n";
	defines += "#define VSM_MAP_KIND_AREA " + itos(MAP_KIND_AREA) + "u\n";
	return defines;
}

/* POOL */

bool RendererVirtualShadowMapStorage::pool_create(uint32_t p_page_count) {
	ERR_FAIL_COND_V_MSG(p_page_count == 0 || p_page_count > POOL_MAX_PAGES, false, vformat("A virtual shadow map pool holds 1 to %d pages.", POOL_MAX_PAGES));
	if (p_page_count == pool_pages) {
		return true;
	}
	if (!_pool_resize(p_page_count)) {
		// The hook keeps the previous pool on failure, so pool_pages still describes it.
		return false;
	}
	pool_pages = p_page_count;
	pool_reallocations++;
	return true;
}

void RendererVirtualShadowMapStorage::pool_free() {
	if (pool_pages == 0) {
		return;
	}
	_pool_resize(0);
	pool_pages = 0;
}

/* MAPS */

uint32_t RendererVirtualShadowMapStorage::_range_take(uint32_t p_count) {
	for (uint32_t i = 0; i < free_ranges.size(); i++) {
		FreeRange &range = free_ranges[i];
		if (range.count < p_count) {
			continue;
		}
		const uint32_t first = range.first;
		range.first += p_count;
		range.count -= p_count;
		if (range.count == 0) {
			free_ranges.remove_at(i);
		}
		return first;
	}
	return MAP_NONE;
}

void RendererVirtualShadowMapStorage::_range_give(uint32_t p_first, uint32_t p_count) {
	if (p_count == 0) {
		return;
	}
	uint32_t index = 0;
	while (index < free_ranges.size() && free_ranges[index].first < p_first) {
		index++;
	}
	DEV_ASSERT(index == 0 || free_ranges[index - 1].first + free_ranges[index - 1].count <= p_first);
	DEV_ASSERT(index == free_ranges.size() || p_first + p_count <= free_ranges[index].first);
	const bool joins_previous = index > 0 && free_ranges[index - 1].first + free_ranges[index - 1].count == p_first;
	const bool joins_next = index < free_ranges.size() && p_first + p_count == free_ranges[index].first;
	if (joins_previous && joins_next) {
		free_ranges[index - 1].count += p_count + free_ranges[index].count;
		free_ranges.remove_at(index);
	} else if (joins_previous) {
		free_ranges[index - 1].count += p_count;
	} else if (joins_next) {
		free_ranges[index].first = p_first;
		free_ranges[index].count += p_count;
	} else {
		FreeRange range;
		range.first = p_first;
		range.count = p_count;
		free_ranges.insert(index, range);
	}
}

bool RendererVirtualShadowMapStorage::_evict_least_recently_used() {
	const RID *oldest = nullptr;
	uint64_t oldest_frame = 0;
	for (const KeyValue<RID, LightMaps> &E : light_maps) {
		const uint64_t used = E.value.last_used_frame;
		if (used + MAP_EVICT_AFTER_FRAMES > frame) {
			continue; // Used in this frame or the one before: a view may still read its maps.
		}
		if (oldest == nullptr || used < oldest_frame) {
			oldest = &E.key;
			oldest_frame = used;
		}
	}
	if (oldest == nullptr) {
		return false;
	}
	const MapRange *range = map_get(*oldest);
	ERR_FAIL_NULL_V(range, false);
	evicted_lights++;
	evicted_maps += range->count;
	map_free(*oldest);
	return true;
}

uint32_t RendererVirtualShadowMapStorage::map_allocate(RID p_light_instance, MapKind p_kind, uint32_t p_levels) {
	ERR_FAIL_COND_V(p_light_instance.is_null(), MAP_NONE);
	ERR_FAIL_UNSIGNED_INDEX_V(uint32_t(p_kind), uint32_t(MAP_KIND_MAX), MAP_NONE);
	const uint32_t count = map_kind_map_count(p_kind, p_levels);

	LightMaps *existing = light_maps.getptr(p_light_instance);
	if (existing != nullptr) {
		if (existing->range.kind == p_kind && existing->range.count == count) {
			existing->last_used_frame = frame;
			return existing->range.first;
		}
		map_free(p_light_instance);
	}

	uint32_t first = _range_take(count);
	// Stale lights make room before the table grows, least recently used first. Freed maps need not
	// be contiguous with each other, so the request is retried after each one.
	while (first == MAP_NONE && _evict_least_recently_used()) {
		first = _range_take(count);
	}
	if (first == MAP_NONE) {
		// Grow the table so that the request fits at its end, joining a free range that ends there.
		uint32_t tail_free = 0;
		if (!free_ranges.is_empty()) {
			const FreeRange &last = free_ranges[free_ranges.size() - 1];
			if (last.first + last.count == table_capacity) {
				tail_free = last.count;
			}
		}
		const uint32_t needed = table_capacity + count - tail_free;
		uint32_t new_capacity = table_capacity;
		while (new_capacity < needed) {
			new_capacity += TABLE_GROWTH_MAPS;
		}
		if (new_capacity > TABLE_MAX_MAPS || !_table_resize(table_capacity, new_capacity)) {
			failed_allocations++;
			return MAP_NONE;
		}
		_range_give(table_capacity, new_capacity - table_capacity);
		table_capacity = new_capacity;
		first = _range_take(count);
		ERR_FAIL_COND_V(first == MAP_NONE, MAP_NONE);
	}

	LightMaps maps;
	maps.range.first = first;
	maps.range.count = count;
	maps.range.kind = p_kind;
	maps.last_used_frame = frame;
	light_maps.insert(p_light_instance, maps);
	allocated_maps += count;
	map_allocations++;
	return first;
}

void RendererVirtualShadowMapStorage::map_free(RID p_light_instance) {
	LightMaps *maps = light_maps.getptr(p_light_instance);
	if (maps == nullptr) {
		return;
	}
	const MapRange range = maps->range;
	light_maps.erase(p_light_instance);
	_table_clear(range.first, range.count);
	_range_give(range.first, range.count);
	allocated_maps -= range.count;
}

const RendererVirtualShadowMapStorage::MapRange *RendererVirtualShadowMapStorage::map_get(RID p_light_instance) const {
	const LightMaps *maps = light_maps.getptr(p_light_instance);
	return maps != nullptr ? &maps->range : nullptr;
}

const RendererVirtualShadowMapStorage::MapRange *RendererVirtualShadowMapStorage::map_touch(RID p_light_instance) {
	LightMaps *maps = light_maps.getptr(p_light_instance);
	if (maps == nullptr) {
		return nullptr;
	}
	maps->last_used_frame = frame;
	return &maps->range;
}

void RendererVirtualShadowMapStorage::get_free_range(uint32_t p_index, uint32_t &r_first, uint32_t &r_count) const {
	r_first = 0;
	r_count = 0;
	ERR_FAIL_UNSIGNED_INDEX(p_index, free_ranges.size());
	r_first = free_ranges[p_index].first;
	r_count = free_ranges[p_index].count;
}

uint32_t RendererVirtualShadowMapStorage::get_free_map_count() const {
	uint32_t count = 0;
	for (const FreeRange &range : free_ranges) {
		count += range.count;
	}
	return count;
}

uint32_t RendererVirtualShadowMapStorage::directional_update(RID p_light_instance, const Basis &p_light_basis, const Vector3 &p_camera_position, float p_max_distance) {
	const uint32_t levels = clipmap_level_count(p_max_distance);
	const uint32_t first = map_allocate(p_light_instance, MAP_KIND_DIRECTIONAL, levels);
	if (first == MAP_NONE) {
		return MAP_NONE;
	}
	LightMaps *maps = light_maps.getptr(p_light_instance);
	ERR_FAIL_NULL_V(maps, MAP_NONE);

	// The light shines along its -Z; its X and Y axes span the plane the clipmap levels lie in.
	const Vector3 axis_x = p_light_basis.get_column(0).normalized();
	const Vector3 axis_y = p_light_basis.get_column(1).normalized();
	const Vector2 center(axis_x.dot(p_camera_position), axis_y.dot(p_camera_position));
	const bool same_plane = maps->origins_valid && maps->axis_x == axis_x && maps->axis_y == axis_y;

	for (uint32_t level = 0; level < levels; level++) {
		const Vector2i origin = clipmap_level_origin(center, level);
		if (maps->origins_valid) {
			const ClipmapScroll scroll = same_plane ? clipmap_scroll(maps->origins[level], origin) : clipmap_scroll_all();
			if (scroll.any) {
				_table_scroll(first + level, scroll);
				scroll_passes++;
			}
		}
		maps->origins[level] = origin;
	}
	maps->axis_x = axis_x;
	maps->axis_y = axis_y;
	maps->origins_valid = true;
	return first;
}

Vector2i RendererVirtualShadowMapStorage::get_clipmap_origin(RID p_light_instance, uint32_t p_level) const {
	const LightMaps *maps = light_maps.getptr(p_light_instance);
	ERR_FAIL_NULL_V(maps, Vector2i());
	ERR_FAIL_COND_V(maps->range.kind != MAP_KIND_DIRECTIONAL, Vector2i());
	ERR_FAIL_UNSIGNED_INDEX_V(p_level, maps->range.count, Vector2i());
	return maps->origins[p_level];
}

void RendererVirtualShadowMapStorage::free_all() {
	while (!light_maps.is_empty()) {
		map_free(light_maps.begin()->key);
	}
	pool_free();
}
