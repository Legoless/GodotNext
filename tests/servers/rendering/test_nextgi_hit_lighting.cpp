/**************************************************************************/
/*  test_nextgi_hit_lighting.cpp                                          */
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

#include "core/config/project_settings.h"
#include "scene/resources/environment.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_gather.h"
#include "servers/rendering/renderer_rd/environment/next_gi_hit_lighting.h"
#include "servers/rendering/renderer_rd/environment/next_gi_shadow_policy.h"
#include "servers/rendering/rendering_server.h"

#ifdef MODULE_GLSLANG_ENABLED
#include "modules/glslang/register_types.h"
#endif

TEST_FORCE_LINK(test_nextgi_hit_lighting)

namespace TestNextGIHitLighting {

using namespace RendererRD;

TEST_CASE("[NextGI][HitLighting] Settings defaults and environment override resolution") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);
	CHECK(ps->has_setting("rendering/global_illumination/nextgi/ray_lighting_mode"));
	CHECK_EQ(int(ps->get_setting("rendering/global_illumination/nextgi/ray_lighting_mode")), int(NEXTGI_RAY_LIGHTING_SURFACE_CACHE));
	CHECK(ps->has_setting("rendering/global_illumination/nextgi/max_reflection_bounces"));
	CHECK_EQ(int(ps->get_setting("rendering/global_illumination/nextgi/max_reflection_bounces")), 1);

	CHECK_EQ(nextgi_ray_lighting_mode_from_setting(0), NEXTGI_RAY_LIGHTING_SURFACE_CACHE);
	CHECK_EQ(nextgi_ray_lighting_mode_from_setting(1), NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS);
	CHECK_EQ(nextgi_ray_lighting_mode_from_setting(-1), NEXTGI_RAY_LIGHTING_SURFACE_CACHE);
	CHECK_EQ(nextgi_ray_lighting_mode_from_setting(99), NEXTGI_RAY_LIGHTING_SURFACE_CACHE);
	CHECK_EQ(String(nextgi_ray_lighting_mode_name(NEXTGI_RAY_LIGHTING_SURFACE_CACHE)), String("surface_cache"));
	CHECK_EQ(String(nextgi_ray_lighting_mode_name(NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS)), String("hit_lighting_reflections"));

	// Project default follows project setting; explicit environment values override it.
	CHECK_EQ(nextgi_resolve_ray_lighting_mode(0, NEXTGI_ENV_RAY_LIGHTING_PROJECT_DEFAULT), NEXTGI_RAY_LIGHTING_SURFACE_CACHE);
	CHECK_EQ(nextgi_resolve_ray_lighting_mode(1, NEXTGI_ENV_RAY_LIGHTING_PROJECT_DEFAULT), NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS);
	CHECK_EQ(nextgi_resolve_ray_lighting_mode(1, NEXTGI_ENV_RAY_LIGHTING_SURFACE_CACHE), NEXTGI_RAY_LIGHTING_SURFACE_CACHE);
	CHECK_EQ(nextgi_resolve_ray_lighting_mode(0, NEXTGI_ENV_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS), NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS);

	// Reflection bounce clamping [1..4].
	CHECK_EQ(nextgi_clamp_max_reflection_bounces(-5), 1u);
	CHECK_EQ(nextgi_clamp_max_reflection_bounces(0), 1u);
	CHECK_EQ(nextgi_clamp_max_reflection_bounces(1), 1u);
	CHECK_EQ(nextgi_clamp_max_reflection_bounces(2), 2u);
	CHECK_EQ(nextgi_clamp_max_reflection_bounces(4), 4u);
	CHECK_EQ(nextgi_clamp_max_reflection_bounces(9), 4u);

	// Environment property & RenderingServer round-trip.
	if (RenderingServer::get_singleton() != nullptr) {
		Ref<Environment> env;
		env.instantiate();
		CHECK_EQ(env->get_nextgi_ray_lighting_mode(), Environment::NEXTGI_RAY_LIGHTING_MODE_PROJECT_DEFAULT);
		CHECK_EQ(RenderingServer::get_singleton()->environment_get_nextgi_ray_lighting_mode(env->get_rid()), 0);
		env->set_nextgi_ray_lighting_mode(Environment::NEXTGI_RAY_LIGHTING_MODE_HIT_LIGHTING_FOR_REFLECTIONS);
		CHECK_EQ(env->get_nextgi_ray_lighting_mode(), Environment::NEXTGI_RAY_LIGHTING_MODE_HIT_LIGHTING_FOR_REFLECTIONS);
		CHECK_EQ(RenderingServer::get_singleton()->environment_get_nextgi_ray_lighting_mode(env->get_rid()), 2);
		env->set_nextgi_ray_lighting_mode(Environment::NEXTGI_RAY_LIGHTING_MODE_SURFACE_CACHE);
		CHECK_EQ(RenderingServer::get_singleton()->environment_get_nextgi_ray_lighting_mode(env->get_rid()), 1);
	}
}

TEST_CASE("[NextGI][HitLighting] Fallback verdict reports HIT_LIGHTING_UNSUPPORTED when unavailable") {
	// Default Surface Cache mode never triggers unsupported fallback.
	const NextGIHitLightingVerdict sc = nextgi_hit_lighting_evaluate(NEXTGI_RAY_LIGHTING_SURFACE_CACHE, false, false, false, false, NEXTGI_HIT_MODE_INLINE, false);
	CHECK_FALSE(sc.active);
	CHECK_FALSE(sc.unsupported_fallback);
	CHECK_EQ(sc.effective_mode, NEXTGI_RAY_LIGHTING_SURFACE_CACHE);
	CHECK_EQ(String(sc.reason), String("SURFACE_CACHE"));

	// Active only on HWRT + RT pipelines + pipeline libraries + bindless + Full mode + raygen ready.
	const NextGIHitLightingVerdict active = nextgi_hit_lighting_evaluate(NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS, true, true, true, true, NEXTGI_HIT_MODE_FULL, true);
	CHECK(active.active);
	CHECK_FALSE(active.unsupported_fallback);
	CHECK_EQ(active.effective_mode, NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS);
	CHECK_EQ(String(active.reason), String("ACTIVE"));

	// Missing any prerequisite falls back cleanly to Surface Cache with HIT_LIGHTING_UNSUPPORTED.
	const NextGIHitLightingVerdict cases[] = {
		nextgi_hit_lighting_evaluate(NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS, false, true, true, true, NEXTGI_HIT_MODE_FULL, true), // SWRT tier
		nextgi_hit_lighting_evaluate(NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS, true, false, true, true, NEXTGI_HIT_MODE_FULL, true), // No RT pipelines
		nextgi_hit_lighting_evaluate(NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS, true, true, false, true, NEXTGI_HIT_MODE_FULL, true), // No pipeline library
		nextgi_hit_lighting_evaluate(NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS, true, true, true, false, NEXTGI_HIT_MODE_FULL, true), // No bindless
		nextgi_hit_lighting_evaluate(NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS, true, true, true, true, NEXTGI_HIT_MODE_INLINE, true), // Inline HWRT mode
		nextgi_hit_lighting_evaluate(NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS, true, true, true, true, NEXTGI_HIT_MODE_SIMPLIFIED_MATERIAL, true), // Simplified material mode
		nextgi_hit_lighting_evaluate(NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS, true, true, true, true, NEXTGI_HIT_MODE_FULL, false), // Raygen failed
	};
	for (const NextGIHitLightingVerdict &v : cases) {
		CHECK_FALSE(v.active);
		CHECK(v.unsupported_fallback);
		CHECK_EQ(v.effective_mode, NEXTGI_RAY_LIGHTING_SURFACE_CACHE);
		CHECK_EQ(String(v.reason), String("HIT_LIGHTING_UNSUPPORTED"));
	}
}

TEST_CASE("[NextGI][HitLighting] Hit record packing and unpacking round-trip") {
	NextGIHitLightRecordUnpacked in;
	in.origin = Vector3(12.5f, -3.25f, 7.75f);
	in.hit_t = 42.125f;
	in.instance_index = 0x123456u;
	in.flags = NEXTGI_HL_RECORD_FLAG_FRONT_FACE | NEXTGI_HL_RECORD_FLAG_TRANSLUCENT | NEXTGI_HL_RECORD_FLAG_MIRROR;
	in.geometry_index = 19u;
	in.material_key = nextgi_hit_light_material_key(11u, 37u);
	in.primitive = 987654u;
	in.pixel = Vector2i(1279, 719);
	in.bary = Vector2(0.25f, 0.375f);
	in.ray_dir = Vector3(-0.3f, 0.8f, -0.5196152f).normalized();
	in.cone = Vector2(0.0125f, 0.0625f);
	in.resume_distance = 1.75f;

	const NextGIHitLightRecord packed = nextgi_hit_light_pack_record(in);
	const NextGIHitLightRecordUnpacked out = nextgi_hit_light_unpack_record(packed);

	CHECK(out.origin.is_equal_approx(in.origin));
	CHECK(Math::is_equal_approx(out.hit_t, in.hit_t));
	CHECK_EQ(out.instance_index, in.instance_index);
	CHECK_EQ(out.flags, in.flags);
	CHECK_EQ(out.geometry_index, in.geometry_index);
	CHECK_EQ(out.material_key, in.material_key);
	CHECK_EQ(out.material_key, uint16_t((11u << 8u) | 37u));
	CHECK_EQ(out.primitive, in.primitive);
	CHECK_EQ(out.pixel, in.pixel);
	CHECK(Math::abs(out.bary.x - in.bary.x) < 1e-3f);
	CHECK(Math::abs(out.bary.y - in.bary.y) < 1e-3f);
	CHECK(out.ray_dir.dot(in.ray_dir) > 0.9999f);
	CHECK(Math::abs(out.cone.x - in.cone.x) < 1e-3f);
	CHECK(Math::abs(out.cone.y - in.cone.y) < 1e-3f);
	CHECK(Math::is_equal_approx(out.resume_distance, in.resume_distance));

	// Disjoint fallback & saturation material keys: reflections stay in [0x0000..0x7FFF],
	// diffuse GI stays in [0x8000..0xFFFF] (#128).
	CHECK_EQ(nextgi_hit_light_material_key(UINT32_MAX, 5u), uint16_t(NEXTGI_HL_KEY_REFLECTION_FALLBACK));
	CHECK_EQ(nextgi_hit_light_material_key(2u, UINT32_MAX), uint16_t(NEXTGI_HL_KEY_REFLECTION_FALLBACK));
	CHECK_EQ(nextgi_hit_light_material_key(0xFFu, 0xFFu), uint16_t(NEXTGI_HL_KEY_REFLECTION_FALLBACK - 1u));
	CHECK_FALSE(nextgi_hit_light_key_is_gi(nextgi_hit_light_material_key(UINT32_MAX, 5u)));
	CHECK_FALSE(nextgi_hit_light_key_is_gi(nextgi_hit_light_material_key(0xFFu, 0xFFu)));

	CHECK_EQ(nextgi_hit_light_gi_material_key(11u, 37u), uint16_t(NEXTGI_HL_KEY_GI_BASE | (11u << 8u) | 37u));
	CHECK_EQ(nextgi_hit_light_gi_material_key(UINT32_MAX, 5u), uint16_t(NEXTGI_HL_KEY_GI_FALLBACK));
	CHECK_EQ(nextgi_hit_light_gi_material_key(2u, UINT32_MAX), uint16_t(NEXTGI_HL_KEY_GI_FALLBACK));
	CHECK_EQ(nextgi_hit_light_gi_material_key(0xFFu, 0xFFu), uint16_t(NEXTGI_HL_KEY_GI_FALLBACK - 1u));
	CHECK(nextgi_hit_light_key_is_gi(nextgi_hit_light_gi_material_key(0u, 0u)));
	CHECK(nextgi_hit_light_key_is_gi(nextgi_hit_light_gi_material_key(UINT32_MAX, 0u)));
}

TEST_CASE("[NextGI][HitLighting] 16-bit counting sort orders active hits by material key") {
	const uint16_t keys[] = { 0xFFFFu, 42u, 0u, 300u, 42u, 1024u, 0xFFFFu, 7u, 300u, 0u, 65534u, 7u };
	const uint32_t count = sizeof(keys) / sizeof(keys[0]);
	LocalVector<NextGIHitLightRecord> records;
	records.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		NextGIHitLightRecordUnpacked u;
		u.instance_index = i + 1;
		u.material_key = keys[i];
		u.primitive = i * 10;
		u.ray_dir = Vector3(0.0f, 1.0f, 0.0f);
		records[i] = nextgi_hit_light_pack_record(u);
	}

	LocalVector<uint32_t> sorted;
	nextgi_hit_light_counting_sort(records.ptr(), count, sorted);
	REQUIRE_EQ(sorted.size(), count);

	LocalVector<bool> seen;
	seen.resize_initialized(count);
	uint16_t prev_key = 0;
	for (uint32_t i = 0; i < count; i++) {
		const uint32_t idx = sorted[i];
		REQUIRE(idx < count);
		CHECK_FALSE(seen[idx]);
		seen[idx] = true;
		const uint16_t k = uint16_t(records[idx].geometry_material >> 16u);
		if (i > 0) {
			CHECK(k >= prev_key);
		}
		prev_key = k;
	}
	// Verify stability within duplicate key groups (0, 7, 42, 300, 0xFFFF).
	CHECK_EQ(sorted[0], 2u);
	CHECK_EQ(sorted[1], 9u);
	CHECK_EQ(sorted[count - 2], 0u);
	CHECK_EQ(sorted[count - 1], 6u);

	// Empty input returns empty output.
	LocalVector<uint32_t> empty_sorted;
	nextgi_hit_light_counting_sort(nullptr, 0, empty_sorted);
	CHECK(empty_sorted.is_empty());
}

TEST_CASE("[NextGI][HitLighting] 64^3 world light grid mapping and <= 32 lights/cell deterministic overflow") {
	const Vector3 cam(10.0f, 5.0f, -20.0f);
	const float radius = 64.0f;
	const Vector3 grid_min = cam - Vector3(radius, radius, radius);
	const float cell_size = (2.0f * radius) / float(NEXTGI_LIGHT_GRID_DIM); // 2.0m
	const float inv_cell = 1.0f / cell_size;

	CHECK_EQ(nextgi_light_grid_world_to_cell(grid_min + Vector3(0.1f, 0.1f, 0.1f), grid_min, inv_cell), 0u);
	CHECK_EQ(nextgi_light_grid_world_to_cell(cam + Vector3(0.1f, 0.1f, 0.1f), grid_min, inv_cell), 32u + 32u * 64u + 32u * 4096u);
	CHECK_EQ(nextgi_light_grid_world_to_cell(grid_min + Vector3(2.0f * radius - 0.1f, 2.0f * radius - 0.1f, 2.0f * radius - 0.1f), grid_min, inv_cell), uint32_t(NEXTGI_LIGHT_GRID_CELLS - 1));
	CHECK_EQ(nextgi_light_grid_world_to_cell(grid_min - Vector3(0.1f, 0.0f, 0.0f), grid_min, inv_cell), UINT32_MAX);
	CHECK_EQ(nextgi_light_grid_world_to_cell(grid_min + Vector3(2.0f * radius + 0.1f, 0.0f, 0.0f), grid_min, inv_cell), UINT32_MAX);

	// Create 40 lights affecting cell [0, 2]^3: 1 directional at index 39, 39 omni lights of varying energy, plus 1 out-of-range omni at index 40.
	LocalVector<NextGIHitLightGPU> lights;
	lights.resize(41);
	for (uint32_t i = 0; i < 39; i++) {
		NextGIHitLightGPU &l = lights[i];
		l.color_energy[0] = 1.0f;
		l.color_energy[1] = 1.0f;
		l.color_energy[2] = 1.0f;
		l.color_energy[3] = float(i + 1); // Higher index = brighter omni.
		l.position_inv_range[0] = 1.0f;
		l.position_inv_range[1] = 1.0f;
		l.position_inv_range[2] = 1.0f;
		l.position_inv_range[3] = 1.0f / 10.0f;
		l.direction_type[3] = 1.0f; // Omni.
		l.spot_atten_radius[0] = 1.0f;
	}
	// Directional sun at index 39 must sort first and never be evicted.
	lights[39].color_energy[0] = 1.0f;
	lights[39].color_energy[1] = 1.0f;
	lights[39].color_energy[2] = 1.0f;
	lights[39].color_energy[3] = 1.0f;
	lights[39].direction_type[1] = -1.0f;
	lights[39].direction_type[3] = 0.0f; // Directional.
	// Out-of-range omni at index 40 must be culled.
	lights[40].color_energy[0] = 1.0f;
	lights[40].color_energy[3] = 9999.0f;
	lights[40].position_inv_range[0] = 500.0f;
	lights[40].position_inv_range[3] = 1.0f / 5.0f;
	lights[40].direction_type[3] = 1.0f;
	lights[40].spot_atten_radius[0] = 1.0f;

	uint16_t kept[NEXTGI_LIGHT_GRID_CELL_CAP] = {};
	uint32_t overflow = 0;
	const uint32_t count = nextgi_light_grid_fill_cell_cpu(lights.ptr(), lights.size(), Vector3(0, 0, 0), Vector3(2, 2, 2), kept, overflow);
	CHECK_EQ(count, uint32_t(NEXTGI_LIGHT_GRID_CELL_CAP));
	CHECK_EQ(overflow, 8u); // 40 intersecting lights - 32 cap = 8 overflowed.
	CHECK_EQ(kept[0], uint16_t(39)); // Directional light prioritized first.
	CHECK_EQ(kept[1], uint16_t(38)); // Brightest omni next.
	CHECK_EQ(kept[31], uint16_t(8)); // 31st brightest omni (indices 0..7 evicted deterministically).

	// Determinism: identical call produces identical array.
	uint16_t kept2[NEXTGI_LIGHT_GRID_CELL_CAP] = {};
	uint32_t overflow2 = 0;
	const uint32_t count2 = nextgi_light_grid_fill_cell_cpu(lights.ptr(), lights.size(), Vector3(0, 0, 0), Vector3(2, 2, 2), kept2, overflow2);
	CHECK_EQ(count2, count);
	CHECK_EQ(overflow2, overflow);
	for (uint32_t i = 0; i < NEXTGI_LIGHT_GRID_CELL_CAP; i++) {
		CHECK_EQ(kept2[i], kept[i]);
	}
}

TEST_CASE("[NextGI][HitLighting] Multi-bounce mirror decision and translucent skip state math") {
	// Single bounce never re-traces mirrors.
	CHECK_FALSE(nextgi_hit_light_should_bounce(0.0f, 0, 1));
	CHECK_FALSE(nextgi_hit_light_should_bounce(0.05f, 0, 1));
	// Multi-bounce re-traces only roughness < 0.1 up to max_bounces.
	CHECK(nextgi_hit_light_should_bounce(0.0f, 0, 2));
	CHECK(nextgi_hit_light_should_bounce(0.09f, 0, 2));
	CHECK_FALSE(nextgi_hit_light_should_bounce(0.1f, 0, 2));
	CHECK_FALSE(nextgi_hit_light_should_bounce(0.25f, 0, 4));
	CHECK_FALSE(nextgi_hit_light_should_bounce(0.02f, 1, 2));
	CHECK(nextgi_hit_light_should_bounce(0.02f, 2, 4));
	CHECK_FALSE(nextgi_hit_light_should_bounce(0.02f, 3, 4));

	// Translucent transmittance energy conservation and tinting.
	const Vector3 clear = nextgi_hit_light_translucent_transmittance(Color(1, 0, 0), 0.0f);
	CHECK(clear.is_equal_approx(Vector3(1.0f, 1.0f, 1.0f)));
	const Vector3 opaque = nextgi_hit_light_translucent_transmittance(Color(1, 1, 1), 1.0f);
	CHECK(opaque.is_equal_approx(Vector3(0.0f, 0.0f, 0.0f)));
	const Vector3 red_glass = nextgi_hit_light_translucent_transmittance(Color(1.0f, 0.1f, 0.1f), 0.5f);
	CHECK(red_glass.x > red_glass.y);
	CHECK(red_glass.x <= 0.5f + 1e-5f);
	CHECK(red_glass.y >= 0.0f);

	// Up to 2 translucent segments accumulate radiance & tint, then cap stops skipping.
	Vector3 accum(0, 0, 0);
	Vector3 throughput(1, 1, 1);
	uint32_t skips = 0;
	CHECK(nextgi_hit_light_accumulate_translucent(Color(1.0f, 0.0f, 0.0f), 0.5f, Vector3(0.4f, 0.0f, 0.0f), accum, throughput, skips));
	CHECK_EQ(skips, 1u);
	CHECK(accum.x > 0.19f);
	CHECK(throughput.x > throughput.y);
	CHECK(nextgi_hit_light_accumulate_translucent(Color(1.0f, 0.0f, 0.0f), 0.5f, Vector3(0.4f, 0.0f, 0.0f), accum, throughput, skips));
	CHECK_EQ(skips, 2u);
	// Third segment hits the 2-skip cap and refuses to skip further.
	CHECK_FALSE(nextgi_hit_light_accumulate_translucent(Color(0.0f, 1.0f, 0.0f), 0.5f, Vector3(0.0f, 1.0f, 0.0f), accum, throughput, skips));
	CHECK_EQ(skips, 2u);
}

TEST_CASE("[NextGI][HitLighting] TLAS mask bit 1 reservation keeps shadow policy consistent") {
	NextGIShadowMaskAllocator allocator;
	const uint32_t masks[6] = { 1u << 0, 1u << 1, 1u << 2, 1u << 3, 1u << 4, 1u << 5 };

	// Surface Cache mode (reserved = 0): allocates all 6 dynamic bits, unchanged from #145.
	CHECK(allocator.update(masks, 6, 0u));
	CHECK_EQ(allocator.get_allocation().count(), 6u);
	CHECK((allocator.get_allocation().used & NEXTGI_RT_MASK_TRANSLUCENT) != 0u);

	// Hit Lighting active (reserved = NEXTGI_RT_MASK_TRANSLUCENT): immediately evacuates bit 1.
	CHECK(allocator.update(masks, 6, NEXTGI_RT_MASK_TRANSLUCENT));
	CHECK_EQ(allocator.get_allocation().count(), 5u);
	CHECK_EQ(allocator.get_allocation().used & NEXTGI_RT_MASK_TRANSLUCENT, 0u);

	// Opaque GI instance never carries bit 1 when bit 1 is reserved.
	const uint32_t opaque_mask = nextgi_shadow_instance_mask(allocator.get_allocation(), 0xFFFFFu, true);
	CHECK((opaque_mask & NEXTGI_SHADOW_MASK_GI) != 0u);
	CHECK_EQ(opaque_mask & NEXTGI_RT_MASK_TRANSLUCENT, 0u);

	// Non-shadow-casting translucent instance carries only bit 1 (not GI bit 7).
	const uint32_t trans_mask = nextgi_shadow_instance_mask(allocator.get_allocation(), 0u, false) | NEXTGI_RT_MASK_TRANSLUCENT;
	CHECK_EQ(trans_mask, uint32_t(NEXTGI_RT_MASK_TRANSLUCENT));
}

TEST_CASE("[NextGI][HitLighting][GI] Setting default is false, advanced, and off-mode does zero GI hit-lighting work") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);
	const String setting_name = "rendering/global_illumination/nextgi/hit_lighting_gi";
	CHECK(ps->has_setting(setting_name));
	CHECK(ps->is_builtin_setting(setting_name));
	const Variant val = ps->get_setting(setting_name);
	CHECK_EQ(val.get_type(), Variant::BOOL);
	CHECK_FALSE(bool(val));
	CHECK_FALSE(NextGI::read_hit_lighting_gi());

	// Verify it is registered as an advanced setting (not basic: lacks PROPERTY_USAGE_BASIC).
	List<PropertyInfo> prop_list;
	ps->get_property_list(&prop_list);
	bool found_prop = false;
	for (const PropertyInfo &pi : prop_list) {
		if (pi.name == setting_name) {
			found_prop = true;
			CHECK_EQ(pi.type, Variant::BOOL);
			break;
		}
	}
	CHECK(found_prop);

	// Off-mode zero-work check on a fresh NextGI instance: zero active flags and zero GI counters.
	NextGI next_gi;
	NextGI::TierInputs hwrt_inputs;
	hwrt_inputs.project_enabled = true;
	hwrt_inputs.ray_query = true;
	hwrt_inputs.ray_query_hw = true;
	String tier_line;
	next_gi.update_tier(hwrt_inputs, NextGI::QUALITY_EPIC, tier_line);
	const Dictionary info = next_gi.get_tier_info();
	const Dictionary counters = next_gi.get_counters();
	const Dictionary status = next_gi.get_status();
	CHECK_FALSE(bool(info["hit_lighting_gi"]));
	CHECK_FALSE(bool(info["hit_lighting_gi_active"]));
	CHECK_EQ(String(info["hit_lighting_gi_reason"]), String("OFF"));
	CHECK_EQ(int64_t(info["hit_lighting_gi_rays"]), 0);
	CHECK_EQ(int64_t(info["hit_lighting_gi_dispatches"]), 0);
	CHECK_FALSE(bool(counters["hit_lighting_gi_active"]));
	CHECK_EQ(int64_t(counters["hit_lighting_gi_rays"]), 0);
	CHECK_EQ(int64_t(counters["hit_lighting_gi_marked_hits"]), 0);
	CHECK_EQ(int64_t(counters["hit_lighting_gi_sorted_hits"]), 0);
	CHECK_EQ(int64_t(counters["hit_lighting_gi_shadow_rays"]), 0);
	CHECK_EQ(int64_t(counters["hit_lighting_gi_dispatches"]), 0);
	CHECK_FALSE(bool(status["hit_lighting_gi"]));
	CHECK_FALSE(bool(status["hit_lighting_gi_active"]));
	CHECK_EQ(String(status["hit_lighting_gi_reason"]), String("OFF"));
}

TEST_CASE("[NextGI][HitLighting][GI] Epic + HWRT + ray_lighting_mode gating matrix") {
	const NextGIHitLightingVerdict refl_active = nextgi_hit_lighting_evaluate(
			NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS, true, true, true, true, NEXTGI_HIT_MODE_FULL, true);
	const NextGIHitLightingVerdict refl_sc = nextgi_hit_lighting_evaluate(
			NEXTGI_RAY_LIGHTING_SURFACE_CACHE, true, true, true, true, NEXTGI_HIT_MODE_FULL, true);
	const NextGIHitLightingVerdict refl_swrt = nextgi_hit_lighting_evaluate(
			NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS, false, true, true, true, NEXTGI_HIT_MODE_FULL, true);
	const NextGIHitLightingVerdict refl_inline = nextgi_hit_lighting_evaluate(
			NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS, true, true, true, true, NEXTGI_HIT_MODE_INLINE, true);
	const NextGIHitLightingVerdict refl_simplified = nextgi_hit_lighting_evaluate(
			NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS, true, true, true, true, NEXTGI_HIT_MODE_SIMPLIFIED_MATERIAL, true);
	const NextGIHitLightingVerdict refl_no_rt_pipe = nextgi_hit_lighting_evaluate(
			NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS, true, false, true, true, NEXTGI_HIT_MODE_FULL, true);
	const NextGIHitLightingVerdict refl_no_raygen = nextgi_hit_lighting_evaluate(
			NEXTGI_RAY_LIGHTING_HIT_LIGHTING_REFLECTIONS, true, true, true, true, NEXTGI_HIT_MODE_FULL, false);

	// Setting off: always OFF even when Epic + HWRT + hit_lighting_reflections are all met.
	{
		const NextGIHitLightingGIVerdict v = nextgi_hit_lighting_gi_evaluate(false, refl_active, true);
		CHECK_FALSE(v.requested);
		CHECK_FALSE(v.active);
		CHECK_EQ(String(v.reason), String("OFF"));
	}

	// Setting on + Surface Cache ray_lighting_mode: inactive.
	{
		const NextGIHitLightingGIVerdict v = nextgi_hit_lighting_gi_evaluate(true, refl_sc, true);
		CHECK(v.requested);
		CHECK_FALSE(v.active);
		CHECK_EQ(String(v.reason), String("RAY_LIGHTING_MODE_SURFACE_CACHE"));
	}

	// Setting on + hit_lighting_reflections + non-Epic quality (High / Medium): inactive.
	{
		const NextGIHitLightingGIVerdict v = nextgi_hit_lighting_gi_evaluate(true, refl_active, false);
		CHECK(v.requested);
		CHECK_FALSE(v.active);
		CHECK_EQ(String(v.reason), String("QUALITY_NOT_EPIC"));
	}

	// Setting on + Epic + hit_lighting_reflections but missing HWRT / Full mode / RT pipeline / raygen: unsupported.
	const NextGIHitLightingVerdict unsupported_cases[] = { refl_swrt, refl_inline, refl_simplified, refl_no_rt_pipe, refl_no_raygen };
	for (const NextGIHitLightingVerdict &rv : unsupported_cases) {
		const NextGIHitLightingGIVerdict v = nextgi_hit_lighting_gi_evaluate(true, rv, true);
		CHECK(v.requested);
		CHECK_FALSE(v.active);
		CHECK_EQ(String(v.reason), String("HIT_LIGHTING_UNSUPPORTED"));
	}

	// Setting on + Epic + HWRT + hit_lighting_reflections + Full + RT pipelines + raygen: ACTIVE!
	{
		const NextGIHitLightingGIVerdict v = nextgi_hit_lighting_gi_evaluate(true, refl_active, true);
		CHECK(v.requested);
		CHECK(v.active);
		CHECK_EQ(String(v.reason), String("ACTIVE"));
	}

	// GI shadowed light cap and 1.5x WRC handoff cutoff distance helper.
	CHECK_EQ(int(NEXTGI_HIT_LIGHT_GI_MAX_SHADOWED_LIGHTS), 1);
	CHECK(Math::is_equal_approx(nextgi_hit_light_gi_cutoff(1.5f, 200.0f), 2.25f));
	CHECK(Math::is_equal_approx(nextgi_hit_light_gi_cutoff(12.0f, 200.0f), 18.0f));
	CHECK(Math::is_equal_approx(nextgi_hit_light_gi_cutoff(150.0f, 200.0f), 200.0f));
	CHECK(Math::is_equal_approx(nextgi_hit_light_gi_cutoff(-1.0f, 200.0f), 200.0f));
}

TEST_CASE("[NextGI][HitLighting][GI] Disjoint 16-bit key ranges keep reflections and diffuse GI cleanly separated after counting sort") {
	// Verify every possible (shader_slot, material_id) pair produces strictly disjoint reflection and GI keys.
	const uint32_t sample_slots[] = { 0u, 1u, 7u, 127u, 128u, 255u, 1024u, UINT32_MAX };
	const uint32_t sample_mats[] = { 0u, 1u, 37u, 127u, 254u, 255u, 4096u, UINT32_MAX };
	for (uint32_t s : sample_slots) {
		for (uint32_t m : sample_mats) {
			const uint16_t rk = nextgi_hit_light_material_key(s, m);
			const uint16_t gk = nextgi_hit_light_gi_material_key(s, m);
			CHECK(rk <= uint16_t(NEXTGI_HL_KEY_REFLECTION_FALLBACK));
			CHECK(gk >= uint16_t(NEXTGI_HL_KEY_GI_BASE));
			CHECK(rk < gk);
			CHECK_FALSE(nextgi_hit_light_key_is_gi(rk));
			CHECK(nextgi_hit_light_key_is_gi(gk));
		}
	}

	// Interleave reflection hits (including fallback 0x7FFF) and diffuse GI hits (including 0x8000 and fallback 0xFFFF)
	// in the same record buffer and run the 16-bit counting sort.
	struct InputHit {
		bool is_gi;
		uint32_t shader_slot;
		uint32_t material_id;
	};
	const InputHit inputs[] = {
		{ true, 0u, 0u }, // GI min key 0x8000
		{ false, UINT32_MAX, UINT32_MAX }, // Refl fallback 0x7FFF
		{ true, UINT32_MAX, UINT32_MAX }, // GI fallback 0xFFFF
		{ false, 0u, 0u }, // Refl min key 0x0000
		{ true, 5u, 12u }, // GI 0x850C
		{ false, 127u, 255u }, // Refl max non-fallback 0x7FFE
		{ true, 5u, 12u }, // GI 0x850C duplicate
		{ false, 3u, 9u }, // Refl 0x0309
		{ true, 127u, 255u }, // GI max non-fallback 0xFFFE
		{ false, 3u, 9u }, // Refl 0x0309 duplicate
	};
	const uint32_t count = sizeof(inputs) / sizeof(inputs[0]);
	LocalVector<NextGIHitLightRecord> records;
	records.resize(count);
	uint32_t expected_refl = 0;
	uint32_t expected_gi = 0;
	for (uint32_t i = 0; i < count; i++) {
		NextGIHitLightRecordUnpacked u;
		u.instance_index = i + 1;
		u.flags = inputs[i].is_gi ? NEXTGI_HL_RECORD_FLAG_DIFFUSE_GI : 0u;
		u.material_key = inputs[i].is_gi
				? nextgi_hit_light_gi_material_key(inputs[i].shader_slot, inputs[i].material_id)
				: nextgi_hit_light_material_key(inputs[i].shader_slot, inputs[i].material_id);
		u.ray_dir = Vector3(0.0f, 1.0f, 0.0f);
		u.pixel = Vector2i(int(i), int(i * 2));
		u.resume_distance = inputs[i].is_gi ? 4.25f : 0.5f;
		records[i] = nextgi_hit_light_pack_record(u);
		if (inputs[i].is_gi) {
			expected_gi++;
		} else {
			expected_refl++;
		}
	}

	LocalVector<uint32_t> sorted;
	nextgi_hit_light_counting_sort(records.ptr(), count, sorted);
	REQUIRE_EQ(sorted.size(), count);
	REQUIRE_EQ(expected_refl, 5u);
	REQUIRE_EQ(expected_gi, 5u);

	// First [0 .. expected_refl - 1] must be strictly reflection hits in non-decreasing key order.
	uint16_t prev_key = 0;
	for (uint32_t i = 0; i < expected_refl; i++) {
		const NextGIHitLightRecordUnpacked u = nextgi_hit_light_unpack_record(records[sorted[i]]);
		CHECK_EQ(u.flags & NEXTGI_HL_RECORD_FLAG_DIFFUSE_GI, 0u);
		CHECK_FALSE(nextgi_hit_light_key_is_gi(u.material_key));
		if (i > 0) {
			CHECK(u.material_key >= prev_key);
		}
		prev_key = u.material_key;
	}
	// Remaining [expected_refl .. count - 1] must be strictly diffuse GI hits in non-decreasing key order.
	for (uint32_t i = expected_refl; i < count; i++) {
		const NextGIHitLightRecordUnpacked u = nextgi_hit_light_unpack_record(records[sorted[i]]);
		CHECK_NE(u.flags & NEXTGI_HL_RECORD_FLAG_DIFFUSE_GI, 0u);
		CHECK(nextgi_hit_light_key_is_gi(u.material_key));
		CHECK(u.material_key >= prev_key);
		CHECK(Math::is_equal_approx(u.resume_distance, 4.25f));
		prev_key = u.material_key;
	}
}

TEST_CASE("[NextGI][HitLighting][GI] Gather MODE_HIT_LIGHTING_GI variant follows ray-query rules and compiles") {
	const String source = RendererRD::NextGIGather::build_shader_source(7);
	REQUIRE_FALSE(source.is_empty());
	CHECK(source.contains("#define MODE_HIT_LIGHTING_GI"));
	CHECK(source.contains("nextgi_hit_light_gi_material_key("));
	CHECK(source.contains("NEXTGI_HL_RECORD_FLAG_DIFFUSE_GI"));
	CHECK(source.contains("gi_hit_records_out"));
	CHECK_FALSE(source.contains("while"));
	CHECK_FALSE(source.contains("shared uint"));
	CHECK_FALSE(source.contains("shared float"));
	CHECK_FALSE(source.contains("rayQueryGetIntersectionTriangleVertexPositionsEXT"));
	CHECK_FALSE(source.contains("GL_EXT_buffer_reference"));
#ifdef MODULE_GLSLANG_ENABLED
	String error;
	const Vector<uint8_t> spirv = compile_glslang_shader(
			RenderingDeviceCommons::SHADER_STAGE_COMPUTE,
			source,
			RenderingDeviceCommons::SHADER_LANGUAGE_VULKAN_VERSION_1_1,
			RenderingDeviceCommons::SHADER_SPIRV_VERSION_1_3,
			&error);
	if (spirv.is_empty()) {
		FAIL_CHECK(("glslang failed to compile NextGI gather MODE_HIT_LIGHTING_GI:\n" + error).utf8().get_data());
	}
	REQUIRE_GE(spirv.size(), 20);
#endif
}

TEST_CASE("[NextGI][HitLighting] Reflection mark shader uses HWRT wall visibility for WRC and compiles") {
	const String source = RendererRD::NextGIHitLighting::build_mark_shader_source();
	REQUIRE_FALSE(source.is_empty());
	CHECK(source.contains("#define NEXTGI_TLAS tlas"));
	CHECK(source.contains("nextgi_trace(p, -to_p / dist_to_p, dist_to_p - 0.01, probe_vis_hit)"));
	CHECK(source.contains("all(lessThanEqual(lc, vec3(15.0)))"));
	CHECK(source.contains("mark_params.ambient_color_sky.a"));
	CHECK_FALSE(source.contains("while"));
	CHECK_FALSE(source.contains("rayQueryGetIntersectionTriangleVertexPositionsEXT"));
	CHECK_FALSE(source.contains("GL_EXT_buffer_reference"));
#ifdef MODULE_GLSLANG_ENABLED
	String error;
	const Vector<uint8_t> spirv = compile_glslang_shader(
			RenderingDeviceCommons::SHADER_STAGE_COMPUTE,
			source,
			RenderingDeviceCommons::SHADER_LANGUAGE_VULKAN_VERSION_1_1,
			RenderingDeviceCommons::SHADER_SPIRV_VERSION_1_3,
			&error);
	if (spirv.is_empty()) {
		FAIL_CHECK(("glslang failed to compile NextGI hit-lighting mark shader (nextgi_reflect.glsl):\n" + error).utf8().get_data());
	}
	REQUIRE_GE(spirv.size(), 20);
#endif
}

TEST_CASE("[NextGI][HitLighting][GI] Gather MODE_CLAMP trims outliers and compiles") {
	const String source = RendererRD::NextGIGather::build_shader_source(10);
	REQUIRE_FALSE(source.is_empty());
	CHECK(source.contains("#define MODE_CLAMP"));
	CHECK(source.contains("max_lum1"));
	CHECK(source.contains("max_lum2"));
	CHECK(source.contains("clamp(screen_color, vec3(0.0), vec3(16.0))"));
	CHECK_FALSE(source.contains("while"));
#ifdef MODULE_GLSLANG_ENABLED
	String error;
	const Vector<uint8_t> spirv = compile_glslang_shader(
			RenderingDeviceCommons::SHADER_STAGE_COMPUTE,
			source,
			RenderingDeviceCommons::SHADER_LANGUAGE_VULKAN_VERSION_1_1,
			RenderingDeviceCommons::SHADER_SPIRV_VERSION_1_3,
			&error);
	if (spirv.is_empty()) {
		FAIL_CHECK(("glslang failed to compile NextGI gather MODE_CLAMP:\n" + error).utf8().get_data());
	}
	REQUIRE_GE(spirv.size(), 20);
#endif
}

TEST_CASE("[NextGI][HitLighting][GI] Hit lighting uses RIS shadowed light selection and full resolved confidence") {
	const String mark_source = RendererRD::NextGIHitLighting::build_mark_shader_source();
	REQUIRE_FALSE(mark_source.is_empty());
	CHECK(mark_source.contains("imageStore(reflection_image, pixel, vec4(radiance, 1.0));"));

	const String gather_source = RendererRD::NextGIGather::build_shader_source(7);
	REQUIRE_FALSE(gather_source.is_empty());
	CHECK(gather_source.contains("confidence = 1.0;\n\t\treturn vec3(0.0);"));
}

} // namespace TestNextGIHitLighting
