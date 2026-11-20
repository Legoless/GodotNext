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

#include "core/config/project_settings.h"
#include "core/error/error_macros.h"
#include "core/math/math_funcs.h"
#include "core/math/projection.h"
#include "core/os/mutex.h"
#include "core/string/print_string.h"
#include "core/templates/safe_refcount.h"
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
	const char *state_names[] = { "REQUESTS", "REQUESTS_DROPPED", "AVAILABLE", "CURSOR", "OVERFLOW", "NEW", "KEPT", "EVICTED", "ORPHANED", "STOLEN", "DIRTY", "DIRTY_DIRECTIONAL", "COARSE", "CACHED", "MAX_MAP_REQUESTS", "BIASED_MAPS", "RESIDENT", "RESIDENT_OVERFLOW", "SINK", "DIRTY_STATIC", "DIRTY_DYNAMIC", "DEFERRED_STATIC", "INVALIDATED_STATIC", "INVALIDATED_DYNAMIC" };
	static_assert(sizeof(state_names) / sizeof(state_names[0]) == STATE_INVALIDATED_DYNAMIC + 1, "One name per per-view counter.");
	for (uint32_t i = 0; i < sizeof(state_names) / sizeof(state_names[0]); i++) {
		defines += "#define VSM_STATE_" + String(state_names[i]) + " " + itos(i) + "u\n";
	}
	defines += "#define VSM_STATE_TOTAL_OVERFLOW " + itos(STATE_TOTAL_OVERFLOW) + "u\n";
	defines += "#define VSM_STATE_OVERFLOW_VIEWS " + itos(STATE_OVERFLOW_VIEWS) + "u\n";
	defines += "#define VSM_STATE_TOTAL_DROPPED " + itos(STATE_TOTAL_DROPPED) + "u\n";
	defines += "#define VSM_STATE_VIEWS " + itos(STATE_VIEWS) + "u\n";
	defines += "#define VSM_STATE_BIAS_FRAME " + itos(STATE_BIAS_FRAME) + "u\n";
	defines += "#define VSM_STATE_TOTAL_RESIDENT_OVERFLOW " + itos(STATE_TOTAL_RESIDENT_OVERFLOW) + "u\n";
	// GodotNext: vsm_caching (#212).
	defines += "#define VSM_STATE_TOTAL_DIRTY_STATIC " + itos(STATE_TOTAL_DIRTY_STATIC) + "u\n";
	defines += "#define VSM_STATE_TOTAL_DIRTY_DYNAMIC " + itos(STATE_TOTAL_DIRTY_DYNAMIC) + "u\n";
	defines += "#define VSM_STATE_TOTAL_DEFERRED_STATIC " + itos(STATE_TOTAL_DEFERRED_STATIC) + "u\n";
	defines += "#define VSM_STATE_TOTAL_INVALIDATED_STATIC " + itos(STATE_TOTAL_INVALIDATED_STATIC) + "u\n";
	defines += "#define VSM_STATE_TOTAL_INVALIDATED_DYNAMIC " + itos(STATE_TOTAL_INVALIDATED_DYNAMIC) + "u\n";
	defines += "#define VSM_STATE_BUCKETS " + itos(STATE_BUCKETS) + "u\n";
	defines += "#define VSM_STATE_BUCKET_OFFSETS " + itos(STATE_BUCKET_OFFSETS) + "u\n";
	defines += "#define VSM_STATE_BUCKET_CURSORS " + itos(STATE_BUCKET_CURSORS) + "u\n";
	defines += "#define VSM_DEFER_BUCKET_COUNT " + itos(DEFER_BUCKET_COUNT) + "u\n";
	defines += "#define VSM_DEFER_SKIPPED " + itos(DEFER_SKIPPED) + "u\n";
	defines += "#define VSM_PAGE_FLAG_HAS_DYNAMIC_PREV " + itos(PAGE_FLAG_HAS_DYNAMIC_PREV) + "u\n";
	defines += "#define VSM_PAGE_FLAG_HAS_DYNAMIC_CURR " + itos(PAGE_FLAG_HAS_DYNAMIC_CURR) + "u\n";
	defines += "#define VSM_STATE_DEFER_BUCKETS " + itos(STATE_DEFER_BUCKETS) + "u\n";
	defines += "#define VSM_STATE_DEFER_ALLOW " + itos(STATE_DEFER_ALLOW) + "u\n";
	defines += "#define VSM_STATE_DEFER_USED " + itos(STATE_DEFER_USED) + "u\n";
	const char *map_state_names[] = { "REQUESTS", "DIRTY_COUNT", "DIRTY_OFFSET", "DIRTY_CURSOR", "BIAS", "CULL_MASK" };
	static_assert(sizeof(map_state_names) / sizeof(map_state_names[0]) == MAP_STATE_REGIONS, "One name per map state region.");
	for (uint32_t i = 0; i < MAP_STATE_REGIONS; i++) {
		defines += "#define VSM_MAP_STATE_" + String(map_state_names[i]) + " " + itos(i) + "u\n";
	}
	defines += "#define VSM_INDIRECT_DIRECTIONAL " + itos(INDIRECT_DIRECTIONAL) + "u\n";
	defines += "#define VSM_INDIRECT_LOCAL " + itos(INDIRECT_LOCAL) + "u\n";
	defines += "#define VSM_INDIRECT_ALL " + itos(INDIRECT_ALL) + "u\n";
	// GodotNext: vsm_projection (#222).
	defines += "#define VSM_LIGHT_FLAGS_SAMPLE " + itos(LIGHT_FLAGS_SAMPLE) + "u\n";
	defines += "#define VSM_CLIPMAP_LEVEL0_EXTENT " + vformat("%.6f", CLIPMAP_LEVEL0_EXTENT) + "\n";
	defines += "#define VSM_SMRT_MAX_RAYS " + itos(SMRT_MAX_RAYS) + "u\n";
	defines += "#define VSM_SMRT_MAX_STEPS " + itos(SMRT_MAX_STEPS) + "u\n";
	defines += "#define VSM_SMRT_MAX_TEXELS " + vformat("%.6f", SMRT_MAX_TEXELS) + "\n";
	defines += "#define VSM_SMRT_FIRST_STEP_TEXELS " + vformat("%.6f", SMRT_FIRST_STEP_TEXELS) + "\n";
	defines += "#define VSM_SMRT_NORMAL_BIAS_TEXELS " + vformat("%.6f", SMRT_NORMAL_BIAS_TEXELS) + "\n";
	defines += "#define VSM_SMRT_DEPTH_BIAS_TEXELS " + vformat("%.6f", SMRT_DEPTH_BIAS_TEXELS) + "\n";
	defines += "#define VSM_SMRT_LOCAL_MAX_REACH " + vformat("%.6f", SMRT_LOCAL_MAX_REACH) + "\n";
	defines += "#define VSM_PREFILTERED_DISTANT_LEVELS " + itos(PREFILTERED_DISTANT_LEVELS) + "u\n";
	defines += "#define VSM_PREFILTERED_DISTANT_TAPS " + itos(PREFILTERED_DISTANT_TAPS) + "u\n";
	defines += "#define VSM_PREFILTERED_DISTANT_MIN_VARIANCE_TEXELS " + vformat("%.6f", PREFILTERED_DISTANT_MIN_VARIANCE_TEXELS) + "\n";
	defines += "#define VSM_PREFILTERED_DISTANT_BLEED " + vformat("%.6f", PREFILTERED_DISTANT_BLEED) + "\n";
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

uint32_t RendererVirtualShadowMapStorage::directional_update(RID p_light_instance, const Basis &p_light_basis, const Vector3 &p_camera_position, float p_max_distance, bool p_invalidate_on_turn) {
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
			if (!same_plane && p_invalidate_on_turn) {
				// GodotNext: vsm_caching (#212). Sun rotation invalidates static layer 0 (and layer 1 where
				// dynamic content existed) so deferred_invalidation_budget can budget static re-renders.
				_table_invalidate_map(first + level, ENTRY_DIRTY_STATIC, true);
			} else {
				const ClipmapScroll scroll = same_plane ? clipmap_scroll(maps->origins[level], origin) : clipmap_scroll_all();
				if (scroll.any) {
					_table_scroll(first + level, scroll);
					scroll_passes++;
				}
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
static_assert(RendererVirtualShadowMapStorage::STATE_INVALIDATED_DYNAMIC < RendererVirtualShadowMapStorage::STATE_VIEW_END, "Per-view counters fit their region.");
static_assert(RendererVirtualShadowMapStorage::STATE_TOTAL_INVALIDATED_DYNAMIC < RendererVirtualShadowMapStorage::STATE_BUCKETS, "Totals fit their region.");
static_assert(sizeof(RendererVirtualShadowMapStorage::InvalidationBox) == 32, "InvalidationBox matches VSMInvalidationBox in vsm_invalidate.glsl.");
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
		// GodotNext: vsm_settings_debug (#221). Plus the map's moving-light bias.
		const int32_t bias = p_lod_bias + data.lod_bias + (p_map_bias != nullptr ? int32_t(overflow_bias(p_map_bias[map])) : 0);
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
	// GodotNext: vsm_settings_debug (#221). Plus the light's moving bias, held by level 0.
	const int32_t level_mip = clipmap_level_mip(p_footprint, p_lod_bias + p_maps[first].lod_bias);
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
	// GodotNext: vsm_caching (#212). Evict unrequested pages whose static layer is dirty so pool slots
	// return to the free list; keep unrequested pages that only carry ENTRY_DIRTY_DYNAMIC.
	if (age >= p_cache_frames || (entry & (ENTRY_SCROLLED | ENTRY_DIRTY_STATIC)) != 0u) {
		return PAGE_UPDATE_EVICTED;
	}
	// Cached: the least recently used first; a page requested earlier in this frame is not reused.
	r_bucket = age != 0 ? p_cache_frames - age : BUCKET_NONE;
	return PAGE_UPDATE_CACHED;
}

RendererVirtualShadowMapStorage::AllocationCounts RendererVirtualShadowMapStorage::allocate_model(uint32_t *r_table, uint64_t p_table_entries, PageRecord *r_pages, uint32_t p_pool_pages, uint32_t p_grid_width, const uint32_t *p_requests, uint32_t p_request_count, uint32_t p_frame, uint32_t p_cache_frames, uint32_t p_resident_count, uint32_t p_deferred_budget, const uint32_t *p_page_coverage) {
	AllocationCounts counts;
	ERR_FAIL_COND_V(p_grid_width == 0 || p_cache_frames == 0 || p_cache_frames > MAX_CACHE_FRAMES, counts);
	// Update: orphaned pages, pages dropped after cache_frames frames or scrolled, the rest by bucket.
	uint32_t bucket_counts[BUCKET_COUNT] = {};
	for (uint32_t p = 0; p < p_pool_pages; p++) {
		PageRecord &page = r_pages[p];
		uint32_t bucket = BUCKET_NONE;
		switch (page_update(page, p, p_grid_width, r_table, p_table_entries, p_frame, p_cache_frames, bucket)) {
			case PAGE_UPDATE_FREE:
				page.pad = 0;
				break;
			case PAGE_UPDATE_KEPT:
				page.last_used = p_frame;
				break;
			case PAGE_UPDATE_CACHED:
				counts.cached++;
				if ((page.pad & PAGE_FLAG_HAS_DYNAMIC_CURR) != 0u) {
					page.pad = PAGE_FLAG_HAS_DYNAMIC_PREV;
				}
				break;
			case PAGE_UPDATE_EVICTED:
				r_table[page.owner - 1] = 0;
				page.owner = 0;
				page.pad = 0;
				counts.evicted++;
				break;
			case PAGE_UPDATE_ORPHANED:
				page.owner = 0;
				page.pad = 0;
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
	struct DeferCandidate {
		uint32_t index = 0;
		uint32_t page = 0;
		uint32_t bucket = 0;
	};
	LocalVector<DeferCandidate> defer_candidates;
	uint32_t defer_bucket_counts[DEFER_BUCKET_COUNT] = {};
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
				const bool forced_dirty = (entry & (ENTRY_SCROLLED | ENTRY_UNCACHED)) != 0u;
				if (forced_dirty) {
					entry |= ENTRY_DIRTY_MASK;
					r_table[index] = entry;
				}
				if (p_deferred_budget > 0u && !forced_dirty && (entry & ENTRY_DIRTY_STATIC) != 0u) {
					const uint32_t p = entry_page_y(entry) * p_grid_width + entry_page_x(entry);
					if (p < p_pool_pages) {
						const uint32_t b = coverage_bucket(p_page_coverage != nullptr ? p_page_coverage[p] : 0u);
						r_pages[p].bucket = b;
						defer_bucket_counts[b]++;
						defer_candidates.push_back({ index, p, b });
						continue;
					}
				}
				if (entry & ENTRY_DIRTY_MASK) {
					counts.dirty++;
					if (entry & ENTRY_DIRTY_STATIC) {
						counts.dirty_static++;
					}
					if (entry & ENTRY_DIRTY_DYNAMIC) {
						counts.dirty_dynamic++;
					}
				}
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
			r_pages[p].pad = 0;
			counts.new_pages++;
			counts.dirty++;
			counts.dirty_static++;
			counts.dirty_dynamic++;
		}
	}
	// GodotNext: vsm_caching (#212). Deferred static invalidation budget: accept highest coverage first.
	if (p_deferred_budget > 0u && !defer_candidates.is_empty()) {
		uint32_t allow[DEFER_BUCKET_COUNT] = {};
		uint32_t used[DEFER_BUCKET_COUNT] = {};
		uint32_t remaining = p_deferred_budget;
		for (int32_t b = int32_t(DEFER_BUCKET_COUNT) - 1; b >= 0; b--) {
			const uint32_t c = defer_bucket_counts[b];
			const uint32_t take = MIN(c, remaining);
			allow[b] = take;
			remaining -= take;
			counts.deferred_static += c - take;
		}
		for (const DeferCandidate &cand : defer_candidates) {
			const uint32_t rank = used[cand.bucket]++;
			const uint32_t entry = r_table[cand.index];
			if (rank < allow[cand.bucket]) {
				r_pages[cand.page].bucket = BUCKET_NONE;
				counts.dirty++;
				counts.dirty_static++;
				if (entry & ENTRY_DIRTY_DYNAMIC) {
					counts.dirty_dynamic++;
				}
			} else {
				r_pages[cand.page].bucket = DEFER_SKIPPED;
				if (entry & ENTRY_DIRTY_DYNAMIC) {
					counts.dirty++;
					counts.dirty_dynamic++;
				}
			}
		}
	}
	return counts;
}

// GodotNext: vsm_caching (#212, plan §21.3 contract S5).
const char *const RendererVirtualShadowMapStorage::SETTING_DEFERRED_INVALIDATION_BUDGET = "rendering/lights_and_shadows/virtual_shadows/deferred_invalidation_budget";

uint32_t RendererVirtualShadowMapStorage::classify_shadow_cache_dynamic(const CacheClassificationInput &p_input) {
	switch (p_input.mode) {
		case RSE::SHADOW_CACHE_INVALIDATION_STATIC:
		case RSE::SHADOW_CACHE_INVALIDATION_RIGID:
			return 0u;
		case RSE::SHADOW_CACHE_INVALIDATION_ALWAYS:
			return CACHE_DYNAMIC_MODE_ALWAYS;
		case RSE::SHADOW_CACHE_INVALIDATION_AUTO:
		default:
			break;
	}
	uint32_t reasons = 0u;
	if (p_input.last_moved_frame != CACHE_NEVER_MOVED && p_input.frame >= p_input.last_moved_frame && (p_input.frame - p_input.last_moved_frame) < CACHE_MOTION_COOLDOWN_FRAMES) {
		reasons |= CACHE_DYNAMIC_MOVED;
	}
	if (p_input.has_mesh_instance) {
		reasons |= CACHE_DYNAMIC_MESH_INSTANCE;
	}
	if (p_input.has_skeleton) {
		reasons |= CACHE_DYNAMIC_SKELETON;
	}
	if (p_input.is_particles) {
		reasons |= CACHE_DYNAMIC_PARTICLES;
	}
	if (p_input.is_dynamic_multimesh) {
		reasons |= CACHE_DYNAMIC_MULTIMESH;
	}
	reasons |= p_input.shader_dynamic_flags & (CACHE_DYNAMIC_VERTEX | CACHE_DYNAMIC_ANIMATED | CACHE_DYNAMIC_MAIN_CAMERA | CACHE_DYNAMIC_VERTEX_TIME | CACHE_DYNAMIC_DEPTH);
	return reasons;
}

uint32_t RendererVirtualShadowMapStorage::coverage_bucket(uint32_t p_pixels) {
	if (p_pixels < 16u) {
		return p_pixels;
	}
	uint32_t msb = 0u;
	uint32_t v = p_pixels;
	while (v >>= 1u) {
		msb++;
	}
	const uint32_t sub = (p_pixels >> (msb - 1u)) & 1u;
	return MIN(16u + (msb - 4u) * 2u + sub, DEFER_BUCKET_COUNT - 1u);
}

static void _invalidate_entry_cpu(uint32_t *r_table, uint32_t p_index, uint32_t p_dirty_flags, RendererVirtualShadowMapStorage::PageRecord *r_pages, uint32_t p_pool_pages, uint32_t p_grid_width, bool p_dynamic_if_used, RendererVirtualShadowMapStorage::InvalidationCounts &r_counts) {
	using VSM = RendererVirtualShadowMapStorage;
	const uint32_t entry = r_table[p_index];
	if ((entry & VSM::ENTRY_ALLOCATED) == 0u) {
		return;
	}
	const uint32_t p = p_grid_width > 0 ? (VSM::entry_page_y(entry) * p_grid_width + VSM::entry_page_x(entry)) : UINT32_MAX;
	uint32_t flags = p_dirty_flags & VSM::ENTRY_DIRTY_MASK;
	if (p_dynamic_if_used && r_pages != nullptr && p < p_pool_pages) {
		if ((r_pages[p].pad & (VSM::PAGE_FLAG_HAS_DYNAMIC_PREV | VSM::PAGE_FLAG_HAS_DYNAMIC_CURR)) != 0u) {
			flags |= VSM::ENTRY_DIRTY_DYNAMIC;
		}
	}
	if (flags == 0u) {
		return;
	}
	if ((flags & VSM::ENTRY_DIRTY_STATIC) != 0u && (entry & VSM::ENTRY_DIRTY_STATIC) == 0u) {
		r_counts.static_pages++;
	}
	if ((flags & VSM::ENTRY_DIRTY_DYNAMIC) != 0u) {
		if ((entry & VSM::ENTRY_DIRTY_DYNAMIC) == 0u) {
			r_counts.dynamic_pages++;
		}
		if (!p_dynamic_if_used && r_pages != nullptr && p < p_pool_pages) {
			r_pages[p].pad |= VSM::PAGE_FLAG_HAS_DYNAMIC_CURR;
		}
	}
	r_table[p_index] = entry | flags;
}

RendererVirtualShadowMapStorage::InvalidationCounts RendererVirtualShadowMapStorage::invalidate_box_model(uint32_t *r_table, uint32_t p_table_maps, const MapData *p_maps, const uint32_t *p_map_cull_masks, PageRecord *r_pages, uint32_t p_pool_pages, uint32_t p_grid_width, const InvalidationBox &p_box) {
	InvalidationCounts counts;
	ERR_FAIL_NULL_V(r_table, counts);
	ERR_FAIL_NULL_V(p_maps, counts);
	const uint32_t dirty_flags = p_box.dirty_flags & ENTRY_DIRTY_MASK;
	if (dirty_flags == 0u) {
		return counts;
	}
	const Vector3 bmin(p_box.min_pos[0], p_box.min_pos[1], p_box.min_pos[2]);
	const Vector3 bmax(p_box.max_pos[0], p_box.max_pos[1], p_box.max_pos[2]);
	if (!(bmax.x >= bmin.x && bmax.y >= bmin.y && bmax.z >= bmin.z)) {
		return counts;
	}
	Vector3 corners[8];
	for (uint32_t c = 0; c < 8u; c++) {
		corners[c] = Vector3((c & 1u) ? bmax.x : bmin.x, (c & 2u) ? bmax.y : bmin.y, (c & 4u) ? bmax.z : bmin.z);
	}
	for (uint32_t m = 0; m < p_table_maps; m++) {
		const MapData &md = p_maps[m];
		if (md.kind == MAP_KIND_MAX) {
			continue;
		}
		if (p_map_cull_masks != nullptr && (p_map_cull_masks[m] & p_box.layer_mask) == 0u) {
			continue;
		}
		if (md.kind == MAP_KIND_DIRECTIONAL) {
			Vector2i level_min(INT32_MAX, INT32_MAX);
			Vector2i level_max(INT32_MIN, INT32_MIN);
			for (uint32_t c = 0; c < 8u; c++) {
				const float tx = CLAMP(std::floor(row_dot(md.rows[0], corners[c])), -1073741824.0f, 1073741824.0f);
				const float ty = CLAMP(std::floor(row_dot(md.rows[1], corners[c])), -1073741824.0f, 1073741824.0f);
				level_min.x = MIN(level_min.x, int32_t(tx));
				level_min.y = MIN(level_min.y, int32_t(ty));
				level_max.x = MAX(level_max.x, int32_t(tx));
				level_max.y = MAX(level_max.y, int32_t(ty));
			}
			const Vector2i origin(md.origin[0], md.origin[1]);
			for (uint32_t mip = 0; mip < MIP_COUNT; mip++) {
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
				const uint32_t mask = (PAGES_PER_SIDE >> mip) - 1u;
				for (int32_t y = _floor_shift(int32_t(lo[1]), shift); y <= _floor_shift(int32_t(hi[1]), shift); y++) {
					for (int32_t x = _floor_shift(int32_t(lo[0]), shift); x <= _floor_shift(int32_t(hi[0]), shift); x++) {
						const uint32_t idx = entry_index(m, mip, uint32_t(x) & mask, uint32_t(y) & mask);
						_invalidate_entry_cpu(r_table, idx, dirty_flags, r_pages, p_pool_pages, p_grid_width, false, counts);
					}
				}
			}
			continue;
		}
		uint32_t outside[6] = { 0, 0, 0, 0, 0, 0 };
		float u_min = 1.0f;
		float u_max = 0.0f;
		float v_min = 1.0f;
		float v_max = 0.0f;
		bool cut = false;
		for (uint32_t c = 0; c < 8u; c++) {
			const float x = row_dot(md.rows[0], corners[c]);
			const float y = row_dot(md.rows[1], corners[c]);
			const float w = row_dot(md.rows[3], corners[c]);
			outside[0] += (w - x < 0.0f) ? 1u : 0u;
			outside[1] += (w + x < 0.0f) ? 1u : 0u;
			outside[2] += (w - y < 0.0f) ? 1u : 0u;
			outside[3] += (w + y < 0.0f) ? 1u : 0u;
			outside[4] += (w - md.near_w < 0.0f) ? 1u : 0u;
			outside[5] += (w > md.max_distance) ? 1u : 0u;
			if (!(w > md.near_w)) {
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
		if (outside[0] == 8u || outside[1] == 8u || outside[2] == 8u || outside[3] == 8u || outside[4] == 8u || outside[5] == 8u) {
			continue;
		}
		uint32_t tx0 = 0;
		uint32_t ty0 = 0;
		uint32_t tx1 = VIRTUAL_SIZE - 1u;
		uint32_t ty1 = VIRTUAL_SIZE - 1u;
		if (!cut) {
			tx0 = MIN(uint32_t(CLAMP(u_min, 0.0f, 1.0f) * float(VIRTUAL_SIZE)), VIRTUAL_SIZE - 1u);
			ty0 = MIN(uint32_t(CLAMP(v_min, 0.0f, 1.0f) * float(VIRTUAL_SIZE)), VIRTUAL_SIZE - 1u);
			tx1 = MIN(uint32_t(CLAMP(u_max, 0.0f, 1.0f) * float(VIRTUAL_SIZE)), VIRTUAL_SIZE - 1u);
			ty1 = MIN(uint32_t(CLAMP(v_max, 0.0f, 1.0f) * float(VIRTUAL_SIZE)), VIRTUAL_SIZE - 1u);
		}
		for (uint32_t mip = 0; mip < MIP_COUNT; mip++) {
			const uint32_t shift = mip + PAGE_SIZE_SHIFT;
			for (uint32_t y = ty0 >> shift; y <= (ty1 >> shift); y++) {
				for (uint32_t x = tx0 >> shift; x <= (tx1 >> shift); x++) {
					const uint32_t idx = entry_index(m, mip, x, y);
					_invalidate_entry_cpu(r_table, idx, dirty_flags, r_pages, p_pool_pages, p_grid_width, false, counts);
				}
			}
		}
	}
	return counts;
}

RendererVirtualShadowMapStorage::InvalidationCounts RendererVirtualShadowMapStorage::invalidate_map_model(uint32_t *r_table, uint32_t p_table_maps, uint32_t p_map, uint32_t p_dirty_flags, PageRecord *r_pages, uint32_t p_pool_pages, uint32_t p_grid_width, bool p_dynamic_if_used) {
	InvalidationCounts counts;
	ERR_FAIL_NULL_V(r_table, counts);
	if (p_map >= p_table_maps) {
		return counts;
	}
	const uint32_t base = p_map * ENTRIES_PER_MAP;
	for (uint32_t local = 0; local < ENTRIES_PER_MAP; local++) {
		_invalidate_entry_cpu(r_table, base + local, p_dirty_flags, r_pages, p_pool_pages, p_grid_width, p_dynamic_if_used, counts);
	}
	return counts;
}

bool RendererVirtualShadowMapStorage::LightProperties::operator==(const LightProperties &p_other) const {
	return transform == p_other.transform && range == p_other.range && spot_angle == p_other.spot_angle && area_size == p_other.area_size && caster_mask == p_other.caster_mask && reverse_cull == p_other.reverse_cull;
}

bool RendererVirtualShadowMapStorage::note_light_properties(RID p_light_instance, const LightProperties &p_props, bool p_check_transform) {
	LightMaps *maps = light_maps.getptr(p_light_instance);
	if (maps == nullptr) {
		return false;
	}
	if (!maps->props_valid) {
		maps->props = p_props;
		maps->props_valid = true;
		return false;
	}
	bool changed = maps->props.range != p_props.range || maps->props.spot_angle != p_props.spot_angle || maps->props.area_size != p_props.area_size || maps->props.caster_mask != p_props.caster_mask || maps->props.reverse_cull != p_props.reverse_cull;
	if (p_check_transform && maps->props.transform != p_props.transform) {
		changed = true;
	}
	if (changed) {
		maps->props = p_props;
	}
	return changed;
}

uint32_t RendererVirtualShadowMapStorage::read_deferred_invalidation_budget() {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	const StringName name = SETTING_DEFERRED_INVALIDATION_BUDGET;
	if (settings == nullptr || !settings->has_setting(name)) {
		return 0u;
	}
	const int64_t value = settings->get_setting_with_override(name);
	return value > 0 ? uint32_t(MIN(value, int64_t(POOL_MAX_PAGES))) : 0u;
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

/* NON-NANITE PAGE CULLING AND RASTER (#218) */

static_assert(sizeof(RendererVirtualShadowMapStorage::DirtyPageRecord) == 16, "DirtyPageRecord matches uvec4 in vsm_allocate_pages.glsl.");

uint32_t RendererVirtualShadowMapStorage::depth_pack(float p_depth) {
	if (!(p_depth == p_depth)) {
		return 0u;
	}
	float neg = CLAMP(-p_depth, -1.0e30f, 1.0e30f);
	if (neg == 0.0f) {
		neg = 0.0f;
	}
	uint32_t bits = 0;
	memcpy(&bits, &neg, sizeof(uint32_t));
	return (bits & 0x80000000u) != 0u ? ~bits : (bits | 0x80000000u);
}

float RendererVirtualShadowMapStorage::depth_unpack(uint32_t p_packed) {
	if (p_packed == 0u) {
		return float(Math::INF);
	}
	const uint32_t bits = (p_packed & 0x80000000u) != 0u ? (p_packed & 0x7FFFFFFFu) : ~p_packed;
	float neg = 0.0f;
	memcpy(&neg, &bits, sizeof(float));
	return neg == 0.0f ? 0.0f : -neg;
}

float RendererVirtualShadowMapStorage::view_facing_reach(const AABB &p_local_aabb) {
	const Vector3 lmin = p_local_aabb.position;
	const Vector3 lmax = p_local_aabb.position + p_local_aabb.size;
	float reach = 0.0f;
	for (uint32_t c = 0; c < 8u; c++) {
		const Vector3 lp((c & 1u) ? lmax.x : lmin.x, (c & 2u) ? lmax.y : lmin.y, (c & 4u) ? lmax.z : lmin.z);
		reach = MAX(reach, float(Math::abs(lp.x) + Math::abs(lp.y) + Math::abs(lp.z)));
	}
	return reach;
}

Transform3D RendererVirtualShadowMapStorage::map_view(const MapData &p_map, const Vector3 &p_camera_position) {
	Vector3 x_axis;
	Vector3 y_axis;
	Vector3 z_axis;
	Vector3 origin;
	if (p_map.kind == MAP_KIND_DIRECTIONAL) {
		x_axis = Vector3(p_map.rows[0][0], p_map.rows[0][1], p_map.rows[0][2]).normalized();
		y_axis = Vector3(p_map.rows[1][0], p_map.rows[1][1], p_map.rows[1][2]).normalized();
		z_axis = -Vector3(p_map.rows[2][0], p_map.rows[2][1], p_map.rows[2][2]);
		origin = p_camera_position;
	} else {
		const float inv_cot = 1.0f / Vector3(p_map.rows[0][0], p_map.rows[0][1], p_map.rows[0][2]).length();
		x_axis = Vector3(p_map.rows[0][0], p_map.rows[0][1], p_map.rows[0][2]) * inv_cot;
		y_axis = Vector3(p_map.rows[1][0], p_map.rows[1][1], p_map.rows[1][2]) * inv_cot;
		z_axis = -Vector3(p_map.rows[3][0], p_map.rows[3][1], p_map.rows[3][2]);
		const Vector3 t(p_map.rows[0][3] * inv_cot, p_map.rows[1][3] * inv_cot, -p_map.rows[3][3]);
		origin = -(x_axis * t.x + y_axis * t.y + z_axis * t.z);
	}
	return Transform3D(Basis(x_axis, y_axis, z_axis), origin); // The axes are the basis's columns.
}

bool RendererVirtualShadowMapStorage::sub_instance_box(const float p_model[12], const float *p_transforms, uint32_t p_sub_index, uint32_t p_current_offset, uint32_t p_stride_vec4, bool p_format_2d, uint32_t p_trail_steps, const AABB &p_local_aabb, float p_extra_margin, AABB &r_world_aabb, bool p_view_facing, float p_corner_reach) {
	ERR_FAIL_NULL_V(p_model, false);
	ERR_FAIL_NULL_V(p_transforms, false);
	const uint32_t steps = MAX(p_trail_steps, 1u);
	if (p_trail_steps <= 1u && p_stride_vec4 == 0u) {
		return false;
	}
	static const float unit_z[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
	const Vector3 lmin = p_local_aabb.position;
	const Vector3 lmax = p_local_aabb.position + p_local_aabb.size;
	Vector3 world_min(1.0e30f, 1.0e30f, 1.0e30f);
	Vector3 world_max(-1.0e30f, -1.0e30f, -1.0e30f);
	bool any_valid = false;
	for (uint32_t seg = 0; seg < steps; seg++) {
		const uint32_t ofs_vec4 = (p_trail_steps > 1u) ? ((p_trail_steps * p_sub_index + seg) * 5u) : (p_stride_vec4 * (p_sub_index + p_current_offset));
		const float *r0 = p_transforms + uint64_t(ofs_vec4) * 4u;
		const float *r1 = r0 + 4u;
		const float *r2 = (p_trail_steps <= 1u && p_format_2d) ? unit_z : (r0 + 8u);
		bool finite = true;
		for (int k = 0; k < 4; k++) {
			finite = finite && (r0[k] == r0[k]) && Math::abs(r0[k]) < 1.0e30f;
			finite = finite && (r1[k] == r1[k]) && Math::abs(r1[k]) < 1.0e30f;
			finite = finite && (r2[k] == r2[k]) && Math::abs(r2[k]) < 1.0e30f;
		}
		if (!finite) {
			continue;
		}
		const float basis_sq = ((r0[0] * r0[0] + r0[1] * r0[1]) + r0[2] * r0[2]) + ((r1[0] * r1[0] + r1[1] * r1[1]) + r1[2] * r1[2]) + ((r2[0] * r2[0] + r2[1] * r2[1]) + r2[2] * r2[2]);
		if (!(basis_sq > 0.0f)) {
			continue;
		}
		for (uint32_t c = 0; c < 8u; c++) {
			const Vector3 lp((c & 1u) ? lmax.x : lmin.x, (c & 2u) ? lmax.y : lmin.y, (c & 4u) ? lmax.z : lmin.z);
			const Vector3 sp(row_dot(r0, lp), row_dot(r1, lp), row_dot(r2, lp));
			const Vector3 wp(row_dot(p_model + 0, sp), row_dot(p_model + 4, sp), row_dot(p_model + 8, sp));
			if (!any_valid) {
				world_min = wp;
				world_max = wp;
				any_valid = true;
			} else {
				world_min = world_min.min(wp);
				world_max = world_max.max(wp);
			}
		}
		if (p_view_facing) {
			const Vector3 inst_origin(r0[3], r1[3], r2[3]);
			const Vector3 origin(row_dot(p_model + 0, inst_origin), row_dot(p_model + 4, inst_origin), row_dot(p_model + 8, inst_origin));
			float longest_sq = 0.0f;
			for (int k = 0; k < 3; k++) {
				const Vector3 inst_axis(r0[k], r1[k], r2[k]);
				const Vector3 world_axis(
						p_model[0] * inst_axis.x + p_model[1] * inst_axis.y + p_model[2] * inst_axis.z,
						p_model[4] * inst_axis.x + p_model[5] * inst_axis.y + p_model[6] * inst_axis.z,
						p_model[8] * inst_axis.x + p_model[9] * inst_axis.y + p_model[10] * inst_axis.z);
				longest_sq = MAX(longest_sq, float(world_axis.length_squared()));
			}
			const float reach = p_corner_reach * MAX(1.0f, std::sqrt(longest_sq));
			world_min = world_min.min(origin - Vector3(reach, reach, reach));
			world_max = world_max.max(origin + Vector3(reach, reach, reach));
		}
	}
	if (!any_valid) {
		return false;
	}
	const float margin = MAX(p_extra_margin, 0.0f);
	world_min -= Vector3(margin, margin, margin);
	world_max += Vector3(margin, margin, margin);
	r_world_aabb = AABB(world_min, world_max - world_min);
	return true;
}

void RendererVirtualShadowMapStorage::cull_box_dirty_pages(const MapData *p_maps, uint32_t p_table_maps, const uint32_t *p_map_dirty_counts, const uint32_t *p_map_dirty_offsets, const DirtyPageRecord *p_dirty, uint32_t p_dirty_limit, uint32_t p_map_base, uint32_t p_light_flags, const AABB &p_box, bool p_dynamic, float p_min_texels, LocalVector<uint32_t> &r_dirty_indices, uint32_t &r_small_culled) {
	r_dirty_indices.clear();
	r_small_culled = 0;
	if (p_maps == nullptr || p_map_dirty_counts == nullptr || p_map_dirty_offsets == nullptr || p_dirty == nullptr || p_dirty_limit == 0 || p_map_base == MAP_NONE) {
		return;
	}
	const Vector3 box_min = p_box.position;
	const Vector3 box_max = p_box.position + p_box.size;
	if (!(box_max.x >= box_min.x && box_max.y >= box_min.y && box_max.z >= box_min.z)) {
		return;
	}
	const float dx = float(box_max.x) - float(box_min.x);
	const float dy = float(box_max.y) - float(box_min.y);
	const float dz = float(box_max.z) - float(box_min.z);
	const float box_extent = MAX(MAX(dx, dy), dz);
	const uint32_t req_dirty = p_dynamic ? ENTRY_DIRTY_DYNAMIC : ENTRY_DIRTY_STATIC;
	const uint32_t map_count = (p_light_flags >> LIGHT_FLAGS_COUNT_SHIFT) & LIGHT_FLAGS_COUNT_MASK;
	for (uint32_t m_idx = 0; m_idx < map_count; m_idx++) {
		const uint32_t map = light_map(p_map_base, p_light_flags, m_idx);
		if (map >= p_table_maps) {
			continue;
		}
		const uint32_t d_offset = p_map_dirty_offsets[map];
		const uint32_t d_count = p_map_dirty_counts[map];
		if (d_count == 0u || d_offset >= p_dirty_limit) {
			continue;
		}
		const uint32_t d_end = MIN(d_offset + d_count, p_dirty_limit);
		const MapData &md = p_maps[map];
		if (md.kind == MAP_KIND_MAX) {
			continue;
		}
		if (md.kind == MAP_KIND_DIRECTIONAL) {
			float min_tx = 1.0e30f;
			float max_tx = -1.0e30f;
			float min_ty = 1.0e30f;
			float max_ty = -1.0e30f;
			for (uint32_t c = 0; c < 8u; c++) {
				const Vector3 cp((c & 1u) ? box_max.x : box_min.x, (c & 2u) ? box_max.y : box_min.y, (c & 4u) ? box_max.z : box_min.z);
				const float tx = row_dot(md.rows[0], cp);
				const float ty = row_dot(md.rows[1], cp);
				if (c == 0u) {
					min_tx = tx;
					max_tx = tx;
					min_ty = ty;
					max_ty = ty;
				} else {
					min_tx = MIN(min_tx, tx);
					max_tx = MAX(max_tx, tx);
					min_ty = MIN(min_ty, ty);
					max_ty = MAX(max_ty, ty);
				}
			}
			min_tx = CLAMP(min_tx, -1073741824.0f, 1073741824.0f);
			max_tx = CLAMP(max_tx, -1073741824.0f, 1073741824.0f);
			min_ty = CLAMP(min_ty, -1073741824.0f, 1073741824.0f);
			max_ty = CLAMP(max_ty, -1073741824.0f, 1073741824.0f);
			const int32_t t_min_x = int32_t(std::floor(min_tx));
			const int32_t t_max_x = int32_t(std::floor(max_tx));
			const int32_t t_min_y = int32_t(std::floor(min_ty));
			const int32_t t_max_y = int32_t(std::floor(max_ty));
			const int32_t win_min_x = md.origin[0] * int32_t(PAGE_SIZE);
			const int32_t win_max_x = (md.origin[0] + int32_t(PAGES_PER_SIDE)) * int32_t(PAGE_SIZE) - 1;
			const int32_t win_min_y = md.origin[1] * int32_t(PAGE_SIZE);
			const int32_t win_max_y = (md.origin[1] + int32_t(PAGES_PER_SIDE)) * int32_t(PAGE_SIZE) - 1;
			if (t_max_x < win_min_x || t_min_x > win_max_x || t_max_y < win_min_y || t_min_y > win_max_y) {
				continue;
			}
			for (uint32_t d = d_offset; d < d_end; d++) {
				const DirtyPageRecord &rec = p_dirty[d];
				if ((rec.flags & req_dirty) == 0u) {
					continue;
				}
				const uint32_t mip = rec.packed_page & 0xFFu;
				if (mip >= MIP_COUNT) {
					continue;
				}
				const uint32_t slot_x = (rec.packed_page >> 8u) & 0xFFu;
				const uint32_t slot_y = (rec.packed_page >> 16u) & 0xFFu;
				const uint32_t mask = (PAGES_PER_SIDE >> mip) - 1u;
				const Vector2i mip_origin = clipmap_mip_origin(Vector2i(md.origin[0], md.origin[1]), mip);
				const int32_t abs_px = mip_origin.x + int32_t((slot_x - uint32_t(mip_origin.x)) & mask);
				const int32_t abs_py = mip_origin.y + int32_t((slot_y - uint32_t(mip_origin.y)) & mask);
				const int32_t page_texels = int32_t(PAGE_SIZE << mip);
				const int32_t p_min_x = abs_px * page_texels;
				const int32_t p_max_x = p_min_x + page_texels - 1;
				const int32_t p_min_y = abs_py * page_texels;
				const int32_t p_max_y = p_min_y + page_texels - 1;
				if (t_max_x < p_min_x || t_min_x > p_max_x || t_max_y < p_min_y || t_min_y > p_max_y) {
					continue;
				}
				const float mip_texel = md.texel_scale * float(1u << mip);
				if (p_min_texels > 0.0f && box_extent < p_min_texels * mip_texel) {
					r_small_culled++;
					continue;
				}
				r_dirty_indices.push_back(d);
			}
		} else {
			float cx[8] = {};
			float cy[8] = {};
			float cw[8] = {};
			uint32_t out_near = 0;
			uint32_t out_far = 0;
			uint32_t out_left = 0;
			uint32_t out_right = 0;
			uint32_t out_bottom = 0;
			uint32_t out_top = 0;
			float min_w = 0.0f;
			for (uint32_t c = 0; c < 8u; c++) {
				const Vector3 cp((c & 1u) ? box_max.x : box_min.x, (c & 2u) ? box_max.y : box_min.y, (c & 4u) ? box_max.z : box_min.z);
				const float x = row_dot(md.rows[0], cp);
				const float y = row_dot(md.rows[1], cp);
				const float w = row_dot(md.rows[3], cp);
				cx[c] = x;
				cy[c] = y;
				cw[c] = w;
				min_w = c == 0u ? w : MIN(min_w, w);
				out_near += w < md.near_w ? 1u : 0u;
				out_far += w > md.max_distance ? 1u : 0u;
				out_left += x < -w ? 1u : 0u;
				out_right += x > w ? 1u : 0u;
				out_bottom += y < -w ? 1u : 0u;
				out_top += y > w ? 1u : 0u;
			}
			if (out_near == 8u || out_far == 8u || out_left == 8u || out_right == 8u || out_bottom == 8u || out_top == 8u) {
				continue;
			}
			uint32_t t_min_x = 0u;
			uint32_t t_max_x = VIRTUAL_SIZE - 1u;
			uint32_t t_min_y = 0u;
			uint32_t t_max_y = VIRTUAL_SIZE - 1u;
			if (min_w > md.near_w) {
				float min_u = 0.0f;
				float max_u = 0.0f;
				float min_v = 0.0f;
				float max_v = 0.0f;
				for (uint32_t c = 0; c < 8u; c++) {
					const float u = (cx[c] / cw[c]) * 0.5f + 0.5f;
					const float v = (cy[c] / cw[c]) * 0.5f + 0.5f;
					if (c == 0u) {
						min_u = u;
						max_u = u;
						min_v = v;
						max_v = v;
					} else {
						min_u = MIN(min_u, u);
						max_u = MAX(max_u, u);
						min_v = MIN(min_v, v);
						max_v = MAX(max_v, v);
					}
				}
				if (max_u < 0.0f || min_u > 1.0f || max_v < 0.0f || min_v > 1.0f) {
					continue;
				}
				t_min_x = MIN(uint32_t(CLAMP(min_u, 0.0f, 1.0f) * float(VIRTUAL_SIZE)), VIRTUAL_SIZE - 1u);
				t_max_x = MIN(uint32_t(CLAMP(max_u, 0.0f, 1.0f) * float(VIRTUAL_SIZE)), VIRTUAL_SIZE - 1u);
				t_min_y = MIN(uint32_t(CLAMP(min_v, 0.0f, 1.0f) * float(VIRTUAL_SIZE)), VIRTUAL_SIZE - 1u);
				t_max_y = MIN(uint32_t(CLAMP(max_v, 0.0f, 1.0f) * float(VIRTUAL_SIZE)), VIRTUAL_SIZE - 1u);
			}
			const float closest_w = MAX(min_w, md.near_w);
			for (uint32_t d = d_offset; d < d_end; d++) {
				const DirtyPageRecord &rec = p_dirty[d];
				if ((rec.flags & req_dirty) == 0u) {
					continue;
				}
				const uint32_t mip = rec.packed_page & 0xFFu;
				if (mip >= MIP_COUNT) {
					continue;
				}
				const uint32_t page_x = (rec.packed_page >> 8u) & 0xFFu;
				const uint32_t page_y = (rec.packed_page >> 16u) & 0xFFu;
				const uint32_t bp_min_x = (t_min_x >> mip) >> PAGE_SIZE_SHIFT;
				const uint32_t bp_max_x = (t_max_x >> mip) >> PAGE_SIZE_SHIFT;
				const uint32_t bp_min_y = (t_min_y >> mip) >> PAGE_SIZE_SHIFT;
				const uint32_t bp_max_y = (t_max_y >> mip) >> PAGE_SIZE_SHIFT;
				if (page_x < bp_min_x || page_x > bp_max_x || page_y < bp_min_y || page_y > bp_max_y) {
					continue;
				}
				const float mip_texel = (md.texel_scale * closest_w) * float(1u << mip);
				if (p_min_texels > 0.0f && box_extent < p_min_texels * mip_texel) {
					r_small_culled++;
					continue;
				}
				r_dirty_indices.push_back(d);
			}
		}
	}
}

/* PRODUCT SURFACE (#221) */

// GodotNext: vsm_settings_debug (#221, plan §21.3.2 S1, budget V6).

static_assert(RendererVirtualShadowMapStorage::SHADOW_METHOD_ATLAS == 0 && RendererVirtualShadowMapStorage::SHADOW_METHOD_VIRTUAL == 1, "The project setting stores the index: 0 atlas, 1 virtual.");
static_assert(RSE::LIGHT_SHADOW_METHOD_MAX <= 32, "The warn-once mask holds every method.");

const char *const RendererVirtualShadowMapStorage::SETTING_QUALITY = "rendering/lights_and_shadows/virtual_shadows/quality";
const char *const RendererVirtualShadowMapStorage::SETTING_MAX_PHYSICAL_PAGES = "rendering/lights_and_shadows/virtual_shadows/max_physical_pages";
const char *const RendererVirtualShadowMapStorage::SETTING_LOD_BIAS_DIRECTIONAL = "rendering/lights_and_shadows/virtual_shadows/resolution_lod_bias_directional";
const char *const RendererVirtualShadowMapStorage::SETTING_LOD_BIAS_LOCAL = "rendering/lights_and_shadows/virtual_shadows/resolution_lod_bias_local";
const char *const RendererVirtualShadowMapStorage::SETTING_LOD_BIAS_DIRECTIONAL_MOVING = "rendering/lights_and_shadows/virtual_shadows/resolution_lod_bias_directional_moving";
const char *const RendererVirtualShadowMapStorage::SETTING_LOD_BIAS_LOCAL_MOVING = "rendering/lights_and_shadows/virtual_shadows/resolution_lod_bias_local_moving";

namespace {

// Process-wide state of the product surface. The Forward+ storage writes it once at startup; the
// light storages, the scene cull, Light3D's warnings and get_rendering_info() read it on any thread.
SafeNumeric<uint32_t> vsm_tier_reason(RendererVirtualShadowMapStorage::TIER_REASON_UNSUPPORTED_RENDERER);
SafeNumeric<uint32_t> vsm_project_method(RendererVirtualShadowMapStorage::DEFAULT_SHADOW_METHOD);
SafeFlag vsm_dev_switch;
SafeFlag vsm_state_set; // A Forward+ storage called set_startup_state().
SafeFlag vsm_fallback_reported;
SafeNumeric<uint32_t> vsm_fallback_lines;
SafeNumeric<uint32_t> vsm_unimplemented_warned; // One bit per RSE::LightShadowMethod.
SafeNumeric<uint32_t> vsm_unimplemented_warnings;
SafeNumeric<uint64_t> vsm_frame_stats[RendererVirtualShadowMapStorage::FRAME_STAT_MAX];

} // namespace

RendererVirtualShadowMapStorage::TierReason RendererVirtualShadowMapStorage::evaluate_tier(const TierInputs &p_inputs) {
	if (!p_inputs.forward_plus) {
		return TIER_REASON_UNSUPPORTED_RENDERER;
	}
	if (!p_inputs.image_atomic_32) {
		return TIER_REASON_NO_IMAGE_ATOMIC_32;
	}
	if (!p_inputs.indirect_count) {
		return TIER_REASON_NO_INDIRECT_COUNT;
	}
	return TIER_REASON_NONE;
}

const char *RendererVirtualShadowMapStorage::get_tier_reason_name(TierReason p_reason) {
	switch (p_reason) {
		case TIER_REASON_NONE:
			return "NONE";
		case TIER_REASON_UNSUPPORTED_RENDERER:
			return "UNSUPPORTED_RENDERER";
		case TIER_REASON_NO_IMAGE_ATOMIC_32:
			return "NO_IMAGE_ATOMIC_32";
		case TIER_REASON_NO_INDIRECT_COUNT:
			return "NO_INDIRECT_COUNT";
		default:
			return "INVALID";
	}
}

String RendererVirtualShadowMapStorage::format_fallback_line(TierReason p_reason) {
	return vformat("VSM: virtual shadow maps are not available (reason=%s); lights with the virtual shadow method use the shadow atlas.", get_tier_reason_name(p_reason));
}

bool RendererVirtualShadowMapStorage::is_light_shadow_method_implemented(RSE::LightShadowMethod p_method) {
	return p_method == RSE::LIGHT_SHADOW_METHOD_PROJECT || p_method == RSE::LIGHT_SHADOW_METHOD_VIRTUAL || p_method == RSE::LIGHT_SHADOW_METHOD_ATLAS;
}

const char *RendererVirtualShadowMapStorage::get_light_shadow_method_name(RSE::LightShadowMethod p_method) {
	switch (p_method) {
		case RSE::LIGHT_SHADOW_METHOD_PROJECT:
			return "PROJECT";
		case RSE::LIGHT_SHADOW_METHOD_VIRTUAL:
			return "VIRTUAL";
		case RSE::LIGHT_SHADOW_METHOD_ATLAS:
			return "ATLAS";
		case RSE::LIGHT_SHADOW_METHOD_RAY_TRACED:
			return "RAY_TRACED";
		case RSE::LIGHT_SHADOW_METHOD_DISTANCE_FIELD:
			return "DISTANCE_FIELD";
		default:
			return "INVALID";
	}
}

bool RendererVirtualShadowMapStorage::light_shadow_method_wants_virtual(RSE::LightShadowMethod p_method, ShadowMethod p_project) {
	if (p_method == RSE::LIGHT_SHADOW_METHOD_VIRTUAL) {
		return true;
	}
	if (p_method == RSE::LIGHT_SHADOW_METHOD_ATLAS) {
		return false;
	}
	// PROJECT, and the methods that are not implemented yet (they resolve to PROJECT).
	return p_project == SHADOW_METHOD_VIRTUAL;
}

RSE::LightShadowMethod RendererVirtualShadowMapStorage::resolve_light_shadow_method(RSE::LightShadowMethod p_method, ShadowMethod p_project, bool p_vsm_available) {
	return (p_vsm_available && light_shadow_method_wants_virtual(p_method, p_project)) ? RSE::LIGHT_SHADOW_METHOD_VIRTUAL : RSE::LIGHT_SHADOW_METHOD_ATLAS;
}

void RendererVirtualShadowMapStorage::set_startup_state(TierReason p_reason, ShadowMethod p_project, bool p_dev_switch) {
	vsm_tier_reason.set(uint32_t(p_reason));
	vsm_project_method.set(uint32_t(p_project));
	vsm_dev_switch.set_to(p_dev_switch);
	vsm_state_set.set();
}

void RendererVirtualShadowMapStorage::reset_startup_state() {
	set_startup_state(TIER_REASON_UNSUPPORTED_RENDERER, DEFAULT_SHADOW_METHOD, false);
	vsm_state_set.clear();
	// A renderer created after this one reports its own fallback line and warnings once again.
	vsm_fallback_reported.clear();
	vsm_unimplemented_warned.set(0);
	reset_frame_stats();
}

RendererVirtualShadowMapStorage::TierReason RendererVirtualShadowMapStorage::get_tier_reason() {
	return TierReason(vsm_tier_reason.get());
}

RendererVirtualShadowMapStorage::ShadowMethod RendererVirtualShadowMapStorage::get_project_shadow_method() {
	return ShadowMethod(vsm_project_method.get());
}

bool RendererVirtualShadowMapStorage::is_dev_switch_forcing() {
	return vsm_dev_switch.is_set();
}

bool RendererVirtualShadowMapStorage::light_uses_vsm(RSE::LightShadowMethod p_method) {
	return resolve_light_shadow_method(p_method, get_project_shadow_method(), is_vsm_available()) == RSE::LIGHT_SHADOW_METHOD_VIRTUAL;
}

bool RendererVirtualShadowMapStorage::report_fallback() {
	const TierReason reason = get_tier_reason();
	if (reason == TIER_REASON_NONE || !vsm_fallback_reported.set_if_clear()) {
		return false;
	}
	vsm_fallback_lines.increment();
	print_line(format_fallback_line(reason));
	return true;
}

void RendererVirtualShadowMapStorage::note_light_shadow_method(RSE::LightShadowMethod p_method) {
	if (uint32_t(p_method) >= RSE::LIGHT_SHADOW_METHOD_MAX) {
		return;
	}
	if (!is_light_shadow_method_implemented(p_method)) {
		const uint32_t bit = 1u << uint32_t(p_method);
		if ((vsm_unimplemented_warned.bit_or(bit) & bit) == 0) {
			vsm_unimplemented_warnings.increment();
			WARN_PRINT(vformat("The %s light shadow method is not implemented yet; lights with it use the project's shadow method (rendering/lights_and_shadows/shadow_method).", get_light_shadow_method_name(p_method)));
		}
	}
	if (light_shadow_method_wants_virtual(p_method, get_project_shadow_method())) {
		report_fallback();
	}
}

void RendererVirtualShadowMapStorage::report_project_shadow_method(bool p_print) {
	if (!vsm_state_set.is_set()) {
		// No Forward+ storage (Mobile, Compatibility, the dummy renderer): the tier reason stays
		// UNSUPPORTED_RENDERER, and the project's method still decides whether lights wanted VSM.
		vsm_project_method.set(uint32_t(read_project_shadow_method()));
	}
	if (p_print && get_project_shadow_method() == SHADOW_METHOD_VIRTUAL) {
		report_fallback();
	}
}

uint32_t RendererVirtualShadowMapStorage::get_unimplemented_warning_count() {
	return vsm_unimplemented_warnings.get();
}

uint32_t RendererVirtualShadowMapStorage::get_fallback_line_count() {
	return vsm_fallback_lines.get();
}

RendererVirtualShadowMapStorage::ShadowMethod RendererVirtualShadowMapStorage::read_project_shadow_method() {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	const StringName name = "rendering/lights_and_shadows/shadow_method";
	if (settings == nullptr || !settings->has_setting(name)) {
		return DEFAULT_SHADOW_METHOD;
	}
	const int64_t value = settings->get_setting_with_override(name);
	return value == int64_t(SHADOW_METHOD_VIRTUAL) ? SHADOW_METHOD_VIRTUAL : SHADOW_METHOD_ATLAS;
}

RendererVirtualShadowMapStorage::Scalability RendererVirtualShadowMapStorage::default_scalability(PoolQuality p_quality) {
	Scalability values;
	values.max_physical_pages = pool_quality_pages(p_quality < POOL_QUALITY_MAX ? p_quality : DEFAULT_QUALITY);
	values.lod_bias_directional = p_quality == POOL_QUALITY_LOW ? 1 : 0;
	values.lod_bias_local = values.lod_bias_directional;
	values.lod_bias_directional_moving = values.lod_bias_directional + 1;
	values.lod_bias_local_moving = values.lod_bias_local + 1;
	return values;
}

RendererVirtualShadowMapStorage::Scalability RendererVirtualShadowMapStorage::clamp_scalability(const Scalability &p_values) {
	Scalability values = p_values;
	values.max_physical_pages = CLAMP(values.max_physical_pages, MIN_PHYSICAL_PAGES, POOL_MAX_PAGES);
	values.lod_bias_directional = CLAMP(values.lod_bias_directional, -MAX_SETTING_LOD_BIAS, MAX_SETTING_LOD_BIAS);
	values.lod_bias_local = CLAMP(values.lod_bias_local, -MAX_SETTING_LOD_BIAS, MAX_SETTING_LOD_BIAS);
	values.lod_bias_directional_moving = CLAMP(values.lod_bias_directional_moving, -MAX_SETTING_LOD_BIAS, MAX_SETTING_LOD_BIAS);
	values.lod_bias_local_moving = CLAMP(values.lod_bias_local_moving, -MAX_SETTING_LOD_BIAS, MAX_SETTING_LOD_BIAS);
	return values;
}

static int64_t _read_preset_setting(const char *p_setting, RendererVirtualShadowMapStorage::PoolQuality p_quality, int64_t p_default) {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (settings == nullptr) {
		return p_default;
	}
	const StringName name = String(p_setting) + "." + RendererVirtualShadowMapStorage::pool_quality_name(p_quality);
	if (!settings->has_setting(name)) {
		return p_default;
	}
	return settings->get_setting(name);
}

RendererVirtualShadowMapStorage::Scalability RendererVirtualShadowMapStorage::read_scalability(PoolQuality p_quality) {
	const Scalability defaults = default_scalability(p_quality);
	Scalability values;
	values.max_physical_pages = uint32_t(CLAMP(_read_preset_setting(SETTING_MAX_PHYSICAL_PAGES, p_quality, defaults.max_physical_pages), int64_t(0), int64_t(UINT32_MAX)));
	values.lod_bias_directional = int32_t(CLAMP(_read_preset_setting(SETTING_LOD_BIAS_DIRECTIONAL, p_quality, defaults.lod_bias_directional), int64_t(INT32_MIN), int64_t(INT32_MAX)));
	values.lod_bias_local = int32_t(CLAMP(_read_preset_setting(SETTING_LOD_BIAS_LOCAL, p_quality, defaults.lod_bias_local), int64_t(INT32_MIN), int64_t(INT32_MAX)));
	values.lod_bias_directional_moving = int32_t(CLAMP(_read_preset_setting(SETTING_LOD_BIAS_DIRECTIONAL_MOVING, p_quality, defaults.lod_bias_directional_moving), int64_t(INT32_MIN), int64_t(INT32_MAX)));
	values.lod_bias_local_moving = int32_t(CLAMP(_read_preset_setting(SETTING_LOD_BIAS_LOCAL_MOVING, p_quality, defaults.lod_bias_local_moving), int64_t(INT32_MIN), int64_t(INT32_MAX)));
	return clamp_scalability(values);
}

RendererVirtualShadowMapStorage::PoolQuality RendererVirtualShadowMapStorage::read_quality() {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	const StringName name = SETTING_QUALITY;
	if (settings == nullptr || !settings->has_setting(name)) {
		return DEFAULT_QUALITY;
	}
	const int64_t value = settings->get_setting_with_override(name);
	return (value >= 0 && value < int64_t(POOL_QUALITY_MAX)) ? PoolQuality(value) : DEFAULT_QUALITY;
}

int32_t RendererVirtualShadowMapStorage::moving_lod_bias(int32_t p_static_bias, int32_t p_moving_bias, uint64_t p_frames_since_moved) {
	const int32_t delta = p_moving_bias - p_static_bias;
	if (delta == 0 || p_frames_since_moved == NEVER_MOVED) {
		return 0;
	}
	const uint64_t steps = p_frames_since_moved / MOVING_DECAY_FRAMES;
	const int32_t magnitude = delta > 0 ? delta : -delta;
	if (steps >= uint64_t(magnitude)) {
		return 0;
	}
	return delta > 0 ? delta - int32_t(steps) : delta + int32_t(steps);
}

uint64_t RendererVirtualShadowMapStorage::note_light_motion(RID p_light_instance, const Transform3D &p_transform) {
	LightMaps *maps = light_maps.getptr(p_light_instance);
	if (maps == nullptr) {
		return NEVER_MOVED;
	}
	if (!maps->motion_valid) {
		maps->motion_transform = p_transform;
		maps->motion_valid = true;
	} else if (maps->motion_transform != p_transform) {
		maps->motion_transform = p_transform;
		maps->moved_frame = frame;
	}
	if (maps->moved_frame == NEVER_MOVED) {
		return NEVER_MOVED;
	}
	return frame >= maps->moved_frame ? frame - maps->moved_frame : 0;
}

uint64_t RendererVirtualShadowMapStorage::get_frame_stat(FrameStat p_stat) {
	ERR_FAIL_UNSIGNED_INDEX_V(p_stat, FRAME_STAT_MAX, 0);
	return vsm_frame_stats[p_stat].get();
}

void RendererVirtualShadowMapStorage::publish_frame_stats(const uint32_t *p_view_states, uint32_t p_view_count) {
	if (p_view_states == nullptr || p_view_count == 0) {
		return;
	}
	uint64_t requested = 0;
	uint64_t rerendered = 0;
	uint64_t overflow = 0;
	for (uint32_t view = 0; view < p_view_count; view++) {
		const uint32_t *state = p_view_states + uint64_t(view) * STATE_VIEW_END;
		requested += state[STATE_REQUESTS];
		rerendered += state[STATE_DIRTY];
		overflow += state[STATE_OVERFLOW];
	}
	// The pool after the frame's last view: pages kept for, given to or cached for an entry. The update pass
	// counts a cached page before the assign pass may give it to a new request (STATE_STOLEN).
	const uint32_t *last = p_view_states + uint64_t(p_view_count - 1) * STATE_VIEW_END;
	const uint64_t held = uint64_t(last[STATE_KEPT]) + uint64_t(last[STATE_NEW]) + uint64_t(last[STATE_CACHED]);
	const uint64_t stolen = MIN(uint64_t(last[STATE_STOLEN]), uint64_t(last[STATE_CACHED]));
	vsm_frame_stats[FRAME_STAT_ALLOCATED].set(held - stolen);
	vsm_frame_stats[FRAME_STAT_REQUESTED].set(requested);
	vsm_frame_stats[FRAME_STAT_RERENDERED].set(rerendered);
	vsm_frame_stats[FRAME_STAT_OVERFLOW].set(overflow);
}

void RendererVirtualShadowMapStorage::reset_frame_stats() {
	for (uint32_t i = 0; i < FRAME_STAT_MAX; i++) {
		vsm_frame_stats[i].set(0);
	}
}

/* PROJECTION AND SMRT SOFT SHADOWS (#222) */

// GodotNext: vsm_projection (#222, plan §21.3 contract S6). CPU models of vsm_smrt_inc.glsl.

static_assert(RendererVirtualShadowMapStorage::smrt_preset_rays(RendererVirtualShadowMapStorage::POOL_QUALITY_LOW) * RendererVirtualShadowMapStorage::smrt_preset_steps(RendererVirtualShadowMapStorage::POOL_QUALITY_LOW) == 8, "Low: Godot's Soft Very Low PCSS reads 4 + 4 texels.");
static_assert(RendererVirtualShadowMapStorage::smrt_preset_rays(RendererVirtualShadowMapStorage::POOL_QUALITY_MEDIUM) * RendererVirtualShadowMapStorage::smrt_preset_steps(RendererVirtualShadowMapStorage::POOL_QUALITY_MEDIUM) == 16, "Medium: Godot's Soft Low PCSS reads 8 + 8 texels.");
static_assert(RendererVirtualShadowMapStorage::smrt_preset_rays(RendererVirtualShadowMapStorage::POOL_QUALITY_HIGH) * RendererVirtualShadowMapStorage::smrt_preset_steps(RendererVirtualShadowMapStorage::POOL_QUALITY_HIGH) == 24, "High: Godot's Soft Medium PCSS reads 12 + 12 texels.");
static_assert(RendererVirtualShadowMapStorage::smrt_preset_rays(RendererVirtualShadowMapStorage::POOL_QUALITY_EPIC) * RendererVirtualShadowMapStorage::smrt_preset_steps(RendererVirtualShadowMapStorage::POOL_QUALITY_EPIC) == 48, "Epic: Godot's Soft High PCSS reads 24 + 24 texels.");
static_assert(RendererVirtualShadowMapStorage::SMRT_MAX_TEXELS <= float(RendererVirtualShadowMapStorage::PAGE_SIZE), "A ray reads at most the neighboring page.");

Vector2 RendererVirtualShadowMapStorage::smrt_disk(uint32_t p_ray, uint32_t p_rays, const Vector2 &p_noise) {
	const float count = float(MAX(p_rays, 1u));
	const float radius = std::sqrt(MIN((float(p_ray) + float(p_noise.x)) / count, 1.0f));
	float turn = float(p_ray) * 0.618034f + float(p_noise.y);
	turn = turn - std::floor(turn);
	const float angle = 6.2831853f * turn;
	return Vector2(std::cos(angle) * radius, std::sin(angle) * radius);
}

Vector2 RendererVirtualShadowMapStorage::smrt_square(uint32_t p_ray, uint32_t p_rays, const Vector2 &p_noise) {
	const float count = float(MAX(p_rays, 1u));
	float u = (float(p_ray) + float(p_noise.x)) / count;
	u = u - std::floor(u);
	float v = float(p_ray) * 0.618034f + float(p_noise.y);
	v = v - std::floor(v);
	return Vector2(u - 0.5f, v - 0.5f);
}

float RendererVirtualShadowMapStorage::smrt_jitter(uint32_t p_ray, const Vector2 &p_noise) {
	float jitter = (float(p_noise.x) + float(p_noise.y)) * 0.5f + float(p_ray) * 0.754878f;
	return jitter - std::floor(jitter);
}

float RendererVirtualShadowMapStorage::smrt_step(uint32_t p_step, uint32_t p_steps, float p_jitter, float p_first, float p_last) {
	const float t = (float(p_step) + p_jitter) / float(MAX(p_steps, 1u));
	if (!(p_last > p_first) || !(p_first > 0.0f)) {
		return p_first;
	}
	return p_first * std::pow(p_last / p_first, t);
}

float RendererVirtualShadowMapStorage::directional_tan_half_angle(float p_size) {
	// size = 1 - cos(d) for the angular diameter d: tan(d / 2) = sqrt((1 - cos d) / (1 + cos d)).
	const float size = CLAMP(p_size, 0.0f, 1.0f);
	return std::sqrt(size / (2.0f - size));
}

bool RendererVirtualShadowMapStorage::clipmap_projection_select(const MapData *p_maps, uint32_t p_table_maps, const uint32_t *p_map_bias, uint32_t p_first, uint32_t p_count, const Vector2i &p_texel0, float p_footprint, int32_t p_lod_bias, uint32_t &r_level, uint32_t &r_mip) {
	// The same choice as mark_directional_point()'s fine request, with the light's moving bias (#221) held by
	// level 0.
	const int32_t level_mip = clipmap_level_mip(p_footprint, p_lod_bias + (p_first < p_table_maps ? p_maps[p_first].lod_bias : 0));
	for (uint32_t level = 0; level < p_count; level++) {
		const uint32_t map = p_first + level;
		if (map >= p_table_maps) {
			break;
		}
		const Vector2i origin(p_maps[map].origin[0], p_maps[map].origin[1]);
		const Vector2i level_texel = clipmap_level_texel(p_texel0, level);
		if (!clipmap_in_window(origin, level_texel, 0)) {
			continue;
		}
		uint32_t mip = biased_mip(level_mip - int32_t(level), 0);
		if (p_map_bias != nullptr) {
			mip = MIN(mip + overflow_bias(p_map_bias[map]), MIP_COUNT - 1);
		}
		if (level + 1 == p_count) {
			while (mip > 0 && !clipmap_in_window(origin, level_texel, mip)) {
				mip--;
			}
		} else if (!clipmap_in_window(origin, level_texel, mip)) {
			continue;
		}
		r_level = level;
		r_mip = mip;
		return true;
	}
	return false;
}

RendererVirtualShadowMapStorage::LookupResult RendererVirtualShadowMapStorage::clipmap_sample_level(const uint32_t *p_table, uint32_t p_table_maps, const MapData *p_maps, uint32_t p_first, uint32_t p_count, const Vector2i &p_texel0, uint32_t p_level, uint32_t p_mip, uint32_t &r_level) {
	for (uint32_t level = p_level; level < p_count; level++) {
		const uint32_t map = p_first + level;
		if (map >= p_table_maps) {
			break;
		}
		const uint32_t mip = uint32_t(MAX(int32_t(p_mip) - int32_t(level - p_level), 0));
		const Vector2i origin(p_maps[map].origin[0], p_maps[map].origin[1]);
		const LookupResult result = lookup_clipmap(p_table, p_table_maps, map, origin, clipmap_level_texel(p_texel0, level), mip, true);
		if (result.valid) {
			r_level = level;
			return result;
		}
	}
	return LookupResult();
}

uint32_t RendererVirtualShadowMapStorage::face_projection_select(const MapData *p_maps, uint32_t p_table_maps, uint32_t p_map_base, uint32_t p_light_flags, const Vector3 &p_world, float &r_w) {
	const uint32_t count = (p_light_flags >> LIGHT_FLAGS_COUNT_SHIFT) & LIGHT_FLAGS_COUNT_MASK;
	uint32_t best = MAP_NONE;
	r_w = 0.0f;
	for (uint32_t face = 0; face < count; face++) {
		const uint32_t map = light_map(p_map_base, p_light_flags, face);
		if (map >= p_table_maps || p_maps[map].kind == MAP_KIND_MAX || p_maps[map].kind == MAP_KIND_DIRECTIONAL) {
			continue;
		}
		float w = 0.0f;
		uint32_t texel_x = 0;
		uint32_t texel_y = 0;
		if (face_texel(p_maps[map], p_world, w, texel_x, texel_y) && (best == MAP_NONE || w > r_w)) {
			best = map;
			r_w = w;
		}
	}
	return best;
}

float RendererVirtualShadowMapStorage::pool_depth(const ProjectionInput &p_in, uint32_t p_texel_x, uint32_t p_texel_y) {
	if (p_in.pool == nullptr || p_texel_x >= p_in.pool_width || p_texel_y >= p_in.pool_height) {
		return float(Math::INF);
	}
	const uint64_t layer = uint64_t(p_in.pool_width) * p_in.pool_height;
	const uint64_t index = uint64_t(p_texel_y) * p_in.pool_width + p_texel_x;
	return depth_unpack(MAX(p_in.pool[index], p_in.pool[layer + index]));
}

// A clipmap sample of SMRT: whether level 0 texel position p_t (floating texels) lies behind the stored
// depth by more than the bias of the texel it reads. Unmapped (after the coarser mips and levels) is lit.
static bool _smrt_clipmap_occluded(const RendererVirtualShadowMapStorage::ProjectionInput &p_in, uint32_t p_first, uint32_t p_count, const Vector2 &p_t, float p_depth, uint32_t p_level, uint32_t p_mip) {
	using VSM = RendererVirtualShadowMapStorage;
	const Vector2i texel0(int32_t(CLAMP(std::floor(float(p_t.x)), -1073741824.0f, 1073741824.0f)), int32_t(CLAMP(std::floor(float(p_t.y)), -1073741824.0f, 1073741824.0f)));
	uint32_t level = p_level;
	const VSM::LookupResult result = VSM::clipmap_sample_level(p_in.table, p_in.table_maps, p_in.maps, p_first, p_count, texel0, p_level, p_mip, level);
	if (!result.valid) {
		return false;
	}
	const float texel = std::ldexp(1.0f, int32_t(level + result.mip) + VSM::CLIPMAP_TEXEL0_EXPONENT);
	const float stored = VSM::pool_depth(p_in, result.texel_x, result.texel_y);
	return stored < p_depth - texel * VSM::SMRT_DEPTH_BIAS_TEXELS;
}

float RendererVirtualShadowMapStorage::shadow_directional(const ProjectionInput &p_in, uint32_t p_map_base, uint32_t p_light_flags, const Vector3 &p_world, const Vector3 &p_normal, float p_tan_half_angle) {
	const uint32_t count = (p_light_flags >> LIGHT_FLAGS_COUNT_SHIFT) & LIGHT_FLAGS_COUNT_MASK;
	const uint32_t first = light_map(p_map_base, p_light_flags, 0);
	if (p_in.table == nullptr || p_in.maps == nullptr || first >= p_in.table_maps || p_in.maps[first].kind != MAP_KIND_DIRECTIONAL) {
		return 1.0f;
	}
	const MapData &level0 = p_in.maps[first];
	uint32_t level = 0;
	uint32_t mip = 0;
	if (!clipmap_projection_select(p_in.maps, p_in.table_maps, p_in.map_bias, first, count, clipmap_texel0(level0, p_world), p_in.footprint, p_in.lod_bias, level, mip)) {
		return 1.0f;
	}
	const float texel = std::ldexp(1.0f, int32_t(level + mip) + CLIPMAP_TEXEL0_EXPONENT);
	// Row 2 is minus the light's Z axis: the light shines along it, and the light lies along -row 2.
	const Vector3 toward(-level0.rows[2][0], -level0.rows[2][1], -level0.rows[2][2]);
	const float n_dot_l = CLAMP(float(p_normal.dot(toward)), -1.0f, 1.0f);
	const float offset = texel * SMRT_NORMAL_BIAS_TEXELS * std::sqrt(MAX(1.0f - n_dot_l * n_dot_l, 0.0f));
	const Vector3 receiver = p_world + p_normal * offset;
	const Vector2 t(row_dot(level0.rows[0], receiver), row_dot(level0.rows[1], receiver));
	const float depth = row_dot(level0.rows[2], receiver);
	// Level 0 texels per meter across the light.
	const float texels_per_meter = std::ldexp(1.0f, -CLIPMAP_TEXEL0_EXPONENT);

	if (p_in.prefiltered_distant && prefiltered_distant_level(level, count)) {
		return prefiltered_distant(p_in, first, count, t, depth, level, mip);
	}
	if (p_in.rays == 0 || !(p_tan_half_angle > 0.0f)) {
		// One 2 x 2 filtered tap on the sampled level and mip.
		const float scale = std::ldexp(1.0f, int32_t(level + mip));
		const Vector2 grid = t / scale - Vector2(0.5f, 0.5f);
		const Vector2 base(std::floor(float(grid.x)), std::floor(float(grid.y)));
		const Vector2 f = grid - base;
		float lit = 0.0f;
		for (uint32_t corner = 0; corner < 4; corner++) {
			const Vector2 offset_c(float(corner & 1u), float(corner >> 1u));
			const Vector2 sample_t = (base + offset_c + Vector2(0.5f, 0.5f)) * scale;
			const float weight = ((corner & 1u) != 0u ? float(f.x) : 1.0f - float(f.x)) * ((corner >> 1u) != 0u ? float(f.y) : 1.0f - float(f.y));
			lit += _smrt_clipmap_occluded(p_in, first, count, sample_t, depth, level, mip) ? 0.0f : weight;
		}
		return lit;
	}

	const uint32_t rays = MIN(p_in.rays, SMRT_MAX_RAYS);
	const uint32_t steps = CLAMP(p_in.steps, 1u, SMRT_MAX_STEPS);
	const float first_height = texel * SMRT_FIRST_STEP_TEXELS;
	const float level_extent = CLIPMAP_LEVEL0_EXTENT * std::ldexp(1.0f, int32_t(level));
	uint32_t lit = 0;
	for (uint32_t ray = 0; ray < rays; ray++) {
		// The ray's slope across the light: a point of the light's disk, in tangent units.
		const Vector2 slope = smrt_disk(ray, rays, p_in.noise) * p_tan_half_angle;
		const float slope_length = float(slope.length());
		float last_height = level_extent;
		if (slope_length > 0.0f) {
			last_height = MIN(SMRT_MAX_TEXELS * texel / slope_length, level_extent);
		}
		last_height = MAX(last_height, first_height);
		const float jitter = smrt_jitter(ray, p_in.noise);
		bool occluded = false;
		for (uint32_t step = 0; step < steps && !occluded; step++) {
			const float height = smrt_step(step, steps, jitter, first_height, last_height);
			const Vector2 sample_t = t + slope * (height * texels_per_meter);
			occluded = _smrt_clipmap_occluded(p_in, first, count, sample_t, depth - height, level, mip);
		}
		lit += occluded ? 0u : 1u;
	}
	return float(lit) / float(rays);
}

float RendererVirtualShadowMapStorage::prefiltered_distant(const ProjectionInput &p_in, uint32_t p_first, uint32_t p_count, const Vector2 &p_t, float p_depth, uint32_t p_level, uint32_t p_mip) {
	const float scale = std::ldexp(1.0f, int32_t(p_level + p_mip));
	const float texel = std::ldexp(1.0f, int32_t(p_level + p_mip) + CLIPMAP_TEXEL0_EXPONENT);
	const Vector2 grid = p_t / scale - Vector2(0.5f, 0.5f);
	const float half_block = float(PREFILTERED_DISTANT_TAPS / 2 - 1);
	const Vector2 base(std::floor(float(grid.x)) - half_block, std::floor(float(grid.y)) - half_block);
	float sum = 0.0f;
	float sum_squares = 0.0f;
	uint32_t stored_count = 0;
	for (uint32_t y = 0; y < PREFILTERED_DISTANT_TAPS; y++) {
		for (uint32_t x = 0; x < PREFILTERED_DISTANT_TAPS; x++) {
			const Vector2 sample_t = (base + Vector2(float(x) + 0.5f, float(y) + 0.5f)) * scale;
			const Vector2i texel0(int32_t(CLAMP(std::floor(float(sample_t.x)), -1073741824.0f, 1073741824.0f)), int32_t(CLAMP(std::floor(float(sample_t.y)), -1073741824.0f, 1073741824.0f)));
			uint32_t found_level = p_level;
			const LookupResult result = clipmap_sample_level(p_in.table, p_in.table_maps, p_in.maps, p_first, p_count, texel0, p_level, p_mip, found_level);
			if (!result.valid) {
				continue;
			}
			const float stored = pool_depth(p_in, result.texel_x, result.texel_y);
			if (!(stored < 1.0e30f)) {
				continue;
			}
			// Center around p_depth before squaring to avoid float32 cancellation on far clipmap levels.
			const float delta = stored - p_depth;
			sum += delta;
			sum_squares += delta * delta;
			stored_count++;
		}
	}
	if (stored_count == 0) {
		return 1.0f;
	}
	const float coverage = float(stored_count) / float(PREFILTERED_DISTANT_TAPS * PREFILTERED_DISTANT_TAPS);
	const float mean_delta = sum / float(stored_count);
	const float min_variance = texel * PREFILTERED_DISTANT_MIN_VARIANCE_TEXELS * texel * PREFILTERED_DISTANT_MIN_VARIANCE_TEXELS;
	const float variance = MAX(sum_squares / float(stored_count) - mean_delta * mean_delta, min_variance);
	const float d = -texel * SMRT_DEPTH_BIAS_TEXELS - mean_delta;
	float lit = 1.0f;
	if (d > 0.0f) {
		lit = variance / (variance + d * d);
		lit = CLAMP((lit - PREFILTERED_DISTANT_BLEED) / (1.0f - PREFILTERED_DISTANT_BLEED), 0.0f, 1.0f);
	}
	return (1.0f - coverage) + coverage * lit;
}

// The face of a local light's sample point: the face p_map when it holds the point, else (omni and area)
// the face with the largest w. MAP_NONE outside every face.
static uint32_t _smrt_face(const RendererVirtualShadowMapStorage::ProjectionInput &p_in, uint32_t p_map_base, uint32_t p_light_flags, uint32_t p_map, const Vector3 &p_point, float &r_x, float &r_y, float &r_w) {
	using VSM = RendererVirtualShadowMapStorage;
	const VSM::MapData &data = p_in.maps[p_map];
	r_x = VSM::row_dot(data.rows[0], p_point);
	r_y = VSM::row_dot(data.rows[1], p_point);
	r_w = VSM::row_dot(data.rows[3], p_point);
	if (r_w > data.near_w && std::fabs(r_x) <= r_w && std::fabs(r_y) <= r_w) {
		return p_map;
	}
	if (VSM::MapKind(p_light_flags & VSM::LIGHT_FLAGS_KIND_MASK) == VSM::MAP_KIND_SPOT) {
		return VSM::MAP_NONE;
	}
	float w = 0.0f;
	const uint32_t map = VSM::face_projection_select(p_in.maps, p_in.table_maps, p_map_base, p_light_flags, p_point, w);
	if (map != VSM::MAP_NONE) {
		const VSM::MapData &other = p_in.maps[map];
		r_x = VSM::row_dot(other.rows[0], p_point);
		r_y = VSM::row_dot(other.rows[1], p_point);
		r_w = w;
	}
	return map;
}

// A face sample of SMRT: whether p_point (w = its depth) lies behind the stored depth. Unmapped is lit.
static bool _smrt_face_occluded(const RendererVirtualShadowMapStorage::ProjectionInput &p_in, uint32_t p_map, float p_x, float p_y, float p_w, uint32_t p_mip) {
	using VSM = RendererVirtualShadowMapStorage;
	const Vector2 uv(p_x / p_w * 0.5f + 0.5f, p_y / p_w * 0.5f + 0.5f);
	const VSM::LookupResult result = VSM::lookup(p_in.table, p_in.table_maps, p_map, uv, p_mip, true);
	if (!result.valid) {
		return false;
	}
	const float texel = p_in.maps[p_map].texel_scale * p_w * float(1u << result.mip);
	const float stored = VSM::pool_depth(p_in, result.texel_x, result.texel_y);
	return stored < p_w - texel * VSM::SMRT_DEPTH_BIAS_TEXELS;
}

float RendererVirtualShadowMapStorage::shadow_local(const ProjectionInput &p_in, uint32_t p_map_base, uint32_t p_light_flags, const Vector3 &p_world, const Vector3 &p_normal, const Vector3 &p_light_position, float p_radius, const Vector3 &p_area_a, const Vector3 &p_area_b) {
	if (p_in.table == nullptr || p_in.maps == nullptr) {
		return 1.0f;
	}
	float w = 0.0f;
	const uint32_t map = face_projection_select(p_in.maps, p_in.table_maps, p_map_base, p_light_flags, p_world, w);
	if (map == MAP_NONE) {
		return 1.0f;
	}
	const MapData &face = p_in.maps[map];
	// GodotNext: vsm_settings_debug (#221). Plus the light's moving bias, as mark_local_point().
	const int32_t bias = p_in.lod_bias + face.lod_bias + (p_in.map_bias != nullptr ? int32_t(overflow_bias(p_in.map_bias[map])) : 0);
	const uint32_t mip = biased_mip(int32_t(face_mip(w * face.texel_scale, p_in.footprint)), bias);
	const float texel = face.texel_scale * w * float(1u << mip);
	const Vector3 to_light = p_light_position - p_world;
	const float distance = float(to_light.length());
	if (!(distance > 0.0f)) {
		return 1.0f;
	}
	const Vector3 light_dir = to_light / distance;
	const float n_dot_l = CLAMP(float(p_normal.dot(light_dir)), -1.0f, 1.0f);
	const float offset = texel * SMRT_NORMAL_BIAS_TEXELS * std::sqrt(MAX(1.0f - n_dot_l * n_dot_l, 0.0f));
	const Vector3 receiver = p_world + p_normal * offset;
	const bool area = p_area_a.length_squared() > 0.0f && p_area_b.length_squared() > 0.0f;

	float x = 0.0f;
	float y = 0.0f;
	float rw = 0.0f;
	const uint32_t receiver_map = _smrt_face(p_in, p_map_base, p_light_flags, map, receiver, x, y, rw);
	if (receiver_map == MAP_NONE) {
		return 1.0f;
	}
	if (p_in.rays == 0 || (!area && !(p_radius > 0.0f))) {
		// One 2 x 2 filtered tap at the receiver's mip.
		const float mip_size = float(VIRTUAL_SIZE >> mip);
		const Vector2 grid = Vector2(x / rw * 0.5f + 0.5f, y / rw * 0.5f + 0.5f) * mip_size - Vector2(0.5f, 0.5f);
		const Vector2 base(std::floor(float(grid.x)), std::floor(float(grid.y)));
		const Vector2 f = grid - base;
		float lit = 0.0f;
		for (uint32_t corner = 0; corner < 4; corner++) {
			const Vector2 uv = (base + Vector2(float(corner & 1u), float(corner >> 1u)) + Vector2(0.5f, 0.5f)) / mip_size;
			const float weight = ((corner & 1u) != 0u ? float(f.x) : 1.0f - float(f.x)) * ((corner >> 1u) != 0u ? float(f.y) : 1.0f - float(f.y));
			lit += _smrt_face_occluded(p_in, receiver_map, (uv.x * 2.0f - 1.0f) * rw, (uv.y * 2.0f - 1.0f) * rw, rw, mip) ? 0.0f : weight;
		}
		return lit;
	}

	// A frame across the direction to the light, for points of a spot or omni light's disk.
	const Vector3 up = std::fabs(float(light_dir.y)) < 0.99f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
	const Vector3 tangent = light_dir.cross(up).normalized();
	const Vector3 bitangent = light_dir.cross(tangent);
	// The angle one texel of the receiver's mip spans, seen from the light.
	const float texel_angle = face.texel_scale * float(1u << mip);
	const uint32_t rays = MIN(p_in.rays, SMRT_MAX_RAYS);
	const uint32_t steps = CLAMP(p_in.steps, 1u, SMRT_MAX_STEPS);
	uint32_t lit = 0;
	for (uint32_t ray = 0; ray < rays; ray++) {
		Vector3 light_offset;
		if (area) {
			const Vector2 square = smrt_square(ray, rays, p_in.noise);
			light_offset = p_area_a * square.x + p_area_b * square.y;
		} else {
			const Vector2 disk = smrt_disk(ray, rays, p_in.noise) * p_radius;
			light_offset = tangent * disk.x + bitangent * disk.y;
		}
		const Vector3 ray_vector = p_light_position + light_offset - receiver;
		const float ray_length = float(ray_vector.length());
		if (!(ray_length > 0.0f)) {
			lit++;
			continue;
		}
		const float first = MIN(texel * SMRT_FIRST_STEP_TEXELS / ray_length, SMRT_LOCAL_MAX_REACH);
		const float offset_length = float(light_offset.length());
		float last = SMRT_LOCAL_MAX_REACH;
		if (offset_length > 0.0f) {
			// The angle between the sample and the receiver seen from the light is about
			// s * offset / ((1 - s) * distance): it reaches SMRT_MAX_TEXELS texels at s = c / (1 + c).
			const float c = SMRT_MAX_TEXELS * texel_angle * distance / offset_length;
			last = MIN(c / (1.0f + c), SMRT_LOCAL_MAX_REACH);
		}
		last = MAX(last, first);
		const float jitter = smrt_jitter(ray, p_in.noise);
		bool occluded = false;
		for (uint32_t step = 0; step < steps && !occluded; step++) {
			const Vector3 point = receiver + ray_vector * smrt_step(step, steps, jitter, first, last);
			float px = 0.0f;
			float py = 0.0f;
			float pw = 0.0f;
			const uint32_t point_map = _smrt_face(p_in, p_map_base, p_light_flags, receiver_map, point, px, py, pw);
			if (point_map != MAP_NONE) {
				occluded = _smrt_face_occluded(p_in, point_map, px, py, pw, mip);
			}
		}
		lit += occluded ? 0u : 1u;
	}
	return float(lit) / float(rays);
}
