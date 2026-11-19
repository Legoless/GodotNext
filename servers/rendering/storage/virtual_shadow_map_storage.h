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

#include "core/math/aabb.h"
#include "core/math/basis.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
// GodotNext: vsm_settings_debug (#221).
#include "servers/rendering/rendering_server_enums.h"

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
	// GodotNext: vsm_projection (#222). Bit 7: the view's light loop samples the light's maps instead of its
	// shadow atlas (vsm_shadow_directional() and vsm_shadow_local() in vsm_smrt_inc.glsl).
	static constexpr uint32_t LIGHT_FLAGS_SAMPLE = 1u << 7;
	static_assert((LIGHT_FLAGS_SAMPLE & (LIGHT_FLAGS_KIND_MASK | (LIGHT_FLAGS_COUNT_MASK << LIGHT_FLAGS_COUNT_SHIFT))) == 0, "LIGHT_FLAGS_SAMPLE must not overlap the kind and count bits.");
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

	/* PAGE MARKING AND ALLOCATION (#219, plan §21.3 contract S3) */

	// Fog froxels and transparents request the coarsest COARSE_MIPS mips only.
	static constexpr uint32_t COARSE_MIPS = 2;
	static constexpr uint32_t COARSE_FIRST_MIP = MIP_COUNT - COARSE_MIPS;
	// Always-resident pages (UE's coarse pages; lead decision 2026-11-16): every marked view requests,
	// for every light with maps, the root page (the last mip) of each spot, omni and area face, and
	// every page of mips CLIPMAP_RESIDENT_MIP and coarser of each directional clipmap level (21 per
	// level). Allocation hands them out before any other request, so a sample whose own page
	// overflowed still finds one: its face's root page, or the resident mips of its level or the next
	// level. A level's window lies within pages 16 to 48 of the next level's, and that level's mip m
	// window holds pages [0, 64 - (origin mod 2^m)): mip 4 is the finest mip whose window always
	// holds the whole finer level. The last level has no next level: a pixel request there outside
	// the resident mips' window also requests, as a coarse request, its page at the coarsest mip
	// whose window holds the point (mark_directional_point()).
	static constexpr uint32_t CLIPMAP_RESIDENT_MIP = 4;
	// Overflow: in every frame whose requests the pool could not hold, the maps that requested the
	// most pages take one mip coarser, up to OVERFLOW_MAX_BIAS; without overflow they give one mip
	// back per OVERFLOW_DECAY_FRAMES frames. Both count frames, not views: a map rises at most once
	// per frame however many views overflow, and only a frame's first marked view moves the decay.
	static constexpr uint32_t OVERFLOW_MAX_BIAS = 3;
	static constexpr uint32_t OVERFLOW_DECAY_FRAMES = 30;
	// The overflow bias state of a map: bits 0-7 the bias, 8-15 frames without overflow, and this bit
	// once the bias rose in the frame of the last update.
	static constexpr uint32_t OVERFLOW_BIAS_RAISED = 1u << 16;
	// A page that no view requested for cache_frames() frames is dropped. Provisional values (until PQ):
	// a quarter, half, one and two seconds at 60 frames per second.
	static constexpr uint32_t MAX_CACHE_FRAMES = 120;
	static constexpr uint32_t cache_frames(PoolQuality p_quality) {
		return p_quality == POOL_QUALITY_LOW ? 15u : (p_quality == POOL_QUALITY_MEDIUM ? 30u : (p_quality == POOL_QUALITY_HIGH ? 60u : 120u));
	}
	// The allocation pass sorts reusable pages into buckets: 0 for free pages, then one per age of a
	// cached page, oldest first (bucket cache_frames - age).
	static constexpr uint32_t BUCKET_COUNT = 128;
	static constexpr uint32_t BUCKET_NONE = 0xFFFFFFFFu;
	// The request list holds every entry first requested in a view: four per pool page, at least 65,536.
	static constexpr uint32_t REQUEST_LIST_MIN = 65536;
	static constexpr uint32_t request_capacity(uint32_t p_pool_pages) {
		return p_pool_pages * 4u > REQUEST_LIST_MIN ? p_pool_pages * 4u : REQUEST_LIST_MIN;
	}
	// Godot's directional light limit, and the transparent boxes one view marks (more merge into the last).
	static constexpr uint32_t MAX_DIRECTIONAL_LIGHTS = 8;
	static constexpr uint32_t MAX_TRANSPARENT_BOXES = 1024;
	// log2 of a clipmap level 0 texel in meters: CLIPMAP_LEVEL0_EXTENT / VIRTUAL_SIZE = 2^-10.
	static constexpr int32_t CLIPMAP_TEXEL0_EXPONENT = -10;
	// Spot, omni and area faces: the near distance along the face axis, a fraction of the range.
	static constexpr float FACE_NEAR_FRACTION = 0.001f;
	static constexpr float FACE_NEAR_MIN = 0.001f;
	static constexpr float FACE_NEAR_MAX = 0.05f;

	// Per-map data, one record per page-table map (VSMMapData in vsm_mark_inc.glsl, std430): how a
	// world position lands in the map. Rows are (x, y, z, constant) coefficients of the world position.
	// - Directional clipmap level: row 0 and 1 give the absolute mip 0 virtual texel of the level
	//   (the light plane's X and Y in meters times VIRTUAL_SIZE / the level's extent, a power of two),
	//   row 2 the depth along the light, row 3 is (0, 0, 0, 1). origin is the level's window origin.
	// - Spot, omni and area face: rows 0 to 3 are a perspective clip transform; the map's uv is
	//   (x / w, y / w) * 0.5 + 0.5, with no Y flip, and a point is in the face when w > near_w and
	//   |x| <= w and |y| <= w. texel_scale * w is the face's mip 0 texel in meters at the point.
	struct MapData {
		float rows[4][4] = {};
		int32_t origin[2] = {};
		uint32_t kind = MAP_KIND_MAX; // MAP_KIND_MAX: no map.
		float texel_scale = 0.0f; // Clipmap: the level's mip 0 texel in meters; face: per meter of w.
		float near_w = 0.0f;
		float max_distance = 0.0f; // Clipmap: no view pixel deeper than this requests the light.
		uint32_t index = 0; // The clipmap level or the face.
		// GodotNext: vsm_settings_debug (#221). Mips the map's light adds to the view's resolution LOD bias:
		// the moving bias minus the static one while the light moves, stepping back to 0 once it stops
		// (moving_lod_bias()). Directional lights read level 0's.
		int32_t lod_bias = 0;
		bool operator==(const MapData &p_other) const;
		bool operator!=(const MapData &p_other) const { return !(*this == p_other); }
	};
	static MapData map_data_clipmap(const Basis &p_light_basis, uint32_t p_level, const Vector2i &p_origin, float p_max_distance);
	// The face's camera: spot face 0 looks along the light's -Z; omni faces 0-5 along the light's +X,
	// -X, +Y, -Y, +Z, -Z; area faces 0-4 along +X, -X, +Y, -Y and -Z (the front hemisphere).
	static Transform3D face_camera(MapKind p_kind, uint32_t p_face, const Transform3D &p_light_transform);
	static float face_fov_degrees(MapKind p_kind, float p_spot_angle);
	static MapData map_data_face(MapKind p_kind, uint32_t p_face, const Transform3D &p_light_transform, float p_range, float p_spot_angle);

	// CPU models of the vsm_mark_inc.glsl functions, written in the same operation order.
	// ((r0 * x + r1 * y) + r2 * z) + r3.
	static float row_dot(const float p_row[4], const Vector3 &p_point);
	// floor(log2(p_value)) from the float's exponent bits; -127 for zero and subnormals.
	static int32_t float_exponent(float p_value);
	// The coarsest face mip whose texel, p_texel0 * 2^mip, is not larger than p_footprint (mip 0 when
	// even mip 0's texel is larger).
	static uint32_t face_mip(float p_texel0, float p_footprint);
	static uint32_t biased_mip(int32_t p_mip, int32_t p_bias);
	// A face's virtual texel of p_world, and w. False when the point is not in the face.
	static bool face_texel(const MapData &p_map, const Vector3 &p_world, float &r_w, uint32_t &r_texel_x, uint32_t &r_texel_y);
	// The absolute mip 0 texel of p_world in a directional light's level 0; level l's is this >> l.
	static Vector2i clipmap_texel0(const MapData &p_level0, const Vector3 &p_world);
	// Whether vsm_lookup_clipmap() reads level p_level_texel at p_mip: inside the level and the mip's window.
	static bool clipmap_in_window(const Vector2i &p_origin, const Vector2i &p_level_texel, uint32_t p_mip);
	// The page slot of a level texel at a mip.
	static Vector2i clipmap_page(const Vector2i &p_level_texel, uint32_t p_mip);
	// level + mip of a clipmap request: a level l mip m texel is 2^(l + m + CLIPMAP_TEXEL0_EXPONENT) m.
	static int32_t clipmap_level_mip(float p_footprint, int32_t p_bias);
	// The level texel of a level 0 texel: floor division by 2^p_level.
	static Vector2i clipmap_level_texel(const Vector2i &p_texel0, uint32_t p_level);

	// The allocation state buffer (uints): per-view counters [0, STATE_VIEW_END), totals since start
	// [STATE_VIEW_END, STATE_BUCKETS), then the free-list buckets' counts, offsets and cursors.
	enum StateIndex : uint32_t {
		STATE_REQUESTS, // Entries first requested in the view (may exceed the list's capacity).
		STATE_REQUESTS_DROPPED, // Requests the full list could not hold.
		STATE_AVAILABLE, // The free list's length.
		STATE_CURSOR, // Pages handed out.
		STATE_OVERFLOW, // Requested entries left without a page.
		STATE_NEW, // Requested entries that got a page.
		STATE_KEPT, // Requested entries that had a page.
		STATE_EVICTED, // Pages dropped: not requested for cache_frames() frames, or scrolled.
		STATE_ORPHANED, // Pages whose entry was cleared.
		STATE_STOLEN, // New pages taken from a cached entry.
		STATE_DIRTY, // Pages in the dirty list.
		STATE_DIRTY_DIRECTIONAL, // Of those, directional pages (the list's first part).
		STATE_COARSE, // Requested entries with ENTRY_COARSE.
		STATE_CACHED, // Pages kept for an entry not requested in the view.
		STATE_MAX_MAP_REQUESTS, // The largest map's request count.
		STATE_BIASED_MAPS, // Maps with an overflow bias after the view.
		STATE_RESIDENT, // Always-resident requests: the request list's first entries.
		STATE_RESIDENT_OVERFLOW, // Of those, entries left without a page.
		STATE_SINK, // Written only by impossible atomic results (#277).
		STATE_VIEW_END = 32,
		STATE_TOTAL_OVERFLOW = STATE_VIEW_END, // Requests left without a page since start.
		STATE_OVERFLOW_VIEWS, // Views with overflow since start.
		STATE_TOTAL_DROPPED, // Requests dropped by a full list since start.
		STATE_VIEWS, // Views marked since start.
		STATE_BIAS_FRAME, // The frame (plus one) of the last view that updated the overflow bias.
		STATE_TOTAL_RESIDENT_OVERFLOW, // Always-resident requests left without a page since start.
		STATE_BUCKETS = 64,
		STATE_BUCKET_OFFSETS = STATE_BUCKETS + BUCKET_COUNT,
		STATE_BUCKET_CURSORS = STATE_BUCKET_OFFSETS + BUCKET_COUNT,
		STATE_SIZE = STATE_BUCKET_CURSORS + BUCKET_COUNT,
	};
	// The per-map state buffer: MAP_STATE_REGIONS regions of the page table's map count (uints).
	enum MapStateRegion : uint32_t {
		MAP_STATE_REQUESTS, // Entries first requested in the view, per map.
		MAP_STATE_DIRTY_COUNT, // Dirty pages per map.
		MAP_STATE_DIRTY_OFFSET, // The map's first record in the dirty list.
		MAP_STATE_DIRTY_CURSOR, // Records written so far.
		MAP_STATE_BIAS, // The overflow bias state (overflow_bias_update()); kept across views.
		MAP_STATE_REGIONS,
	};
	// The dirty list's indirect arguments (uints): dispatch (count, 1, 1) for the directional pages,
	// the local pages and all pages, each one workgroup per page.
	static constexpr uint32_t INDIRECT_DIRECTIONAL = 0;
	static constexpr uint32_t INDIRECT_LOCAL = 3;
	static constexpr uint32_t INDIRECT_ALL = 6;
	static constexpr uint32_t INDIRECT_WORDS = 9;

	// Request keys of the CPU models: the entry index, plus bit 32 for a coarse request.
	static constexpr uint64_t REQUEST_COARSE = uint64_t(1) << 32;
	// A local light as the mark pass reads it from LightData (view space).
	struct MarkLight {
		MapKind kind = MAP_KIND_SPOT;
		Vector3 position; // View space.
		Vector3 direction; // View space, where the light shines.
		float inv_radius = 0.0f; // Omni and spot: 1 / range. Area: 1 / (range + half the diagonal).
		float cos_angle = 0.0f; // Spot only.
		float fog_energy = 0.0f;
		uint32_t map_base = MAP_NONE;
		uint32_t flags = 0;
	};
	// Whether the light reaches a point p_rel (the point minus the light's position, view space).
	static bool light_reaches(const MarkLight &p_light, const Vector3 &p_rel);
	// Whether the light reaches the fog cells around a froxel grid vertex p_rel (the vertex minus the
	// light's position, view space) whose cells' diagonal is p_cell_size: within its range plus one
	// diagonal, with no cone or hemisphere test (a point of the cells may lie inside the cone while
	// the vertex does not). r_all_faces is set when the light is within one diagonal of the vertex:
	// the cells around it may then meet every face.
	static bool light_reaches_cell(const MarkLight &p_light, const Vector3 &p_rel, float p_cell_size, bool &r_all_faces);
	// The sampling check (GV-2's sampled-but-unmapped pages): each fine request of mark_local_point()
	// and mark_directional_point() is looked up in a page table after allocation, as the projection
	// (#222) will look it up: a face from the request's mip down to its root page (lookup()); a
	// clipmap from the request's level and mip (clipmap_sample()).
	struct SampleCheck {
		const uint32_t *table = nullptr; // The page table after allocation.
		const uint32_t *requested = nullptr; // Optional: the table after marking. Requests it does not flag are not sampled.
		uint32_t sampled = 0;
		uint32_t fallback = 0; // Served by a coarser page: the request's own entry had none.
		uint32_t unmapped = 0; // Served by no page.
		uint32_t unconfirmed = 0; // Not sampled: the GPU did not make the request (float rounding).
	};
	// The sample of a clipmap request at p_level and p_mip: the level's lookup from p_mip while the
	// level's window holds the point (lookup_clipmap()), then each next level from the mip with the
	// same texel size, p_mip minus the levels moved (at least 0).
	static LookupResult clipmap_sample(const uint32_t *p_table, uint32_t p_table_maps, const MapData *p_maps, uint32_t p_first, uint32_t p_count, const Vector2i &p_texel0, uint32_t p_level, uint32_t p_mip);
	// The coarse sample of a directional clipmap (#215): mips CLIPMAP_RESIDENT_MIP and coarser in the
	// finest level whose mip-CLIPMAP_RESIDENT_MIP window holds the point, or, in the last level's far
	// strip outside that window, the coarsest mip (< CLIPMAP_RESIDENT_MIP) whose window holds it.
	static LookupResult clipmap_coarse_sample(const uint32_t *p_table, uint32_t p_table_maps, const MapData *p_maps, uint32_t p_first, uint32_t p_count, const Vector2i &p_texel0);
	// The fine requests of a pixel (or the coarse ones of a fog point) in one local light's maps.
	// p_map_bias holds each map's overflow bias state (bits 0-7 the bias), or is null. r_sample, when
	// set, looks up every fine or coarse request.
	static void mark_local_point(const MapData *p_maps, uint32_t p_table_maps, const uint32_t *p_map_bias, const MarkLight &p_light, const Vector3 &p_world, float p_footprint, int32_t p_lod_bias, bool p_coarse, bool p_all_faces, LocalVector<uint64_t> &r_requests, SampleCheck *r_sample = nullptr);
	// The same for a directional light whose levels start at p_map_base. A request in the last level
	// outside its resident mips' window also requests its page at the coarsest mip whose window holds
	// the point, as a coarse request.
	static void mark_directional_point(const MapData *p_maps, uint32_t p_table_maps, const uint32_t *p_map_bias, uint32_t p_map_base, uint32_t p_light_flags, const Vector3 &p_world, float p_footprint, int32_t p_lod_bias, bool p_coarse, LocalVector<uint64_t> &r_requests, SampleCheck *r_sample = nullptr);
	// The always-resident requests of a light (coarse keys): the root page of each face of a spot, omni
	// or area light, or every page of mips CLIPMAP_RESIDENT_MIP and coarser of each clipmap level.
	static void mark_resident(const MapData *p_maps, uint32_t p_table_maps, uint32_t p_map_base, uint32_t p_light_flags, LocalVector<uint64_t> &r_requests);
	// The coarse requests of a world-space box: every coarse page its corners' bounding rect touches,
	// in every level or face the box reaches (a face whose near plane cuts the box: all coarse pages;
	// the last clipmap level's far strip: the coarsest in-window mip < CLIPMAP_RESIDENT_MIP).
	static void mark_local_box(const MapData *p_maps, uint32_t p_table_maps, const MarkLight &p_light, const Vector3 &p_light_world, const AABB &p_box, LocalVector<uint64_t> &r_requests, SampleCheck *r_sample = nullptr);
	static void mark_directional_box(const MapData *p_maps, uint32_t p_table_maps, uint32_t p_map_base, uint32_t p_light_flags, const AABB &p_box, LocalVector<uint64_t> &r_requests, SampleCheck *r_sample = nullptr);

	// The physical page records of the pool header (vsm_allocate_pages.glsl): the owning entry plus
	// one (0: free), the frame it was last requested in, and its bucket in this view's free list.
	struct PageRecord {
		uint32_t owner = 0;
		uint32_t last_used = 0;
		uint32_t bucket = BUCKET_NONE;
		uint32_t pad = 0;
	};
	struct AllocationCounts {
		uint32_t kept = 0; // Requested entries that had a page.
		uint32_t new_pages = 0; // Requested entries that got a page.
		uint32_t stolen = 0; // Of those, pages taken from a cached entry.
		uint32_t overflow = 0; // Requested entries left without a page.
		uint32_t evicted = 0; // Pages dropped: not requested for cache_frames() frames, or scrolled.
		uint32_t orphaned = 0; // Pages whose entry was cleared (a freed light, a new pool).
		uint32_t cached = 0; // Pages kept for an entry not requested in this view.
		uint32_t available = 0; // The free list's length.
		uint32_t dirty = 0; // Requested entries whose page must be rendered.
		uint32_t coarse = 0; // Requested entries with ENTRY_COARSE.
		uint32_t resident_overflow = 0; // Always-resident requests left without a page.
	};
	// Whether an entry's page must be rendered after allocation.
	static constexpr uint32_t ENTRY_DIRTY_MASK = ENTRY_DIRTY_STATIC | ENTRY_DIRTY_DYNAMIC;
	// The update step of the allocation pass for one physical page (MODE_UPDATE), given the page table
	// after marking: whether the page is kept for a requested entry, stays cached for an unrequested
	// one, is dropped (not requested for p_cache_frames frames, or scrolled), was orphaned (its entry
	// was cleared) or was free. r_bucket is its free-list bucket: 0 for a page that is free after the
	// step, cache_frames - age for a cached page (the oldest come first), BUCKET_NONE otherwise.
	enum PageUpdate {
		PAGE_UPDATE_FREE,
		PAGE_UPDATE_KEPT,
		PAGE_UPDATE_CACHED,
		PAGE_UPDATE_EVICTED,
		PAGE_UPDATE_ORPHANED,
	};
	static PageUpdate page_update(const PageRecord &p_page, uint32_t p_page_index, uint32_t p_grid_width, const uint32_t *p_table, uint64_t p_table_entries, uint32_t p_frame, uint32_t p_cache_frames, uint32_t &r_bucket);
	// The CPU model of the allocation passes (update, free list, assign in three phases: the
	// always-resident requests, which are the list's first p_resident_count, then the other coarse
	// requests, then the rest), in place on a page table and the page records. Pages go out in
	// free-list order (bucket, then page index); the GPU hands them out in atomic order, so only the
	// counts and the outcomes that do not depend on that order compare.
	static AllocationCounts allocate_model(uint32_t *r_table, uint64_t p_table_entries, PageRecord *r_pages, uint32_t p_pool_pages, uint32_t p_grid_width, const uint32_t *p_requests, uint32_t p_request_count, uint32_t p_frame, uint32_t p_cache_frames, uint32_t p_resident_count = 0);
	// The page-table entry after the emit pass: request flags cleared, dirty flags handed to the list.
	static constexpr uint32_t entry_after_emit(uint32_t p_entry) {
		return p_entry & ~(ENTRY_REQUESTED | ENTRY_COARSE | ENTRY_DIRTY_STATIC | ENTRY_DIRTY_DYNAMIC | ENTRY_SCROLLED | ((p_entry & ENTRY_ALLOCATED) ? ENTRY_OVERFLOW : 0u));
	}
	// The overflow bias state of a map (OVERFLOW_BIAS_RAISED) after a view with p_overflow requests
	// left without a page; p_max_requests is the largest map's request count; p_new_frame is set for
	// the first marked view of a frame.
	static uint32_t overflow_bias_update(uint32_t p_state, uint32_t p_requests, uint32_t p_max_requests, uint32_t p_overflow, bool p_new_frame);
	static uint32_t overflow_bias(uint32_t p_state) { return p_state & 0xFFu; }

	struct MapRange {
		uint32_t first = MAP_NONE;
		uint32_t count = 0;
		MapKind kind = MAP_KIND_SPOT;
	};

	/* NON-NANITE PAGE CULLING AND RASTER (#218, plan §21.3 contract S4) */

	// Sub-texel small-caster culling threshold in virtual texels at the target page's mip; 0 disables it.
	static constexpr float DEFAULT_MIN_CASTER_TEXELS = 1.0f;
	static constexpr uint32_t RASTER_PAIR_LIST_MIN = 262144;
	static constexpr uint32_t raster_pair_capacity(uint32_t p_pool_pages) {
		return p_pool_pages * 64u > RASTER_PAIR_LIST_MIN ? p_pool_pages * 64u : RASTER_PAIR_LIST_MIN;
	}

	// Order-preserving reversed-Z uint32 encoding of linear depth for R32_UINT imageAtomicMax:
	// smaller (nearer) depth maps to a strictly larger uint in (0, 0xFFFFFFFF), so nearer casters win
	// over farther ones and over cleared empty texels (0), while 0xFFFFFFFF stays reserved for #277 sinks.
	static uint32_t depth_pack(float p_depth);
	static float depth_unpack(uint32_t p_packed);

	// One record of the dirty-page list emitted by vsm_allocate_pages.glsl (uvec4, std430).
	struct DirtyPageRecord {
		uint32_t map = 0;
		uint32_t packed_page = 0; // mip | (page_x << 8) | (page_y << 16).
		uint32_t phys_entry = 0; // Physical page x and y without flags.
		uint32_t flags = 0; // ENTRY_DIRTY_STATIC, ENTRY_DIRTY_DYNAMIC, etc.
	};

	// CPU model of a MultiMesh or GPUParticles3D sub-instance's world-space AABB in vsm_instance_cull.glsl.
	// Rejects inactive (-INF) particle slots and zero-scale transforms without producing NaN. With
	// p_view_facing (the shadow material writes MODELVIEW_MATRIX: billboards), the box also holds
	// p_corner_reach (view_facing_reach()) times max(1, the instance's longest world axis) around the
	// instance's world origin, wherever the mesh is turned.
	static bool sub_instance_box(const float p_model[12], const float *p_transforms, uint32_t p_sub_index, uint32_t p_current_offset, uint32_t p_stride_vec4, bool p_format_2d, uint32_t p_trail_steps, const AABB &p_local_aabb, float p_extra_margin, AABB &r_world_aabb, bool p_view_facing = false, float p_corner_reach = 0.0f);
	// The largest |x| + |y| + |z| of a corner of p_local_aabb: a basis of unit axes (a billboard's) keeps
	// every point of the box within it of the origin.
	static float view_facing_reach(const AABB &p_local_aabb);
	// CPU model of vsm_map_view() in vsm_raster_inc.glsl: the light's camera for one map (view to world).
	// A face's is face_camera(); a clipmap level's looks along the light from p_camera_position.
	static Transform3D map_view(const MapData &p_map, const Vector3 &p_camera_position);

	// CPU model of vsm_cull_box_dirty_pages() in vsm_instance_cull.glsl: emits the dirty-record indices
	// of a light's maps whose page frustum and static/dynamic dirty layer touch p_box and pass the
	// small-caster threshold p_min_texels.
	static void cull_box_dirty_pages(const MapData *p_maps, uint32_t p_table_maps, const uint32_t *p_map_dirty_counts, const uint32_t *p_map_dirty_offsets, const DirtyPageRecord *p_dirty, uint32_t p_dirty_limit, uint32_t p_map_base, uint32_t p_light_flags, const AABB &p_box, bool p_dynamic, float p_min_texels, LocalVector<uint32_t> &r_dirty_indices, uint32_t &r_small_culled);

	/* PRODUCT SURFACE (#221, plan §21.3.2 S1, budget V6) */

	// rendering/lights_and_shadows/shadow_method, read once at startup (changing it needs a restart).
	enum ShadowMethod : uint32_t {
		SHADOW_METHOD_ATLAS,
		SHADOW_METHOD_VIRTUAL,
		SHADOW_METHOD_MAX,
	};
	// What EditorNode::get_initial_settings() writes into a new project. Existing projects keep the setting's
	// default, ATLAS. #223 sets this to VIRTUAL once gate GV passes in PQ, and the default stays ATLAS.
	static constexpr ShadowMethod NEW_PROJECT_SHADOW_METHOD = SHADOW_METHOD_ATLAS;
	static constexpr ShadowMethod DEFAULT_SHADOW_METHOD = SHADOW_METHOD_ATLAS;

	// The tier rule: VSM needs Forward+, 32-bit image atomics and indirect-count draws. Below it, lights that
	// would use virtual shadow maps use the shadow atlas, and one line names the reason.
	enum TierReason : uint32_t {
		TIER_REASON_NONE,
		TIER_REASON_UNSUPPORTED_RENDERER, // Mobile, Compatibility, or no Forward+ virtual shadow map storage.
		TIER_REASON_NO_IMAGE_ATOMIC_32, // RenderingDevice::SUPPORTS_IMAGE_ATOMIC_32_BIT.
		TIER_REASON_NO_INDIRECT_COUNT, // RenderingDevice::SUPPORTS_DRAW_INDIRECT_COUNT.
		TIER_REASON_MAX,
	};
	struct TierInputs {
		bool forward_plus = false;
		bool image_atomic_32 = false;
		bool indirect_count = false;
	};
	// Pure: the first failing check wins, in the order above.
	static TierReason evaluate_tier(const TierInputs &p_inputs);
	static const char *get_tier_reason_name(TierReason p_reason);
	// The one line printed when a light would use virtual shadow maps below the tier.
	static String format_fallback_line(TierReason p_reason);

	// What a light's shadow method resolves to: LIGHT_SHADOW_METHOD_VIRTUAL or LIGHT_SHADOW_METHOD_ATLAS.
	// PROJECT takes p_project; RAY_TRACED (#141) and DISTANCE_FIELD (#214) are not implemented yet and take
	// PROJECT; VIRTUAL without p_vsm_available takes ATLAS. Pure.
	static RSE::LightShadowMethod resolve_light_shadow_method(RSE::LightShadowMethod p_method, ShadowMethod p_project, bool p_vsm_available);
	// Whether p_method resolves to VIRTUAL before the tier is applied (resolve_light_shadow_method() with VSM
	// available).
	static bool light_shadow_method_wants_virtual(RSE::LightShadowMethod p_method, ShadowMethod p_project);
	static bool is_light_shadow_method_implemented(RSE::LightShadowMethod p_method);
	static const char *get_light_shadow_method_name(RSE::LightShadowMethod p_method);

	// The process-wide state the light storages and the scene cull read. The Forward+ storage sets it at
	// startup (set_startup_state()); without one the tier reason is TIER_REASON_UNSUPPORTED_RENDERER.
	static void set_startup_state(TierReason p_reason, ShadowMethod p_project, bool p_dev_switch);
	// Back to the state without a Forward+ storage; the once-per-process lines and warnings may print again.
	static void reset_startup_state();
	static TierReason get_tier_reason();
	static bool is_vsm_available() { return get_tier_reason() == TIER_REASON_NONE; }
	static ShadowMethod get_project_shadow_method();
	static bool is_dev_switch_forcing();
	// resolve_light_shadow_method() with that state: whether a shadowed light with p_method gets virtual maps.
	static bool light_uses_vsm(RSE::LightShadowMethod p_method);
	// Every LightStorage::light_set_shadow_method() calls this. An unimplemented method prints one warning per
	// method per process; VIRTUAL below the tier prints the fallback line (once per process, report_fallback()).
	static void note_light_shadow_method(RSE::LightShadowMethod p_method);
	// Prints format_fallback_line() once per process, when VSM is not available. Returns true when it printed.
	static bool report_fallback();
	// RenderingServerDefault calls this once the renderer exists: a virtual project below the tier prints
	// the fallback line. Without p_print (the dummy renderer, which draws no shadows) it only records the
	// project's method.
	static void report_project_shadow_method(bool p_print = true);
	// Counters for the doctests: unimplemented-method warnings and fallback lines printed since start.
	static uint32_t get_unimplemented_warning_count();
	static uint32_t get_fallback_line_count();
	// rendering/lights_and_shadows/shadow_method, as an index (0 atlas, 1 virtual).
	static ShadowMethod read_project_shadow_method();

	// Scalability (budget V6): rendering/lights_and_shadows/virtual_shadows/quality picks a preset, and each
	// preset reads <setting>.low, .medium, .high or .epic. The defaults below are the only copies.
	struct Scalability {
		uint32_t max_physical_pages = 0;
		int32_t lod_bias_directional = 0;
		int32_t lod_bias_local = 0;
		int32_t lod_bias_directional_moving = 0;
		int32_t lod_bias_local_moving = 0;
	};
	static constexpr PoolQuality DEFAULT_QUALITY = POOL_QUALITY_HIGH;
	// Provisional until PQ (docs/godotnext/source-log/vsm.md): no bias at High and Epic; Low takes one mip,
	// a quarter of the pages, for its quarter of High's pool; moving lights take one mip more than static
	// ones at every preset.
	static Scalability default_scalability(PoolQuality p_quality);
	static constexpr uint32_t MIN_PHYSICAL_PAGES = 64;
	static constexpr int32_t MAX_SETTING_LOD_BIAS = 4;
	// Clamps a preset's values to what the pool and the mark pass take.
	static Scalability clamp_scalability(const Scalability &p_values);
	static const char *const SETTING_QUALITY;
	static const char *const SETTING_MAX_PHYSICAL_PAGES;
	static const char *const SETTING_LOD_BIAS_DIRECTIONAL;
	static const char *const SETTING_LOD_BIAS_LOCAL;
	static const char *const SETTING_LOD_BIAS_DIRECTIONAL_MOVING;
	static const char *const SETTING_LOD_BIAS_LOCAL_MOVING;
	// The preset's values from the project settings (the defaults where a setting is missing).
	static Scalability read_scalability(PoolQuality p_quality);
	static PoolQuality read_quality();

	// Moving lights: a light whose transform (a directional light's basis) changed in the current frame is
	// moving. It adds the moving bias minus the static bias to its maps; once it stops, that extra bias steps
	// one mip back toward 0 per MOVING_DECAY_FRAMES frames.
	static constexpr uint32_t MOVING_DECAY_FRAMES = 30;
	static int32_t moving_lod_bias(int32_t p_static_bias, int32_t p_moving_bias, uint64_t p_frames_since_moved);
	static constexpr uint64_t NEVER_MOVED = UINT64_MAX;
	// Records the light's transform in the current frame and returns the frames since it last moved
	// (NEVER_MOVED for a light that has not moved since it got its maps). Needs the light's maps.
	uint64_t note_light_motion(RID p_light_instance, const Transform3D &p_transform);

	// RENDERING_INFO_VSM_PAGES_*, read back from the GPU asynchronously: the read starts one frame after the views and arrives when the GPU is done (a few frames late). Process-wide, so get_rendering_info()
	// reads them on any thread; 0 without virtual shadow maps.
	enum FrameStat : uint32_t {
		FRAME_STAT_ALLOCATED, // Pages holding an entry after the frame's last marked view: kept + new + cached - stolen.
		FRAME_STAT_REQUESTED, // Entries the frame's views requested.
		FRAME_STAT_RERENDERED, // Pages the frame's views cleared and rendered.
		FRAME_STAT_OVERFLOW, // Requests the frame's views left without a page.
		FRAME_STAT_MAX,
	};
	static uint64_t get_frame_stat(FrameStat p_stat);
	// Sums the per-view allocation counters of one frame (STATE_VIEW_END words per view) into the stats.
	static void publish_frame_stats(const uint32_t *p_view_states, uint32_t p_view_count);
	static void reset_frame_stats();

	// A grid-stride compute dispatch over p_items with p_group_size threads per group: the groups to dispatch,
	// at least 1 and at most p_max_groups (LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X, 65,535 on D3D12). Each thread
	// then steps by the groups times p_group_size. The VSM_OVERDRAW pass dispatches over up to 1 << 22 pairs.
	static constexpr uint32_t grid_stride_groups(uint32_t p_items, uint32_t p_group_size, uint32_t p_max_groups) {
		const uint64_t size = p_group_size > 0 ? uint64_t(p_group_size) : 1u;
		const uint64_t wanted = (uint64_t(p_items) + size - 1u) / size;
		const uint64_t cap = p_max_groups > 0 ? uint64_t(p_max_groups) : 1u;
		return uint32_t(wanted < 1u ? 1u : (wanted > cap ? cap : wanted));
	}

	/* PROJECTION AND SMRT SOFT SHADOWS (#222, plan §21.3 contract S6) */

	// GodotNext: vsm_projection (#222). The forward light loop samples a light's maps with Shadow Map Ray
	// Tracing: R rays from the receiver toward points of the light's disk (directional: the angular size;
	// local: LIGHT_PARAM_SIZE as the radius; area: the rectangle), each marched in S steps through the map
	// at the receiver's mip; a step behind the stored depth occludes its ray, and the shadow factor is the
	// share of rays that reach the light. R = 0, or a light without size, takes one 2 x 2 filtered tap.
	// vsm_smrt_inc.glsl is the GPU code; the functions below are its CPU models, written in the same order.
	//
	// Provisional values (PQ derives the final ones with GV-5; docs/godotnext/source-log/vsm.md records the
	// derivation). Rays x steps per VSM quality preset equal the texel reads of Godot's own PCSS at the
	// soft shadow quality the preset matches (blocker search plus filter, 2 x penumbra samples): Low and
	// Soft Very Low 8, Medium and Soft Low 16, High and Soft Medium 24, Epic and Soft High 48. The project
	// settings rendering/lights_and_shadows/virtual_shadows/smrt_rays_local, smrt_rays_directional and
	// smrt_steps override them; their default, -1, takes the preset's value (smrt_setting()).
	static constexpr uint32_t smrt_preset_rays(PoolQuality p_quality) {
		return p_quality == POOL_QUALITY_LOW ? 2u : (p_quality == POOL_QUALITY_MEDIUM ? 4u : (p_quality == POOL_QUALITY_HIGH ? 4u : 8u));
	}
	static constexpr uint32_t smrt_preset_steps(PoolQuality p_quality) {
		return p_quality == POOL_QUALITY_LOW ? 4u : (p_quality == POOL_QUALITY_MEDIUM ? 4u : 6u);
	}
	// The settings' ranges, and the shader's loop bounds.
	static constexpr uint32_t SMRT_MAX_RAYS = 32;
	static constexpr uint32_t SMRT_MAX_STEPS = 16;
	// A ray ends this many texels of the sampled mip from the receiver's texel in the map, so it reads at
	// most the neighboring page: half a page. This clamps the penumbra.
	static constexpr float SMRT_MAX_TEXELS = float(PAGE_SIZE / 2);
	// The first step's height above the receiver, toward the light, in texels of the sampled mip.
	static constexpr float SMRT_FIRST_STEP_TEXELS = 2.0f;
	// The receiver's offset along its normal (times the sine of its angle to the light) and the depth bias
	// of every compare, in texels of the mip a sample reads.
	static constexpr float SMRT_NORMAL_BIAS_TEXELS = 1.0f;
	static constexpr float SMRT_DEPTH_BIAS_TEXELS = 1.0f;
	// Local lights: no step goes further than this share of the way from the receiver to the light.
	static constexpr float SMRT_LOCAL_MAX_REACH = 0.95f;
	// The value a light loop uses: p_setting when it is 0 or more (clamped to p_max), else p_preset.
	static constexpr uint32_t smrt_setting(int64_t p_setting, uint32_t p_preset, uint32_t p_max) {
		return p_setting < 0 ? p_preset : (p_setting > int64_t(p_max) ? p_max : uint32_t(p_setting));
	}

	// Prefiltered Distant (rendering/lights_and_shadows/virtual_shadows/prefiltered_distant, UE Experimental,
	// off): a receiver whose clipmap level is one of the last PREFILTERED_DISTANT_LEVELS levels of a light
	// with more levels than that takes a moments filter instead of SMRT. The mean and mean square of the
	// stored depths of the PREFILTERED_DISTANT_TAPS x PREFILTERED_DISTANT_TAPS block of texels around the
	// receiver at its level and mip give Chebyshev's upper bound on the lit share of the block's occluders,
	// with a variance of at least PREFILTERED_DISTANT_MIN_VARIANCE_TEXELS^2 texels^2 and light bleeding cut
	// below PREFILTERED_DISTANT_BLEED; empty and unmapped texels count as lit. The moments are taken per
	// lookup from the pool, not stored as a separate mip (docs/godotnext/source-log/vsm.md).
	static constexpr uint32_t PREFILTERED_DISTANT_LEVELS = 2;
	static constexpr uint32_t PREFILTERED_DISTANT_TAPS = 4;
	static constexpr float PREFILTERED_DISTANT_MIN_VARIANCE_TEXELS = 0.5f;
	static constexpr float PREFILTERED_DISTANT_BLEED = 0.2f;
	// Whether a receiver in clipmap level p_level of a light with p_count levels takes the moments filter.
	static constexpr bool prefiltered_distant_level(uint32_t p_level, uint32_t p_count) {
		return p_count > PREFILTERED_DISTANT_LEVELS && p_level + PREFILTERED_DISTANT_LEVELS >= p_count;
	}

	// Ray p_ray of p_rays: a point of the unit disk, stratified in radius and turned by the golden angle,
	// rotated by the pixel's blue noise (#54's tile).
	static Vector2 smrt_disk(uint32_t p_ray, uint32_t p_rays, const Vector2 &p_noise);
	// Ray p_ray of p_rays on an area light: a point of [-0.5, 0.5]^2 of the rectangle.
	static Vector2 smrt_square(uint32_t p_ray, uint32_t p_rays, const Vector2 &p_noise);
	// The ray's jitter of its step positions, in [0, 1).
	static float smrt_jitter(uint32_t p_ray, const Vector2 &p_noise);
	// Step p_step of p_steps: geometric from p_first to p_last, shifted by p_jitter of a step.
	static float smrt_step(uint32_t p_step, uint32_t p_steps, float p_jitter, float p_first, float p_last);
	// tan of half a directional light's angular diameter from DirectionalLightData::size, which is
	// 1 - cos(angular diameter).
	static float directional_tan_half_angle(float p_size);
	// The level and mip the projection reads for a receiver: the marking pass's fine request
	// (mark_directional_point()), so the sample finds the page the view asked for.
	static bool clipmap_projection_select(const MapData *p_maps, uint32_t p_table_maps, const uint32_t *p_map_bias, uint32_t p_first, uint32_t p_count, const Vector2i &p_texel0, float p_footprint, int32_t p_lod_bias, uint32_t &r_level, uint32_t &r_mip);
	// clipmap_sample() that also gives the level it found the page in.
	static LookupResult clipmap_sample_level(const uint32_t *p_table, uint32_t p_table_maps, const MapData *p_maps, uint32_t p_first, uint32_t p_count, const Vector2i &p_texel0, uint32_t p_level, uint32_t p_mip, uint32_t &r_level);
	// The face of a spot, omni or area light that holds p_world (the one with the largest w where faces
	// overlap), or MAP_NONE; r_w is the face's w at the point.
	static uint32_t face_projection_select(const MapData *p_maps, uint32_t p_table_maps, uint32_t p_map_base, uint32_t p_light_flags, const Vector3 &p_world, float &r_w);

	// What the CPU models of the projection read. pool holds the physical pool as uint texels, layer 0
	// (static) then layer 1 (dynamic), each pool_width x pool_height.
	struct ProjectionInput {
		const uint32_t *table = nullptr;
		uint32_t table_maps = 0;
		const MapData *maps = nullptr;
		const uint32_t *map_bias = nullptr; // Each map's overflow bias state, or null.
		const uint32_t *pool = nullptr;
		uint32_t pool_width = 0;
		uint32_t pool_height = 0;
		float footprint = 0.0f; // The receiver pixel's footprint in meters.
		int32_t lod_bias = 0;
		uint32_t rays = 0;
		uint32_t steps = 1;
		Vector2 noise;
		bool prefiltered_distant = false;
	};
	// The nearest depth of a pool texel (the larger packed value of the two layers), +INF when empty.
	static float pool_depth(const ProjectionInput &p_in, uint32_t p_texel_x, uint32_t p_texel_y);
	// CPU models of vsm_shadow_directional() and vsm_shadow_local(): 1 lit, 0 shadowed. p_normal is the
	// receiver's world normal; p_tan_half_angle the directional light's (directional_tan_half_angle()).
	// p_radius is a spot or omni light's radius; an area light passes its two edge vectors instead.
	static float shadow_directional(const ProjectionInput &p_in, uint32_t p_map_base, uint32_t p_light_flags, const Vector3 &p_world, const Vector3 &p_normal, float p_tan_half_angle);
	// The moments filter of Prefiltered Distant at level-0 texel position p_t and depth p_depth along the
	// light, on p_level and p_mip (CPU model of vsm_prefiltered_distant()).
	static float prefiltered_distant(const ProjectionInput &p_in, uint32_t p_first, uint32_t p_count, const Vector2 &p_t, float p_depth, uint32_t p_level, uint32_t p_mip);
	static float shadow_local(const ProjectionInput &p_in, uint32_t p_map_base, uint32_t p_light_flags, const Vector3 &p_world, const Vector3 &p_normal, const Vector3 &p_light_position, float p_radius, const Vector3 &p_area_a = Vector3(), const Vector3 &p_area_b = Vector3());

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
		// GodotNext: vsm_settings_debug (#221). Moving lights.
		Transform3D motion_transform;
		bool motion_valid = false;
		uint64_t moved_frame = NEVER_MOVED;
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
