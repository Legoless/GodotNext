/**************************************************************************/
/*  test_rt_shadows.cpp                                                   */
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

TEST_FORCE_LINK(test_rt_shadows)

#include "core/config/project_settings.h"
#include "core/math/projection.h"
#include "scene/3d/light_3d.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_shadows.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_forward_clustered.h"
#include "servers/rendering/renderer_scene_cull.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/storage/virtual_shadow_map_storage.h"

namespace {

using VSM = RendererVirtualShadowMapStorage;

bool warnings_contain(const PackedStringArray &p_warnings, const String &p_needle) {
	for (const String &w : p_warnings) {
		if (w.contains(p_needle)) {
			return true;
		}
	}
	return false;
}

} // namespace

TEST_CASE("[RTShadows] Setting default is false and tier info exposes RT fields (#141)") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);
	CHECK(ps->has_setting(VSM::SETTING_RAY_TRACED_SHADOWS));
	CHECK(bool(ps->get_setting(VSM::SETTING_RAY_TRACED_SHADOWS)) == false);
	CHECK_FALSE(VSM::read_ray_traced_shadows());

	RenderingServer *rs = RenderingServer::get_singleton();
	REQUIRE(rs != nullptr);
	const Dictionary tier = rs->virtual_shadow_maps_get_tier_info();
	CHECK(tier.has("rt_available"));
	CHECK(tier.has("rt_reason"));
	CHECK(tier.has("ray_traced_shadows"));
	CHECK(tier.has("rt_fallback_lines"));
	CHECK(bool(tier["ray_traced_shadows"]) == false);
}

TEST_CASE("[RTShadows] RT tier rule and per-light shadow method resolution across HWRT, SWRT and off (#141)") {
	// Tier rule checks Forward+ renderer and NextGI HWRT active.
	{
		VSM::RTTierInputs in;
		in.forward_plus = false;
		in.nextgi_hwrt = true;
		CHECK(VSM::evaluate_rt_tier(in) == VSM::RT_TIER_REASON_UNSUPPORTED_RENDERER);
	}
	{
		VSM::RTTierInputs in;
		in.forward_plus = true;
		in.nextgi_hwrt = false;
		CHECK(VSM::evaluate_rt_tier(in) == VSM::RT_TIER_REASON_HWRT_UNAVAILABLE);
	}
	{
		VSM::RTTierInputs in;
		in.forward_plus = true;
		in.nextgi_hwrt = true;
		CHECK(VSM::evaluate_rt_tier(in) == VSM::RT_TIER_REASON_NONE);
	}
	CHECK(String(VSM::get_rt_tier_reason_name(VSM::RT_TIER_REASON_NONE)) == "NONE");
	CHECK(String(VSM::get_rt_tier_reason_name(VSM::RT_TIER_REASON_UNSUPPORTED_RENDERER)) == "UNSUPPORTED_RENDERER");
	CHECK(String(VSM::get_rt_tier_reason_name(VSM::RT_TIER_REASON_HWRT_UNAVAILABLE)) == "HWRT_UNAVAILABLE");

	// Per-light method resolution across HWRT on/off, ray_traced_shadows on/off, VSM/Atlas project defaults.
	for (VSM::ShadowMethod proj : { VSM::SHADOW_METHOD_ATLAS, VSM::SHADOW_METHOD_VIRTUAL }) {
		const RSE::LightShadowMethod proj_fallback = (proj == VSM::SHADOW_METHOD_VIRTUAL) ? RSE::LIGHT_SHADOW_METHOD_VIRTUAL : RSE::LIGHT_SHADOW_METHOD_ATLAS;

		// HWRT active, global ray_traced_shadows = false:
		// RAY_TRACED overrides per light; PROJECT follows shadow_method (VSM/Atlas).
		CHECK(VSM::resolve_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_RAY_TRACED, proj, true, true, true, false) == RSE::LIGHT_SHADOW_METHOD_RAY_TRACED);
		CHECK(VSM::resolve_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_RAY_TRACED, proj, false, false, true, false) == RSE::LIGHT_SHADOW_METHOD_RAY_TRACED);
		CHECK(VSM::resolve_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_PROJECT, proj, true, true, true, false) == proj_fallback);
		CHECK(VSM::resolve_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_VIRTUAL, proj, true, true, true, false) == RSE::LIGHT_SHADOW_METHOD_VIRTUAL);
		CHECK(VSM::resolve_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_ATLAS, proj, true, true, true, false) == RSE::LIGHT_SHADOW_METHOD_ATLAS);
		CHECK(VSM::resolve_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_DISTANCE_FIELD, proj, true, true, true, false) == RSE::LIGHT_SHADOW_METHOD_DISTANCE_FIELD);

		// HWRT active, global ray_traced_shadows = true:
		// Both PROJECT and RAY_TRACED resolve to RAY_TRACED; explicit VIRTUAL/ATLAS/DISTANCE_FIELD keep their override.
		CHECK(VSM::resolve_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_PROJECT, proj, true, true, true, true) == RSE::LIGHT_SHADOW_METHOD_RAY_TRACED);
		CHECK(VSM::resolve_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_PROJECT, proj, false, false, true, true) == RSE::LIGHT_SHADOW_METHOD_RAY_TRACED);
		CHECK(VSM::resolve_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_RAY_TRACED, proj, true, true, true, true) == RSE::LIGHT_SHADOW_METHOD_RAY_TRACED);
		CHECK(VSM::resolve_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_VIRTUAL, proj, true, true, true, true) == RSE::LIGHT_SHADOW_METHOD_VIRTUAL);
		CHECK(VSM::resolve_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_ATLAS, proj, true, true, true, true) == RSE::LIGHT_SHADOW_METHOD_ATLAS);
		CHECK(VSM::resolve_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_DISTANCE_FIELD, proj, true, true, true, true) == RSE::LIGHT_SHADOW_METHOD_DISTANCE_FIELD);

		// HWRT unavailable (SWRT or NextGI off):
		// Both RAY_TRACED and PROJECT (even with ray_traced_shadows = true) fall back cleanly to VSM (if available) or Atlas.
		CHECK(VSM::resolve_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_RAY_TRACED, proj, true, true, false, true) == proj_fallback);
		CHECK(VSM::resolve_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_RAY_TRACED, proj, false, true, false, true) == RSE::LIGHT_SHADOW_METHOD_ATLAS);
		CHECK(VSM::resolve_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_PROJECT, proj, true, true, false, true) == proj_fallback);
		CHECK(VSM::resolve_light_shadow_method(RSE::LIGHT_SHADOW_METHOD_PROJECT, proj, false, true, false, true) == RSE::LIGHT_SHADOW_METHOD_ATLAS);
	}
}

TEST_CASE("[RTShadows][SceneTree] Light3D configuration warnings for SHADOW_METHOD_RAY_TRACED (#141)") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);
	const Variant saved_rt = ps->get_setting(VSM::SETTING_RAY_TRACED_SHADOWS, false);

	OmniLight3D *light = memnew(OmniLight3D);
	light->set_shadow(true);
	light->set_shadow_method(Light3D::SHADOW_METHOD_RAY_TRACED);

	// HWRT unavailable on Forward+ -> warns with HWRT_UNAVAILABLE, never says "not implemented yet".
	VSM::reset_startup_state();
	VSM::set_startup_state(VSM::TIER_REASON_NONE, VSM::SHADOW_METHOD_ATLAS, false);
	VSM::set_df_nextgi_hwrt(false);
	PackedStringArray warnings = light->get_configuration_warnings();
	CHECK_FALSE(warnings_contain(warnings, "not implemented yet"));
	CHECK(warnings_contain(warnings, "Ray-traced shadows are not available"));
	CHECK(warnings_contain(warnings, "HWRT_UNAVAILABLE"));

	// HWRT active on Forward+ -> zero RT warnings.
	VSM::set_df_nextgi_hwrt(true);
	warnings = light->get_configuration_warnings();
	CHECK_FALSE(warnings_contain(warnings, "Ray-traced shadows are not available"));
	CHECK_FALSE(warnings_contain(warnings, "not implemented yet"));

	// Global ray_traced_shadows = true with PROJECT light: warns when HWRT unavailable, quiet when HWRT active.
	ps->set_setting(VSM::SETTING_RAY_TRACED_SHADOWS, true);
	light->set_shadow_method(Light3D::SHADOW_METHOD_PROJECT);
	VSM::set_df_nextgi_hwrt(false);
	warnings = light->get_configuration_warnings();
	CHECK(warnings_contain(warnings, "Ray-traced shadows are not available"));
	VSM::set_df_nextgi_hwrt(true);
	warnings = light->get_configuration_warnings();
	CHECK_FALSE(warnings_contain(warnings, "Ray-traced shadows are not available"));

	memdelete(light);
	ps->set_setting(VSM::SETTING_RAY_TRACED_SHADOWS, saved_rt);
	VSM::set_df_nextgi_hwrt(false);
	VSM::reset_startup_state();
}

TEST_CASE("[RTShadows] Tile-sparse atlas sizing, preset budgets, 64+ lights and overflow fallback counting (#141)") {
	// Preset tile budgets and R8 atlas VRAM sizes (plan budget M-7: High = 65,536 tiles = 16 MiB).
	CHECK(RendererRD::nextgi_rt_shadow_preset_tile_budget(VSM::POOL_QUALITY_LOW) == 16384u);
	CHECK(RendererRD::nextgi_rt_shadow_preset_tile_budget(VSM::POOL_QUALITY_MEDIUM) == 32768u);
	CHECK(RendererRD::nextgi_rt_shadow_preset_tile_budget(VSM::POOL_QUALITY_HIGH) == 65536u);
	CHECK(RendererRD::nextgi_rt_shadow_preset_tile_budget(VSM::POOL_QUALITY_EPIC) == 131072u);

	CHECK(RendererRD::nextgi_rt_shadow_r8_atlas_bytes(16384u) == uint64_t(4u) * 1024u * 1024u);
	CHECK(RendererRD::nextgi_rt_shadow_r8_atlas_bytes(32768u) == uint64_t(8u) * 1024u * 1024u);
	CHECK(RendererRD::nextgi_rt_shadow_r8_atlas_bytes(65536u) == uint64_t(16u) * 1024u * 1024u);
	CHECK(RendererRD::nextgi_rt_shadow_r8_atlas_bytes(131072u) == uint64_t(32u) * 1024u * 1024u);

	const Size2i high_atlas = RendererRD::nextgi_rt_shadow_atlas_size(65536u);
	CHECK(high_atlas == Size2i(4096, 4096));

	// 1080p full-screen directional light (120 x 68 = 8,160 tiles) + 64 local lights (each 600 tiles = 38,400 tiles).
	// Total = 46,560 tiles <= 65,536 -> ALL 65 lights fit with 0 overflow and 0 fallbacks!
	RendererRD::NextGIRTShadowBudget budget;
	budget.reset(RendererRD::nextgi_rt_shadow_preset_tile_budget(VSM::POOL_QUALITY_HIGH));
	CHECK(budget.try_allocate_light(120u * 68u));
	for (uint32_t i = 0; i < 64u; i++) {
		CHECK(budget.try_allocate_light(600u));
	}
	CHECK(budget.lights_accepted == 65u);
	CHECK(budget.tiles_used == 46560u);
	CHECK(budget.overflow_tiles == 0u);
	CHECK(budget.overflow_lights == 0u);
	CHECK(budget.fallback_lights == 0u);

	// Exceeding the 65,536-tile cap counts overflow tiles and overflow/fallback lights cleanly.
	uint32_t extra_overflow_lights = 0u;
	for (uint32_t i = 0; i < 40u; i++) {
		if (!budget.try_allocate_light(600u)) {
			extra_overflow_lights++;
		}
	}
	// Remaining capacity was 65,536 - 46,560 = 18,976 tiles -> 31 full 600-tile lights fit (18,600 tiles),
	// and the remaining 9 lights (5,400 tiles) overflow and fall back cleanly.
	CHECK(budget.lights_accepted == 96u);
	CHECK(budget.tiles_used == 46560u + 31u * 600u);
	CHECK(budget.overflow_tiles == 9u * 600u);
	CHECK(budget.overflow_lights == 9u);
	CHECK(budget.fallback_lights == 9u);
	CHECK(extra_overflow_lights == 9u);
}

TEST_CASE("[RTShadows] Screen-tile rect projection, history light-ID invalidation, transmittance, penumbra and offset (#141)") {
	// Screen-tile projection at 1920x1080 (120 x 68 tiles).
	Projection proj;
	proj.set_perspective(60.0f, 16.0f / 9.0f, 0.1f, 100.0f);
	const uint32_t tiles_x = 120u;
	const uint32_t tiles_y = 68u;
	const Vector3 cam_pos(0, 0, 0);

	// Directional light covers full screen.
	const RendererRD::NextGIRTShadowTileRect dir_rect = RendererRD::nextgi_rt_shadow_project_rect(
			RendererRD::NEXTGI_RT_SHADOW_TYPE_DIRECTIONAL, Vector3(), 100.0f, proj, cam_pos, 0.1f, tiles_x, tiles_y);
	CHECK(dir_rect.min_tx == 0u);
	CHECK(dir_rect.min_ty == 0u);
	CHECK(dir_rect.width == tiles_x);
	CHECK(dir_rect.height == tiles_y);

	// Small local light in front of camera projects to a tight interior tile sub-rect.
	const RendererRD::NextGIRTShadowTileRect near_rect = RendererRD::nextgi_rt_shadow_project_rect(
			RendererRD::NEXTGI_RT_SHADOW_TYPE_OMNI, Vector3(0, 0, -10.0f), 1.5f, proj, cam_pos, 0.1f, tiles_x, tiles_y);
	CHECK_FALSE(near_rect.is_empty());
	CHECK(near_rect.tile_count() < tiles_x * tiles_y);

	// Local light far behind the camera culls to empty.
	const RendererRD::NextGIRTShadowTileRect behind_rect = RendererRD::nextgi_rt_shadow_project_rect(
			RendererRD::NEXTGI_RT_SHADOW_TYPE_OMNI, Vector3(0, 0, 20.0f), 2.0f, proj, cam_pos, 0.1f, tiles_x, tiles_y);
	CHECK(behind_rect.is_empty());

	// Camera inside local light sphere covers full screen.
	const RendererRD::NextGIRTShadowTileRect inside_rect = RendererRD::nextgi_rt_shadow_project_rect(
			RendererRD::NEXTGI_RT_SHADOW_TYPE_OMNI, Vector3(0, 0, -1.0f), 5.0f, proj, cam_pos, 0.1f, tiles_x, tiles_y);
	CHECK(inside_rect.width == tiles_x);
	CHECK(inside_rect.height == tiles_y);

	// Tile history invalidation when light ID, screen tile, depth or normal changes.
	const uint32_t st_a = (10u & 0xFFFFu) | (20u << 16u);
	const uint32_t st_b = (11u & 0xFFFFu) | (20u << 16u);
	const Vector3 n_up(0, 1, 0);
	const Vector3 n_side(1, 0, 0);
	CHECK(RendererRD::nextgi_rt_shadow_history_valid(7u, 7u, st_a, st_a, 5.0f, 5.05f, n_up, n_up));
	CHECK_FALSE(RendererRD::nextgi_rt_shadow_history_valid(7u, 8u, st_a, st_a, 5.0f, 5.0f, n_up, n_up)); // Light ID changed.
	CHECK_FALSE(RendererRD::nextgi_rt_shadow_history_valid(7u, 0u, st_a, st_a, 5.0f, 5.0f, n_up, n_up)); // Unowned prev tile.
	CHECK_FALSE(RendererRD::nextgi_rt_shadow_history_valid(0u, 0u, st_a, st_a, 5.0f, 5.0f, n_up, n_up)); // Invalid current ID.
	CHECK_FALSE(RendererRD::nextgi_rt_shadow_history_valid(7u, 7u, st_a, st_b, 5.0f, 5.0f, n_up, n_up)); // Screen tile changed.
	CHECK_FALSE(RendererRD::nextgi_rt_shadow_history_valid(7u, 7u, st_a, st_a, 5.0f, 7.0f, n_up, n_up)); // Depth disocclusion.
	CHECK_FALSE(RendererRD::nextgi_rt_shadow_history_valid(7u, 7u, st_a, st_a, 5.0f, 5.0f, n_up, n_side)); // Normal disocclusion.

	// Subsurface transmittance distance calculation from ray hit distance + LIGHT_PARAM_TRANSMITTANCE_BIAS.
	CHECK(RendererRD::nextgi_rt_shadow_transmittance_thickness(RendererRD::NEXTGI_RT_SHADOW_HIT_MISS, 0.05f) < 0.0f);
	CHECK(RendererRD::nextgi_rt_shadow_transmittance_thickness(-1.0f, 0.05f) < 0.0f);
	CHECK(Math::is_equal_approx(RendererRD::nextgi_rt_shadow_transmittance_thickness(0.4f, 0.05f), 0.45f));
	CHECK(Math::is_equal_approx(RendererRD::nextgi_rt_shadow_transmittance_thickness(0.02f, -0.1f), 0.0f));

	// Penumbra width scales with hit_distance and light_size.
	CHECK(RendererRD::nextgi_rt_shadow_penumbra_pixels(true, RendererRD::NEXTGI_RT_SHADOW_HIT_MISS, 0.1f, 100.0f, 0.01f) == 0.0f);
	CHECK(RendererRD::nextgi_rt_shadow_penumbra_pixels(true, 2.0f, 0.0f, 100.0f, 0.01f) == 0.0f);
	const float pen_small = RendererRD::nextgi_rt_shadow_penumbra_pixels(true, 2.0f, 0.02f, 100.0f, 0.01f);
	const float pen_large = RendererRD::nextgi_rt_shadow_penumbra_pixels(true, 2.0f, 0.06f, 100.0f, 0.01f);
	CHECK(pen_large > pen_small);
	const float pen_local_near = RendererRD::nextgi_rt_shadow_penumbra_pixels(false, 1.0f, 0.3f, 8.0f, 0.01f);
	const float pen_local_far = RendererRD::nextgi_rt_shadow_penumbra_pixels(false, 3.0f, 0.3f, 8.0f, 0.01f);
	CHECK(pen_local_far > pen_local_near);
	CHECK(RendererRD::nextgi_rt_shadow_penumbra_pixels(true, 100.0f, 1.0f, 100.0f, 0.001f) <= 16.0f);

	// §8.2 ray origin offset: 4 * eps_rt * scale, clamped <= 5 cm (0.05 m).
	CHECK(RendererRD::nextgi_rt_shadow_origin_offset(1e-3f, 1.0f, 0.0f) >= 0.004f);
	CHECK(RendererRD::nextgi_rt_shadow_origin_offset(1e-3f, 4.0f, 0.0f) > RendererRD::nextgi_rt_shadow_origin_offset(1e-3f, 1.0f, 0.0f));
	CHECK(RendererRD::nextgi_rt_shadow_origin_offset(1e-3f, 1.0f, 10.0f) > RendererRD::nextgi_rt_shadow_origin_offset(1e-3f, 1.0f, 1.0f));
	CHECK(RendererRD::nextgi_rt_shadow_origin_offset(1.0f, 100.0f, 10000.0f) <= RendererRD::NEXTGI_RT_SHADOW_MAX_ORIGIN_OFFSET);

	// Specialization bit 15 (use_rt_shadows) has zero collision with bits 11..14.
	RendererSceneRenderImplementation::SceneShaderForwardClustered::ShaderSpecialization spec = {};
	spec.use_rt_shadows = 1;
	CHECK(spec.packed_1 == (1u << 15u));

	// Geometry pairing skip under standalone RT shadows.
	CHECK(RendererSceneCull::light_should_skip_geometry_pairing(false, true, RSE::LIGHT_SHADOW_METHOD_PROJECT, false, false, false, true));
	CHECK_FALSE(RendererSceneCull::light_should_skip_geometry_pairing(false, true, RSE::LIGHT_SHADOW_METHOD_PROJECT, false, false, false, false));
	CHECK_FALSE(RendererSceneCull::light_should_skip_geometry_pairing(false, true, RSE::LIGHT_SHADOW_METHOD_ATLAS, false, false, false, true));
}
