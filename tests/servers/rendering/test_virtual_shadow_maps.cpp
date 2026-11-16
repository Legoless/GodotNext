/**************************************************************************/
/*  test_virtual_shadow_maps.cpp                                          */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_virtual_shadow_maps)

#include "core/math/math_funcs.h"
#include "core/math/random_pcg.h"
#include "servers/rendering/storage/virtual_shadow_map_storage.h"

// GodotNext: vsm_page_pool (#220, plan §21.3 contract S2). The Virtual Shadow Map core without a GPU:
// the sizes and budget V6, the entry format, map allocation and its leaks, the clipmap math and
// the CPU model of the page lookup. godotnext/testbed/tests/vsm/page_table.gd compares the GPU
// lookup and scroll pass with the same CPU model.

namespace TestVirtualShadowMaps {

using VSM = RendererVirtualShadowMapStorage;

// Records the GPU hooks and keeps a CPU copy of the page table that they write, as the GPU would.
class TestVSMStorage : public VSM {
public:
	LocalVector<uint32_t> table;
	uint32_t resize_calls = 0;
	uint32_t clear_calls = 0;
	uint64_t cleared_maps = 0;
	uint32_t scroll_calls = 0;
	LocalVector<uint32_t> scrolled_maps;
	uint32_t pool_calls = 0;
	uint32_t last_pool_pages = 0; // The pool the fake holds.
	bool fail_resize = false;
	bool fail_pool = false;

	uint32_t *map_entries(uint32_t p_map) { return table.ptr() + uint64_t(p_map) * ENTRIES_PER_MAP; }

	uint64_t nonzero_entries() const {
		uint64_t count = 0;
		for (uint32_t entry : table) {
			count += entry != 0 ? 1 : 0;
		}
		return count;
	}

protected:
	bool _table_resize(uint32_t p_old_maps, uint32_t p_new_maps) override {
		if (fail_resize) {
			return false;
		}
		CHECK(table.size() == p_old_maps * ENTRIES_PER_MAP);
		CHECK(p_new_maps > p_old_maps);
		CHECK(p_new_maps % TABLE_GROWTH_MAPS == 0);
		const uint32_t old_size = table.size();
		table.resize(p_new_maps * ENTRIES_PER_MAP);
		for (uint32_t i = old_size; i < table.size(); i++) {
			table[i] = 0;
		}
		resize_calls++;
		return true;
	}
	void _table_clear(uint32_t p_first_map, uint32_t p_map_count) override {
		CHECK(uint64_t(p_first_map + p_map_count) * ENTRIES_PER_MAP <= table.size());
		for (uint64_t i = uint64_t(p_first_map) * ENTRIES_PER_MAP; i < uint64_t(p_first_map + p_map_count) * ENTRIES_PER_MAP; i++) {
			table[i] = 0;
		}
		clear_calls++;
		cleared_maps += p_map_count;
	}
	void _table_scroll(uint32_t p_map, const ClipmapScroll &p_scroll) override {
		clipmap_apply_scroll(map_entries(p_map), p_scroll);
		scroll_calls++;
		scrolled_maps.push_back(p_map);
	}
	bool _pool_resize(uint32_t p_page_count) override {
		pool_calls++;
		if (fail_pool && p_page_count != 0) {
			return false; // As the hook's contract says: the previous pool stays.
		}
		last_pool_pages = p_page_count;
		return true;
	}
};

RID light_rid(uint64_t p_index) {
	return RID::from_uint64(0x1000 + p_index);
}

// Marks every entry of the light's maps allocated, as the allocation pass (#219) would.
void fill_light(TestVSMStorage &r_storage, RID p_light, uint32_t p_page) {
	const VSM::MapRange *range = r_storage.map_get(p_light);
	REQUIRE(range != nullptr);
	for (uint32_t map = range->first; map < range->first + range->count; map++) {
		uint32_t *entries = r_storage.map_entries(map);
		for (uint32_t i = 0; i < VSM::ENTRIES_PER_MAP; i++) {
			entries[i] = VSM::entry_pack(p_page, p_page, VSM::ENTRY_ALLOCATED);
		}
	}
}

} // namespace TestVirtualShadowMaps

using namespace TestVirtualShadowMaps;

TEST_CASE("[VSM] Provisional sizes, the GLSL defines and budget V6") {
	CHECK(VSM::PAGE_SIZE == 64);
	CHECK(VSM::VIRTUAL_SIZE == 4096);
	CHECK(VSM::PAGES_PER_SIDE == 64);
	CHECK(VSM::MIP_COUNT == 7);
	CHECK(VSM::ENTRIES_PER_MAP == 64 * 64 + 32 * 32 + 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2 + 1);
	uint32_t offset = 0;
	for (uint32_t mip = 0; mip < VSM::MIP_COUNT; mip++) {
		CHECK(VSM::mip_entry_offset(mip) == offset);
		offset += (VSM::PAGES_PER_SIDE >> mip) * (VSM::PAGES_PER_SIDE >> mip);
	}
	CHECK(VSM::mip_entry_offset(VSM::MIP_COUNT) == offset);
	CHECK(VSM::MAP_TABLE_BYTES == 5461u * 4u);

	// V6: 64 / 128 / 256 / 512 MiB pools, two R32 layers of 64 x 64 texel pages.
	const uint64_t mib = 1024 * 1024;
	const uint64_t v6_mib[VSM::POOL_QUALITY_MAX] = { 64, 128, 256, 512 };
	const Vector2i grids[VSM::POOL_QUALITY_MAX] = { Vector2i(64, 32), Vector2i(64, 64), Vector2i(128, 64), Vector2i(128, 128) };
	for (uint32_t quality = 0; quality < VSM::POOL_QUALITY_MAX; quality++) {
		const uint32_t pages = VSM::pool_quality_pages(VSM::PoolQuality(quality));
		CHECK(VSM::pool_bytes(pages) == v6_mib[quality] * mib);
		const Vector2i grid = VSM::pool_page_grid(pages);
		CHECK(grid == grids[quality]);
		CHECK(uint32_t(grid.x * grid.y) == pages);
		const uint64_t texture_bytes = uint64_t(grid.x) * VSM::PAGE_SIZE * grid.y * VSM::PAGE_SIZE * VSM::POOL_TEXEL_BYTES * VSM::POOL_LAYERS;
		CHECK(texture_bytes == v6_mib[quality] * mib);
		CHECK(grid.x * int32_t(VSM::PAGE_SIZE) <= 16384);
		CHECK(grid.y * int32_t(VSM::PAGE_SIZE) <= 16384);
	}
	CHECK(VSM::pool_page_grid(3) == Vector2i(2, 2));
	CHECK(VSM::pool_page_grid(512) == Vector2i(32, 16));

	// V6: the page table of 64 lights and a directional light stays within 16 MiB. Omni lights take
	// the most maps, and the directional light takes every clipmap level.
	TestVSMStorage storage;
	for (uint64_t i = 0; i < 64; i++) {
		CHECK(storage.map_allocate(light_rid(i), VSM::MAP_KIND_OMNI) != VSM::MAP_NONE);
	}
	CHECK(storage.map_allocate(light_rid(64), VSM::MAP_KIND_DIRECTIONAL, VSM::CLIPMAP_MAX_LEVELS) != VSM::MAP_NONE);
	CHECK(storage.get_allocated_map_count() == 64 * 6 + 16);
	CHECK(storage.get_table_capacity() == 512);
	CHECK(VSM::table_bytes(storage.get_table_capacity()) == 11184128u);
	CHECK(VSM::table_bytes(storage.get_table_capacity()) <= 16 * mib);

	// The shaders get the sizes from the header, never a copy.
	const String defines = VSM::get_glsl_defines();
	CHECK(defines.contains("#define VSM_PAGE_SIZE 64u\n"));
	CHECK(defines.contains("#define VSM_PAGE_SIZE_SHIFT 6u\n"));
	CHECK(defines.contains("#define VSM_VIRTUAL_SIZE 4096u\n"));
	CHECK(defines.contains("#define VSM_MIP_COUNT 7u\n"));
	CHECK(defines.contains("#define VSM_ENTRIES_PER_MAP 5461u\n"));
	CHECK(defines.contains(vformat("#define VSM_ENTRY_ALLOCATED %du\n", uint32_t(VSM::ENTRY_ALLOCATED))));
	CHECK(defines.contains(vformat("#define VSM_ENTRY_SCROLLED %du\n", uint32_t(VSM::ENTRY_SCROLLED))));
	CHECK(defines.contains("#define VSM_MAP_NONE 4294967295u\n"));
}

TEST_CASE("[VSM] Page-table entries round-trip the page and all eight flags") {
	RandomPCG rng(220);
	const uint32_t all_flags[8] = { VSM::ENTRY_ALLOCATED, VSM::ENTRY_REQUESTED, VSM::ENTRY_DIRTY_STATIC, VSM::ENTRY_DIRTY_DYNAMIC, VSM::ENTRY_COARSE, VSM::ENTRY_UNCACHED, VSM::ENTRY_SCROLLED, VSM::ENTRY_OVERFLOW };
	uint32_t combined = 0;
	for (uint32_t flag : all_flags) {
		CHECK((combined & flag) == 0);
		combined |= flag;
	}
	CHECK(combined == uint32_t(VSM::ENTRY_FLAGS_MASK));
	uint32_t failures = 0;
	for (uint32_t set = 0; set < 256; set++) {
		uint32_t flags = 0;
		for (uint32_t bit = 0; bit < 8; bit++) {
			if (set & (1u << bit)) {
				flags |= all_flags[bit];
			}
		}
		const uint32_t xs[4] = { 0, 4095, rng.rand(4096), rng.rand(4096) };
		const uint32_t ys[4] = { 4095, 0, rng.rand(4096), rng.rand(4096) };
		for (int i = 0; i < 4; i++) {
			const uint32_t entry = VSM::entry_pack(xs[i], ys[i], flags);
			failures += VSM::entry_page_x(entry) != xs[i] ? 1 : 0;
			failures += VSM::entry_page_y(entry) != ys[i] ? 1 : 0;
			failures += VSM::entry_flags(entry) != flags ? 1 : 0;
		}
	}
	CHECK(failures == 0);
	// Coordinates past 12 bits never reach the flags.
	CHECK(VSM::entry_pack(4096 + 7, 4096 + 9, 0) == VSM::entry_pack(7, 9, 0));
	CHECK(VSM::entry_flags(VSM::entry_pack(0xFFFFFFFFu, 0xFFFFFFFFu, 0)) == 0);
}

TEST_CASE("[VSM] Maps of every light kind allocate, persist, free and are reused") {
	TestVSMStorage storage;
	CHECK(VSM::map_kind_map_count(VSM::MAP_KIND_SPOT, 9) == 1);
	CHECK(VSM::map_kind_map_count(VSM::MAP_KIND_OMNI, 9) == 6);
	CHECK(VSM::map_kind_map_count(VSM::MAP_KIND_AREA, 9) == 5);
	CHECK(VSM::map_kind_map_count(VSM::MAP_KIND_DIRECTIONAL, 9) == 9);
	CHECK(VSM::map_kind_map_count(VSM::MAP_KIND_DIRECTIONAL, 0) == 1);
	CHECK(VSM::map_kind_map_count(VSM::MAP_KIND_DIRECTIONAL, 99) == VSM::CLIPMAP_MAX_LEVELS);

	const uint32_t spot = storage.map_allocate(light_rid(1), VSM::MAP_KIND_SPOT);
	const uint32_t omni = storage.map_allocate(light_rid(2), VSM::MAP_KIND_OMNI);
	const uint32_t area = storage.map_allocate(light_rid(3), VSM::MAP_KIND_AREA);
	const uint32_t directional = storage.map_allocate(light_rid(4), VSM::MAP_KIND_DIRECTIONAL, 7);
	CHECK(spot == 0);
	CHECK(omni == 1);
	CHECK(area == 7);
	CHECK(directional == 12);
	CHECK(storage.get_allocated_map_count() == 1 + 6 + 5 + 7);
	CHECK(storage.get_light_count() == 4);
	CHECK(storage.get_table_capacity() == VSM::TABLE_GROWTH_MAPS);
	CHECK(storage.resize_calls == 1);
	CHECK(VSM::light_flags(VSM::MAP_KIND_OMNI, 6) == (2u | (6u << 2)));

	// The same request returns the same maps: caches survive frames.
	CHECK(storage.map_allocate(light_rid(2), VSM::MAP_KIND_OMNI) == omni);
	CHECK(storage.map_allocate(light_rid(4), VSM::MAP_KIND_DIRECTIONAL, 7) == directional);
	CHECK(storage.clear_calls == 0);

	// Freeing clears exactly the light's maps, and the next request reuses them.
	fill_light(storage, light_rid(2), 3);
	fill_light(storage, light_rid(3), 4);
	storage.map_free(light_rid(2));
	CHECK(storage.clear_calls == 1);
	CHECK(storage.cleared_maps == 6);
	for (uint32_t map = omni; map < omni + 6; map++) {
		CHECK(storage.map_entries(map)[0] == 0);
		CHECK(storage.map_entries(map)[VSM::ENTRIES_PER_MAP - 1] == 0);
	}
	CHECK(storage.map_entries(area)[0] != 0);
	CHECK(storage.map_get(light_rid(2)) == nullptr);
	CHECK(storage.map_allocate(light_rid(5), VSM::MAP_KIND_AREA) == omni);
	CHECK(storage.map_allocate(light_rid(6), VSM::MAP_KIND_SPOT) == omni + 5);

	// Another level count moves the light to new maps and clears its old ones.
	storage.map_allocate(light_rid(4), VSM::MAP_KIND_DIRECTIONAL, 9);
	CHECK(storage.cleared_maps == 6 + 7);
	CHECK(storage.map_get(light_rid(4))->count == 9);

	// Freeing an unknown light, or one twice, changes nothing.
	storage.map_free(light_rid(99));
	storage.map_free(light_rid(2));
	CHECK(storage.clear_calls == 2);

	ERR_PRINT_OFF;
	CHECK(storage.map_allocate(RID(), VSM::MAP_KIND_SPOT) == VSM::MAP_NONE);
	CHECK(storage.map_allocate(light_rid(7), VSM::MAP_KIND_MAX) == VSM::MAP_NONE);
	ERR_PRINT_ON;
}

TEST_CASE("[VSM] 10,000 allocate and free cycles leak no map and leave no entry behind") {
	TestVSMStorage storage;
	RandomPCG rng(0x220);
	LocalVector<RID> live;
	uint64_t allocated_maps = 0;
	uint64_t next = 1;
	uint64_t peak_nonzero = 0;
	uint32_t failures = 0;
	for (uint32_t cycle = 0; cycle < 10000; cycle++) {
		if (live.size() < 64 && (live.is_empty() || rng.randf() < 0.5f)) {
			const VSM::MapKind kind = VSM::MapKind(rng.rand(VSM::MAP_KIND_MAX));
			const uint32_t levels = 1 + rng.rand(VSM::CLIPMAP_MAX_LEVELS);
			const RID rid = light_rid(next++);
			if (storage.map_allocate(rid, kind, levels) == VSM::MAP_NONE) {
				failures++;
				continue;
			}
			fill_light(storage, rid, 1 + rng.rand(4095));
			allocated_maps += VSM::map_kind_map_count(kind, levels);
			live.push_back(rid);
		} else {
			const uint32_t index = rng.rand(live.size());
			storage.map_free(live[index]);
			live.remove_at_unordered(index);
		}
		if (cycle % 1000 == 999) {
			peak_nonzero = MAX(peak_nonzero, storage.nonzero_entries());
		}
	}
	CHECK(failures == 0);
	CHECK(peak_nonzero > 0);
	for (const RID &rid : live) {
		storage.map_free(rid);
	}
	CHECK(storage.get_allocated_map_count() == 0);
	CHECK(storage.get_light_count() == 0);
	CHECK(storage.get_free_map_count() == storage.get_table_capacity());
	CHECK(storage.get_free_range_count() == 1);
	CHECK(storage.cleared_maps == allocated_maps);
	CHECK(storage.nonzero_entries() == 0);
	CHECK(storage.get_table_capacity() <= VSM::TABLE_MAX_MAPS);
	MESSAGE(vformat("Maps allocated over the cycles: %d. Page-table capacity: %d maps.", allocated_maps, storage.get_table_capacity()));
}

TEST_CASE("[VSM] The page table grows to its cap and fails cleanly when full") {
	TestVSMStorage storage;
	uint64_t next = 1;
	uint32_t capacity = 0;
	while (true) {
		const uint32_t first = storage.map_allocate(light_rid(next), VSM::MAP_KIND_DIRECTIONAL, VSM::CLIPMAP_MAX_LEVELS);
		if (first == VSM::MAP_NONE) {
			break;
		}
		CHECK(storage.get_table_capacity() % VSM::TABLE_GROWTH_MAPS == 0);
		CHECK(storage.get_table_capacity() >= capacity);
		capacity = storage.get_table_capacity();
		next++;
	}
	CHECK(storage.get_table_capacity() == VSM::TABLE_MAX_MAPS);
	CHECK(storage.get_allocated_map_count() == VSM::TABLE_MAX_MAPS);
	CHECK(storage.get_failed_allocation_count() == 1);
	CHECK(storage.map_get(light_rid(next)) == nullptr);
	CHECK(storage.resize_calls == VSM::TABLE_MAX_MAPS / VSM::TABLE_GROWTH_MAPS);

	// A freed light makes room again.
	storage.map_free(light_rid(3));
	CHECK(storage.map_allocate(light_rid(next), VSM::MAP_KIND_OMNI) == 2 * VSM::CLIPMAP_MAX_LEVELS);
	CHECK(storage.map_allocate(light_rid(next + 1), VSM::MAP_KIND_DIRECTIONAL, VSM::CLIPMAP_MAX_LEVELS) == VSM::MAP_NONE);

	// A failed growth changes nothing either.
	TestVSMStorage failing;
	failing.fail_resize = true;
	CHECK(failing.map_allocate(light_rid(1), VSM::MAP_KIND_SPOT) == VSM::MAP_NONE);
	CHECK(failing.get_table_capacity() == 0);
	CHECK(failing.get_light_count() == 0);
	CHECK(failing.get_failed_allocation_count() == 1);
}

TEST_CASE("[VSM] The CPU model of the page lookup") {
	const uint32_t maps = 3;
	LocalVector<uint32_t> table;
	table.resize(maps * VSM::ENTRIES_PER_MAP);
	for (uint32_t &entry : table) {
		entry = 0;
	}
	// Map 0: page (3, 5) of mip 0 maps physical page (10, 20); page (0, 0) of mip 2 maps (7, 8).
	table[VSM::entry_index(0, 0, 3, 5)] = VSM::entry_pack(10, 20, VSM::ENTRY_ALLOCATED | VSM::ENTRY_DIRTY_STATIC);
	table[VSM::entry_index(0, 2, 0, 0)] = VSM::entry_pack(7, 8, VSM::ENTRY_ALLOCATED);
	// Map 1: only the coarsest mip. Map 2: nothing, and flags without ALLOCATED do not count.
	table[VSM::entry_index(1, VSM::MIP_COUNT - 1, 0, 0)] = VSM::entry_pack(1, 2, VSM::ENTRY_ALLOCATED);
	table[VSM::entry_index(2, 0, 0, 0)] = VSM::entry_pack(5, 5, VSM::ENTRY_REQUESTED | VSM::ENTRY_SCROLLED);

	// A texel inside page (3, 5) at mip 0: texel (3 * 64 + 17, 5 * 64 + 40).
	const Vector2 inside((3 * 64 + 17 + 0.5f) / 4096.0f, (5 * 64 + 40 + 0.5f) / 4096.0f);
	VSM::LookupResult hit = VSM::lookup(table.ptr(), maps, 0, inside, 0);
	CHECK(hit.valid);
	CHECK(hit.mip == 0);
	CHECK(hit.texel_x == 10 * 64 + 17);
	CHECK(hit.texel_y == 20 * 64 + 40);
	CHECK(hit.entry == VSM::entry_pack(10, 20, VSM::ENTRY_ALLOCATED | VSM::ENTRY_DIRTY_STATIC));

	// Page (0, 0) of mip 0 is not resident: mip 1 is not either, mip 2 is. At mip 2 the texel is (3, 6).
	const Vector2 corner(12.5f / 4096.0f, 25.5f / 4096.0f);
	VSM::LookupResult fallback = VSM::lookup(table.ptr(), maps, 0, corner, 0);
	CHECK(fallback.valid);
	CHECK(fallback.mip == 2);
	CHECK(fallback.texel_x == 7 * 64 + 3);
	CHECK(fallback.texel_y == 8 * 64 + 6);
	CHECK_FALSE(VSM::lookup(table.ptr(), maps, 0, corner, 0, false).valid);
	CHECK(VSM::lookup(table.ptr(), maps, 0, corner, 2, false).valid);

	// Map 1 falls back to its single coarsest page from any mip; the texel is uv * 64.
	VSM::LookupResult coarse = VSM::lookup(table.ptr(), maps, 1, Vector2(0.5f, 0.25f), 0);
	CHECK(coarse.valid);
	CHECK(coarse.mip == VSM::MIP_COUNT - 1);
	CHECK(coarse.texel_x == 1 * 64 + 32);
	CHECK(coarse.texel_y == 2 * 64 + 16);

	// Nothing resident: invalid, mip MIP_COUNT.
	VSM::LookupResult miss = VSM::lookup(table.ptr(), maps, 2, Vector2(0.001f, 0.001f), 0);
	CHECK_FALSE(miss.valid);
	CHECK(miss.mip == VSM::MIP_COUNT);
	CHECK(miss.entry == 0);
	CHECK(miss.texel_x == 0);

	// uv clamps to [0, 1] and 1.0 is the last texel; mips past the last clamp to it.
	CHECK(VSM::lookup(table.ptr(), maps, 1, Vector2(1.0f, 1.0f), 0).texel_x == 1 * 64 + 63);
	CHECK(VSM::lookup(table.ptr(), maps, 1, Vector2(7.0f, -3.0f), 0).texel_x == 1 * 64 + 63);
	CHECK(VSM::lookup(table.ptr(), maps, 1, Vector2(7.0f, -3.0f), 0).texel_y == 2 * 64);
	CHECK(VSM::lookup(table.ptr(), maps, 1, Vector2(0.5f, 0.5f), 99).mip == VSM::MIP_COUNT - 1);

	// A light without maps, a map past the table and a table told to end before the map all miss,
	// without reading the table, as on the GPU.
	CHECK_FALSE(VSM::lookup(table.ptr(), maps, maps, Vector2(), 0).valid);
	CHECK_FALSE(VSM::lookup(table.ptr(), maps, VSM::MAP_NONE, Vector2(0.5f, 0.25f), 0).valid);
	CHECK(VSM::lookup(table.ptr(), maps, VSM::MAP_NONE, Vector2(0.5f, 0.25f), 0).mip == VSM::MIP_COUNT);
	CHECK(VSM::lookup(table.ptr(), 2, 1, Vector2(0.5f, 0.25f), 0).valid);
	CHECK_FALSE(VSM::lookup(table.ptr(), 1, 1, Vector2(0.5f, 0.25f), 0).valid);

	// vsm_light_map(): a light's level or face, never past its count, never from MAP_NONE.
	const uint32_t omni_flags = VSM::light_flags(VSM::MAP_KIND_OMNI, 6);
	CHECK(VSM::light_map(40, omni_flags, 0) == 40);
	CHECK(VSM::light_map(40, omni_flags, 5) == 45);
	CHECK(VSM::light_map(40, omni_flags, 6) == VSM::MAP_NONE);
	CHECK(VSM::light_map(VSM::MAP_NONE, omni_flags, 0) == VSM::MAP_NONE);
	CHECK(VSM::light_map(VSM::MAP_NONE, omni_flags, 1) == VSM::MAP_NONE); // Not map 0.
	CHECK(VSM::light_map(7, 0, 0) == VSM::MAP_NONE); // vsm_flags 0: a light without maps.
	CHECK(VSM::light_map(3, VSM::light_flags(VSM::MAP_KIND_DIRECTIONAL, 16), 15) == 18);

	// Against a reference written with division instead of shifts, on random tables and queries.
	RandomPCG rng(65536);
	for (uint32_t &entry : table) {
		entry = rng.randf() < 0.3f ? VSM::entry_pack(rng.rand(4096), rng.rand(4096), VSM::ENTRY_ALLOCATED) : (rng.randf() < 0.5f ? uint32_t(VSM::ENTRY_REQUESTED) : 0u);
	}
	uint32_t failures = 0;
	uint32_t fallbacks = 0;
	for (uint32_t q = 0; q < 65536; q++) {
		const uint32_t map = rng.rand(maps);
		const Vector2 uv(-0.1f + 1.2f * rng.randf(), -0.1f + 1.2f * rng.randf());
		const uint32_t mip = rng.rand(VSM::MIP_COUNT + 1);
		const VSM::LookupResult result = VSM::lookup(table.ptr(), maps, map, uv, mip);
		VSM::LookupResult reference;
		for (uint32_t m = MIN(mip, VSM::MIP_COUNT - 1); m < VSM::MIP_COUNT; m++) {
			const uint32_t size = VSM::VIRTUAL_SIZE / (1u << m);
			const uint32_t tx = MIN(uint32_t(CLAMP(double(uv.x), 0.0, 1.0) * size), size - 1);
			const uint32_t ty = MIN(uint32_t(CLAMP(double(uv.y), 0.0, 1.0) * size), size - 1);
			const uint32_t pages = VSM::PAGES_PER_SIDE / (1u << m);
			const uint32_t entry = table[map * VSM::ENTRIES_PER_MAP + VSM::mip_entry_offset(m) + (ty / VSM::PAGE_SIZE) * pages + tx / VSM::PAGE_SIZE];
			if (entry & VSM::ENTRY_ALLOCATED) {
				reference.valid = true;
				reference.mip = m;
				reference.entry = entry;
				reference.texel_x = VSM::entry_page_x(entry) * VSM::PAGE_SIZE + tx % VSM::PAGE_SIZE;
				reference.texel_y = VSM::entry_page_y(entry) * VSM::PAGE_SIZE + ty % VSM::PAGE_SIZE;
				break;
			}
		}
		failures += (result.valid != reference.valid || result.mip != reference.mip || result.entry != reference.entry || result.texel_x != reference.texel_x || result.texel_y != reference.texel_y) ? 1 : 0;
		fallbacks += (result.valid && result.mip > MIN(mip, VSM::MIP_COUNT - 1)) ? 1 : 0;
	}
	CHECK(failures == 0);
	CHECK(fallbacks > 0);
}

TEST_CASE("[VSM] Clipmap lookups fall back only inside each mip's window") {
	const uint32_t maps = 2;
	LocalVector<uint32_t> table;
	table.resize(maps * VSM::ENTRIES_PER_MAP);
	for (uint32_t &entry : table) {
		entry = 0;
	}
	// A level whose window moved one page right of the centered origin: mip 0 holds absolute pages
	// [-31, 33), mip 1 starts at floor(-31 / 2) = -16 and holds [-16, 16), so its slot 16 holds
	// absolute mip 1 page -16, the level's near edge.
	const Vector2i origin(-31, -32);
	const uint32_t near_entry = VSM::entry_pack(9, 4, VSM::ENTRY_ALLOCATED);
	table[VSM::entry_index(0, 1, 16, 0)] = near_entry;
	// The near edge, absolute mip 0 page -31: not resident at mip 0, so mip 1's page -16 answers.
	const Vector2i near_texel(-31 * 64 + 10, 5);
	VSM::LookupResult near = VSM::lookup_clipmap(table.ptr(), maps, 0, origin, near_texel, 0);
	CHECK(near.valid);
	CHECK(near.mip == 1);
	CHECK(near.entry == near_entry);
	CHECK(near.texel_x == 9 * 64 + 37); // floor(-1974 / 2) = -987, which is 37 modulo 64.
	CHECK(near.texel_y == 4 * 64 + 2);
	// The far edge, absolute mip 0 page 32: inside the level, but its mip 1 page 16 is outside the
	// mip 1 window and shares slot 16 with page -16, 4 m away. The clipmap lookup misses; the plain
	// lookup on the wrapped texel would return the near edge's texel.
	const Vector2i far_texel(32 * 64 + 10, 5);
	CHECK_FALSE(VSM::lookup_clipmap(table.ptr(), maps, 0, origin, far_texel, 0).valid);
	const Vector2 far_uv((float(far_texel.x & 4095) + 0.5f) / 4096.0f, (float(far_texel.y & 4095) + 0.5f) / 4096.0f);
	CHECK(VSM::lookup(table.ptr(), maps, 0, far_uv, 0).valid);
	// Outside the level, or with no map, it misses too.
	CHECK_FALSE(VSM::lookup_clipmap(table.ptr(), maps, 0, origin, Vector2i(33 * 64, 5), 1).valid);
	CHECK_FALSE(VSM::lookup_clipmap(table.ptr(), maps, 0, origin, Vector2i(-31 * 64 - 1, 5), 1).valid);
	CHECK_FALSE(VSM::lookup_clipmap(table.ptr(), maps, VSM::MAP_NONE, origin, near_texel, 0).valid);
	CHECK_FALSE(VSM::lookup_clipmap(table.ptr(), maps, maps, origin, near_texel, 0).valid);
	// An origin 63 modulo 64: the single mip 6 page covers the window's first column only.
	table[VSM::entry_index(1, VSM::MIP_COUNT - 1, 0, 0)] = VSM::entry_pack(2, 3, VSM::ENTRY_ALLOCATED);
	const Vector2i origin63(63, -64);
	CHECK(VSM::lookup_clipmap(table.ptr(), maps, 1, origin63, Vector2i(63 * 64 + 7, -64 * 64), 0).valid);
	CHECK_FALSE(VSM::lookup_clipmap(table.ptr(), maps, 1, origin63, Vector2i(64 * 64 + 7, -64 * 64), 0).valid);
	CHECK(VSM::lookup(table.ptr(), maps, 1, Vector2((float((64 * 64 + 7) & 4095) + 0.5f) / 4096.0f, 0.0f), 0).valid);

	// Against a reference written with floor division on absolute pages, which also tries every
	// coarser mip after one outside its window (the model stops there: none is inside again).
	RandomPCG rng(0x220c);
	for (uint32_t &entry : table) {
		entry = rng.randf() < 0.3f ? VSM::entry_pack(rng.rand(4096), rng.rand(4096), VSM::ENTRY_ALLOCATED) : 0u;
	}
	auto floor_div = [](int64_t p_value, int64_t p_divisor) {
		return int64_t(Math::floor(double(p_value) / double(p_divisor)));
	};
	uint32_t failures = 0;
	uint32_t window_stops = 0;
	uint32_t fallbacks = 0;
	for (uint32_t q = 0; q < 65536; q++) {
		const uint32_t map = rng.rand(maps);
		Vector2i o;
		Vector2i texel;
		for (int axis = 0; axis < 2; axis++) {
			o[axis] = rng.rand(4) == 0 ? 64 * (int32_t(rng.rand(9)) - 4) + 63 : int32_t(rng.rand(1001)) - 500;
			texel[axis] = o[axis] * 64 + int32_t(rng.rand(4096 + 256)) - 128;
		}
		const uint32_t mip = rng.rand(VSM::MIP_COUNT + 1);
		const VSM::LookupResult result = VSM::lookup_clipmap(table.ptr(), maps, map, o, texel, mip);
		VSM::LookupResult reference;
		const bool inside = floor_div(texel.x, 64) >= o.x && floor_div(texel.x, 64) < o.x + 64 && floor_div(texel.y, 64) >= o.y && floor_div(texel.y, 64) < o.y + 64;
		for (uint32_t m = MIN(mip, VSM::MIP_COUNT - 1); inside && m < VSM::MIP_COUNT; m++) {
			const int64_t pages = int64_t(VSM::PAGES_PER_SIDE >> m);
			const int64_t page_x = floor_div(texel.x, int64_t(64) << m);
			const int64_t page_y = floor_div(texel.y, int64_t(64) << m);
			const int64_t start_x = floor_div(o.x, int64_t(1) << m);
			const int64_t start_y = floor_div(o.y, int64_t(1) << m);
			if (page_x < start_x || page_x >= start_x + pages || page_y < start_y || page_y >= start_y + pages) {
				continue;
			}
			const uint32_t entry = table[map * VSM::ENTRIES_PER_MAP + VSM::mip_entry_offset(m) + uint32_t(Math::posmod(page_y, pages) * pages + Math::posmod(page_x, pages))];
			if (entry & VSM::ENTRY_ALLOCATED) {
				reference.valid = true;
				reference.mip = m;
				reference.entry = entry;
				reference.texel_x = VSM::entry_page_x(entry) * 64 + uint32_t(Math::posmod(floor_div(texel.x, int64_t(1) << m), int64_t(64)));
				reference.texel_y = VSM::entry_page_y(entry) * 64 + uint32_t(Math::posmod(floor_div(texel.y, int64_t(1) << m), int64_t(64)));
				break;
			}
		}
		failures += (result.valid != reference.valid || result.mip != reference.mip || result.entry != reference.entry || result.texel_x != reference.texel_x || result.texel_y != reference.texel_y) ? 1 : 0;
		fallbacks += (result.valid && result.mip > MIN(mip, VSM::MIP_COUNT - 1)) ? 1 : 0;
		const Vector2 uv((float(texel.x & 4095) + 0.5f) / 4096.0f, (float(texel.y & 4095) + 0.5f) / 4096.0f);
		const VSM::LookupResult wrapped = VSM::lookup(table.ptr(), maps, map, uv, mip);
		window_stops += (wrapped.valid != result.valid || wrapped.entry != result.entry || wrapped.mip != result.mip) ? 1 : 0;
	}
	CHECK(failures == 0);
	CHECK(fallbacks > 0);
	CHECK(window_stops > 0);
}

TEST_CASE("[VSM] Clipmap levels cover the max distance and origins snap to whole pages") {
	CHECK(VSM::clipmap_level_extent(0) == doctest::Approx(4.0));
	CHECK(VSM::clipmap_level_extent(5) == doctest::Approx(128.0));
	CHECK(VSM::clipmap_page_world_size(0) == doctest::Approx(0.0625));
	// The default directional max distance of 100 m: the 7th level (256 m) reaches 124 m.
	CHECK(VSM::clipmap_level_count(100.0f) == 7);
	CHECK(VSM::clipmap_level_count(0.0f) == 1);
	CHECK(VSM::clipmap_level_count(1.0f) == 1);
	CHECK(VSM::clipmap_level_count(1.95f) == 2);
	CHECK(VSM::clipmap_level_count(1e9f) == VSM::CLIPMAP_MAX_LEVELS);
	for (uint32_t count = 1; count < VSM::CLIPMAP_MAX_LEVELS; count++) {
		const float reach = VSM::clipmap_level_extent(count - 1) * 0.5f - VSM::clipmap_page_world_size(count - 1);
		CHECK(VSM::clipmap_level_count(reach) == count);
		CHECK(VSM::clipmap_level_count(reach * 1.01f) == count + 1);
	}

	// The window is centered on the camera's page.
	CHECK(VSM::clipmap_level_origin(Vector2(0.0f, 0.0f), 0) == Vector2i(-32, -32));
	CHECK(VSM::clipmap_level_origin(Vector2(0.0624f, 0.0f), 0) == Vector2i(-32, -32));
	CHECK(VSM::clipmap_level_origin(Vector2(0.0626f, 0.0f), 0) == Vector2i(-31, -32));
	CHECK(VSM::clipmap_level_origin(Vector2(-0.01f, 0.0f), 0) == Vector2i(-33, -32));
	CHECK(VSM::clipmap_level_origin(Vector2(10.0f, -10.0f), 3) == Vector2i(20 - 32, -20 - 32));

	// Mip origins are floor divisions, also below zero.
	CHECK(VSM::clipmap_mip_origin(Vector2i(5, -5), 0) == Vector2i(5, -5));
	CHECK(VSM::clipmap_mip_origin(Vector2i(5, -5), 1) == Vector2i(2, -3));
	CHECK(VSM::clipmap_mip_origin(Vector2i(-1, -64), 6) == Vector2i(-1, -1));
	CHECK(VSM::clipmap_mip_origin(Vector2i(-65, 64), 6) == Vector2i(-2, 1));
}

TEST_CASE("[VSM] Clipmap scroll offsets match a page-ownership model at every mip") {
	// One page right: column 0 of mip 0 wraps; mip 1 does not move until the origin crosses 2.
	VSM::ClipmapScroll scroll = VSM::clipmap_scroll(Vector2i(0, 0), Vector2i(1, 0));
	CHECK(scroll.any);
	CHECK(scroll.mips[0].col_start == 0);
	CHECK(scroll.mips[0].col_count == 1);
	CHECK(scroll.mips[0].row_count == 0);
	CHECK(scroll.mips[1].col_count == 0);
	scroll = VSM::clipmap_scroll(Vector2i(1, 0), Vector2i(2, 0));
	CHECK(scroll.mips[1].col_start == 0);
	CHECK(scroll.mips[1].col_count == 1);
	// One page left from -1: page -2 enters, into slot 62.
	scroll = VSM::clipmap_scroll(Vector2i(-1, 0), Vector2i(-2, 0));
	CHECK(scroll.mips[0].col_start == 62);
	CHECK(scroll.mips[0].col_count == 1);
	CHECK(scroll.mips[1].col_count == 0);
	scroll = VSM::clipmap_scroll(Vector2i(-2, 0), Vector2i(-3, 0));
	CHECK(scroll.mips[1].col_start == 30);
	CHECK(scroll.mips[1].col_count == 1);
	// A whole level or more: every slot.
	scroll = VSM::clipmap_scroll(Vector2i(0, 0), Vector2i(0, -64));
	CHECK(scroll.mips[0].row_count == 64);
	CHECK(scroll.mips[VSM::MIP_COUNT - 1].row_count == 1);
	CHECK_FALSE(VSM::clipmap_scroll(Vector2i(7, -9), Vector2i(7, -9)).any);

	// Random walks with negative origins and steps past a whole level. A slot changes when the
	// absolute page it holds (the one in the mip's window congruent to it) changes.
	RandomPCG rng(3);
	Vector2i origin(-40, 17);
	uint32_t mismatches = 0;
	uint32_t moved_slots = 0;
	for (int step = 0; step < 400; step++) {
		const Vector2i next = origin + Vector2i(int32_t(rng.rand(141)) - 70, int32_t(rng.rand(141)) - 70);
		scroll = VSM::clipmap_scroll(origin, next);
		for (uint32_t mip = 0; mip < VSM::MIP_COUNT; mip++) {
			const int32_t pages = int32_t(VSM::PAGES_PER_SIDE >> mip);
			const Vector2i from = VSM::clipmap_mip_origin(origin, mip);
			const Vector2i to = VSM::clipmap_mip_origin(next, mip);
			for (int32_t y = 0; y < pages; y++) {
				for (int32_t x = 0; x < pages; x++) {
					bool changed = false;
					const int32_t slot[2] = { x, y };
					for (int axis = 0; axis < 2; axis++) {
						// The page in [start, start + pages) congruent to the slot.
						const int64_t old_page = from[axis] + Math::posmod(int64_t(slot[axis]) - from[axis], int64_t(pages));
						const int64_t new_page = to[axis] + Math::posmod(int64_t(slot[axis]) - to[axis], int64_t(pages));
						changed = changed || old_page != new_page;
					}
					moved_slots += changed ? 1 : 0;
					mismatches += changed != VSM::clipmap_scroll_hits(scroll, mip, uint32_t(x), uint32_t(y)) ? 1 : 0;
				}
			}
		}
		origin = next;
	}
	CHECK(mismatches == 0);
	CHECK(moved_slots > 0);

	// The CPU scroll marks allocated entries only, and keeps their pages.
	LocalVector<uint32_t> entries;
	entries.resize(VSM::ENTRIES_PER_MAP);
	for (uint32_t i = 0; i < entries.size(); i++) {
		entries[i] = (i % 3) ? VSM::entry_pack(i % 4096, 9, VSM::ENTRY_ALLOCATED) : 0;
	}
	scroll = VSM::clipmap_scroll(Vector2i(0, 0), Vector2i(3, 0));
	const uint32_t marked = VSM::clipmap_apply_scroll(entries.ptr(), scroll);
	uint32_t expected = 0;
	for (uint32_t mip = 0; mip < VSM::MIP_COUNT; mip++) {
		const uint32_t pages = VSM::PAGES_PER_SIDE >> mip;
		for (uint32_t y = 0; y < pages; y++) {
			for (uint32_t x = 0; x < pages; x++) {
				const uint32_t i = VSM::mip_entry_offset(mip) + y * pages + x;
				const bool hit = VSM::clipmap_scroll_hits(scroll, mip, x, y);
				if (i % 3) {
					expected += hit ? 1 : 0;
					CHECK(VSM::entry_page_x(entries[i]) == i % 4096);
					CHECK(((entries[i] & VSM::ENTRY_SCROLLED) != 0) == hit);
				} else {
					CHECK(entries[i] == 0);
				}
			}
		}
	}
	CHECK(marked == expected);
	CHECK(marked > 0);
}

TEST_CASE("[VSM] Directional lights allocate their levels and scroll only the pages that wrap") {
	TestVSMStorage storage;
	const RID sun = light_rid(1);
	const Basis basis;
	const uint32_t first = storage.directional_update(sun, basis, Vector3(0, 0, 0), 100.0f);
	REQUIRE(first != VSM::MAP_NONE);
	CHECK(storage.map_get(sun)->count == 7);
	CHECK(storage.map_get(sun)->kind == VSM::MAP_KIND_DIRECTIONAL);
	CHECK(storage.scroll_calls == 0);
	for (uint32_t level = 0; level < 7; level++) {
		CHECK(storage.get_clipmap_origin(sun, level) == Vector2i(-32, -32));
	}
	fill_light(storage, sun, 11);

	// Still: nothing scrolls. One level-0 page along the light's X: only level 0 scrolls, one column.
	storage.directional_update(sun, basis, Vector3(0.01f, 0, 0), 100.0f);
	CHECK(storage.scroll_calls == 0);
	storage.directional_update(sun, basis, Vector3(0.07f, 0, -3.0f), 100.0f);
	CHECK(storage.scroll_calls == 1);
	CHECK(storage.scrolled_maps[0] == first);
	CHECK(storage.get_clipmap_origin(sun, 0) == Vector2i(-31, -32));
	CHECK(storage.get_clipmap_origin(sun, 1) == Vector2i(-32, -32));
	// Moving along the light direction (Z) moves nothing.
	uint32_t scrolled = 0;
	for (uint32_t i = 0; i < VSM::ENTRIES_PER_MAP; i++) {
		scrolled += (storage.map_entries(first)[i] & VSM::ENTRY_SCROLLED) ? 1 : 0;
	}
	VSM::ClipmapScroll expected_scroll = VSM::clipmap_scroll(Vector2i(-32, -32), Vector2i(-31, -32));
	uint32_t expected = 0;
	for (uint32_t mip = 0; mip < VSM::MIP_COUNT; mip++) {
		const uint32_t pages = VSM::PAGES_PER_SIDE >> mip;
		for (uint32_t y = 0; y < pages; y++) {
			for (uint32_t x = 0; x < pages; x++) {
				expected += VSM::clipmap_scroll_hits(expected_scroll, mip, x, y) ? 1 : 0;
			}
		}
	}
	CHECK(scrolled == expected);
	CHECK(scrolled == 64);
	CHECK((storage.map_entries(first + 1)[0] & VSM::ENTRY_SCROLLED) == 0);

	// A turned light re-renders every level.
	storage.directional_update(sun, Basis(Vector3(0, 1, 0), 0.1f), Vector3(0.07f, 0, -3.0f), 100.0f);
	CHECK(storage.scroll_calls == 1 + 7);

	// A shorter max distance takes fewer levels: new maps, the old ones cleared.
	const uint64_t cleared = storage.cleared_maps;
	storage.directional_update(sun, basis, Vector3(), 10.0f);
	CHECK(storage.map_get(sun)->count == VSM::clipmap_level_count(10.0f));
	CHECK(storage.cleared_maps == cleared + 7);
	CHECK(storage.scroll_calls == 8);
}

TEST_CASE("[VSM] The pool is reallocated only when its size changes") {
	TestVSMStorage storage;
	CHECK(storage.pool_create(2048));
	CHECK(storage.pool_calls == 1);
	CHECK(storage.last_pool_pages == 2048);
	CHECK(storage.pool_create(2048));
	CHECK(storage.pool_calls == 1);
	CHECK(storage.get_pool_reallocation_count() == 1);
	CHECK(storage.pool_create(16384));
	CHECK(storage.pool_calls == 2);
	CHECK(storage.get_pool_page_count() == 16384);
	storage.pool_free();
	CHECK(storage.pool_calls == 3);
	CHECK(storage.last_pool_pages == 0);
	CHECK(storage.get_pool_page_count() == 0);
	ERR_PRINT_OFF;
	CHECK_FALSE(storage.pool_create(0));
	CHECK_FALSE(storage.pool_create(VSM::POOL_MAX_PAGES + 1));
	ERR_PRINT_ON;
	CHECK(storage.pool_calls == 3);
}

TEST_CASE("[VSM] A failed pool resize keeps the previous pool") {
	TestVSMStorage storage;
	CHECK(storage.pool_create(8192));
	storage.fail_pool = true;
	ERR_PRINT_OFF;
	CHECK_FALSE(storage.pool_create(16384));
	ERR_PRINT_ON;
	CHECK(storage.get_pool_page_count() == 8192);
	CHECK(storage.last_pool_pages == 8192);
	CHECK(storage.get_pool_reallocation_count() == 1);
	// The page count still describes the pool the hook kept, so asking for it again is no change.
	CHECK(storage.pool_create(8192));
	CHECK(storage.pool_calls == 2);
	storage.fail_pool = false;
	CHECK(storage.pool_create(16384));
	CHECK(storage.get_pool_page_count() == 16384);
	CHECK(storage.last_pool_pages == 16384);
	CHECK(storage.get_pool_reallocation_count() == 2);
}

TEST_CASE("[VSM] Stale lights make room before the page table grows") {
	// A light drawn in the frame before stays; one drawn in neither this frame nor the one before
	// is evicted for a light that does not fit, before the table grows.
	{
		TestVSMStorage storage;
		storage.set_frame(1);
		const RID stale = light_rid(1);
		const uint32_t stale_first = storage.map_allocate(stale, VSM::MAP_KIND_OMNI);
		fill_light(storage, stale, 5);
		storage.set_frame(2);
		for (uint64_t i = 0; i < 21; i++) {
			CHECK(storage.map_allocate(light_rid(100 + i), VSM::MAP_KIND_OMNI) != VSM::MAP_NONE);
		}
		CHECK(storage.get_table_capacity() == 2 * VSM::TABLE_GROWTH_MAPS);
		CHECK(storage.map_get(stale) != nullptr);
		CHECK(storage.get_evicted_light_count() == 0);

		storage.set_frame(3);
		for (uint64_t i = 0; i < 20; i++) {
			storage.map_allocate(light_rid(100 + i), VSM::MAP_KIND_OMNI);
		}
		CHECK(storage.map_touch(light_rid(120)) != nullptr); // A reflection probe's view.
		for (uint64_t i = 0; i < 20; i++) {
			CHECK(storage.map_allocate(light_rid(200 + i), VSM::MAP_KIND_OMNI) != VSM::MAP_NONE);
		}
		CHECK(storage.get_evicted_light_count() == 0);
		const uint64_t cleared = storage.cleared_maps;
		CHECK(storage.map_allocate(light_rid(300), VSM::MAP_KIND_OMNI) == stale_first);
		CHECK(storage.get_evicted_light_count() == 1);
		CHECK(storage.get_evicted_map_count() == 6);
		CHECK(storage.cleared_maps == cleared + 6);
		CHECK(storage.map_get(stale) == nullptr);
		CHECK(storage.map_get(light_rid(120)) != nullptr);
		CHECK(storage.get_table_capacity() == 2 * VSM::TABLE_GROWTH_MAPS);
		CHECK(storage.get_failed_allocation_count() == 0);
	}

	// 200 shadowed omni lights and a directional light with every level, 64 of the omni lights in
	// view at a time: the view slides by 8 lights a frame. The table stays at the 512 maps of V6's
	// 64 lights, and a light in every frame keeps its maps.
	{
		TestVSMStorage storage;
		const RID sun = light_rid(0);
		uint32_t sun_first = VSM::MAP_NONE;
		uint32_t peak_capacity = 0;
		uint32_t failures = 0;
		for (uint64_t frame = 1; frame <= 400; frame++) {
			storage.set_frame(frame);
			const uint32_t first = storage.map_allocate(sun, VSM::MAP_KIND_DIRECTIONAL, VSM::CLIPMAP_MAX_LEVELS);
			if (frame == 1) {
				sun_first = first;
			}
			failures += first != sun_first ? 1 : 0;
			const uint64_t start = (frame * 8) % 200;
			for (uint64_t i = 0; i < 64; i++) {
				failures += storage.map_allocate(light_rid(1 + (start + i) % 200), VSM::MAP_KIND_OMNI) == VSM::MAP_NONE ? 1 : 0;
			}
			peak_capacity = MAX(peak_capacity, storage.get_table_capacity());
		}
		CHECK(failures == 0);
		CHECK(storage.get_failed_allocation_count() == 0);
		CHECK(peak_capacity == 512);
		CHECK(VSM::table_bytes(peak_capacity) <= 16 * 1024 * 1024);
		CHECK(storage.get_evicted_light_count() > 0);
		CHECK(storage.get_allocated_map_count() <= storage.get_table_capacity());
	}

	// 1,064 distinct omni lights over time, never freed, 64 in view at a time: past the 682 that
	// would fill the 4096-map cap, every light in view still gets its maps.
	{
		TestVSMStorage storage;
		uint32_t failures = 0;
		for (uint64_t frame = 1; frame <= 126; frame++) {
			storage.set_frame(frame);
			for (uint64_t i = 0; i < 64; i++) {
				failures += storage.map_allocate(light_rid(1 + (frame - 1) * 8 + i), VSM::MAP_KIND_OMNI) == VSM::MAP_NONE ? 1 : 0;
			}
		}
		CHECK(failures == 0);
		CHECK(storage.get_failed_allocation_count() == 0);
		CHECK(storage.get_table_capacity() == 4 * VSM::TABLE_GROWTH_MAPS);
		CHECK(storage.get_evicted_light_count() + storage.get_light_count() == 1 + 125 * 8 + 63);
	}
}
