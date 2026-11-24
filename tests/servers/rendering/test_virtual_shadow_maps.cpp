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

#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "core/math/random_pcg.h"
#include "core/object/class_db.h"
#include "main/performance.h"
#include "scene/3d/light_3d.h"
#include "scene/main/viewport.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_route.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_vsm.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_forward_clustered.h"
#include "servers/rendering/renderer_rd/storage_rd/virtual_shadow_map_storage.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"
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
	LocalVector<uint32_t> invalidated_maps; // GodotNext: vsm_caching (#212).
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
	// GodotNext: vsm_caching (#212). As the GPU's invalidation pass would, on the CPU table.
	void _table_invalidate_map(uint32_t p_map, uint32_t p_dirty_flags, bool p_dynamic_if_used) override {
		invalidate_map_model(table.ptr(), table.size() / ENTRIES_PER_MAP, p_map, p_dirty_flags, nullptr, 0, 0, p_dynamic_if_used);
		invalidated_maps.push_back(p_map);
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
	CHECK(defines.contains("#define VSM_CLIPMAP_RESIDENT_MIP 4u"));
	CHECK(defines.contains("#define VSM_CLIPMAP_TEXEL0_EXPONENT (-10)"));
	CHECK(defines.contains("#define VSM_MAP_KIND_NONE 4u"));
	CHECK(defines.contains("#define VSM_STATE_RESIDENT 16u"));
	CHECK(defines.contains("#define VSM_STATE_RESIDENT_OVERFLOW 17u"));
	CHECK(defines.contains("#define VSM_STATE_SINK 18u"));
	CHECK(defines.contains("#define VSM_STATE_TOTAL_OVERFLOW 32u"));
	CHECK(defines.contains("#define VSM_MAP_STATE_BIAS 4u"));
	CHECK(defines.contains("#define VSM_INDIRECT_ALL 6u"));
	CHECK(defines.contains("#define VSM_STATE_BIAS_FRAME " + itos(VSM::STATE_BIAS_FRAME) + "u"));
	CHECK(defines.contains("#define VSM_STATE_TOTAL_RESIDENT_OVERFLOW " + itos(VSM::STATE_TOTAL_RESIDENT_OVERFLOW) + "u"));
	CHECK(defines.contains("#define VSM_OVERFLOW_BIAS_RAISED 65536u"));
	CHECK(VSM::STATE_TOTAL_RESIDENT_OVERFLOW < VSM::STATE_BUCKETS);
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
	// Coarse: each coarse mip from CLIPMAP_RESIDENT_MIP in the finest level whose window holds the
	// point at that mip (plus the coarsest in-window mip < CLIPMAP_RESIDENT_MIP in the last level's
	// far strip). Here the point is in level 0's mip 4 and mip 5 windows; mip 6's single page covers
	// an aligned half of each level that misses this point in every level.
	requests.clear();
	VSM::mark_directional_point(maps.ptr(), table_maps, bias.ptr(), 0, sun_flags, near_point, 1.0f, 0, true, requests);
	REQUIRE(requests.size() == 2);
	for (uint32_t m = 0; m < 2; m++) {
		const uint32_t mip = VSM::CLIPMAP_RESIDENT_MIP + m;
		CHECK((requests[m] & VSM::REQUEST_COARSE) != 0);
		CHECK(uint32_t(requests[m]) / VSM::ENTRIES_PER_MAP == 0);
		CHECK(uint32_t(requests[m]) % VSM::ENTRIES_PER_MAP >= VSM::mip_entry_offset(mip));
		CHECK(uint32_t(requests[m]) % VSM::ENTRIES_PER_MAP < VSM::mip_entry_offset(mip + 1));
	}
	// The same rule, brute force over the levels, for points around the camera.
	RandomPCG rng(21900);
	uint32_t coarse_mismatches = 0;
	for (uint32_t i = 0; i < 1024; i++) {
		const Vector3 point(camera.x + rng.randf() * 260.0f - 130.0f, 0.0f, camera.z + rng.randf() * 260.0f - 130.0f);
		requests.clear();
		VSM::mark_directional_point(maps.ptr(), table_maps, bias.ptr(), 0, sun_flags, point, 1.0f, 0, true, requests);
		const Vector2i texel0 = VSM::clipmap_texel0(maps[0], point);
		uint32_t expected = 0;
		for (uint32_t mip = VSM::CLIPMAP_RESIDENT_MIP; mip < VSM::MIP_COUNT; mip++) {
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
		const Vector2i last_origin(maps[6].origin[0], maps[6].origin[1]);
		const Vector2i last_texel = VSM::clipmap_level_texel(texel0, 6);
		if (VSM::clipmap_in_window(last_origin, last_texel, 0) && !VSM::clipmap_in_window(last_origin, last_texel, VSM::CLIPMAP_RESIDENT_MIP)) {
			uint32_t resident = 0;
			while (resident + 1 < VSM::CLIPMAP_RESIDENT_MIP && VSM::clipmap_in_window(last_origin, last_texel, resident + 1)) {
				resident++;
			}
			const Vector2i page = VSM::clipmap_page(last_texel, resident);
			const uint64_t key = uint64_t(VSM::entry_index(6, resident, uint32_t(page.x), uint32_t(page.y))) | VSM::REQUEST_COARSE;
			coarse_mismatches += requests.find(key) < 0 ? 1 : 0;
			expected++;
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

TEST_CASE("[VSM] Always-resident coarse pages: root page per face and mips 4 to 6 per clipmap level") {
	const uint32_t table_maps = 20;
	LocalVector<VSM::MapData> maps;
	maps.resize(table_maps);
	const Basis sun;
	for (uint32_t level = 0; level < 7; level++) {
		maps[level] = VSM::map_data_clipmap(sun, level, Vector2i(-32, -32), 100.0f);
	}
	const Transform3D xform(Basis(), Vector3(1.0f, 2.0f, 3.0f));
	maps[7] = VSM::map_data_face(VSM::MAP_KIND_SPOT, 0, xform, 10.0f, 30.0f);
	for (uint32_t face = 0; face < 6; face++) {
		maps[8 + face] = VSM::map_data_face(VSM::MAP_KIND_OMNI, face, xform, 10.0f, 0.0f);
	}
	for (uint32_t face = 0; face < 5; face++) {
		maps[14 + face] = VSM::map_data_face(VSM::MAP_KIND_AREA, face, xform, 10.0f, 0.0f);
	}
	// maps[19] stays MAP_KIND_MAX (unowned).

	LocalVector<uint64_t> requests;
	VSM::mark_resident(maps.ptr(), table_maps, 0, VSM::light_flags(VSM::MAP_KIND_DIRECTIONAL, 7), requests);
	// 16 + 4 + 1 = 21 coarse entries per clipmap level.
	CHECK(requests.size() == 7 * 21);
	for (uint64_t r : requests) {
		CHECK((r & VSM::REQUEST_COARSE) != 0);
		const uint32_t local = uint32_t(r) % VSM::ENTRIES_PER_MAP;
		CHECK(local >= VSM::mip_entry_offset(VSM::CLIPMAP_RESIDENT_MIP));
	}

	requests.clear();
	VSM::mark_resident(maps.ptr(), table_maps, 7, VSM::light_flags(VSM::MAP_KIND_SPOT, 1), requests);
	REQUIRE(requests.size() == 1);
	CHECK(requests[0] == (uint64_t(VSM::entry_index(7, VSM::MIP_COUNT - 1, 0, 0)) | VSM::REQUEST_COARSE));

	requests.clear();
	VSM::mark_resident(maps.ptr(), table_maps, 8, VSM::light_flags(VSM::MAP_KIND_OMNI, 6), requests);
	REQUIRE(requests.size() == 6);
	for (uint32_t face = 0; face < 6; face++) {
		CHECK(requests[face] == (uint64_t(VSM::entry_index(8 + face, VSM::MIP_COUNT - 1, 0, 0)) | VSM::REQUEST_COARSE));
	}

	requests.clear();
	VSM::mark_resident(maps.ptr(), table_maps, 14, VSM::light_flags(VSM::MAP_KIND_AREA, 5), requests);
	REQUIRE(requests.size() == 5);

	// MAP_NONE, past table_maps, or an unowned map marks nothing.
	requests.clear();
	VSM::mark_resident(maps.ptr(), table_maps, VSM::MAP_NONE, VSM::light_flags(VSM::MAP_KIND_OMNI, 6), requests);
	VSM::mark_resident(maps.ptr(), table_maps, 19, VSM::light_flags(VSM::MAP_KIND_SPOT, 1), requests);
	VSM::mark_resident(maps.ptr(), table_maps, 20, VSM::light_flags(VSM::MAP_KIND_DIRECTIONAL, 4), requests);
	CHECK(requests.is_empty());
}

TEST_CASE("[VSM] Allocation serves resident pages first, then other coarse requests, and never evicts active resident pages") {
	AllocationFixture fixture(4, 2);
	const uint32_t cache = 8;
	// List: 2 resident requests [0..2), 3 other coarse requests (indices 10, 11, 12), 3 fine requests (20, 21, 22).
	// Even if fine requests appear earlier than phase-1 coarse requests after the resident prefix,
	// phase 0 gets 2 pages, phase 1 gets the remaining 2 pages, and phase 2 gets none.
	LocalVector<uint32_t> requests;
	const uint32_t res0 = VSM::entry_index(0, VSM::MIP_COUNT - 1, 0, 0);
	const uint32_t res1 = VSM::entry_index(0, VSM::MIP_COUNT - 2, 0, 0);
	requests.push_back(res0);
	requests.push_back(res1);
	requests.push_back(20);
	requests.push_back(21);
	requests.push_back(10);
	requests.push_back(11);
	requests.push_back(12);
	requests.push_back(22);
	for (uint32_t r : requests) {
		fixture.table[r] |= VSM::ENTRY_REQUESTED;
	}
	fixture.table[res0] |= VSM::ENTRY_COARSE;
	fixture.table[res1] |= VSM::ENTRY_COARSE;
	fixture.table[10] |= VSM::ENTRY_COARSE;
	fixture.table[11] |= VSM::ENTRY_COARSE;
	fixture.table[12] |= VSM::ENTRY_COARSE;
	VSM::AllocationCounts counts = VSM::allocate_model(fixture.table.ptr(), fixture.table.size(), fixture.pages.ptr(), fixture.pages.size(), fixture.width, requests.ptr(), requests.size(), 1, cache, 2);
	for (uint32_t r : requests) {
		fixture.table[r] = VSM::entry_after_emit(fixture.table[r]);
	}
	CHECK(counts.new_pages == 4);
	CHECK(counts.overflow == 4);
	CHECK(counts.resident_overflow == 0);
	CHECK(fixture.page_of(res0) != UINT32_MAX);
	CHECK(fixture.page_of(res1) != UINT32_MAX);
	CHECK(fixture.page_of(10) != UINT32_MAX);
	CHECK(fixture.page_of(11) != UINT32_MAX);
	CHECK(fixture.page_of(12) == UINT32_MAX);
	CHECK(fixture.page_of(20) == UINT32_MAX);
	CHECK(fixture.consistent());

	// Next frame with 6 new fine requests overflowing the 4-page pool: the two active resident pages
	// keep their physical pages and are never stolen or evicted; the two unrequested cached pages are stolen.
	const uint32_t p0 = fixture.page_of(res0);
	const uint32_t p1 = fixture.page_of(res1);
	LocalVector<uint32_t> frame2;
	frame2.push_back(res0);
	frame2.push_back(res1);
	for (uint32_t i = 30; i < 36; i++) {
		frame2.push_back(i);
	}
	for (uint32_t r : frame2) {
		fixture.table[r] |= VSM::ENTRY_REQUESTED;
	}
	fixture.table[res0] |= VSM::ENTRY_COARSE;
	fixture.table[res1] |= VSM::ENTRY_COARSE;
	counts = VSM::allocate_model(fixture.table.ptr(), fixture.table.size(), fixture.pages.ptr(), fixture.pages.size(), fixture.width, frame2.ptr(), frame2.size(), 2, cache, 2);
	for (uint32_t r : frame2) {
		fixture.table[r] = VSM::entry_after_emit(fixture.table[r]);
	}
	CHECK(counts.kept == 2);
	CHECK(counts.stolen == 2);
	CHECK(counts.overflow == 4);
	CHECK(counts.resident_overflow == 0);
	CHECK(fixture.page_of(res0) == p0);
	CHECK(fixture.page_of(res1) == p1);
	CHECK(fixture.consistent());

	// When the pool has fewer pages than the resident requests themselves, resident_overflow counts them.
	AllocationFixture tiny(1, 1);
	LocalVector<uint32_t> three_res;
	three_res.push_back(res0);
	three_res.push_back(res1);
	three_res.push_back(10);
	for (uint32_t r : three_res) {
		tiny.table[r] |= VSM::ENTRY_REQUESTED | VSM::ENTRY_COARSE;
	}
	counts = VSM::allocate_model(tiny.table.ptr(), tiny.table.size(), tiny.pages.ptr(), tiny.pages.size(), tiny.width, three_res.ptr(), three_res.size(), 1, cache, 3);
	CHECK(counts.new_pages == 1);
	CHECK(counts.overflow == 2);
	CHECK(counts.resident_overflow == 2);
}

TEST_CASE("[VSM] Overflow sampling: resident pages and the last level's far-edge fallback leave zero unmapped samples") {
	// 1. Local lights (spot, omni, area): even when every fine request overflows and only mark_resident
	// pages are mapped, every pixel request hits its face's root page.
	{
		const uint32_t table_maps = 12;
		LocalVector<VSM::MapData> maps;
		maps.resize(table_maps);
		const Transform3D xform(Basis::from_euler(Vector3(0.4f, -0.6f, 0.1f)), Vector3(2.0f, 3.0f, -1.0f));
		maps[0] = VSM::map_data_face(VSM::MAP_KIND_SPOT, 0, xform, 15.0f, 40.0f);
		for (uint32_t face = 0; face < 6; face++) {
			maps[1 + face] = VSM::map_data_face(VSM::MAP_KIND_OMNI, face, xform, 15.0f, 0.0f);
		}
		for (uint32_t face = 0; face < 5; face++) {
			maps[7 + face] = VSM::map_data_face(VSM::MAP_KIND_AREA, face, xform, 15.0f, 0.0f);
		}
		VSM::MarkLight lights[3];
		lights[0].kind = VSM::MAP_KIND_SPOT;
		lights[0].map_base = 0;
		lights[0].flags = VSM::light_flags(VSM::MAP_KIND_SPOT, 1);
		lights[1].kind = VSM::MAP_KIND_OMNI;
		lights[1].map_base = 1;
		lights[1].flags = VSM::light_flags(VSM::MAP_KIND_OMNI, 6);
		lights[2].kind = VSM::MAP_KIND_AREA;
		lights[2].map_base = 7;
		lights[2].flags = VSM::light_flags(VSM::MAP_KIND_AREA, 5);

		LocalVector<uint32_t> table;
		table.resize(table_maps * VSM::ENTRIES_PER_MAP);
		memset(table.ptr(), 0, table.size() * sizeof(uint32_t));
		LocalVector<uint32_t> empty_table(table);
		LocalVector<uint64_t> resident;
		for (const VSM::MarkLight &l : lights) {
			VSM::mark_resident(maps.ptr(), table_maps, l.map_base, l.flags, resident);
		}
		REQUIRE(resident.size() == 12);
		for (uint64_t r : resident) {
			table[uint32_t(r)] = VSM::entry_pack(1, 1, VSM::ENTRY_ALLOCATED);
		}

		VSM::SampleCheck sample;
		sample.table = table.ptr();
		VSM::SampleCheck mutant;
		mutant.table = empty_table.ptr();
		RandomPCG rng(21940);
		LocalVector<uint64_t> requests;
		for (uint32_t i = 0; i < 4096; i++) {
			const Vector3 dir = Vector3(rng.randf() * 2.0f - 1.0f, rng.randf() * 2.0f - 1.0f, rng.randf() * 2.0f - 1.0f).normalized();
			const Vector3 world = xform.origin + dir * (0.2f + rng.randf() * 14.0f);
			const float footprint = 0.001f + rng.randf() * 0.05f;
			for (const VSM::MarkLight &l : lights) {
				requests.clear();
				VSM::mark_local_point(maps.ptr(), table_maps, nullptr, l, world, footprint, 0, false, false, requests, &sample);
				requests.clear();
				VSM::mark_local_point(maps.ptr(), table_maps, nullptr, l, world, footprint, 0, false, false, requests, &mutant);
			}
		}
		CHECK(sample.sampled > 4000);
		CHECK(sample.fallback > 0);
		CHECK(sample.unmapped == 0);
		CHECK(mutant.unmapped == sample.sampled);
	}

	// 2. Directional clipmaps: adversarial camera positions, 1 to 16 levels, overflow biases 0 to 3,
	// and points both near the camera and in the last level's far strip outside mip 4's window.
	{
		const Basis sun = Basis::from_euler(Vector3(Math::deg_to_rad(-90.0f), 0.0f, 0.0f));
		LocalVector<VSM::MapData> maps;
		maps.resize(VSM::CLIPMAP_MAX_LEVELS);
		LocalVector<uint32_t> bias;
		bias.resize(VSM::CLIPMAP_MAX_LEVELS);
		LocalVector<uint32_t> table;
		table.resize(VSM::CLIPMAP_MAX_LEVELS * VSM::ENTRIES_PER_MAP);
		LocalVector<uint32_t> table_no_mip4;
		table_no_mip4.resize(table.size());
		LocalVector<uint32_t> table_no_far;
		table_no_far.resize(table.size());

		RandomPCG rng(21941);
		uint32_t sampled = 0;
		uint32_t fallbacks = 0;
		uint32_t unmapped = 0;
		uint32_t unbiased_unmapped = 0;
		uint32_t mutant_no_mip4_unmapped = 0;
		uint32_t mutant_no_far_unmapped = 0;
		LocalVector<uint64_t> resident;
		LocalVector<uint64_t> requests;

		for (uint32_t trial = 0; trial < 4096; trial++) {
			const uint32_t count = 1 + rng.rand(VSM::CLIPMAP_MAX_LEVELS);
			const uint32_t flags = VSM::light_flags(VSM::MAP_KIND_DIRECTIONAL, count);
			Vector2 center(rng.randf() * 2000.0f - 1000.0f, rng.randf() * 2000.0f - 1000.0f);
			if ((trial & 3) == 0) {
				// Adversarial camera origins with large remainder modulo 16 and 32.
				const float page0 = VSM::clipmap_page_world_size(0);
				center = Vector2(-page0 * float(1 + int32_t(rng.rand(64))), page0 * float(63 - int32_t(rng.rand(16))));
			}
			for (uint32_t level = 0; level < count; level++) {
				maps[level] = VSM::map_data_clipmap(sun, level, VSM::clipmap_level_origin(center, level), 1.0e6f);
				bias[level] = rng.rand(VSM::OVERFLOW_MAX_BIAS + 1);
			}
			// Pick a world point inside either a random level or the last level's far strip.
			const uint32_t pick = (trial & 1) ? (count - 1) : rng.rand(count);
			const Vector2i o(maps[pick].origin[0], maps[pick].origin[1]);
			const float page_m = VSM::clipmap_page_world_size(pick);
			const int32_t wp_x = (trial & 1) ? int32_t(48 + rng.rand(16)) : int32_t(rng.rand(VSM::PAGES_PER_SIDE));
			const int32_t wp_y = (trial & 1) ? int32_t(48 + rng.rand(16)) : int32_t(rng.rand(VSM::PAGES_PER_SIDE));
			const float lx = (float(o.x + wp_x) + 0.25f + 0.5f * rng.randf()) * page_m;
			const float ly = (float(o.y + wp_y) + 0.25f + 0.5f * rng.randf()) * page_m;
			// sun looks straight down (-90 deg around X): light X is world +X, light Y is world -Z.
			const Vector3 world(lx, 0.0f, -ly);
			const float footprint = std::ldexp(1.0f, int32_t(rng.rand(24)) - 12);
			const int32_t lod_bias = int32_t(rng.rand(5)) - 2;

			resident.clear();
			VSM::mark_resident(maps.ptr(), count, 0, flags, resident);
			requests.clear();
			VSM::mark_directional_point(maps.ptr(), count, bias.ptr(), 0, flags, world, footprint, lod_bias, false, requests);
			if (requests.is_empty()) {
				continue;
			}

			memset(table.ptr(), 0, count * VSM::ENTRIES_PER_MAP * sizeof(uint32_t));
			memset(table_no_mip4.ptr(), 0, count * VSM::ENTRIES_PER_MAP * sizeof(uint32_t));
			memset(table_no_far.ptr(), 0, count * VSM::ENTRIES_PER_MAP * sizeof(uint32_t));
			for (uint64_t r : resident) {
				const uint32_t idx = uint32_t(r);
				table[idx] = VSM::entry_pack(1, 0, VSM::ENTRY_ALLOCATED);
				table_no_far[idx] = VSM::entry_pack(1, 0, VSM::ENTRY_ALLOCATED);
				const uint32_t in_map = idx % VSM::ENTRIES_PER_MAP;
				if (in_map >= VSM::mip_entry_offset(VSM::CLIPMAP_RESIDENT_MIP + 1)) {
					table_no_mip4[idx] = VSM::entry_pack(1, 0, VSM::ENTRY_ALLOCATED);
				}
			}
			// Allocate coarse requests from mark_directional_point (the last level's far-edge fallback),
			// leaving every fine request overflowed.
			for (uint64_t r : requests) {
				if (r & VSM::REQUEST_COARSE) {
					const uint32_t idx = uint32_t(r);
					table[idx] = VSM::entry_pack(2, 0, VSM::ENTRY_ALLOCATED);
					table_no_mip4[idx] = VSM::entry_pack(2, 0, VSM::ENTRY_ALLOCATED);
				}
			}

			VSM::SampleCheck s_ok;
			s_ok.table = table.ptr();
			VSM::SampleCheck s_no_mip4;
			s_no_mip4.table = table_no_mip4.ptr();
			VSM::SampleCheck s_no_far;
			s_no_far.table = table_no_far.ptr();
			requests.clear();
			VSM::mark_directional_point(maps.ptr(), count, bias.ptr(), 0, flags, world, footprint, lod_bias, false, requests, &s_ok);
			requests.clear();
			VSM::mark_directional_point(maps.ptr(), count, bias.ptr(), 0, flags, world, footprint, lod_bias, false, requests, &s_no_mip4);
			requests.clear();
			VSM::mark_directional_point(maps.ptr(), count, bias.ptr(), 0, flags, world, footprint, lod_bias, false, requests, &s_no_far);
			sampled += s_ok.sampled;
			fallbacks += s_ok.fallback;
			unmapped += s_ok.unmapped;
			mutant_no_mip4_unmapped += s_no_mip4.unmapped;
			mutant_no_far_unmapped += s_no_far.unmapped;

			// Also verify unbiased projection (without p_map_bias, as #222 looks up).
			VSM::SampleCheck s_unbiased;
			s_unbiased.table = table.ptr();
			requests.clear();
			VSM::mark_directional_point(maps.ptr(), count, nullptr, 0, flags, world, footprint, lod_bias, false, requests, &s_unbiased);
			unbiased_unmapped += s_unbiased.unmapped;
		}
		CHECK(sampled > 3000);
		CHECK(fallbacks > 0);
		CHECK(unmapped == 0);
		CHECK(unbiased_unmapped == 0);
		CHECK(mutant_no_mip4_unmapped > 0);
		CHECK(mutant_no_far_unmapped > 0);
	}
}

TEST_CASE("[VSM] Directional coarse marking and lookup cover (origin & 31) == 31 and the last level's far strip") {
	const Basis sun = Basis::from_euler(Vector3(Math::deg_to_rad(-90.0f), 0.0f, 0.0f));
	// 1. Camera at (-0.05, -0.05): (origin & 31) == 31 across every level, so mips 5 and 6 end 1 page
	// past the camera in every level and miss world.x = +5.0 m; mip 4 (CLIPMAP_RESIDENT_MIP) covers it,
	// with zero pixel requests.
	{
		const uint32_t count = 7;
		const uint32_t flags = VSM::light_flags(VSM::MAP_KIND_DIRECTIONAL, count);
		LocalVector<VSM::MapData> maps;
		maps.resize(count);
		for (uint32_t level = 0; level < count; level++) {
			maps[level] = VSM::map_data_clipmap(sun, level, VSM::clipmap_level_origin(Vector2(-0.05f, -0.05f), level), 100.0f);
			CHECK((uint32_t(maps[level].origin[0]) & 31u) == 31u);
		}
		const Vector3 world(5.0f, 0.0f, 0.0f);
		const Vector2i texel0 = VSM::clipmap_texel0(maps[0], world);
		for (uint32_t level = 0; level < count; level++) {
			const Vector2i origin(maps[level].origin[0], maps[level].origin[1]);
			const Vector2i lt = VSM::clipmap_level_texel(texel0, level);
			CHECK_FALSE(VSM::clipmap_in_window(origin, lt, 5));
			CHECK_FALSE(VSM::clipmap_in_window(origin, lt, 6));
		}

		LocalVector<uint64_t> froxel_requests;
		VSM::mark_directional_point(maps.ptr(), count, nullptr, 0, flags, world, 1.0f, 0, true, froxel_requests);
		REQUIRE(!froxel_requests.is_empty());

		LocalVector<uint32_t> table;
		table.resize(count * VSM::ENTRIES_PER_MAP);
		memset(table.ptr(), 0, table.size() * sizeof(uint32_t));
		for (uint64_t r : froxel_requests) {
			table[uint32_t(r)] = VSM::entry_pack(1, 0, VSM::ENTRY_ALLOCATED);
		}
		const VSM::LookupResult froxel_hit = VSM::clipmap_coarse_sample(table.ptr(), count, maps.ptr(), 0, count, texel0);
		CHECK(froxel_hit.valid);
		CHECK(froxel_hit.mip == VSM::CLIPMAP_RESIDENT_MIP);

		// Same for a transparent box at +5 m with zero pixel requests.
		LocalVector<uint64_t> box_requests;
		const AABB box(Vector3(4.8f, 0.5f, -0.2f), Vector3(0.4f, 0.4f, 0.4f));
		VSM::mark_directional_box(maps.ptr(), count, 0, flags, box, box_requests);
		REQUIRE(!box_requests.is_empty());
		memset(table.ptr(), 0, table.size() * sizeof(uint32_t));
		for (uint64_t r : box_requests) {
			table[uint32_t(r)] = VSM::entry_pack(2, 0, VSM::ENTRY_ALLOCATED);
		}
		VSM::SampleCheck box_sample;
		box_sample.table = table.ptr();
		box_requests.clear();
		VSM::mark_directional_box(maps.ptr(), count, 0, flags, box, box_requests, &box_sample);
		CHECK(box_sample.sampled == 9);
		CHECK(box_sample.unmapped == 0);
	}

	// 2. Adversarial trials across 1..16 levels, (origin & 31) == 31, and the last level's far strip
	// ([64 - (origin mod 16), 64)) with ZERO pixel requests (sky behind fog froxels and transparent
	// boxes) and all fine pages unallocated.
	{
		LocalVector<VSM::MapData> maps;
		maps.resize(VSM::CLIPMAP_MAX_LEVELS);
		LocalVector<uint32_t> table_froxel;
		table_froxel.resize(VSM::CLIPMAP_MAX_LEVELS * VSM::ENTRIES_PER_MAP);
		LocalVector<uint32_t> table_box;
		table_box.resize(table_froxel.size());
		LocalVector<uint32_t> table_resident_only;
		table_resident_only.resize(table_froxel.size());
		LocalVector<uint32_t> table_no_mip4;
		table_no_mip4.resize(table_froxel.size());

		RandomPCG rng(21942);
		uint32_t froxel_sampled = 0;
		uint32_t froxel_unmapped = 0;
		uint32_t froxel_no_far_unmapped = 0;
		uint32_t froxel_no_mip4_unmapped = 0;
		uint32_t box_sampled = 0;
		uint32_t box_unmapped = 0;
		uint32_t box_no_far_unmapped = 0;
		uint32_t box_no_mip4_unmapped = 0;
		LocalVector<uint64_t> resident;
		LocalVector<uint64_t> requests;

		for (uint32_t trial = 0; trial < 4096; trial++) {
			const uint32_t count = 1 + rng.rand(VSM::CLIPMAP_MAX_LEVELS);
			const uint32_t flags = VSM::light_flags(VSM::MAP_KIND_DIRECTIONAL, count);
			const float page0 = VSM::clipmap_page_world_size(0);
			Vector2 center(rng.randf() * 1000.0f - 500.0f, rng.randf() * 1000.0f - 500.0f);
			if ((trial & 1) == 0) {
				// Force (origin & 31) == 31 across levels, or large origin mod 16.
				center = Vector2(-page0 * float(1 + int32_t(rng.rand(4))), -page0 * float(1 + int32_t(rng.rand(16))));
			}
			for (uint32_t level = 0; level < count; level++) {
				maps[level] = VSM::map_data_clipmap(sun, level, VSM::clipmap_level_origin(center, level), 1.0e6f);
			}
			const uint32_t pick = (trial % 3 != 0) ? (count - 1) : rng.rand(count);
			const Vector2i o(maps[pick].origin[0], maps[pick].origin[1]);
			const float page_m = VSM::clipmap_page_world_size(pick);
			const int32_t wp_x = (trial % 3 == 1) ? int32_t(48 + rng.rand(16)) : ((trial % 3 == 2) ? int32_t(32 + rng.rand(17)) : int32_t(rng.rand(VSM::PAGES_PER_SIDE)));
			const int32_t wp_y = (trial % 3 == 1) ? int32_t(48 + rng.rand(16)) : int32_t(rng.rand(VSM::PAGES_PER_SIDE));
			const float lx = (float(o.x + wp_x) + 0.1f + 0.8f * rng.randf()) * page_m;
			const float ly = (float(o.y + wp_y) + 0.1f + 0.8f * rng.randf()) * page_m;
			const Vector3 world(lx, 2.0f, -ly);
			const Vector3 box_size(page_m * (0.2f + 1.5f * rng.randf()), 1.0f, page_m * (0.2f + 1.5f * rng.randf()));
			const AABB box(world - box_size * 0.5f, box_size);

			resident.clear();
			VSM::mark_resident(maps.ptr(), count, 0, flags, resident);
			const size_t bytes = count * VSM::ENTRIES_PER_MAP * sizeof(uint32_t);
			memset(table_froxel.ptr(), 0, bytes);
			memset(table_box.ptr(), 0, bytes);
			memset(table_resident_only.ptr(), 0, bytes);
			memset(table_no_mip4.ptr(), 0, bytes);
			for (uint64_t r : resident) {
				const uint32_t idx = uint32_t(r);
				table_froxel[idx] = VSM::entry_pack(1, 0, VSM::ENTRY_ALLOCATED);
				table_box[idx] = VSM::entry_pack(1, 0, VSM::ENTRY_ALLOCATED);
				table_resident_only[idx] = VSM::entry_pack(1, 0, VSM::ENTRY_ALLOCATED);
				if ((idx % VSM::ENTRIES_PER_MAP) >= VSM::mip_entry_offset(VSM::CLIPMAP_RESIDENT_MIP + 1)) {
					table_no_mip4[idx] = VSM::entry_pack(1, 0, VSM::ENTRY_ALLOCATED);
				}
			}

			// Froxel marking only (zero pixel requests).
			requests.clear();
			VSM::mark_directional_point(maps.ptr(), count, nullptr, 0, flags, world, 1.0f, 0, true, requests);
			for (uint64_t r : requests) {
				const uint32_t idx = uint32_t(r);
				table_froxel[idx] = VSM::entry_pack(2, 0, VSM::ENTRY_ALLOCATED);
				if ((idx % VSM::ENTRIES_PER_MAP) < VSM::mip_entry_offset(VSM::CLIPMAP_RESIDENT_MIP)) {
					table_no_mip4[idx] = VSM::entry_pack(2, 0, VSM::ENTRY_ALLOCATED);
				}
			}
			VSM::SampleCheck sf_ok{ table_froxel.ptr(), nullptr, 0, 0, 0, 0 };
			VSM::SampleCheck sf_no_far{ table_resident_only.ptr(), nullptr, 0, 0, 0, 0 };
			VSM::SampleCheck sf_no_mip4{ table_no_mip4.ptr(), nullptr, 0, 0, 0, 0 };
			requests.clear();
			VSM::mark_directional_point(maps.ptr(), count, nullptr, 0, flags, world, 1.0f, 0, true, requests, &sf_ok);
			requests.clear();
			VSM::mark_directional_point(maps.ptr(), count, nullptr, 0, flags, world, 1.0f, 0, true, requests, &sf_no_far);
			requests.clear();
			VSM::mark_directional_point(maps.ptr(), count, nullptr, 0, flags, world, 1.0f, 0, true, requests, &sf_no_mip4);
			froxel_sampled += sf_ok.sampled;
			froxel_unmapped += sf_ok.unmapped;
			froxel_no_far_unmapped += sf_no_far.unmapped;
			froxel_no_mip4_unmapped += sf_no_mip4.unmapped;

			// Box marking only (zero pixel requests).
			memset(table_no_mip4.ptr(), 0, bytes);
			for (uint64_t r : resident) {
				const uint32_t idx = uint32_t(r);
				if ((idx % VSM::ENTRIES_PER_MAP) >= VSM::mip_entry_offset(VSM::CLIPMAP_RESIDENT_MIP + 1)) {
					table_no_mip4[idx] = VSM::entry_pack(1, 0, VSM::ENTRY_ALLOCATED);
				}
			}
			requests.clear();
			VSM::mark_directional_box(maps.ptr(), count, 0, flags, box, requests);
			for (uint64_t r : requests) {
				const uint32_t idx = uint32_t(r);
				table_box[idx] = VSM::entry_pack(3, 0, VSM::ENTRY_ALLOCATED);
				if ((idx % VSM::ENTRIES_PER_MAP) < VSM::mip_entry_offset(VSM::CLIPMAP_RESIDENT_MIP)) {
					table_no_mip4[idx] = VSM::entry_pack(3, 0, VSM::ENTRY_ALLOCATED);
				}
			}
			VSM::SampleCheck sb_ok{ table_box.ptr(), nullptr, 0, 0, 0, 0 };
			VSM::SampleCheck sb_no_far{ table_resident_only.ptr(), nullptr, 0, 0, 0, 0 };
			VSM::SampleCheck sb_no_mip4{ table_no_mip4.ptr(), nullptr, 0, 0, 0, 0 };
			requests.clear();
			VSM::mark_directional_box(maps.ptr(), count, 0, flags, box, requests, &sb_ok);
			requests.clear();
			VSM::mark_directional_box(maps.ptr(), count, 0, flags, box, requests, &sb_no_far);
			requests.clear();
			VSM::mark_directional_box(maps.ptr(), count, 0, flags, box, requests, &sb_no_mip4);
			box_sampled += sb_ok.sampled;
			box_unmapped += sb_ok.unmapped;
			box_no_far_unmapped += sb_no_far.unmapped;
			box_no_mip4_unmapped += sb_no_mip4.unmapped;
		}
		CHECK(froxel_sampled > 3000);
		CHECK(froxel_unmapped == 0);
		CHECK(froxel_no_far_unmapped > 0);
		CHECK(froxel_no_mip4_unmapped > 0);
		CHECK(box_sampled > 20000);
		CHECK(box_unmapped == 0);
		CHECK(box_no_far_unmapped > 0);
		CHECK(box_no_mip4_unmapped > 0);
	}
}

// GodotNext: vsm_settings_debug (#221, plan §21.3.2 S1, budget V6, §13.2). The product surface: the tier
// rule with mocked device capabilities, the shadow methods, the scalability presets, moving lights, the
// frame stats and the debug-draw and rendering-info enums.

TEST_CASE("[VSM] Tier rule: Forward+, 32-bit image atomics and indirect-count draws, first failure wins (#221)") {
	VSM::TierInputs inputs;
	inputs.forward_plus = true;
	inputs.image_atomic_32 = true;
	inputs.indirect_count = true;
	CHECK(VSM::evaluate_tier(inputs) == VSM::TIER_REASON_NONE);
	VSM::TierInputs no_indirect = inputs;
	no_indirect.indirect_count = false;
	CHECK(VSM::evaluate_tier(no_indirect) == VSM::TIER_REASON_NO_INDIRECT_COUNT);
	VSM::TierInputs no_atomics = inputs;
	no_atomics.image_atomic_32 = false;
	no_atomics.indirect_count = false;
	CHECK(VSM::evaluate_tier(no_atomics) == VSM::TIER_REASON_NO_IMAGE_ATOMIC_32);
	VSM::TierInputs mobile = no_atomics;
	mobile.forward_plus = false;
	CHECK(VSM::evaluate_tier(mobile) == VSM::TIER_REASON_UNSUPPORTED_RENDERER);
	for (uint32_t reason = 0; reason < VSM::TIER_REASON_MAX; reason++) {
		CHECK(String(VSM::get_tier_reason_name(VSM::TierReason(reason))) != "INVALID");
		CHECK(VSM::format_fallback_line(VSM::TierReason(reason)).contains(VSM::get_tier_reason_name(VSM::TierReason(reason))));
	}
}

static VSM::TierReason vsm_mocked_tier(uint64_t p_features) {
	RenderingDevice *rd = memnew(RenderingDevice);
	rd->set_feature_mock(true, p_features);
	VSM::TierInputs inputs;
	RendererRD::VirtualShadowMapStorage::collect_device_inputs(rd, true, inputs);
	memdelete(rd);
	return VSM::evaluate_tier(inputs);
}

TEST_CASE("[VSM] Tier rule with mocked device capabilities (#221, §13.2)") {
	const uint64_t atomics = uint64_t(1) << RenderingDevice::SUPPORTS_IMAGE_ATOMIC_32_BIT;
	const uint64_t indirect_count = uint64_t(1) << RenderingDevice::SUPPORTS_DRAW_INDIRECT_COUNT;
	CHECK(vsm_mocked_tier(atomics | indirect_count) == VSM::TIER_REASON_NONE);
	CHECK(vsm_mocked_tier(indirect_count) == VSM::TIER_REASON_NO_IMAGE_ATOMIC_32);
	CHECK(vsm_mocked_tier(atomics) == VSM::TIER_REASON_NO_INDIRECT_COUNT);
	CHECK(vsm_mocked_tier(0) == VSM::TIER_REASON_NO_IMAGE_ATOMIC_32);
	// 64-bit atomics are not part of the rule: VSM without them is an extension, not a fallback.
	CHECK(vsm_mocked_tier(atomics | indirect_count | (uint64_t(1) << RenderingDevice::SUPPORTS_BUFFER_INT64_ATOMICS)) == VSM::TIER_REASON_NONE);
	VSM::TierInputs none;
	RendererRD::VirtualShadowMapStorage::collect_device_inputs(nullptr, true, none);
	CHECK(VSM::evaluate_tier(none) == VSM::TIER_REASON_NO_IMAGE_ATOMIC_32);
}

TEST_CASE("[VSM] Mocked capabilities: a virtual project with no VSM tier uses the atlas and logs one reason line (#221)") {
	VSM::reset_startup_state();
	const uint32_t lines_before = VSM::get_fallback_line_count();
	// A device without 32-bit image atomics, in a project with shadow_method = virtual.
	VSM::set_startup_state(vsm_mocked_tier(uint64_t(1) << RenderingDevice::SUPPORTS_DRAW_INDIRECT_COUNT), VSM::SHADOW_METHOD_VIRTUAL, false);
	CHECK(VSM::get_tier_reason() == VSM::TIER_REASON_NO_IMAGE_ATOMIC_32);
	CHECK_FALSE(VSM::is_vsm_available());
	// Every method resolves to the atlas: no light gets virtual maps (LightStorage::update_light_buffers()
	// and the scene cull ask light_uses_vsm()).
	for (uint32_t method = 0; method < RSE::LIGHT_SHADOW_METHOD_MAX; method++) {
		CHECK_FALSE(VSM::light_uses_vsm(RSE::LightShadowMethod(method)));
	}
	// One line, however many lights and starts report it.
	ERR_PRINT_OFF;
	VSM::report_project_shadow_method();
	VSM::note_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_PROJECT);
	VSM::note_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_VIRTUAL);
	VSM::report_project_shadow_method();
	ERR_PRINT_ON;
	CHECK(VSM::get_fallback_line_count() - lines_before == 1);
	// With the tier, the same project gets virtual maps and prints nothing.
	VSM::reset_startup_state();
	const uint32_t lines_tier = VSM::get_fallback_line_count();
	VSM::set_startup_state(VSM::TIER_REASON_NONE, VSM::SHADOW_METHOD_VIRTUAL, false);
	CHECK(VSM::light_uses_vsm(RSE::LIGHT_SHADOW_METHOD_PROJECT));
	CHECK(VSM::light_uses_vsm(RSE::LIGHT_SHADOW_METHOD_VIRTUAL));
	CHECK_FALSE(VSM::light_uses_vsm(RSE::LIGHT_SHADOW_METHOD_ATLAS));
	VSM::report_project_shadow_method();
	CHECK(VSM::get_fallback_line_count() == lines_tier);
	// An atlas project prints nothing until a light asks for VIRTUAL.
	VSM::reset_startup_state();
	VSM::set_startup_state(VSM::TIER_REASON_NO_INDIRECT_COUNT, VSM::SHADOW_METHOD_ATLAS, false);
	const uint32_t lines_atlas = VSM::get_fallback_line_count();
	VSM::report_project_shadow_method();
	VSM::note_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_ATLAS);
	CHECK(VSM::get_fallback_line_count() == lines_atlas);
	ERR_PRINT_OFF;
	VSM::note_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_VIRTUAL);
	ERR_PRINT_ON;
	CHECK(VSM::get_fallback_line_count() - lines_atlas == 1);
	VSM::reset_startup_state();
}

TEST_CASE("[VSM] Light shadow methods resolve to VIRTUAL, ATLAS or DISTANCE_FIELD; unimplemented ones take PROJECT (#221, #214)") {
	using M = RSE::LightShadowMethod;
	const VSM::ShadowMethod projects[2] = { VSM::SHADOW_METHOD_ATLAS, VSM::SHADOW_METHOD_VIRTUAL };
	for (VSM::ShadowMethod project : projects) {
		const M project_result = project == VSM::SHADOW_METHOD_VIRTUAL ? RSE::LIGHT_SHADOW_METHOD_VIRTUAL : RSE::LIGHT_SHADOW_METHOD_ATLAS;
		CHECK(VSM::resolve_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_PROJECT, project, true, false) == project_result);
		CHECK(VSM::resolve_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_VIRTUAL, project, true, false) == RSE::LIGHT_SHADOW_METHOD_VIRTUAL);
		CHECK(VSM::resolve_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_ATLAS, project, true, false) == RSE::LIGHT_SHADOW_METHOD_ATLAS);
		CHECK(VSM::resolve_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_RAY_TRACED, project, true, false) == project_result);
		CHECK(VSM::resolve_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_DISTANCE_FIELD, project, true, false) == project_result);
		CHECK(VSM::resolve_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_DISTANCE_FIELD, project, true, true) == RSE::LIGHT_SHADOW_METHOD_DISTANCE_FIELD);
		CHECK(VSM::resolve_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_DISTANCE_FIELD, project, false, true) == RSE::LIGHT_SHADOW_METHOD_DISTANCE_FIELD);
		// Below both tiers everything takes the atlas.
		for (uint32_t method = 0; method < RSE::LIGHT_SHADOW_METHOD_MAX; method++) {
			CHECK(VSM::resolve_light_shadow_method(M(method), project, false, false) == RSE::LIGHT_SHADOW_METHOD_ATLAS);
		}
	}
	CHECK(VSM::is_light_shadow_method_implemented(RSE::LIGHT_SHADOW_METHOD_PROJECT));
	CHECK(VSM::is_light_shadow_method_implemented(RSE::LIGHT_SHADOW_METHOD_VIRTUAL));
	CHECK(VSM::is_light_shadow_method_implemented(RSE::LIGHT_SHADOW_METHOD_ATLAS));
	CHECK_FALSE(VSM::is_light_shadow_method_implemented(RSE::LIGHT_SHADOW_METHOD_RAY_TRACED));
	CHECK(VSM::is_light_shadow_method_implemented(RSE::LIGHT_SHADOW_METHOD_DISTANCE_FIELD));
}

TEST_CASE("[VSM] Unimplemented shadow methods warn once each (#221, #214)") {
	VSM::reset_startup_state();
	const uint32_t before = VSM::get_unimplemented_warning_count();
	ERR_PRINT_OFF;
	for (int i = 0; i < 3; i++) {
		VSM::note_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_RAY_TRACED);
	}
	ERR_PRINT_ON;
	CHECK(VSM::get_unimplemented_warning_count() - before == 1);
	ERR_PRINT_OFF;
	VSM::note_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_DISTANCE_FIELD);
	VSM::note_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_DISTANCE_FIELD);
	VSM::note_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_RAY_TRACED);
	ERR_PRINT_ON;
	CHECK(VSM::get_unimplemented_warning_count() - before == 1);
	// Implemented methods never warn.
	VSM::note_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_PROJECT);
	VSM::note_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_VIRTUAL);
	VSM::note_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_ATLAS);
	VSM::note_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_DISTANCE_FIELD);
	CHECK(VSM::get_unimplemented_warning_count() - before == 1);
	VSM::reset_startup_state();
}

TEST_CASE("[VSM][SceneTree] The dummy renderer prints no fallback line and no shadow-method warning (#221)") {
	// Headless runs, imports and exports use the dummy renderer, which draws no shadows.
	RenderingServer *rs = RenderingServer::get_singleton();
	REQUIRE(rs != nullptr);
	REQUIRE(RSG::light_storage != nullptr);
	CHECK_FALSE(RSG::light_storage->draws_shadows());
	ProjectSettings *settings = ProjectSettings::get_singleton();
	REQUIRE(settings != nullptr);
	const StringName setting = "rendering/lights_and_shadows/shadow_method";
	const Variant saved = settings->get_setting(setting, 0);
	settings->set_setting(setting, int64_t(VSM::SHADOW_METHOD_VIRTUAL));
	VSM::reset_startup_state();
	const uint32_t lines = VSM::get_fallback_line_count();
	const uint32_t warnings = VSM::get_unimplemented_warning_count();
	// What RenderingServerDefault does at startup on this renderer: the project's method is recorded (Light3D's
	// warnings read it), and nothing is printed.
	VSM::report_project_shadow_method(RSG::light_storage->draws_shadows());
	CHECK(VSM::get_project_shadow_method() == VSM::SHADOW_METHOD_VIRTUAL);
	CHECK(VSM::get_fallback_line_count() == lines);
	DirectionalLight3D *light = memnew(DirectionalLight3D);
	for (uint32_t method = 0; method < RSE::LIGHT_SHADOW_METHOD_MAX; method++) {
		rs->light_set_shadow_method(light->get_base(), RSE::LightShadowMethod(method));
		light->set_shadow_method(Light3D::ShadowMethod(method));
	}
	memdelete(light);
	CHECK(VSM::get_fallback_line_count() == lines);
	CHECK(VSM::get_unimplemented_warning_count() == warnings);
	// A renderer that draws shadows (Mobile, Compatibility) prints the one line for the same project.
	ERR_PRINT_OFF;
	VSM::report_project_shadow_method(true);
	ERR_PRINT_ON;
	CHECK(VSM::get_fallback_line_count() - lines == 1);
	settings->set_setting(setting, saved);
	VSM::reset_startup_state();
}

// RenderingServer has a setter and no getter (as for the other light parameters), and the doctests run on the
// dummy renderer, which stores nothing: every value is bound on the server and round-trips through Light3D here.
// tests/vsm/product_surface.gd sets PROJECT, ATLAS, RAY_TRACED and DISTANCE_FIELD on a light of the Forward+
// light storage and checks what each resolves to.
TEST_CASE("[VSM][SceneTree] Every shadow method is bound on RenderingServer and round-trips through Light3D (#221)") {
	const char *names[RSE::LIGHT_SHADOW_METHOD_MAX] = { "PROJECT", "VIRTUAL", "ATLAS", "RAY_TRACED", "DISTANCE_FIELD" };
	List<StringName> constants;
	ClassDB::get_enum_constants("RenderingServer", "LightShadowMethod", &constants);
	CHECK(constants.size() == RSE::LIGHT_SHADOW_METHOD_MAX + 1); // Plus LIGHT_SHADOW_METHOD_MAX.
	List<StringName> light_constants;
	ClassDB::get_enum_constants("Light3D", "ShadowMethod", &light_constants);
	CHECK(light_constants.size() == RSE::LIGHT_SHADOW_METHOD_MAX);
	RenderingServer *rs = RenderingServer::get_singleton();
	REQUIRE(rs != nullptr);
	VSM::reset_startup_state();
	DirectionalLight3D *light = memnew(DirectionalLight3D);
	CHECK(light->get_shadow_method() == Light3D::SHADOW_METHOD_PROJECT);
	ERR_PRINT_OFF;
	for (uint32_t method = 0; method < RSE::LIGHT_SHADOW_METHOD_MAX; method++) {
		const String name = String("LIGHT_SHADOW_METHOD_") + names[method];
		bool valid = false;
		CHECK(ClassDB::get_integer_constant("RenderingServer", name, &valid) == int64_t(method));
		CHECK(valid);
		CHECK(ClassDB::get_integer_constant_enum("RenderingServer", name) == StringName("LightShadowMethod"));
		valid = false;
		CHECK(ClassDB::get_integer_constant("Light3D", String("SHADOW_METHOD_") + names[method], &valid) == int64_t(method));
		CHECK(valid);
		CHECK(String(VSM::get_light_shadow_method_name(RSE::LightShadowMethod(method))) == names[method]);
		// The bound server setter takes every value, and the node keeps it (unimplemented ones are kept, too:
		// only their resolution falls back).
		Callable::CallError error;
		const Variant light_rid = light->get_base();
		const Variant value = int64_t(method);
		const Variant *args[2] = { &light_rid, &value };
		rs->callp("light_set_shadow_method", args, 2, error);
		CHECK(error.error == Callable::CallError::CALL_OK);
		light->set_shadow_method(Light3D::ShadowMethod(method));
		CHECK(int(light->get_shadow_method()) == int(method));
		CHECK(int(light->get("shadow_method")) == int(method));
	}
	ERR_PRINT_ON;
	memdelete(light);
	VSM::reset_startup_state();
}

static bool vsm_has_warning(const PackedStringArray &p_warnings, const String &p_text) {
	for (const String &warning : p_warnings) {
		if (warning.contains(p_text)) {
			return true;
		}
	}
	return false;
}

TEST_CASE("[VSM][SceneTree] Light3D shows the tier reason on lights that would use virtual shadow maps (#221)") {
	// The inspector hint of issue step 3: Light3D's configuration warnings read the tier rule's stored reason.
	const String fallback = "Virtual shadow maps are not available";
	OmniLight3D *light = memnew(OmniLight3D);
	light->set_shadow(true);
	ERR_PRINT_OFF;
	// A virtual project on a device without 32-bit image atomics (mocked, as the tier tests above).
	VSM::reset_startup_state();
	VSM::set_startup_state(vsm_mocked_tier(uint64_t(1) << RenderingDevice::SUPPORTS_DRAW_INDIRECT_COUNT), VSM::SHADOW_METHOD_VIRTUAL, false);
	PackedStringArray warnings = light->get_configuration_warnings();
	CHECK(vsm_has_warning(warnings, fallback));
	CHECK(vsm_has_warning(warnings, "NO_IMAGE_ATOMIC_32"));
	light->set_shadow_method(Light3D::SHADOW_METHOD_ATLAS);
	CHECK_FALSE(vsm_has_warning(light->get_configuration_warnings(), fallback));
	light->set_shadow_method(Light3D::SHADOW_METHOD_VIRTUAL);
	CHECK(vsm_has_warning(light->get_configuration_warnings(), fallback));
	// Unimplemented methods take PROJECT (here Virtual), so they show both warnings.
	light->set_shadow_method(Light3D::SHADOW_METHOD_RAY_TRACED);
	warnings = light->get_configuration_warnings();
	CHECK(vsm_has_warning(warnings, "not implemented yet"));
	CHECK(vsm_has_warning(warnings, fallback));
	// A light without shadows shows nothing.
	light->set_shadow(false);
	CHECK_FALSE(vsm_has_warning(light->get_configuration_warnings(), fallback));
	light->set_shadow(true);
	light->set_shadow_method(Light3D::SHADOW_METHOD_PROJECT);
	// An atlas project shows nothing for PROJECT lights, but a VIRTUAL light still names the reason.
	VSM::reset_startup_state();
	VSM::set_startup_state(VSM::TIER_REASON_NO_INDIRECT_COUNT, VSM::SHADOW_METHOD_ATLAS, false);
	CHECK_FALSE(vsm_has_warning(light->get_configuration_warnings(), fallback));
	light->set_shadow_method(Light3D::SHADOW_METHOD_VIRTUAL);
	warnings = light->get_configuration_warnings();
	CHECK(vsm_has_warning(warnings, fallback));
	CHECK(vsm_has_warning(warnings, "NO_INDIRECT_COUNT"));
	// At the tier, no light shows it.
	VSM::reset_startup_state();
	VSM::set_startup_state(VSM::TIER_REASON_NONE, VSM::SHADOW_METHOD_VIRTUAL, false);
	CHECK_FALSE(vsm_has_warning(light->get_configuration_warnings(), fallback));
	light->set_shadow_method(Light3D::SHADOW_METHOD_PROJECT);
	CHECK_FALSE(vsm_has_warning(light->get_configuration_warnings(), fallback));
	ERR_PRINT_ON;
	memdelete(light);
	VSM::reset_startup_state();
}

TEST_CASE("[VSM] Scalability presets: pool memory per preset equals V6 within 1 MiB (#221, Q15)") {
	const uint64_t v6_mib[VSM::POOL_QUALITY_MAX] = { 64, 128, 256, 512 };
	for (uint32_t quality = 0; quality < VSM::POOL_QUALITY_MAX; quality++) {
		const VSM::Scalability values = VSM::default_scalability(VSM::PoolQuality(quality));
		const int64_t bytes = int64_t(VSM::pool_bytes(values.max_physical_pages));
		CHECK(Math::abs(bytes - int64_t(v6_mib[quality] << 20)) <= int64_t(1) << 20);
		CHECK(values.max_physical_pages == VSM::pool_quality_pages(VSM::PoolQuality(quality)));
		// The settings read back the same values when the project sets none (or the registered defaults).
		const VSM::Scalability read = VSM::read_scalability(VSM::PoolQuality(quality));
		CHECK(read.max_physical_pages == values.max_physical_pages);
		CHECK(read.lod_bias_directional == values.lod_bias_directional);
		CHECK(read.lod_bias_local == values.lod_bias_local);
		CHECK(read.lod_bias_directional_moving == values.lod_bias_directional_moving);
		CHECK(read.lod_bias_local_moving == values.lod_bias_local_moving);
		// Moving lights take one mip more than static ones.
		CHECK(values.lod_bias_directional_moving == values.lod_bias_directional + 1);
		CHECK(values.lod_bias_local_moving == values.lod_bias_local + 1);
		// The pool texture stays within 16384 texels per side.
		const Vector2i grid = VSM::pool_page_grid(values.max_physical_pages);
		CHECK(grid.x * int32_t(VSM::PAGE_SIZE) <= 16384);
		CHECK(grid.y * int32_t(VSM::PAGE_SIZE) <= 16384);
	}
	CHECK(VSM::default_scalability(VSM::POOL_QUALITY_LOW).lod_bias_directional == 1);
	CHECK(VSM::default_scalability(VSM::POOL_QUALITY_HIGH).lod_bias_directional == 0);
	CHECK(VSM::DEFAULT_QUALITY == VSM::POOL_QUALITY_HIGH);
	// Out-of-range values clamp to what the pool and the mark pass take.
	VSM::Scalability wild;
	wild.max_physical_pages = 1;
	wild.lod_bias_directional = -100;
	wild.lod_bias_local = 100;
	const VSM::Scalability clamped = VSM::clamp_scalability(wild);
	CHECK(clamped.max_physical_pages == VSM::MIN_PHYSICAL_PAGES);
	CHECK(clamped.lod_bias_directional == -VSM::MAX_SETTING_LOD_BIAS);
	CHECK(clamped.lod_bias_local == VSM::MAX_SETTING_LOD_BIAS);
	wild.max_physical_pages = UINT32_MAX;
	CHECK(VSM::clamp_scalability(wild).max_physical_pages == VSM::POOL_MAX_PAGES);
}

TEST_CASE("[VSM] Scalability presets read their .low, .medium, .high and .epic settings (#221)") {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	REQUIRE(settings != nullptr);
	const String pages_high = String(VSM::SETTING_MAX_PHYSICAL_PAGES) + ".high";
	const String bias_low = String(VSM::SETTING_LOD_BIAS_LOCAL_MOVING) + ".low";
	// RenderingServer::init() registers them in the test setup, as in a running project.
	REQUIRE(settings->has_setting(pages_high));
	REQUIRE(settings->has_setting(bias_low));
	REQUIRE(settings->has_setting(VSM::SETTING_QUALITY));
	const Variant old_pages = settings->get_setting(pages_high);
	const Variant old_bias = settings->get_setting(bias_low);
	const Variant old_quality = settings->get_setting(VSM::SETTING_QUALITY);

	settings->set_setting(pages_high, 2048); // A quarter of the High pool, as GODOT_DEV_VSM=25% does.
	settings->set_setting(bias_low, 3);
	settings->set_setting(VSM::SETTING_QUALITY, int(VSM::POOL_QUALITY_LOW));
	CHECK(VSM::read_scalability(VSM::POOL_QUALITY_HIGH).max_physical_pages == 2048);
	CHECK(VSM::read_scalability(VSM::POOL_QUALITY_LOW).lod_bias_local_moving == 3);
	CHECK(VSM::read_scalability(VSM::POOL_QUALITY_MEDIUM).max_physical_pages == VSM::pool_quality_pages(VSM::POOL_QUALITY_MEDIUM));
	CHECK(VSM::read_quality() == VSM::POOL_QUALITY_LOW);
	settings->set_setting(VSM::SETTING_QUALITY, 9); // Out of range: the default preset.
	CHECK(VSM::read_quality() == VSM::DEFAULT_QUALITY);

	settings->set_setting(pages_high, old_pages);
	settings->set_setting(bias_low, old_bias);
	settings->set_setting(VSM::SETTING_QUALITY, old_quality);
	CHECK(VSM::read_scalability(VSM::POOL_QUALITY_HIGH).max_physical_pages == VSM::pool_quality_pages(VSM::POOL_QUALITY_HIGH));
	CHECK(VSM::read_quality() == VSM::DEFAULT_QUALITY);
}

TEST_CASE("[VSM] Project shadow method: atlas by default and for new projects until #223 (#221)") {
	CHECK(VSM::DEFAULT_SHADOW_METHOD == VSM::SHADOW_METHOD_ATLAS);
	CHECK(VSM::NEW_PROJECT_SHADOW_METHOD == VSM::SHADOW_METHOD_ATLAS);
	ProjectSettings *settings = ProjectSettings::get_singleton();
	REQUIRE(settings != nullptr);
	const String name = "rendering/lights_and_shadows/shadow_method";
	// RenderingServer::init() registers it in the test setup, as in a running project.
	REQUIRE(settings->has_setting(name));
	const Variant old = settings->get_setting(name);
	CHECK(int(settings->property_get_revert(name)) == int(VSM::SHADOW_METHOD_ATLAS));
	CHECK(int(old) == int(VSM::SHADOW_METHOD_ATLAS));
	settings->set_setting(name, int(VSM::SHADOW_METHOD_VIRTUAL));
	CHECK(VSM::read_project_shadow_method() == VSM::SHADOW_METHOD_VIRTUAL);
	settings->set_setting(name, int(VSM::SHADOW_METHOD_ATLAS));
	CHECK(VSM::read_project_shadow_method() == VSM::SHADOW_METHOD_ATLAS);
	settings->set_setting(name, old);
}

TEST_CASE("[VSM] Moving lights take the moving bias and step back one mip per 30 frames (#221)") {
	CHECK(VSM::moving_lod_bias(0, 1, 0) == 1);
	CHECK(VSM::moving_lod_bias(0, 1, VSM::MOVING_DECAY_FRAMES - 1) == 1);
	CHECK(VSM::moving_lod_bias(0, 1, VSM::MOVING_DECAY_FRAMES) == 0);
	CHECK(VSM::moving_lod_bias(0, 2, VSM::MOVING_DECAY_FRAMES) == 1);
	CHECK(VSM::moving_lod_bias(0, 2, 2 * VSM::MOVING_DECAY_FRAMES) == 0);
	CHECK(VSM::moving_lod_bias(1, -1, 0) == -2);
	CHECK(VSM::moving_lod_bias(1, -1, VSM::MOVING_DECAY_FRAMES) == -1);
	CHECK(VSM::moving_lod_bias(1, -1, 5 * VSM::MOVING_DECAY_FRAMES) == 0);
	CHECK(VSM::moving_lod_bias(2, 2, 0) == 0);
	CHECK(VSM::moving_lod_bias(0, 1, VSM::NEVER_MOVED) == 0);

	TestVSMStorage storage;
	const RID light = light_rid(77);
	CHECK(storage.note_light_motion(light, Transform3D()) == VSM::NEVER_MOVED); // No maps yet.
	storage.set_frame(10);
	REQUIRE(storage.map_allocate(light, VSM::MAP_KIND_SPOT) != VSM::MAP_NONE);
	const Transform3D first(Basis(), Vector3(1.0f, 2.0f, 3.0f));
	CHECK(storage.note_light_motion(light, first) == VSM::NEVER_MOVED);
	storage.set_frame(11);
	CHECK(storage.note_light_motion(light, first) == VSM::NEVER_MOVED);
	const Transform3D moved(Basis(), Vector3(1.5f, 2.0f, 3.0f));
	storage.set_frame(12);
	CHECK(storage.note_light_motion(light, moved) == 0);
	CHECK(storage.note_light_motion(light, moved) == 0); // A second view of the same frame.
	storage.set_frame(12 + VSM::MOVING_DECAY_FRAMES);
	CHECK(storage.note_light_motion(light, moved) == VSM::MOVING_DECAY_FRAMES);
	storage.free_all();
}

TEST_CASE("[VSM] A map's moving bias adds to the view's LOD bias in the CPU model of page marking (#221)") {
	const uint32_t table_maps = 16;
	LocalVector<VSM::MapData> maps;
	maps.resize(table_maps);
	LocalVector<uint32_t> overflow;
	overflow.resize(table_maps);
	memset(overflow.ptr(), 0, table_maps * sizeof(uint32_t));
	// A spot at map 7, as in the marking test above.
	const Transform3D spot_transform(Basis::from_euler(Vector3(Math::deg_to_rad(-90.0f), 0.0f, 0.0f)), Vector3(0.0f, 4.0f, 0.0f));
	maps[7] = VSM::map_data_face(VSM::MAP_KIND_SPOT, 0, spot_transform, 20.0f, 30.0f);
	VSM::MarkLight spot;
	spot.kind = VSM::MAP_KIND_SPOT;
	spot.inv_radius = 1.0f / 20.0f;
	spot.map_base = 7;
	spot.flags = VSM::light_flags(VSM::MAP_KIND_SPOT, 1);
	LocalVector<uint64_t> with_map_bias;
	LocalVector<uint64_t> with_view_bias;
	LocalVector<uint64_t> without;
	VSM::mark_local_point(maps.ptr(), table_maps, overflow.ptr(), spot, Vector3(0.0f, 0.0f, 0.0f), 0.001f, 0, false, false, without);
	VSM::mark_local_point(maps.ptr(), table_maps, overflow.ptr(), spot, Vector3(0.0f, 0.0f, 0.0f), 0.001f, 1, false, false, with_view_bias);
	maps[7].lod_bias = 1;
	VSM::mark_local_point(maps.ptr(), table_maps, overflow.ptr(), spot, Vector3(0.0f, 0.0f, 0.0f), 0.001f, 0, false, false, with_map_bias);
	REQUIRE(with_map_bias.size() == 1);
	REQUIRE(without.size() == 1);
	CHECK(with_map_bias[0] == with_view_bias[0]);
	CHECK(with_map_bias[0] != without[0]);
	CHECK(with_map_bias[0] - VSM::ENTRIES_PER_MAP * 7 >= VSM::mip_entry_offset(1));

	// A directional light reads level 0's moving bias for every level.
	const Basis sun = Basis::from_euler(Vector3(Math::deg_to_rad(-90.0f), 0.0f, 0.0f));
	const Vector3 camera(0.3f, 2.0f, 0.7f);
	const Vector2 center(sun.get_column(0).normalized().dot(camera), sun.get_column(1).normalized().dot(camera));
	for (uint32_t level = 0; level < 7; level++) {
		maps[level] = VSM::map_data_clipmap(sun, level, VSM::clipmap_level_origin(center, level), 100.0f);
	}
	const uint32_t sun_flags = VSM::light_flags(VSM::MAP_KIND_DIRECTIONAL, 7);
	const Vector3 point(0.3f, 0.0f, 0.7f);
	LocalVector<uint64_t> dir_without;
	LocalVector<uint64_t> dir_view_bias;
	LocalVector<uint64_t> dir_map_bias;
	VSM::mark_directional_point(maps.ptr(), table_maps, overflow.ptr(), 0, sun_flags, point, std::ldexp(1.0f, -10), 0, false, dir_without);
	VSM::mark_directional_point(maps.ptr(), table_maps, overflow.ptr(), 0, sun_flags, point, std::ldexp(1.0f, -10), 1, false, dir_view_bias);
	maps[0].lod_bias = 1;
	VSM::mark_directional_point(maps.ptr(), table_maps, overflow.ptr(), 0, sun_flags, point, std::ldexp(1.0f, -10), 0, false, dir_map_bias);
	REQUIRE(dir_map_bias.size() == dir_view_bias.size());
	REQUIRE(dir_map_bias.size() > 0);
	CHECK(dir_map_bias[0] == dir_view_bias[0]);
	CHECK(dir_map_bias[0] != dir_without[0]);
}

TEST_CASE("[VSM] Frame stats: the frame's views add up; allocated pages come from the last view (#221)") {
	VSM::reset_frame_stats();
	CHECK(VSM::get_frame_stat(VSM::FRAME_STAT_REQUESTED) == 0);
	LocalVector<uint32_t> views;
	views.resize(2 * VSM::STATE_VIEW_END);
	memset(views.ptr(), 0, views.size() * sizeof(uint32_t));
	uint32_t *first = views.ptr();
	uint32_t *second = views.ptr() + VSM::STATE_VIEW_END;
	first[VSM::STATE_REQUESTS] = 100;
	first[VSM::STATE_DIRTY] = 40;
	first[VSM::STATE_OVERFLOW] = 3;
	first[VSM::STATE_KEPT] = 50;
	first[VSM::STATE_NEW] = 40;
	first[VSM::STATE_CACHED] = 10;
	second[VSM::STATE_REQUESTS] = 30;
	second[VSM::STATE_DIRTY] = 5;
	second[VSM::STATE_OVERFLOW] = 0;
	second[VSM::STATE_KEPT] = 120;
	second[VSM::STATE_NEW] = 5;
	second[VSM::STATE_CACHED] = 7;
	second[VSM::STATE_STOLEN] = 3; // Cached pages the assign pass gave to new requests: counted in NEW, not CACHED.
	VSM::publish_frame_stats(views.ptr(), 2);
	CHECK(VSM::get_frame_stat(VSM::FRAME_STAT_REQUESTED) == 130);
	CHECK(VSM::get_frame_stat(VSM::FRAME_STAT_RERENDERED) == 45);
	CHECK(VSM::get_frame_stat(VSM::FRAME_STAT_OVERFLOW) == 3);
	CHECK(VSM::get_frame_stat(VSM::FRAME_STAT_ALLOCATED) == 129);
	// RenderingServer reports them as RENDERING_INFO_VSM_PAGES_* (the test setup's server, on the dummy
	// renderer). The Performance monitors read the same values; tests/vsm/product_surface.gd checks them in a
	// running project, where the Performance singleton exists.
	RenderingServer *rs = RenderingServer::get_singleton();
	REQUIRE(rs != nullptr);
	CHECK(rs->get_rendering_info(RSE::RENDERING_INFO_VSM_PAGES_REQUESTED) == 130);
	CHECK(rs->get_rendering_info(RSE::RENDERING_INFO_VSM_PAGES_ALLOCATED) == 129);
	CHECK(rs->get_rendering_info(RSE::RENDERING_INFO_VSM_PAGES_RERENDERED) == 45);
	CHECK(rs->get_rendering_info(RSE::RENDERING_INFO_VSM_PAGES_OVERFLOW) == 3);
	VSM::publish_frame_stats(nullptr, 0); // Ignored.
	CHECK(VSM::get_frame_stat(VSM::FRAME_STAT_REQUESTED) == 130);
	VSM::reset_frame_stats();
	CHECK(VSM::get_frame_stat(VSM::FRAME_STAT_ALLOCATED) == 0);
}

TEST_CASE("[VSM] Debug draws, rendering info and monitors are appended and stay aligned (#221)") {
	// Appended after the path tracer's debug draw (#167), which landed first.
	CHECK(int(RSE::VIEWPORT_DEBUG_DRAW_VSM_MASK) == int(RSE::VIEWPORT_DEBUG_DRAW_PATH_TRACER) + 1);
	CHECK(int(RSE::VIEWPORT_DEBUG_DRAW_VSM_CLIPMAP) == int(RSE::VIEWPORT_DEBUG_DRAW_VSM_MASK) + 5);
	CHECK(int(Viewport::DEBUG_DRAW_VSM_MASK) == int(RSE::VIEWPORT_DEBUG_DRAW_VSM_MASK));
	CHECK(int(Viewport::DEBUG_DRAW_VSM_MIP) == int(RSE::VIEWPORT_DEBUG_DRAW_VSM_MIP));
	CHECK(int(Viewport::DEBUG_DRAW_VSM_PAGES) == int(RSE::VIEWPORT_DEBUG_DRAW_VSM_PAGES));
	CHECK(int(Viewport::DEBUG_DRAW_VSM_CACHE) == int(RSE::VIEWPORT_DEBUG_DRAW_VSM_CACHE));
	CHECK(int(Viewport::DEBUG_DRAW_VSM_OVERDRAW) == int(RSE::VIEWPORT_DEBUG_DRAW_VSM_OVERDRAW));
	CHECK(int(Viewport::DEBUG_DRAW_VSM_CLIPMAP) == int(RSE::VIEWPORT_DEBUG_DRAW_VSM_CLIPMAP));
	const RSE::ViewportDebugDraw draws[6] = { RSE::VIEWPORT_DEBUG_DRAW_VSM_MASK, RSE::VIEWPORT_DEBUG_DRAW_VSM_MIP, RSE::VIEWPORT_DEBUG_DRAW_VSM_PAGES, RSE::VIEWPORT_DEBUG_DRAW_VSM_CACHE, RSE::VIEWPORT_DEBUG_DRAW_VSM_OVERDRAW, RSE::VIEWPORT_DEBUG_DRAW_VSM_CLIPMAP };
	for (RSE::ViewportDebugDraw draw : draws) {
		// Overlays: the view keeps virtual geometry and is not routed through the legacy path.
		CHECK_FALSE(RendererRD::nextgeo_is_unsupported_debug_draw(draw));
		CHECK_FALSE(RendererRD::nextgeo_is_debug_view(draw));
	}
	CHECK(int(RSE::RENDERING_INFO_VSM_PAGES_ALLOCATED) == int(RSE::RENDERING_INFO_NEXTGI_REFERENCE_SAMPLES) + 1);
	CHECK(int(RSE::RENDERING_INFO_VSM_PAGES_OVERFLOW) == int(RSE::RENDERING_INFO_MAX) - 1);
	CHECK(int(Performance::RENDER_VSM_PAGES_OVERFLOW) == int(Performance::MONITOR_MAX) - 1);
}

TEST_CASE("[VSM] The VSM_OVERDRAW pair pass stays within the dispatch limit at every pair capacity (#221)") {
	// vsm_debug_pages.glsl MODE_PAIRS: 64 threads per group, a grid-stride loop over the pairs. The pair list
	// grows to 1 << 22 pairs (#218), which one thread per pair would dispatch as 65,536 groups; D3D12 and many
	// Vulkan devices take at most 65,535 (LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X).
	const uint32_t max_pairs = 1u << 22;
	static_assert(VSM::grid_stride_groups(1u << 22, 64, 65535) == 65535);
	CHECK(VSM::grid_stride_groups(0, 64, 65535) == 1);
	CHECK(VSM::grid_stride_groups(1, 64, 65535) == 1);
	CHECK(VSM::grid_stride_groups(64, 64, 65535) == 1);
	CHECK(VSM::grid_stride_groups(65, 64, 65535) == 2);
	CHECK(VSM::grid_stride_groups(max_pairs, 64, 65535) == 65535);
	CHECK(VSM::grid_stride_groups(max_pairs, 64, 0x7FFFFFFF) == 65536);
	CHECK(VSM::grid_stride_groups(max_pairs, 64, 0) == 1);
	CHECK(VSM::grid_stride_groups(100, 0, 65535) == 100);
	// The loop's CPU model: thread t visits t, t + stride, ... below the pair count, so every pair is visited
	// exactly once whatever the cap.
	const uint32_t pair_counts[] = { 0u, 1u, 63u, 64u, 65u, 4096u, 65535u * 64u + 1u, max_pairs };
	const uint32_t limits[] = { 1u, 7u, 65535u, 0x7FFFFFFFu };
	for (uint32_t pairs : pair_counts) {
		for (uint32_t limit : limits) {
			const uint32_t groups = VSM::grid_stride_groups(pairs, 64, limit);
			CHECK(groups >= 1);
			CHECK(groups <= limit);
			const uint64_t stride = uint64_t(groups) * 64u;
			uint64_t visits = 0;
			for (uint64_t t = 0; t < stride && t < pairs; t++) {
				visits += (uint64_t(pairs) - 1u - t) / stride + 1u;
			}
			CHECK(visits == pairs);
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
	VSM::LookupResult near_result = VSM::lookup_clipmap(table.ptr(), maps, 0, origin, near_texel, 0);
	CHECK(near_result.valid);
	CHECK(near_result.mip == 1);
	CHECK(near_result.entry == near_entry);
	CHECK(near_result.texel_x == 9 * 64 + 37); // floor(-1974 / 2) = -987, which is 37 modulo 64.
	CHECK(near_result.texel_y == 4 * 64 + 2);
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

TEST_CASE("[VSM] Reversed-Z R32_UINT depth encoding preserves strict order and round-trips (#218)") {
	// Strictly sorted ascending depths (nearer first, including negative directional light-space depths):
	// packed uint32 values must be strictly descending so R32_UINT imageAtomicMax selects the nearest occluder.
	const float ordered_depths[] = {
		-10000.0f, -250.5f, -12.25f, -1.0f, -0.001f, -1.0e-6f,
		0.0f,
		1.0e-6f, 0.001f, 0.05f, 1.0f, 3.5f, 42.125f, 1000.0f, 65536.0f, 1.0e6f
	};
	const uint32_t count = sizeof(ordered_depths) / sizeof(ordered_depths[0]);
	uint32_t prev_packed = 0xFFFFFFFFu;
	for (uint32_t i = 0; i < count; i++) {
		const float d = ordered_depths[i];
		const uint32_t p = VSM::depth_pack(d);
		CHECK(p > 0u);
		CHECK(p < 0xFFFFFFFFu);
		CHECK(p < prev_packed);
		const float u = VSM::depth_unpack(p);
		if (d == 0.0f) {
			CHECK(u == 0.0f);
		} else {
			CHECK(u == d);
		}
		prev_packed = p;
	}
	// -0.0f and +0.0f map to the same canonical value.
	CHECK(VSM::depth_pack(-0.0f) == VSM::depth_pack(0.0f));

	// Cleared pool texels hold 0u, which unpacks to +INF and loses against every valid depth.
	const float empty_depth = VSM::depth_unpack(0u);
	CHECK(std::isinf(empty_depth));
	CHECK(empty_depth > 0.0f);

	// NaN is rejected to 0u; +/-INF stay strictly inside (0u, 0xFFFFFFFFu) so 0xFFFFFFFFu stays reserved for #277 sinks.
	CHECK(VSM::depth_pack(std::nanf("")) == 0u);
	const uint32_t pos_inf = VSM::depth_pack(float(Math::INF));
	const uint32_t neg_inf = VSM::depth_pack(-float(Math::INF));
	CHECK(pos_inf > 0u);
	CHECK(pos_inf < neg_inf);
	CHECK(neg_inf < 0xFFFFFFFFu);

	// Random finite round-trip and monotonicity fuzz.
	RandomPCG rng(21801);
	uint32_t order_failures = 0;
	uint32_t roundtrip_failures = 0;
	for (uint32_t i = 0; i < 2000; i++) {
		const float a = (rng.randf() * 2.0f - 1.0f) * 5000.0f;
		const float b = (rng.randf() * 2.0f - 1.0f) * 5000.0f;
		const uint32_t pa = VSM::depth_pack(a);
		const uint32_t pb = VSM::depth_pack(b);
		if (VSM::depth_unpack(pa) != (a == 0.0f ? 0.0f : a)) {
			roundtrip_failures++;
		}
		if (a < b && !(pa > pb)) {
			order_failures++;
		} else if (a > b && !(pa < pb)) {
			order_failures++;
		}
	}
	CHECK(roundtrip_failures == 0);
	CHECK(order_failures == 0);
}

TEST_CASE("[VSM] Sub-instance AABB construction handles 3D/2D MultiMesh, particle trails and inactive sentinels (#218)") {
	const AABB unit_box(Vector3(-0.5f, -1.0f, -0.25f), Vector3(1.0f, 2.0f, 0.5f));
	// Parent model transform: translate by (10, 20, -5), scale X by 2.
	const float model[12] = {
		2.0f, 0.0f, 0.0f, 10.0f,
		0.0f, 1.0f, 0.0f, 20.0f,
		0.0f, 0.0f, 1.0f, -5.0f
	};

	// 1. 3D MultiMesh with stride = 5 vec4s (transform + color + custom) and motion-vector current_offset = 1.
	{
		float buf[3 * 5 * 4] = {};
		// Slot 0 (skipped by current_offset = 1): zero scale.
		// Slot 1 (sub_index = 0 at current_offset = 1): translate local by (3, -2, 4).
		float *slot1 = buf + 1 * 20;
		slot1[0] = 1.0f;
		slot1[3] = 3.0f;
		slot1[5] = 1.0f;
		slot1[7] = -2.0f;
		slot1[10] = 1.0f;
		slot1[11] = 4.0f;

		AABB world;
		CHECK(VSM::sub_instance_box(model, buf, 0, 1, 5, false, 1, unit_box, 0.25f, world));
		// Sub-space X is [2.5, 3.5] -> model X is [15.0, 17.0], plus 0.25 margin -> [14.75, 17.25].
		// Sub-space Y is [-3.0, -1.0] -> model Y is [17.0, 19.0], plus 0.25 margin -> [16.75, 19.25].
		// Sub-space Z is [3.75, 4.25] -> model Z is [-1.25, -0.75], plus 0.25 margin -> [-1.5, -0.5].
		CHECK(world.position.is_equal_approx(Vector3(14.75f, 16.75f, -1.5f)));
		CHECK(world.size.is_equal_approx(Vector3(2.5f, 2.5f, 1.0f)));

		// Slot 2 has zero transform -> rejected cleanly.
		AABB rejected;
		CHECK_FALSE(VSM::sub_instance_box(model, buf, 1, 1, 5, false, 1, unit_box, 0.0f, rejected));
	}

	// 2. 2D MultiMesh (format_2d = true, stride = 2 vec4s): implicit Z row (0, 0, 1, 0).
	{
		const float identity_model[12] = {
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f
		};
		const float buf2d[8] = {
			1.0f, 0.0f, 0.0f, 5.0f,
			0.0f, 2.0f, 0.0f, -3.0f
		};
		AABB world2d;
		CHECK(VSM::sub_instance_box(identity_model, buf2d, 0, 0, 2, true, 1, unit_box, 0.0f, world2d));
		CHECK(world2d.position.is_equal_approx(Vector3(4.5f, -5.0f, -0.25f)));
		CHECK(world2d.size.is_equal_approx(Vector3(1.0f, 4.0f, 0.5f)));
	}

	// 3. Inactive GPUParticles3D (-INF diagonal sentinel from particles_copy.glsl) and trailed particles.
	{
		const float identity_model[12] = {
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f
		};
		const float neg_inf = -float(Math::INF);
		// Trail of 3 steps for particle 0: step 0 inactive (-INF), step 1 at x = -2, step 2 at x = +4;
		// particle 1: all 3 steps inactive (-INF).
		float trail_buf[2 * 3 * 20] = {};
		auto set_inactive = [&](float *p_seg) {
			p_seg[0] = neg_inf;
			p_seg[5] = neg_inf;
			p_seg[10] = neg_inf;
		};
		set_inactive(trail_buf + 0 * 20);
		float *seg1 = trail_buf + 1 * 20;
		seg1[0] = 1.0f;
		seg1[3] = -2.0f;
		seg1[5] = 1.0f;
		seg1[10] = 1.0f;
		float *seg2 = trail_buf + 2 * 20;
		seg2[0] = 1.0f;
		seg2[3] = 4.0f;
		seg2[5] = 1.0f;
		seg2[10] = 1.0f;
		set_inactive(trail_buf + 3 * 20);
		set_inactive(trail_buf + 4 * 20);
		set_inactive(trail_buf + 5 * 20);

		AABB trail_world;
		CHECK(VSM::sub_instance_box(identity_model, trail_buf, 0, 0, 5, false, 3, unit_box, 0.0f, trail_world));
		CHECK(trail_world.position.is_equal_approx(Vector3(-2.5f, -1.0f, -0.25f)));
		CHECK(trail_world.size.is_equal_approx(Vector3(7.0f, 2.0f, 0.5f)));

		AABB inactive_world;
		CHECK_FALSE(VSM::sub_instance_box(identity_model, trail_buf, 1, 0, 5, false, 3, unit_box, 0.0f, inactive_world));
	}
}

TEST_CASE("[VSM] Per-page box culling handles toroidal clipmaps, perspective faces, dirty layers and small-caster culling (#218)") {
	// 1. Directional clipmap level 2 (extent 16 m, page 0.25 m at mip 0, 0.5 m at mip 1) with odd negative origin (-3, 5).
	{
		const Basis sun_basis; // Light looks down -Z, light-plane X = +X, Y = +Y.
		const Vector2i origin(-3, 5);
		VSM::MapData maps[2];
		maps[0] = VSM::map_data_clipmap(sun_basis, 2, origin, 200.0f);
		maps[1] = VSM::MapData();

		// At mip 1 (32x32 slots): mip_origin = floor((-3, 5) / 2) = (-2, 2).
		// Absolute mip-1 page (-1, 3) wraps to slot ((-1) & 31, 3 & 31) = (31, 3), covering world X [-0.5, 0.0), Y [1.5, 2.0).
		// Notice absolute mip-1 page (31, 3) (world X [15.5, 16.0)) would ALSO wrap to slot (31, 3), so a box at X = 15.7
		// outside the clipmap window must NOT match slot (31, 3)!
		VSM::DirtyPageRecord dirty[3];
		dirty[0] = { 0u, 1u | (31u << 8u) | (3u << 16u), VSM::entry_pack(0, 0, 0), VSM::ENTRY_DIRTY_STATIC };
		dirty[1] = { 0u, 1u | (31u << 8u) | (3u << 16u), VSM::entry_pack(1, 0, 0), VSM::ENTRY_DIRTY_DYNAMIC };
		dirty[2] = { 0u, 4u | (3u << 8u) | (0u << 16u), VSM::entry_pack(2, 0, 0), VSM::ENTRY_DIRTY_STATIC | VSM::ENTRY_DIRTY_DYNAMIC };
		// Mip 4 (4x4 slots): mip_origin = floor((-3, 5) / 16) = (-1, 0). Abs page (-1, 0) -> slot (3, 0), world X [-4, 0), Y [0, 4).
		const uint32_t counts[2] = { 3u, 0u };
		const uint32_t offsets[2] = { 0u, 0u };
		const uint32_t flags = VSM::light_flags(VSM::MAP_KIND_DIRECTIONAL, 1);

		LocalVector<uint32_t> hits;
		uint32_t small_culled = 0;
		const AABB inside_box(Vector3(-0.3f, 1.6f, -2.0f), Vector3(0.1f, 0.1f, 0.1f));

		// Static caster (extent 0.1 m): mip 1 texel = (16/4096)*2 = 0.0078125 m (passes 1-texel threshold -> hits dirty[0]);
		// mip 4 texel = (16/4096)*16 = 0.0625 m (0.1 >= 0.0625 -> also hits dirty[2]); skips dynamic-only dirty[1]!
		VSM::cull_box_dirty_pages(maps, 2, counts, offsets, dirty, 3, 0, flags, inside_box, false, 1.0f, hits, small_culled);
		CHECK(hits.size() == 2);
		CHECK(hits[0] == 0u);
		CHECK(hits[1] == 2u);
		CHECK(small_culled == 0u);

		// Dynamic caster: skips static-only dirty[0], hits dirty[1] and dirty[2].
		VSM::cull_box_dirty_pages(maps, 2, counts, offsets, dirty, 3, 0, flags, inside_box, true, 1.0f, hits, small_culled);
		CHECK(hits.size() == 2);
		CHECK(hits[0] == 1u);
		CHECK(hits[1] == 2u);

		// Tiny caster (extent 0.02 m): passes mip 1 (0.02 >= 0.0078125) -> hits dirty[0], culled on mip 4 (0.02 < 0.0625) -> small_culled == 1!
		const AABB tiny_box(Vector3(-0.3f, 1.6f, -2.0f), Vector3(0.02f, 0.02f, 0.02f));
		VSM::cull_box_dirty_pages(maps, 2, counts, offsets, dirty, 3, 0, flags, tiny_box, false, 1.0f, hits, small_culled);
		CHECK(hits.size() == 1);
		CHECK(hits[0] == 0u);
		CHECK(small_culled == 1u);

		// Disabling the threshold (min_texels = 0) keeps mip 4 as well.
		VSM::cull_box_dirty_pages(maps, 2, counts, offsets, dirty, 3, 0, flags, tiny_box, false, 0.0f, hits, small_culled);
		CHECK(hits.size() == 2);
		CHECK(small_culled == 0u);

		// Wrapped alias outside the toroidal window at X = 15.7 must NOT match!
		const AABB wrapped_outside(Vector3(15.65f, 1.6f, -2.0f), Vector3(0.1f, 0.1f, 0.1f));
		VSM::cull_box_dirty_pages(maps, 2, counts, offsets, dirty, 3, 0, flags, wrapped_outside, false, 1.0f, hits, small_culled);
		CHECK(hits.is_empty());
	}

	// 2. Perspective spot/area faces: 6-plane frustum, near-plane straddle, conservative closest-w small-caster check.
	{
		const Transform3D spot_xform; // Origin, looking down -Z, 45 deg half-angle, range 20 m.
		VSM::MapData spot_map = VSM::map_data_face(VSM::MAP_KIND_SPOT, 0, spot_xform, 20.0f, 45.0f);
		// Center page at mip 0 is (32, 32), covering UV [0.5, 0.5 + 1/64).
		VSM::DirtyPageRecord dirty[2];
		dirty[0] = { 0u, 0u | (32u << 8u) | (32u << 16u), VSM::entry_pack(0, 0, 0), VSM::ENTRY_DIRTY_STATIC };
		dirty[1] = { 0u, 0u | (5u << 8u) | (5u << 16u), VSM::entry_pack(1, 0, 0), VSM::ENTRY_DIRTY_STATIC };
		const uint32_t counts[1] = { 2u };
		const uint32_t offsets[1] = { 0u };
		const uint32_t flags = VSM::light_flags(VSM::MAP_KIND_SPOT, 1);

		LocalVector<uint32_t> hits;
		uint32_t small_culled = 0;

		// Behind the light (z > 0 -> w < 0 < near_w): rejected.
		VSM::cull_box_dirty_pages(&spot_map, 1, counts, offsets, dirty, 2, 0, flags, AABB(Vector3(-0.5f, -0.5f, 1.0f), Vector3(1.0f, 1.0f, 1.0f)), false, 1.0f, hits, small_culled);
		CHECK(hits.is_empty());

		// Beyond range (z = -25 -> w = 25 > 20): rejected.
		VSM::cull_box_dirty_pages(&spot_map, 1, counts, offsets, dirty, 2, 0, flags, AABB(Vector3(-0.5f, -0.5f, -26.0f), Vector3(1.0f, 1.0f, 1.0f)), false, 1.0f, hits, small_culled);
		CHECK(hits.is_empty());

		// Straddling the near plane (z in [-0.5, +0.5], so min_w = -0.5 <= near_w <= max_w = +0.5): conservatively touches all dirty pages in the face!
		VSM::cull_box_dirty_pages(&spot_map, 1, counts, offsets, dirty, 2, 0, flags, AABB(Vector3(-0.2f, -0.2f, -0.5f), Vector3(0.4f, 0.4f, 1.0f)), false, 1.0f, hits, small_culled);
		CHECK(hits.size() == 2);

		// Small-caster check at w = 16 m (mip 0 texel = (2 / 4096) * 16 = 0.0078125 m): a 0.002 m speck is culled!
		VSM::cull_box_dirty_pages(&spot_map, 1, counts, offsets, dirty, 2, 0, flags, AABB(Vector3(0.001f, 0.001f, -16.002f), Vector3(0.002f, 0.002f, 0.002f)), false, 1.0f, hits, small_culled);
		CHECK(hits.is_empty());
		CHECK(small_culled == 1u);
	}
}

TEST_CASE("[VSM] SceneShaderForwardClustered VSM shader group layout and pair capacity (#218)") {
	using Shader = RendererSceneRenderImplementation::SceneShaderForwardClustered;
	using Version = Shader::ShaderVersion;

	CHECK(int(Version::SHADER_VERSION_VSM_COUNT) == 2);
	CHECK(int(Version::SHADER_VERSION_VSM_DEPTH_UBERSHADER) == int(Version::SHADER_VERSION_VSM_DEPTH) + 1);
	// The VSM block follows the Stage B RR block, and the AVBOIT pre-pass block (#295) follows it, so
	// every VSM variant has a vertex input mask and no other variant shares its index.
	CHECK(int(Version::SHADER_VERSION_VSM_BASE) == int(Version::SHADER_VERSION_VISIBILITY_RESOLVE_RR_GUIDES_BASE) + int(Version::SHADER_VERSION_VISIBILITY_RESOLVE_RR_GUIDES_COUNT));
	CHECK(int(Version::SHADER_VERSION_AVBOIT_BASE) == int(Version::SHADER_VERSION_VSM_BASE) + int(Version::SHADER_VERSION_VSM_COUNT));
	CHECK(int(Shader::ShaderData::VERTEX_INPUT_MASKS_SIZE) >= int(Version::SHADER_VERSION_VSM_BASE) + int(Version::SHADER_VERSION_VSM_COUNT));

	CHECK(VSM::raster_pair_capacity(0) == VSM::RASTER_PAIR_LIST_MIN);
	CHECK(VSM::raster_pair_capacity(2048) == VSM::RASTER_PAIR_LIST_MIN);
	CHECK(VSM::raster_pair_capacity(8192) == 8192u * 64u);
	CHECK(VSM::raster_pair_capacity(16384) == 16384u * 64u);
}

TEST_CASE("[VSM] Billboard sub-instance boxes hold the mesh turned any way about the instance origin (#218)") {
	// A 1 m quad off its origin (as an offset QuadMesh), flat in Z like QuadMesh's AABB.
	const AABB quad(Vector3(-0.3f, -0.6f, 0.0f), Vector3(1.0f, 1.0f, 0.0f));
	CHECK(VSM::view_facing_reach(quad) == doctest::Approx(0.7f + 0.6f));
	const float model[12] = {
		1.0f, 0.0f, 0.0f, 2.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, -1.0f
	};
	// One instance at (3, 1, 0) in the model, scaled by 0.5 on X and 2.5 on Y.
	const float inst[12] = {
		0.5f, 0.0f, 0.0f, 3.0f,
		0.0f, 2.5f, 0.0f, 1.0f,
		0.0f, 0.0f, 1.0f, 0.0f
	};
	AABB flat;
	CHECK(VSM::sub_instance_box(model, inst, 0, 0, 3, false, 1, quad, 0.0f, flat));
	CHECK(flat.size.z == doctest::Approx(0.0f));
	AABB facing;
	CHECK(VSM::sub_instance_box(model, inst, 0, 0, 3, false, 1, quad, 0.0f, facing, true, VSM::view_facing_reach(quad)));
	CHECK(facing.encloses(flat));
	const Vector3 origin(5.0f, 1.0f, -1.0f);
	const float longest = 2.5f; // Keep Scale scales by MODEL_MATRIX's axis lengths; otherwise by 1.
	RandomPCG rng(218);
	uint32_t outside = 0;
	for (int i = 0; i < 2000; i++) {
		// A random unit basis: orthonormal (BILLBOARD_ENABLED, BILLBOARD_PARTICLES) or, every third one,
		// unit axes that are not orthogonal (BILLBOARD_FIXED_Y under a rolled camera).
		Basis basis(Vector3(rng.randf() * 2.0f - 1.0f, rng.randf() * 2.0f - 1.0f, rng.randf() * 2.0f - 1.0f).normalized(), rng.randf() * Math::TAU);
		if (i % 3 == 0) {
			basis = Basis(Vector3(rng.randf() - 0.5f, rng.randf() - 0.5f, rng.randf() - 0.5f).normalized(), Vector3(0.0f, 1.0f, 0.0f), Vector3(rng.randf() - 0.5f, 0.0f, rng.randf() - 0.5f).normalized());
		}
		const float scale = (i % 2 == 0) ? 1.0f : longest;
		for (uint32_t c = 0; c < 4u; c++) {
			const Vector3 lp((c & 1u) ? quad.position.x + quad.size.x : quad.position.x, (c & 2u) ? quad.position.y + quad.size.y : quad.position.y, 0.0f);
			const Vector3 world = origin + basis.xform(lp) * scale;
			if (!facing.grow(1e-4f).has_point(world)) {
				outside++;
			}
		}
	}
	CHECK(outside == 0);
}

TEST_CASE("[VSM] Each map's light camera matches face_camera() and the light's axes (#218)") {
	const Transform3D light(Basis(Vector3(0.3f, 0.8f, -0.2f).normalized(), 1.1f), Vector3(4.0f, -2.0f, 7.5f));
	const VSM::MapKind kinds[3] = { VSM::MAP_KIND_SPOT, VSM::MAP_KIND_OMNI, VSM::MAP_KIND_AREA };
	for (VSM::MapKind kind : kinds) {
		const uint32_t faces = kind == VSM::MAP_KIND_SPOT ? 1u : (kind == VSM::MAP_KIND_OMNI ? 6u : 5u);
		for (uint32_t face = 0; face < faces; face++) {
			const VSM::MapData map = VSM::map_data_face(kind, face, light, 12.0f, 30.0f);
			const Transform3D expected = VSM::face_camera(kind, face, light);
			const Transform3D got = VSM::map_view(map, Vector3(100.0f, 100.0f, 100.0f));
			CHECK(got.basis.get_column(0).is_equal_approx(expected.basis.get_column(0)));
			CHECK(got.basis.get_column(1).is_equal_approx(expected.basis.get_column(1)));
			CHECK(got.basis.get_column(2).is_equal_approx(expected.basis.get_column(2)));
			CHECK(got.origin.distance_to(expected.origin) < 1e-3f);
		}
	}
	// A clipmap level looks along the light (-Z) from the camera position, at every level.
	const Basis sun(Vector3(-0.4f, 1.0f, 0.2f).normalized(), 0.9f);
	const Vector3 camera(12.0f, 3.0f, -8.0f);
	for (uint32_t level = 0; level < 4u; level++) {
		const Transform3D got = VSM::map_view(VSM::map_data_clipmap(sun, level, Vector2i(-3, 5), 200.0f), camera);
		CHECK(got.basis.get_column(0).is_equal_approx(sun.get_column(0).normalized()));
		CHECK(got.basis.get_column(1).is_equal_approx(sun.get_column(1).normalized()));
		CHECK(got.basis.get_column(2).is_equal_approx(sun.get_column(2).normalized()));
		CHECK(got.origin.is_equal_approx(camera));
	}
}

// GodotNext: vsm_projection (#222, plan §21.3 contract S6). The CPU models of vsm_smrt_inc.glsl on an
// analytic scene: ground at y = 0 and a horizontal slab of half size slab_half at slab_height, centered
// on x = slab_x, z = 0. Every allocated page holds the scene's nearest depth at its texel centers, as the
// raster (#218) stores it. godotnext/testbed/tests/vsm/projection.gd checks the GPU light loop.
namespace TestVirtualShadowMapProjection {

using VSM = RendererVirtualShadowMapStorage;

struct ProjectionScene {
	struct Page {
		uint32_t map = 0;
		uint32_t mip = 0;
		uint32_t x = 0;
		uint32_t y = 0;
	};
	static constexpr uint32_t GRID = 32; // Pool pages per row.

	float slab_x = 0.0f;
	float slab_height = 1.0f;
	float slab_half = 0.5f;
	LocalVector<VSM::MapData> maps;
	LocalVector<uint32_t> table;
	LocalVector<Page> pages;
	LocalVector<uint32_t> pool;
	uint32_t pool_width = GRID * VSM::PAGE_SIZE;
	uint32_t pool_height = 0;
	Vector3 light_position; // Faces: the light's position.

	void set_maps(const LocalVector<VSM::MapData> &p_maps) {
		maps = p_maps;
		table.resize(maps.size() * VSM::ENTRIES_PER_MAP);
		for (uint32_t &entry : table) {
			entry = 0;
		}
	}
	// Allocates every page of p_map at p_mip_min and coarser.
	void allocate(uint32_t p_map, uint32_t p_mip_min) {
		for (uint32_t mip = p_mip_min; mip < VSM::MIP_COUNT; mip++) {
			const uint32_t side = VSM::PAGES_PER_SIDE >> mip;
			for (uint32_t y = 0; y < side; y++) {
				for (uint32_t x = 0; x < side; x++) {
					const uint32_t page = pages.size();
					table[VSM::entry_index(p_map, mip, x, y)] = VSM::entry_pack(page % GRID, page / GRID, VSM::ENTRY_ALLOCATED);
					pages.push_back({ p_map, mip, x, y });
				}
			}
		}
	}
	bool covers(float p_x, float p_z) const {
		return std::fabs(p_x - slab_x) <= slab_half && std::fabs(p_z) <= slab_half;
	}
	// Writes every allocated page's texels (layer 0; layer 1 stays empty).
	void fill() {
		const uint32_t rows = (pages.size() + GRID - 1) / GRID;
		pool_height = rows * VSM::PAGE_SIZE;
		pool.resize(uint64_t(pool_width) * pool_height * 2);
		for (uint32_t &texel : pool) {
			texel = 0;
		}
		for (uint32_t page = 0; page < pages.size(); page++) {
			const Page &record = pages[page];
			const VSM::MapData &map = maps[record.map];
			for (uint32_t ty = 0; ty < VSM::PAGE_SIZE; ty++) {
				for (uint32_t tx = 0; tx < VSM::PAGE_SIZE; tx++) {
					const uint32_t px = (page % GRID) * VSM::PAGE_SIZE + tx;
					const uint32_t py = (page / GRID) * VSM::PAGE_SIZE + ty;
					pool[uint64_t(py) * pool_width + px] = map.kind == VSM::MAP_KIND_DIRECTIONAL ? _clipmap_texel(record, tx, ty) : _face_texel(record, tx, ty);
				}
			}
		}
	}
	VSM::ProjectionInput input() const {
		VSM::ProjectionInput in;
		in.table = table.ptr();
		in.table_maps = maps.size();
		in.maps = maps.ptr();
		in.pool = pool.ptr();
		in.pool_width = pool_width;
		in.pool_height = pool_height;
		return in;
	}

private:
	// A clipmap slot's texel: the slot holds the page of the mip's window that wraps to it.
	uint32_t _clipmap_texel(const Page &p_page, uint32_t p_tx, uint32_t p_ty) const {
		const VSM::MapData &map = maps[p_page.map];
		const Vector2i mip_origin = VSM::clipmap_mip_origin(Vector2i(map.origin[0], map.origin[1]), p_page.mip);
		const int32_t side = int32_t(VSM::PAGES_PER_SIDE >> p_page.mip);
		auto unwrap = [side](int32_t p_slot, int32_t p_origin) {
			int32_t d = (p_slot - p_origin) % side;
			return p_origin + (d < 0 ? d + side : d);
		};
		const int32_t ax = unwrap(int32_t(p_page.x), mip_origin.x);
		const int32_t ay = unwrap(int32_t(p_page.y), mip_origin.y);
		// A level l mip m texel spans 2^(l + m) level 0 texels; level 0 has 2^-CLIPMAP_TEXEL0_EXPONENT per meter.
		const double span = std::ldexp(1.0, int32_t(p_page.mip + map.index) + VSM::CLIPMAP_TEXEL0_EXPONENT);
		const double cx = (double(ax * int32_t(VSM::PAGE_SIZE) + int32_t(p_tx)) + 0.5) * span;
		const double cy = (double(ay * int32_t(VSM::PAGE_SIZE) + int32_t(p_ty)) + 0.5) * span;
		// Rows 0 and 1 of level 0 are the light's X and Y axes times 2^-CLIPMAP_TEXEL0_EXPONENT.
		const float scale = std::ldexp(1.0f, VSM::CLIPMAP_TEXEL0_EXPONENT + int32_t(map.index));
		const Vector3 axis_x = Vector3(map.rows[0][0], map.rows[0][1], map.rows[0][2]) * scale;
		const Vector3 axis_y = Vector3(map.rows[1][0], map.rows[1][1], map.rows[1][2]) * scale;
		const Vector3 column = axis_x * float(cx) + axis_y * float(cy);
		const float top = covers(float(column.x), float(column.z)) ? slab_height : 0.0f;
		return VSM::depth_pack(VSM::row_dot(map.rows[2], Vector3(column.x, top, column.z)));
	}
	// A face's texel: the nearest hit of the ray from the light through the texel's center.
	uint32_t _face_texel(const Page &p_page, uint32_t p_tx, uint32_t p_ty) const {
		const VSM::MapData &map = maps[p_page.map];
		Basis rows;
		rows.rows[0] = Vector3(map.rows[0][0], map.rows[0][1], map.rows[0][2]);
		rows.rows[1] = Vector3(map.rows[1][0], map.rows[1][1], map.rows[1][2]);
		rows.rows[2] = Vector3(map.rows[3][0], map.rows[3][1], map.rows[3][2]);
		const float span = float(1u << p_page.mip) / float(VSM::VIRTUAL_SIZE);
		const float u = (float(p_page.x * VSM::PAGE_SIZE + p_tx) + 0.5f) * span;
		const float v = (float(p_page.y * VSM::PAGE_SIZE + p_ty) + 0.5f) * span;
		const Vector3 dir = rows.inverse().xform(Vector3(u * 2.0f - 1.0f, v * 2.0f - 1.0f, 1.0f));
		if (!(dir.y < 0.0f)) {
			return 0u;
		}
		float hit = -light_position.y / dir.y;
		const float slab_t = (slab_height - light_position.y) / dir.y;
		const Vector3 slab_point = light_position + dir * slab_t;
		if (slab_t > 0.0f && covers(float(slab_point.x), float(slab_point.z))) {
			hit = MIN(hit, slab_t);
		}
		return VSM::depth_pack(VSM::row_dot(map.rows[3], light_position + dir * hit));
	}
};

// A sun straight down: the light's -Z is world -Y.
Basis down_sun() {
	return Basis::looking_at(Vector3(0, -1, 0), Vector3(0, 0, -1));
}

// p_count clipmap levels of the sun centered on the world origin.
LocalVector<VSM::MapData> sun_levels(uint32_t p_count) {
	LocalVector<VSM::MapData> maps;
	for (uint32_t level = 0; level < p_count; level++) {
		maps.push_back(VSM::map_data_clipmap(down_sun(), level, VSM::clipmap_level_origin(Vector2(), level), 1000.0f));
	}
	return maps;
}

// The shadow factor at a ground point, averaged over a 4 x 4 grid of noise values.
float ground_shadow_directional(VSM::ProjectionInput p_in, uint32_t p_count, float p_x, float p_z, float p_tan_half_angle) {
	float sum = 0.0f;
	for (uint32_t i = 0; i < 16; i++) {
		p_in.noise = Vector2((float(i % 4) + 0.5f) / 4.0f, (float(i / 4) + 0.5f) / 4.0f);
		sum += VSM::shadow_directional(p_in, 0, VSM::light_flags(VSM::MAP_KIND_DIRECTIONAL, p_count), Vector3(p_x, 0.0f, p_z), Vector3(0, 1, 0), p_tan_half_angle);
	}
	return sum / 16.0f;
}

float ground_shadow_spot(const ProjectionScene &p_scene, VSM::ProjectionInput p_in, float p_x, float p_radius) {
	float sum = 0.0f;
	for (uint32_t i = 0; i < 16; i++) {
		p_in.noise = Vector2((float(i % 4) + 0.5f) / 4.0f, (float(i / 4) + 0.5f) / 4.0f);
		sum += VSM::shadow_local(p_in, 0, VSM::light_flags(VSM::MAP_KIND_SPOT, 1), Vector3(p_x, 0.0f, 0.0f), Vector3(0, 1, 0), p_scene.light_position, p_radius);
	}
	return sum / 16.0f;
}

// The penumbra's width along +x: from the first sample above 0.05 to the first above 0.95. -1 when the
// profile never leaves the umbra or never reaches full light.
template <typename F>
float penumbra_width(F p_shadow_at, float p_from, float p_to, float p_step) {
	float low = -1.0f;
	for (float x = p_from; x <= p_to; x += p_step) {
		const float shadow = p_shadow_at(x);
		if (low < 0.0f && shadow > 0.05f) {
			low = x;
		}
		if (shadow > 0.95f) {
			return low < 0.0f ? 0.0f : x - low;
		}
	}
	return -1.0f;
}

} // namespace TestVirtualShadowMapProjection

TEST_CASE("[VSM] SMRT helpers, presets, settings and the sample flag (#222)") {
	using VSM = RendererVirtualShadowMapStorage;
	// UE documents 8 rays at Epic shadow scalability; 0 rays is one hard tap.
	CHECK(VSM::smrt_preset_rays(VSM::POOL_QUALITY_EPIC) == 8);
	for (uint32_t quality = 0; quality < VSM::POOL_QUALITY_MAX; quality++) {
		CHECK(VSM::smrt_preset_rays(VSM::PoolQuality(quality)) <= VSM::SMRT_MAX_RAYS);
		CHECK(VSM::smrt_preset_steps(VSM::PoolQuality(quality)) >= 1);
		CHECK(VSM::smrt_preset_steps(VSM::PoolQuality(quality)) <= VSM::SMRT_MAX_STEPS);
	}
	CHECK(VSM::smrt_setting(-1, 4, VSM::SMRT_MAX_RAYS) == 4);
	CHECK(VSM::smrt_setting(0, 4, VSM::SMRT_MAX_RAYS) == 0);
	CHECK(VSM::smrt_setting(7, 4, VSM::SMRT_MAX_RAYS) == 7);
	CHECK(VSM::smrt_setting(1000, 4, VSM::SMRT_MAX_RAYS) == VSM::SMRT_MAX_RAYS);

	// Ray samples: inside the unit disk and the light's rectangle, stratified in radius.
	for (uint32_t rays = 1; rays <= VSM::SMRT_MAX_RAYS; rays *= 2) {
		float radius_squared_sum = 0.0f;
		for (uint32_t ray = 0; ray < rays; ray++) {
			const Vector2 disk = VSM::smrt_disk(ray, rays, Vector2(0.5f, 0.25f));
			CHECK(disk.length() <= 1.0f + 1e-5f);
			radius_squared_sum += float(disk.length_squared());
			const Vector2 square = VSM::smrt_square(ray, rays, Vector2(0.5f, 0.25f));
			CHECK(Math::abs(float(square.x)) <= 0.5f);
			CHECK(Math::abs(float(square.y)) <= 0.5f);
			const float jitter = VSM::smrt_jitter(ray, Vector2(0.9f, 0.7f));
			CHECK(jitter >= 0.0f);
			CHECK(jitter < 1.0f);
		}
		// Stratified radii: the mean squared radius of a uniform disk is 1/2.
		CHECK(Math::abs(radius_squared_sum / float(rays) - 0.5f) <= 0.5f / float(rays) + 1e-5f);
	}
	// Steps: geometric from the first height, increasing, below the last.
	float previous = 0.0f;
	for (uint32_t step = 0; step < 6; step++) {
		const float height = VSM::smrt_step(step, 6, 0.0f, 0.01f, 4.0f);
		CHECK(height > previous);
		CHECK(height < 4.0f);
		previous = height;
	}
	CHECK(VSM::smrt_step(0, 6, 0.0f, 0.01f, 4.0f) == doctest::Approx(0.01f));
	CHECK(VSM::smrt_step(3, 6, 0.5f, 0.02f, 0.01f) == doctest::Approx(0.02f)); // Last below first: the first.

	// A directional light's size is 1 - cos(angular diameter).
	const float diameter = Math::deg_to_rad(2.0f);
	CHECK(VSM::directional_tan_half_angle(1.0f - Math::cos(diameter)) == doctest::Approx(Math::tan(diameter * 0.5f)).epsilon(1e-4));
	CHECK(VSM::directional_tan_half_angle(0.0f) == 0.0f);

	// The sample flag leaves the light's maps alone.
	const uint32_t flags = VSM::light_flags(VSM::MAP_KIND_OMNI, 6);
	for (uint32_t face = 0; face < 7; face++) {
		CHECK(VSM::light_map(40, flags | VSM::LIGHT_FLAGS_SAMPLE, face) == VSM::light_map(40, flags, face));
	}

	// Prefiltered Distant: the last two levels of a light with more than two.
	CHECK_FALSE(VSM::prefiltered_distant_level(0, 4));
	CHECK_FALSE(VSM::prefiltered_distant_level(1, 4));
	CHECK(VSM::prefiltered_distant_level(2, 4));
	CHECK(VSM::prefiltered_distant_level(3, 4));
	CHECK_FALSE(VSM::prefiltered_distant_level(1, 2));

	const String defines = VSM::get_glsl_defines();
	CHECK(defines.contains("#define VSM_LIGHT_FLAGS_SAMPLE 128u"));
	CHECK(defines.contains("#define VSM_SMRT_MAX_RAYS 32u"));
	CHECK(defines.contains("#define VSM_SMRT_MAX_STEPS 16u"));
	CHECK(defines.contains("#define VSM_PREFILTERED_DISTANT_LEVELS 2u"));
	CHECK(defines.contains("#define VSM_PREFILTERED_DISTANT_TAPS 4u"));
}

TEST_CASE("[VSM] The projection reads the level and mip of the marking pass's fine request (#222)") {
	using VSM = RendererVirtualShadowMapStorage;
	RandomPCG rng(222);
	const Basis sun(Vector3(0.3f, 0.9f, -0.2f).normalized(), 0.7f);
	const uint32_t count = 7; // Level 6 reaches 128 m from the camera.
	const Vector3 camera(13.0f, 2.0f, -41.0f);
	const Vector2 center(float(sun.get_column(0).normalized().dot(camera)), float(sun.get_column(1).normalized().dot(camera)));
	LocalVector<VSM::MapData> maps;
	for (uint32_t level = 0; level < count; level++) {
		maps.push_back(VSM::map_data_clipmap(sun, level, VSM::clipmap_level_origin(center, level), 10000.0f));
	}
	uint32_t map_bias[count];
	for (uint32_t level = 0; level < count; level++) {
		map_bias[level] = level % 3; // Overflow bias 0 to 2 mips.
	}
	const uint32_t flags = VSM::light_flags(VSM::MAP_KIND_DIRECTIONAL, count);
	uint32_t compared = 0;
	uint32_t outside = 0;
	for (uint32_t i = 0; i < 20000; i++) {
		const Vector3 world = camera + Vector3(rng.random(-60.0f, 60.0f), rng.random(-20.0f, 20.0f), rng.random(-60.0f, 60.0f));
		const float footprint = std::ldexp(1.0f, int32_t(rng.rand() % 16) - 12) * rng.random(1.0f, 2.0f);
		const int32_t lod_bias = int32_t(rng.rand() % 3) - 1;
		// A moving light's extra bias (#221), held by level 0: the projection adds it as the marking pass does.
		maps[0].lod_bias = int32_t(i % 2);
		LocalVector<uint64_t> requests;
		VSM::mark_directional_point(maps.ptr(), count, map_bias, 0, flags, world, footprint, lod_bias, false, requests);
		uint32_t level = 0;
		uint32_t mip = 0;
		const Vector2i texel0 = VSM::clipmap_texel0(maps[0], world);
		const bool selected = VSM::clipmap_projection_select(maps.ptr(), count, map_bias, 0, count, texel0, footprint, lod_bias, level, mip);
		CHECK(selected == !requests.is_empty());
		if (!selected || requests.is_empty()) {
			outside++;
			continue;
		}
		// The first request is the fine one; decode its map, mip and page.
		const uint32_t index = uint32_t(requests[0] & 0xFFFFFFFFu);
		const uint32_t map = index / VSM::ENTRIES_PER_MAP;
		uint32_t request_mip = 0;
		while (request_mip + 1 < VSM::MIP_COUNT && VSM::mip_entry_offset(request_mip + 1) <= index % VSM::ENTRIES_PER_MAP) {
			request_mip++;
		}
		const Vector2i page = VSM::clipmap_page(VSM::clipmap_level_texel(texel0, level), mip);
		CHECK(map == level);
		CHECK(request_mip == mip);
		CHECK(index == VSM::entry_index(level, mip, uint32_t(page.x), uint32_t(page.y)));
		compared++;
	}
	// Every point lies within the last level.
	CHECK(compared == 20000);
	CHECK(outside == 0);
}

TEST_CASE("[VSM] Directional projection: shadow where the caster is, lit elsewhere and where no page is mapped (#222)") {
	using namespace TestVirtualShadowMapProjection;
	ProjectionScene scene;
	const uint32_t count = 4;
	scene.set_maps(sun_levels(count));
	scene.allocate(0, 3);
	scene.fill();
	VSM::ProjectionInput in = scene.input();
	in.footprint = std::ldexp(1.0f, -7); // Level 0, mip 3: 7.8 mm texels.
	in.rays = 0;
	const uint32_t flags = VSM::light_flags(VSM::MAP_KIND_DIRECTIONAL, count);
	uint32_t level = 99;
	uint32_t mip = 99;
	REQUIRE(VSM::clipmap_projection_select(scene.maps.ptr(), count, nullptr, 0, count, VSM::clipmap_texel0(scene.maps[0], Vector3()), in.footprint, 0, level, mip));
	CHECK(level == 0);
	CHECK(mip == 3);

	const Vector3 up(0, 1, 0);
	CHECK(VSM::shadow_directional(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), up, 0.0f) == doctest::Approx(0.0f));
	CHECK(VSM::shadow_directional(in, 0, flags, Vector3(0.4f, 0.0f, -0.3f), up, 0.0f) == doctest::Approx(0.0f));
	CHECK(VSM::shadow_directional(in, 0, flags, Vector3(0.6f, 0.0f, 0.0f), up, 0.0f) == doctest::Approx(1.0f));
	CHECK(VSM::shadow_directional(in, 0, flags, Vector3(1.5f, 0.0f, 1.2f), up, 0.0f) == doctest::Approx(1.0f));
	// The slab's own top is lit (the depth and normal biases keep it from shadowing itself).
	CHECK(VSM::shadow_directional(in, 0, flags, Vector3(0.1f, 1.0f, 0.1f), up, 0.0f) == doctest::Approx(1.0f));
	// The LIGHT_FLAGS_SAMPLE bit does not change what is read.
	CHECK(VSM::shadow_directional(in, 0, flags | VSM::LIGHT_FLAGS_SAMPLE, Vector3(0.0f, 0.0f, 0.0f), up, 0.0f) == doctest::Approx(0.0f));
	// Rays without a light size take the hard tap too.
	in.rays = 8;
	in.steps = 6;
	CHECK(VSM::shadow_directional(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), up, 0.0f) == doctest::Approx(0.0f));
	in.rays = 0;

	// Mip 3 unmapped: the receiver falls back to mip 4 and stays shadowed.
	for (uint32_t y = 0; y < (VSM::PAGES_PER_SIDE >> 3); y++) {
		for (uint32_t x = 0; x < (VSM::PAGES_PER_SIDE >> 3); x++) {
			scene.table[VSM::entry_index(0, 3, x, y)] = 0;
		}
	}
	CHECK(VSM::shadow_directional(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), up, 0.0f) == doctest::Approx(0.0f));
	CHECK(VSM::shadow_directional(in, 0, flags, Vector3(1.5f, 0.0f, 1.2f), up, 0.0f) == doctest::Approx(1.0f));
	// No page at any mip or level: lit.
	for (uint32_t &entry : scene.table) {
		entry = 0;
	}
	CHECK(VSM::shadow_directional(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), up, 0.0f) == doctest::Approx(1.0f));
	// A light without maps is lit.
	CHECK(VSM::shadow_directional(in, VSM::MAP_NONE, VSM::light_flags(VSM::MAP_KIND_DIRECTIONAL, 0), Vector3(), up, 0.0f) == doctest::Approx(1.0f));
}

TEST_CASE("[VSM] SMRT: the penumbra widens with the sun's angular size and with the caster's height (#222)") {
	using namespace TestVirtualShadowMapProjection;
	auto width_for = [](float p_slab_height, float p_diameter_degrees) {
		const uint32_t count = 4;
		ProjectionScene scene;
		scene.slab_height = p_slab_height;
		scene.set_maps(sun_levels(count));
		scene.allocate(0, 2);
		scene.fill();
		VSM::ProjectionInput in = scene.input();
		in.footprint = std::ldexp(1.0f, -8); // Level 0, mip 2: 3.9 mm texels.
		in.rays = VSM::SMRT_MAX_RAYS;
		in.steps = VSM::SMRT_MAX_STEPS;
		const float tan_half = VSM::directional_tan_half_angle(1.0f - Math::cos(Math::deg_to_rad(p_diameter_degrees)));
		// The umbra under the slab's middle and full light far from it.
		CHECK(ground_shadow_directional(in, count, 0.0f, 0.0f, tan_half) < 0.05f);
		CHECK(ground_shadow_directional(in, count, 1.5f, 0.0f, tan_half) > 0.95f);
		return penumbra_width([&](float p_x) { return ground_shadow_directional(in, count, p_x, 0.0f, tan_half); }, 0.25f, 0.9f, 0.002f);
	};
	const float narrow = width_for(1.0f, 0.5f);
	const float wide = width_for(1.0f, 4.0f);
	const float wide_low = width_for(0.5f, 4.0f);
	MESSAGE(vformat("SMRT penumbra widths: 0.5 deg %.4f m, 4 deg %.4f m, 4 deg at half the height %.4f m", narrow, wide, wide_low));
	REQUIRE(narrow >= 0.0f);
	REQUIRE(wide > 0.0f);
	REQUIRE(wide_low > 0.0f);
	// The full penumbra of a 4 degree disk 1 m above the ground is 1 m * tan(4 deg) = 7 cm.
	CHECK(wide > 2.0f * narrow);
	CHECK(wide > 0.2f * Math::tan(Math::deg_to_rad(4.0f)));
	CHECK(wide < 1.5f * Math::tan(Math::deg_to_rad(4.0f)));
	// Contact hardening: a lower caster gives a sharper shadow.
	CHECK(wide > 1.4f * wide_low);
}

TEST_CASE("[VSM] SMRT for a spot light: shadow under the caster, and a penumbra that widens with the light's radius (#222)") {
	using namespace TestVirtualShadowMapProjection;
	auto make_scene = [](ProjectionScene &r_scene) {
		const Transform3D light(Basis::looking_at(Vector3(0, -1, 0), Vector3(0, 0, -1)), Vector3(0, 3, 0));
		r_scene.light_position = light.origin;
		LocalVector<VSM::MapData> maps;
		maps.push_back(VSM::map_data_face(VSM::MAP_KIND_SPOT, 0, light, 10.0f, 45.0f));
		r_scene.set_maps(maps);
		// The face's rows put the light at x = y = w = 0 (the texel fill relies on it).
		CHECK(Math::abs(VSM::row_dot(maps[0].rows[0], light.origin)) < 1e-4f);
		CHECK(Math::abs(VSM::row_dot(maps[0].rows[1], light.origin)) < 1e-4f);
		CHECK(Math::abs(VSM::row_dot(maps[0].rows[3], light.origin)) < 1e-4f);
		r_scene.allocate(0, 2);
		r_scene.fill();
	};
	ProjectionScene scene;
	make_scene(scene);
	VSM::ProjectionInput in = scene.input();
	// Ground points lie 3 m along the face's axis; a footprint of 4.5 mip 0 texels there reads mip 2.
	in.footprint = scene.maps[0].texel_scale * 3.0f * 4.5f;
	CHECK(VSM::face_mip(scene.maps[0].texel_scale * 3.0f, in.footprint) == 2);
	in.rays = 0;
	const uint32_t flags = VSM::light_flags(VSM::MAP_KIND_SPOT, 1);
	const Vector3 up(0, 1, 0);
	// The slab's shadow edge on the ground is at 0.5 * 3 / (3 - 1) = 0.75 m.
	CHECK(VSM::shadow_local(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), up, scene.light_position, 0.0f) == doctest::Approx(0.0f));
	CHECK(VSM::shadow_local(in, 0, flags, Vector3(0.7f, 0.0f, 0.0f), up, scene.light_position, 0.0f) == doctest::Approx(0.0f));
	CHECK(VSM::shadow_local(in, 0, flags, Vector3(0.8f, 0.0f, 0.0f), up, scene.light_position, 0.0f) == doctest::Approx(1.0f));
	CHECK(VSM::shadow_local(in, 0, flags, Vector3(1.6f, 0.0f, -1.0f), up, scene.light_position, 0.0f) == doctest::Approx(1.0f));
	// Outside the spot's face: lit.
	CHECK(VSM::shadow_local(in, 0, flags, Vector3(0.0f, 5.0f, 0.0f), up, scene.light_position, 0.0f) == doctest::Approx(1.0f));

	in.rays = VSM::SMRT_MAX_RAYS;
	in.steps = VSM::SMRT_MAX_STEPS;
	CHECK(ground_shadow_spot(scene, in, 0.0f, 0.3f) < 0.05f);
	CHECK(ground_shadow_spot(scene, in, 1.6f, 0.3f) > 0.95f);
	const float narrow = penumbra_width([&](float p_x) { return ground_shadow_spot(scene, in, p_x, 0.05f); }, 0.4f, 1.4f, 0.003f);
	const float wide = penumbra_width([&](float p_x) { return ground_shadow_spot(scene, in, p_x, 0.3f); }, 0.4f, 1.4f, 0.003f);
	MESSAGE(vformat("Spot SMRT penumbra widths: radius 0.05 m %.4f m, radius 0.3 m %.4f m", narrow, wide));
	REQUIRE(narrow >= 0.0f);
	REQUIRE(wide > 0.0f);
	// The full penumbra of a 0.3 m radius light 2 m above a caster 1 m above the ground is 0.3 m.
	CHECK(wide > 2.0f * narrow);
	CHECK(wide > 0.2f * 0.3f);
	CHECK(wide < 1.5f * 0.3f);
	// An area light's rectangle softens the shadow as well.
	const float area = penumbra_width([&](float p_x) {
		float sum = 0.0f;
		for (uint32_t i = 0; i < 16; i++) {
			in.noise = Vector2((float(i % 4) + 0.5f) / 4.0f, (float(i / 4) + 0.5f) / 4.0f);
			sum += VSM::shadow_local(in, 0, flags, Vector3(p_x, 0.0f, 0.0f), up, scene.light_position, 0.0f, Vector3(0.6f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 0.6f));
		}
		return sum / 16.0f;
	},
			0.4f, 1.4f, 0.003f);
	MESSAGE(vformat("Area SMRT penumbra width (0.6 m square): %.4f m", area));
	CHECK(area > 2.0f * narrow);
}

TEST_CASE("[VSM] Prefiltered Distant: the moments filter of the last levels shadows under the caster and is lit in the open (#222)") {
	using namespace TestVirtualShadowMapProjection;
	ProjectionScene scene;
	scene.slab_x = 3.0f; // Outside level 0 (+-2 m): level 1, one of the last two of three.
	const uint32_t count = 3;
	scene.set_maps(sun_levels(count));
	scene.allocate(1, 2);
	scene.fill();
	VSM::ProjectionInput in = scene.input();
	in.footprint = std::ldexp(1.0f, -7); // Level 1, mip 2: 7.8 mm texels.
	const uint32_t flags = VSM::light_flags(VSM::MAP_KIND_DIRECTIONAL, count);
	uint32_t level = 99;
	uint32_t mip = 99;
	REQUIRE(VSM::clipmap_projection_select(scene.maps.ptr(), count, nullptr, 0, count, VSM::clipmap_texel0(scene.maps[0], Vector3(3.0f, 0.0f, 0.0f)), in.footprint, 0, level, mip));
	CHECK(level == 1);
	CHECK(mip == 2);
	CHECK(VSM::prefiltered_distant_level(level, count));
	const Vector3 up(0, 1, 0);
	const float tan_half = VSM::directional_tan_half_angle(1.0f - Math::cos(Math::deg_to_rad(1.0f)));

	// Off: the hard tap.
	in.rays = 0;
	CHECK(VSM::shadow_directional(in, 0, flags, Vector3(3.0f, 0.0f, 0.0f), up, tan_half) == doctest::Approx(0.0f));
	CHECK(VSM::shadow_directional(in, 0, flags, Vector3(3.0f, 0.0f, 1.5f), up, tan_half) == doctest::Approx(1.0f));
	// On: the moments filter, whatever the ray count.
	in.prefiltered_distant = true;
	in.rays = VSM::SMRT_MAX_RAYS;
	in.steps = VSM::SMRT_MAX_STEPS;
	CHECK(VSM::shadow_directional(in, 0, flags, Vector3(3.0f, 0.0f, 0.0f), up, tan_half) < 0.05f);
	CHECK(VSM::shadow_directional(in, 0, flags, Vector3(3.0f, 0.0f, 1.5f), up, tan_half) > 0.95f);
	CHECK(VSM::shadow_directional(in, 0, flags, Vector3(3.2f, 1.0f, 0.1f), up, tan_half) > 0.95f); // The slab's top.
	// Across the edge the filter is soft: some samples lie strictly between shadow and light, rising.
	uint32_t partial = 0;
	float previous = -1.0f;
	bool rising = true;
	for (float x = 3.3f; x <= 3.7f; x += 0.002f) {
		const float shadow = VSM::shadow_directional(in, 0, flags, Vector3(x, 0.0f, 0.0f), up, tan_half);
		partial += (shadow > 0.05f && shadow < 0.95f) ? 1u : 0u;
		rising = rising && shadow >= previous - 1e-4f;
		previous = shadow;
	}
	CHECK(partial > 0);
	CHECK(rising);
	// The CPU model of the filter is what shadow_directional() returns.
	const Vector2 t(VSM::row_dot(scene.maps[0].rows[0], Vector3(3.0f, 0.0f, 0.0f)), VSM::row_dot(scene.maps[0].rows[1], Vector3(3.0f, 0.0f, 0.0f)));
	CHECK(VSM::prefiltered_distant(in, 0, count, t, 0.0f, 1, 2) < 0.05f);
	// Large world-space depths (far from origin): centering around p_depth avoids float32 variance cancellation.
	{
		ProjectionScene far_scene;
		far_scene.slab_x = 3.0f;
		far_scene.set_maps(sun_levels(count));
		far_scene.allocate(1, 2);
		far_scene.fill();
		const uint64_t layer_size = uint64_t(far_scene.pool_width) * far_scene.pool_height;
		const float base_depth = 10000.0f;
		for (uint64_t i = 0; i < layer_size; i++) {
			far_scene.pool[i] = VSM::depth_pack(base_depth);
		}
		VSM::ProjectionInput far_in = far_scene.input();
		// A flat receiver 0.5 m behind the stored plane at depth ~10 km should stay solidly shadowed, matching near origin.
		const float far_shadow = VSM::prefiltered_distant(far_in, 0, count, t, base_depth + 0.5f, 1, 2);
		for (uint64_t i = 0; i < layer_size; i++) {
			far_scene.pool[i] = VSM::depth_pack(10.0f);
		}
		const float near_shadow = VSM::prefiltered_distant(far_in, 0, count, t, 10.5f, 1, 2);
		CHECK(far_shadow < 0.05f);
		CHECK(far_shadow == doctest::Approx(near_shadow).epsilon(1e-4f));
	}
	// A receiver in a finer level of a light with more levels than that takes SMRT, not the filter.
	ProjectionScene near_scene;
	near_scene.set_maps(sun_levels(count));
	near_scene.allocate(0, 2);
	near_scene.fill();
	VSM::ProjectionInput near_in = near_scene.input();
	near_in.footprint = std::ldexp(1.0f, -8);
	near_in.prefiltered_distant = true;
	near_in.rays = 0;
	CHECK(VSM::shadow_directional(near_in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), up, tan_half) == doctest::Approx(0.0f));
	CHECK(VSM::shadow_directional(near_in, 0, flags, Vector3(0.52f, 0.0f, 0.0f), up, tan_half) == doctest::Approx(1.0f));
}

TEST_CASE("[VSM] Omni projection picks the face that holds the point (#222)") {
	using VSM = RendererVirtualShadowMapStorage;
	const Transform3D light(Basis(), Vector3(1.0f, 2.0f, 3.0f));
	LocalVector<VSM::MapData> maps;
	for (uint32_t face = 0; face < 6; face++) {
		maps.push_back(VSM::map_data_face(VSM::MAP_KIND_OMNI, face, light, 10.0f, 0.0f));
	}
	const uint32_t flags = VSM::light_flags(VSM::MAP_KIND_OMNI, 6);
	// Faces 0-5 look along the light's +X, -X, +Y, -Y, +Z, -Z.
	const Vector3 directions[6] = { Vector3(1, 0.1f, 0.2f), Vector3(-1, -0.2f, 0.1f), Vector3(0.1f, 1, -0.2f), Vector3(-0.2f, -1, 0.1f), Vector3(0.2f, 0.1f, 1), Vector3(0.1f, -0.2f, -1) };
	for (uint32_t face = 0; face < 6; face++) {
		float w = 0.0f;
		CHECK(VSM::face_projection_select(maps.ptr(), 6, 0, flags, light.origin + directions[face] * 2.0f, w) == face);
		CHECK(w > 0.0f);
	}
	// Past the table's map count: nothing.
	float w = 0.0f;
	CHECK(VSM::face_projection_select(maps.ptr(), 3, 0, flags, light.origin + directions[5] * 2.0f, w) == VSM::MAP_NONE);
}

// GodotNext: vsm_nanite (#217, plan §21.3 contract S4). CPU reference tests for VSM x Nanite:
// layout/setting contracts, page-mip projected error matching P5's texel rule, world/local AABB
// per-page culling, 1-level 8x8 per-page max-depth Hi-Z build & occlusion, SW/HW triangle-size
// routing, perspective-correct 1/w software depth raster, static/dynamic layer hook, and shared
// RASTER_BIN_TABLE parity with #155.

namespace TestVSMNanite {

using VSM = RendererVirtualShadowMapStorage;
using VMS = RendererVirtualMeshStorage;

void fill_xform_rows(const Transform3D &p_xform, float r_rows[3][4]) {
	for (int r = 0; r < 3; r++) {
		r_rows[r][0] = float(p_xform.basis.rows[r][0]);
		r_rows[r][1] = float(p_xform.basis.rows[r][1]);
		r_rows[r][2] = float(p_xform.basis.rows[r][2]);
		r_rows[r][3] = float(p_xform.origin[r]);
	}
}

} // namespace TestVSMNanite

TEST_CASE("[VSM][NextGeo] Layout, capacities, provisional settings and shared RASTER_BIN_TABLE parity (#217)") {
	using namespace RendererRD;
	using namespace TestVSMNanite;

	CHECK_EQ(sizeof(NextGeoVSMMapMeta), size_t(16));
	CHECK_EQ(sizeof(NextGeoVSMParams), size_t(352));
	CHECK_EQ(sizeof(NextGeoVSMCounters), size_t(128));
	CHECK_EQ(NEXTGEO_VSM_HIZ_PAGE_SIZE, 8u);
	CHECK_EQ(NEXTGEO_VSM_HIZ_BLOCK_SIZE, 8u);
	CHECK(NEXTGEO_VSM_CLUSTER_CAPACITY <= (1u << 21));
	CHECK_EQ(NEXTGEO_VSM_COUNTER_CLUSTER_COUNT, 11u);
	CHECK_EQ(NEXTGEO_VSM_COUNTER_SW_CLUSTERS, 12u);
	CHECK_EQ(NEXTGEO_VSM_COUNTER_SW_WRITES, 20u);
	CHECK_EQ(NEXTGEO_VSM_COUNTER_SINK, 21u);
	CHECK_EQ(NEXTGEO_VSM_COUNTER_HIZ_PAGES_BUILT, 22u);

	// Provisional SW/HW switch threshold clamping.
	CHECK_EQ(nextgeo_vsm_sw_switch_texels(0.0f), 0.0f);
	CHECK_EQ(nextgeo_vsm_sw_switch_texels(-5.0f), 0.0f);
	CHECK_EQ(nextgeo_vsm_sw_switch_texels(NAN), 0.0f);
	CHECK_EQ(nextgeo_vsm_sw_switch_texels(INFINITY), 0.0f);
	CHECK_EQ(nextgeo_vsm_sw_switch_texels(NEXTGEO_VSM_DEFAULT_SW_MAX_EDGE_TEXELS), doctest::Approx(8.0f));
	CHECK_EQ(nextgeo_vsm_sw_switch_texels(128.0f), doctest::Approx(float(NEXTGEO_SW_FULL_PRECISION_EXTENT_PX)));

	// Project setting registered with provisional default 8.0.
	const ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);
	CHECK(ps->has_setting("rendering/lights_and_shadows/virtual_shadows/nanite_sw_raster_max_edge_texels"));
	CHECK(double(ps->get_setting("rendering/lights_and_shadows/virtual_shadows/nanite_sw_raster_max_edge_texels")) == doctest::Approx(double(NEXTGEO_VSM_DEFAULT_SW_MAX_EDGE_TEXELS)));

	// Step 7: every visibility entry in RASTER_BIN_TABLE has a matching VSM entry with identical bin kind.
	uint32_t vsm_entries = 0;
	for (uint32_t i = 0; i < VMS::RASTER_BIN_TABLE_SIZE; i++) {
		const VMS::RasterBinEntry &entry = VMS::RASTER_BIN_TABLE[i];
		if (entry.pass == VMS::RASTER_PASS_VSM) {
			vsm_entries++;
		}
		const VMS::RasterPass other = entry.pass == VMS::RASTER_PASS_VISIBILITY ? VMS::RASTER_PASS_VSM : VMS::RASTER_PASS_VISIBILITY;
		const int32_t twin = VMS::find_raster_bin_entry(entry.features, other);
		REQUIRE(twin >= 0);
		CHECK_EQ(uint32_t(VMS::RASTER_BIN_TABLE[twin].bin), uint32_t(entry.bin));
	}
	CHECK(vsm_entries > 0u);
	CHECK_EQ(VMS::resolve_raster_bin(0u, VMS::RASTER_PASS_VSM), int32_t(VMS::RASTER_BIN_FIXED));
	CHECK_EQ(VMS::resolve_raster_bin(VMS::RASTER_FEATURE_MASKED, VMS::RASTER_PASS_VSM), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));
	CHECK_EQ(VMS::resolve_raster_bin(VMS::RASTER_FEATURE_VERTEX_OFFSET, VMS::RASTER_PASS_VSM), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));
	CHECK_EQ(VMS::resolve_raster_bin(VMS::RASTER_FEATURE_MASKED | VMS::RASTER_FEATURE_VERTEX_OFFSET, VMS::RASTER_PASS_VSM), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));
}

TEST_CASE("[VSM][NextGeo] Page-mip projected error matches P5 at mip 0 and scales by 2^mip (#217)") {
	using namespace RendererRD;
	using namespace TestVSMNanite;

	const Basis sun = Basis::from_euler(Vector3(Math::deg_to_rad(-55.0f), Math::deg_to_rad(25.0f), 0.0f));
	const VSM::MapData clip0 = VSM::map_data_clipmap(sun, 0, Vector2i(-17, 29), 100.0f);
	const VSM::MapData clip2 = VSM::map_data_clipmap(sun, 2, Vector2i(5, -9), 100.0f);

	Transform3D xf(Basis::from_euler(Vector3(0.2f, -0.4f, 0.1f)).scaled(Vector3(2.0f, 2.0f, 2.0f)), Vector3(1.5f, 0.5f, -2.0f));
	float rows[3][4];
	fill_xform_rows(xf, rows);
	const float lod_sphere[4] = { 0.1f, 0.2f, -0.1f, 0.5f };
	const float err_m = 0.004f;

	// Directional: matches P5 ortho error (error * scale / texel_size) at mip 0 and halves per mip.
	const float dir_mip0 = nextgeo_vsm_page_projected_error(rows, lod_sphere, err_m, clip0, 0u, 1.0f, 1.0f);
	CHECK(dir_mip0 == doctest::Approx(err_m * 2.0f / clip0.texel_scale).epsilon(1e-5f));
	for (uint32_t mip = 1; mip < VSM::MIP_COUNT; mip++) {
		const float dir_m = nextgeo_vsm_page_projected_error(rows, lod_sphere, err_m, clip0, mip, 1.0f, 1.0f);
		CHECK(dir_m == doctest::Approx(dir_mip0 / float(1u << mip)).epsilon(1e-5f));
	}
	// Clipmap level 2 has 4x coarser texels than level 0, matching mip 2 of level 0.
	CHECK(nextgeo_vsm_page_projected_error(rows, lod_sphere, err_m, clip2, 0u, 1.0f, 1.0f) == doctest::Approx(dir_mip0 * 0.25f).epsilon(1e-5f));
	// Infinite error always refines.
	CHECK(nextgeo_vsm_page_projected_error(rows, lod_sphere, 3.4e38f, clip0, 0u, 1.0f, 1.0f) > 1.0f);
	// Mirrored non-uniformly scaled transforms must use the largest axis scale (4.0), never Basis::get_scale()'s max(-4,-1,-1) = -1.
	{
		// Right-multiply (scaled_local) keeps orthogonal columns of lengths (4, 1, 1) under rotation.
		const Transform3D mirrored_xf(Basis::from_euler(Vector3(0.3f, -0.5f, 0.2f)).scaled_local(Vector3(-4.0f, 1.0f, 1.0f)), Vector3(1.5f, 0.5f, -2.0f));
		float mirrored_rows[3][4];
		fill_xform_rows(mirrored_xf, mirrored_rows);
		CHECK(nextgeo_vsm_instance_scale(mirrored_rows) == doctest::Approx(4.0f).epsilon(1e-4f));
		const float mirrored_dir = nextgeo_vsm_page_projected_error(mirrored_rows, lod_sphere, err_m, clip0, 0u, 1.0f, 1.0f);
		CHECK(mirrored_dir == doctest::Approx(err_m * 4.0f / clip0.texel_scale).epsilon(1e-4f));

		// Left-multiply (sheared mirrored transform): Gershgorin bound stays conservatively >= max singular value (4.0).
		const Transform3D sheared_xf(Basis::from_euler(Vector3(0.3f, -0.5f, 0.2f)).scaled(Vector3(-4.0f, 1.0f, 1.0f)), Vector3(1.5f, 0.5f, -2.0f));
		float sheared_rows[3][4];
		fill_xform_rows(sheared_xf, sheared_rows);
		CHECK(nextgeo_vsm_instance_scale(sheared_rows) >= 4.0f - 1e-4f);

		const Transform3D mirrored_aniso(Basis::from_scale(Vector3(-4.0f, 2.0f, 0.5f)), Vector3(0.0f, 0.0f, 0.0f));
		float aniso_rows[3][4];
		fill_xform_rows(mirrored_aniso, aniso_rows);
		CHECK(nextgeo_vsm_instance_scale(aniso_rows) == doctest::Approx(4.0f).epsilon(1e-4f));
		CHECK(nextgeo_vsm_page_projected_error(aniso_rows, lod_sphere, err_m, clip0, 0u, 1.0f, 1.0f) == doctest::Approx(err_m * 4.0f / clip0.texel_scale).epsilon(1e-4f));
	}

	// Local perspective faces: light origin recovery is exact for spot, all 6 omni faces and 5 area faces.
	const Transform3D light_xf(Basis::from_euler(Vector3(-0.7f, 0.5f, -0.3f)), Vector3(-3.2f, 4.7f, 2.1f));
	for (uint32_t face = 0; face < 6; face++) {
		const VSM::MapData omni = VSM::map_data_face(VSM::MAP_KIND_OMNI, face, light_xf, 20.0f, 0.0f);
		const Vector3 recovered = nextgeo_vsm_light_origin(omni);
		CHECK(recovered.distance_to(light_xf.origin) < 1e-4f);
	}
	const VSM::MapData spot = VSM::map_data_face(VSM::MAP_KIND_SPOT, 0, light_xf, 20.0f, 35.0f);
	CHECK(nextgeo_vsm_light_origin(spot).distance_to(light_xf.origin) < 1e-4f);

	// Perspective projected error at mip 0 matches P5's exact formula:
	// error * scale * 0.5 * VIRTUAL_SIZE * cot(spot_angle) / max(dist - radius, near_w).
	const Vector3 center_w = nextgeo_vsm_xform_point(rows, Vector3(lod_sphere[0], lod_sphere[1], lod_sphere[2]));
	const float dist_eff = MAX(float(center_w.distance_to(light_xf.origin)) - lod_sphere[3] * 2.0f, spot.near_w);
	const float p5_mip0 = err_m * 2.0f * 0.5f * float(VSM::VIRTUAL_SIZE) / (float(Math::tan(Math::deg_to_rad(35.0))) * dist_eff);
	const float vsm_mip0 = nextgeo_vsm_page_projected_error(rows, lod_sphere, err_m, spot, 0u, 1.0f, 1.0f);
	CHECK(vsm_mip0 == doctest::Approx(p5_mip0).epsilon(1e-4f));
	for (uint32_t mip = 1; mip < VSM::MIP_COUNT; mip++) {
		CHECK(nextgeo_vsm_page_projected_error(rows, lod_sphere, err_m, spot, mip, 1.0f, 1.0f) == doctest::Approx(vsm_mip0 / float(1u << mip)).epsilon(1e-4f));
	}
	// Bounding sphere enclosing the light clamps effective distance to near_w (finite, never negative).
	float unit_rows[3][4];
	fill_xform_rows(Transform3D(Basis(), Vector3(1.5f, 0.5f, -2.0f)), unit_rows);
	const float huge_sphere[4] = { 0.0f, 0.0f, 0.0f, 100.0f };
	const float enclosed_err = nextgeo_vsm_page_projected_error(unit_rows, huge_sphere, err_m, spot, 0u, 1.0f, 1.0f);
	CHECK(enclosed_err == doctest::Approx(err_m / (spot.texel_scale * spot.near_w)).epsilon(1e-4f));
}

TEST_CASE("[VSM][NextGeo] Per-page AABB overlap culling across directional wrap and local faces (#217)") {
	using namespace RendererRD;
	using namespace TestVSMNanite;

	// Directional clipmap with negative non-power-of-two toroidal origin.
	const Basis sun = Basis::from_euler(Vector3(Math::deg_to_rad(-90.0f), 0.0f, 0.0f));
	const Vector3 cam(1.3f, 2.0f, -0.7f);
	const Vector2 cam_light(sun.get_column(0).normalized().dot(cam), sun.get_column(1).normalized().dot(cam));
	const VSM::MapData clip = VSM::map_data_clipmap(sun, 0, VSM::clipmap_level_origin(cam_light, 0), 100.0f);

	const Vector3 box_center(1.25f, 0.5f, -0.75f);
	const Vector2i tex0 = VSM::clipmap_texel0(clip, box_center);
	const Vector2i page0 = VSM::clipmap_page(tex0, 0);
	Vector2 px_min;
	Vector2 px_max;
	float min_depth = 0.0f;
	bool straddles_near = false;
	CHECK(nextgeo_vsm_aabb_page_bounds(box_center - Vector3(0.01f, 0.1f, 0.01f), box_center + Vector3(0.01f, 0.1f, 0.01f), clip, 0u, page0, px_min, px_max, min_depth, straddles_near));
	CHECK_FALSE(straddles_near);
	CHECK(px_max.x >= 0.0f);
	CHECK(px_min.x <= float(VSM::PAGE_SIZE));
	CHECK(px_max.y >= 0.0f);
	CHECK(px_min.y <= float(VSM::PAGE_SIZE));
	CHECK(min_depth == doctest::Approx(VSM::row_dot(clip.rows[2], box_center + Vector3(0.0f, 0.1f, 0.0f))).epsilon(1e-4f));

	// A distant toroidal slot on the opposite side of the clipmap does not overlap this small box.
	const Vector2i far_slot((page0.x + 32) & 63, (page0.y + 32) & 63);
	CHECK_FALSE(nextgeo_vsm_aabb_page_bounds(box_center - Vector3(0.01f, 0.1f, 0.01f), box_center + Vector3(0.01f, 0.1f, 0.01f), clip, 0u, far_slot, px_min, px_max, min_depth, straddles_near));

	// Local spot face: box at axis center overlaps center page, rejects far corner, behind-light, and past-range.
	const Transform3D spot_xf(Basis::from_euler(Vector3(Math::deg_to_rad(-90.0f), 0.0f, 0.0f)), Vector3(0.0f, 6.0f, 0.0f));
	const VSM::MapData spot = VSM::map_data_face(VSM::MAP_KIND_SPOT, 0, spot_xf, 12.0f, 30.0f);
	const Vector2i root_slot(0, 0);
	CHECK(nextgeo_vsm_aabb_page_bounds(Vector3(-0.2f, 1.0f, -0.2f), Vector3(0.2f, 2.0f, 0.2f), spot, VSM::MIP_COUNT - 1, root_slot, px_min, px_max, min_depth, straddles_near));
	CHECK_FALSE(straddles_near);
	CHECK(min_depth == doctest::Approx(4.0f).epsilon(1e-4f)); // 6.0 - 2.0 = 4.0 m from spot.
	CHECK(px_min.x > 16.0f);
	CHECK(px_max.x < 48.0f);

	// Behind the light: rejected.
	CHECK_FALSE(nextgeo_vsm_aabb_page_bounds(Vector3(-0.5f, 7.0f, -0.5f), Vector3(0.5f, 8.0f, 0.5f), spot, VSM::MIP_COUNT - 1, root_slot, px_min, px_max, min_depth, straddles_near));
	// Past max_distance (12 m below y = 6 -> y < -6): rejected.
	CHECK_FALSE(nextgeo_vsm_aabb_page_bounds(Vector3(-0.5f, -9.0f, -0.5f), Vector3(0.5f, -7.0f, 0.5f), spot, VSM::MIP_COUNT - 1, root_slot, px_min, px_max, min_depth, straddles_near));
	// Outside the 30-degree cone: rejected.
	CHECK_FALSE(nextgeo_vsm_aabb_page_bounds(Vector3(8.0f, 1.0f, -0.5f), Vector3(9.0f, 2.0f, 0.5f), spot, VSM::MIP_COUNT - 1, root_slot, px_min, px_max, min_depth, straddles_near));
	// Straddling near plane: conservatively accepted with full page rect and straddles_near = true.
	CHECK(nextgeo_vsm_aabb_page_bounds(Vector3(-0.3f, 5.5f, -0.3f), Vector3(0.3f, 6.5f, 0.3f), spot, 0u, Vector2i(32, 32), px_min, px_max, min_depth, straddles_near));
	CHECK(straddles_near);
	CHECK(px_min == Vector2(0.0f, 0.0f));
	CHECK(px_max == Vector2(float(VSM::PAGE_SIZE), float(VSM::PAGE_SIZE)));

	// Local rotated AABB helper produces identical overlap decision.
	Transform3D box_xf(Basis::from_euler(Vector3(0.0f, Math::deg_to_rad(45.0f), 0.0f)), Vector3(0.0f, 1.5f, 0.0f));
	float box_rows[3][4];
	fill_xform_rows(box_xf, box_rows);
	CHECK(nextgeo_vsm_local_aabb_page_bounds(box_rows, Vector3(-0.2f, -0.5f, -0.2f), Vector3(0.2f, 0.5f, 0.2f), spot, VSM::MIP_COUNT - 1, root_slot, px_min, px_max, min_depth, straddles_near));
	CHECK_FALSE(straddles_near);
	CHECK(min_depth == doctest::Approx(4.0f).epsilon(1e-4f));
}

TEST_CASE("[VSM][NextGeo] Per-page 1-level 8x8 max-depth Hi-Z build and conservative occlusion (#217)") {
	using namespace RendererRD;
	using namespace TestVSMNanite;

	LocalVector<uint32_t> static_page;
	static_page.resize(VSM::PAGE_SIZE * VSM::PAGE_SIZE);
	const uint32_t d_wall = VSM::depth_pack(5.0f);
	const uint32_t d_near = VSM::depth_pack(3.0f);
	for (uint32_t i = 0; i < static_page.size(); i++) {
		static_page[i] = d_wall;
	}
	// Make cell (1, 1) mixed (3 m and 5 m -> max depth is still 5 m = min packed uint32).
	static_page[8 * VSM::PAGE_SIZE + 8] = d_near;
	// Make cell (6, 6) contain one empty texel (0u -> cell must become 0u and never occlude).
	static_page[50 * VSM::PAGE_SIZE + 50] = 0u;

	uint32_t hiz[NEXTGEO_VSM_HIZ_PAGE_SIZE * NEXTGEO_VSM_HIZ_PAGE_SIZE] = {};
	nextgeo_vsm_hiz_build_page(static_page.ptr(), hiz);
	CHECK_EQ(hiz[0], d_wall);
	CHECK_EQ(hiz[1 * 8 + 1], d_wall); // Farthest depth wins over nearer 3 m texel.
	CHECK_EQ(hiz[6 * 8 + 6], 0u); // Empty texel disables occlusion for that 8x8 block.

	// Cached page with dynamic-only dirty bit and matching owner:
	// Behind wall (6 m > 5 m) in solid cells -> occluded.
	CHECK(nextgeo_vsm_hiz_occluded(hiz, VSM::ENTRY_DIRTY_DYNAMIC, true, Vector2(2.0f, 2.0f), Vector2(20.0f, 20.0f), 6.0f, false));
	// In front of wall (4 m < 5 m) -> visible.
	CHECK_FALSE(nextgeo_vsm_hiz_occluded(hiz, VSM::ENTRY_DIRTY_DYNAMIC, true, Vector2(2.0f, 2.0f), Vector2(20.0f, 20.0f), 4.0f, false));
	// Exact equal depth (5 m) -> conservative visible (strict <).
	CHECK_FALSE(nextgeo_vsm_hiz_occluded(hiz, VSM::ENTRY_DIRTY_DYNAMIC, true, Vector2(2.0f, 2.0f), Vector2(20.0f, 20.0f), 5.0f, false));
	// Bounds overlapping the partial-empty cell (6, 6) -> visible even at 10 m.
	CHECK_FALSE(nextgeo_vsm_hiz_occluded(hiz, VSM::ENTRY_DIRTY_DYNAMIC, true, Vector2(40.0f, 40.0f), Vector2(56.0f, 56.0f), 10.0f, false));

	// Uncached, scrolled, static-dirty, unowned, or near-straddling pages ALWAYS skip Hi-Z.
	CHECK_FALSE(nextgeo_vsm_hiz_occluded(hiz, VSM::ENTRY_UNCACHED | VSM::ENTRY_DIRTY_DYNAMIC, true, Vector2(2.0f, 2.0f), Vector2(20.0f, 20.0f), 10.0f, false));
	CHECK_FALSE(nextgeo_vsm_hiz_occluded(hiz, VSM::ENTRY_SCROLLED | VSM::ENTRY_DIRTY_DYNAMIC, true, Vector2(2.0f, 2.0f), Vector2(20.0f, 20.0f), 10.0f, false));
	CHECK_FALSE(nextgeo_vsm_hiz_occluded(hiz, VSM::ENTRY_DIRTY_STATIC | VSM::ENTRY_DIRTY_DYNAMIC, true, Vector2(2.0f, 2.0f), Vector2(20.0f, 20.0f), 10.0f, false));
	CHECK_FALSE(nextgeo_vsm_hiz_occluded(hiz, VSM::ENTRY_DIRTY_DYNAMIC, false, Vector2(2.0f, 2.0f), Vector2(20.0f, 20.0f), 10.0f, false));
	CHECK_FALSE(nextgeo_vsm_hiz_occluded(hiz, VSM::ENTRY_DIRTY_DYNAMIC, true, Vector2(2.0f, 2.0f), Vector2(20.0f, 20.0f), 10.0f, true));
}

TEST_CASE("[VSM][NextGeo] SW/HW triangle-size routing and perspective-correct SW depth raster (#217)") {
	using namespace RendererRD;
	using namespace TestVSMNanite;

	const Basis sun = Basis::from_euler(Vector3(Math::deg_to_rad(-90.0f), 0.0f, 0.0f));
	const VSM::MapData clip0 = VSM::map_data_clipmap(sun, 0, Vector2i(-32, -32), 100.0f);
	const Transform3D spot_xf(Basis::from_euler(Vector3(Math::deg_to_rad(-90.0f), 0.0f, 0.0f)), Vector3(0.0f, 6.0f, 0.0f));
	const VSM::MapData spot = VSM::map_data_face(VSM::MAP_KIND_SPOT, 0, spot_xf, 16.0f, 35.0f);

	float id_rows[3][4];
	fill_xform_rows(Transform3D(), id_rows);
	const float bounds_spot[4] = { 0.0f, 2.0f, 0.0f, 0.25f }; // w_center = 4 m, w_min = 3.75 m.

	// Directional: level 0 texel = 2^-10 (~0.976 mm). Edge 0.004 m -> ~4.1 texels (< 8 -> SW); 0.02 m -> ~20.5 texels (HW at mip 0, SW at mip 2).
	CHECK(nextgeo_vsm_sw_route(id_rows, 1.0f, bounds_spot, 0.004f, clip0, 0u, 8.0f));
	CHECK_FALSE(nextgeo_vsm_sw_route(id_rows, 1.0f, bounds_spot, 0.02f, clip0, 0u, 8.0f));
	CHECK(nextgeo_vsm_sw_route(id_rows, 1.0f, bounds_spot, 0.02f, clip0, 2u, 8.0f));
	CHECK_FALSE(nextgeo_vsm_sw_route(id_rows, 1.0f, bounds_spot, 0.004f, clip0, 0u, 0.0f)); // Threshold 0 forces HW.

	// Local perspective: small edge at w_min = 3.75 m routes SW; sphere touching near plane forces HW.
	CHECK(nextgeo_vsm_sw_route(id_rows, 1.0f, bounds_spot, 0.005f, spot, 0u, 8.0f));
	CHECK_FALSE(nextgeo_vsm_sw_route(id_rows, 1.0f, bounds_spot, 0.1f, spot, 0u, 8.0f));
	const float near_bounds[4] = { 0.0f, 5.9f, 0.0f, 0.25f }; // w_min < near_w -> must stay HW.
	CHECK_FALSE(nextgeo_vsm_sw_route(id_rows, 1.0f, near_bounds, 0.001f, spot, 0u, 64.0f));

	// Directional SW raster: tilted plane z = a*x + b*z + c matches analytic depth at every covered pixel.
	const Vector3 dv0(-0.02f, 1.0f, -0.02f);
	const Vector3 dv1(0.03f, 2.5f, -0.01f);
	const Vector3 dv2(-0.01f, 0.4f, 0.03f);
	const Vector2i dslot = VSM::clipmap_page(VSM::clipmap_texel0(clip0, Vector3(0.0f, 1.0f, 0.0f)), 0);
	const Vector4 dp0 = nextgeo_vsm_sw_project_vertex(clip0, 0u, dslot, dv0);
	const Vector4 dp1 = nextgeo_vsm_sw_project_vertex(clip0, 0u, dslot, dv1);
	const Vector4 dp2 = nextgeo_vsm_sw_project_vertex(clip0, 0u, dslot, dv2);
	const float dpx[3][2] = { { float(dp0.x), float(dp0.y) }, { float(dp1.x), float(dp1.y) }, { float(dp2.x), float(dp2.y) } };
	const float dz[3] = { float(dp0.z), float(dp1.z), float(dp2.z) };
	const NextGeoSwTriangle dtri = nextgeo_sw_setup_triangle(dpx, dz, int32_t(VSM::PAGE_SIZE), int32_t(VSM::PAGE_SIZE), NEXTGEO_SW_KEEP_BOTH);
	REQUIRE(dtri.valid);
	const Vector3 d_normal = (dv1 - dv0).cross(dv2 - dv0);
	uint32_t dir_samples = 0;
	float dir_max_err = 0.0f;
	for (int32_t y = dtri.min_y; y <= dtri.max_y; y++) {
		for (int32_t x = dtri.min_x; x <= dtri.max_x; x++) {
			float interp = 0.0f;
			if (!nextgeo_sw_sample(dtri, x, y, interp)) {
				continue;
			}
			float depth = 0.0f;
			REQUIRE(nextgeo_vsm_sw_recover_depth(clip0, interp, depth));
			// Analytic plane intersection along straight-down sun (-Y): tx = x_w / texel, ty = -z_w / texel.
			const Vector2 bary_p(float(x) + 0.5f, float(y) + 0.5f);
			const float denom = (dp1.x - dp0.x) * (dp2.y - dp0.y) - (dp1.y - dp0.y) * (dp2.x - dp0.x);
			const float l1 = ((bary_p.x - dp0.x) * (dp2.y - dp0.y) - (bary_p.y - dp0.y) * (dp2.x - dp0.x)) / denom;
			const float l2 = ((dp1.x - dp0.x) * (bary_p.y - dp0.y) - (dp1.y - dp0.y) * (bary_p.x - dp0.x)) / denom;
			const float l0 = 1.0f - l1 - l2;
			const Vector3 world_p = dv0 * l0 + dv1 * l1 + dv2 * l2;
			CHECK(Math::abs((world_p - dv0).dot(d_normal)) < 1e-4f);
			const float analytic_depth = VSM::row_dot(clip0.rows[2], world_p);
			dir_max_err = MAX(dir_max_err, Math::abs(depth - analytic_depth));
			dir_samples++;
		}
	}
	CHECK(dir_samples > 20u);
	CHECK(dir_max_err < 1e-3f);

	// Local perspective SW raster: steeply tilted triangle (w from 2 m to 8 m!) verifies exact 1/w interpolation.
	const Vector3 pv0(-0.8f, 4.0f, -0.6f); // w = 2.0 m
	const Vector3 pv1(3.2f, -2.0f, -2.4f); // w = 8.0 m
	const Vector3 pv2(-0.8f, 4.0f, 0.8f); // w = 2.0 m
	const Vector4 pp0 = nextgeo_vsm_sw_project_vertex(spot, VSM::MIP_COUNT - 1, Vector2i(0, 0), pv0);
	const Vector4 pp1 = nextgeo_vsm_sw_project_vertex(spot, VSM::MIP_COUNT - 1, Vector2i(0, 0), pv1);
	const Vector4 pp2 = nextgeo_vsm_sw_project_vertex(spot, VSM::MIP_COUNT - 1, Vector2i(0, 0), pv2);
	REQUIRE(pp0.w == 1.0f);
	REQUIRE(pp1.w == 1.0f);
	REQUIRE(pp2.w == 1.0f);
	const float ppx[3][2] = { { float(pp0.x), float(pp0.y) }, { float(pp1.x), float(pp1.y) }, { float(pp2.x), float(pp2.y) } };
	const float pz[3] = { float(pp0.z), float(pp1.z), float(pp2.z) };
	const NextGeoSwTriangle ptri = nextgeo_sw_setup_triangle(ppx, pz, int32_t(VSM::PAGE_SIZE), int32_t(VSM::PAGE_SIZE), NEXTGEO_SW_KEEP_BOTH);
	REQUIRE(ptri.valid);
	const Vector3 p_normal = (pv1 - pv0).cross(pv2 - pv0).normalized();
	const float plane_d = p_normal.dot(pv0);
	const float tan_spot = float(Math::tan(Math::deg_to_rad(35.0)));
	uint32_t persp_samples = 0;
	float persp_max_err = 0.0f;
	for (int32_t y = ptri.min_y; y <= ptri.max_y; y++) {
		for (int32_t x = ptri.min_x; x <= ptri.max_x; x++) {
			float interp_inv_w = 0.0f;
			if (!nextgeo_sw_sample(ptri, x, y, interp_inv_w)) {
				continue;
			}
			float depth_w = 0.0f;
			REQUIRE(nextgeo_vsm_sw_recover_depth(spot, interp_inv_w, depth_w));
			// Cast ray from spot origin through pixel center (x + 0.5, y + 0.5) on root mip (mip 6, slot 0,0).
			const float ndc_x = ((float(x) + 0.5f) / float(VSM::PAGE_SIZE)) * 2.0f - 1.0f;
			const float ndc_y = ((float(y) + 0.5f) / float(VSM::PAGE_SIZE)) * 2.0f - 1.0f;
			// For spot looking straight down (-Y): row0 = cot * (+X), row1 = cot * (-Z), row3 = -Y + 6.
			const Vector3 ray_dir = spot_xf.basis.get_column(0) * (ndc_x * tan_spot) + spot_xf.basis.get_column(1) * (ndc_y * tan_spot) - spot_xf.basis.get_column(2);
			const float t_hit = (plane_d - p_normal.dot(spot_xf.origin)) / p_normal.dot(ray_dir);
			// Since ray_dir has unit projection along -spot.basis.z, t_hit IS exact linear depth w!
			persp_max_err = MAX(persp_max_err, Math::abs(depth_w - t_hit));
			persp_samples++;
		}
	}
	CHECK(persp_samples > 30u);
	CHECK(persp_max_err < 0.02f); // Sub-pixel vertex snap over 6 m depth span gives ~0.005 m error (vs > 0.5 m for affine w).
}

TEST_CASE("[VSM][NextGeo] Static/dynamic layer routing hook defaults to static until #212 (#217)") {
	using namespace RendererRD;
	using namespace TestVSMNanite;

	const uint32_t valid = VMS::RECORD_VALID | VMS::RECORD_VISIBLE;
	float cur[3][4];
	float prev[3][4];
	fill_xform_rows(Transform3D(Basis(), Vector3(1.0f, 2.0f, 3.0f)), cur);
	fill_xform_rows(Transform3D(Basis(), Vector3(1.0f, 2.0f, 3.0f)), prev);
	CHECK_FALSE(nextgeo_vsm_transform_moved(cur, prev));
	cur[0][3] = 1.25f;
	CHECK(nextgeo_vsm_transform_moved(cur, prev));

	// Default (hook unset): every instance routes to static layer 0.
	CHECK_EQ(nextgeo_vsm_instance_layer(valid, false, 0u, false), 0u);
	CHECK_EQ(nextgeo_vsm_instance_layer(valid, true, 0u, false), 0u);
	CHECK_EQ(nextgeo_vsm_instance_layer(valid | VMS::RECORD_RT_SKINNED, false, 0u, false), 0u);
	CHECK_EQ(nextgeo_vsm_instance_layer(valid, false, 0u, true), 0u);

	// Hook enabled (#212): stationary opaque stays static (0); moved-this-frame/skinned/WPO routes dynamic (1).
	const uint32_t hook = NEXTGEO_VSM_CULL_FLAG_DYNAMIC_HOOK;
	CHECK_EQ(nextgeo_vsm_instance_layer(valid, false, hook, false), 0u);
	CHECK_EQ(nextgeo_vsm_instance_layer(valid | VMS::RECORD_MOVED, false, hook, false), 0u); // Sticky flag alone does not keep a stopped instance dynamic.
	CHECK_EQ(nextgeo_vsm_instance_layer(valid, true, hook, false), 1u);
	CHECK_EQ(nextgeo_vsm_instance_layer(valid | VMS::RECORD_RT_SKINNED, false, hook, false), 1u);
	CHECK_EQ(nextgeo_vsm_instance_layer(valid, false, hook, true), 1u);
}

// GodotNext: vsm_caching (#212, plan §21.3 contract S5). CPU models of the VSM cache: the AUTO rule and the
// cache modes, the deferred budget's coverage buckets, the invalidation pass (boxes and maps), the deferred
// invalidation budget in the allocation model, sun turns, light properties, the NextGeo record bits and the
// API. godotnext/testbed/tests/vsm/caching.gd checks the GPU passes against the same models.

namespace TestVSMCaching {

// One map's table with every entry allocated (page (0, 0): no page records are read).
LocalVector<uint32_t> allocated_table(uint32_t p_maps) {
	LocalVector<uint32_t> table;
	table.resize(p_maps * VSM::ENTRIES_PER_MAP);
	for (uint32_t i = 0; i < table.size(); i++) {
		table[i] = VSM::entry_pack(0, 0, VSM::ENTRY_ALLOCATED);
	}
	return table;
}

VSM::InvalidationBox make_box(const AABB &p_aabb, uint32_t p_flags, uint32_t p_layer_mask = 0xFFFFFFFFu) {
	VSM::InvalidationBox box;
	const Vector3 lo = p_aabb.position;
	const Vector3 hi = p_aabb.position + p_aabb.size;
	box.min_pos[0] = float(lo.x);
	box.min_pos[1] = float(lo.y);
	box.min_pos[2] = float(lo.z);
	box.max_pos[0] = float(hi.x);
	box.max_pos[1] = float(hi.y);
	box.max_pos[2] = float(hi.z);
	box.layer_mask = p_layer_mask;
	box.dirty_flags = p_flags;
	return box;
}

uint32_t count_flag(const LocalVector<uint32_t> &p_table, uint32_t p_map, uint32_t p_flag) {
	uint32_t count = 0;
	for (uint32_t i = p_map * VSM::ENTRIES_PER_MAP; i < (p_map + 1) * VSM::ENTRIES_PER_MAP; i++) {
		count += (p_table[i] & p_flag) ? 1 : 0;
	}
	return count;
}

// The emit pass on the CPU: a page the budget deferred keeps ENTRY_DIRTY_STATIC for a later frame.
void emit_requests(LocalVector<uint32_t> &r_table, const LocalVector<VSM::PageRecord> &p_pages, uint32_t p_width, const LocalVector<uint32_t> &p_requests) {
	for (uint32_t index : p_requests) {
		const uint32_t entry = r_table[index];
		bool deferred = false;
		if (entry & VSM::ENTRY_ALLOCATED) {
			const uint32_t p = VSM::entry_page_y(entry) * p_width + VSM::entry_page_x(entry);
			deferred = p < p_pages.size() && p_pages[p].bucket == VSM::DEFER_SKIPPED;
		}
		r_table[index] = VSM::entry_after_emit(entry) | (deferred ? VSM::ENTRY_DIRTY_STATIC : 0u);
	}
}

} // namespace TestVSMCaching

TEST_CASE("[VSM] Cache classification: the four modes, the 8-frame motion cooldown and every AUTO reason (#212)") {
	using namespace TestVSMCaching;
	VSM::CacheClassificationInput input;
	input.frame = 100;
	CHECK(VSM::classify_shadow_cache_dynamic(input) == 0u); // AUTO, never moved, nothing dynamic.

	// Moved at frame 100: dynamic for frames 100 to 107, static again from frame 108.
	input.last_moved_frame = 100;
	for (uint64_t frame = 100; frame < 100 + VSM::CACHE_MOTION_COOLDOWN_FRAMES; frame++) {
		input.frame = frame;
		CHECK(VSM::classify_shadow_cache_dynamic(input) == uint32_t(VSM::CACHE_DYNAMIC_MOVED));
	}
	input.frame = 100 + VSM::CACHE_MOTION_COOLDOWN_FRAMES;
	CHECK(VSM::classify_shadow_cache_dynamic(input) == 0u);
	input.frame = 99; // A frame counter behind the move (a reset) is not a move.
	CHECK(VSM::classify_shadow_cache_dynamic(input) == 0u);
	input.last_moved_frame = VSM::CACHE_NEVER_MOVED;
	input.frame = 100;

	// Every other AUTO reason sets its own bit.
	struct Case {
		bool skeleton = false;
		bool mesh_instance = false;
		bool particles = false;
		bool multimesh = false;
		uint32_t shader = 0;
		uint32_t expected = 0;
	};
	Case cases[9];
	cases[0].skeleton = true;
	cases[0].expected = VSM::CACHE_DYNAMIC_SKELETON;
	cases[1].mesh_instance = true; // Blend shapes.
	cases[1].expected = VSM::CACHE_DYNAMIC_MESH_INSTANCE;
	cases[2].particles = true;
	cases[2].expected = VSM::CACHE_DYNAMIC_PARTICLES;
	cases[3].multimesh = true;
	cases[3].expected = VSM::CACHE_DYNAMIC_MULTIMESH;
	cases[4].shader = cases[4].expected = VSM::CACHE_DYNAMIC_VERTEX;
	cases[5].shader = cases[5].expected = VSM::CACHE_DYNAMIC_VERTEX_TIME;
	cases[6].shader = cases[6].expected = VSM::CACHE_DYNAMIC_DEPTH;
	cases[7].shader = cases[7].expected = VSM::CACHE_DYNAMIC_MAIN_CAMERA;
	cases[8].shader = cases[8].expected = VSM::CACHE_DYNAMIC_ANIMATED;
	for (const Case &c : cases) {
		VSM::CacheClassificationInput in;
		in.frame = 100;
		in.has_skeleton = c.skeleton;
		in.has_mesh_instance = c.mesh_instance;
		in.is_particles = c.particles;
		in.is_dynamic_multimesh = c.multimesh;
		in.shader_dynamic_flags = c.shader;
		CHECK(VSM::classify_shadow_cache_dynamic(in) == c.expected);
		// RIGID and STATIC stay static whatever the instance does; ALWAYS is always dynamic.
		in.last_moved_frame = 100;
		in.mode = RSE::SHADOW_CACHE_INVALIDATION_RIGID;
		CHECK(VSM::classify_shadow_cache_dynamic(in) == 0u);
		in.mode = RSE::SHADOW_CACHE_INVALIDATION_STATIC;
		CHECK(VSM::classify_shadow_cache_dynamic(in) == 0u);
		in.mode = RSE::SHADOW_CACHE_INVALIDATION_ALWAYS;
		CHECK(VSM::classify_shadow_cache_dynamic(in) == uint32_t(VSM::CACHE_DYNAMIC_MODE_ALWAYS));
	}
	// Shader flags only carry shader reasons: a motion or mode bit in them is ignored.
	VSM::CacheClassificationInput masked;
	masked.shader_dynamic_flags = VSM::CACHE_DYNAMIC_MOVED | VSM::CACHE_DYNAMIC_SKELETON | VSM::CACHE_DYNAMIC_MODE_ALWAYS;
	CHECK(VSM::classify_shadow_cache_dynamic(masked) == 0u);
	// ALWAYS with nothing dynamic.
	VSM::CacheClassificationInput always;
	always.mode = RSE::SHADOW_CACHE_INVALIDATION_ALWAYS;
	CHECK(VSM::classify_shadow_cache_dynamic(always) == uint32_t(VSM::CACHE_DYNAMIC_MODE_ALWAYS));
}

TEST_CASE("[VSM] Deferred budget coverage buckets: exact below 16 pixels, two per power of two, monotonic and capped (#212)") {
	using namespace TestVSMCaching;
	for (uint32_t pixels = 0; pixels < 16; pixels++) {
		CHECK(VSM::coverage_bucket(pixels) == pixels);
	}
	CHECK(VSM::coverage_bucket(16) == 16u);
	CHECK(VSM::coverage_bucket(23) == 16u);
	CHECK(VSM::coverage_bucket(24) == 17u);
	CHECK(VSM::coverage_bucket(31) == 17u);
	CHECK(VSM::coverage_bucket(32) == 18u);
	CHECK(VSM::coverage_bucket(48) == 19u);
	CHECK(VSM::coverage_bucket(1u << 27) == 62u);
	CHECK(VSM::coverage_bucket(3u << 26) == VSM::DEFER_BUCKET_COUNT - 1);
	CHECK(VSM::coverage_bucket(UINT32_MAX) == VSM::DEFER_BUCKET_COUNT - 1);
	uint32_t previous = 0;
	for (uint32_t pixels = 1; pixels < (1u << 20); pixels++) {
		const uint32_t bucket = VSM::coverage_bucket(pixels);
		CHECK_MESSAGE(bucket >= previous, "pixels ", pixels);
		CHECK(bucket < VSM::DEFER_BUCKET_COUNT);
		previous = bucket;
	}
	for (uint32_t shift = 20; shift < 32; shift++) {
		const uint32_t bucket = VSM::coverage_bucket(1u << shift);
		CHECK(bucket >= previous);
		CHECK(bucket < VSM::DEFER_BUCKET_COUNT);
		previous = bucket;
	}
}

TEST_CASE("[VSM] Box invalidation: clipmap window and mip edges, negative origins, the layer mask, local frustums and the near plane (#212)") {
	using namespace TestVSMCaching;
	// Map 0: clipmap level 2 (256 texels per meter, a mip 0 page is 0.25 m) at origin (-3, 5), so its window
	// covers X [-0.75, 15.25) and Y [1.25, 17.25). Map 1: a spot face at the origin looking down -Z, range 20.
	VSM::MapData maps[2];
	maps[0] = VSM::map_data_clipmap(Basis(), 2, Vector2i(-3, 5), 200.0f);
	maps[1] = VSM::map_data_face(VSM::MAP_KIND_SPOT, 0, Transform3D(), 20.0f, 45.0f);
	const uint32_t all_layers[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };

	// A box inside one mip 0 page: one page per mip, except mip 6. A mip's window is aligned to its own pages
	// (#219): at mip 6 the level's single page holds the window [-64, 0) of mip 0 pages, which misses X > 0
	// (the next level covers it).
	{
		LocalVector<uint32_t> table = allocated_table(2);
		const VSM::InvalidationCounts counts = VSM::invalidate_box_model(table.ptr(), 2, maps, all_layers, nullptr, 0, 0, make_box(AABB(Vector3(0.1f, 2.1f, -1.0f), Vector3(0.1f, 0.1f, 2.0f)), VSM::ENTRY_DIRTY_STATIC));
		CHECK(counts.static_pages == VSM::MIP_COUNT - 1);
		CHECK(counts.dynamic_pages == 0u);
		CHECK(count_flag(table, 0, VSM::ENTRY_DIRTY_STATIC) == VSM::MIP_COUNT - 1);
		CHECK(count_flag(table, 0, VSM::ENTRY_DIRTY_DYNAMIC) == 0u);
		// Texels x [25, 51] and y [537, 563].
		for (uint32_t mip = 0; mip < VSM::MIP_COUNT; mip++) {
			const uint32_t mask = (VSM::PAGES_PER_SIDE >> mip) - 1;
			const bool in_window = mip + 1 < VSM::MIP_COUNT;
			CHECK(((table[VSM::entry_index(0, mip, (25u >> (6 + mip)) & mask, (537u >> (6 + mip)) & mask)] & VSM::ENTRY_DIRTY_STATIC) != 0) == in_window);
		}
		// The spot face does not see it: Y 2.1 is above its 90 degree frustum at w <= 1.
		CHECK(count_flag(table, 1, VSM::ENTRY_DIRTY_STATIC) == 0u);
		// The same box again marks nothing new.
		const VSM::InvalidationCounts again = VSM::invalidate_box_model(table.ptr(), 2, maps, all_layers, nullptr, 0, 0, make_box(AABB(Vector3(0.1f, 2.1f, -1.0f), Vector3(0.1f, 0.1f, 2.0f)), VSM::ENTRY_DIRTY_STATIC));
		CHECK(again.static_pages == 0u);
	}
	// A box across a mip 0 page edge (X 0.25): two pages at mip 0, one at mips 1 to 5.
	{
		LocalVector<uint32_t> table = allocated_table(1);
		const VSM::InvalidationCounts counts = VSM::invalidate_box_model(table.ptr(), 1, maps, all_layers, nullptr, 0, 0, make_box(AABB(Vector3(0.2f, 2.1f, -1.0f), Vector3(0.1f, 0.1f, 2.0f)), VSM::ENTRY_DIRTY_STATIC));
		CHECK(counts.static_pages == VSM::MIP_COUNT);
	}
	// Left of X 0 the pages are negative and wrap: absolute page -3 is slot 61 at mip 0, -2 is slot 30 at mip 1;
	// mip 6's window [-64, 0) holds it, so every mip marks one page.
	{
		LocalVector<uint32_t> table = allocated_table(1);
		const VSM::InvalidationCounts counts = VSM::invalidate_box_model(table.ptr(), 1, maps, all_layers, nullptr, 0, 0, make_box(AABB(Vector3(-0.7f, 2.1f, -1.0f), Vector3(0.1f, 0.1f, 2.0f)), VSM::ENTRY_DIRTY_STATIC));
		CHECK(counts.static_pages == VSM::MIP_COUNT);
		CHECK((table[VSM::entry_index(0, 0, 61, 8)] & VSM::ENTRY_DIRTY_STATIC) != 0);
		CHECK((table[VSM::entry_index(0, 1, 30, 4)] & VSM::ENTRY_DIRTY_STATIC) != 0);
		CHECK((table[VSM::entry_index(0, 0, 0, 8)] & VSM::ENTRY_DIRTY_STATIC) == 0);
	}
	// Past the window's far edge at every mip (X 15.3; its alias would be slot 61): nothing.
	{
		LocalVector<uint32_t> table = allocated_table(1);
		const VSM::InvalidationCounts counts = VSM::invalidate_box_model(table.ptr(), 1, maps, all_layers, nullptr, 0, 0, make_box(AABB(Vector3(15.3f, 2.1f, -1.0f), Vector3(0.1f, 0.1f, 2.0f)), VSM::ENTRY_DIRTY_STATIC));
		CHECK(counts.static_pages == 0u);
		CHECK(count_flag(table, 0, VSM::ENTRY_DIRTY_STATIC) == 0u);
	}
	// The map's shadow caster mask must share a layer with the box.
	{
		LocalVector<uint32_t> table = allocated_table(1);
		const uint32_t layer_two[1] = { 2u };
		VSM::InvalidationCounts counts = VSM::invalidate_box_model(table.ptr(), 1, maps, layer_two, nullptr, 0, 0, make_box(AABB(Vector3(0.1f, 2.1f, -1.0f), Vector3(0.1f, 0.1f, 2.0f)), VSM::ENTRY_DIRTY_STATIC, 1u));
		CHECK(counts.static_pages == 0u);
		counts = VSM::invalidate_box_model(table.ptr(), 1, maps, layer_two, nullptr, 0, 0, make_box(AABB(Vector3(0.1f, 2.1f, -1.0f), Vector3(0.1f, 0.1f, 2.0f)), VSM::ENTRY_DIRTY_STATIC, 3u));
		CHECK(counts.static_pages == VSM::MIP_COUNT - 1);
	}
	// Unallocated entries are not marked; empty flags and inverted boxes mark nothing.
	{
		LocalVector<uint32_t> table;
		table.resize(VSM::ENTRIES_PER_MAP);
		memset(table.ptr(), 0, table.size() * sizeof(uint32_t));
		VSM::InvalidationCounts counts = VSM::invalidate_box_model(table.ptr(), 1, maps, all_layers, nullptr, 0, 0, make_box(AABB(Vector3(0.1f, 2.1f, -1.0f), Vector3(0.1f, 0.1f, 2.0f)), VSM::ENTRY_DIRTY_STATIC));
		CHECK(counts.static_pages == 0u);
		CHECK(count_flag(table, 0, VSM::ENTRY_DIRTY_STATIC) == 0u);
		LocalVector<uint32_t> full = allocated_table(1);
		counts = VSM::invalidate_box_model(full.ptr(), 1, maps, all_layers, nullptr, 0, 0, make_box(AABB(Vector3(0.1f, 2.1f, -1.0f), Vector3(0.1f, 0.1f, 2.0f)), 0u));
		CHECK(counts.static_pages == 0u);
		VSM::InvalidationBox inverted = make_box(AABB(Vector3(0.1f, 2.1f, -1.0f), Vector3(0.1f, 0.1f, 2.0f)), VSM::ENTRY_DIRTY_STATIC);
		inverted.max_pos[0] = inverted.min_pos[0] - 1.0f;
		counts = VSM::invalidate_box_model(full.ptr(), 1, maps, all_layers, nullptr, 0, 0, inverted);
		CHECK(counts.static_pages == 0u);
	}
	// The spot face: behind the light, past its range and outside its cone mark nothing; a small box near its
	// axis marks one page per mip; a box across the near plane marks the whole face.
	{
		const VSM::MapData *spot = &maps[1];
		const AABB rejected[3] = {
			AABB(Vector3(-0.5f, -0.5f, 1.0f), Vector3(1.0f, 1.0f, 1.0f)),
			AABB(Vector3(-0.5f, -0.5f, -26.0f), Vector3(1.0f, 1.0f, 1.0f)),
			AABB(Vector3(30.0f, -0.5f, -10.5f), Vector3(1.0f, 1.0f, 1.0f)),
		};
		for (const AABB &aabb : rejected) {
			LocalVector<uint32_t> table = allocated_table(1);
			const VSM::InvalidationCounts counts = VSM::invalidate_box_model(table.ptr(), 1, spot, all_layers, nullptr, 0, 0, make_box(aabb, VSM::ENTRY_DIRTY_STATIC));
			CHECK(counts.static_pages == 0u);
		}
		LocalVector<uint32_t> table = allocated_table(1);
		VSM::InvalidationCounts counts = VSM::invalidate_box_model(table.ptr(), 1, spot, all_layers, nullptr, 0, 0, make_box(AABB(Vector3(0.01f, 0.01f, -10.01f), Vector3(0.01f, 0.01f, 0.01f)), VSM::ENTRY_DIRTY_STATIC));
		CHECK(counts.static_pages == VSM::MIP_COUNT);
		CHECK((table[VSM::entry_index(0, VSM::MIP_COUNT - 1, 0, 0)] & VSM::ENTRY_DIRTY_STATIC) != 0);
		LocalVector<uint32_t> cut = allocated_table(1);
		counts = VSM::invalidate_box_model(cut.ptr(), 1, spot, all_layers, nullptr, 0, 0, make_box(AABB(Vector3(-0.2f, -0.2f, -0.5f), Vector3(0.4f, 0.4f, 1.0f)), VSM::ENTRY_DIRTY_STATIC));
		CHECK(counts.static_pages == VSM::ENTRIES_PER_MAP);
	}
	// A dynamic box marks DIRTY_DYNAMIC and records that the pages now hold dynamic depth.
	{
		LocalVector<uint32_t> table;
		table.resize(VSM::ENTRIES_PER_MAP);
		memset(table.ptr(), 0, table.size() * sizeof(uint32_t));
		const uint32_t index = VSM::entry_index(0, 0, 0, 8);
		table[index] = VSM::entry_pack(1, 0, VSM::ENTRY_ALLOCATED);
		VSM::PageRecord pages[4];
		pages[1].owner = index + 1;
		const VSM::InvalidationCounts counts = VSM::invalidate_box_model(table.ptr(), 1, maps, all_layers, pages, 4, 2, make_box(AABB(Vector3(0.1f, 2.1f, -1.0f), Vector3(0.1f, 0.1f, 2.0f)), VSM::ENTRY_DIRTY_DYNAMIC));
		CHECK(counts.dynamic_pages == 1u);
		CHECK(counts.static_pages == 0u);
		CHECK(table[index] == VSM::entry_pack(1, 0, VSM::ENTRY_ALLOCATED | VSM::ENTRY_DIRTY_DYNAMIC));
		CHECK(pages[1].pad == VSM::PAGE_FLAG_HAS_DYNAMIC_CURR);
		CHECK(pages[0].pad == 0u);
	}
}

TEST_CASE("[VSM] Map invalidation marks every allocated page; dynamic_if_used adds layer 1 only where it held depth (#212)") {
	using namespace TestVSMCaching;
	LocalVector<uint32_t> table;
	table.resize(2 * VSM::ENTRIES_PER_MAP);
	memset(table.ptr(), 0, table.size() * sizeof(uint32_t));
	// Map 0 holds pages 0 to 3 (a 2x2 grid); page 1 had dynamic depth last frame, page 2 this frame.
	for (uint32_t i = 0; i < 4; i++) {
		table[i * 7] = VSM::entry_pack(i % 2, i / 2, VSM::ENTRY_ALLOCATED);
	}
	table[VSM::ENTRIES_PER_MAP + 3] = VSM::entry_pack(0, 0, VSM::ENTRY_ALLOCATED); // Map 1.
	VSM::PageRecord pages[4];
	pages[1].pad = VSM::PAGE_FLAG_HAS_DYNAMIC_PREV;
	pages[2].pad = VSM::PAGE_FLAG_HAS_DYNAMIC_CURR;

	LocalVector<uint32_t> plain(table);
	VSM::InvalidationCounts counts = VSM::invalidate_map_model(plain.ptr(), 2, 0, VSM::ENTRY_DIRTY_STATIC, pages, 4, 2, false);
	CHECK(counts.static_pages == 4u);
	CHECK(counts.dynamic_pages == 0u);
	CHECK(count_flag(plain, 1, VSM::ENTRY_DIRTY_STATIC) == 0u); // Other maps keep their pages.

	LocalVector<uint32_t> used(table);
	counts = VSM::invalidate_map_model(used.ptr(), 2, 0, VSM::ENTRY_DIRTY_STATIC, pages, 4, 2, true);
	CHECK(counts.static_pages == 4u);
	CHECK(counts.dynamic_pages == 2u);
	CHECK((used[0] & VSM::ENTRY_DIRTY_DYNAMIC) == 0);
	CHECK((used[7] & VSM::ENTRY_DIRTY_DYNAMIC) != 0);
	CHECK((used[14] & VSM::ENTRY_DIRTY_DYNAMIC) != 0);
	CHECK((used[21] & VSM::ENTRY_DIRTY_DYNAMIC) == 0);
	// dynamic_if_used reads the page flags; it does not add to them.
	CHECK(pages[0].pad == 0u);
	CHECK(pages[1].pad == VSM::PAGE_FLAG_HAS_DYNAMIC_PREV);
	CHECK(pages[3].pad == 0u);

	// A dynamic map invalidation records layer-1 use on every page; a map past the table does nothing.
	LocalVector<uint32_t> dynamic(table);
	counts = VSM::invalidate_map_model(dynamic.ptr(), 2, 0, VSM::ENTRY_DIRTY_DYNAMIC, pages, 4, 2, false);
	CHECK(counts.dynamic_pages == 4u);
	for (uint32_t p = 0; p < 4; p++) {
		CHECK((pages[p].pad & VSM::PAGE_FLAG_HAS_DYNAMIC_CURR) != 0);
	}
	counts = VSM::invalidate_map_model(dynamic.ptr(), 2, 2, VSM::ENTRY_DIRTY_STATIC, pages, 4, 2, false);
	CHECK(counts.static_pages == 0u);
}

TEST_CASE("[VSM] Deferred invalidation budget: 0 renders every dirty static page, N renders the N with the most screen coverage (#212)") {
	using namespace TestVSMCaching;
	// Ten pool pages; entries 0 to 7 get pages in frame 1. In frame 2 entries 0 to 5 are invalidated
	// (static), entry 2 also dynamic; entry i's page covers 2^(i + 4) screen pixels.
	struct Run {
		LocalVector<uint32_t> table;
		LocalVector<VSM::PageRecord> pages;
		LocalVector<uint32_t> coverage;
		LocalVector<uint32_t> requests;
		Run() {
			table.resize(VSM::ENTRIES_PER_MAP);
			memset(table.ptr(), 0, table.size() * sizeof(uint32_t));
			pages.resize(10);
			coverage.resize(10);
			memset(coverage.ptr(), 0, coverage.size() * sizeof(uint32_t));
			for (uint32_t i = 0; i < 8; i++) {
				requests.push_back(i);
			}
		}
		uint32_t page_of(uint32_t p_index) const {
			const uint32_t entry = table[p_index];
			return VSM::entry_page_y(entry) * 4 + VSM::entry_page_x(entry);
		}
		VSM::AllocationCounts frame(uint32_t p_frame, uint32_t p_budget) {
			for (uint32_t index : requests) {
				table[index] |= VSM::ENTRY_REQUESTED;
			}
			const VSM::AllocationCounts counts = VSM::allocate_model(table.ptr(), table.size(), pages.ptr(), pages.size(), 4, requests.ptr(), requests.size(), p_frame, 30, 0, p_budget, coverage.ptr());
			emit_requests(table, pages, 4, requests);
			return counts;
		}
		void invalidate() {
			for (uint32_t i = 0; i < 6; i++) {
				table[i] |= VSM::ENTRY_DIRTY_STATIC;
				coverage[page_of(i)] = 1u << (i + 4);
			}
			table[2] |= VSM::ENTRY_DIRTY_DYNAMIC;
		}
	};
	// Budget 0 (unlimited) and a budget no smaller than the dirty pages: all six now.
	for (uint32_t budget : { 0u, 6u, 100u }) {
		Run run;
		VSM::AllocationCounts counts = run.frame(1, budget);
		CHECK(counts.new_pages == 8u);
		run.invalidate();
		counts = run.frame(2, budget);
		CHECK(counts.kept == 8u);
		CHECK(counts.dirty == 6u);
		CHECK(counts.dirty_static == 6u);
		CHECK(counts.dirty_dynamic == 1u);
		CHECK(counts.deferred_static == 0u);
		for (uint32_t i = 0; i < 8; i++) {
			CHECK((run.table[i] & VSM::ENTRY_DIRTY_MASK) == 0);
		}
	}

	// Budget 2: entries 5 and 4 (the most coverage) now, then 3 and 2, then 1 and 0. Entry 2's dynamic layer
	// renders in frame 2 even though its static layer waits; the deferred pages keep ENTRY_DIRTY_STATIC.
	{
		Run run;
		run.frame(1, 2);
		run.invalidate();
		VSM::AllocationCounts counts = run.frame(2, 2);
		CHECK(counts.dirty_static == 2u);
		CHECK(counts.deferred_static == 4u);
		CHECK(counts.dirty_dynamic == 1u);
		CHECK(counts.dirty == 3u);
		for (uint32_t i = 0; i < 6; i++) {
			const bool deferred = i < 4;
			CHECK_MESSAGE(((run.table[i] & VSM::ENTRY_DIRTY_STATIC) != 0) == deferred, "entry ", i);
			CHECK((run.table[i] & VSM::ENTRY_DIRTY_DYNAMIC) == 0);
		}
		counts = run.frame(3, 2);
		CHECK(counts.dirty_static == 2u);
		CHECK(counts.deferred_static == 2u);
		CHECK((run.table[3] & VSM::ENTRY_DIRTY_STATIC) == 0);
		CHECK((run.table[2] & VSM::ENTRY_DIRTY_STATIC) == 0);
		CHECK((run.table[1] & VSM::ENTRY_DIRTY_STATIC) != 0);
		counts = run.frame(4, 2);
		CHECK(counts.dirty_static == 2u);
		CHECK(counts.deferred_static == 0u);
		counts = run.frame(5, 2);
		CHECK(counts.dirty == 0u);
		CHECK(counts.deferred_static == 0u);
	}

	// New pages and scrolled pages are never deferred, even with a budget of 1.
	{
		Run run;
		run.frame(1, 1);
		run.invalidate();
		run.table[6] |= VSM::ENTRY_SCROLLED | VSM::ENTRY_DIRTY_STATIC;
		run.requests.push_back(8); // A new page.
		VSM::AllocationCounts counts = run.frame(2, 1);
		CHECK(counts.new_pages == 1u);
		CHECK(counts.deferred_static == 5u); // Entries 0 to 4; entry 5 has the most coverage.
		CHECK(counts.dirty_static == 3u); // Entry 5, the scrolled entry 6 and the new entry 8.
		CHECK((run.table[6] & VSM::ENTRY_DIRTY_MASK) == 0);
		CHECK((run.table[8] & VSM::ENTRY_ALLOCATED) != 0);
	}
}

TEST_CASE("[VSM] Unrequested pages: dirty static ones are evicted, ones with only dirty dynamic stay cached (#212)") {
	using namespace TestVSMCaching;
	LocalVector<uint32_t> table;
	table.resize(VSM::ENTRIES_PER_MAP);
	memset(table.ptr(), 0, table.size() * sizeof(uint32_t));
	VSM::PageRecord page;
	page.owner = 6;
	page.last_used = 10;
	uint32_t bucket = 0;
	table[5] = VSM::entry_pack(1, 0, VSM::ENTRY_ALLOCATED | VSM::ENTRY_DIRTY_STATIC);
	CHECK(VSM::page_update(page, 1, 4, table.ptr(), table.size(), 11, 30, bucket) == VSM::PAGE_UPDATE_EVICTED);
	table[5] = VSM::entry_pack(1, 0, VSM::ENTRY_ALLOCATED | VSM::ENTRY_DIRTY_DYNAMIC);
	CHECK(VSM::page_update(page, 1, 4, table.ptr(), table.size(), 11, 30, bucket) == VSM::PAGE_UPDATE_CACHED);
	table[5] = VSM::entry_pack(1, 0, VSM::ENTRY_ALLOCATED | VSM::ENTRY_DIRTY_STATIC | VSM::ENTRY_REQUESTED);
	CHECK(VSM::page_update(page, 1, 4, table.ptr(), table.size(), 11, 30, bucket) == VSM::PAGE_UPDATE_KEPT);

	// In the allocation model: a cached page's "dynamic this frame" becomes "dynamic last frame"; an evicted
	// page loses both.
	LocalVector<VSM::PageRecord> pages;
	pages.resize(4);
	table[5] = VSM::entry_pack(1, 0, VSM::ENTRY_ALLOCATED | VSM::ENTRY_DIRTY_DYNAMIC);
	table[9] = VSM::entry_pack(2, 0, VSM::ENTRY_ALLOCATED | VSM::ENTRY_DIRTY_STATIC);
	pages[1].owner = 6;
	pages[1].last_used = 10;
	pages[1].pad = VSM::PAGE_FLAG_HAS_DYNAMIC_CURR;
	pages[2].owner = 10;
	pages[2].last_used = 10;
	pages[2].pad = VSM::PAGE_FLAG_HAS_DYNAMIC_CURR;
	const VSM::AllocationCounts counts = VSM::allocate_model(table.ptr(), table.size(), pages.ptr(), pages.size(), 4, nullptr, 0, 11, 30);
	CHECK(counts.cached == 1u);
	CHECK(counts.evicted == 1u);
	CHECK(pages[1].pad == VSM::PAGE_FLAG_HAS_DYNAMIC_PREV);
	CHECK(pages[2].owner == 0u);
	CHECK(pages[2].pad == 0u);
	CHECK(table[9] == 0u);
}

TEST_CASE("[VSM] Light property changes are noted per light, and the deferred budget setting clamps (#212)") {
	using namespace TestVSMCaching;
	TestVSMStorage storage;
	const RID sun = light_rid(7);
	REQUIRE(storage.directional_update(sun, Basis(), Vector3(), 100.0f) != VSM::MAP_NONE);
	VSM::LightProperties props;
	props.range = 10.0f;
	CHECK_FALSE(storage.note_light_properties(sun, props, true)); // The first note only records.
	CHECK_FALSE(storage.note_light_properties(sun, props, true));
	VSM::LightProperties changed = props;
	changed.range = 11.0f;
	CHECK(storage.note_light_properties(sun, changed, true));
	CHECK_FALSE(storage.note_light_properties(sun, changed, true));
	changed.transform.origin = Vector3(1, 0, 0);
	CHECK_FALSE(storage.note_light_properties(sun, changed, false)); // A directional light's position is not a property.
	CHECK(storage.note_light_properties(sun, changed, true));
	changed.spot_angle = 30.0f;
	CHECK(storage.note_light_properties(sun, changed, true));
	changed.area_size = Vector2(1, 2);
	CHECK(storage.note_light_properties(sun, changed, true));
	changed.caster_mask = 1u;
	CHECK(storage.note_light_properties(sun, changed, true));
	changed.reverse_cull = true;
	CHECK(storage.note_light_properties(sun, changed, true));
	CHECK_FALSE(storage.note_light_properties(light_rid(8), changed, true)); // A light without maps.

	ProjectSettings *settings = ProjectSettings::get_singleton();
	REQUIRE(settings != nullptr);
	const StringName name = VSM::SETTING_DEFERRED_INVALIDATION_BUDGET;
	const bool had = settings->has_setting(name);
	const Variant old_value = had ? settings->get_setting(name) : Variant(0);
	settings->set_setting(name, -5);
	CHECK(VSM::read_deferred_invalidation_budget() == 0u);
	settings->set_setting(name, 512);
	CHECK(VSM::read_deferred_invalidation_budget() == 512u);
	settings->set_setting(name, int64_t(1) << 40);
	CHECK(VSM::read_deferred_invalidation_budget() == VSM::POOL_MAX_PAGES);
	settings->set_setting(name, old_value);
}

TEST_CASE("[VSM] A sun turn scrolls the pages that leave each level's window and invalidates the rest (#212)") {
	using namespace TestVSMCaching;
	TestVSMStorage storage;
	const RID sun = light_rid(9);
	const Vector3 camera(0.07f, 0.0f, -3.0f);
	const uint32_t first = storage.directional_update(sun, Basis(), camera, 100.0f, true);
	REQUIRE(first != VSM::MAP_NONE);
	const uint32_t levels = storage.map_get(sun)->count;
	fill_light(storage, sun, 3);
	Vector2i old_origins[VSM::CLIPMAP_MAX_LEVELS];
	for (uint32_t level = 0; level < levels; level++) {
		old_origins[level] = storage.get_clipmap_origin(sun, level);
	}
	CHECK(storage.invalidated_maps.is_empty());
	CHECK(storage.scroll_calls == 0);

	// Turned: every level is invalidated (static layer; layer 1 only where it was used), and the levels whose
	// window moved also scroll the pages that left it, which are never deferred.
	storage.directional_update(sun, Basis(Vector3(0, 1, 0), 0.1f), camera, 100.0f, true);
	CHECK(storage.invalidated_maps.size() == levels);
	uint32_t moved_levels = 0;
	for (uint32_t level = 0; level < levels; level++) {
		CHECK(storage.invalidated_maps[level] == first + level);
		const VSM::ClipmapScroll scroll = VSM::clipmap_scroll(old_origins[level], storage.get_clipmap_origin(sun, level));
		moved_levels += scroll.any ? 1 : 0;
		uint32_t expected_scrolled = 0;
		for (uint32_t mip = 0; mip < VSM::MIP_COUNT; mip++) {
			const uint32_t pages = VSM::PAGES_PER_SIDE >> mip;
			for (uint32_t y = 0; y < pages; y++) {
				for (uint32_t x = 0; x < pages; x++) {
					expected_scrolled += VSM::clipmap_scroll_hits(scroll, mip, x, y) ? 1 : 0;
				}
			}
		}
		CHECK(count_flag(storage.table, first + level, VSM::ENTRY_SCROLLED) == expected_scrolled);
		CHECK(count_flag(storage.table, first + level, VSM::ENTRY_DIRTY_STATIC) == VSM::ENTRIES_PER_MAP);
	}
	CHECK(moved_levels > 0);
	CHECK(storage.scroll_calls == moved_levels);

	// Without p_invalidate_on_turn a turn scrolls every page of every level, as before #212.
	TestVSMStorage plain;
	plain.directional_update(sun, Basis(), camera, 100.0f);
	fill_light(plain, sun, 3);
	plain.directional_update(sun, Basis(Vector3(0, 1, 0), 0.1f), camera, 100.0f);
	CHECK(plain.invalidated_maps.is_empty());
	CHECK(plain.scroll_calls == levels);
	CHECK(count_flag(plain.table, plain.map_get(sun)->first, VSM::ENTRY_SCROLLED) == VSM::ENTRIES_PER_MAP);
}

TEST_CASE("[VSM] Freed maps drop their pending map invalidations; the other maps keep theirs (#212)") {
	LocalVector<VSM::PendingMapInvalidation> pending;
	const uint32_t maps[6] = { 3, 7, 8, 9, 12, 8 };
	for (uint32_t map : maps) {
		VSM::PendingMapInvalidation item;
		item.map = map;
		item.dirty_flags = VSM::ENTRY_DIRTY_STATIC;
		pending.push_back(item);
	}
	VSM::prune_map_invalidations(pending, 7, 3); // Maps 7, 8 and 9 were freed.
	REQUIRE(pending.size() == 2u);
	const bool kept_3_and_12 = (pending[0].map == 3u && pending[1].map == 12u) || (pending[0].map == 12u && pending[1].map == 3u);
	CHECK(kept_3_and_12);
	VSM::prune_map_invalidations(pending, 0, 0); // No map.
	CHECK(pending.size() == 2u);
	VSM::prune_map_invalidations(pending, 4, UINT32_MAX); // A range that would wrap past UINT32_MAX holds map 12, not map 3.
	REQUIRE(pending.size() == 1u);
	CHECK(pending[0].map == 3u);
	CHECK(pending[0].dirty_flags == uint32_t(VSM::ENTRY_DIRTY_STATIC));
}

namespace TestVSMCaching {

// RendererSceneRender::RenderVSMInvalidation's fields, which append_invalidation() and publish_invalidations() use.
struct TestInvalidation {
	AABB world_aabb;
	uint32_t layer_mask = 0;
	uint32_t dirty_flags = 0;
};

TestInvalidation make_invalidation(float p_x, uint32_t p_layer_mask, uint32_t p_dirty_flags) {
	TestInvalidation event;
	event.world_aabb = AABB(Vector3(p_x, 0, 0), Vector3(1, 1, 1));
	event.layer_mask = p_layer_mask;
	event.dirty_flags = p_dirty_flags;
	return event;
}

} // namespace TestVSMCaching

TEST_CASE("[VSM] A scenario's invalidation list is capped by merging into its last box, and boxes no view applied stay published (#212)") {
	using namespace TestVSMCaching;
	const uint32_t cap = VSM::MAX_PENDING_INVALIDATIONS;
	LocalVector<TestInvalidation> list;
	for (uint32_t i = 0; i < cap; i++) {
		VSM::append_invalidation(list, make_invalidation(float(i), 1u, VSM::ENTRY_DIRTY_STATIC));
	}
	REQUIRE(list.size() == cap);
	const AABB last_before = list[cap - 1].world_aabb;
	VSM::append_invalidation(list, make_invalidation(-50.0f, 4u, VSM::ENTRY_DIRTY_DYNAMIC));
	VSM::append_invalidation(list, make_invalidation(40000.0f, 2u, VSM::ENTRY_DIRTY_STATIC));
	CHECK(list.size() == cap);
	const TestInvalidation &last = list[cap - 1];
	CHECK(last.world_aabb.encloses(last_before));
	CHECK(last.world_aabb.encloses(make_invalidation(-50.0f, 0, 0).world_aabb));
	CHECK(last.world_aabb.encloses(make_invalidation(40000.0f, 0, 0).world_aabb));
	CHECK(last.layer_mask == 7u);
	CHECK(last.dirty_flags == uint32_t(VSM::ENTRY_DIRTY_STATIC | VSM::ENTRY_DIRTY_DYNAMIC));
	CHECK(list[0].world_aabb == make_invalidation(0.0f, 0, 0).world_aabb); // The other boxes are untouched.
	CHECK(list[cap - 2].layer_mask == 1u);

	// Publishing: the boxes written since the last publish replace the published ones once a view applied them, and
	// are added to them while no view has (a frame without a page-marking view of the scenario drops none).
	LocalVector<TestInvalidation> published;
	LocalVector<TestInvalidation> writing;
	writing.push_back(make_invalidation(1.0f, 1u, VSM::ENTRY_DIRTY_STATIC));
	writing.push_back(make_invalidation(2.0f, 1u, VSM::ENTRY_DIRTY_STATIC));
	VSM::publish_invalidations(published, writing, false);
	CHECK(published.size() == 2u);
	CHECK(writing.is_empty());
	writing.push_back(make_invalidation(3.0f, 2u, VSM::ENTRY_DIRTY_DYNAMIC));
	VSM::publish_invalidations(published, writing, false); // Not applied: kept, and the new box added.
	REQUIRE(published.size() == 3u);
	CHECK(published[0].world_aabb.position.x == 1.0f);
	CHECK(published[2].world_aabb.position.x == 3.0f);
	CHECK(published[2].dirty_flags == uint32_t(VSM::ENTRY_DIRTY_DYNAMIC));
	writing.push_back(make_invalidation(4.0f, 1u, VSM::ENTRY_DIRTY_STATIC));
	VSM::publish_invalidations(published, writing, true); // Applied: replaced.
	REQUIRE(published.size() == 1u);
	CHECK(published[0].world_aabb.position.x == 4.0f);
	VSM::publish_invalidations(published, writing, true); // Applied, and nothing new.
	CHECK(published.is_empty());

	// Boxes kept while no view applies them stay within the cap.
	published.push_back(make_invalidation(-1.0f, 1u, VSM::ENTRY_DIRTY_STATIC));
	for (uint32_t i = 0; i < cap + 10; i++) {
		writing.push_back(make_invalidation(float(i), 1u, VSM::ENTRY_DIRTY_STATIC));
	}
	VSM::publish_invalidations(published, writing, false);
	CHECK(published.size() == cap);
	CHECK(published[0].world_aabb.position.x == -1.0f);
	CHECK(published[cap - 1].world_aabb.encloses(make_invalidation(float(cap + 9), 0, 0).world_aabb));
	CHECK(writing.is_empty());
}

TEST_CASE("[VSM][NextGeo] Record bits 12-14 hold the cache mode and the resolved dynamic flag; each mode routes to its layer (#212)") {
	using namespace RendererRD;
	using VMS = RendererVirtualMeshStorage;
	static_assert(VMS::RECORD_VSM_CACHE_MODE_SHIFT == 12u);
	static_assert(VMS::RECORD_VSM_CACHE_MODE_MASK == (3u << 12));
	static_assert(VMS::RECORD_VSM_DYNAMIC == (1u << 14));
	static_assert((VMS::RECORD_VSM_DYNAMIC & VMS::RECORD_VSM_CACHE_MODE_MASK) == 0u);
	static_assert(((VMS::RECORD_RT_FAR | VMS::RECORD_RT_ALPHA_AS_OPAQUE | VMS::RECORD_RT_SKINNED | VMS::RECORD_RT_STATIC | VMS::RECORD_MOVED) & (VMS::RECORD_VSM_CACHE_MODE_MASK | VMS::RECORD_VSM_DYNAMIC)) == 0u);
	const RSE::ShadowCacheInvalidation modes[4] = { RSE::SHADOW_CACHE_INVALIDATION_AUTO, RSE::SHADOW_CACHE_INVALIDATION_ALWAYS, RSE::SHADOW_CACHE_INVALIDATION_RIGID, RSE::SHADOW_CACHE_INVALIDATION_STATIC };
	const uint32_t record_modes[4] = { VMS::RECORD_VSM_CACHE_AUTO, VMS::RECORD_VSM_CACHE_ALWAYS, VMS::RECORD_VSM_CACHE_RIGID, VMS::RECORD_VSM_CACHE_STATIC };
	for (uint32_t i = 0; i < 4; i++) {
		CHECK(((uint32_t(modes[i]) << VMS::RECORD_VSM_CACHE_MODE_SHIFT) & VMS::RECORD_VSM_CACHE_MODE_MASK) == record_modes[i]);
	}

	const uint32_t valid = VMS::RECORD_VALID | VMS::RECORD_VISIBLE;
	const uint32_t hook = NEXTGEO_VSM_CULL_FLAG_DYNAMIC_HOOK;
	// ALWAYS: layer 1 even when nothing about it is dynamic. RIGID and STATIC: layer 0 even when moved this
	// frame, skinned, with a vertex-offset bin or flagged dynamic.
	CHECK(nextgeo_vsm_instance_layer(valid | VMS::RECORD_VSM_CACHE_ALWAYS, false, hook, false) == 1u);
	const uint32_t everything = VMS::RECORD_RT_SKINNED | VMS::RECORD_VSM_DYNAMIC;
	CHECK(nextgeo_vsm_instance_layer(valid | VMS::RECORD_VSM_CACHE_RIGID | everything, true, hook, true) == 0u);
	CHECK(nextgeo_vsm_instance_layer(valid | VMS::RECORD_VSM_CACHE_STATIC | everything, true, hook, true) == 0u);
	// AUTO: RendererSceneCull's resolved dynamic flag (bit 14), a move this frame, skinning or a dynamic bin.
	CHECK(nextgeo_vsm_instance_layer(valid, false, hook, false) == 0u);
	CHECK(nextgeo_vsm_instance_layer(valid | VMS::RECORD_VSM_DYNAMIC, false, hook, false) == 1u);
	CHECK(nextgeo_vsm_instance_layer(valid, true, hook, false) == 1u);
	CHECK(nextgeo_vsm_instance_layer(valid | VMS::RECORD_RT_SKINNED, false, hook, false) == 1u);
	CHECK(nextgeo_vsm_instance_layer(valid, false, hook, true) == 1u);
	// Without the hook (#217's debug path) everything stays on layer 0, ALWAYS included.
	CHECK(nextgeo_vsm_instance_layer(valid | VMS::RECORD_VSM_CACHE_ALWAYS | VMS::RECORD_VSM_DYNAMIC, true, 0u, true) == 0u);
}

TEST_CASE("[VSM][SceneTree] shadow_cache_invalidation is bound and round-trips through GeometryInstance3D and RenderingServer (#212)") {
	const char *names[4] = { "AUTO", "ALWAYS", "RIGID", "STATIC" };
	List<StringName> constants;
	ClassDB::get_enum_constants("RenderingServer", "ShadowCacheInvalidation", &constants);
	CHECK(constants.size() == 4);
	List<StringName> node_constants;
	ClassDB::get_enum_constants("GeometryInstance3D", "ShadowCacheInvalidation", &node_constants);
	CHECK(node_constants.size() == 4);
	RenderingServer *rs = RenderingServer::get_singleton();
	REQUIRE(rs != nullptr);
	// A MeshInstance3D made through ClassDB: its header would declare a global Shader that another test's
	// alias shadows.
	GeometryInstance3D *node = Object::cast_to<GeometryInstance3D>(ClassDB::instantiate("MeshInstance3D"));
	REQUIRE(node != nullptr);
	CHECK(node->get_shadow_cache_invalidation() == GeometryInstance3D::SHADOW_CACHE_INVALIDATION_AUTO);
	CHECK(int(node->get("shadow_cache_invalidation")) == 0);
	for (uint32_t mode = 0; mode < 4; mode++) {
		const String name = String("SHADOW_CACHE_INVALIDATION_") + names[mode];
		bool valid = false;
		CHECK(ClassDB::get_integer_constant("RenderingServer", name, &valid) == int64_t(mode));
		CHECK(valid);
		CHECK(ClassDB::get_integer_constant_enum("RenderingServer", name) == StringName("ShadowCacheInvalidation"));
		valid = false;
		CHECK(ClassDB::get_integer_constant("GeometryInstance3D", name, &valid) == int64_t(mode));
		CHECK(valid);
		Callable::CallError error;
		const Variant instance = node->get_instance();
		const Variant value = int64_t(mode);
		const Variant *args[2] = { &instance, &value };
		rs->callp("instance_geometry_set_shadow_cache_invalidation", args, 2, error);
		CHECK(error.error == Callable::CallError::CALL_OK);
		node->set("shadow_cache_invalidation", int64_t(mode));
		CHECK(int(node->get_shadow_cache_invalidation()) == int(mode));
		CHECK(int(node->get("shadow_cache_invalidation")) == int(mode));
	}
	memdelete(node);
}

TEST_CASE("[VSM] Translucent quality setting default, clamping and coarse-miss defines (#215)") {
	using VSM = RendererVirtualShadowMapStorage;
	CHECK(VSM::DEFAULT_TRANSLUCENT_QUALITY == VSM::TRANSLUCENT_QUALITY_COARSE);
	CHECK(uint32_t(VSM::TRANSLUCENT_QUALITY_COARSE) == 0u);
	CHECK(uint32_t(VSM::TRANSLUCENT_QUALITY_SMRT) == 1u);
	CHECK(VSM::clamp_translucent_quality(-1) == VSM::TRANSLUCENT_QUALITY_COARSE);
	CHECK(VSM::clamp_translucent_quality(0) == VSM::TRANSLUCENT_QUALITY_COARSE);
	CHECK(VSM::clamp_translucent_quality(1) == VSM::TRANSLUCENT_QUALITY_SMRT);
	CHECK(VSM::clamp_translucent_quality(2) == VSM::TRANSLUCENT_QUALITY_COARSE);
	CHECK(VSM::clamp_translucent_quality(999) == VSM::TRANSLUCENT_QUALITY_COARSE);

	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (settings != nullptr) {
		const StringName key = VSM::SETTING_TRANSLUCENT_QUALITY;
		const bool had_key = settings->has_setting(key);
		const Variant previous = had_key ? settings->get_setting(key) : Variant();
		CHECK(VSM::read_translucent_quality() == VSM::TRANSLUCENT_QUALITY_COARSE);
		settings->set_setting(key, int64_t(VSM::TRANSLUCENT_QUALITY_SMRT));
		CHECK(VSM::read_translucent_quality() == VSM::TRANSLUCENT_QUALITY_SMRT);
		settings->set_setting(key, int64_t(7));
		CHECK(VSM::read_translucent_quality() == VSM::TRANSLUCENT_QUALITY_COARSE);
		if (had_key) {
			settings->set_setting(key, previous);
		} else {
			settings->set_setting(key, Variant());
		}
	}

	const String defines = VSM::get_glsl_defines();
	CHECK(defines.contains("#define VSM_COARSE_MISS_FOG_TOTAL 0u"));
	CHECK(defines.contains("#define VSM_COARSE_MISS_TRANSLUCENT_TOTAL 1u"));
	CHECK(defines.contains("#define VSM_COARSE_MISS_FOG_VIEW 2u"));
	CHECK(defines.contains("#define VSM_COARSE_MISS_TRANSLUCENT_VIEW 3u"));
	CHECK(defines.contains("#define VSM_COARSE_MISS_SINK 4u"));
}

TEST_CASE("[VSM] Directional coarse lookups for fog and translucency and missing-coarse-page counter (#215)") {
	using namespace TestVirtualShadowMapProjection;
	const uint32_t count = 4;
	ProjectionScene scene;
	scene.slab_half = 1.0f;
	scene.slab_height = 1.0f;
	scene.set_maps(sun_levels(count));
	// Allocate ONLY the always-resident coarse mips (CLIPMAP_RESIDENT_MIP .. MIP_COUNT - 1) on every level.
	for (uint32_t level = 0; level < count; level++) {
		for (uint32_t mip = VSM::CLIPMAP_RESIDENT_MIP; mip < VSM::MIP_COUNT; mip++) {
			scene.allocate(level, mip);
		}
	}
	scene.fill();

	const VSM::ProjectionInput in = scene.input();
	const uint32_t flags = VSM::light_flags(VSM::MAP_KIND_DIRECTIONAL, count) | VSM::LIGHT_FLAGS_SAMPLE;
	const Vector3 up(0, 1, 0);
	const float inv_fog_fade = 10.0f;
	uint32_t misses = 0;

	// Volumetric fog directional coarse lookup: exponential fade using stored - receiver_depth.
	// Slab top is at y = 1.0 (depth = -1.0); ground is y = 0.0 (depth = 0.0), so stored - receiver = -1.0.
	CHECK(VSM::fog_shadow_coarse_directional(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), 5.0f, inv_fog_fade, &misses) == doctest::Approx(std::exp(-1.0f * inv_fog_fade)).epsilon(1e-3));
	// Halfway below the slab (y = 0.8): stored - receiver = -0.2 -> exp(-2.0).
	CHECK(VSM::fog_shadow_coarse_directional(in, 0, flags, Vector3(0.0f, 0.8f, 0.0f), 5.0f, inv_fog_fade, &misses) == doctest::Approx(std::exp(-0.2f * inv_fog_fade)).epsilon(1e-3));
	// Above the slab and outside its footprint: lit (1.0).
	CHECK(VSM::fog_shadow_coarse_directional(in, 0, flags, Vector3(0.0f, 1.5f, 0.0f), 5.0f, inv_fog_fade, &misses) == doctest::Approx(1.0f));
	CHECK(VSM::fog_shadow_coarse_directional(in, 0, flags, Vector3(2.0f, 0.0f, 0.0f), 5.0f, inv_fog_fade, &misses) == doctest::Approx(1.0f));
	CHECK(misses == 0u);

	// Forward transparent directional coarse lookup: shadowed under slab, lit outside and on slab top.
	CHECK(VSM::shadow_coarse_directional(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), up, 5.0f, &misses) == doctest::Approx(0.0f));
	CHECK(VSM::shadow_coarse_directional(in, 0, flags, Vector3(2.0f, 0.0f, 0.0f), up, 5.0f, &misses) == doctest::Approx(1.0f));
	CHECK(VSM::shadow_coarse_directional(in, 0, flags, Vector3(0.0f, 1.0f, 0.0f), up, 5.0f, &misses) == doctest::Approx(1.0f));
	CHECK(misses == 0u);

	// Last clipmap level far strip: shift the last level's origin so its far edge falls outside mip 4's window
	// (only in-window at mip < CLIPMAP_RESIDENT_MIP).
	{
		ProjectionScene far_scene;
		far_scene.slab_half = 40.0f;
		far_scene.slab_height = 2.0f;
		const Vector2i far_origin(-33, -33); // -33 mod 16 == 15 -> mip 4 window covers window pages [0, 49).
		LocalVector<VSM::MapData> far_maps;
		far_maps.push_back(VSM::map_data_clipmap(down_sun(), 0, far_origin, 100.0f));
		far_scene.set_maps(far_maps);
		// Window page 60 has (15 + 60) >= 64 -> outside mip 4 (and mip 3), inside mip 2.
		const float far_x = float((far_origin.x + 60) * int32_t(VSM::PAGE_SIZE) + 32) * std::ldexp(1.0f, VSM::CLIPMAP_TEXEL0_EXPONENT);
		const Vector3 far_pt(far_x, 0.0f, far_x);
		const Vector2i texel0 = VSM::clipmap_texel0(far_scene.maps[0], far_pt);
		REQUIRE(VSM::clipmap_in_window(far_origin, texel0, 0));
		REQUIRE_FALSE(VSM::clipmap_in_window(far_origin, texel0, VSM::CLIPMAP_RESIDENT_MIP));
		uint32_t resident = 0;
		while (resident + 1 < VSM::CLIPMAP_RESIDENT_MIP && VSM::clipmap_in_window(far_origin, texel0, resident + 1)) {
			resident++;
		}
		// Unmapped far strip inside domain -> returns 1.0 and increments missing-coarse-page counter.
		uint32_t far_misses = 0;
		CHECK(VSM::fog_shadow_coarse_directional(far_scene.input(), 0, VSM::light_flags(VSM::MAP_KIND_DIRECTIONAL, 1), far_pt, 10.0f, inv_fog_fade, &far_misses) == doctest::Approx(1.0f));
		CHECK(far_misses == 1u);
		// Allocate the far-strip resident mip -> finds it, shadows, and records 0 new misses.
		far_scene.allocate(0, resident);
		far_scene.fill();
		uint32_t found_level = 99;
		bool in_domain = false;
		const VSM::LookupResult far_lookup = VSM::clipmap_coarse_sample_level(far_scene.table.ptr(), 1, far_scene.maps.ptr(), 0, 1, texel0, found_level, in_domain);
		CHECK(far_lookup.valid);
		CHECK(in_domain);
		CHECK(found_level == 0u);
		CHECK(far_lookup.mip == resident);
		CHECK(VSM::shadow_coarse_directional(far_scene.input(), 0, VSM::light_flags(VSM::MAP_KIND_DIRECTIONAL, 1), far_pt, up, 10.0f, &far_misses) == doctest::Approx(0.0f));
		CHECK(far_misses == 1u);
	}

	// Empty page table: misses inside domain increment counter and return 1.0; points past max_distance or
	// outside all clipmap levels return 1.0 without incrementing the counter.
	for (uint32_t &entry : scene.table) {
		entry = 0u;
	}
	uint32_t empty_misses = 0;
	CHECK(VSM::fog_shadow_coarse_directional(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), 5.0f, inv_fog_fade, &empty_misses) == doctest::Approx(1.0f));
	CHECK(empty_misses == 1u);
	CHECK(VSM::shadow_coarse_directional(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), up, 5.0f, &empty_misses) == doctest::Approx(1.0f));
	CHECK(empty_misses == 2u);
	// Past max_distance (1000.0): no miss increment.
	CHECK(VSM::fog_shadow_coarse_directional(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), 1500.0f, inv_fog_fade, &empty_misses) == doctest::Approx(1.0f));
	CHECK(VSM::shadow_coarse_directional(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), up, 1500.0f, &empty_misses) == doctest::Approx(1.0f));
	CHECK(empty_misses == 2u);
	// Outside every clipmap level's window (level 3 extent is 32 m): no miss increment.
	CHECK(VSM::fog_shadow_coarse_directional(in, 0, flags, Vector3(500.0f, 0.0f, 500.0f), 5.0f, inv_fog_fade, &empty_misses) == doctest::Approx(1.0f));
	CHECK(VSM::shadow_coarse_directional(in, 0, flags, Vector3(500.0f, 0.0f, 500.0f), up, 5.0f, &empty_misses) == doctest::Approx(1.0f));
	CHECK(empty_misses == 2u);
}

TEST_CASE("[VSM] Local (spot, omni, area) coarse lookups for fog and translucency and missing-coarse-page counter (#215)") {
	using namespace TestVirtualShadowMapProjection;
	const float inv_fog_fade = 10.0f;
	const Vector3 up(0, 1, 0);
	const Transform3D spot_xform(Basis::looking_at(Vector3(0, -1, 0), Vector3(0, 0, -1)), Vector3(0, 4.0f, 0));

	// Spot light: allocate coarse mips (COARSE_FIRST_MIP and root MIP_COUNT - 1) only.
	{
		ProjectionScene scene;
		scene.light_position = spot_xform.origin;
		scene.slab_half = 1.0f;
		scene.slab_height = 1.0f;
		LocalVector<VSM::MapData> maps;
		maps.push_back(VSM::map_data_face(VSM::MAP_KIND_SPOT, 0, spot_xform, 12.0f, 45.0f));
		scene.set_maps(maps);
		for (uint32_t mip = VSM::COARSE_FIRST_MIP; mip < VSM::MIP_COUNT; mip++) {
			scene.allocate(0, mip);
		}
		scene.fill();

		const VSM::ProjectionInput in = scene.input();
		const uint32_t flags = VSM::light_flags(VSM::MAP_KIND_SPOT, 1) | VSM::LIGHT_FLAGS_SAMPLE;
		uint32_t misses = 0;

		// Light at y=4, slab at y=1 (stored w=3.0), receiver at y=0 (w=4.0) -> stored - w = -1.0.
		CHECK(VSM::fog_shadow_coarse_local(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), inv_fog_fade, &misses) == doctest::Approx(std::exp(-1.0f * inv_fog_fade)).epsilon(1e-3));
		CHECK(VSM::fog_shadow_coarse_local(in, 0, flags, Vector3(0.0f, 0.8f, 0.0f), inv_fog_fade, &misses) == doctest::Approx(std::exp(-0.2f * inv_fog_fade)).epsilon(1e-3));
		CHECK(VSM::fog_shadow_coarse_local(in, 0, flags, Vector3(2.2f, 0.0f, 0.0f), inv_fog_fade, &misses) == doctest::Approx(1.0f));
		CHECK(VSM::shadow_coarse_local(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), up, scene.light_position, &misses) == doctest::Approx(0.0f));
		CHECK(VSM::shadow_coarse_local(in, 0, flags, Vector3(2.2f, 0.0f, 0.0f), up, scene.light_position, &misses) == doctest::Approx(1.0f));
		CHECK(VSM::shadow_coarse_local(in, 0, flags, Vector3(0.0f, 1.0f, 0.0f), up, scene.light_position, &misses) == doctest::Approx(1.0f));
		CHECK(misses == 0u);

		// Evict COARSE_FIRST_MIP leaving only the always-resident root mip (MIP_COUNT - 1): fallback stays valid with 0 misses.
		const uint32_t side = VSM::PAGES_PER_SIDE >> VSM::COARSE_FIRST_MIP;
		for (uint32_t y = 0; y < side; y++) {
			for (uint32_t x = 0; x < side; x++) {
				scene.table[VSM::entry_index(0, VSM::COARSE_FIRST_MIP, x, y)] = 0u;
			}
		}
		CHECK(VSM::fog_shadow_coarse_local(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), inv_fog_fade, &misses) < 0.01f);
		CHECK(VSM::shadow_coarse_local(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), up, scene.light_position, &misses) == doctest::Approx(0.0f));
		CHECK(VSM::shadow_coarse_local(in, 0, flags, Vector3(0.0f, 1.0f, 0.0f), up, scene.light_position, &misses) == doctest::Approx(1.0f));
		CHECK(misses == 0u);

		// Evict root mip too -> inside face returns 1.0 and increments missing-coarse-page counter; outside cone does not.
		scene.table[VSM::entry_index(0, VSM::MIP_COUNT - 1, 0, 0)] = 0u;
		CHECK(VSM::fog_shadow_coarse_local(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), inv_fog_fade, &misses) == doctest::Approx(1.0f));
		CHECK(misses == 1u);
		CHECK(VSM::shadow_coarse_local(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), up, scene.light_position, &misses) == doctest::Approx(1.0f));
		CHECK(misses == 2u);
		// Outside the 45-degree spot cone or behind the light: MAP_NONE -> 1.0 with no miss increment.
		CHECK(VSM::fog_shadow_coarse_local(in, 0, flags, Vector3(10.0f, 0.0f, 0.0f), inv_fog_fade, &misses) == doctest::Approx(1.0f));
		CHECK(VSM::shadow_coarse_local(in, 0, flags, Vector3(0.0f, 6.0f, 0.0f), up, scene.light_position, &misses) == doctest::Approx(1.0f));
		CHECK(misses == 2u);
	}

	// Omni (6 faces) and Area (5 front-hemisphere faces) coarse lookups.
	for (VSM::MapKind kind : { VSM::MAP_KIND_OMNI, VSM::MAP_KIND_AREA }) {
		const uint32_t face_count = VSM::map_kind_map_count(kind, 1);
		ProjectionScene scene;
		scene.light_position = spot_xform.origin;
		scene.slab_half = 1.0f;
		scene.slab_height = 1.0f;
		LocalVector<VSM::MapData> maps;
		for (uint32_t face = 0; face < face_count; face++) {
			maps.push_back(VSM::map_data_face(kind, face, spot_xform, 12.0f, 45.0f));
		}
		scene.set_maps(maps);
		for (uint32_t face = 0; face < face_count; face++) {
			scene.allocate(face, VSM::MIP_COUNT - 1);
		}
		scene.fill();

		const VSM::ProjectionInput in = scene.input();
		const uint32_t flags = VSM::light_flags(kind, face_count) | VSM::LIGHT_FLAGS_SAMPLE;
		uint32_t misses = 0;
		CHECK(VSM::fog_shadow_coarse_local(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), inv_fog_fade, &misses) < 0.01f);
		CHECK(VSM::fog_shadow_coarse_local(in, 0, flags, Vector3(2.2f, 0.0f, 0.0f), inv_fog_fade, &misses) == doctest::Approx(1.0f));
		CHECK(VSM::shadow_coarse_local(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), up, scene.light_position, &misses) == doctest::Approx(0.0f));
		CHECK(VSM::shadow_coarse_local(in, 0, flags, Vector3(2.2f, 0.0f, 0.0f), up, scene.light_position, &misses) == doctest::Approx(1.0f));
		CHECK(misses == 0u);
		if (kind == VSM::MAP_KIND_AREA) {
			// Behind the area light's front hemisphere: MAP_NONE -> lit with 0 misses even on an empty table.
			for (uint32_t &entry : scene.table) {
				entry = 0u;
			}
			CHECK(VSM::fog_shadow_coarse_local(in, 0, flags, Vector3(0.0f, 8.0f, 0.0f), inv_fog_fade, &misses) == doctest::Approx(1.0f));
			CHECK(misses == 0u);
		}
	}
}

TEST_CASE("[VSM] Resident-mip-first resolution scales bias and SMRT reach to coarse pages (#215)") {
	using namespace TestVirtualShadowMapProjection;
	const Vector3 up(0, 1, 0);

	// Directional light with fine mips 0..3 unmapped and only coarse mip 4 resident, small footprint requesting mip 0.
	{
		const uint32_t count = 4;
		ProjectionScene scene;
		scene.slab_half = 1.0f;
		scene.slab_height = 1.0f;
		scene.set_maps(sun_levels(count));
		for (uint32_t level = 0; level < count; level++) {
			scene.allocate(level, VSM::CLIPMAP_RESIDENT_MIP);
		}
		scene.fill();

		VSM::ProjectionInput in = scene.input();
		in.footprint = 0.001f; // Requests fine mip 0, which is unmapped -> resolves to coarse mip 4 first.
		in.rays = 8;
		in.steps = 6;
		const uint32_t flags = VSM::light_flags(VSM::MAP_KIND_DIRECTIONAL, count);
		const float tan_half = VSM::directional_tan_half_angle(1.0f - Math::cos(Math::deg_to_rad(1.0f)));
		CHECK(VSM::shadow_directional(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), up, tan_half) < 0.05f);
		CHECK(VSM::shadow_directional(in, 0, flags, Vector3(2.2f, 0.0f, 0.0f), up, tan_half) > 0.95f);
		CHECK(VSM::shadow_directional(in, 0, flags, Vector3(0.0f, 1.0f, 0.0f), up, tan_half) > 0.95f);
		// Hard tap (rays = 0) also resolves to coarse mip 4 cleanly.
		in.rays = 0;
		CHECK(VSM::shadow_directional(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), up, 0.0f) == doctest::Approx(0.0f));
		CHECK(VSM::shadow_directional(in, 0, flags, Vector3(0.0f, 1.0f, 0.0f), up, 0.0f) == doctest::Approx(1.0f));
	}

	// Local spot light with fine mips 0..5 unmapped and only root mip 6 resident.
	{
		const Transform3D spot_xform(Basis::looking_at(Vector3(0, -1, 0), Vector3(0, 0, -1)), Vector3(0, 4.0f, 0));
		ProjectionScene scene;
		scene.light_position = spot_xform.origin;
		scene.slab_half = 1.0f;
		scene.slab_height = 1.0f;
		LocalVector<VSM::MapData> maps;
		maps.push_back(VSM::map_data_face(VSM::MAP_KIND_SPOT, 0, spot_xform, 12.0f, 45.0f));
		scene.set_maps(maps);
		scene.allocate(0, VSM::MIP_COUNT - 1);
		scene.fill();

		VSM::ProjectionInput in = scene.input();
		in.footprint = 0.001f; // Requests fine mip 0 -> resolves to resident root mip 6 first.
		in.rays = 8;
		in.steps = 6;
		const uint32_t flags = VSM::light_flags(VSM::MAP_KIND_SPOT, 1);
		CHECK(VSM::shadow_local(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), up, scene.light_position, 0.05f) < 0.05f);
		CHECK(VSM::shadow_local(in, 0, flags, Vector3(2.2f, 0.0f, 0.0f), up, scene.light_position, 0.05f) > 0.95f);
		CHECK(VSM::shadow_local(in, 0, flags, Vector3(0.0f, 1.0f, 0.0f), up, scene.light_position, 0.05f) > 0.95f);
	}
}

TEST_CASE("[VSM] Local light map kinds, projections and 256+ local lights bypassing MAX_UPDATE_SHADOWS (#216)") {
	using VSM = RendererVirtualShadowMapStorage;

	// Map counts per local kind: Spot 1, Omni 6 cube faces, Area 5 front-hemisphere faces.
	CHECK(VSM::map_kind_map_count(VSM::MAP_KIND_SPOT, 1) == 1u);
	CHECK(VSM::map_kind_map_count(VSM::MAP_KIND_OMNI, 1) == 6u);
	CHECK(VSM::map_kind_map_count(VSM::MAP_KIND_AREA, 1) == 5u);

	const Transform3D xform(Basis(), Vector3(2.0f, 3.0f, -1.0f));

	// Spot: 1 perspective map along -Z.
	{
		const VSM::MapData spot_map = VSM::map_data_face(VSM::MAP_KIND_SPOT, 0, xform, 12.0f, 45.0f);
		CHECK(spot_map.kind == VSM::MAP_KIND_SPOT);
		CHECK(spot_map.max_distance == doctest::Approx(12.0f));
		float w = 0.0f;
		uint32_t tx = 0;
		uint32_t ty = 0;
		CHECK(VSM::face_texel(spot_map, xform.origin + Vector3(0, 0, -4.0f), w, tx, ty));
		CHECK(w == doctest::Approx(4.0f));
		CHECK(tx == VSM::VIRTUAL_SIZE / 2u);
		CHECK(ty == VSM::VIRTUAL_SIZE / 2u);
		CHECK_FALSE(VSM::face_texel(spot_map, xform.origin + Vector3(0, 0, 4.0f), w, tx, ty));
	}

	// Omni: 6 cube faces covering all 6 cardinal directions.
	{
		VSM::MapData omni_maps[6];
		for (uint32_t f = 0; f < 6; f++) {
			omni_maps[f] = VSM::map_data_face(VSM::MAP_KIND_OMNI, f, xform, 10.0f, 45.0f);
			CHECK(omni_maps[f].kind == VSM::MAP_KIND_OMNI);
			CHECK(omni_maps[f].index == f);
		}
		const uint32_t omni_flags = VSM::light_flags(VSM::MAP_KIND_OMNI, 6);
		const Vector3 dirs[6] = { Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 1, 0), Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(0, 0, -1) };
		for (uint32_t f = 0; f < 6; f++) {
			float w = 0.0f;
			const uint32_t selected = VSM::face_projection_select(omni_maps, 6, 0, omni_flags, xform.origin + dirs[f] * 3.0f, w);
			CHECK(selected == f);
			CHECK(w == doctest::Approx(3.0f));
		}
	}

	// Area: 5 front-hemisphere faces (+X, -X, +Y, -Y, -Z), excluding back hemisphere (+Z), and supporting
	// area reach (range + half diagonal).
	{
		const Vector2 area_size(2.0f, 2.0f);
		const float range = 8.0f;
		const float reach = range + float(area_size.length()) * 0.5f;
		VSM::MapData area_maps[5];
		for (uint32_t f = 0; f < 5; f++) {
			area_maps[f] = VSM::map_data_face(VSM::MAP_KIND_AREA, f, xform, reach, 45.0f);
			CHECK(area_maps[f].kind == VSM::MAP_KIND_AREA);
			CHECK(area_maps[f].max_distance == doctest::Approx(reach));
		}
		const uint32_t area_flags = VSM::light_flags(VSM::MAP_KIND_AREA, 5);
		const Vector3 front_dirs[5] = {
			Vector3(3.0f, 0.0f, -0.5f),
			Vector3(-3.0f, 0.0f, -0.5f),
			Vector3(0.0f, 3.0f, -0.5f),
			Vector3(0.0f, -3.0f, -0.5f),
			Vector3(0.0f, 0.0f, -3.0f),
		};
		for (uint32_t f = 0; f < 5; f++) {
			float w = 0.0f;
			CHECK(VSM::face_projection_select(area_maps, 5, 0, area_flags, xform.origin + front_dirs[f], w) == f);
		}
		// Strictly behind the area light (+Z): none of the 5 front-hemisphere faces hold it.
		float back_w = 0.0f;
		CHECK(VSM::face_projection_select(area_maps, 5, 0, area_flags, xform.origin + Vector3(0.0f, 0.0f, 4.0f), back_w) == VSM::MAP_NONE);
	}

	// 256 mixed Spot/Omni/Area lights (86 + 85 + 85 = 1021 maps) and >512 local lights bypass the 88-slot
	// positional atlas and MAX_UPDATE_SHADOWS = 512 with zero allocation failures.
	{
		TestVSMStorage storage;
		storage.set_frame(1);
		uint32_t expected_maps = 0;
		for (uint32_t i = 0; i < 256; i++) {
			const VSM::MapKind kind = (i % 3u == 0u) ? VSM::MAP_KIND_SPOT : ((i % 3u == 1u) ? VSM::MAP_KIND_OMNI : VSM::MAP_KIND_AREA);
			const uint32_t need = VSM::map_kind_map_count(kind, 1);
			CHECK(storage.can_allocate_maps(need));
			const uint32_t first = storage.map_allocate(light_rid(1000 + i), kind);
			CHECK(first != VSM::MAP_NONE);
			const VSM::MapRange *range = storage.map_get(light_rid(1000 + i));
			REQUIRE(range != nullptr);
			CHECK(range->first == first);
			CHECK(range->count == need);
			CHECK(range->kind == kind);
			expected_maps += need;
		}
		CHECK(storage.get_light_count() == 256u);
		CHECK(storage.get_allocated_map_count() == expected_maps);
		CHECK(expected_maps == 86u * 1u + 85u * 6u + 85u * 5u);
		CHECK(storage.get_failed_allocation_count() == 0u);

		// Push beyond MAX_UPDATE_SHADOWS (512) with 300 additional mixed local lights (556 total).
		for (uint32_t i = 256; i < 556; i++) {
			const VSM::MapKind kind = (i % 3u == 0u) ? VSM::MAP_KIND_SPOT : ((i % 3u == 1u) ? VSM::MAP_KIND_OMNI : VSM::MAP_KIND_AREA);
			const uint32_t need = VSM::map_kind_map_count(kind, 1);
			CHECK(storage.can_allocate_maps(need));
			CHECK(storage.map_allocate(light_rid(1000 + i), kind) != VSM::MAP_NONE);
		}
		CHECK(storage.get_light_count() == 556u);
		CHECK(storage.get_failed_allocation_count() == 0u);
	}

	// can_allocate_maps() returns false when TABLE_MAX_MAPS is full of active lights and true once stale.
	{
		TestVSMStorage storage;
		storage.set_frame(1);
		uint64_t next_id = 1;
		while (storage.get_allocated_map_count() + 6u <= VSM::TABLE_MAX_MAPS) {
			CHECK(storage.map_allocate(light_rid(next_id++), VSM::MAP_KIND_OMNI) != VSM::MAP_NONE);
		}
		// Remaining tail < 6 maps -> cannot fit another 6-map omni light in the same frame.
		CHECK_FALSE(storage.can_allocate_maps(6u));
		// After MAP_EVICT_AFTER_FRAMES frames, stale lights can be evicted so can_allocate_maps(6) is true.
		storage.set_frame(1 + VSM::MAP_EVICT_AFTER_FRAMES);
		CHECK(storage.can_allocate_maps(6u));
	}
}

TEST_CASE("[VSM] Per-light shadow_resolution_bias packing, clamping and mip selection (#216)") {
	using VSM = RendererVirtualShadowMapStorage;

	// Clamping including non-finite values.
	CHECK(VSM::clamp_resolution_bias(-10.0f) == doctest::Approx(VSM::RESOLUTION_BIAS_MIN));
	CHECK(VSM::clamp_resolution_bias(10.0f) == doctest::Approx(VSM::RESOLUTION_BIAS_MAX));
	CHECK(VSM::clamp_resolution_bias(-float(Math::INF)) == doctest::Approx(VSM::RESOLUTION_BIAS_MIN));
	CHECK(VSM::clamp_resolution_bias(float(Math::INF)) == doctest::Approx(VSM::RESOLUTION_BIAS_MAX));
	CHECK(VSM::clamp_resolution_bias(std::nanf("")) == doctest::Approx(0.0f));
	CHECK(VSM::clamp_resolution_bias(0.0f) == doctest::Approx(0.0f));
	CHECK(VSM::clamp_resolution_bias(-1.5f) == doctest::Approx(-1.5f));
	CHECK(VSM::clamp_resolution_bias(2.75f) == doctest::Approx(2.75f));

	// Zero bias encodes to 0 so legacy vsm_flags are completely unchanged.
	CHECK(VSM::pack_resolution_bias(0.0f) == 0u);
	CHECK(VSM::unpack_resolution_bias(0u) == doctest::Approx(0.0f));

	// Round-trip every 1/16 step across [-2.0, +4.0] alongside all other vsm_flags bits.
	const uint32_t base_flags = VSM::light_flags(VSM::MAP_KIND_OMNI, 6) | VSM::LIGHT_FLAGS_SAMPLE | VSM::LIGHT_FLAGS_DISTANT_COARSE | VSM::df_pack_flags(5, true);
	for (int32_t q = -32; q <= 64; q++) {
		const float bias = float(q) / VSM::RESOLUTION_BIAS_SCALE;
		const uint32_t packed = base_flags | VSM::pack_resolution_bias(bias);
		CHECK(VSM::unpack_resolution_bias(packed) == doctest::Approx(bias));
		CHECK(VSM::light_map(120, packed, 3) == 123u);
		CHECK(VSM::light_map(120, packed, 6) == VSM::MAP_NONE);
		CHECK(VSM::df_layer(packed) == 5u);
		CHECK((packed & VSM::LIGHT_FLAGS_SAMPLE) != 0u);
		CHECK((packed & VSM::LIGHT_FLAGS_DISTANT_COARSE) != 0u);
	}

	// resolution_bias_parts(): exact integer floor and [1, 2) footprint scale.
	{
		int32_t ib = 0;
		float scale = 0.0f;
		VSM::resolution_bias_parts(VSM::pack_resolution_bias(0.0f), ib, scale);
		CHECK(ib == 0);
		CHECK(scale == 1.0f);

		VSM::resolution_bias_parts(VSM::pack_resolution_bias(-2.0f), ib, scale);
		CHECK(ib == -2);
		CHECK(scale == 1.0f);

		VSM::resolution_bias_parts(VSM::pack_resolution_bias(-0.5f), ib, scale);
		CHECK(ib == -1);
		CHECK(scale == doctest::Approx(std::sqrt(2.0f)));

		VSM::resolution_bias_parts(VSM::pack_resolution_bias(0.5f), ib, scale);
		CHECK(ib == 0);
		CHECK(scale == doctest::Approx(std::sqrt(2.0f)));

		VSM::resolution_bias_parts(VSM::pack_resolution_bias(3.25f), ib, scale);
		CHECK(ib == 3);
		CHECK(scale == doctest::Approx(std::exp2(0.25f)));
	}

	// mark_local_point() shifts requested mip monotonically with shadow_resolution_bias.
	{
		const Transform3D spot_xform(Basis::looking_at(Vector3(0, -1, 0), Vector3(0, 0, -1)), Vector3(0, 4.0f, 0));
		const VSM::MapData map = VSM::map_data_face(VSM::MAP_KIND_SPOT, 0, spot_xform, 12.0f, 45.0f);
		const Vector3 world(0.0f, 0.0f, 0.0f);
		// Pick a footprint equal to 4.5x mip-0 texel at w=4 so bias=0 selects mip 2 cleanly.
		const float footprint = map.texel_scale * 4.0f * 4.5f;
		auto requested_mip = [&](float p_bias) -> uint32_t {
			VSM::MarkLight light;
			light.kind = VSM::MAP_KIND_SPOT;
			light.map_base = 0;
			light.flags = VSM::light_flags(VSM::MAP_KIND_SPOT, 1) | VSM::pack_resolution_bias(p_bias);
			LocalVector<uint64_t> reqs;
			VSM::mark_local_point(&map, 1, nullptr, light, world, footprint, 0, false, false, reqs);
			REQUIRE(reqs.size() == 1u);
			const uint32_t local_idx = uint32_t(reqs[0]) % VSM::ENTRIES_PER_MAP;
			for (uint32_t m = 0; m < VSM::MIP_COUNT; m++) {
				if (local_idx < VSM::mip_entry_offset(m + 1)) {
					return m;
				}
			}
			return VSM::MIP_COUNT;
		};
		CHECK(requested_mip(-2.0f) == 0u);
		CHECK(requested_mip(-1.0f) == 1u);
		CHECK(requested_mip(0.0f) == 2u);
		CHECK(requested_mip(1.0f) == 3u);
		CHECK(requested_mip(2.0f) == 4u);
		CHECK(requested_mip(4.0f) == 6u);
		// Fractional +0.5 pushes 4.5 * sqrt(2) = 6.36 < 8 -> mip 2; +0.875 pushes 4.5 * 2^0.875 = 8.25 >= 8 -> mip 3.
		CHECK(requested_mip(0.5f) == 2u);
		CHECK(requested_mip(0.875f) == 3u);
	}

	// Directional light: mark_directional_point() and clipmap_projection_select() agree across biases.
	{
		using namespace TestVirtualShadowMapProjection;
		const LocalVector<VSM::MapData> levels = sun_levels(6);
		const Vector3 world(0.3f, 0.0f, -0.4f);
		const Vector2i texel0 = VSM::clipmap_texel0(levels[0], world);
		const float footprint = 0.01f;
		uint32_t prev_total = 0;
		const float biases[] = { -2.0f, -0.5f, 0.0f, 1.5f, 3.0f, 4.0f };
		for (uint32_t i = 0; i < sizeof(biases) / sizeof(biases[0]); i++) {
			const uint32_t flags = VSM::light_flags(VSM::MAP_KIND_DIRECTIONAL, levels.size()) | VSM::pack_resolution_bias(biases[i]);
			uint32_t sel_level = 0;
			uint32_t sel_mip = 0;
			REQUIRE(VSM::clipmap_projection_select(levels.ptr(), levels.size(), nullptr, 0, levels.size(), texel0, footprint, 0, sel_level, sel_mip, flags));
			LocalVector<uint64_t> reqs;
			VSM::mark_directional_point(levels.ptr(), levels.size(), nullptr, 0, flags, world, footprint, 0, false, reqs);
			REQUIRE(reqs.size() >= 1u);
			const uint32_t req_map = uint32_t(reqs[0]) / VSM::ENTRIES_PER_MAP;
			const uint32_t req_local = uint32_t(reqs[0]) % VSM::ENTRIES_PER_MAP;
			uint32_t req_mip = VSM::MIP_COUNT;
			for (uint32_t m = 0; m < VSM::MIP_COUNT; m++) {
				if (req_local < VSM::mip_entry_offset(m + 1)) {
					req_mip = m;
					break;
				}
			}
			CHECK(req_map == sel_level);
			CHECK(req_mip == sel_mip);
			const uint32_t total = sel_level + sel_mip;
			if (i > 0) {
				CHECK(total >= prev_total);
			}
			prev_total = total;
		}
		CHECK(prev_total > 0u);
	}

	// GLSL defines expose the #216 flag constants.
	const String defines = VSM::get_glsl_defines();
	CHECK(defines.contains("VSM_LIGHT_FLAGS_DISTANT_COARSE"));
	CHECK(defines.contains("VSM_LIGHT_FLAGS_RES_BIAS_SHIFT"));
	CHECK(defines.contains("VSM_LIGHT_FLAGS_RES_BIAS_MASK"));
}

TEST_CASE("[VSM][SceneTree] Light3D.shadow_resolution_bias and LIGHT_PARAM_SHADOW_RESOLUTION_BIAS bind and clamp (#216)") {
	bool valid = false;
	CHECK(ClassDB::get_integer_constant("Light3D", "PARAM_SHADOW_RESOLUTION_BIAS", &valid) == int64_t(Light3D::PARAM_SHADOW_RESOLUTION_BIAS));
	CHECK(valid);
	CHECK(int64_t(Light3D::PARAM_SHADOW_RESOLUTION_BIAS) == int64_t(RSE::LIGHT_PARAM_SHADOW_RESOLUTION_BIAS));
	CHECK(ClassDB::get_integer_constant("RenderingServer", "LIGHT_PARAM_SHADOW_RESOLUTION_BIAS", &valid) == int64_t(RSE::LIGHT_PARAM_SHADOW_RESOLUTION_BIAS));
	CHECK(valid);

	Light3D *lights[4] = {
		memnew(DirectionalLight3D),
		memnew(OmniLight3D),
		memnew(SpotLight3D),
		memnew(AreaLight3D),
	};
	for (Light3D *light : lights) {
		CHECK(light->get_param(Light3D::PARAM_SHADOW_RESOLUTION_BIAS) == doctest::Approx(0.0f));
		CHECK(float(light->get("shadow_resolution_bias")) == doctest::Approx(0.0f));
		light->set("shadow_resolution_bias", 1.75f);
		CHECK(light->get_param(Light3D::PARAM_SHADOW_RESOLUTION_BIAS) == doctest::Approx(1.75f));
		light->set_param(Light3D::PARAM_SHADOW_RESOLUTION_BIAS, -9.0f);
		CHECK(light->get_param(Light3D::PARAM_SHADOW_RESOLUTION_BIAS) == doctest::Approx(-2.0f));
		light->set_param(Light3D::PARAM_SHADOW_RESOLUTION_BIAS, 9.0f);
		CHECK(light->get_param(Light3D::PARAM_SHADOW_RESOLUTION_BIAS) == doctest::Approx(4.0f));
		memdelete(light);
	}
}

TEST_CASE("[VSM] Distant-light screen-radius policy, coarse-only marking and 25% pool overflow safety (#216)") {
	using VSM = RendererVirtualShadowMapStorage;

	// Preset defaults decrease strictly from Low to Epic and clamp cleanly.
	const VSM::Scalability low = VSM::default_scalability(VSM::POOL_QUALITY_LOW);
	const VSM::Scalability med = VSM::default_scalability(VSM::POOL_QUALITY_MEDIUM);
	const VSM::Scalability high = VSM::default_scalability(VSM::POOL_QUALITY_HIGH);
	const VSM::Scalability epic = VSM::default_scalability(VSM::POOL_QUALITY_EPIC);
	CHECK(low.distant_light_screen_radius > med.distant_light_screen_radius);
	CHECK(med.distant_light_screen_radius > high.distant_light_screen_radius);
	CHECK(high.distant_light_screen_radius > epic.distant_light_screen_radius);
	CHECK(epic.distant_light_screen_radius > 0.0f);
	{
		VSM::Scalability s = high;
		s.distant_light_screen_radius = -1.0f;
		CHECK(VSM::clamp_scalability(s).distant_light_screen_radius == 0.0f);
		s.distant_light_screen_radius = 5.0f;
		CHECK(VSM::clamp_scalability(s).distant_light_screen_radius == 1.0f);
		s.distant_light_screen_radius = std::nanf("");
		CHECK(VSM::clamp_scalability(s).distant_light_screen_radius == 0.0f);
	}

	// Projected screen radius (perspective, camera-inside, orthographic, and disabled threshold).
	Projection persp;
	persp.set_perspective(90.0f, 1.0f, 0.1f, 500.0f, false); // P[0][0] == P[1][1] == 1.0
	CHECK(VSM::local_light_projected_radius(Vector3(0.0f, 0.0f, -100.0f), 2.0f, persp) == doctest::Approx(0.01f));
	CHECK(VSM::is_local_light_distant(VSM::local_light_projected_radius(Vector3(0.0f, 0.0f, -100.0f), 2.0f, persp), high.distant_light_screen_radius));
	CHECK_FALSE(VSM::is_local_light_distant(VSM::local_light_projected_radius(Vector3(0.0f, 0.0f, -10.0f), 2.0f, persp), low.distant_light_screen_radius));
	// Camera inside or on sphere -> 1.0 (never demoted).
	CHECK(VSM::local_light_projected_radius(Vector3(0.0f, 0.0f, -1.5f), 2.0f, persp) == doctest::Approx(1.0f));
	CHECK(VSM::local_light_projected_radius(Vector3(0.0f, 0.0f, -10.0f), 0.0f, persp) == 0.0f);
	// Threshold 0 disables distant demotion.
	CHECK_FALSE(VSM::is_local_light_distant(0.001f, 0.0f));
	// Orthographic projection.
	Projection ortho;
	ortho.set_orthogonal(-10.0f, 10.0f, -10.0f, 10.0f, 0.1f, 100.0f); // P[0][0] = P[1][1] = 0.1
	CHECK(VSM::local_light_projected_radius(Vector3(0.0f, 0.0f, -50.0f), 2.0f, ortho) == doctest::Approx(0.1f));

	// LIGHT_FLAGS_DISTANT_COARSE clamps fine marking requests to >= COARSE_FIRST_MIP and flags REQUEST_COARSE,
	// even when shadow_resolution_bias is -2.0.
	{
		const Transform3D spot_xform(Basis::looking_at(Vector3(0, -1, 0), Vector3(0, 0, -1)), Vector3(0, 4.0f, 0));
		const VSM::MapData map = VSM::map_data_face(VSM::MAP_KIND_SPOT, 0, spot_xform, 12.0f, 45.0f);
		VSM::MarkLight light;
		light.kind = VSM::MAP_KIND_SPOT;
		light.map_base = 0;
		light.flags = VSM::light_flags(VSM::MAP_KIND_SPOT, 1) | VSM::LIGHT_FLAGS_DISTANT_COARSE | VSM::pack_resolution_bias(-2.0f);
		LocalVector<uint64_t> reqs;
		VSM::mark_local_point(&map, 1, nullptr, light, Vector3(0.0f, 0.0f, 0.0f), 0.0005f, 0, false, false, reqs);
		REQUIRE(reqs.size() == 1u);
		CHECK((reqs[0] & VSM::REQUEST_COARSE) != 0u);
		const uint32_t local_idx = uint32_t(reqs[0]) % VSM::ENTRIES_PER_MAP;
		CHECK(local_idx >= VSM::mip_entry_offset(VSM::COARSE_FIRST_MIP));
		CHECK(local_idx < VSM::mip_entry_offset(VSM::COARSE_FIRST_MIP + 1));
	}

	// 25% pool overflow (2048 pages) with 256 mixed local lights (1021 maps): Phase-0 root pages stay resident,
	// coarse requests come before fine, overflow hits fine pages only, and 0 samples are unmapped.
	{
		const uint32_t pool_pages = VSM::pool_quality_pages(VSM::POOL_QUALITY_HIGH) / 4u; // 2048 pages (25%)
		const uint32_t grid_width = uint32_t(VSM::pool_page_grid(pool_pages).x);
		LocalVector<VSM::MapData> maps;
		LocalVector<uint32_t> light_bases;
		LocalVector<uint32_t> light_flags_list;
		for (uint32_t i = 0; i < 256; i++) {
			const VSM::MapKind kind = (i % 3u == 0u) ? VSM::MAP_KIND_SPOT : ((i % 3u == 1u) ? VSM::MAP_KIND_OMNI : VSM::MAP_KIND_AREA);
			const uint32_t count = VSM::map_kind_map_count(kind, 1);
			const uint32_t base = maps.size();
			light_bases.push_back(base);
			const bool distant = (i >= 192);
			light_flags_list.push_back(VSM::light_flags(kind, count) | (distant ? VSM::LIGHT_FLAGS_DISTANT_COARSE : 0u));
			const Transform3D xf(Basis::looking_at(Vector3(0, -1, 0), Vector3(0, 0, -1)), Vector3(float(i % 16) * 2.0f, 4.0f, float(i / 16) * 2.0f));
			for (uint32_t f = 0; f < count; f++) {
				maps.push_back(VSM::map_data_face(kind, f, xf, 8.0f, 45.0f));
			}
		}
		const uint32_t table_maps = maps.size();
		REQUIRE(table_maps == 1021u);
		const uint64_t table_entries = uint64_t(table_maps) * VSM::ENTRIES_PER_MAP;
		LocalVector<uint32_t> table;
		table.resize(table_entries);
		memset(table.ptr(), 0, table_entries * sizeof(uint32_t));
		LocalVector<VSM::PageRecord> pages;
		pages.resize(pool_pages);

		LocalVector<uint32_t> request_list;
		auto add_request = [&](uint32_t p_idx, bool p_coarse) {
			const uint32_t prev = table[p_idx];
			table[p_idx] = prev | VSM::ENTRY_REQUESTED | (p_coarse ? VSM::ENTRY_COARSE : 0u);
			if ((prev & VSM::ENTRY_REQUESTED) == 0u) {
				request_list.push_back(p_idx);
			}
		};
		// Phase 0: always-resident root pages of every face of all 256 lights (1021 requests).
		LocalVector<uint64_t> res_reqs;
		for (uint32_t i = 0; i < 256; i++) {
			VSM::mark_resident(maps.ptr(), table_maps, light_bases[i], light_flags_list[i], res_reqs);
		}
		for (uint64_t r : res_reqs) {
			add_request(uint32_t(r), true);
		}
		const uint32_t resident_count = request_list.size();
		CHECK(resident_count == table_maps);
		// Phase 1 (distant coarse) + Phase 2 (fine mip-0 requests exceeding pool capacity).
		for (uint32_t m = 0; m < table_maps; m++) {
			// Request 4 fine mip-0 pages per map -> 4084 fine + 1021 resident = 5105 > 2048 pages.
			for (uint32_t k = 0; k < 4; k++) {
				add_request(VSM::entry_index(m, 0, k, k), false);
			}
		}

		const VSM::AllocationCounts counts = VSM::allocate_model(table.ptr(), table_entries, pages.ptr(), pool_pages, grid_width, request_list.ptr(), request_list.size(), 1, 60, resident_count);
		CHECK(counts.overflow > 0u);
		CHECK(counts.resident_overflow == 0u);
		// Every fine lookup across every map of all 256 lights falls back cleanly to an allocated page -> 0 unmapped.
		uint32_t unmapped = 0;
		for (uint32_t m = 0; m < table_maps; m++) {
			const VSM::LookupResult lr = VSM::lookup(table.ptr(), table_maps, m, Vector2(0.5f, 0.5f), 0, true);
			if (!lr.valid) {
				unmapped++;
			}
		}
		CHECK(unmapped == 0u);
	}
}

TEST_CASE("[VSM] Subsurface transmittance VSM depth lookup matches analytic thickness (#216)") {
	using VSM = RendererVirtualShadowMapStorage;
	using namespace TestVirtualShadowMapProjection;

	// Directional light: slab at y=1, receiver at y=0 -> thickness = 1.0 m; front face y=1.02 -> small positive abs;
	// outside slab -> -1.0 sentinel.
	{
		ProjectionScene scene;
		scene.slab_half = 1.5f;
		scene.slab_height = 1.0f;
		scene.set_maps(sun_levels(4));
		for (uint32_t l = 0; l < 4; l++) {
			scene.allocate(l, 0);
			scene.allocate(l, VSM::CLIPMAP_RESIDENT_MIP);
		}
		scene.fill();
		const VSM::ProjectionInput in = scene.input();
		const uint32_t flags = VSM::light_flags(VSM::MAP_KIND_DIRECTIONAL, 4) | VSM::LIGHT_FLAGS_SAMPLE;

		CHECK(VSM::transmittance_directional(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), 4.0f, false) == doctest::Approx(1.0f).epsilon(0.02));
		CHECK(VSM::transmittance_directional(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), 4.0f, true) == doctest::Approx(1.0f).epsilon(0.05));
		// Front face slightly toward light: small negative raw delta returns abs >= 0 (never collides with -1 sentinel).
		const float front_t = VSM::transmittance_directional(in, 0, flags, Vector3(0.0f, 1.02f, 0.0f), 4.0f, false);
		CHECK(front_t >= 0.0f);
		CHECK(front_t == doctest::Approx(0.02f).epsilon(0.01));
		// Lit ground outside slab: receiver sits on the ground plane stored in VSM -> thickness ~ 0.0.
		CHECK(VSM::transmittance_directional(in, 0, flags, Vector3(5.0f, 0.0f, 0.0f), 4.0f, false) == doctest::Approx(0.0f).epsilon(0.01));
		// Empty texel (cleared pool) returns -1.0 sentinel so caller keeps transmittance_depth.
		VSM::ProjectionInput empty_in = in;
		LocalVector<uint32_t> empty_pool;
		empty_pool.resize(scene.pool.size());
		memset(empty_pool.ptr(), 0, empty_pool.size() * sizeof(uint32_t));
		empty_in.pool = empty_pool.ptr();
		CHECK(VSM::transmittance_directional(empty_in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), 4.0f, false) == doctest::Approx(-1.0f));
	}

	// Spot light: axial thickness along spot -Z.
	{
		const Transform3D spot_xform(Basis::looking_at(Vector3(0, -1, 0), Vector3(0, 0, -1)), Vector3(0, 4.0f, 0));
		ProjectionScene scene;
		scene.light_position = spot_xform.origin;
		scene.slab_half = 3.0f;
		scene.slab_height = 1.0f;
		LocalVector<VSM::MapData> maps;
		maps.push_back(VSM::map_data_face(VSM::MAP_KIND_SPOT, 0, spot_xform, 12.0f, 55.0f));
		scene.set_maps(maps);
		scene.allocate(0, 2);
		scene.allocate(0, VSM::COARSE_FIRST_MIP);
		scene.fill();

		const VSM::ProjectionInput in = scene.input();
		const uint32_t flags = VSM::light_flags(VSM::MAP_KIND_SPOT, 1) | VSM::LIGHT_FLAGS_SAMPLE;
		CHECK(VSM::transmittance_local(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), false) == doctest::Approx(1.0f).epsilon(0.02));
		CHECK(VSM::transmittance_local(in, 0, flags, Vector3(1.5f, 0.0f, 0.0f), false) == doctest::Approx(1.0f).epsilon(0.02));
		CHECK(VSM::transmittance_local(in, 0, flags, Vector3(0.0f, 1.02f, 0.0f), false) == doctest::Approx(0.02f).epsilon(0.01));
		// Lit ground outside slab -> thickness ~ 0.0; point behind spot light -> -1.0 sentinel.
		CHECK(VSM::transmittance_local(in, 0, flags, Vector3(4.5f, 0.0f, 0.0f), false) == doctest::Approx(0.0f).epsilon(0.01));
		CHECK(VSM::transmittance_local(in, 0, flags, Vector3(0.0f, 6.0f, 0.0f), false) == doctest::Approx(-1.0f));
	}

	// Omni and Area lights: cube-face radial conversion sqrt(1 + x_ndc^2 + y_ndc^2) matches Euclidean radial
	// thickness of Godot's dual-paraboloid atlas even near cube-face corners.
	for (uint32_t k = 0; k < 2; k++) {
		const VSM::MapKind kind = k == 0 ? VSM::MAP_KIND_OMNI : VSM::MAP_KIND_AREA;
		const uint32_t face_count = VSM::map_kind_map_count(kind, 1);
		const Transform3D xf(Basis::looking_at(Vector3(0, -1, 0), Vector3(0, 0, -1)), Vector3(0, 4.0f, 0));
		ProjectionScene scene;
		scene.light_position = xf.origin;
		scene.slab_half = 4.0f;
		scene.slab_height = 1.0f;
		LocalVector<VSM::MapData> maps;
		for (uint32_t f = 0; f < face_count; f++) {
			maps.push_back(VSM::map_data_face(kind, f, xf, 12.0f, 45.0f));
		}
		scene.set_maps(maps);
		for (uint32_t f = 0; f < face_count; f++) {
			scene.allocate(f, 2);
			scene.allocate(f, VSM::COARSE_FIRST_MIP);
		}
		scene.fill();

		const VSM::ProjectionInput in = scene.input();
		const uint32_t flags = VSM::light_flags(kind, face_count) | VSM::LIGHT_FLAGS_SAMPLE;
		// On-axis (0, 0, 0): radial thickness == axial thickness == 1.0 m.
		CHECK(VSM::transmittance_local(in, 0, flags, Vector3(0.0f, 0.0f, 0.0f), false) == doctest::Approx(1.0f).epsilon(0.02));
		// Off-axis (2, 0, 2) from light at (0, 4, 0): x_ndc = 0.5, y_ndc = 0.5 -> exact radial thickness = sqrt(1.5).
		const float expected_radial = std::sqrt(1.5f);
		CHECK(VSM::transmittance_local(in, 0, flags, Vector3(2.0f, 0.0f, 2.0f), false) == doctest::Approx(expected_radial).epsilon(0.04));
		// Distant coarse mode still resolves cleanly through coarse mip.
		CHECK(VSM::transmittance_local(in, 0, flags | VSM::LIGHT_FLAGS_DISTANT_COARSE, Vector3(2.0f, 0.0f, 2.0f), false) == doctest::Approx(expected_radial).epsilon(0.08));
		if (kind == VSM::MAP_KIND_AREA) {
			// Behind area light front hemisphere -> -1.0.
			CHECK(VSM::transmittance_local(in, 0, flags, Vector3(0.0f, 6.0f, 0.0f), false) == doctest::Approx(-1.0f));
		}
	}
}
