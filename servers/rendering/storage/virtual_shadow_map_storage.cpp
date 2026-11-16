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
#include "core/math/projection.h"
#include "core/variant/variant.h"

#include <cmath>
#include <cstring>

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
	// GodotNext: vsm_page_marking (#219).
	defines += "#define VSM_MAP_KIND_NONE " + itos(MAP_KIND_MAX) + "u\n";
	defines += "#define VSM_ENTRY_DIRTY_MASK " + itos(ENTRY_DIRTY_MASK) + "u\n";
	defines += "#define VSM_ENTRY_FLAGS_MASK " + itos(ENTRY_FLAGS_MASK) + "u\n";
	defines += "#define VSM_COARSE_FIRST_MIP " + itos(COARSE_FIRST_MIP) + "u\n";
	defines += "#define VSM_OVERFLOW_MAX_BIAS " + itos(OVERFLOW_MAX_BIAS) + "u\n";
	defines += "#define VSM_OVERFLOW_DECAY_FRAMES " + itos(OVERFLOW_DECAY_FRAMES) + "u\n";
	defines += "#define VSM_OVERFLOW_BIAS_RAISED " + itos(OVERFLOW_BIAS_RAISED) + "u\n";
	defines += "#define VSM_BUCKET_COUNT " + itos(BUCKET_COUNT) + "u\n";
	defines += "#define VSM_BUCKET_NONE " + itos(BUCKET_NONE) + "u\n";
	defines += "#define VSM_MAX_DIRECTIONAL_LIGHTS " + itos(MAX_DIRECTIONAL_LIGHTS) + "u\n";
	defines += "#define VSM_CLIPMAP_TEXEL0_EXPONENT (" + itos(CLIPMAP_TEXEL0_EXPONENT) + ")\n";
	defines += "#define VSM_CLIPMAP_RESIDENT_MIP " + itos(CLIPMAP_RESIDENT_MIP) + "u\n";
	const char *state_names[] = { "REQUESTS", "REQUESTS_DROPPED", "AVAILABLE", "CURSOR", "OVERFLOW", "NEW", "KEPT", "EVICTED", "ORPHANED", "STOLEN", "DIRTY", "DIRTY_DIRECTIONAL", "COARSE", "CACHED", "MAX_MAP_REQUESTS", "BIASED_MAPS", "RESIDENT", "RESIDENT_OVERFLOW", "SINK" };
	static_assert(sizeof(state_names) / sizeof(state_names[0]) == STATE_SINK + 1, "One name per per-view counter.");
	for (uint32_t i = 0; i < sizeof(state_names) / sizeof(state_names[0]); i++) {
		defines += "#define VSM_STATE_" + String(state_names[i]) + " " + itos(i) + "u\n";
	}
	defines += "#define VSM_STATE_TOTAL_OVERFLOW " + itos(STATE_TOTAL_OVERFLOW) + "u\n";
	defines += "#define VSM_STATE_OVERFLOW_VIEWS " + itos(STATE_OVERFLOW_VIEWS) + "u\n";
	defines += "#define VSM_STATE_TOTAL_DROPPED " + itos(STATE_TOTAL_DROPPED) + "u\n";
	defines += "#define VSM_STATE_VIEWS " + itos(STATE_VIEWS) + "u\n";
	defines += "#define VSM_STATE_BIAS_FRAME " + itos(STATE_BIAS_FRAME) + "u\n";
	defines += "#define VSM_STATE_TOTAL_RESIDENT_OVERFLOW " + itos(STATE_TOTAL_RESIDENT_OVERFLOW) + "u\n";
	defines += "#define VSM_STATE_BUCKETS " + itos(STATE_BUCKETS) + "u\n";
	defines += "#define VSM_STATE_BUCKET_OFFSETS " + itos(STATE_BUCKET_OFFSETS) + "u\n";
	defines += "#define VSM_STATE_BUCKET_CURSORS " + itos(STATE_BUCKET_CURSORS) + "u\n";
	const char *map_state_names[] = { "REQUESTS", "DIRTY_COUNT", "DIRTY_OFFSET", "DIRTY_CURSOR", "BIAS" };
	static_assert(sizeof(map_state_names) / sizeof(map_state_names[0]) == MAP_STATE_REGIONS, "One name per map state region.");
	for (uint32_t i = 0; i < MAP_STATE_REGIONS; i++) {
		defines += "#define VSM_MAP_STATE_" + String(map_state_names[i]) + " " + itos(i) + "u\n";
	}
	defines += "#define VSM_INDIRECT_DIRECTIONAL " + itos(INDIRECT_DIRECTIONAL) + "u\n";
	defines += "#define VSM_INDIRECT_LOCAL " + itos(INDIRECT_LOCAL) + "u\n";
	defines += "#define VSM_INDIRECT_ALL " + itos(INDIRECT_ALL) + "u\n";
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

/* PAGE MARKING AND ALLOCATION (#219) */

// GodotNext: vsm_page_marking (#219, plan §21.3 contract S3). CPU models of vsm_mark_inc.glsl,
// vsm_mark_pages.glsl and vsm_allocate_pages.glsl, written in the shaders' operation order.

static_assert(sizeof(RendererVirtualShadowMapStorage::MapData) == 96, "MapData must match VSMMapData in vsm_mark_inc.glsl.");
static_assert(RendererVirtualShadowMapStorage::CLIPMAP_LEVEL0_EXTENT / float(RendererVirtualShadowMapStorage::VIRTUAL_SIZE) == 1.0f / 1024.0f, "A clipmap level 0 texel is 2^CLIPMAP_TEXEL0_EXPONENT meters.");
static_assert(RendererVirtualShadowMapStorage::CLIPMAP_TEXEL0_EXPONENT == -10, "A clipmap level 0 texel is 2^CLIPMAP_TEXEL0_EXPONENT meters.");
static_assert(RendererVirtualShadowMapStorage::cache_frames(RendererVirtualShadowMapStorage::POOL_QUALITY_EPIC) <= RendererVirtualShadowMapStorage::MAX_CACHE_FRAMES, "Every quality's cache frames fit the buckets.");
static_assert(RendererVirtualShadowMapStorage::MAX_CACHE_FRAMES < RendererVirtualShadowMapStorage::BUCKET_COUNT, "Bucket 0 and one bucket per cached age.");
static_assert(RendererVirtualShadowMapStorage::STATE_SINK < RendererVirtualShadowMapStorage::STATE_VIEW_END, "Per-view counters fit their region.");
static_assert(RendererVirtualShadowMapStorage::STATE_TOTAL_RESIDENT_OVERFLOW < RendererVirtualShadowMapStorage::STATE_BUCKETS, "Totals fit their region.");
// A level's window lies within pages [16, 48] of the next level's; the next level's window at the
// resident mip holds pages [0, 64 - (origin mod 2^mip)), so 2^mip - 1 must stay below 16.
static_assert(RendererVirtualShadowMapStorage::CLIPMAP_RESIDENT_MIP < RendererVirtualShadowMapStorage::MIP_COUNT && (1u << RendererVirtualShadowMapStorage::CLIPMAP_RESIDENT_MIP) <= RendererVirtualShadowMapStorage::PAGES_PER_SIDE / 4, "The next level's resident mips always hold the whole finer level.");
static_assert(RendererVirtualShadowMapStorage::OVERFLOW_DECAY_FRAMES < 256 && RendererVirtualShadowMapStorage::OVERFLOW_MAX_BIAS < 256, "The bias state packs both in 8 bits.");
static_assert(RendererVirtualShadowMapStorage::OVERFLOW_BIAS_RAISED == (1u << 16), "The raised bit follows the bias and the decay.");

bool RendererVirtualShadowMapStorage::MapData::operator==(const MapData &p_other) const {
	return memcmp(this, &p_other, sizeof(MapData)) == 0;
}

RendererVirtualShadowMapStorage::MapData RendererVirtualShadowMapStorage::map_data_clipmap(const Basis &p_light_basis, uint32_t p_level, const Vector2i &p_origin, float p_max_distance) {
	MapData data;
	// The axes directional_update() centers the levels with.
	const Vector3 axis_x = p_light_basis.get_column(0).normalized();
	const Vector3 axis_y = p_light_basis.get_column(1).normalized();
	const Vector3 axis_z = p_light_basis.get_column(2).normalized();
	// VIRTUAL_SIZE / the level's extent, a power of two: rows of different levels differ by exact
	// powers of two, so a level's texel is level 0's divided by 2^level with no rounding.
	const float scale = std::ldexp(1.0f, -CLIPMAP_TEXEL0_EXPONENT - int32_t(MIN(p_level, CLIPMAP_MAX_LEVELS - 1)));
	const Vector3 axes[3] = { axis_x * scale, axis_y * scale, -axis_z };
	for (int row = 0; row < 3; row++) {
		data.rows[row][0] = float(axes[row].x);
		data.rows[row][1] = float(axes[row].y);
		data.rows[row][2] = float(axes[row].z);
		data.rows[row][3] = 0.0f;
	}
	data.rows[3][3] = 1.0f;
	data.origin[0] = p_origin.x;
	data.origin[1] = p_origin.y;
	data.kind = MAP_KIND_DIRECTIONAL;
	data.texel_scale = 1.0f / scale;
	data.max_distance = p_max_distance;
	data.index = p_level;
	return data;
}

Transform3D RendererVirtualShadowMapStorage::face_camera(MapKind p_kind, uint32_t p_face, const Transform3D &p_light_transform) {
	// Directions and up vectors in the light's space: +X, -X, +Y, -Y, +Z, -Z.
	static const Vector3 forward[6] = { Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 1, 0), Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(0, 0, -1) };
	static const Vector3 up[6] = { Vector3(0, 1, 0), Vector3(0, 1, 0), Vector3(0, 0, -1), Vector3(0, 0, 1), Vector3(0, 1, 0), Vector3(0, 1, 0) };
	static const uint32_t area_faces[5] = { 0, 1, 2, 3, 5 };
	uint32_t direction = 5; // Spot: the light's -Z.
	if (p_kind == MAP_KIND_OMNI) {
		direction = MIN(p_face, 5u);
	} else if (p_kind == MAP_KIND_AREA) {
		direction = area_faces[MIN(p_face, 4u)];
	}
	const Basis local = Basis::looking_at(forward[direction], up[direction]);
	return Transform3D(p_light_transform.basis.orthonormalized() * local, p_light_transform.origin);
}

float RendererVirtualShadowMapStorage::face_fov_degrees(MapKind p_kind, float p_spot_angle) {
	if (p_kind == MAP_KIND_SPOT) {
		// The map's square frustum holds the cone. Cones of 90 degrees or more are clamped.
		return CLAMP(2.0f * p_spot_angle, 1.0f, 179.0f);
	}
	return 90.0f;
}

RendererVirtualShadowMapStorage::MapData RendererVirtualShadowMapStorage::map_data_face(MapKind p_kind, uint32_t p_face, const Transform3D &p_light_transform, float p_range, float p_spot_angle) {
	MapData data;
	const Transform3D camera = face_camera(p_kind, p_face, p_light_transform);
	const float fov = face_fov_degrees(p_kind, p_spot_angle);
	const float range = MAX(p_range, 0.001f);
	const float z_near = CLAMP(range * FACE_NEAR_FRACTION, FACE_NEAR_MIN, MIN(FACE_NEAR_MAX, range * 0.5f));
	Projection projection;
	projection.set_perspective(fov, 1.0, z_near, range, false);
	const Projection clip = projection * Projection(camera.affine_inverse());
	for (int row = 0; row < 4; row++) {
		for (int column = 0; column < 4; column++) {
			data.rows[row][column] = float(clip.columns[column][row]);
		}
	}
	data.kind = p_kind;
	data.texel_scale = float(2.0 * Math::tan(Math::deg_to_rad(double(fov)) * 0.5) / double(VIRTUAL_SIZE));
	data.near_w = z_near;
	data.max_distance = range;
	data.index = p_face;
	return data;
}

float RendererVirtualShadowMapStorage::row_dot(const float p_row[4], const Vector3 &p_point) {
	const float x = float(p_point.x);
	const float y = float(p_point.y);
	const float z = float(p_point.z);
	float sum = p_row[0] * x;
	sum = sum + p_row[1] * y;
	sum = sum + p_row[2] * z;
	return sum + p_row[3];
}

int32_t RendererVirtualShadowMapStorage::float_exponent(float p_value) {
	uint32_t bits = 0;
	memcpy(&bits, &p_value, sizeof(uint32_t));
	const int32_t exponent = int32_t((bits >> 23) & 0xFFu);
	return exponent == 0 ? -127 : exponent - 127;
}

uint32_t RendererVirtualShadowMapStorage::face_mip(float p_texel0, float p_footprint) {
	uint32_t mip = 0;
	while (mip + 1 < MIP_COUNT && p_texel0 * float(1u << (mip + 1)) <= p_footprint) {
		mip++;
	}
	return mip;
}

uint32_t RendererVirtualShadowMapStorage::biased_mip(int32_t p_mip, int32_t p_bias) {
	return uint32_t(CLAMP(p_mip + p_bias, 0, int32_t(MIP_COUNT) - 1));
}

bool RendererVirtualShadowMapStorage::face_texel(const MapData &p_map, const Vector3 &p_world, float &r_w, uint32_t &r_texel_x, uint32_t &r_texel_y) {
	const float x = row_dot(p_map.rows[0], p_world);
	const float y = row_dot(p_map.rows[1], p_world);
	const float w = row_dot(p_map.rows[3], p_world);
	r_w = w;
	r_texel_x = 0;
	r_texel_y = 0;
	if (!(w > p_map.near_w) || std::fabs(x) > w || std::fabs(y) > w) {
		return false;
	}
	const float u = x / w * 0.5f + 0.5f;
	const float v = y / w * 0.5f + 0.5f;
	r_texel_x = MIN(uint32_t(CLAMP(u, 0.0f, 1.0f) * float(VIRTUAL_SIZE)), VIRTUAL_SIZE - 1);
	r_texel_y = MIN(uint32_t(CLAMP(v, 0.0f, 1.0f) * float(VIRTUAL_SIZE)), VIRTUAL_SIZE - 1);
	return true;
}

Vector2i RendererVirtualShadowMapStorage::clipmap_texel0(const MapData &p_level0, const Vector3 &p_world) {
	// Clamped to +-2^30 texels (1,048 km at level 0), so the integers and the shifts stay in range.
	const float limit = 1073741824.0f;
	const float x = CLAMP(std::floor(row_dot(p_level0.rows[0], p_world)), -limit, limit);
	const float y = CLAMP(std::floor(row_dot(p_level0.rows[1], p_world)), -limit, limit);
	return Vector2i(int32_t(x), int32_t(y));
}

bool RendererVirtualShadowMapStorage::clipmap_in_window(const Vector2i &p_origin, const Vector2i &p_level_texel, uint32_t p_mip) {
	for (int axis = 0; axis < 2; axis++) {
		const int64_t rel = int64_t(p_level_texel[axis]) - int64_t(p_origin[axis]) * int64_t(PAGE_SIZE);
		if (rel < 0 || rel >= int64_t(VIRTUAL_SIZE)) {
			return false;
		}
		const uint32_t offset = (uint32_t(p_origin[axis]) & ((1u << p_mip) - 1u)) + (uint32_t(rel) >> PAGE_SIZE_SHIFT);
		if (offset >= PAGES_PER_SIDE) {
			return false;
		}
	}
	return true;
}

Vector2i RendererVirtualShadowMapStorage::clipmap_page(const Vector2i &p_level_texel, uint32_t p_mip) {
	const uint32_t x = uint32_t(p_level_texel.x) & (VIRTUAL_SIZE - 1);
	const uint32_t y = uint32_t(p_level_texel.y) & (VIRTUAL_SIZE - 1);
	return Vector2i(int32_t((x >> p_mip) >> PAGE_SIZE_SHIFT), int32_t((y >> p_mip) >> PAGE_SIZE_SHIFT));
}

int32_t RendererVirtualShadowMapStorage::clipmap_level_mip(float p_footprint, int32_t p_bias) {
	return float_exponent(p_footprint) - CLIPMAP_TEXEL0_EXPONENT + p_bias;
}

Vector2i RendererVirtualShadowMapStorage::clipmap_level_texel(const Vector2i &p_texel0, uint32_t p_level) {
	return Vector2i(_floor_shift(p_texel0.x, p_level), _floor_shift(p_texel0.y, p_level));
}

bool RendererVirtualShadowMapStorage::light_reaches(const MarkLight &p_light, const Vector3 &p_rel) {
	const float rx = float(p_rel.x);
	const float ry = float(p_rel.y);
	const float rz = float(p_rel.z);
	float d2 = rx * rx;
	d2 = d2 + ry * ry;
	d2 = d2 + rz * rz;
	if (!((d2 * p_light.inv_radius) * p_light.inv_radius < 1.0f)) {
		return false;
	}
	if (p_light.kind == MAP_KIND_OMNI) {
		return true;
	}
	float c = rx * float(p_light.direction.x);
	c = c + ry * float(p_light.direction.y);
	c = c + rz * float(p_light.direction.z);
	if (p_light.kind == MAP_KIND_AREA) {
		return c > 0.0f; // The front hemisphere.
	}
	// Spot: the angle to the axis is below the spot angle: c / |rel| > cos(angle).
	const float limit = (p_light.cos_angle * p_light.cos_angle) * d2;
	if (p_light.cos_angle >= 0.0f) {
		return c > 0.0f && c * c > limit;
	}
	return c > 0.0f || c * c < limit;
}

bool RendererVirtualShadowMapStorage::light_reaches_cell(const MarkLight &p_light, const Vector3 &p_rel, float p_cell_size, bool &r_all_faces) {
	const float rx = float(p_rel.x);
	const float ry = float(p_rel.y);
	const float rz = float(p_rel.z);
	float d2 = rx * rx;
	d2 = d2 + ry * ry;
	d2 = d2 + rz * rz;
	r_all_faces = d2 < p_cell_size * p_cell_size;
	// |rel| < range + cell size, without a division: |rel| / range < 1 + cell size / range.
	const float reach = 1.0f + p_cell_size * p_light.inv_radius;
	return (d2 * p_light.inv_radius) * p_light.inv_radius < reach * reach;
}

// Records one sample of SampleCheck: the request's entry p_index and what its lookup found.
static void _sample_record(RendererVirtualShadowMapStorage::SampleCheck *r_sample, uint32_t p_index, const RendererVirtualShadowMapStorage::LookupResult &p_result) {
	r_sample->sampled++;
	if (!p_result.valid) {
		r_sample->unmapped++;
	} else if (!(r_sample->table[p_index] & RendererVirtualShadowMapStorage::ENTRY_ALLOCATED)) {
		r_sample->fallback++;
	}
}

// Whether SampleCheck looks up the request p_index (the GPU made it too).
static bool _sample_confirmed(RendererVirtualShadowMapStorage::SampleCheck *r_sample, uint32_t p_index) {
	if (r_sample->requested != nullptr && !(r_sample->requested[p_index] & RendererVirtualShadowMapStorage::ENTRY_REQUESTED)) {
		r_sample->unconfirmed++;
		return false;
	}
	return true;
}

RendererVirtualShadowMapStorage::LookupResult RendererVirtualShadowMapStorage::clipmap_sample(const uint32_t *p_table, uint32_t p_table_maps, const MapData *p_maps, uint32_t p_first, uint32_t p_count, const Vector2i &p_texel0, uint32_t p_level, uint32_t p_mip) {
	for (uint32_t level = p_level; level < p_count; level++) {
		const uint32_t map = p_first + level;
		if (map >= p_table_maps) {
			break;
		}
		const uint32_t mip = uint32_t(MAX(int32_t(p_mip) - int32_t(level - p_level), 0));
		const Vector2i origin(p_maps[map].origin[0], p_maps[map].origin[1]);
		const LookupResult result = lookup_clipmap(p_table, p_table_maps, map, origin, clipmap_level_texel(p_texel0, level), mip, true);
		if (result.valid) {
			return result;
		}
	}
	return LookupResult();
}

// The entry clipmap_coarse_sample() tries first for p_texel0, or UINT32_MAX outside the clipmap.
static uint32_t _clipmap_coarse_entry(const RendererVirtualShadowMapStorage::MapData *p_maps, uint32_t p_table_maps, uint32_t p_first, uint32_t p_count, const Vector2i &p_texel0) {
	using VSM = RendererVirtualShadowMapStorage;
	for (uint32_t level = 0; level < p_count; level++) {
		const uint32_t map = p_first + level;
		if (map >= p_table_maps) {
			break;
		}
		const Vector2i origin(p_maps[map].origin[0], p_maps[map].origin[1]);
		const Vector2i level_texel = VSM::clipmap_level_texel(p_texel0, level);
		if (VSM::clipmap_in_window(origin, level_texel, VSM::CLIPMAP_RESIDENT_MIP)) {
			const Vector2i page = VSM::clipmap_page(level_texel, VSM::CLIPMAP_RESIDENT_MIP);
			return VSM::entry_index(map, VSM::CLIPMAP_RESIDENT_MIP, uint32_t(page.x), uint32_t(page.y));
		}
	}
	if (p_count > 0 && p_first + p_count - 1 < p_table_maps) {
		const uint32_t last = p_count - 1;
		const uint32_t map = p_first + last;
		const Vector2i origin(p_maps[map].origin[0], p_maps[map].origin[1]);
		const Vector2i level_texel = VSM::clipmap_level_texel(p_texel0, last);
		if (VSM::clipmap_in_window(origin, level_texel, 0)) {
			uint32_t resident = 0;
			while (resident + 1 < VSM::CLIPMAP_RESIDENT_MIP && VSM::clipmap_in_window(origin, level_texel, resident + 1)) {
				resident++;
			}
			const Vector2i page = VSM::clipmap_page(level_texel, resident);
			return VSM::entry_index(map, resident, uint32_t(page.x), uint32_t(page.y));
		}
	}
	return UINT32_MAX;
}

RendererVirtualShadowMapStorage::LookupResult RendererVirtualShadowMapStorage::clipmap_coarse_sample(const uint32_t *p_table, uint32_t p_table_maps, const MapData *p_maps, uint32_t p_first, uint32_t p_count, const Vector2i &p_texel0) {
	for (uint32_t level = 0; level < p_count; level++) {
		const uint32_t map = p_first + level;
		if (map >= p_table_maps) {
			break;
		}
		const Vector2i origin(p_maps[map].origin[0], p_maps[map].origin[1]);
		const LookupResult result = lookup_clipmap(p_table, p_table_maps, map, origin, clipmap_level_texel(p_texel0, level), CLIPMAP_RESIDENT_MIP, true);
		if (result.valid) {
			return result;
		}
	}
	if (p_count > 0 && p_first + p_count - 1 < p_table_maps) {
		const uint32_t last = p_count - 1;
		const uint32_t map = p_first + last;
		const Vector2i origin(p_maps[map].origin[0], p_maps[map].origin[1]);
		const Vector2i level_texel = clipmap_level_texel(p_texel0, last);
		if (clipmap_in_window(origin, level_texel, 0) && !clipmap_in_window(origin, level_texel, CLIPMAP_RESIDENT_MIP)) {
			uint32_t resident = 0;
			while (resident + 1 < CLIPMAP_RESIDENT_MIP && clipmap_in_window(origin, level_texel, resident + 1)) {
				resident++;
			}
			return lookup_clipmap(p_table, p_table_maps, map, origin, level_texel, resident, false);
		}
	}
	return LookupResult();
}

void RendererVirtualShadowMapStorage::mark_resident(const MapData *p_maps, uint32_t p_table_maps, uint32_t p_map_base, uint32_t p_light_flags, LocalVector<uint64_t> &r_requests) {
	const uint32_t count = (p_light_flags >> LIGHT_FLAGS_COUNT_SHIFT) & LIGHT_FLAGS_COUNT_MASK;
	if (MapKind(p_light_flags & LIGHT_FLAGS_KIND_MASK) == MAP_KIND_DIRECTIONAL) {
		const uint32_t first = light_map(p_map_base, p_light_flags, 0);
		if (first >= p_table_maps || p_maps[first].kind != MAP_KIND_DIRECTIONAL) {
			return;
		}
		for (uint32_t level = 0; level < count; level++) {
			const uint32_t map = first + level;
			if (map >= p_table_maps) {
				break;
			}
			for (uint32_t mip = CLIPMAP_RESIDENT_MIP; mip < MIP_COUNT; mip++) {
				const uint32_t pages = PAGES_PER_SIDE >> mip;
				for (uint32_t y = 0; y < pages; y++) {
					for (uint32_t x = 0; x < pages; x++) {
						r_requests.push_back(uint64_t(entry_index(map, mip, x, y)) | REQUEST_COARSE);
					}
				}
			}
		}
		return;
	}
	for (uint32_t face = 0; face < count; face++) {
		const uint32_t map = light_map(p_map_base, p_light_flags, face);
		if (map >= p_table_maps || p_maps[map].kind == MAP_KIND_MAX || p_maps[map].kind == MAP_KIND_DIRECTIONAL) {
			continue;
		}
		r_requests.push_back(uint64_t(entry_index(map, MIP_COUNT - 1, 0, 0)) | REQUEST_COARSE);
	}
}

void RendererVirtualShadowMapStorage::mark_local_point(const MapData *p_maps, uint32_t p_table_maps, const uint32_t *p_map_bias, const MarkLight &p_light, const Vector3 &p_world, float p_footprint, int32_t p_lod_bias, bool p_coarse, bool p_all_faces, LocalVector<uint64_t> &r_requests, SampleCheck *r_sample) {
	const uint32_t count = (p_light.flags >> LIGHT_FLAGS_COUNT_SHIFT) & LIGHT_FLAGS_COUNT_MASK;
	for (uint32_t face = 0; face < count; face++) {
		const uint32_t map = light_map(p_light.map_base, p_light.flags, face);
		if (map >= p_table_maps) {
			continue;
		}
		const MapData &data = p_maps[map];
		if (data.kind == MAP_KIND_MAX || data.kind == MAP_KIND_DIRECTIONAL) {
			continue;
		}
		float w = 0.0f;
		uint32_t texel_x = 0;
		uint32_t texel_y = 0;
		const bool inside = face_texel(data, p_world, w, texel_x, texel_y);
		if (p_coarse) {
			if (!inside && !p_all_faces) {
				continue;
			}
			for (uint32_t mip = COARSE_FIRST_MIP; mip < MIP_COUNT; mip++) {
				const uint32_t pages = PAGES_PER_SIDE >> mip;
				if (inside) {
					r_requests.push_back(uint64_t(entry_index(map, mip, (texel_x >> mip) >> PAGE_SIZE_SHIFT, (texel_y >> mip) >> PAGE_SIZE_SHIFT)) | REQUEST_COARSE);
				} else {
					for (uint32_t y = 0; y < pages; y++) {
						for (uint32_t x = 0; x < pages; x++) {
							r_requests.push_back(uint64_t(entry_index(map, mip, x, y)) | REQUEST_COARSE);
						}
					}
				}
			}
			if (inside && r_sample != nullptr && r_sample->table != nullptr) {
				const uint32_t coarse_index = entry_index(map, COARSE_FIRST_MIP, (texel_x >> COARSE_FIRST_MIP) >> PAGE_SIZE_SHIFT, (texel_y >> COARSE_FIRST_MIP) >> PAGE_SIZE_SHIFT);
				if (_sample_confirmed(r_sample, coarse_index)) {
					const Vector2 uv((float(texel_x) + 0.5f) / float(VIRTUAL_SIZE), (float(texel_y) + 0.5f) / float(VIRTUAL_SIZE));
					_sample_record(r_sample, coarse_index, lookup(r_sample->table, p_table_maps, map, uv, COARSE_FIRST_MIP, true));
				}
			}
			continue;
		}
		if (!inside) {
			continue;
		}
		const int32_t bias = p_lod_bias + (p_map_bias != nullptr ? int32_t(overflow_bias(p_map_bias[map])) : 0);
		const uint32_t mip = biased_mip(int32_t(face_mip(w * data.texel_scale, p_footprint)), bias);
		const uint32_t index = entry_index(map, mip, (texel_x >> mip) >> PAGE_SIZE_SHIFT, (texel_y >> mip) >> PAGE_SIZE_SHIFT);
		r_requests.push_back(uint64_t(index));
		if (r_sample != nullptr && r_sample->table != nullptr && _sample_confirmed(r_sample, index)) {
			// The texel's center: lookup() lands on the same texel.
			const Vector2 uv((float(texel_x) + 0.5f) / float(VIRTUAL_SIZE), (float(texel_y) + 0.5f) / float(VIRTUAL_SIZE));
			_sample_record(r_sample, index, lookup(r_sample->table, p_table_maps, map, uv, mip, true));
		}
	}
}

void RendererVirtualShadowMapStorage::mark_directional_point(const MapData *p_maps, uint32_t p_table_maps, const uint32_t *p_map_bias, uint32_t p_map_base, uint32_t p_light_flags, const Vector3 &p_world, float p_footprint, int32_t p_lod_bias, bool p_coarse, LocalVector<uint64_t> &r_requests, SampleCheck *r_sample) {
	const uint32_t count = (p_light_flags >> LIGHT_FLAGS_COUNT_SHIFT) & LIGHT_FLAGS_COUNT_MASK;
	const uint32_t first = light_map(p_map_base, p_light_flags, 0);
	if (first >= p_table_maps || p_maps[first].kind != MAP_KIND_DIRECTIONAL) {
		return;
	}
	const Vector2i texel0 = clipmap_texel0(p_maps[first], p_world);
	if (p_coarse) {
		// Each coarse mip (down to CLIPMAP_RESIDENT_MIP) in the finest level whose window holds the point
		// at that mip, plus the coarsest in-window mip (< CLIPMAP_RESIDENT_MIP) in the last level's far strip.
		for (uint32_t mip = CLIPMAP_RESIDENT_MIP; mip < MIP_COUNT; mip++) {
			for (uint32_t level = 0; level < count; level++) {
				const uint32_t map = first + level;
				if (map >= p_table_maps) {
					break;
				}
				const Vector2i origin(p_maps[map].origin[0], p_maps[map].origin[1]);
				const Vector2i level_texel = clipmap_level_texel(texel0, level);
				if (clipmap_in_window(origin, level_texel, mip)) {
					const Vector2i page = clipmap_page(level_texel, mip);
					r_requests.push_back(uint64_t(entry_index(map, mip, uint32_t(page.x), uint32_t(page.y))) | REQUEST_COARSE);
					break;
				}
			}
		}
		if (count > 0 && first + count - 1 < p_table_maps) {
			const uint32_t last = count - 1;
			const uint32_t last_map = first + last;
			const Vector2i origin(p_maps[last_map].origin[0], p_maps[last_map].origin[1]);
			const Vector2i level_texel = clipmap_level_texel(texel0, last);
			if (clipmap_in_window(origin, level_texel, 0) && !clipmap_in_window(origin, level_texel, CLIPMAP_RESIDENT_MIP)) {
				uint32_t resident = 0;
				while (resident + 1 < CLIPMAP_RESIDENT_MIP && clipmap_in_window(origin, level_texel, resident + 1)) {
					resident++;
				}
				const Vector2i page = clipmap_page(level_texel, resident);
				r_requests.push_back(uint64_t(entry_index(last_map, resident, uint32_t(page.x), uint32_t(page.y))) | REQUEST_COARSE);
			}
		}
		if (r_sample != nullptr && r_sample->table != nullptr) {
			const uint32_t coarse_index = _clipmap_coarse_entry(p_maps, p_table_maps, first, count, texel0);
			if (coarse_index != UINT32_MAX && _sample_confirmed(r_sample, coarse_index)) {
				_sample_record(r_sample, coarse_index, clipmap_coarse_sample(r_sample->table, p_table_maps, p_maps, first, count, texel0));
			}
		}
		return;
	}
	// The finest level that holds the point; level + mip as the footprint asks, then the map's overflow
	// bias. A page outside that mip's window goes to the next level.
	const int32_t level_mip = clipmap_level_mip(p_footprint, p_lod_bias);
	for (uint32_t level = 0; level < count; level++) {
		const uint32_t map = first + level;
		if (map >= p_table_maps) {
			break;
		}
		const Vector2i origin(p_maps[map].origin[0], p_maps[map].origin[1]);
		const Vector2i level_texel = clipmap_level_texel(texel0, level);
		if (!clipmap_in_window(origin, level_texel, 0)) {
			continue;
		}
		uint32_t mip = biased_mip(level_mip - int32_t(level), 0);
		if (p_map_bias != nullptr) {
			mip = MIN(mip + overflow_bias(p_map_bias[map]), MIP_COUNT - 1);
		}
		if (level + 1 == count) {
			while (mip > 0 && !clipmap_in_window(origin, level_texel, mip)) {
				mip--;
			}
		} else if (!clipmap_in_window(origin, level_texel, mip)) {
			continue;
		}
		const Vector2i page = clipmap_page(level_texel, mip);
		const uint32_t index = entry_index(map, mip, uint32_t(page.x), uint32_t(page.y));
		// The last level's far edge: outside its resident mips' window, and no coarser level holds it.
		// The page at the coarsest mip whose window holds the point is requested ahead of the fine
		// requests, so the sample always finds a page.
		const bool far_resident = level + 1 == count && !clipmap_in_window(origin, level_texel, CLIPMAP_RESIDENT_MIP);
		uint32_t resident = mip;
		if (far_resident) {
			while (resident + 1 < CLIPMAP_RESIDENT_MIP && clipmap_in_window(origin, level_texel, resident + 1)) {
				resident++;
			}
		}
		r_requests.push_back(uint64_t(index) | (far_resident && resident == mip ? REQUEST_COARSE : 0u));
		if (far_resident && resident != mip) {
			const Vector2i resident_page = clipmap_page(level_texel, resident);
			r_requests.push_back(uint64_t(entry_index(map, resident, uint32_t(resident_page.x), uint32_t(resident_page.y))) | REQUEST_COARSE);
		}
		if (r_sample != nullptr && r_sample->table != nullptr && _sample_confirmed(r_sample, index)) {
			_sample_record(r_sample, index, clipmap_sample(r_sample->table, p_table_maps, p_maps, first, count, texel0, level, mip));
		}
		return;
	}
}

void RendererVirtualShadowMapStorage::mark_local_box(const MapData *p_maps, uint32_t p_table_maps, const MarkLight &p_light, const Vector3 &p_light_world, const AABB &p_box, LocalVector<uint64_t> &r_requests, SampleCheck *r_sample) {
	if (!(p_light.inv_radius > 0.0f)) {
		return;
	}
	// The light's sphere must touch the box.
	const float radius = 1.0f / p_light.inv_radius;
	const Vector3 box_min = p_box.position;
	const Vector3 box_max = p_box.position + p_box.size;
	float d2 = 0.0f;
	for (int axis = 0; axis < 3; axis++) {
		const float below = float(box_min[axis]) - float(p_light_world[axis]);
		const float above = float(p_light_world[axis]) - float(box_max[axis]);
		const float excess = MAX(MAX(below, above), 0.0f);
		d2 = d2 + excess * excess;
	}
	if (d2 > radius * radius) {
		return;
	}
	Vector3 corners[9];
	for (int i = 0; i < 8; i++) {
		corners[i] = Vector3((i & 1) ? box_max.x : box_min.x, (i & 2) ? box_max.y : box_min.y, (i & 4) ? box_max.z : box_min.z);
	}
	corners[8] = (box_min + box_max) * 0.5f;
	const uint32_t count = (p_light.flags >> LIGHT_FLAGS_COUNT_SHIFT) & LIGHT_FLAGS_COUNT_MASK;
	for (uint32_t face = 0; face < count; face++) {
		const uint32_t map = light_map(p_light.map_base, p_light.flags, face);
		if (map >= p_table_maps) {
			continue;
		}
		const MapData &data = p_maps[map];
		if (data.kind == MAP_KIND_MAX || data.kind == MAP_KIND_DIRECTIONAL) {
			continue;
		}
		// Clip-space planes w - x, w + x, w - y, w + y and w - near: skip the face when every corner is
		// outside one of them; take every coarse page when the near plane cuts the box.
		uint32_t outside[5] = { 0, 0, 0, 0, 0 };
		float u_min = 1.0f;
		float u_max = 0.0f;
		float v_min = 1.0f;
		float v_max = 0.0f;
		bool cut = false;
		for (int i = 0; i < 8; i++) {
			const float x = row_dot(data.rows[0], corners[i]);
			const float y = row_dot(data.rows[1], corners[i]);
			const float w = row_dot(data.rows[3], corners[i]);
			outside[0] += (w - x < 0.0f) ? 1 : 0;
			outside[1] += (w + x < 0.0f) ? 1 : 0;
			outside[2] += (w - y < 0.0f) ? 1 : 0;
			outside[3] += (w + y < 0.0f) ? 1 : 0;
			outside[4] += (w - data.near_w < 0.0f) ? 1 : 0;
			if (!(w > data.near_w)) {
				cut = true;
				continue;
			}
			const float u = x / w * 0.5f + 0.5f;
			const float v = y / w * 0.5f + 0.5f;
			u_min = MIN(u_min, u);
			u_max = MAX(u_max, u);
			v_min = MIN(v_min, v);
			v_max = MAX(v_max, v);
		}
		if (outside[0] == 8 || outside[1] == 8 || outside[2] == 8 || outside[3] == 8 || outside[4] == 8) {
			continue;
		}
		uint32_t tx0 = 0;
		uint32_t ty0 = 0;
		uint32_t tx1 = VIRTUAL_SIZE - 1;
		uint32_t ty1 = VIRTUAL_SIZE - 1;
		if (!cut) {
			tx0 = MIN(uint32_t(CLAMP(u_min, 0.0f, 1.0f) * float(VIRTUAL_SIZE)), VIRTUAL_SIZE - 1);
			ty0 = MIN(uint32_t(CLAMP(v_min, 0.0f, 1.0f) * float(VIRTUAL_SIZE)), VIRTUAL_SIZE - 1);
			tx1 = MIN(uint32_t(CLAMP(u_max, 0.0f, 1.0f) * float(VIRTUAL_SIZE)), VIRTUAL_SIZE - 1);
			ty1 = MIN(uint32_t(CLAMP(v_max, 0.0f, 1.0f) * float(VIRTUAL_SIZE)), VIRTUAL_SIZE - 1);
		}
		for (uint32_t mip = COARSE_FIRST_MIP; mip < MIP_COUNT; mip++) {
			const uint32_t shift = mip + PAGE_SIZE_SHIFT;
			for (uint32_t y = ty0 >> shift; y <= (ty1 >> shift); y++) {
				for (uint32_t x = tx0 >> shift; x <= (tx1 >> shift); x++) {
					r_requests.push_back(uint64_t(entry_index(map, mip, x, y)) | REQUEST_COARSE);
				}
			}
		}
		if (r_sample != nullptr && r_sample->table != nullptr) {
			for (int i = 0; i < 9; i++) {
				float w = 0.0f;
				uint32_t tx = 0;
				uint32_t ty = 0;
				if (!face_texel(data, corners[i], w, tx, ty)) {
					continue;
				}
				const uint32_t coarse_index = entry_index(map, COARSE_FIRST_MIP, (tx >> COARSE_FIRST_MIP) >> PAGE_SIZE_SHIFT, (ty >> COARSE_FIRST_MIP) >> PAGE_SIZE_SHIFT);
				if (_sample_confirmed(r_sample, coarse_index)) {
					const Vector2 uv((float(tx) + 0.5f) / float(VIRTUAL_SIZE), (float(ty) + 0.5f) / float(VIRTUAL_SIZE));
					_sample_record(r_sample, coarse_index, lookup(r_sample->table, p_table_maps, map, uv, COARSE_FIRST_MIP, true));
				}
			}
		}
	}
}

void RendererVirtualShadowMapStorage::mark_directional_box(const MapData *p_maps, uint32_t p_table_maps, uint32_t p_map_base, uint32_t p_light_flags, const AABB &p_box, LocalVector<uint64_t> &r_requests, SampleCheck *r_sample) {
	const uint32_t count = (p_light_flags >> LIGHT_FLAGS_COUNT_SHIFT) & LIGHT_FLAGS_COUNT_MASK;
	const uint32_t first = light_map(p_map_base, p_light_flags, 0);
	if (first >= p_table_maps || p_maps[first].kind != MAP_KIND_DIRECTIONAL) {
		return;
	}
	const Vector3 box_min = p_box.position;
	const Vector3 box_max = p_box.position + p_box.size;
	Vector3 sample_points[9];
	Vector2i texel_min(INT32_MAX, INT32_MAX);
	Vector2i texel_max(INT32_MIN, INT32_MIN);
	for (int i = 0; i < 8; i++) {
		const Vector3 corner((i & 1) ? box_max.x : box_min.x, (i & 2) ? box_max.y : box_min.y, (i & 4) ? box_max.z : box_min.z);
		sample_points[i] = corner;
		const Vector2i texel = clipmap_texel0(p_maps[first], corner);
		texel_min = Vector2i(MIN(texel_min.x, texel.x), MIN(texel_min.y, texel.y));
		texel_max = Vector2i(MAX(texel_max.x, texel.x), MAX(texel_max.y, texel.y));
	}
	sample_points[8] = (box_min + box_max) * 0.5f;
	for (uint32_t level = 0; level < count; level++) {
		const uint32_t map = first + level;
		if (map >= p_table_maps) {
			break;
		}
		const Vector2i origin(p_maps[map].origin[0], p_maps[map].origin[1]);
		const Vector2i level_min = clipmap_level_texel(texel_min, level);
		const Vector2i level_max = clipmap_level_texel(texel_max, level);
		const uint32_t first_mip = level + 1 == count ? 0 : CLIPMAP_RESIDENT_MIP;
		for (uint32_t mip = first_mip; mip < MIP_COUNT; mip++) {
			// The level texels the lookup reads at this mip: [origin * PAGE_SIZE, mip window end).
			int64_t lo[2];
			int64_t hi[2];
			bool empty = false;
			for (int axis = 0; axis < 2; axis++) {
				const int64_t window_start = int64_t(_floor_shift(origin[axis], mip)) * (int64_t(PAGE_SIZE) << mip);
				lo[axis] = MAX(int64_t(level_min[axis]), int64_t(origin[axis]) * int64_t(PAGE_SIZE));
				hi[axis] = MIN(int64_t(level_max[axis]), window_start + int64_t(VIRTUAL_SIZE) - 1);
				empty = empty || lo[axis] > hi[axis];
			}
			if (empty) {
				continue;
			}
			const uint32_t shift = mip + PAGE_SIZE_SHIFT;
			const uint32_t mask = (PAGES_PER_SIDE >> mip) - 1;
			const int32_t next_end_x = _floor_shift(origin.x, mip + 1) * 2 + int32_t(PAGES_PER_SIDE >> mip);
			const int32_t next_end_y = _floor_shift(origin.y, mip + 1) * 2 + int32_t(PAGES_PER_SIDE >> mip);
			for (int32_t y = _floor_shift(int32_t(lo[1]), shift); y <= _floor_shift(int32_t(hi[1]), shift); y++) {
				for (int32_t x = _floor_shift(int32_t(lo[0]), shift); x <= _floor_shift(int32_t(hi[0]), shift); x++) {
					if (mip >= CLIPMAP_RESIDENT_MIP || x >= next_end_x || y >= next_end_y) {
						r_requests.push_back(uint64_t(entry_index(map, mip, uint32_t(x) & mask, uint32_t(y) & mask)) | REQUEST_COARSE);
					}
				}
			}
		}
	}
	if (r_sample != nullptr && r_sample->table != nullptr) {
		for (int i = 0; i < 9; i++) {
			const Vector2i point_texel0 = clipmap_texel0(p_maps[first], sample_points[i]);
			const uint32_t coarse_index = _clipmap_coarse_entry(p_maps, p_table_maps, first, count, point_texel0);
			if (coarse_index != UINT32_MAX && _sample_confirmed(r_sample, coarse_index)) {
				_sample_record(r_sample, coarse_index, clipmap_coarse_sample(r_sample->table, p_table_maps, p_maps, first, count, point_texel0));
			}
		}
	}
}

RendererVirtualShadowMapStorage::PageUpdate RendererVirtualShadowMapStorage::page_update(const PageRecord &p_page, uint32_t p_page_index, uint32_t p_grid_width, const uint32_t *p_table, uint64_t p_table_entries, uint32_t p_frame, uint32_t p_cache_frames, uint32_t &r_bucket) {
	r_bucket = 0;
	if (p_page.owner == 0) {
		return PAGE_UPDATE_FREE;
	}
	const uint64_t index = p_page.owner - 1;
	const uint32_t entry = index < p_table_entries ? p_table[index] : 0u;
	if (!(entry & ENTRY_ALLOCATED) || p_grid_width == 0 || entry_page_x(entry) != p_page_index % p_grid_width || entry_page_y(entry) != p_page_index / p_grid_width) {
		return PAGE_UPDATE_ORPHANED;
	}
	if (entry & ENTRY_REQUESTED) {
		r_bucket = BUCKET_NONE;
		return PAGE_UPDATE_KEPT;
	}
	const uint32_t age = p_frame - p_page.last_used;
	if (age >= p_cache_frames || (entry & ENTRY_SCROLLED)) {
		return PAGE_UPDATE_EVICTED;
	}
	// Cached: the least recently used first; a page requested earlier in this frame is not reused.
	r_bucket = age != 0 ? p_cache_frames - age : BUCKET_NONE;
	return PAGE_UPDATE_CACHED;
}

RendererVirtualShadowMapStorage::AllocationCounts RendererVirtualShadowMapStorage::allocate_model(uint32_t *r_table, uint64_t p_table_entries, PageRecord *r_pages, uint32_t p_pool_pages, uint32_t p_grid_width, const uint32_t *p_requests, uint32_t p_request_count, uint32_t p_frame, uint32_t p_cache_frames, uint32_t p_resident_count) {
	AllocationCounts counts;
	ERR_FAIL_COND_V(p_grid_width == 0 || p_cache_frames == 0 || p_cache_frames > MAX_CACHE_FRAMES, counts);
	// Update: orphaned pages, pages dropped after cache_frames frames or scrolled, the rest by bucket.
	uint32_t bucket_counts[BUCKET_COUNT] = {};
	for (uint32_t p = 0; p < p_pool_pages; p++) {
		PageRecord &page = r_pages[p];
		uint32_t bucket = BUCKET_NONE;
		switch (page_update(page, p, p_grid_width, r_table, p_table_entries, p_frame, p_cache_frames, bucket)) {
			case PAGE_UPDATE_FREE:
				break;
			case PAGE_UPDATE_KEPT:
				page.last_used = p_frame;
				break;
			case PAGE_UPDATE_CACHED:
				counts.cached++;
				break;
			case PAGE_UPDATE_EVICTED:
				r_table[page.owner - 1] = 0;
				page.owner = 0;
				counts.evicted++;
				break;
			case PAGE_UPDATE_ORPHANED:
				page.owner = 0;
				counts.orphaned++;
				break;
		}
		page.bucket = bucket;
		if (bucket != BUCKET_NONE) {
			bucket_counts[bucket]++;
		}
	}
	// The free list: free pages first, then cached pages, least recently used first.
	uint32_t offsets[BUCKET_COUNT] = {};
	for (uint32_t b = 0; b < BUCKET_COUNT; b++) {
		offsets[b] = counts.available;
		counts.available += bucket_counts[b];
	}
	LocalVector<uint32_t> free_list;
	free_list.resize(counts.available);
	for (uint32_t p = 0; p < p_pool_pages; p++) {
		if (r_pages[p].bucket != BUCKET_NONE) {
			free_list[offsets[r_pages[p].bucket]++] = p;
		}
	}
	// Assign: the always-resident requests (the list's first p_resident_count) first, then the other
	// coarse requests, then the rest.
	uint32_t cursor = 0;
	for (uint32_t phase = 0; phase < 3; phase++) {
		for (uint32_t i = 0; i < p_request_count; i++) {
			const uint32_t index = p_requests[i];
			ERR_CONTINUE(uint64_t(index) >= p_table_entries);
			uint32_t entry = r_table[index];
			const bool coarse = (entry & ENTRY_COARSE) != 0;
			const uint32_t request_phase = i < p_resident_count ? 0 : (coarse ? 1 : 2);
			if (request_phase != phase) {
				continue;
			}
			counts.coarse += coarse ? 1 : 0;
			if (entry & ENTRY_ALLOCATED) {
				counts.kept++;
				if (entry & (ENTRY_SCROLLED | ENTRY_UNCACHED)) {
					entry |= ENTRY_DIRTY_MASK;
					r_table[index] = entry;
				}
				counts.dirty += (entry & ENTRY_DIRTY_MASK) ? 1 : 0;
				continue;
			}
			const uint32_t slot = cursor++;
			if (slot >= counts.available) {
				r_table[index] = entry | ENTRY_OVERFLOW;
				counts.overflow++;
				counts.resident_overflow += phase == 0 ? 1 : 0;
				continue;
			}
			const uint32_t p = free_list[slot];
			if (r_pages[p].owner != 0) {
				r_table[r_pages[p].owner - 1] = 0;
				counts.stolen++;
			}
			r_table[index] = entry_pack(p % p_grid_width, p / p_grid_width, ENTRY_ALLOCATED | ENTRY_REQUESTED | ENTRY_DIRTY_MASK | (entry & (ENTRY_COARSE | ENTRY_UNCACHED)));
			r_pages[p].owner = index + 1;
			r_pages[p].last_used = p_frame;
			r_pages[p].bucket = BUCKET_NONE;
			counts.new_pages++;
			counts.dirty++;
		}
	}
	return counts;
}

uint32_t RendererVirtualShadowMapStorage::overflow_bias_update(uint32_t p_state, uint32_t p_requests, uint32_t p_max_requests, uint32_t p_overflow, bool p_new_frame) {
	uint32_t bias = p_state & 0xFFu;
	uint32_t decay = (p_state >> 8) & 0xFFu;
	bool raised = !p_new_frame && (p_state & OVERFLOW_BIAS_RAISED) != 0;
	if (p_overflow > 0) {
		// Pressure: the largest requesters (at least half the largest map's requests) take one mip more,
		// once per frame.
		decay = 0;
		if (!raised && p_requests > 0 && uint64_t(p_requests) * 2 >= uint64_t(p_max_requests)) {
			bias = MIN(bias + 1, OVERFLOW_MAX_BIAS);
			raised = true;
		}
	} else if (p_new_frame) {
		// A frame without overflow so far: its first view counts it.
		if (bias > 0) {
			decay++;
			if (decay >= OVERFLOW_DECAY_FRAMES) {
				bias--;
				decay = 0;
			}
		} else {
			decay = 0;
		}
	}
	return bias | (decay << 8) | (raised ? OVERFLOW_BIAS_RAISED : 0u);
}
