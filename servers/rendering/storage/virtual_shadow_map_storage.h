/**************************************************************************/
/*  virtual_shadow_map_storage.h                                          */
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

#include "core/math/basis.h"
#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"

// GodotNext: vsm_page_pool (#220, plan §21.3 contract S2, budget V6)
//
// The Virtual Shadow Map data model every other VSM task writes to:
// - the physical pool, a 2D R32_UINT array texture whose layer 0 holds static depth and layer 1
//   dynamic depth, as reversed-Z uint so that raster can use 32-bit imageAtomicMax;
// - one page-table buffer in which each virtual map owns a mip chain of 32-bit entries;
// - virtual maps per light: one per directional clipmap level, 1 per spot light, 6 per omni light
//   (cube faces) and 5 per area light (front hemisphere). A light's maps are contiguous and persist
//   per light instance, so caches survive frames.
//
// This class is the backend-independent core: the sizes, the entry format, map allocation, the
// clipmap math and the CPU model of the page lookup. The RenderingDevice side owns the GPU
// resources through the protected hooks. The sizes below are the only copies: GLSL gets them from
// get_glsl_defines().
class RendererVirtualShadowMapStorage {
public:
	// Provisional sizes. Plan §21.3.5 and #220 step 1 derive the final ones in PQ (§22) and record
	// them in Appendix A. Until then these are derived from Godot's own defaults, not copied from
	// another engine (§19); docs/godotnext/source-log/vsm.md records the derivation.
	static constexpr uint32_t PAGE_SIZE_SHIFT = 6;
	static constexpr uint32_t PAGE_SIZE = 1u << PAGE_SIZE_SHIFT; // Texels per page side.
	static constexpr uint32_t VIRTUAL_SIZE_SHIFT = 12;
	static constexpr uint32_t VIRTUAL_SIZE = 1u << VIRTUAL_SIZE_SHIFT; // Texels per map side at mip 0.
	static constexpr uint32_t PAGES_PER_SIDE_SHIFT = VIRTUAL_SIZE_SHIFT - PAGE_SIZE_SHIFT;
	static constexpr uint32_t PAGES_PER_SIDE = 1u << PAGES_PER_SIDE_SHIFT; // Pages per map side at mip 0.
	static constexpr uint32_t MIP_COUNT = PAGES_PER_SIDE_SHIFT + 1; // Down to one page.

	// Entries of mips 0 to p_mip - 1 of one map: mip m holds (PAGES_PER_SIDE >> m)^2 entries.
	static constexpr uint32_t mip_entry_offset(uint32_t p_mip) {
		return ((1u << (2 * MIP_COUNT)) - (1u << (2 * (MIP_COUNT - p_mip)))) / 3u;
	}
	// mip_entry_offset(MIP_COUNT); a constant cannot call a member function of its own class.
	static constexpr uint32_t ENTRIES_PER_MAP = ((1u << (2 * MIP_COUNT)) - 1u) / 3u;
	static constexpr uint32_t ENTRY_BYTES = 4;
	static constexpr uint64_t MAP_TABLE_BYTES = uint64_t(ENTRIES_PER_MAP) * ENTRY_BYTES;

	// Page-table entry: the physical page's x in bits 0-11, its y in bits 12-23, flags in 24-31.
	static constexpr uint32_t ENTRY_PAGE_BITS = 12;
	static constexpr uint32_t ENTRY_PAGE_MASK = (1u << ENTRY_PAGE_BITS) - 1;
	static constexpr uint32_t ENTRY_FLAGS_SHIFT = 2 * ENTRY_PAGE_BITS;
	enum EntryFlags : uint32_t {
		ENTRY_ALLOCATED = 1u << 24, // The entry maps a physical page.
		ENTRY_REQUESTED = 1u << 25, // A pixel sampled the page this frame (#219).
		ENTRY_DIRTY_STATIC = 1u << 26, // Layer 0 must be re-rendered.
		ENTRY_DIRTY_DYNAMIC = 1u << 27, // Layer 1 must be re-rendered.
		ENTRY_COARSE = 1u << 28, // Kept resident for fog and transparents (#219).
		ENTRY_UNCACHED = 1u << 29, // Rendered every frame, never cached.
		ENTRY_SCROLLED = 1u << 30, // A clipmap scroll moved the page to new ground: re-render, keep the allocation.
		ENTRY_OVERFLOW = 1u << 31, // The pool overflowed when the page was requested (#219).
		ENTRY_FLAGS_MASK = 0xFF000000u,
	};

	static constexpr uint32_t entry_pack(uint32_t p_page_x, uint32_t p_page_y, uint32_t p_flags) {
		return (p_page_x & ENTRY_PAGE_MASK) | ((p_page_y & ENTRY_PAGE_MASK) << ENTRY_PAGE_BITS) | (p_flags & ENTRY_FLAGS_MASK);
	}
	static constexpr uint32_t entry_page_x(uint32_t p_entry) { return p_entry & ENTRY_PAGE_MASK; }
	static constexpr uint32_t entry_page_y(uint32_t p_entry) { return (p_entry >> ENTRY_PAGE_BITS) & ENTRY_PAGE_MASK; }
	static constexpr uint32_t entry_flags(uint32_t p_entry) { return p_entry & ENTRY_FLAGS_MASK; }

	// The physical pool. One page holds PAGE_SIZE^2 texels in each of the two layers.
	enum PoolQuality : uint32_t {
		POOL_QUALITY_LOW,
		POOL_QUALITY_MEDIUM,
		POOL_QUALITY_HIGH,
		POOL_QUALITY_EPIC,
		POOL_QUALITY_MAX,
	};
	static constexpr uint32_t POOL_LAYERS = 2; // 0: static depth, 1: dynamic depth.
	static constexpr uint32_t POOL_TEXEL_BYTES = 4; // R32_UINT.
	static constexpr uint64_t POOL_PAGE_BYTES = uint64_t(PAGE_SIZE) * PAGE_SIZE * POOL_TEXEL_BYTES * POOL_LAYERS;
	// The pool texture stays within 16384 texels per side, the 2D size limit of every target GPU.
	static constexpr uint32_t POOL_MAX_PAGES_PER_SIDE = 16384 / PAGE_SIZE;
	static constexpr uint32_t POOL_MAX_PAGES = POOL_MAX_PAGES_PER_SIDE * POOL_MAX_PAGES_PER_SIDE;
	// Budget V6: 64, 128, 256 and 512 MiB.
	static constexpr uint32_t pool_quality_pages(PoolQuality p_quality) {
		return p_quality == POOL_QUALITY_LOW ? 2048u : (p_quality == POOL_QUALITY_MEDIUM ? 4096u : (p_quality == POOL_QUALITY_HIGH ? 8192u : 16384u));
	}
	static constexpr uint64_t pool_bytes(uint32_t p_page_count) { return uint64_t(p_page_count) * POOL_PAGE_BYTES; }
	// The page grid of a pool: a power-of-two width, then as many rows as needed.
	static Vector2i pool_page_grid(uint32_t p_page_count);
	static const char *pool_quality_name(PoolQuality p_quality);

	// Virtual maps per light.
	enum MapKind : uint32_t {
		MAP_KIND_DIRECTIONAL, // One map per clipmap level.
		MAP_KIND_SPOT, // One map.
		MAP_KIND_OMNI, // Six cube faces.
		MAP_KIND_AREA, // Five faces, the front hemisphere.
		MAP_KIND_MAX,
	};
	static uint32_t map_kind_map_count(MapKind p_kind, uint32_t p_levels);

	// The page table grows in steps of TABLE_GROWTH_MAPS maps, up to TABLE_MAX_MAPS. 64 omni lights
	// and one directional light with every clipmap level take 400 maps: a 512-map table, 10.7 MiB,
	// within V6's 16 MiB.
	static constexpr uint32_t TABLE_GROWTH_MAPS = 128;
	static constexpr uint32_t TABLE_MAX_MAPS = 4096;
	static constexpr uint64_t table_bytes(uint32_t p_maps) { return uint64_t(p_maps) * MAP_TABLE_BYTES; }
	static constexpr uint32_t MAP_NONE = 0xFFFFFFFFu;

	// LightData::vsm_map_base is the light's first map, or MAP_NONE. LightData::vsm_flags holds the
	// map kind in bits 0-1 and the number of maps (clipmap levels or faces) in bits 2-6.
	static constexpr uint32_t LIGHT_FLAGS_KIND_MASK = 0x3u;
	static constexpr uint32_t LIGHT_FLAGS_COUNT_SHIFT = 2;
	static constexpr uint32_t LIGHT_FLAGS_COUNT_MASK = 0x1Fu;
	static constexpr uint32_t light_flags(MapKind p_kind, uint32_t p_map_count) {
		return (uint32_t(p_kind) & LIGHT_FLAGS_KIND_MASK) | ((p_map_count & LIGHT_FLAGS_COUNT_MASK) << LIGHT_FLAGS_COUNT_SHIFT);
	}
	// The map of a light's clipmap level or face p_index, or MAP_NONE when the light has no maps or
	// p_index is not below its map count. The CPU model of vsm_light_map() in
	// vsm_page_table_inc.glsl: consumers never add an index to vsm_map_base themselves, since
	// MAP_NONE + 1 wraps to map 0.
	static constexpr uint32_t light_map(uint32_t p_map_base, uint32_t p_light_flags, uint32_t p_index) {
		return (p_map_base == MAP_NONE || p_index >= ((p_light_flags >> LIGHT_FLAGS_COUNT_SHIFT) & LIGHT_FLAGS_COUNT_MASK)) ? MAP_NONE : p_map_base + p_index;
	}

	// Directional clipmaps. Level l is a square of CLIPMAP_LEVEL0_EXTENT * 2^l meters in the plane
	// facing the light, centered on the camera. Its window origin snaps to whole mip 0 pages, and
	// the map is addressed toroidally: a world position's virtual texel is its absolute texel
	// modulo VIRTUAL_SIZE, at every mip. A camera move then only changes the pages that wrap.
	static constexpr float CLIPMAP_LEVEL0_EXTENT = 4.0f;
	static constexpr uint32_t CLIPMAP_MAX_LEVELS = 16;
	static uint32_t clipmap_level_count(float p_max_distance);
	static float clipmap_level_extent(uint32_t p_level);
	static float clipmap_page_world_size(uint32_t p_level);
	// The absolute mip 0 page of the low corner of the level's window around p_center (light-plane
	// coordinates in meters).
	static Vector2i clipmap_level_origin(const Vector2 &p_center, uint32_t p_level);
	// The absolute page at p_mip where the level's window starts: floor(p_origin / 2^p_mip).
	static Vector2i clipmap_mip_origin(const Vector2i &p_origin, uint32_t p_mip);

	// The page slots a scroll moves to new ground, per mip: whole columns and whole rows, as wrapped
	// ranges of slots [start, start + count) modulo the mip's pages per side.
	struct ScrollRange {
		uint32_t col_start = 0;
		uint32_t col_count = 0;
		uint32_t row_start = 0;
		uint32_t row_count = 0;
	};
	struct ClipmapScroll {
		ScrollRange mips[MIP_COUNT];
		bool any = false;
	};
	static ClipmapScroll clipmap_scroll(const Vector2i &p_old_origin, const Vector2i &p_new_origin);
	static ClipmapScroll clipmap_scroll_all();
	static bool clipmap_scroll_hits(const ClipmapScroll &p_scroll, uint32_t p_mip, uint32_t p_x, uint32_t p_y);
	// The CPU model of the scroll pass: sets ENTRY_SCROLLED on every allocated entry of the map in a
	// moved column or row. Returns how many entries it marked.
	static uint32_t clipmap_apply_scroll(uint32_t *r_map_entries, const ClipmapScroll &p_scroll);

	// The CPU model of vsm_lookup() in vsm_page_table_inc.glsl, for spot, omni and area maps. p_uv
	// is clamped to [0, 1] and p_mip to the last mip. The lookup starts at p_mip and falls back to
	// coarser mips until it finds an allocated entry. Without one, valid is false and mip is
	// MIP_COUNT. A map that is MAP_NONE or not below p_table_maps (the table's map count) is invalid,
	// as on the GPU.
	struct LookupResult {
		uint32_t texel_x = 0;
		uint32_t texel_y = 0;
		uint32_t mip = MIP_COUNT;
		uint32_t entry = 0;
		bool valid = false;
	};
	static uint32_t entry_index(uint32_t p_map, uint32_t p_mip, uint32_t p_page_x, uint32_t p_page_y);
	static LookupResult lookup(const uint32_t *p_table, uint32_t p_table_maps, uint32_t p_map, const Vector2 &p_uv, uint32_t p_mip, bool p_fallback = true);
	// The CPU model of vsm_lookup_clipmap(), for a directional clipmap level. p_level_origin is the
	// absolute mip 0 page where the level's window starts (get_clipmap_origin()), and p_texel the
	// absolute mip 0 virtual texel; both may be negative. At mip m the window starts at
	// floor(p_level_origin / 2^m) and holds PAGES_PER_SIDE >> m pages, so when the origin is not a
	// multiple of 2^m the level's far edge is outside it and its slot holds the near edge. The lookup
	// is invalid for a texel outside the level, and stops falling back at the first mip whose page
	// is outside that mip's window (every coarser mip's is too); the caller then tries the next
	// clipmap level.
	static LookupResult lookup_clipmap(const uint32_t *p_table, uint32_t p_table_maps, uint32_t p_map, const Vector2i &p_level_origin, const Vector2i &p_texel, uint32_t p_mip, bool p_fallback = true);

	// "#define VSM_..." lines for every shader that includes vsm_page_table_inc.glsl.
	static String get_glsl_defines();

	struct MapRange {
		uint32_t first = MAP_NONE;
		uint32_t count = 0;
		MapKind kind = MAP_KIND_SPOT;
	};

private:
	struct FreeRange {
		uint32_t first = 0;
		uint32_t count = 0;
	};

	struct LightMaps {
		MapRange range;
		uint64_t last_used_frame = 0;
		// Directional lights: the light plane and each level's window origin.
		Vector3 axis_x;
		Vector3 axis_y;
		Vector2i origins[CLIPMAP_MAX_LEVELS];
		bool origins_valid = false;
	};

	HashMap<RID, LightMaps> light_maps;
	LocalVector<FreeRange> free_ranges; // Sorted by first, never adjacent.
	uint32_t table_capacity = 0; // In maps.
	uint32_t allocated_maps = 0;
	uint32_t pool_pages = 0;
	uint64_t frame = 0;
	uint64_t pool_reallocations = 0;
	uint64_t scroll_passes = 0;
	uint64_t failed_allocations = 0;
	uint64_t map_allocations = 0;
	uint64_t evicted_lights = 0;
	uint64_t evicted_maps = 0;

	uint32_t _range_take(uint32_t p_count);
	void _range_give(uint32_t p_first, uint32_t p_count);
	bool _evict_least_recently_used();

protected:
	// Grows the page table from p_old_maps to p_new_maps, keeping the old entries and clearing the
	// new ones.
	virtual bool _table_resize(uint32_t p_old_maps, uint32_t p_new_maps) { return true; }
	// Clears the entries of maps [p_first_map, p_first_map + p_map_count) within one frame.
	virtual void _table_clear(uint32_t p_first_map, uint32_t p_map_count) {}
	// Applies clipmap_apply_scroll() to one map on the GPU.
	virtual void _table_scroll(uint32_t p_map, const ClipmapScroll &p_scroll) {}
	// Replaces the pool with one of p_page_count pages (0 frees it). On failure it returns false and
	// leaves the previous pool exactly as it was.
	virtual bool _pool_resize(uint32_t p_page_count) { return true; }

public:
	// The pool is reallocated only when the page count changes, never per frame. On failure the
	// previous pool and its page count stay. A new pool holds none of the old pages: the page
	// marking and allocation pass (#219) must drop every entry's ALLOCATED page when it sees
	// get_pool_reallocation_count() change.
	bool pool_create(uint32_t p_page_count);
	void pool_free();
	uint32_t get_pool_page_count() const { return pool_pages; }
	uint64_t get_pool_reallocation_count() const { return pool_reallocations; }

	// The renderer's frame number. A light whose maps no view used in this frame or the one before
	// it (MAP_EVICT_AFTER_FRAMES) is stale: its maps go, least recently used first, when another
	// light needs room, before the page table grows. A light in no view's cull result keeps its
	// maps only while the table has room.
	static constexpr uint64_t MAP_EVICT_AFTER_FRAMES = 2;
	void set_frame(uint64_t p_frame) { frame = p_frame; }
	uint64_t get_frame() const { return frame; }

	// Allocates p_kind's maps for the light instance, or returns its current maps when kind and count
	// match, and marks them used in this frame. A light keeps its maps until map_free(), until it asks
	// for another kind or count, or until it is stale and another light needs the room.
	// Returns the first map, or MAP_NONE when the page table is full.
	uint32_t map_allocate(RID p_light_instance, MapKind p_kind, uint32_t p_levels = 1);
	// Frees the light's maps; their entries are cleared before the next frame reads them.
	void map_free(RID p_light_instance);
	const MapRange *map_get(RID p_light_instance) const;
	// As map_get(), and marks the light's maps used in this frame.
	const MapRange *map_touch(RID p_light_instance);

	// Directional lights: allocates one map per clipmap level that p_max_distance needs and moves each
	// level's window to the camera. Pages that wrap are marked ENTRY_SCROLLED; a change of the light
	// plane marks every allocated page. Returns the first map, or MAP_NONE.
	uint32_t directional_update(RID p_light_instance, const Basis &p_light_basis, const Vector3 &p_camera_position, float p_max_distance);
	Vector2i get_clipmap_origin(RID p_light_instance, uint32_t p_level) const;

	void free_all();

	uint32_t get_table_capacity() const { return table_capacity; }
	uint32_t get_allocated_map_count() const { return allocated_maps; }
	uint32_t get_free_map_count() const;
	uint32_t get_free_range_count() const { return free_ranges.size(); }
	// The free range p_index, in map order.
	void get_free_range(uint32_t p_index, uint32_t &r_first, uint32_t &r_count) const;
	uint32_t get_light_count() const { return light_maps.size(); }
	uint64_t get_scroll_pass_count() const { return scroll_passes; }
	uint64_t get_failed_allocation_count() const { return failed_allocations; }
	// Ranges handed to a light since start, and the stale lights and maps evicted.
	uint64_t get_map_allocation_count() const { return map_allocations; }
	uint64_t get_evicted_light_count() const { return evicted_lights; }
	uint64_t get_evicted_map_count() const { return evicted_maps; }

	RendererVirtualShadowMapStorage() {}
	virtual ~RendererVirtualShadowMapStorage() {}
};
