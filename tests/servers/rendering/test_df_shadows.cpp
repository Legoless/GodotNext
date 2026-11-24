/**************************************************************************/
/*  test_df_shadows.cpp                                                   */
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

TEST_FORCE_LINK(test_df_shadows)

#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "scene/3d/light_3d.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/storage/virtual_shadow_map_storage.h"

#include <cmath>

// GodotNext: df_shadows (#214, plan §21.3 contract S7, GV-10).
// CPU unit tests for distance-field shadows: directional 10% blend-band math and C1 continuity,
// directional handoff clamping and VSM non-Nanite far-caster dropping, cone-trace soft-shadow
// penumbra math against analytic spheres, depth- and normal-aware 2x2 bilateral upsample, layer
// budget clamping and flag packing, PS SDF tier fallback rules, and Light3D inspector surface.

namespace TestDFShadows {

using VSM = RendererVirtualShadowMapStorage;

TEST_CASE("[DFShadows] Directional blend-band math and C1 continuity (#214)") {
	// Invalid or disabled configurations produce 0 (no DF blend).
	CHECK(VSM::df_directional_blend(25.0f, 0.0f, 100.0f) == doctest::Approx(0.0f));
	CHECK(VSM::df_directional_blend(25.0f, -5.0f, 100.0f) == doctest::Approx(0.0f));
	CHECK(VSM::df_directional_blend(25.0f, 50.0f, 50.0f) == doctest::Approx(0.0f));
	CHECK(VSM::df_directional_blend(25.0f, 50.0f, 30.0f) == doctest::Approx(0.0f));
	CHECK(VSM::df_directional_blend(NAN, 50.0f, 200.0f) == doctest::Approx(0.0f));

	const float start = 50.0f;
	const float distance = 200.0f;
	// 10% blend band spans [45.0, 50.0].
	CHECK(VSM::df_directional_blend(0.0f, start, distance) == doctest::Approx(0.0f));
	CHECK(VSM::df_directional_blend(44.99f, start, distance) == doctest::Approx(0.0f));
	CHECK(VSM::df_directional_blend(45.0f, start, distance) == doctest::Approx(0.0f));
	CHECK(VSM::df_directional_blend(47.5f, start, distance) == doctest::Approx(0.5f));
	CHECK(VSM::df_directional_blend(50.0f, start, distance) == doctest::Approx(1.0f));
	CHECK(VSM::df_directional_blend(120.0f, start, distance) == doctest::Approx(1.0f));
	CHECK(VSM::df_directional_blend(200.0f, start, distance) == doctest::Approx(1.0f));
	CHECK(VSM::df_directional_blend(200.01f, start, distance) == doctest::Approx(1.0f));

	// Smoothstep C1 continuity and monotonicity across the 10% handoff band.
	float prev = 0.0f;
	float max_step = 0.0f;
	const int steps = 100;
	for (int i = 0; i <= steps; i++) {
		const float z = 44.5f + 6.0f * (float(i) / float(steps));
		const float b = VSM::df_directional_blend(z, start, distance);
		CHECK(b >= prev - 1e-6f);
		CHECK(b >= 0.0f);
		CHECK(b <= 1.0f);
		max_step = MAX(max_step, b - prev);
		prev = b;
	}
	// Derivative vanishes at both endpoints (smoothstep 3t^2 - 2t^3).
	CHECK(VSM::df_directional_blend(45.01f, start, distance) < 1e-4f);
	CHECK(1.0f - VSM::df_directional_blend(49.99f, start, distance) < 1e-4f);
	CHECK(max_step < 0.025f);
	// Antisymmetry around midpoint: b(mid - d) + b(mid + d) == 1.
	for (float offset : { 0.5f, 1.25f, 2.0f }) {
		const float lo = VSM::df_directional_blend(47.5f - offset, start, distance);
		const float hi = VSM::df_directional_blend(47.5f + offset, start, distance);
		CHECK(lo + hi == doctest::Approx(1.0f).epsilon(1e-5));
	}
}

TEST_CASE("[DFShadows] Directional handoff distance clamping and VSM far-caster culling (#214)") {
	CHECK(VSM::df_directional_handoff_start(30.0f, 150.0f, 100.0f, true) == doctest::Approx(30.0f));
	CHECK(VSM::df_directional_handoff_start(30.0f, 150.0f, 100.0f, false) == doctest::Approx(0.0f));
	// When start == 0 and distance > shadow_max_distance > 0, hands off at shadow_max_distance.
	CHECK(VSM::df_directional_handoff_start(0.0f, 150.0f, 100.0f, true) == doctest::Approx(100.0f));
	CHECK(VSM::df_directional_handoff_start(0.0f, 80.0f, 100.0f, true) == doctest::Approx(0.0f));
	CHECK(VSM::df_directional_handoff_start(30.0f, 30.0f, 100.0f, true) == doctest::Approx(0.0f));
	CHECK(VSM::df_directional_handoff_start(30.0f, 10.0f, 100.0f, true) == doctest::Approx(0.0f));
	CHECK(VSM::df_directional_handoff_start(0.0f, 0.0f, 100.0f, true) == doctest::Approx(0.0f));

	const Vector3 cam(0.0f, 5.0f, 0.0f);
	const float handoff = 20.0f;
	const AABB near_box(Vector3(-1.0f, 0.0f, -10.0f), Vector3(2.0f, 2.0f, 2.0f));
	const AABB straddle_box(Vector3(-1.0f, 0.0f, -25.0f), Vector3(2.0f, 2.0f, 10.0f)); // Nearest z = -15 (dist ~15.8 <= 20).
	const AABB far_box(Vector3(-1.0f, 0.0f, -45.0f), Vector3(2.0f, 2.0f, 2.0f)); // Nearest z = -43 (dist > 43 > 20).

	CHECK_FALSE(VSM::df_directional_caster_culled(near_box, cam, handoff));
	CHECK_FALSE(VSM::df_directional_caster_culled(straddle_box, cam, handoff));
	CHECK(VSM::df_directional_caster_culled(far_box, cam, handoff));
	CHECK_FALSE(VSM::df_directional_caster_culled(far_box, cam, 0.0f));

	// Integration with cull_box_dirty_pages(): far directional casters are dropped and counted.
	const Basis light_basis = Basis::looking_at(Vector3(0.0f, -1.0f, -0.3f).normalized(), Vector3(0.0f, 0.0f, -1.0f));
	const Vector2 light_center(float(light_basis.get_column(0).dot(cam)), float(light_basis.get_column(1).dot(cam)));
	const Vector2i level_origin = VSM::clipmap_level_origin(light_center, 5);
	const VSM::MapData dir_map = VSM::map_data_clipmap(light_basis, 5, level_origin, 128.0f);

	// Mark all 4 mip-5 pages dirty so every point in the level-5 window (128 m square) hits a dirty page.
	VSM::DirtyPageRecord dirty[4];
	for (uint32_t i = 0; i < 4; i++) {
		dirty[i].map = 0;
		dirty[i].packed_page = 5u | ((i & 1u) << 8u) | (((i >> 1u) & 1u) << 16u);
		dirty[i].phys_entry = VSM::ENTRY_ALLOCATED;
		dirty[i].flags = VSM::ENTRY_DIRTY_STATIC;
	}

	const uint32_t map_dirty_counts[1] = { 4u };
	const uint32_t map_dirty_offsets[1] = { 0u };
	const uint32_t dir_flags = VSM::light_flags(VSM::MAP_KIND_DIRECTIONAL, 1u);

	LocalVector<uint32_t> out_dirty;
	uint32_t small_culled = 0;
	uint32_t df_far_culled = 0;

	// With handoff active at 20 m: near caster emits dirty page(s), far caster is dropped.
	VSM::cull_box_dirty_pages(&dir_map, 1, map_dirty_counts, map_dirty_offsets, dirty, 4, 0, dir_flags, near_box, false, 0.0f, out_dirty, small_culled, cam, handoff, &df_far_culled);
	CHECK(out_dirty.size() >= 1);
	CHECK(df_far_culled == 0);

	VSM::cull_box_dirty_pages(&dir_map, 1, map_dirty_counts, map_dirty_offsets, dirty, 4, 0, dir_flags, far_box, false, 0.0f, out_dirty, small_culled, cam, handoff, &df_far_culled);
	CHECK(out_dirty.is_empty());
	CHECK(df_far_culled == 1);

	// With handoff disabled (0.0): far caster stays in VSM.
	VSM::cull_box_dirty_pages(&dir_map, 1, map_dirty_counts, map_dirty_offsets, dirty, 4, 0, dir_flags, far_box, false, 0.0f, out_dirty, small_culled, cam, 0.0f, &df_far_culled);
	CHECK(out_dirty.size() >= 1);
	CHECK(df_far_culled == 0);

	// Local lights are never far-culled by directional handoff.
	const VSM::MapData spot_map = VSM::map_data_face(VSM::MAP_KIND_SPOT, 0, Transform3D(Basis::looking_at(Vector3(0.0f, -1.0f, 0.0f), Vector3(0.0f, 0.0f, -1.0f)), Vector3(0.0f, 10.0f, -44.0f)), 60.0f, 60.0f);
	const uint32_t spot_flags = VSM::light_flags(VSM::MAP_KIND_SPOT, 1u);
	VSM::cull_box_dirty_pages(&spot_map, 1, map_dirty_counts, map_dirty_offsets, dirty, 4, 0, spot_flags, far_box, false, 0.0f, out_dirty, small_culled, cam, handoff, &df_far_culled);
	CHECK(df_far_culled == 0);
}

TEST_CASE("[DFShadows] Cone-trace penumbra and soft-shadow math against analytic sphere occluders (#214)") {
	// Step visibility function invariants.
	CHECK(VSM::df_cone_visibility_step(0.0f, 5.0f, 0.1f, 0.05f) == doctest::Approx(0.0f));
	CHECK(VSM::df_cone_visibility_step(-0.2f, 5.0f, 0.1f, 0.05f) == doctest::Approx(0.0f));
	CHECK(VSM::df_cone_visibility_step(0.25f, 5.0f, 0.1f, 0.05f) == doctest::Approx(0.5f)); // cone_r = 0.5, u = 0.5.
	CHECK(VSM::df_cone_visibility_step(0.5f, 5.0f, 0.1f, 0.05f) == doctest::Approx(1.0f));
	CHECK(VSM::df_cone_visibility_step(1.0f, 5.0f, 0.1f, 0.05f) == doctest::Approx(1.0f));

	const Vector3 sphere_center(0.0f, 5.0f, 0.0f);
	const float sphere_radius = 1.0f;
	const Vector3 up(0.0f, 1.0f, 0.0f);

	// Direct hit through sphere center -> full umbra (0.0).
	CHECK(VSM::df_cone_trace_analytic_sphere(Vector3(0.0f, 0.0f, 0.0f), up, 20.0f, 0.05f, sphere_center, sphere_radius) == doctest::Approx(0.0f));
	// Far miss -> full light (1.0).
	CHECK(VSM::df_cone_trace_analytic_sphere(Vector3(10.0f, 0.0f, 0.0f), up, 20.0f, 0.05f, sphere_center, sphere_radius) == doctest::Approx(1.0f));
	// Occluder beyond light distance (tmax < sphere distance) -> unoccluded (1.0).
	CHECK(VSM::df_cone_trace_analytic_sphere(Vector3(0.0f, 0.0f, 0.0f), up, 3.0f, 0.05f, sphere_center, sphere_radius) == doctest::Approx(1.0f));
	// Occluder behind ray origin -> unoccluded (1.0).
	CHECK(VSM::df_cone_trace_analytic_sphere(Vector3(0.0f, 8.0f, 0.0f), up, 20.0f, 0.05f, sphere_center, sphere_radius) == doctest::Approx(1.0f));

	// Grazing penumbra: x = 1.25 is 0.25 m outside the sphere surface at t = 5 m.
	// With cone_tan = 0.1 (cone_r = 0.5 m at t = 5 m), visibility is strictly in (0, 1).
	const float vis_narrow = VSM::df_cone_trace_analytic_sphere(Vector3(1.25f, 0.0f, 0.0f), up, 20.0f, 0.02f, sphere_center, sphere_radius);
	const float vis_medium = VSM::df_cone_trace_analytic_sphere(Vector3(1.25f, 0.0f, 0.0f), up, 20.0f, 0.10f, sphere_center, sphere_radius);
	const float vis_wide = VSM::df_cone_trace_analytic_sphere(Vector3(1.25f, 0.0f, 0.0f), up, 20.0f, 0.25f, sphere_center, sphere_radius);
	CHECK(vis_narrow == doctest::Approx(1.0f)); // 0.25 > 0.02 * 5 = 0.10 -> outside narrow cone.
	CHECK(vis_medium > 0.05f);
	CHECK(vis_medium < 0.95f);
	CHECK(vis_wide < vis_medium); // Wider cone (larger light_size) produces softer/wider penumbra.
	CHECK(vis_wide > 0.0f);

	// Penumbra is monotonically non-decreasing moving outward from x = 0.5 (umbra) to x = 3.0 (lit).
	float prev = 0.0f;
	for (int i = 0; i <= 25; i++) {
		const float x = 0.5f + 0.1f * float(i);
		const float v = VSM::df_cone_trace_analytic_sphere(Vector3(x, 0.0f, 0.0f), up, 20.0f, 0.15f, sphere_center, sphere_radius);
		CHECK(v >= prev - 1e-4f);
		CHECK(v >= 0.0f);
		CHECK(v <= 1.0f);
		prev = v;
	}
	CHECK(prev == doctest::Approx(1.0f));
}

TEST_CASE("[DFShadows] Depth- and normal-aware 2x2 bilateral upsample (#214)") {
	const Vector3 up(0.0f, 1.0f, 0.0f);
	const Vector3 side(0.0f, 0.0f, 1.0f);

	// Flat surface: identical depth and normal across all 4 taps -> exact bilinear average.
	{
		const float masks[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
		const float depths[4] = { 10.0f, 10.0f, 10.0f, 10.0f };
		const Vector3 normals[4] = { up, up, up, up };
		CHECK(VSM::df_upsample_2x2(10.0f, up, Vector2(0.5f, 0.5f), masks, depths, normals) == doctest::Approx(0.5f));
		CHECK(VSM::df_upsample_2x2(10.0f, up, Vector2(0.25f, 0.5f), masks, depths, normals) == doctest::Approx(0.25f));
		CHECK(VSM::df_upsample_2x2(10.0f, up, Vector2(0.75f, 0.5f), masks, depths, normals) == doctest::Approx(0.75f));
	}

	// Depth discontinuity: left taps at 10 m (shadowed 0.0), right taps at 30 m (lit 1.0).
	{
		const float masks[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
		const float depths[4] = { 10.0f, 30.0f, 10.0f, 30.0f };
		const Vector3 normals[4] = { up, up, up, up };
		const float fg = VSM::df_upsample_2x2(10.0f, up, Vector2(0.5f, 0.5f), masks, depths, normals);
		const float bg = VSM::df_upsample_2x2(30.0f, up, Vector2(0.5f, 0.5f), masks, depths, normals);
		CHECK(fg < 0.01f); // Foreground stays shadowed without bleeding from background.
		CHECK(bg > 0.99f); // Background stays lit without bleeding from foreground.
	}

	// Normal crease at identical depth: top taps face up (0.0), bottom taps face side (1.0).
	{
		const float masks[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
		const float depths[4] = { 10.0f, 10.0f, 10.0f, 10.0f };
		const Vector3 normals[4] = { up, up, side, side };
		const float on_up = VSM::df_upsample_2x2(10.0f, up, Vector2(0.5f, 0.5f), masks, depths, normals);
		const float on_side = VSM::df_upsample_2x2(10.0f, side, Vector2(0.5f, 0.5f), masks, depths, normals);
		CHECK(on_up < 0.01f);
		CHECK(on_side > 0.99f);
	}

	// Sky/background taps (depth == 0) are ignored; all-sky falls back to unshadowed 1.0.
	{
		const float masks[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		const float depths[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		const Vector3 normals[4] = { up, up, up, up };
		CHECK(VSM::df_upsample_2x2(10.0f, up, Vector2(0.5f, 0.5f), masks, depths, normals) == doctest::Approx(1.0f));
	}

	// Nearest-depth fallback when all valid taps are far from the receiver depth.
	{
		const float masks[4] = { 0.2f, 0.9f, 0.9f, 0.9f };
		const float depths[4] = { 20.0f, 80.0f, 80.0f, 80.0f };
		const Vector3 normals[4] = { up, up, up, up };
		const float result = VSM::df_upsample_2x2(5.0f, up, Vector2(0.5f, 0.5f), masks, depths, normals);
		CHECK(Math::is_finite(result));
		CHECK(result == doctest::Approx(0.2f));
	}
}

TEST_CASE("[DFShadows] Layer budget clamping and non-overlapping flag packing (#214)") {
	CHECK(VSM::DF_MAX_LIGHTS_PER_VIEW == 8u);
	CHECK(VSM::df_preset_max_lights(VSM::POOL_QUALITY_LOW) == 2u);
	CHECK(VSM::df_preset_max_lights(VSM::POOL_QUALITY_MEDIUM) == 4u);
	CHECK(VSM::df_preset_max_lights(VSM::POOL_QUALITY_HIGH) == 8u);
	CHECK(VSM::df_preset_max_lights(VSM::POOL_QUALITY_EPIC) == 8u);
	CHECK(VSM::df_preset_max_steps(VSM::POOL_QUALITY_LOW) == 32u);
	CHECK(VSM::df_preset_max_steps(VSM::POOL_QUALITY_MEDIUM) == 48u);
	CHECK(VSM::df_preset_max_steps(VSM::POOL_QUALITY_HIGH) == 64u);
	CHECK(VSM::df_preset_max_steps(VSM::POOL_QUALITY_EPIC) == 64u);

	CHECK(VSM::df_effective_max_lights(-1, VSM::POOL_QUALITY_LOW) == 2u);
	CHECK(VSM::df_effective_max_lights(-1, VSM::POOL_QUALITY_HIGH) == 8u);
	CHECK(VSM::df_effective_max_lights(0, VSM::POOL_QUALITY_HIGH) == 0u);
	CHECK(VSM::df_effective_max_lights(5, VSM::POOL_QUALITY_LOW) == 5u);
	CHECK(VSM::df_effective_max_lights(99, VSM::POOL_QUALITY_EPIC) == VSM::DF_MAX_LIGHTS_PER_VIEW);

	// DF flag packing round-trips all layers 0..7 and never collides with VSM map/sample flags.
	CHECK(VSM::df_pack_flags(VSM::DF_LAYER_NONE, false) == 0u);
	CHECK(VSM::df_pack_flags(VSM::DF_LAYER_NONE, true) == 0u);
	CHECK(VSM::df_pack_flags(VSM::DF_MAX_LIGHTS_PER_VIEW, false) == 0u);

	const uint32_t vsm_base_flags = VSM::light_flags(VSM::MAP_KIND_DIRECTIONAL, 16u) | VSM::LIGHT_FLAGS_SAMPLE;
	for (uint32_t layer = 0; layer < VSM::DF_MAX_LIGHTS_PER_VIEW; layer++) {
		const uint32_t packed_handoff = VSM::df_pack_flags(layer, false);
		const uint32_t packed_full = VSM::df_pack_flags(layer, true);
		CHECK((packed_handoff & vsm_base_flags) == 0u);
		CHECK((packed_full & vsm_base_flags) == 0u);
		const uint32_t combined_handoff = vsm_base_flags | packed_handoff;
		const uint32_t combined_full = vsm_base_flags | packed_full;
		CHECK(VSM::df_layer(combined_handoff) == layer);
		CHECK(VSM::df_layer(combined_full) == layer);
		CHECK((combined_handoff & VSM::LIGHT_FLAGS_DF_SAMPLE) != 0u);
		CHECK((combined_handoff & VSM::LIGHT_FLAGS_DF_FULL) == 0u);
		CHECK((combined_full & VSM::LIGHT_FLAGS_DF_FULL) != 0u);
		CHECK((combined_handoff & VSM::LIGHT_FLAGS_KIND_MASK) == VSM::MAP_KIND_DIRECTIONAL);
		CHECK(((combined_handoff >> VSM::LIGHT_FLAGS_COUNT_SHIFT) & VSM::LIGHT_FLAGS_COUNT_MASK) == 16u);
		CHECK((combined_handoff & VSM::LIGHT_FLAGS_SAMPLE) != 0u);
	}
}

TEST_CASE("[DFShadows] Tier rule, fallback reasons and method resolution (#214)") {
	VSM::DFTierInputs in;
	in.forward_plus = false;
	in.mesh_sdf_enabled = true;
	in.nextgi_hwrt = false;
	in.keep_when_hwrt = true;
	CHECK(VSM::evaluate_df_tier(in) == VSM::DF_TIER_REASON_UNSUPPORTED_RENDERER);
	CHECK(String(VSM::get_df_tier_reason_name(VSM::DF_TIER_REASON_UNSUPPORTED_RENDERER)) == "UNSUPPORTED_RENDERER");

	in.forward_plus = true;
	in.mesh_sdf_enabled = false;
	CHECK(VSM::evaluate_df_tier(in) == VSM::DF_TIER_REASON_MESH_SDF_DISABLED);
	CHECK(String(VSM::get_df_tier_reason_name(VSM::DF_TIER_REASON_MESH_SDF_DISABLED)) == "MESH_SDF_DISABLED");

	in.mesh_sdf_enabled = true;
	in.nextgi_hwrt = true;
	in.keep_when_hwrt = false;
	CHECK(VSM::evaluate_df_tier(in) == VSM::DF_TIER_REASON_DROPPED_FOR_HWRT);
	CHECK(String(VSM::get_df_tier_reason_name(VSM::DF_TIER_REASON_DROPPED_FOR_HWRT)) == "DROPPED_FOR_HWRT");

	in.keep_when_hwrt = true;
	CHECK(VSM::evaluate_df_tier(in) == VSM::DF_TIER_REASON_NONE);
	CHECK(String(VSM::get_df_tier_reason_name(VSM::DF_TIER_REASON_NONE)) == "NONE");

	in.nextgi_hwrt = false;
	in.keep_when_hwrt = false;
	CHECK(VSM::evaluate_df_tier(in) == VSM::DF_TIER_REASON_NONE);

	// Fallback line reports once per process and resets cleanly.
	VSM::reset_startup_state();
	const uint32_t lines_before = VSM::get_df_fallback_line_count();
	ERR_PRINT_OFF;
	CHECK(VSM::report_df_fallback());
	CHECK_FALSE(VSM::report_df_fallback());
	ERR_PRINT_ON;
	CHECK(VSM::get_df_fallback_line_count() - lines_before == 1u);
	VSM::reset_startup_state();
	ERR_PRINT_OFF;
	CHECK(VSM::report_df_fallback());
	ERR_PRINT_ON;
	CHECK(VSM::get_df_fallback_line_count() - lines_before == 2u);
	VSM::reset_startup_state();

	// Fallback formatting includes the reason code.
	CHECK(VSM::format_df_fallback_line(VSM::DF_TIER_REASON_MESH_SDF_DISABLED).contains("MESH_SDF_DISABLED"));
	CHECK(VSM::format_df_fallback_line(VSM::DF_TIER_REASON_DROPPED_FOR_HWRT).contains("DROPPED_FOR_HWRT"));
	CHECK(VSM::format_df_fallback_line(VSM::DF_TIER_REASON_UNSUPPORTED_RENDERER).contains("UNSUPPORTED_RENDERER"));
}

TEST_CASE("[DFShadows][SceneTree] Light3D distance-field properties, inspector visibility and warnings (#214)") {
	DirectionalLight3D *dir = memnew(DirectionalLight3D);
	OmniLight3D *omni = memnew(OmniLight3D);
	SpotLight3D *spot = memnew(SpotLight3D);
	AreaLight3D *area = memnew(AreaLight3D);

	// Defaults are 0.0 (DF far-shadow handoff off by default).
	CHECK(dir->get_param(Light3D::PARAM_DISTANCE_FIELD_SHADOW_START) == doctest::Approx(0.0f));
	CHECK(dir->get_param(Light3D::PARAM_DISTANCE_FIELD_SHADOW_DISTANCE) == doctest::Approx(0.0f));
	CHECK(double(dir->get("distance_field_shadow_start")) == doctest::Approx(0.0));
	CHECK(double(dir->get("distance_field_shadow_distance")) == doctest::Approx(0.0));

	// Round-trip through both property API and Param enum.
	dir->set("distance_field_shadow_start", 35.0);
	dir->set("distance_field_shadow_distance", 250.0);
	CHECK(dir->get_param(Light3D::PARAM_DISTANCE_FIELD_SHADOW_START) == doctest::Approx(35.0f));
	CHECK(dir->get_param(Light3D::PARAM_DISTANCE_FIELD_SHADOW_DISTANCE) == doctest::Approx(250.0f));
	dir->set_param(Light3D::PARAM_DISTANCE_FIELD_SHADOW_START, 50.0f);
	dir->set_param(Light3D::PARAM_DISTANCE_FIELD_SHADOW_DISTANCE, 500.0f);
	CHECK(double(dir->get("distance_field_shadow_start")) == doctest::Approx(50.0));
	CHECK(double(dir->get("distance_field_shadow_distance")) == doctest::Approx(500.0));

	// Inspector property visibility: shown only on DirectionalLight3D, hidden on local lights.
	auto has_editor_prop = [](const Object *p_obj, const StringName &p_name) -> bool {
		List<PropertyInfo> props;
		p_obj->get_property_list(&props);
		for (const PropertyInfo &pi : props) {
			if (pi.name == p_name) {
				return (pi.usage & PROPERTY_USAGE_EDITOR) != 0;
			}
		}
		return false;
	};
	CHECK(has_editor_prop(dir, "distance_field_shadow_start"));
	CHECK(has_editor_prop(dir, "distance_field_shadow_distance"));
	CHECK_FALSE(has_editor_prop(omni, "distance_field_shadow_start"));
	CHECK_FALSE(has_editor_prop(omni, "distance_field_shadow_distance"));
	CHECK_FALSE(has_editor_prop(spot, "distance_field_shadow_start"));
	CHECK_FALSE(has_editor_prop(spot, "distance_field_shadow_distance"));
	CHECK_FALSE(has_editor_prop(area, "distance_field_shadow_start"));
	CHECK_FALSE(has_editor_prop(area, "distance_field_shadow_distance"));

	// Tier info exposes DF status and reason cleanly on the dummy renderer.
	RenderingServer *rs = RenderingServer::get_singleton();
	REQUIRE(rs != nullptr);
	const Dictionary tier = rs->virtual_shadow_maps_get_tier_info();
	CHECK(tier.has("df_available"));
	CHECK(tier.has("df_reason"));
	CHECK(tier.has("df_fallback_lines"));
	CHECK(tier.has("df_max_lights"));
	CHECK_FALSE(bool(tier["df_available"]));
	CHECK(String(tier["df_reason"]) == "UNSUPPORTED_RENDERER");

	// Configuration warnings on dummy renderer (DF tier unavailable).
	dir->set_shadow(true);
	const PackedStringArray dir_warnings = dir->get_configuration_warnings();
	bool found_handoff_warn = false;
	for (const String &w : dir_warnings) {
		if (w.contains("distance-field") && w.contains("UNSUPPORTED_RENDERER")) {
			found_handoff_warn = true;
		}
	}
	CHECK(found_handoff_warn);

	omni->set_shadow(true);
	omni->set_shadow_method(Light3D::SHADOW_METHOD_DISTANCE_FIELD);
	const PackedStringArray omni_warnings = omni->get_configuration_warnings();
	bool found_local_warn = false;
	for (const String &w : omni_warnings) {
		if (w.contains("Distance-field") && w.contains("UNSUPPORTED_RENDERER")) {
			found_local_warn = true;
		}
	}
	CHECK(found_local_warn);

	memdelete(dir);
	memdelete(omni);
	memdelete(spot);
	memdelete(area);
}

} // namespace TestDFShadows
