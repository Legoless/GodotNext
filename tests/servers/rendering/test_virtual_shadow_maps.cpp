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

#include <cmath>
#include <cstring>

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

// GodotNext: vsm_page_marking (#219, plan §21.3 contract S3). The CPU models of the page marking and
// allocation passes. godotnext/testbed/tests/vsm/page_marking.gd compares the GPU passes with them.

TEST_CASE("[VSM] Marking and allocation constants and their GLSL defines") {
	CHECK(VSM::COARSE_FIRST_MIP == VSM::MIP_COUNT - 2);
	CHECK(VSM::OVERFLOW_MAX_BIAS == 3);
	CHECK(VSM::OVERFLOW_DECAY_FRAMES == 30);
	for (uint32_t quality = 0; quality < VSM::POOL_QUALITY_MAX; quality++) {
		const uint32_t frames = VSM::cache_frames(VSM::PoolQuality(quality));
		CHECK(frames > 0);
		CHECK(frames <= VSM::MAX_CACHE_FRAMES);
		CHECK(VSM::request_capacity(VSM::pool_quality_pages(VSM::PoolQuality(quality))) >= VSM::pool_quality_pages(VSM::PoolQuality(quality)));
	}
	CHECK(VSM::request_capacity(1) == VSM::REQUEST_LIST_MIN);
	// A clipmap level 0 texel is 2^CLIPMAP_TEXEL0_EXPONENT meters.
	CHECK(std::ldexp(1.0f, VSM::CLIPMAP_TEXEL0_EXPONENT) == VSM::CLIPMAP_LEVEL0_EXTENT / float(VSM::VIRTUAL_SIZE));
	const String defines = VSM::get_glsl_defines();
	CHECK(defines.contains("#define VSM_COARSE_FIRST_MIP 5u"));
	CHECK(defines.contains("#define VSM_CLIPMAP_TEXEL0_EXPONENT (-10)"));
	CHECK(defines.contains("#define VSM_MAP_KIND_NONE 4u"));
	CHECK(defines.contains("#define VSM_STATE_SINK 16u"));
	CHECK(defines.contains("#define VSM_STATE_TOTAL_OVERFLOW 32u"));
	CHECK(defines.contains("#define VSM_MAP_STATE_BIAS 4u"));
	CHECK(defines.contains("#define VSM_INDIRECT_ALL 6u"));
	CHECK(defines.contains("#define VSM_STATE_BIAS_FRAME " + itos(VSM::STATE_BIAS_FRAME) + "u"));
	CHECK(defines.contains("#define VSM_OVERFLOW_BIAS_RAISED 65536u"));
	CHECK(VSM::STATE_BIAS_FRAME < VSM::STATE_BUCKETS);
	CHECK(defines.contains("#define VSM_ENTRY_DIRTY_MASK " + itos(VSM::ENTRY_DIRTY_MASK) + "u"));
}

TEST_CASE("[VSM] Map data: clipmap levels differ by exact powers of two and faces cover their lights") {
	const Basis sun = Basis::from_euler(Vector3(Math::deg_to_rad(-50.0f), Math::deg_to_rad(30.0f), 0.0f));
	const VSM::MapData level0 = VSM::map_data_clipmap(sun, 0, Vector2i(-32, -32), 100.0f);
	CHECK(level0.kind == VSM::MAP_KIND_DIRECTIONAL);
	CHECK(level0.texel_scale == std::ldexp(1.0f, VSM::CLIPMAP_TEXEL0_EXPONENT));
	for (uint32_t level = 1; level < VSM::CLIPMAP_MAX_LEVELS; level++) {
		const VSM::MapData data = VSM::map_data_clipmap(sun, level, Vector2i(), 100.0f);
		CHECK(data.texel_scale == std::ldexp(1.0f, VSM::CLIPMAP_TEXEL0_EXPONENT + int32_t(level)));
		for (int row = 0; row < 2; row++) {
			for (int column = 0; column < 3; column++) {
				CHECK(data.rows[row][column] == std::ldexp(level0.rows[row][column], -int32_t(level)));
			}
		}
	}
	// So a level's texel is level 0's divided by 2^level with no rounding, for any point.
	RandomPCG rng(219);
	uint32_t level_texel_mismatches = 0;
	for (uint32_t i = 0; i < 4096; i++) {
		const Vector3 world(rng.randf() * 600.0f - 300.0f, rng.randf() * 60.0f - 5.0f, rng.randf() * 600.0f - 300.0f);
		const Vector2i texel0 = VSM::clipmap_texel0(level0, world);
		const uint32_t level = rng.rand(VSM::CLIPMAP_MAX_LEVELS);
		const VSM::MapData data = VSM::map_data_clipmap(sun, level, Vector2i(), 100.0f);
		level_texel_mismatches += VSM::clipmap_texel0(data, world) != VSM::clipmap_level_texel(texel0, level) ? 1 : 0;
	}
	CHECK(level_texel_mismatches == 0);

	// Omni faces see every direction; area faces every direction in front of the light, none behind.
	const Transform3D light(Basis::from_euler(Vector3(0.3f, -0.7f, 0.2f)), Vector3(3.0f, 2.0f, -1.0f));
	VSM::MapData omni[6];
	VSM::MapData area[5];
	for (uint32_t face = 0; face < 6; face++) {
		omni[face] = VSM::map_data_face(VSM::MAP_KIND_OMNI, face, light, 10.0f, 0.0f);
		CHECK(Math::is_equal_approx(omni[face].texel_scale, 2.0f / float(VSM::VIRTUAL_SIZE)));
	}
	for (uint32_t face = 0; face < 5; face++) {
		area[face] = VSM::map_data_face(VSM::MAP_KIND_AREA, face, light, 10.0f, 0.0f);
	}
	uint32_t omni_missed = 0;
	uint32_t area_missed = 0;
	uint32_t area_behind = 0;
	const Vector3 front = -light.basis.get_column(2).normalized();
	for (uint32_t i = 0; i < 4096; i++) {
		const Vector3 direction = Vector3(rng.randf() * 2.0f - 1.0f, rng.randf() * 2.0f - 1.0f, rng.randf() * 2.0f - 1.0f).normalized();
		const Vector3 point = light.origin + direction * (0.5f + rng.randf() * 9.0f);
		float w = 0.0f;
		uint32_t x = 0;
		uint32_t y = 0;
		bool in_omni = false;
		for (const VSM::MapData &data : omni) {
			in_omni = in_omni || VSM::face_texel(data, point, w, x, y);
		}
		omni_missed += in_omni ? 0 : 1;
		bool in_area = false;
		for (const VSM::MapData &data : area) {
			in_area = in_area || VSM::face_texel(data, point, w, x, y);
		}
		const float facing = direction.dot(front);
		if (facing > 0.01f) {
			area_missed += in_area ? 0 : 1;
		} else if (facing < -0.71f) {
			area_behind += in_area ? 1 : 0; // Only the back face's cone is uncovered.
		}
	}
	CHECK(omni_missed == 0);
	CHECK(area_missed == 0);
	CHECK(area_behind == 0);

	// A spot's map: the axis lands in the map's center, its texel at 1 m is 2 tan(angle) / 4096 m.
	const Transform3D spot(Basis(), Vector3(0.0f, 4.0f, 0.0f));
	const VSM::MapData spot_map = VSM::map_data_face(VSM::MAP_KIND_SPOT, 0, spot, 20.0f, 30.0f);
	float w = 0.0f;
	uint32_t x = 0;
	uint32_t y = 0;
	CHECK(VSM::face_texel(spot_map, Vector3(0.0f, 4.0f, -2.0f), w, x, y));
	CHECK(Math::is_equal_approx(w, 2.0f));
	CHECK(x == VSM::VIRTUAL_SIZE / 2);
	CHECK(y == VSM::VIRTUAL_SIZE / 2);
	CHECK(Math::is_equal_approx(spot_map.texel_scale, float(2.0 * Math::tan(Math::deg_to_rad(30.0))) / float(VSM::VIRTUAL_SIZE)));
	CHECK_FALSE(VSM::face_texel(spot_map, Vector3(0.0f, 4.0f, 2.0f), w, x, y)); // Behind the light.
	CHECK_FALSE(VSM::face_texel(spot_map, Vector3(5.0f, 4.0f, -2.0f), w, x, y)); // Outside the cone's frustum.
}

TEST_CASE("[VSM] The mip a request takes: at most one shadow texel per pixel footprint") {
	// float_exponent() is floor(log2()).
	CHECK(VSM::float_exponent(1.0f) == 0);
	CHECK(VSM::float_exponent(1.99f) == 0);
	CHECK(VSM::float_exponent(2.0f) == 1);
	CHECK(VSM::float_exponent(0.75f) == -1);
	CHECK(VSM::float_exponent(std::ldexp(1.0f, -10)) == -10);
	CHECK(VSM::float_exponent(0.0f) == -127);
	// Faces: the coarsest mip whose texel is not larger than the footprint, clamped to the chain.
	CHECK(VSM::face_mip(1.0f, 0.5f) == 0);
	CHECK(VSM::face_mip(1.0f, 1.0f) == 0);
	CHECK(VSM::face_mip(1.0f, 1.99f) == 0);
	CHECK(VSM::face_mip(1.0f, 2.0f) == 1);
	CHECK(VSM::face_mip(1.0f, 7.9f) == 2);
	CHECK(VSM::face_mip(1.0f, 1.0e6f) == VSM::MIP_COUNT - 1);
	RandomPCG rng(2190);
	uint32_t too_coarse = 0;
	uint32_t too_fine = 0;
	for (uint32_t i = 0; i < 65536; i++) {
		const float texel0 = 1.0e-4f + rng.randf() * 0.01f;
		const float footprint = rng.randf() * 0.5f;
		const uint32_t mip = VSM::face_mip(texel0, footprint);
		const float texel = texel0 * float(1u << mip);
		too_coarse += (mip > 0 && texel > footprint) ? 1 : 0;
		too_fine += (mip + 1 < VSM::MIP_COUNT && texel * 2.0f <= footprint) ? 1 : 0;
	}
	CHECK(too_coarse == 0);
	CHECK(too_fine == 0);
	CHECK(VSM::biased_mip(2, 3) == 5);
	CHECK(VSM::biased_mip(5, 3) == VSM::MIP_COUNT - 1);
	CHECK(VSM::biased_mip(1, -3) == 0);
	// Clipmaps: level + mip is the exponent of the footprint over a level 0 texel.
	CHECK(VSM::clipmap_level_mip(std::ldexp(1.0f, -10), 0) == 0);
	CHECK(VSM::clipmap_level_mip(std::ldexp(1.5f, -10), 0) == 0);
	CHECK(VSM::clipmap_level_mip(std::ldexp(1.0f, -7), 0) == 3);
	CHECK(VSM::clipmap_level_mip(std::ldexp(1.0f, -7), 2) == 5);
	CHECK(VSM::clipmap_level_mip(std::ldexp(1.0f, -12), 0) == -2);
}

TEST_CASE("[VSM] Lights reach points by range, cone and hemisphere") {
	VSM::MarkLight omni;
	omni.kind = VSM::MAP_KIND_OMNI;
	omni.inv_radius = 0.5f;
	CHECK(VSM::light_reaches(omni, Vector3(1.9f, 0.0f, 0.0f)));
	CHECK_FALSE(VSM::light_reaches(omni, Vector3(2.0f, 0.1f, 0.0f)));
	VSM::MarkLight spot = omni;
	spot.kind = VSM::MAP_KIND_SPOT;
	spot.direction = Vector3(0.0f, 0.0f, -1.0f);
	spot.cos_angle = Math::cos(Math::deg_to_rad(30.0f));
	CHECK(VSM::light_reaches(spot, Vector3(0.0f, 0.5f, -1.0f))); // 26.6 degrees off the axis.
	CHECK_FALSE(VSM::light_reaches(spot, Vector3(0.0f, 0.6f, -1.0f))); // 31 degrees.
	CHECK_FALSE(VSM::light_reaches(spot, Vector3(0.0f, 0.0f, 1.0f))); // Behind.
	spot.cos_angle = Math::cos(Math::deg_to_rad(120.0f)); // A cone wider than a hemisphere.
	CHECK(VSM::light_reaches(spot, Vector3(0.0f, 1.0f, 0.3f)));
	CHECK_FALSE(VSM::light_reaches(spot, Vector3(0.0f, 0.1f, 1.0f)));
	VSM::MarkLight area = omni;
	area.kind = VSM::MAP_KIND_AREA;
	area.direction = Vector3(0.0f, -1.0f, 0.0f);
	CHECK(VSM::light_reaches(area, Vector3(1.0f, -0.1f, 0.0f)));
	CHECK_FALSE(VSM::light_reaches(area, Vector3(1.0f, 0.1f, 0.0f)));
}

TEST_CASE("[VSM] Directional and local requests: levels, mips, overflow bias and coarse pages") {
	// One directional light (levels 0 to 6 from map 0), one spot (map 7) and an omni light (8 to 13).
	const uint32_t table_maps = 16;
	LocalVector<VSM::MapData> maps;
	maps.resize(table_maps);
	LocalVector<uint32_t> bias;
	bias.resize(table_maps);
	memset(bias.ptr(), 0, table_maps * sizeof(uint32_t));
	const Basis sun = Basis::from_euler(Vector3(Math::deg_to_rad(-90.0f), 0.0f, 0.0f)); // Straight down.
	const Vector3 camera(0.3f, 2.0f, 0.7f);
	const Vector2 center(sun.get_column(0).normalized().dot(camera), sun.get_column(1).normalized().dot(camera));
	for (uint32_t level = 0; level < 7; level++) {
		maps[level] = VSM::map_data_clipmap(sun, level, VSM::clipmap_level_origin(center, level), 100.0f);
	}
	const uint32_t sun_flags = VSM::light_flags(VSM::MAP_KIND_DIRECTIONAL, 7);
	LocalVector<uint64_t> requests;

	// A point under the camera with a footprint of exactly one level 0 texel: level 0, mip 0.
	const Vector3 near_point(0.3f, 0.0f, 0.7f);
	VSM::mark_directional_point(maps.ptr(), table_maps, bias.ptr(), 0, sun_flags, near_point, std::ldexp(1.0f, -10), 0, false, requests);
	REQUIRE(requests.size() == 1);
	CHECK(requests[0] < uint64_t(VSM::ENTRIES_PER_MAP));
	CHECK(requests[0] < VSM::mip_entry_offset(1));
	// Footprint 8 level 0 texels: still level 0, mip 3. With overflow bias 2 on level 0: mip 5.
	requests.clear();
	VSM::mark_directional_point(maps.ptr(), table_maps, bias.ptr(), 0, sun_flags, near_point, std::ldexp(1.0f, -7), 0, false, requests);
	REQUIRE(requests.size() == 1);
	CHECK(requests[0] >= VSM::mip_entry_offset(3));
	CHECK(requests[0] < VSM::mip_entry_offset(4));
	bias[0] = 2;
	requests.clear();
	VSM::mark_directional_point(maps.ptr(), table_maps, bias.ptr(), 0, sun_flags, near_point, std::ldexp(1.0f, -7), 0, false, requests);
	REQUIRE(requests.size() == 1);
	CHECK(requests[0] >= VSM::mip_entry_offset(5));
	CHECK(requests[0] < VSM::mip_entry_offset(6));
	bias[0] = 0;
	// 30 m away: outside levels 0 to 3 (half extents 2 to 16 m), inside level 4 (32 m).
	requests.clear();
	VSM::mark_directional_point(maps.ptr(), table_maps, bias.ptr(), 0, sun_flags, Vector3(30.3f, 0.0f, 0.7f), std::ldexp(1.0f, -10), 0, false, requests);
	REQUIRE(requests.size() == 1);
	CHECK(requests[0] / VSM::ENTRIES_PER_MAP == 4);
	// Past the last level: no request.
	requests.clear();
	VSM::mark_directional_point(maps.ptr(), table_maps, bias.ptr(), 0, sun_flags, Vector3(500.0f, 0.0f, 0.7f), std::ldexp(1.0f, -10), 0, false, requests);
	CHECK(requests.is_empty());
	// Coarse: each coarse mip in the finest level whose window holds the point at that mip. Here the
	// point is in level 0's mip 5 window; mip 6's single page covers an aligned half of each level that
	// misses this point in every level (origins of -28, -30, -31 and -32 pages).
	requests.clear();
	VSM::mark_directional_point(maps.ptr(), table_maps, bias.ptr(), 0, sun_flags, near_point, 1.0f, 0, true, requests);
	REQUIRE(requests.size() == 1);
	CHECK((requests[0] & VSM::REQUEST_COARSE) != 0);
	CHECK(uint32_t(requests[0]) / VSM::ENTRIES_PER_MAP == 0);
	CHECK(uint32_t(requests[0]) % VSM::ENTRIES_PER_MAP >= VSM::mip_entry_offset(VSM::COARSE_FIRST_MIP));
	CHECK(uint32_t(requests[0]) % VSM::ENTRIES_PER_MAP < VSM::mip_entry_offset(VSM::COARSE_FIRST_MIP + 1));
	// The same rule, brute force over the levels, for points around the camera.
	RandomPCG rng(21900);
	uint32_t coarse_mismatches = 0;
	for (uint32_t i = 0; i < 1024; i++) {
		const Vector3 point(camera.x + rng.randf() * 200.0f - 100.0f, 0.0f, camera.z + rng.randf() * 200.0f - 100.0f);
		requests.clear();
		VSM::mark_directional_point(maps.ptr(), table_maps, bias.ptr(), 0, sun_flags, point, 1.0f, 0, true, requests);
		const Vector2i texel0 = VSM::clipmap_texel0(maps[0], point);
		uint32_t expected = 0;
		for (uint32_t mip = VSM::COARSE_FIRST_MIP; mip < VSM::MIP_COUNT; mip++) {
			for (uint32_t level = 0; level < 7; level++) {
				const Vector2i level_texel = VSM::clipmap_level_texel(texel0, level);
				if (VSM::clipmap_in_window(Vector2i(maps[level].origin[0], maps[level].origin[1]), level_texel, mip)) {
					const Vector2i page = VSM::clipmap_page(level_texel, mip);
					const uint64_t key = uint64_t(VSM::entry_index(level, mip, uint32_t(page.x), uint32_t(page.y))) | VSM::REQUEST_COARSE;
					coarse_mismatches += requests.find(key) < 0 ? 1 : 0;
					expected++;
					break;
				}
			}
		}
		coarse_mismatches += requests.size() != expected ? 1 : 0;
	}
	CHECK(coarse_mismatches == 0);

	// The spot looks down from 4 m: the point under it, 4 m away, footprint 1 mm: mip 0, as its texel
	// there is 4 * 2 tan(30) / 4096 = 1.13 mm > 1 mm; footprint 3 mm: mip 1.
	const Transform3D spot_transform(Basis::from_euler(Vector3(Math::deg_to_rad(-90.0f), 0.0f, 0.0f)), Vector3(0.0f, 4.0f, 0.0f));
	maps[7] = VSM::map_data_face(VSM::MAP_KIND_SPOT, 0, spot_transform, 20.0f, 30.0f);
	VSM::MarkLight spot;
	spot.kind = VSM::MAP_KIND_SPOT;
	spot.inv_radius = 1.0f / 20.0f;
	spot.map_base = 7;
	spot.flags = VSM::light_flags(VSM::MAP_KIND_SPOT, 1);
	requests.clear();
	VSM::mark_local_point(maps.ptr(), table_maps, bias.ptr(), spot, Vector3(0.0f, 0.0f, 0.0f), 0.001f, 0, false, false, requests);
	REQUIRE(requests.size() == 1);
	CHECK(requests[0] >= VSM::ENTRIES_PER_MAP * 7);
	CHECK(requests[0] < VSM::ENTRIES_PER_MAP * 7 + VSM::mip_entry_offset(1));
	requests.clear();
	VSM::mark_local_point(maps.ptr(), table_maps, bias.ptr(), spot, Vector3(0.0f, 0.0f, 0.0f), 0.003f, 0, false, false, requests);
	REQUIRE(requests.size() == 1);
	CHECK(requests[0] - VSM::ENTRIES_PER_MAP * 7 >= VSM::mip_entry_offset(1));
	CHECK(requests[0] - VSM::ENTRIES_PER_MAP * 7 < VSM::mip_entry_offset(2));
	// The configured bias and the map's overflow bias add up, clamped to the last mip.
	bias[7] = 3;
	requests.clear();
	VSM::mark_local_point(maps.ptr(), table_maps, bias.ptr(), spot, Vector3(0.0f, 0.0f, 0.0f), 0.003f, 4, false, false, requests);
	REQUIRE(requests.size() == 1);
	CHECK(requests[0] - VSM::ENTRIES_PER_MAP * 7 == VSM::mip_entry_offset(VSM::MIP_COUNT - 1));
	bias[7] = 0;

	// An omni light requests one face for a point inside a face, two on an edge between faces.
	const Transform3D omni_transform(Basis(), Vector3(10.0f, 2.0f, 0.0f));
	for (uint32_t face = 0; face < 6; face++) {
		maps[8 + face] = VSM::map_data_face(VSM::MAP_KIND_OMNI, face, omni_transform, 8.0f, 0.0f);
	}
	VSM::MarkLight omni;
	omni.kind = VSM::MAP_KIND_OMNI;
	omni.map_base = 8;
	omni.flags = VSM::light_flags(VSM::MAP_KIND_OMNI, 6);
	requests.clear();
	VSM::mark_local_point(maps.ptr(), table_maps, bias.ptr(), omni, Vector3(12.0f, 2.3f, 0.1f), 0.01f, 0, false, false, requests);
	CHECK(requests.size() == 1);
	CHECK(requests[0] / VSM::ENTRIES_PER_MAP == 8); // The +X face.
	requests.clear();
	VSM::mark_local_point(maps.ptr(), table_maps, bias.ptr(), omni, Vector3(12.0f, 4.0f, 0.0f), 0.01f, 0, false, false, requests);
	CHECK(requests.size() >= 1); // On the edge of +X and +Y: one face or both, as rounding goes.
	CHECK(requests.size() <= 2);
	// A fog point at the light (inside no face): with p_all_faces, every face's coarse pages (5 each).
	requests.clear();
	VSM::mark_local_point(maps.ptr(), table_maps, bias.ptr(), omni, Vector3(10.0f, 2.0f, 0.0f), 0.0f, 0, true, true, requests);
	CHECK(requests.size() == 6 * 5);
	requests.clear();
	VSM::mark_local_point(maps.ptr(), table_maps, bias.ptr(), omni, Vector3(10.0f, 2.0f, 0.0f), 0.0f, 0, true, false, requests);
	CHECK(requests.is_empty());
	// Boxes: in front of the spot, its coarse pages; a box the near plane cuts, all of them; a box
	// outside its frustum, none.
	// A small box in one quadrant of the spot's map: one page of mip 5, the one page of mip 6.
	requests.clear();
	VSM::mark_local_box(maps.ptr(), table_maps, spot, spot_transform.origin, AABB(Vector3(0.5f, 0.0f, -0.7f), Vector3(0.2f, 0.2f, 0.2f)), requests);
	CHECK(requests.size() == 2);
	// A box under the light that spans the map's center: all four mip 5 pages and mip 6.
	requests.clear();
	VSM::mark_local_box(maps.ptr(), table_maps, spot, spot_transform.origin, AABB(Vector3(-0.5f, 0.0f, -0.5f), Vector3(1.0f, 1.0f, 1.0f)), requests);
	CHECK(requests.size() == 5);
	requests.clear();
	VSM::mark_local_box(maps.ptr(), table_maps, spot, spot_transform.origin, AABB(Vector3(-0.5f, 3.5f, -0.5f), Vector3(1.0f, 1.0f, 1.0f)), requests);
	CHECK(requests.size() == 5);
	requests.clear();
	VSM::mark_local_box(maps.ptr(), table_maps, spot, spot_transform.origin, AABB(Vector3(8.0f, 0.0f, -0.5f), Vector3(1.0f, 1.0f, 1.0f)), requests);
	CHECK(requests.is_empty());
	requests.clear();
	VSM::mark_directional_box(maps.ptr(), table_maps, 0, sun_flags, AABB(Vector3(-0.2f, 0.0f, 0.2f), Vector3(1.0f, 1.0f, 1.0f)), requests);
	CHECK(requests.size() >= 7); // Every level's mip 5 window holds the box, which is next to the camera.
	for (uint64_t request : requests) {
		CHECK((request & VSM::REQUEST_COARSE) != 0);
	}
}

namespace {

// A page table of one map and a pool of p_pages pages in a grid of p_width columns.
struct AllocationFixture {
	LocalVector<uint32_t> table;
	LocalVector<VSM::PageRecord> pages;
	uint32_t width = 4;

	AllocationFixture(uint32_t p_pages, uint32_t p_width) {
		table.resize(VSM::ENTRIES_PER_MAP);
		memset(table.ptr(), 0, table.size() * sizeof(uint32_t));
		pages.resize(p_pages);
		width = p_width;
	}
	VSM::AllocationCounts run(const LocalVector<uint32_t> &p_requests, uint32_t p_frame, uint32_t p_cache_frames) {
		for (uint32_t index : p_requests) {
			table[index] |= VSM::ENTRY_REQUESTED;
		}
		const VSM::AllocationCounts counts = VSM::allocate_model(table.ptr(), table.size(), pages.ptr(), pages.size(), width, p_requests.ptr(), p_requests.size(), p_frame, p_cache_frames);
		for (uint32_t index : p_requests) {
			table[index] = VSM::entry_after_emit(table[index]);
		}
		return counts;
	}
	uint32_t page_of(uint32_t p_index) const {
		const uint32_t entry = table[p_index];
		return (entry & VSM::ENTRY_ALLOCATED) ? VSM::entry_page_y(entry) * width + VSM::entry_page_x(entry) : UINT32_MAX;
	}
	bool consistent() const {
		for (uint32_t p = 0; p < pages.size(); p++) {
			if (pages[p].owner != 0 && page_of(pages[p].owner - 1) != p) {
				return false;
			}
		}
		for (uint32_t i = 0; i < table.size(); i++) {
			const uint32_t p = page_of(i);
			if (p != UINT32_MAX && (p >= pages.size() || pages[p].owner != i + 1)) {
				return false;
			}
		}
		return true;
	}
};

} // namespace

TEST_CASE("[VSM] Allocation keeps cached pages, reuses the least recently used and drops stale pages") {
	AllocationFixture fixture(8, 4);
	const uint32_t cache = 10;
	// Frame 1: four requests get four free pages, all to be rendered.
	LocalVector<uint32_t> first;
	for (uint32_t i = 0; i < 4; i++) {
		first.push_back(i);
	}
	VSM::AllocationCounts counts = fixture.run(first, 1, cache);
	CHECK(counts.new_pages == 4);
	CHECK(counts.dirty == 4);
	CHECK(counts.available == 8);
	CHECK(counts.overflow == 0);
	CHECK(fixture.consistent());
	uint32_t pages_before[4];
	for (uint32_t i = 0; i < 4; i++) {
		pages_before[i] = fixture.page_of(i);
		CHECK(pages_before[i] != UINT32_MAX);
		CHECK((fixture.table[i] & (VSM::ENTRY_REQUESTED | VSM::ENTRY_DIRTY_MASK)) == 0); // Handed to the dirty list.
	}
	// Frame 2: the same requests keep their pages; nothing is re-rendered.
	counts = fixture.run(first, 2, cache);
	CHECK(counts.kept == 4);
	CHECK(counts.new_pages == 0);
	CHECK(counts.dirty == 0);
	for (uint32_t i = 0; i < 4; i++) {
		CHECK(fixture.page_of(i) == pages_before[i]);
	}
	// Frame 3: entries 0 and 1 only. Entries 2 and 3 stay cached.
	LocalVector<uint32_t> two;
	two.push_back(0);
	two.push_back(1);
	counts = fixture.run(two, 3, cache);
	CHECK(counts.kept == 2);
	// Frame 5: six new requests: four free pages, then the two cached pages, both 2 frames old.
	LocalVector<uint32_t> six;
	for (uint32_t i = 10; i < 16; i++) {
		six.push_back(i);
	}
	six.push_back(0);
	six.push_back(1);
	counts = fixture.run(six, 5, cache);
	CHECK(counts.kept == 2);
	CHECK(counts.new_pages == 6);
	CHECK(counts.stolen == 2);
	CHECK(counts.cached == 2);
	CHECK(fixture.page_of(2) == UINT32_MAX);
	CHECK(fixture.page_of(3) == UINT32_MAX);
	CHECK(fixture.consistent());
	// Frame 6: two more requests; the pool is full and every page is in use or 1 frame old: the
	// oldest cached pages go first. Entries 10 to 15 were requested at frame 5, 0 and 1 too.
	LocalVector<uint32_t> late;
	late.push_back(20);
	late.push_back(21);
	late.push_back(0);
	late.push_back(1);
	counts = fixture.run(late, 6, cache);
	CHECK(counts.new_pages == 2);
	CHECK(counts.stolen == 2);
	CHECK(counts.overflow == 0);
	CHECK(fixture.consistent());
	// Frame 30: nothing for 24 frames: every page is past the 10 cached frames and is dropped.
	LocalVector<uint32_t> none;
	counts = fixture.run(none, 30, cache);
	CHECK(counts.evicted == 8);
	CHECK(counts.available == 8);
	for (uint32_t p = 0; p < 8; p++) {
		CHECK(fixture.pages[p].owner == 0);
	}
	CHECK(fixture.consistent());
}

TEST_CASE("[VSM] Allocation: overflow, coarse requests first, scrolled and orphaned pages") {
	AllocationFixture fixture(4, 2);
	const uint32_t cache = 10;
	// Six requests for four pages; two coarse ones among them get pages first.
	LocalVector<uint32_t> requests;
	for (uint32_t i = 0; i < 6; i++) {
		requests.push_back(i);
	}
	fixture.table[4] |= VSM::ENTRY_COARSE;
	fixture.table[5] |= VSM::ENTRY_COARSE;
	VSM::AllocationCounts counts = fixture.run(requests, 1, cache);
	CHECK(counts.new_pages == 4);
	CHECK(counts.overflow == 2);
	CHECK(counts.coarse == 2);
	CHECK(fixture.page_of(4) != UINT32_MAX);
	CHECK(fixture.page_of(5) != UINT32_MAX);
	uint32_t flagged = 0;
	for (uint32_t i = 0; i < 6; i++) {
		if (fixture.page_of(i) == UINT32_MAX) {
			flagged += (fixture.table[i] & VSM::ENTRY_OVERFLOW) ? 1 : 0;
		} else {
			CHECK((fixture.table[i] & VSM::ENTRY_OVERFLOW) == 0);
		}
	}
	CHECK(flagged == 2);
	CHECK(fixture.consistent());

	// A scrolled entry that is requested keeps its page and is re-rendered there.
	const uint32_t scrolled = 4;
	const uint32_t scrolled_page = fixture.page_of(scrolled);
	fixture.table[scrolled] |= VSM::ENTRY_SCROLLED;
	LocalVector<uint32_t> again;
	again.push_back(scrolled);
	counts = fixture.run(again, 2, cache);
	CHECK(counts.kept == 1);
	CHECK(counts.dirty == 1);
	CHECK(fixture.page_of(scrolled) == scrolled_page);
	CHECK((fixture.table[scrolled] & VSM::ENTRY_SCROLLED) == 0);
	// A scrolled entry nobody requests is dropped: its page holds the wrong ground.
	const uint32_t other = 5;
	fixture.table[other] |= VSM::ENTRY_SCROLLED;
	LocalVector<uint32_t> none;
	counts = fixture.run(none, 3, cache);
	CHECK(counts.evicted == 1);
	CHECK(fixture.page_of(other) == UINT32_MAX);
	// A cleared entry (a freed light) leaves its page orphaned: free again.
	uint32_t cleared = 0;
	for (uint32_t i = 0; i < 6; i++) {
		if (fixture.page_of(i) != UINT32_MAX && i != scrolled) {
			cleared = i;
			break;
		}
	}
	fixture.table[cleared] = 0;
	counts = fixture.run(none, 4, cache);
	CHECK(counts.orphaned == 1);
	CHECK(fixture.consistent());
}

TEST_CASE("[VSM] Allocation reuses free pages first, then cached pages oldest first") {
	AllocationFixture fixture(6, 3);
	const uint32_t cache = 20;
	// Frames 1 to 5: entry i is last requested at frame i + 1 (entries 4 and 5 at frame 5).
	for (uint32_t frame = 1; frame <= 5; frame++) {
		LocalVector<uint32_t> requests;
		for (uint32_t i = frame - 1; i < 6; i++) {
			requests.push_back(i);
		}
		fixture.run(requests, frame, cache);
	}
	CHECK(fixture.consistent());
	uint32_t pages[6];
	for (uint32_t i = 0; i < 6; i++) {
		pages[i] = fixture.page_of(i);
		REQUIRE(pages[i] != UINT32_MAX);
	}
	// The update step at frame 6: every page is cached, in bucket cache_frames - age.
	for (uint32_t i = 0; i < 6; i++) {
		uint32_t bucket = 0;
		const VSM::PageUpdate update = VSM::page_update(fixture.pages[pages[i]], pages[i], fixture.width, fixture.table.ptr(), fixture.table.size(), 6, cache, bucket);
		CHECK(update == VSM::PAGE_UPDATE_CACHED);
		CHECK(bucket == cache - (6 - MIN(i + 1, 5u)));
	}
	// Frame 6: two new requests and no free page: the two least recently used entries (ages 5 and 4)
	// lose their pages; the more recent ones keep theirs.
	LocalVector<uint32_t> two_new;
	two_new.push_back(10);
	two_new.push_back(11);
	VSM::AllocationCounts counts = fixture.run(two_new, 6, cache);
	CHECK(counts.available == 6);
	CHECK(counts.new_pages == 2);
	CHECK(counts.stolen == 2);
	CHECK(fixture.page_of(0) == UINT32_MAX);
	CHECK(fixture.page_of(1) == UINT32_MAX);
	for (uint32_t i = 2; i < 6; i++) {
		CHECK(fixture.page_of(i) == pages[i]);
	}
	CHECK(((fixture.page_of(10) == pages[0] && fixture.page_of(11) == pages[1]) || (fixture.page_of(10) == pages[1] && fixture.page_of(11) == pages[0])));
	CHECK(fixture.consistent());
	// Frame 7: entry 3's light is gone (its entry cleared): its page is free and goes before the
	// oldest cached page (entry 2's, 4 frames old).
	fixture.table[3] = 0;
	LocalVector<uint32_t> one_new;
	one_new.push_back(12);
	counts = fixture.run(one_new, 7, cache);
	CHECK(counts.orphaned == 1);
	CHECK(counts.stolen == 0);
	CHECK(fixture.page_of(12) == pages[3]);
	CHECK(fixture.page_of(2) == pages[2]);
	CHECK(fixture.consistent());
	// A page requested in this frame is never reused, and a free page is in bucket 0.
	uint32_t bucket = 0;
	CHECK(VSM::page_update(fixture.pages[pages[3]], pages[3], fixture.width, fixture.table.ptr(), fixture.table.size(), 7, cache, bucket) == VSM::PAGE_UPDATE_CACHED);
	CHECK(bucket == VSM::BUCKET_NONE);
	VSM::PageRecord free_page;
	CHECK(VSM::page_update(free_page, 0, fixture.width, fixture.table.ptr(), fixture.table.size(), 7, cache, bucket) == VSM::PAGE_UPDATE_FREE);
	CHECK(bucket == 0);
}

TEST_CASE("[VSM] Fog cells: a light reaches the cells around a vertex within its range plus one cell") {
	VSM::MarkLight omni;
	omni.kind = VSM::MAP_KIND_OMNI;
	omni.inv_radius = 1.0f / 1.5f;
	// A light of 1.5 m range at the center of a cell whose diagonal is 3.2 m: every vertex of the cell
	// is 1.6 m away, out of its range, but the fog shades the cell's center.
	const Vector3 rel(0.92376f, 0.92376f, 0.92376f);
	CHECK_FALSE(VSM::light_reaches(omni, rel));
	bool all_faces = false;
	CHECK(VSM::light_reaches_cell(omni, rel, 3.2f, all_faces));
	CHECK(all_faces);
	// 2 m away with 1 m cells: within range plus one cell, not within one cell.
	CHECK(VSM::light_reaches_cell(omni, Vector3(2.0f, 0.0f, 0.0f), 1.0f, all_faces));
	CHECK_FALSE(all_faces);
	CHECK_FALSE(VSM::light_reaches_cell(omni, Vector3(2.6f, 0.0f, 0.0f), 1.0f, all_faces));
	// A spot: a vertex just outside the cone still reaches the cells around it, which the cone may cross.
	VSM::MarkLight spot;
	spot.kind = VSM::MAP_KIND_SPOT;
	spot.inv_radius = 1.0f / 20.0f;
	spot.direction = Vector3(0.0f, 0.0f, -1.0f);
	spot.cos_angle = Math::cos(Math::deg_to_rad(30.0f));
	CHECK_FALSE(VSM::light_reaches(spot, Vector3(0.0f, 0.6f, -1.0f)));
	CHECK(VSM::light_reaches_cell(spot, Vector3(0.0f, 0.6f, -1.0f), 0.5f, all_faces));
	CHECK_FALSE(all_faces);
}

TEST_CASE("[VSM] Overflow bias: the largest requesters take up to three mips and give one back per 30 frames") {
	uint32_t state = 0;
	// Overflow with this map among the largest requesters (at least half the largest count), one view
	// per frame.
	for (uint32_t frame = 0; frame < 5; frame++) {
		state = VSM::overflow_bias_update(state, 50, 100, 7, true);
	}
	CHECK(VSM::overflow_bias(state) == VSM::OVERFLOW_MAX_BIAS);
	// A map below half the largest count keeps its bias.
	CHECK(VSM::overflow_bias(VSM::overflow_bias_update(0, 49, 100, 7, true)) == 0);
	CHECK(VSM::overflow_bias(VSM::overflow_bias_update(0, 0, 0, 7, true)) == 0);
	// Without overflow: one mip back after 30 frames, then 30 more for the next.
	for (uint32_t frame = 0; frame < 29; frame++) {
		state = VSM::overflow_bias_update(state, 50, 100, 0, true);
	}
	CHECK(VSM::overflow_bias(state) == 3);
	state = VSM::overflow_bias_update(state, 50, 100, 0, true);
	CHECK(VSM::overflow_bias(state) == 2);
	// Pressure again restarts the wait.
	for (uint32_t frame = 0; frame < 20; frame++) {
		state = VSM::overflow_bias_update(state, 50, 100, 0, true);
	}
	state = VSM::overflow_bias_update(state, 10, 100, 3, true);
	CHECK(VSM::overflow_bias(state) == 2);
	for (uint32_t frame = 0; frame < 29; frame++) {
		state = VSM::overflow_bias_update(state, 50, 100, 0, true);
	}
	CHECK(VSM::overflow_bias(state) == 2);
	for (uint32_t frame = 0; frame < 61; frame++) {
		state = VSM::overflow_bias_update(state, 50, 100, 0, true);
	}
	CHECK(VSM::overflow_bias(state) == 0);
	CHECK(state == 0);
}

TEST_CASE("[VSM] Overflow bias counts frames, not views") {
	// Two views per frame (two viewports), both overflowing: one mip per frame, not two.
	uint32_t state = 0;
	for (uint32_t frame = 0; frame < 2; frame++) {
		state = VSM::overflow_bias_update(state, 50, 100, 7, true);
		state = VSM::overflow_bias_update(state, 50, 100, 7, false);
	}
	CHECK(VSM::overflow_bias(state) == 2);
	// A second view that overflows after a first view that did not: the frame still raises once.
	state = VSM::overflow_bias_update(state, 50, 100, 0, true);
	state = VSM::overflow_bias_update(state, 50, 100, 7, false);
	CHECK(VSM::overflow_bias(state) == 3);
	// The bias goes back after 30 frames without overflow, whatever the number of views in each.
	uint32_t frames = 0;
	while (VSM::overflow_bias(state) == 3 && frames < 100) {
		for (uint32_t view = 0; view < 3; view++) {
			state = VSM::overflow_bias_update(state, 50, 100, 0, view == 0);
		}
		frames++;
	}
	CHECK(frames == VSM::OVERFLOW_DECAY_FRAMES);
	// An overflow in a later view of a frame restarts the wait that the frame's first view counted:
	// 28 frames, then a frame whose first view counts the 29th and whose second view overflows.
	for (uint32_t frame = 0; frame < 28; frame++) {
		state = VSM::overflow_bias_update(state, 50, 100, 0, true);
	}
	state = VSM::overflow_bias_update(state, 10, 100, 0, true);
	CHECK(((state >> 8) & 0xFFu) == 29);
	state = VSM::overflow_bias_update(state, 10, 100, 5, false); // Not a top requester: no rise.
	CHECK(VSM::overflow_bias(state) == 2);
	CHECK(((state >> 8) & 0xFFu) == 0);
	state = VSM::overflow_bias_update(state, 50, 100, 0, true);
	CHECK(VSM::overflow_bias(state) == 2); // One frame into a new wait of 30.
	CHECK(((state >> 8) & 0xFFu) == 1);
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
