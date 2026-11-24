/**************************************************************************/
/*  test_nextgi_world_cache.cpp                                           */
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

// GodotNext: nextgi_world_cache (#294, plan §21.2.1 / §26.1 / §26.2).
// Device-free unit tests covering default-off project setting and startup read, availability
// gating and one-time fallback logging for software tier / Inline HWRT / Metal, simplified
// consumer bit and get_hit_availability_for() / has_current_records() gating, SRC non-allocation
// policy, Simplified Material surface colour payload packing, sparse 8-way world-space hash
// table cascades/blending/eviction/hole safety, debug view parity, and HWRT shader variants.

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_nextgi_world_cache)

#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "scene/main/viewport.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_gather.h"
#include "servers/rendering/renderer_rd/environment/next_gi_hit_materials.h"
#include "servers/rendering/renderer_rd/environment/next_gi_world_cache.h"
#include "servers/rendering/renderer_rd/environment/next_gi_wrc.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_route.h"
#include "servers/rendering/rendering_server_enums.h"

using namespace RendererRD;

namespace TestNextGIWorldCache {

static const char *WC_SETTING = "rendering/global_illumination/nextgi/world_cache";
static const char *HWRT_MODE_SETTING = "rendering/global_illumination/nextgi/hardware_ray_tracing_mode";

struct ScopedSetting {
	String name;
	Variant previous;
	bool existed = false;

	ScopedSetting(const String &p_name, const Variant &p_value) :
			name(p_name) {
		ProjectSettings *ps = ProjectSettings::get_singleton();
		existed = ps->has_setting(p_name);
		previous = existed ? ps->get_setting(p_name) : Variant();
		ps->set_setting(p_name, p_value);
	}

	~ScopedSetting() {
		ProjectSettings *ps = ProjectSettings::get_singleton();
		if (existed) {
			ps->set_setting(name, previous);
		} else if (ps->has_setting(name)) {
			ps->clear(name);
		}
	}
};

TEST_CASE("[NextGI][WorldCache] Project setting defaults to off, requires restart, and is read at startup") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);
	REQUIRE(ps->has_setting(WC_SETTING));
	CHECK_FALSE(bool(ps->get_setting(WC_SETTING)));
	CHECK_FALSE(bool(ps->property_get_revert(WC_SETTING)));
	CHECK_FALSE(NextGI::read_world_cache());

	List<PropertyInfo> props;
	ps->get_property_list(&props);
	bool found = false;
	for (const PropertyInfo &info : props) {
		if (info.name == WC_SETTING) {
			found = true;
			CHECK_EQ(info.type, Variant::BOOL);
			CHECK((info.usage & PROPERTY_USAGE_RESTART_IF_CHANGED) != 0);
			break;
		}
	}
	CHECK(found);

	// Default-off NextGI keeps zero World Cache state and zero allocated bytes.
	{
		NextGI next_gi;
		next_gi.init(true);
		CHECK_FALSE(next_gi.is_world_cache_requested());
		CHECK_FALSE(next_gi.is_world_cache_replacing_src());
		CHECK_FALSE(next_gi.is_world_cache_active());
		CHECK(next_gi.get_world_cache() == nullptr);
		const Dictionary status = next_gi.get_status();
		CHECK_FALSE(bool(status["world_cache"]));
		CHECK_FALSE(bool(status["world_cache_active"]));
		CHECK_FALSE(bool(status["world_cache_replaces_src"]));
		CHECK_EQ(String(status["world_cache_reason"]), String("OFF"));
		const Dictionary info = next_gi.get_tier_info();
		CHECK_EQ(int64_t(info["world_cache_bytes"]), 0);
		CHECK_EQ(int64_t(info["world_cache_dispatches"]), 0);
		const Dictionary counters = next_gi.get_counters();
		CHECK_EQ(int64_t(counters["world_cache_bytes"]), 0);
		CHECK_EQ(int64_t(counters["world_cache_paths_traced"]), 0);
		CHECK_EQ(int64_t(counters["world_cache_cells_active"]), 0);
	}

	// Setting is read once at startup inside NextGI::init(); subsequent setting edits do not mutate the live flag.
	{
		ScopedSetting enable_wc(WC_SETTING, true);
		CHECK(NextGI::read_world_cache());
		NextGI next_gi;
		CHECK_FALSE(next_gi.is_world_cache_requested());
		next_gi.init(true);
		CHECK(next_gi.is_world_cache_requested());
		ps->set_setting(WC_SETTING, false);
		CHECK_FALSE(NextGI::read_world_cache());
		CHECK(next_gi.is_world_cache_requested());
	}
}

TEST_CASE("[NextGI][WorldCache] Availability gating covers Vulkan/D3D12, Full/Simplified, has_current_records, and all three fallback cases") {
	// API support helper: Vulkan and D3D12 only; Metal and OpenGL are unsupported.
	CHECK(nextgi_world_cache_api_supported("Vulkan"));
	CHECK(nextgi_world_cache_api_supported("D3D12"));
	CHECK(nextgi_world_cache_api_supported("Direct3D 12"));
	CHECK_FALSE(nextgi_world_cache_api_supported("Metal"));
	CHECK_FALSE(nextgi_world_cache_api_supported("OpenGL"));
	CHECK_FALSE(nextgi_world_cache_api_supported(""));
	CHECK(nextgi_world_cache_api_is_metal("Metal"));
	CHECK(nextgi_world_cache_api_is_metal("metal"));
	CHECK_FALSE(nextgi_world_cache_api_is_metal("Vulkan"));

	// Off by default: never replaces SRC.
	{
		const NextGIWorldCacheVerdict v = nextgi_world_cache_evaluate(
				false, true, false, "Vulkan", false, NEXTGI_HIT_MODE_FULL, NEXTGI_HIT_AVAILABLE, true, true, false);
		CHECK_FALSE(v.requested);
		CHECK_FALSE(v.active);
		CHECK_FALSE(v.replaces_src);
		CHECK_EQ(v.reason, NEXTGI_WC_REASON_OFF);
	}

	// Unsupported case (a): Software tier (SWRT) keeps the Surface Cache.
	{
		const NextGIWorldCacheVerdict v = nextgi_world_cache_evaluate(
				true, false, true, "Vulkan", false, NEXTGI_HIT_MODE_FULL, NEXTGI_HIT_NOT_HWRT, true, true, false);
		CHECK(v.requested);
		CHECK_FALSE(v.active);
		CHECK_FALSE(v.replaces_src);
		CHECK_EQ(v.reason, NEXTGI_WC_REASON_SOFTWARE_TIER);
		CHECK_EQ(String(nextgi_world_cache_reason_name(v.reason)), String("SOFTWARE_TIER"));
		CHECK(String(nextgi_world_cache_reason_message(v.reason)).contains("software"));
	}

	// Unsupported case (b): Inline HWRT mode keeps the Surface Cache.
	{
		const NextGIWorldCacheVerdict v = nextgi_world_cache_evaluate(
				true, true, false, "Vulkan", false, NEXTGI_HIT_MODE_INLINE, NEXTGI_HIT_HWRT_MODE_INLINE, true, true, false);
		CHECK(v.requested);
		CHECK_FALSE(v.active);
		CHECK_FALSE(v.replaces_src);
		CHECK_EQ(v.reason, NEXTGI_WC_REASON_INLINE_HWRT);
		CHECK_EQ(String(nextgi_world_cache_reason_name(v.reason)), String("INLINE_HWRT"));
		CHECK(String(nextgi_world_cache_reason_message(v.reason)).contains("Inline"));
	}

	// Unsupported case (c): Metal / macOS keeps the Surface Cache (§26.2).
	for (int variant = 0; variant < 2; variant++) {
		const String api = variant == 0 ? String("Metal") : String("Vulkan");
		const bool is_metal = variant == 1;
		const NextGIWorldCacheVerdict v = nextgi_world_cache_evaluate(
				true, true, false, api, is_metal, NEXTGI_HIT_MODE_FULL, NEXTGI_HIT_AVAILABLE, true, true, false);
		CHECK(v.requested);
		CHECK_FALSE(v.active);
		CHECK_FALSE(v.replaces_src);
		CHECK_EQ(v.reason, NEXTGI_WC_REASON_METAL_UNSUPPORTED);
		CHECK_EQ(String(nextgi_world_cache_reason_name(v.reason)), String("METAL_UNSUPPORTED"));
		CHECK(String(nextgi_world_cache_reason_message(v.reason)).contains("Metal"));
	}

	// Other unsupported cases: non-Vulkan/D3D12 API, unavailable hit-material path, or missing raygen.
	{
		const NextGIWorldCacheVerdict api_v = nextgi_world_cache_evaluate(
				true, true, false, "OpenGL", false, NEXTGI_HIT_MODE_FULL, NEXTGI_HIT_AVAILABLE, true, true, false);
		CHECK_FALSE(api_v.replaces_src);
		CHECK_EQ(api_v.reason, NEXTGI_WC_REASON_UNSUPPORTED_API);

		for (NextGIHitAvailability bad : { NEXTGI_HIT_NO_RAYTRACING_PIPELINE, NEXTGI_HIT_NO_PIPELINE_LIBRARY, NEXTGI_HIT_NO_BINDLESS, NEXTGI_HIT_INIT_FAILED, NEXTGI_HIT_RELEASING }) {
			const NextGIWorldCacheVerdict hm_v = nextgi_world_cache_evaluate(
					true, true, false, "Vulkan", false, NEXTGI_HIT_MODE_FULL, bad, true, true, false);
			CHECK_FALSE(hm_v.active);
			CHECK_FALSE(hm_v.replaces_src);
			CHECK_EQ(hm_v.reason, NEXTGI_WC_REASON_HIT_MATERIALS_UNAVAILABLE);
		}

		const NextGIWorldCacheVerdict rg_v = nextgi_world_cache_evaluate(
				true, true, false, "D3D12", false, NEXTGI_HIT_MODE_SIMPLIFIED_MATERIAL, NEXTGI_HIT_AVAILABLE, false, true, false);
		CHECK_FALSE(rg_v.active);
		CHECK_FALSE(rg_v.replaces_src);
		CHECK_EQ(rg_v.reason, NEXTGI_WC_REASON_RAYGEN_UNAVAILABLE);
	}

	// Active on Vulkan and D3D12 HWRT in both Full and Simplified Material modes: replaces SRC completely.
	for (const char *api : { "Vulkan", "D3D12", "Direct3D 12" }) {
		for (NextGIHitMode mode : { NEXTGI_HIT_MODE_FULL, NEXTGI_HIT_MODE_SIMPLIFIED_MATERIAL }) {
			const NextGIWorldCacheVerdict v = nextgi_world_cache_evaluate(
					true, true, false, api, false, mode, NEXTGI_HIT_AVAILABLE, true, true, true);
			CHECK(v.requested);
			CHECK(v.active);
			CHECK(v.replaces_src);
			CHECK_EQ(v.reason, NEXTGI_WC_REASON_ACTIVE);
		}
	}

	// Gating on has_current_records(): before TLAS hit records are built (e.g. frame 0 before BLASes finish),
	// World Cache still replaces SRC (SRC is NOT allocated) while deferring trace dispatch until records exist.
	{
		const NextGIWorldCacheVerdict pre = nextgi_world_cache_evaluate(
				true, true, false, "Vulkan", false, NEXTGI_HIT_MODE_FULL, NEXTGI_HIT_AVAILABLE, true, false, false);
		CHECK(pre.replaces_src);
		CHECK(pre.active);

		const NextGIWorldCacheVerdict trace = nextgi_world_cache_evaluate(
				true, true, false, "Vulkan", false, NEXTGI_HIT_MODE_FULL, NEXTGI_HIT_AVAILABLE, true, false, true);
		CHECK(trace.replaces_src);
		CHECK_FALSE(trace.active);
		CHECK_EQ(trace.reason, NEXTGI_WC_REASON_NO_CURRENT_RECORDS);
	}

	// One-time-per-session logging for each distinct fallback reason.
	uint32_t logged_mask = 0;
	CHECK_FALSE(nextgi_world_cache_should_log_reason(NEXTGI_WC_REASON_OFF, logged_mask));
	CHECK_FALSE(nextgi_world_cache_should_log_reason(NEXTGI_WC_REASON_ACTIVE, logged_mask));
	CHECK_FALSE(nextgi_world_cache_should_log_reason(NEXTGI_WC_REASON_NO_CURRENT_RECORDS, logged_mask));
	for (NextGIWorldCacheReason r : { NEXTGI_WC_REASON_SOFTWARE_TIER, NEXTGI_WC_REASON_INLINE_HWRT, NEXTGI_WC_REASON_METAL_UNSUPPORTED }) {
		CHECK(nextgi_world_cache_should_log_reason(r, logged_mask));
		CHECK_FALSE(nextgi_world_cache_should_log_reason(r, logged_mask));
		CHECK_FALSE(nextgi_world_cache_should_log_reason(r, logged_mask));
	}
	for (int i = 0; i < NEXTGI_WC_REASON_MAX; i++) {
		CHECK_NE(String(nextgi_world_cache_reason_name(NextGIWorldCacheReason(i))), String("UNKNOWN"));
	}
}

TEST_CASE("[NextGI][WorldCache] Consumer bit belongs to NEXTGI_HIT_SIMPLIFIED_CONSUMERS and gates via get_hit_availability_for") {
	CHECK((NEXTGI_HIT_SIMPLIFIED_CONSUMERS & NEXTGI_HIT_CONSUMER_WORLD_CACHE) != 0u);
	CHECK_EQ(nextgi_hit_mode_consumers(NEXTGI_HIT_CONSUMER_WORLD_CACHE, NEXTGI_HIT_MODE_FULL), uint32_t(NEXTGI_HIT_CONSUMER_WORLD_CACHE));
	CHECK_EQ(nextgi_hit_mode_consumers(NEXTGI_HIT_CONSUMER_WORLD_CACHE, NEXTGI_HIT_MODE_SIMPLIFIED_MATERIAL), uint32_t(NEXTGI_HIT_CONSUMER_WORLD_CACHE));
	CHECK_EQ(nextgi_hit_mode_consumers(NEXTGI_HIT_CONSUMER_WORLD_CACHE, NEXTGI_HIT_MODE_INLINE), 0u);

	CHECK_EQ(nextgi_hit_availability(NEXTGI_HIT_CONSUMER_WORLD_CACHE, true, true, true, true, false, NEXTGI_HIT_MODE_FULL), NEXTGI_HIT_AVAILABLE);
	CHECK_EQ(nextgi_hit_availability(NEXTGI_HIT_CONSUMER_WORLD_CACHE, true, true, true, true, false, NEXTGI_HIT_MODE_SIMPLIFIED_MATERIAL), NEXTGI_HIT_AVAILABLE);
	CHECK_EQ(nextgi_hit_availability(NEXTGI_HIT_CONSUMER_WORLD_CACHE, true, true, true, true, false, NEXTGI_HIT_MODE_INLINE), NEXTGI_HIT_HWRT_MODE_INLINE);

	// Simplified Material builds no attribute stream and no material hit groups even while World Cache runs.
	CHECK_FALSE(nextgi_hit_stream_needed(NEXTGI_HIT_AVAILABLE, true, NEXTGI_HIT_MODE_SIMPLIFIED_MATERIAL));
	CHECK(nextgi_hit_stream_needed(NEXTGI_HIT_AVAILABLE, true, NEXTGI_HIT_MODE_FULL));

	// NextGI::get_hit_availability_for() reflects Inline vs Simplified Material on the World Cache consumer bit.
	const String windows = String(HWRT_MODE_SETTING) + ".windows";
	const String linuxbsd = String(HWRT_MODE_SETTING) + ".linuxbsd";
	{
		ScopedSetting base(HWRT_MODE_SETTING, int(NEXTGI_HIT_MODE_INLINE));
		ScopedSetting win(windows, int(NEXTGI_HIT_MODE_INLINE));
		ScopedSetting lin(linuxbsd, int(NEXTGI_HIT_MODE_INLINE));
		NextGI next_gi;
		next_gi.init(true);
		CHECK_EQ(next_gi.get_hit_availability_for(NEXTGI_HIT_CONSUMER_WORLD_CACHE), int(NEXTGI_HIT_HWRT_MODE_INLINE));
		const Dictionary by_consumer = next_gi.get_tier_info()["hit_groups_by_consumer"];
		CHECK_EQ(String(by_consumer["world_cache"]), String("HWRT_MODE_INLINE"));
	}
	{
		ScopedSetting base(HWRT_MODE_SETTING, int(NEXTGI_HIT_MODE_SIMPLIFIED_MATERIAL));
		ScopedSetting win(windows, int(NEXTGI_HIT_MODE_SIMPLIFIED_MATERIAL));
		ScopedSetting lin(linuxbsd, int(NEXTGI_HIT_MODE_SIMPLIFIED_MATERIAL));
		NextGI next_gi;
		next_gi.init(true);
		// Without a live GPU device in doctests, Simplified Material passes the mode gate and reports NOT_HWRT.
		CHECK_EQ(next_gi.get_hit_availability_for(NEXTGI_HIT_CONSUMER_WORLD_CACHE), int(NEXTGI_HIT_NOT_HWRT));
		const Dictionary by_consumer = next_gi.get_tier_info()["hit_groups_by_consumer"];
		CHECK_EQ(String(by_consumer["world_cache"]), String("NOT_HWRT"));
	}
}

TEST_CASE("[NextGI][WorldCache] Simplified Material surface colour payload round-trips albedo, HDR emission, roughness, and metallic") {
	struct Sample {
		Color albedo;
		Color emission;
		float roughness;
		float metallic;
	};
	const Sample samples[] = {
		{ Color(0.85f, 0.12f, 0.08f, 1.0f), Color(0.0f, 0.0f, 0.0f, 1.0f), 0.75f, 0.0f },
		{ Color(0.10f, 0.90f, 0.25f, 1.0f), Color(6.5f, 1.25f, 0.25f, 1.0f), 0.20f, 0.85f },
		{ Color(1.0f, 1.0f, 1.0f, 1.0f), Color(0.0f, 14.0f, 3.5f, 1.0f), 1.0f, 1.0f },
		{ Color(0.0f, 0.0f, 0.0f, 1.0f), Color(0.0f, 0.0f, 0.0f, 1.0f), 0.0f, 0.0f },
	};
	for (const Sample &s : samples) {
		uint32_t word_albedo = 0;
		uint32_t word_emission = 0;
		uint32_t flags = 0;
		nextgi_hit_pack_simplified_surface(s.albedo, s.emission, s.roughness, s.metallic, word_albedo, word_emission, flags);
		CHECK((flags & NEXTGI_HIT_SURFACE_SIMPLIFIED_COLOR) != 0u);
		CHECK((flags & NEXTGI_HIT_SURFACE_HAS_STREAM) == 0u);

		Color out_albedo;
		Color out_emission;
		float out_roughness = -1.0f;
		float out_metallic = -1.0f;
		nextgi_hit_unpack_simplified_surface(word_albedo, word_emission, flags, out_albedo, out_emission, out_roughness, out_metallic);
		CHECK(Math::is_equal_approx(out_albedo.r, s.albedo.r, 1.0f / 254.0f));
		CHECK(Math::is_equal_approx(out_albedo.g, s.albedo.g, 1.0f / 254.0f));
		CHECK(Math::is_equal_approx(out_albedo.b, s.albedo.b, 1.0f / 254.0f));
		CHECK(Math::is_equal_approx(out_roughness, s.roughness, 1.0f / 254.0f));
		CHECK(Math::is_equal_approx(out_metallic, s.metallic, 1.0f / 254.0f));
		// RGB9E5 shared-exponent HDR emission stays within 1% relative (or 1e-3 absolute for zero).
		CHECK(Math::abs(out_emission.r - s.emission.r) <= MAX(0.01f * s.emission.r, 0.002f));
		CHECK(Math::abs(out_emission.g - s.emission.g) <= MAX(0.01f * s.emission.g, 0.002f));
		CHECK(Math::abs(out_emission.b - s.emission.b) <= MAX(0.01f * s.emission.b, 0.002f));
	}

	// Unflagged record unpacks neutral fallback defaults.
	Color a, e;
	float r = -1.0f, m = -1.0f;
	nextgi_hit_unpack_simplified_surface(0u, 0u, 0u, a, e, r, m);
	CHECK(Math::is_equal_approx(a.r, 0.5f, 0.01f));
	CHECK(Math::is_equal_approx(e.r, 0.0f, 0.001f));
	CHECK(Math::is_equal_approx(r, 1.0f, 0.01f));
	CHECK(Math::is_equal_approx(m, 0.0f, 0.01f));
}

TEST_CASE("[NextGI][WorldCache] Sparse world-space grid cascades, non-zero keys, temporal blending, aging, and eviction-hole safety") {
	// Six dominant normal faces.
	CHECK_EQ(nextgi_wc_normal_face(Vector3(1, 0, 0)), 0u);
	CHECK_EQ(nextgi_wc_normal_face(Vector3(-1, 0, 0)), 1u);
	CHECK_EQ(nextgi_wc_normal_face(Vector3(0, 1, 0)), 2u);
	CHECK_EQ(nextgi_wc_normal_face(Vector3(0, -1, 0)), 3u);
	CHECK_EQ(nextgi_wc_normal_face(Vector3(0, 0, 1)), 4u);
	CHECK_EQ(nextgi_wc_normal_face(Vector3(0, 0, -1)), 5u);

	// Five distance cascades and doubling cell sizes (0.5m .. 8.0m).
	CHECK_EQ(nextgi_wc_level_for_distance(0.0f), 0u);
	CHECK_EQ(nextgi_wc_level_for_distance(11.9f), 0u);
	CHECK_EQ(nextgi_wc_level_for_distance(12.0f), 1u);
	CHECK_EQ(nextgi_wc_level_for_distance(27.9f), 1u);
	CHECK_EQ(nextgi_wc_level_for_distance(28.0f), 2u);
	CHECK_EQ(nextgi_wc_level_for_distance(64.0f), 3u);
	CHECK_EQ(nextgi_wc_level_for_distance(140.0f), 4u);
	CHECK(Math::is_equal_approx(nextgi_wc_cell_size(0), 0.5f));
	CHECK(Math::is_equal_approx(nextgi_wc_cell_size(1), 1.0f));
	CHECK(Math::is_equal_approx(nextgi_wc_cell_size(2), 2.0f));
	CHECK(Math::is_equal_approx(nextgi_wc_cell_size(3), 4.0f));
	CHECK(Math::is_equal_approx(nextgi_wc_cell_size(4), 8.0f));

	// Probe grid sizing helpers.
	CHECK_EQ(nextgi_wc_probe_stride(Size2i(320, 240)), 4u);
	CHECK_EQ(nextgi_wc_probe_stride(Size2i(1920, 1080)), 8u);
	CHECK_EQ(nextgi_wc_probe_grid_size(Size2i(1920, 1080), 8u), Size2i(240, 135));

	// Non-zero keys across positive/negative coordinates, all levels, and all 6 faces.
	for (uint32_t level = 0; level < NEXTGI_WC_LEVELS; level++) {
		for (uint32_t face = 0; face < 6; face++) {
			CHECK_NE(nextgi_wc_make_key(Vector3i(0, 0, 0), level, face), uint32_t(NEXTGI_WC_EMPTY_KEY));
			CHECK_NE(nextgi_wc_make_key(Vector3i(-1, -1, -1), level, face), uint32_t(NEXTGI_WC_EMPTY_KEY));
			CHECK_LT(nextgi_wc_hash_bucket(Vector3i(3, -7, 11), level, face), uint32_t(NEXTGI_WC_BUCKETS));
		}
	}

	// Sparse table insertion, multi-sample averaging, per-face separation, temporal convergence, coarse fallback, and eviction.
	NextGIWorldCacheCPUTable table;
	table.init();
	const Vector3 cam(0.0f, 1.5f, 0.0f);
	const Vector3 pos(2.25f, 1.25f, -3.25f);

	// Two samples in the same frame average cleanly on +Y, while -Y stays independent.
	CHECK(table.insert_and_accum(pos, Vector3(0, 1, 0), cam, Vector3(2.0f, 0.0f, 0.0f)));
	CHECK(table.insert_and_accum(pos, Vector3(0, 1, 0), cam, Vector3(0.0f, 2.0f, 0.0f)));
	CHECK(table.insert_and_accum(pos, Vector3(0, -1, 0), cam, Vector3(0.0f, 0.0f, 3.0f)));
	table.resolve_and_evict(1);
	CHECK_EQ(table.cells_inserted, 2u);
	CHECK_EQ(table.cells_updated, 2u);
	CHECK_EQ(table.cells_active, 2u);
	CHECK_EQ(table.cells_evicted, 0u);

	Vector3 got;
	uint32_t got_level = 99u;
	CHECK(table.lookup(pos, Vector3(0, 1, 0), cam, got, &got_level));
	CHECK_EQ(got_level, 0u);
	CHECK(Math::is_equal_approx(got.x, 1.0f, 0.01f));
	CHECK(Math::is_equal_approx(got.y, 1.0f, 0.01f));
	CHECK(Math::is_equal_approx(got.z, 0.0f, 0.01f));

	CHECK(table.lookup(pos, Vector3(0, -1, 0), cam, got));
	CHECK(Math::is_equal_approx(got.x, 0.0f, 0.01f));
	CHECK(Math::is_equal_approx(got.y, 0.0f, 0.01f));
	CHECK(Math::is_equal_approx(got.z, 3.0f, 0.01f));

	// Temporal filtering over 24 frames converges smoothly toward new target lighting (history cap = 16).
	for (uint32_t f = 2; f <= 25; f++) {
		CHECK(table.insert_and_accum(pos, Vector3(0, 1, 0), cam, Vector3(4.0f, 0.0f, 0.0f)));
		table.resolve_and_evict(f);
	}
	CHECK(table.lookup(pos, Vector3(0, 1, 0), cam, got));
	CHECK(got.x > 3.2f);
	CHECK(got.y < 0.3f);

	// Coarse-level fallback: populate a level-1 cell at distance 15m, then look up from a closer camera (level 0).
	const Vector3 far_pos(0.0f, 0.25f, -15.25f);
	CHECK(table.insert_and_accum(far_pos, Vector3(0, 1, 0), cam, Vector3(0.5f, 1.5f, 2.5f)));
	table.resolve_and_evict(26);
	const Vector3 moved_cam(0.0f, 1.5f, -8.0f); // Now ~7.4m from far_pos -> base level 0, falls back to level 1.
	CHECK(table.lookup(far_pos, Vector3(0, 1, 0), moved_cam, got, &got_level));
	CHECK_EQ(got_level, 1u);
	CHECK(Math::is_equal_approx(got.y, 1.5f, 0.02f));

	// Eviction after NEXTGI_WC_MAX_AGE untouched frames.
	table.resolve_and_evict(26 + NEXTGI_WC_MAX_AGE);
	CHECK(table.lookup(far_pos, Vector3(0, 1, 0), cam, got));
	table.resolve_and_evict(26 + NEXTGI_WC_MAX_AGE + 1);
	CHECK_FALSE(table.lookup(far_pos, Vector3(0, 1, 0), cam, got));
	CHECK(table.cells_evicted >= 1u);

	// Eviction-hole safety: evicting slot 0 in an 8-way bucket must not orphan slot 1 or create duplicates.
	table.clear();
	const Vector3 hole_pos(1.25f, 0.25f, -1.25f);
	const Vector3i cell = nextgi_wc_quantize(hole_pos, 0u);
	const uint32_t key = nextgi_wc_make_key(cell, 0u, 2u);
	const uint32_t base_slot = nextgi_wc_hash_bucket(cell, 0u, 2u) * NEXTGI_WC_SLOTS_PER_BUCKET;
	// Place a dummy occupant in slot 0 and our target cell in slot 1, then evict slot 0.
	table.keys[base_slot + 0] = key ^ 1u;
	table.radiance[base_slot + 0] = { Vector3(9.0f, 9.0f, 9.0f), 1.0f };
	table.meta_frame[base_slot + 0] = 1u;
	table.keys[base_slot + 1] = key;
	table.radiance[base_slot + 1] = { Vector3(0.25f, 0.75f, 0.5f), 1.0f };
	table.meta_frame[base_slot + 1] = 20u;
	table.resolve_and_evict(1u + NEXTGI_WC_MAX_AGE + 1u); // Evicts slot 0 (age 33), keeps slot 1 (age 14).
	CHECK_EQ(table.keys[base_slot + 0], uint32_t(NEXTGI_WC_EMPTY_KEY));
	CHECK_EQ(table.keys[base_slot + 1], key);
	CHECK(table.lookup(hole_pos, Vector3(0, 1, 0), cam, got));
	CHECK(Math::is_equal_approx(got.y, 0.75f, 0.01f));
	// Re-inserting hole_pos updates slot 1 in place rather than duplicating into the empty slot 0.
	CHECK(table.insert_and_accum(hole_pos, Vector3(0, 1, 0), cam, Vector3(0.25f, 0.75f, 0.5f)));
	CHECK_EQ(table.keys[base_slot + 0], uint32_t(NEXTGI_WC_EMPTY_KEY));
	CHECK_EQ(table.keys[base_slot + 1], key);
}

TEST_CASE("[NextGI][WorldCache] Debug visualization enum parity and HWRT World Cache shader variants") {
	CHECK_EQ(int(Viewport::DEBUG_DRAW_NEXTGI_WORLD_CACHE), int(RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_WORLD_CACHE));
	CHECK_EQ(NextGI::debug_view_from_viewport(RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_WORLD_CACHE), NextGI::DEBUG_VIEW_WORLD_CACHE);
	CHECK_FALSE(nextgeo_is_unsupported_debug_draw(RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_WORLD_CACHE));

	NextGI::TierResult hwrt_result;
	hwrt_result.tier = NextGI::TIER_HWRT;
	const NextGI::DebugView view = NextGI::resolve_debug_view(NextGI::DEBUG_VIEW_WORLD_CACHE, hwrt_result);
	CHECK_EQ(view.label, String("world cache"));

	const Vector<String> gather_defines = NextGIGather::get_shader_variant_defines();
	REQUIRE(gather_defines.size() == 10);
	CHECK(gather_defines[8].contains("MODE_WORLD_CACHE"));
	CHECK(gather_defines[9].contains("MODE_REFLECT"));
	CHECK(gather_defines[9].contains("MODE_WORLD_CACHE"));
	const String gather_wc_src = NextGIGather::build_shader_source(8);
	CHECK(gather_wc_src.contains("nextgi_wc_lookup"));

	const Vector<String> wrc_defines = NextGIWRC::get_shader_variant_defines();
	REQUIRE(wrc_defines.size() == 8);
	CHECK(wrc_defines[6].contains("MODE_WORLD_CACHE"));
	CHECK(wrc_defines[7].contains("MODE_TRANSLUCENCY_TRACE"));
	CHECK(wrc_defines[7].contains("MODE_WORLD_CACHE"));
	const String wrc_wc_src = NextGIWRC::build_update_shader_source(6);
	CHECK(wrc_wc_src.contains("nextgi_wrc_wc_lookup"));
}

} // namespace TestNextGIWorldCache
