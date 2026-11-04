/**************************************************************************/
/*  test_nextgi.cpp                                                       */
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

TEST_FORCE_LINK(test_nextgi)

// NextGI doctests live in this file. Append new cases here.
// Editor-only cases stay inside TOOLS_ENABLED so later issues can add runtime cases below.

#ifdef TOOLS_ENABLED

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/string/translation_server.h"
#include "editor/editor_node.h"
#include "editor/scene/3d/nextgi_editor_plugin.h"
#include "tests/test_utils.h"

namespace TestNextGI {

static void restore_setting(const String &p_name, bool p_existed, const Variant &p_previous) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (p_existed) {
		ps->set_setting(p_name, p_previous);
	} else if (ps->has_setting(p_name)) {
		ps->clear(p_name);
	}
}

TEST_CASE("[NextGI][Editor] New projects map driver.windows to vulkan") {
	const HashMap<String, Variant> settings = EditorNode::get_initial_settings();
	const Variant *driver = settings.getptr("rendering/rendering_device/driver.windows");
	REQUIRE(driver != nullptr);
	CHECK_EQ(String(*driver), "vulkan");
}

TEST_CASE("[NextGI][Editor] Vulkan switch offer truth table") {
	CHECK(NextGIEditorPlugin::should_offer_vulkan_switch(false, true, "d3d12", false));

	CHECK_FALSE(NextGIEditorPlugin::should_offer_vulkan_switch(false, true, "d3d12", true));
	CHECK_FALSE(NextGIEditorPlugin::should_offer_vulkan_switch(false, true, "vulkan", false));
	CHECK_FALSE(NextGIEditorPlugin::should_offer_vulkan_switch(true, true, "d3d12", false));
	CHECK_FALSE(NextGIEditorPlugin::should_offer_vulkan_switch(false, false, "d3d12", false));
}

// GodotNext: mesh_sdf_offer_test
TEST_CASE("[MeshSDF][Editor] New projects generate mesh distance fields") {
	const HashMap<String, Variant> settings = EditorNode::get_initial_settings();
	const Variant *generate = settings.getptr("rendering/global_illumination/mesh_sdf/generate");
	REQUIRE(generate != nullptr);
	CHECK_EQ(bool(*generate), true);
}

TEST_CASE("[MeshSDF][Editor] Mesh SDF offer truth table") {
	CHECK(NextGIEditorPlugin::should_offer_mesh_sdf(false, true, false, false));

	CHECK_FALSE(NextGIEditorPlugin::should_offer_mesh_sdf(false, true, false, true));
	CHECK_FALSE(NextGIEditorPlugin::should_offer_mesh_sdf(false, true, true, false));
	CHECK_FALSE(NextGIEditorPlugin::should_offer_mesh_sdf(true, true, false, false));
	CHECK_FALSE(NextGIEditorPlugin::should_offer_mesh_sdf(false, false, false, false));
}

TEST_CASE("[NextGI][Editor] apply_vulkan_switch writes vulkan and keeps the D3D12 fallback") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String driver_setting = "rendering/rendering_device/driver.windows";
	const String fallback_setting = "rendering/rendering_device/fallback_to_d3d12";
	const String features_setting = "application/config/features";

	const bool had_driver = ps->has_setting(driver_setting);
	const bool had_fallback = ps->has_setting(fallback_setting);
	const bool had_features = ps->has_setting(features_setting);
	const Variant previous_driver = ps->get_setting(driver_setting);
	const Variant previous_fallback = ps->get_setting(fallback_setting);
	const Variant previous_features = ps->get_setting(features_setting);
	String &resource_path = TestProjectSettingsInternalsAccessor::resource_path();
	const String previous_resource_path = resource_path;

	const String temp_path = TestUtils::get_temp_path("nextgi_vulkan_switch");
	const Error mkdir_error = DirAccess::make_dir_recursive_absolute(temp_path);
	REQUIRE_EQ(mkdir_error, Error::OK);

	resource_path = temp_path;
	ps->set_setting(driver_setting, "d3d12");
	ps->set_setting(fallback_setting, false);

	NextGIEditorPlugin::apply_vulkan_switch();

	CHECK_EQ(String(ps->get_setting(driver_setting)), "vulkan");
	CHECK_EQ(bool(ps->get_setting(fallback_setting)), true);
	CHECK(FileAccess::exists(temp_path.path_join("project.godot")));

	restore_setting(driver_setting, had_driver, previous_driver);
	restore_setting(fallback_setting, had_fallback, previous_fallback);
	restore_setting(features_setting, had_features, previous_features);
	resource_path = previous_resource_path;
	DirAccess::remove_absolute(temp_path.path_join("project.godot"));
}

TEST_CASE("[NextGI][Editor] The inspector Vulkan button is Windows and d3d12 only") {
	CHECK(NextGIEditorPlugin::should_show_inspector_vulkan_button("Windows", "d3d12"));
	CHECK_FALSE(NextGIEditorPlugin::should_show_inspector_vulkan_button("macOS", "d3d12"));
	CHECK_FALSE(NextGIEditorPlugin::should_show_inspector_vulkan_button("Linux", "d3d12"));
	CHECK_FALSE(NextGIEditorPlugin::should_show_inspector_vulkan_button("Windows", "vulkan"));
	CHECK_FALSE(NextGIEditorPlugin::should_show_inspector_vulkan_button("Windows", ""));
}

TEST_CASE("[NextGI][Editor] Vulkan switch dialog names the HDR and DComp cost") {
	TranslationServer::get_singleton()->set_locale("en");

	const String title = NextGIEditorPlugin::get_vulkan_switch_dialog_title();
	const String text = NextGIEditorPlugin::get_vulkan_switch_dialog_text();

	CHECK_EQ(title, "Use Vulkan on Windows for hardware RT");
	CHECK(text.contains("SDFGI"));
	CHECK(text.contains("Direct3D 12 fallback"));
	CHECK(text.contains("HDR output"));
	CHECK(text.contains("DComp transparency"));
	CHECK(text.contains("restart"));
}

TEST_CASE("[NextGI][Editor] View Information tier readout") {
	Dictionary fallback;
	fallback["tier"] = "FALLBACK";
	fallback["reason"] = "NO_RAY_QUERY";
	CHECK_EQ(NextGIEditorPlugin::format_tier_readout(fallback), "NextGI: FALLBACK (NO_RAY_QUERY)");

	Dictionary hwrt;
	hwrt["tier"] = "HWRT";
	hwrt["reason"] = "NONE";
	CHECK_EQ(NextGIEditorPlugin::format_tier_readout(hwrt), "NextGI: HWRT");

	// The readout is off while nextgi/enabled is false (the default).
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String enabled_setting = "rendering/global_illumination/nextgi/enabled";
	const bool had_enabled = ps->has_setting(enabled_setting);
	const Variant previous_enabled = ps->get_setting(enabled_setting);
	ps->set_setting(enabled_setting, false);
	CHECK(NextGIEditorPlugin::get_tier_readout().is_empty());
	restore_setting(enabled_setting, had_enabled, previous_enabled);
}

} // namespace TestNextGI

#endif // TOOLS_ENABLED

// Renderer-independent cases: the SDFGI fallback hook and the default (GLES3 and dummy) tier info.

#include "scene/3d/world_environment.h"
#include "scene/resources/environment.h"
#include "servers/rendering/dummy/rasterizer_scene_dummy.h"
#include "servers/rendering/rendering_server.h"

namespace TestNextGI {

class FallbackSceneRender : public RasterizerSceneDummy {
public:
	bool fallback = false;

protected:
	virtual bool _nextgi_sdfgi_fallback() const override { return fallback; }
};

TEST_CASE("[NextGI] SDFGI runs for nextgi_enabled Environments only on the fallback tier") {
	FallbackSceneRender scene_render;
	const RID env = scene_render.environment_allocate();
	scene_render.environment_initialize(env);

	// Neither SDFGI nor NextGI.
	CHECK_FALSE(scene_render.environment_get_sdfgi_enabled(env));
	scene_render.fallback = true;
	CHECK_FALSE(scene_render.environment_get_sdfgi_enabled(env));

	// NextGI on: SDFGI only while the tier is the fallback.
	scene_render.environment_set_nextgi_enabled(env, true);
	CHECK(scene_render.environment_get_nextgi_enabled(env));
	CHECK(scene_render.environment_get_sdfgi_enabled(env));
	scene_render.fallback = false;
	CHECK_FALSE(scene_render.environment_get_sdfgi_enabled(env));

	// The Environment's own SDFGI is unchanged by the hook.
	scene_render.environment_set_nextgi_enabled(env, false);
	ERR_PRINT_OFF; // SDFGI warns once outside Forward+.
	scene_render.environment_set_sdfgi(env, true, 4, 0.2, RSE::ENV_SDFGI_Y_SCALE_75_PERCENT, false, 0.5, true, 1.0, 1.1, 1.1);
	ERR_PRINT_ON;
	CHECK(scene_render.environment_get_sdfgi_enabled(env));

	scene_render.environment_free(env);
}

TEST_CASE("[NextGI] Renderers without NextGI report UNSUPPORTED_RENDERER") {
	RasterizerSceneDummy scene_render;
	const Dictionary info = scene_render.nextgi_get_tier_info();
	CHECK_EQ(String(info["tier"]), "UNSUPPORTED_RENDERER");
	CHECK_EQ(String(info["reason"]), "NONE");
	CHECK_EQ(String(info["quality"]), "NONE");
	CHECK_EQ(int(info["ray_query_shaders_initialized"]), 0);
}

TEST_CASE("[NextGI] RenderingServer round-trips nextgi energy and sky light on the dummy renderer") {
	RenderingServer *rs = RenderingServer::get_singleton();
	const RID env = rs->environment_create();
	CHECK_FALSE(rs->environment_get_nextgi_enabled(env));
	CHECK(Math::is_equal_approx(rs->environment_get_nextgi_energy(env), 1.0f));
	CHECK(rs->environment_get_nextgi_read_sky_light(env));

	rs->environment_set_nextgi(env, true, 0.35f, false);
	CHECK(rs->environment_get_nextgi_enabled(env));
	CHECK(Math::is_equal_approx(rs->environment_get_nextgi_energy(env), 0.35f));
	CHECK_FALSE(rs->environment_get_nextgi_read_sky_light(env));

	// The enable-only setter keeps the energy and the sky-light read.
	rs->environment_set_nextgi_enabled(env, false);
	CHECK_FALSE(rs->environment_get_nextgi_enabled(env));
	CHECK(Math::is_equal_approx(rs->environment_get_nextgi_energy(env), 0.35f));
	CHECK_FALSE(rs->environment_get_nextgi_read_sky_light(env));

	Ref<Environment> resource;
	resource.instantiate();
	resource->set_nextgi_energy(2.5f);
	resource->set_nextgi_read_sky_light(false);
	resource->set_nextgi_enabled(true);
	CHECK(Math::is_equal_approx(resource->get_nextgi_energy(), 2.5f));
	CHECK_FALSE(resource->is_nextgi_reading_sky_light());
	CHECK(rs->environment_get_nextgi_enabled(resource->get_rid()));
	CHECK(Math::is_equal_approx(rs->environment_get_nextgi_energy(resource->get_rid()), 2.5f));
	CHECK_FALSE(rs->environment_get_nextgi_read_sky_light(resource->get_rid()));

	rs->free_rid(env);
}

TEST_CASE("[NextGI] Dummy status is fallback SETTING_OFF and counters stay zero") {
	RenderingServer *rs = RenderingServer::get_singleton();
	const Dictionary status = rs->nextgi_get_status();
	CHECK_EQ(String(status["tier"]), "FALLBACK");
	CHECK_EQ(String(status["reason"]), "SETTING_OFF");
	CHECK_EQ(String(status["quality"]), "NONE");
	CHECK_EQ(String(status["quality_reason"]), "NONE");
	CHECK(status.has("driver"));
	// Per-instance reasons never ride in the tier status.
	CHECK_NE(String(status["reason"]), "NOT_IN_RT_SCENE");
	CHECK_NE(String(status["reason"]), "RT_INSTANCE_CAP");
	CHECK_NE(String(status["reason"]), "RT_DYNAMIC_EXCLUDED");

	const Dictionary counters = rs->nextgi_get_counters();
	const char *keys[] = {
		"tlas_instances",
		"rt_coverage",
		"blas_bytes_compacted",
		"blas_bytes_uncompacted",
		"rt_arena_bytes",
		"skinned_refit_triangles",
		"rays_per_pass",
		"src_capture_texels",
		"src_relight_texels",
		"src_shadow_rays",
		"dirty_aabb",
		"src_page_updates",
		"clamped_energy_fraction",
	};
	CHECK_EQ(counters.size(), 13);
	for (const char *key : keys) {
		CHECK(counters.has(key));
		CHECK_EQ(int64_t(counters[key]), int64_t(0));
	}
}

TEST_CASE("[NextGI] WorldEnvironment warns when NextGI is enabled off the HWRT tier") {
	Ref<Environment> env;
	env.instantiate();
	env->set_nextgi_enabled(true);
	WorldEnvironment *world = memnew(WorldEnvironment);
	world->set_environment(env);
	const PackedStringArray warnings = world->get_configuration_warnings();
	bool found = false;
	for (const String &warning : warnings) {
		if (warning.contains("FALLBACK") && warning.contains("SETTING_OFF")) {
			found = true;
		}
	}
	CHECK(found);
	memdelete(world);
}

} // namespace TestNextGI

#ifdef RD_ENABLED

#include "core/templates/hash_set.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
// GodotNext: nextgi_rt_scene
#include "servers/rendering/renderer_rd/environment/next_gi_reference.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_scene.h"
// GodotNext: nextgi_as_slots
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
// GodotNext: nextgi_src_cards
#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/environment/next_gi_gather.h"
#include "servers/rendering/renderer_rd/environment/next_gi_src.h"
#include "servers/rendering/renderer_rd/environment/next_gi_src_relight.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgi_velocity.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"

// The §3.1 tier rule (#15). Device capabilities are mocked: the rule is pure, and the device inputs
// come from a driverless RenderingDevice through the rd_feature_mock hook (#17).

namespace TestNextGI {

using NextGI = RendererRD::NextGI;

// A Forward+ project with NextGI on and a device that passes every HWRT check.
static NextGI::TierInputs hwrt_inputs() {
	NextGI::TierInputs inputs;
	inputs.forward_plus = true;
	inputs.project_enabled = true;
	inputs.hwrt_setting = NextGI::HARDWARE_RAY_TRACING_AUTO;
	inputs.ray_query = true;
	inputs.driver_reason = NextGI::TIER_REASON_NONE;
	inputs.cpu_device = false;
	inputs.ray_query_hw = true;
	return inputs;
}

// A device without ray query that reports p_reason.
static NextGI::TierInputs no_ray_query_inputs(NextGI::TierReason p_reason) {
	NextGI::TierInputs inputs = hwrt_inputs();
	inputs.ray_query = false;
	inputs.ray_query_hw = false;
	inputs.driver_reason = p_reason;
	return inputs;
}

static void check_tier(const NextGI::TierInputs &p_inputs, NextGI::Tier p_tier, NextGI::TierReason p_reason, bool p_warn_force = false) {
	const NextGI::TierResult result = NextGI::evaluate_tier(p_inputs);
	CHECK_EQ(NextGI::get_tier_name(result.tier), NextGI::get_tier_name(p_tier));
	CHECK_EQ(NextGI::get_reason_name(result.reason), NextGI::get_reason_name(p_reason));
	CHECK_EQ(result.warn_force, p_warn_force);
}

static uint64_t ray_query_feature_bits(bool p_ray_query) {
	return p_ray_query ? (uint64_t(1) << RenderingDevice::SUPPORTS_RAY_QUERY) : 0;
}

// Device inputs from a driverless RenderingDevice with mocked features, as NextGI::init reads them.
static NextGI::TierResult evaluate_mocked_device(bool p_ray_query, const String &p_api_name, RenderingDeviceEnums::DeviceType p_device_type, NextGI::HardwareRayTracing p_hwrt = NextGI::HARDWARE_RAY_TRACING_AUTO) {
	RenderingDevice *rd = memnew(RenderingDevice);
	rd->set_feature_mock(true, ray_query_feature_bits(p_ray_query));
	NextGI::TierInputs inputs;
	inputs.forward_plus = true;
	inputs.project_enabled = true;
	inputs.hwrt_setting = p_hwrt;
	NextGI::collect_device_inputs(rd, p_api_name, p_device_type, inputs);
	memdelete(rd);
	return NextGI::evaluate_tier(inputs);
}

TEST_CASE("[NextGI] Tier rule: an RTX GPU selects HWRT") {
	check_tier(hwrt_inputs(), NextGI::TIER_HWRT, NextGI::TIER_REASON_NONE);
}

TEST_CASE("[NextGI] Tier rule: a GTX GPU off the RT-core allowlist gives NOT_HW_ACCELERATED") {
	NextGI::TierInputs gtx = hwrt_inputs();
	gtx.ray_query_hw = false; // Driver-emulated RT exposes ray query without RT cores.
	check_tier(gtx, NextGI::TIER_FALLBACK, NextGI::NOT_HW_ACCELERATED);
}

TEST_CASE("[NextGI] Tier rule: Force puts a GTX GPU on HWRT") {
	NextGI::TierInputs gtx = hwrt_inputs();
	gtx.ray_query_hw = false;
	gtx.hwrt_setting = NextGI::HARDWARE_RAY_TRACING_FORCE;
	check_tier(gtx, NextGI::TIER_HWRT, NextGI::TIER_REASON_NONE);
}

TEST_CASE("[NextGI] Tier rule: RADV emulated RT gives NOT_HW_ACCELERATED") {
	NextGI::TierInputs radv = hwrt_inputs();
	radv.ray_query_hw = false; // RADV_PERFTEST=emulate_rt on a pre-RDNA2 GPU.
	check_tier(radv, NextGI::TIER_FALLBACK, NextGI::NOT_HW_ACCELERATED);
}

TEST_CASE("[NextGI] Tier rule: lavapipe gives CPU_DEVICE, or NO_RAY_QUERY without ray query") {
	NextGI::TierInputs lavapipe = hwrt_inputs();
	lavapipe.cpu_device = true;
	lavapipe.ray_query_hw = false;
	check_tier(lavapipe, NextGI::TIER_FALLBACK, NextGI::CPU_DEVICE);

	// The CPU check comes before the hardware flag, so a CPU device never reads as hardware.
	lavapipe.ray_query_hw = true;
	check_tier(lavapipe, NextGI::TIER_FALLBACK, NextGI::CPU_DEVICE);

	NextGI::TierInputs lavapipe_no_rq = no_ray_query_inputs(NextGI::NO_RAY_QUERY);
	lavapipe_no_rq.cpu_device = true;
	check_tier(lavapipe_no_rq, NextGI::TIER_FALLBACK, NextGI::NO_RAY_QUERY);
}

TEST_CASE("[NextGI] Tier rule: Apple7 and Apple8 give NOT_HW_ACCELERATED") {
	// The Metal ray-tracing API exists on M1 and M2, but there is no RT hardware (#48 feeds this).
	NextGI::TierInputs apple = hwrt_inputs();
	apple.ray_query_hw = false;
	check_tier(apple, NextGI::TIER_FALLBACK, NextGI::NOT_HW_ACCELERATED);
}

TEST_CASE("[NextGI] Tier rule: D3D12 without DXR gives D3D12_NO_DXR") {
	check_tier(no_ray_query_inputs(NextGI::D3D12_NO_DXR), NextGI::TIER_FALLBACK, NextGI::D3D12_NO_DXR);
}

TEST_CASE("[NextGI] Tier rule: D3D12 under Force stays D3D12_NO_DXR, with the Force warning") {
	NextGI::TierInputs d3d12 = no_ray_query_inputs(NextGI::D3D12_NO_DXR);
	d3d12.hwrt_setting = NextGI::HARDWARE_RAY_TRACING_FORCE;
	check_tier(d3d12, NextGI::TIER_FALLBACK, NextGI::D3D12_NO_DXR, true);
}

TEST_CASE("[NextGI] Tier rule: Intel UHD gives NO_RAY_QUERY") {
	check_tier(no_ray_query_inputs(NextGI::NO_RAY_QUERY), NextGI::TIER_FALLBACK, NextGI::NO_RAY_QUERY);
}

TEST_CASE("[NextGI] Tier rule: Metal without Barriers sync mode gives METAL_SYNC_MODE") {
	check_tier(no_ray_query_inputs(NextGI::METAL_SYNC_MODE), NextGI::TIER_FALLBACK, NextGI::METAL_SYNC_MODE);
}

TEST_CASE("[NextGI] Tier rule: Metal without argument buffers gives METAL_NO_ARGUMENT_BUFFERS") {
	check_tier(no_ray_query_inputs(NextGI::METAL_NO_ARGUMENT_BUFFERS), NextGI::TIER_FALLBACK, NextGI::METAL_NO_ARGUMENT_BUFFERS);
}

TEST_CASE("[NextGI] Tier rule: a driver without a reason code gives NO_RAY_QUERY") {
	check_tier(no_ray_query_inputs(NextGI::TIER_REASON_NONE), NextGI::TIER_FALLBACK, NextGI::NO_RAY_QUERY);
	// Only driver reason codes pass through.
	check_tier(no_ray_query_inputs(NextGI::AS_MEMORY), NextGI::TIER_FALLBACK, NextGI::NO_RAY_QUERY);
}

TEST_CASE("[NextGI] Tier rule: hardware_ray_tracing Off gives SETTING_OFF") {
	NextGI::TierInputs off = hwrt_inputs();
	off.hwrt_setting = NextGI::HARDWARE_RAY_TRACING_OFF;
	check_tier(off, NextGI::TIER_FALLBACK, NextGI::SETTING_OFF);

	// Off is checked before the device.
	NextGI::TierInputs off_no_rq = no_ray_query_inputs(NextGI::D3D12_NO_DXR);
	off_no_rq.hwrt_setting = NextGI::HARDWARE_RAY_TRACING_OFF;
	check_tier(off_no_rq, NextGI::TIER_FALLBACK, NextGI::SETTING_OFF);
}

TEST_CASE("[NextGI] Tier rule: a failed self-test gives SELFTEST_FAILED, also under Force") {
	NextGI::TierInputs failed = hwrt_inputs();
	failed.selftest_passed = false;
	check_tier(failed, NextGI::TIER_FALLBACK, NextGI::SELFTEST_FAILED);
	failed.hwrt_setting = NextGI::HARDWARE_RAY_TRACING_FORCE;
	check_tier(failed, NextGI::TIER_FALLBACK, NextGI::SELFTEST_FAILED);
}

TEST_CASE("[NextGI] Tier rule: a latched HWRT fault gives HWRT_FAULTED") {
	NextGI::TierInputs faulted = hwrt_inputs();
	faulted.fault_latched = true;
	check_tier(faulted, NextGI::TIER_FALLBACK, NextGI::HWRT_FAULTED);
}

TEST_CASE("[NextGI] Tier rule: Force clears HWRT_FAULTED") {
	NextGI::TierInputs faulted = hwrt_inputs();
	faulted.fault_latched = true;
	faulted.hwrt_setting = NextGI::HARDWARE_RAY_TRACING_FORCE;
	check_tier(faulted, NextGI::TIER_HWRT, NextGI::TIER_REASON_NONE);
}

TEST_CASE("[NextGI] Tier rule: an unmet AS memory budget gives AS_MEMORY") {
	NextGI::TierInputs as_memory = hwrt_inputs();
	as_memory.as_memory_ok = false;
	check_tier(as_memory, NextGI::TIER_FALLBACK, NextGI::AS_MEMORY);
	as_memory.hwrt_setting = NextGI::HARDWARE_RAY_TRACING_FORCE;
	check_tier(as_memory, NextGI::TIER_FALLBACK, NextGI::AS_MEMORY);
}

TEST_CASE("[NextGI] Tier rule: Mobile gives UNSUPPORTED_RENDERER") {
	NextGI::TierInputs mobile = hwrt_inputs();
	mobile.forward_plus = false;
	check_tier(mobile, NextGI::TIER_UNSUPPORTED_RENDERER, NextGI::TIER_REASON_NONE);
	// The renderer is checked first, before the project setting.
	mobile.project_enabled = false;
	check_tier(mobile, NextGI::TIER_UNSUPPORTED_RENDERER, NextGI::TIER_REASON_NONE);
}

TEST_CASE("[NextGI] Tier rule: nextgi/enabled false gives DISABLED") {
	NextGI::TierInputs disabled = hwrt_inputs();
	disabled.project_enabled = false;
	check_tier(disabled, NextGI::TIER_DISABLED, NextGI::TIER_REASON_NONE);
	NextGI::TierInputs disabled_no_rq = no_ray_query_inputs(NextGI::D3D12_NO_DXR);
	disabled_no_rq.project_enabled = false;
	disabled_no_rq.hwrt_setting = NextGI::HARDWARE_RAY_TRACING_FORCE;
	check_tier(disabled_no_rq, NextGI::TIER_DISABLED, NextGI::TIER_REASON_NONE);
}

TEST_CASE("[NextGI] Tier rule: the first failing check wins") {
	NextGI::TierInputs inputs = hwrt_inputs();
	inputs.ray_query_hw = false;
	inputs.selftest_passed = false;
	inputs.fault_latched = true;
	inputs.as_memory_ok = false;
	check_tier(inputs, NextGI::TIER_FALLBACK, NextGI::NOT_HW_ACCELERATED);
	inputs.ray_query_hw = true;
	check_tier(inputs, NextGI::TIER_FALLBACK, NextGI::SELFTEST_FAILED);
	inputs.selftest_passed = true;
	check_tier(inputs, NextGI::TIER_FALLBACK, NextGI::HWRT_FAULTED);
	inputs.fault_latched = false;
	check_tier(inputs, NextGI::TIER_FALLBACK, NextGI::AS_MEMORY);
	inputs.as_memory_ok = true;
	check_tier(inputs, NextGI::TIER_HWRT, NextGI::TIER_REASON_NONE);
}

TEST_CASE("[NextGI] Tier and reason names are unique and cover all 10 reasons") {
	HashSet<String> names;
	for (int i = 0; i < NextGI::TIER_MAX; i++) {
		const String name = NextGI::get_tier_name(NextGI::Tier(i));
		CHECK_FALSE(name.is_empty());
		names.insert(name);
	}
	CHECK_EQ(names.size(), 4u);

	names.clear();
	for (int i = 0; i < NextGI::TIER_REASON_MAX; i++) {
		const String name = NextGI::get_reason_name(NextGI::TierReason(i));
		CHECK_FALSE(name.is_empty());
		names.insert(name);
	}
	// NONE plus the 10 reasons of plan §6.3.
	CHECK_EQ(names.size(), 11u);
	for (const char *reason : { "NO_RAY_QUERY", "NOT_HW_ACCELERATED", "CPU_DEVICE", "D3D12_NO_DXR", "METAL_SYNC_MODE", "METAL_NO_ARGUMENT_BUFFERS", "SETTING_OFF", "AS_MEMORY", "SELFTEST_FAILED", "HWRT_FAULTED" }) {
		CHECK(names.has(reason));
	}
}

TEST_CASE("[NextGI] Device inputs: the G0-2 configurations with mocked device features") {
	// RTX 4080 SUPER on Vulkan, device 1: ray query on a discrete GPU.
	NextGI::TierResult result = evaluate_mocked_device(true, "Vulkan", RenderingDeviceEnums::DEVICE_TYPE_DISCRETE_GPU);
	CHECK_EQ(result.tier, NextGI::TIER_HWRT);
	CHECK_EQ(result.reason, NextGI::TIER_REASON_NONE);

	// --gpu-index 0 on the RTX 4080 SUPER machine: the integrated UHD 770, no ray query.
	result = evaluate_mocked_device(false, "Vulkan", RenderingDeviceEnums::DEVICE_TYPE_INTEGRATED_GPU);
	CHECK_EQ(result.tier, NextGI::TIER_FALLBACK);
	CHECK_EQ(result.reason, NextGI::NO_RAY_QUERY);

	// --rendering-driver d3d12: no DXR until PD.
	result = evaluate_mocked_device(false, "D3D12", RenderingDeviceEnums::DEVICE_TYPE_DISCRETE_GPU);
	CHECK_EQ(result.tier, NextGI::TIER_FALLBACK);
	CHECK_EQ(result.reason, NextGI::D3D12_NO_DXR);
	CHECK_FALSE(result.warn_force);

	// d3d12 under Force: the same reason, with the warning.
	result = evaluate_mocked_device(false, "D3D12", RenderingDeviceEnums::DEVICE_TYPE_DISCRETE_GPU, NextGI::HARDWARE_RAY_TRACING_FORCE);
	CHECK_EQ(result.reason, NextGI::D3D12_NO_DXR);
	CHECK(result.warn_force);

	// A mocked Metal device without ray query. The live M5 Max reports the same until #47
	// flips the acceleration-structure gate. The [NextGI][Metal] cases feed the helper.
	result = evaluate_mocked_device(false, "Metal", RenderingDeviceEnums::DEVICE_TYPE_INTEGRATED_GPU);
	CHECK_EQ(result.tier, NextGI::TIER_FALLBACK);
	CHECK_EQ(result.reason, NextGI::NO_RAY_QUERY);

	// The Linux VM: lavapipe gives CPU_DEVICE with ray query, NO_RAY_QUERY without.
	result = evaluate_mocked_device(true, "Vulkan", RenderingDeviceEnums::DEVICE_TYPE_CPU);
	CHECK_EQ(result.reason, NextGI::CPU_DEVICE);
	result = evaluate_mocked_device(false, "Vulkan", RenderingDeviceEnums::DEVICE_TYPE_CPU);
	CHECK_EQ(result.reason, NextGI::NO_RAY_QUERY);
}

TEST_CASE("[NextGI] One tier line per change, and none while NextGI is disabled") {
	NextGI next_gi;
	String line;

	NextGI::TierInputs disabled = no_ray_query_inputs(NextGI::D3D12_NO_DXR);
	disabled.project_enabled = false;
	CHECK_FALSE(next_gi.update_tier(disabled, NextGI::QUALITY_AUTO, line));
	CHECK(line.is_empty());
	CHECK_EQ(next_gi.get_tier_result().tier, NextGI::TIER_DISABLED);

	const NextGI::TierInputs d3d12 = no_ray_query_inputs(NextGI::D3D12_NO_DXR);
	REQUIRE(next_gi.update_tier(d3d12, NextGI::QUALITY_AUTO, line));
	CHECK_EQ(line, "NextGI: tier=FALLBACK reason=D3D12_NO_DXR (SDFGI fallback) quality=HIGH (auto selection pending)");
	CHECK_EQ(line.count("NextGI:"), 1);

	// Re-evaluating the same inputs prints nothing.
	line = String();
	CHECK_FALSE(next_gi.update_tier(d3d12, NextGI::QUALITY_AUTO, line));
	CHECK(line.is_empty());

	// A changed result prints again.
	NextGI::TierInputs off = d3d12;
	off.hwrt_setting = NextGI::HARDWARE_RAY_TRACING_OFF;
	REQUIRE(next_gi.update_tier(off, NextGI::QUALITY_EPIC, line));
	CHECK_EQ(line, "NextGI: tier=FALLBACK reason=SETTING_OFF (SDFGI fallback) quality=EPIC");

	// Disabling prints nothing. Enabling again is a new selection, so it prints.
	line = String();
	CHECK_FALSE(next_gi.update_tier(disabled, NextGI::QUALITY_AUTO, line));
	CHECK(line.is_empty());
	REQUIRE(next_gi.update_tier(d3d12, NextGI::QUALITY_AUTO, line));
	CHECK_EQ(line, "NextGI: tier=FALLBACK reason=D3D12_NO_DXR (SDFGI fallback) quality=HIGH (auto selection pending)");
}

TEST_CASE("[NextGI] Tier line formats") {
	NextGI::TierResult hwrt;
	hwrt.tier = NextGI::TIER_HWRT;
	CHECK_EQ(NextGI::format_tier_line(hwrt, NextGI::QUALITY_MEDIUM), "NextGI: tier=HWRT reason=NONE quality=MEDIUM");

	NextGI::TierResult mobile;
	mobile.tier = NextGI::TIER_UNSUPPORTED_RENDERER;
	CHECK_EQ(NextGI::format_tier_line(mobile, NextGI::QUALITY_AUTO), "NextGI: tier=UNSUPPORTED_RENDERER reason=NONE");
}

TEST_CASE("[NextGI] Auto quality resolves to High until the microbenchmark lands") {
	CHECK_EQ(NextGI::resolve_quality(NextGI::QUALITY_AUTO), NextGI::QUALITY_HIGH);
	CHECK_EQ(NextGI::resolve_quality(NextGI::QUALITY_MEDIUM), NextGI::QUALITY_MEDIUM);
	CHECK_EQ(NextGI::resolve_quality(NextGI::QUALITY_HIGH), NextGI::QUALITY_HIGH);
	CHECK_EQ(NextGI::resolve_quality(NextGI::QUALITY_EPIC), NextGI::QUALITY_EPIC);
}

TEST_CASE("[NextGI] Ray-query ShaderRDs initialize only under the HWRT tier") {
	NextGI next_gi;
	String line;

	// The M5 Max today: fallback, so no ray-query ShaderRD may initialize.
	next_gi.update_tier(no_ray_query_inputs(NextGI::NO_RAY_QUERY), NextGI::QUALITY_AUTO, line);
	CHECK_FALSE(next_gi.can_initialize_ray_query_shaders());
	CHECK(next_gi.is_sdfgi_fallback());
	ERR_PRINT_OFF;
	next_gi.notify_ray_query_shader_initialized();
	ERR_PRINT_ON;
	Dictionary info = next_gi.get_tier_info();
	CHECK_EQ(String(info["tier"]), "FALLBACK");
	CHECK_EQ(String(info["reason"]), "NO_RAY_QUERY");
	CHECK_EQ(String(info["quality"]), "HIGH");
	CHECK_EQ(int(info["ray_query_shaders_initialized"]), 0);

	// HWRT: ray-query ShaderRDs may initialize, and the count is reported.
	next_gi.update_tier(hwrt_inputs(), NextGI::QUALITY_AUTO, line);
	CHECK(next_gi.can_initialize_ray_query_shaders());
	CHECK_FALSE(next_gi.is_sdfgi_fallback());
	next_gi.notify_ray_query_shader_initialized();
	info = next_gi.get_tier_info();
	CHECK_EQ(String(info["tier"]), "HWRT");
	CHECK_EQ(int(info["ray_query_shaders_initialized"]), 1);

	// Disabled: neither ray-query shaders nor the SDFGI fallback.
	NextGI::TierInputs disabled = hwrt_inputs();
	disabled.project_enabled = false;
	next_gi.update_tier(disabled, NextGI::QUALITY_AUTO, line);
	CHECK_FALSE(next_gi.can_initialize_ray_query_shaders());
	CHECK_FALSE(next_gi.is_sdfgi_fallback());
	CHECK_EQ(String(next_gi.get_tier_info()["quality"]), "NONE");
}

TEST_CASE("[NextGI] Status reason stays a tier reason, and the tier view tints NO_RAY_QUERY") {
	NextGI next_gi;
	String line;
	next_gi.update_tier(no_ray_query_inputs(NextGI::NO_RAY_QUERY), NextGI::QUALITY_AUTO, line);

	const Dictionary status = next_gi.get_status();
	CHECK_EQ(String(status["tier"]), "FALLBACK");
	CHECK_EQ(String(status["reason"]), "NO_RAY_QUERY");
	CHECK_EQ(String(status["quality"]), "HIGH");
	CHECK_EQ(String(status["quality_reason"]), "auto selection pending");
	CHECK(NextGI::is_tier_reason_name(status["reason"]));
	for (const String &instance_reason : NextGI::get_instance_fallback_reason_names()) {
		CHECK_FALSE(NextGI::is_tier_reason_name(instance_reason));
		CHECK_NE(String(status["reason"]), instance_reason);
	}

	const NextGI::DebugView tier_view = NextGI::resolve_debug_view(NextGI::DEBUG_VIEW_TIER, next_gi.get_tier_result());
	CHECK(tier_view.has_data);
	CHECK(tier_view.tint.is_equal_approx(NextGI::debug_tint_fallback()));
	CHECK(tier_view.label.contains("NO_RAY_QUERY"));
	CHECK(tier_view.label.contains("FALLBACK"));

	NextGI::TierResult hwrt;
	hwrt.tier = NextGI::TIER_HWRT;
	hwrt.reason = NextGI::TIER_REASON_NONE;
	const NextGI::DebugView hwrt_view = NextGI::resolve_debug_view(NextGI::DEBUG_VIEW_TIER, hwrt);
	CHECK(hwrt_view.tint.is_equal_approx(NextGI::debug_tint_hwrt()));
	CHECK(hwrt_view.label.contains("HWRT"));

	const NextGI::DebugView cards = NextGI::resolve_debug_view(NextGI::DEBUG_VIEW_SRC_CARDS, next_gi.get_tier_result());
	CHECK_FALSE(cards.has_data);
	CHECK(cards.tint.is_equal_approx(NextGI::debug_tint_no_data()));
	CHECK(cards.label == "no data");

	const NextGI::DebugView reasons = NextGI::resolve_debug_view(NextGI::DEBUG_VIEW_FALLBACK_REASONS, next_gi.get_tier_result());
	CHECK(reasons.label.contains("no data"));
	CHECK(reasons.label.contains("NOT_IN_RT_SCENE"));
	CHECK(reasons.label.contains("RT_INSTANCE_CAP"));
	CHECK(reasons.label.contains("RT_DYNAMIC_EXCLUDED"));

	CHECK_EQ(NextGI::debug_view_from_viewport(RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_TIER), NextGI::DEBUG_VIEW_TIER);
	CHECK_EQ(NextGI::debug_view_from_viewport(RSE::VIEWPORT_DEBUG_DRAW_DISABLED), NextGI::DEBUG_VIEW_MAX);

	const Dictionary counters = next_gi.get_counters();
	CHECK_EQ(int64_t(counters["tlas_instances"]), int64_t(0));
	CHECK_EQ(int64_t(counters["clamped_energy_fraction"]), int64_t(0));
}

TEST_CASE("[NextGI] set_hardware_ray_tracing re-evaluates the tier") {
	NextGI next_gi;
	String line;
	next_gi.update_tier(hwrt_inputs(), NextGI::QUALITY_HIGH, line);
	REQUIRE_EQ(next_gi.get_tier_result().tier, NextGI::TIER_HWRT);

	next_gi.set_hardware_ray_tracing(NextGI::HARDWARE_RAY_TRACING_OFF);
	CHECK_EQ(next_gi.get_tier_result().reason, NextGI::SETTING_OFF);
	CHECK_EQ(next_gi.get_tier_inputs().hwrt_setting, NextGI::HARDWARE_RAY_TRACING_OFF);

	next_gi.set_hardware_ray_tracing(NextGI::HARDWARE_RAY_TRACING_AUTO);
	CHECK_EQ(next_gi.get_tier_result().tier, NextGI::TIER_HWRT);

	// Out-of-range modes are rejected and change nothing.
	ERR_PRINT_OFF;
	next_gi.set_hardware_ray_tracing(NextGI::HARDWARE_RAY_TRACING_MAX);
	ERR_PRINT_ON;
	CHECK_EQ(next_gi.get_tier_inputs().hwrt_setting, NextGI::HARDWARE_RAY_TRACING_AUTO);
}

TEST_CASE("[NextGI] The toolchain self-test stays off outside the HWRT tier") {
	// A ray-query device whose NextGI tier is the fallback: the ShaderRD is never built.
	// Headless doctests have no renderer, so both objects become the singletons.
	RenderingDevice *rd = memnew(RenderingDevice);
	REQUIRE(RenderingDevice::get_singleton() == rd);
	rd->set_feature_mock(true, ray_query_feature_bits(true));
	NextGI next_gi;
	REQUIRE(NextGI::get_singleton() == &next_gi);
	NextGI::TierInputs off = hwrt_inputs();
	off.hwrt_setting = NextGI::HARDWARE_RAY_TRACING_OFF;
	String line;
	next_gi.update_tier(off, NextGI::QUALITY_AUTO, line);

	String report;
	CHECK_FALSE(NextGI::run_toolchain_selftest(report));
	CHECK_EQ(report, "{\"pass\":false,\"reason\":\"SETTING_OFF\"}");
	CHECK_EQ(next_gi.get_ray_query_shaders_initialized(), 0);
	memdelete(rd);
}

// Metal ray-query capability (#48). The helper is pure; these cases feed it into evaluate_tier.
// Apple8's experimental opt-in is #131. Until the acceleration-structure issue lands, has_feature
// stays false; that gate is not part of the helper.

static NextGI::TierReason metal_tier_reason(RenderingDeviceDriver::RayQueryUnsupportedReason p_reason) {
	switch (p_reason) {
		case RenderingDeviceDriver::RAY_QUERY_SUPPORTED:
			return NextGI::TIER_REASON_NONE;
		case RenderingDeviceDriver::RAY_QUERY_REASON_NO_RAY_QUERY:
			return NextGI::NO_RAY_QUERY;
		case RenderingDeviceDriver::RAY_QUERY_REASON_D3D12_NO_DXR:
			return NextGI::D3D12_NO_DXR;
		case RenderingDeviceDriver::RAY_QUERY_REASON_METAL_SYNC_MODE:
			return NextGI::METAL_SYNC_MODE;
		case RenderingDeviceDriver::RAY_QUERY_REASON_METAL_NO_ARGUMENT_BUFFERS:
			return NextGI::METAL_NO_ARGUMENT_BUFFERS;
	}
	return NextGI::NO_RAY_QUERY;
}

static RenderingDeviceDriver::MetalRayQueryCaps metal_family(uint32_t p_family, bool p_barriers = true, bool p_argument_buffers = true, bool p_macos = true, uint32_t p_os_major = RenderingDeviceDriver::METAL_RAY_QUERY_MIN_OS_MAJOR, bool p_supports_raytracing = true, uint32_t p_msl_version = RenderingDeviceDriver::METAL_RAY_QUERY_MIN_MSL, bool p_gpu_address = true) {
	return RenderingDeviceDriver::metal_ray_query_caps(p_family, p_macos, p_os_major, p_supports_raytracing, p_msl_version, p_gpu_address, p_barriers, p_argument_buffers);
}

static void check_metal_tier(const RenderingDeviceDriver::MetalRayQueryCaps &p_caps, NextGI::Tier p_tier, NextGI::TierReason p_reason, NextGI::HardwareRayTracing p_hwrt = NextGI::HARDWARE_RAY_TRACING_AUTO) {
	NextGI::TierInputs inputs = hwrt_inputs();
	inputs.hwrt_setting = p_hwrt;
	inputs.ray_query = p_caps.ray_query;
	inputs.ray_query_hw = p_caps.hardware;
	inputs.driver_reason = p_caps.ray_query ? NextGI::TIER_REASON_NONE : metal_tier_reason(p_caps.reason);
	const bool warn_force = p_hwrt == NextGI::HARDWARE_RAY_TRACING_FORCE && !p_caps.ray_query;
	check_tier(inputs, p_tier, p_reason, warn_force);
}

TEST_CASE("[NextGI][Metal] Apple7 and Apple8 give NOT_HW_ACCELERATED, and HWRT under force") {
	for (uint32_t family : { 1007u, 1008u }) {
		const RenderingDeviceDriver::MetalRayQueryCaps caps = metal_family(family);
		CHECK(caps.ray_query);
		CHECK_FALSE(caps.hardware);
		CHECK_EQ(caps.reason, RenderingDeviceDriver::RAY_QUERY_SUPPORTED);
		check_metal_tier(caps, NextGI::TIER_FALLBACK, NextGI::NOT_HW_ACCELERATED);
		check_metal_tier(caps, NextGI::TIER_HWRT, NextGI::TIER_REASON_NONE, NextGI::HARDWARE_RAY_TRACING_FORCE);
	}
}

TEST_CASE("[NextGI][Metal] Apple9, Apple10 and Apple11 give HWRT") {
	for (uint32_t family : { 1009u, 1010u, 1011u }) {
		const RenderingDeviceDriver::MetalRayQueryCaps caps = metal_family(family);
		CHECK(caps.ray_query);
		CHECK(caps.hardware);
		CHECK_EQ(caps.reason, RenderingDeviceDriver::RAY_QUERY_SUPPORTED);
		check_metal_tier(caps, NextGI::TIER_HWRT, NextGI::TIER_REASON_NONE);
	}
}

TEST_CASE("[NextGI][Metal] No Barriers sync gives METAL_SYNC_MODE") {
	const RenderingDeviceDriver::MetalRayQueryCaps caps = metal_family(1010, false, true);
	CHECK_FALSE(caps.ray_query);
	CHECK_FALSE(caps.hardware);
	CHECK_EQ(caps.reason, RenderingDeviceDriver::RAY_QUERY_REASON_METAL_SYNC_MODE);
	check_metal_tier(caps, NextGI::TIER_FALLBACK, NextGI::METAL_SYNC_MODE);

	// Barriers is checked before argument buffers.
	const RenderingDeviceDriver::MetalRayQueryCaps both = metal_family(1010, false, false);
	CHECK_EQ(both.reason, RenderingDeviceDriver::RAY_QUERY_REASON_METAL_SYNC_MODE);
	check_metal_tier(both, NextGI::TIER_FALLBACK, NextGI::METAL_SYNC_MODE);
}

TEST_CASE("[NextGI][Metal] Argument buffers off gives METAL_NO_ARGUMENT_BUFFERS") {
	const RenderingDeviceDriver::MetalRayQueryCaps caps = metal_family(1010, true, false);
	CHECK_FALSE(caps.ray_query);
	CHECK_EQ(caps.reason, RenderingDeviceDriver::RAY_QUERY_REASON_METAL_NO_ARGUMENT_BUFFERS);
	check_metal_tier(caps, NextGI::TIER_FALLBACK, NextGI::METAL_NO_ARGUMENT_BUFFERS);
}

TEST_CASE("[NextGI][Metal] macOS 13 gives NO_RAY_QUERY") {
	const RenderingDeviceDriver::MetalRayQueryCaps caps = metal_family(1010, true, true, true, 13);
	CHECK_FALSE(caps.ray_query);
	CHECK_FALSE(caps.hardware);
	CHECK_EQ(caps.reason, RenderingDeviceDriver::RAY_QUERY_REASON_NO_RAY_QUERY);
	check_metal_tier(caps, NextGI::TIER_FALLBACK, NextGI::NO_RAY_QUERY);
}

TEST_CASE("[NextGI][Metal] A missing ray-query capability gives NO_RAY_QUERY") {
	CHECK_EQ(metal_family(1010, true, true, false).reason, RenderingDeviceDriver::RAY_QUERY_REASON_NO_RAY_QUERY);
	CHECK_EQ(metal_family(1010, true, true, true, 14, false).reason, RenderingDeviceDriver::RAY_QUERY_REASON_NO_RAY_QUERY);
	CHECK_EQ(metal_family(1010, true, true, true, 14, true, 20300).reason, RenderingDeviceDriver::RAY_QUERY_REASON_NO_RAY_QUERY);
	CHECK_EQ(metal_family(1010, true, true, true, 14, true, RenderingDeviceDriver::METAL_RAY_QUERY_MIN_MSL, false).reason, RenderingDeviceDriver::RAY_QUERY_REASON_NO_RAY_QUERY);
	check_metal_tier(metal_family(1009, true, true, false), NextGI::TIER_FALLBACK, NextGI::NO_RAY_QUERY);
}

TEST_CASE("[NextGI][Metal] The acceleration structure instance size is 72") {
	CHECK_EQ(RenderingDeviceDriver::METAL_ACCELERATION_STRUCTURE_INSTANCE_SIZE, 72u);
}

// The Forward+ frame block (#26). The GPU side needs the HWRT tier; these cases cover its rules.

TEST_CASE("[NextGI] Frame block: 12 timestamp slots in plan §6.1 step 5 order, named per §2.1 line") {
	static const char *expected_names[NextGI::PASS_SLOT_MAX] = {
		"> NextGI",
		"NextGI: SRC capture",
		"NextGI: AS updates",
		"NextGI: AS builds",
		"NextGI: SRC relight",
		"NextGI: WRC update",
		"NextGI: Screen traces",
		"NextGI: Diffuse gather",
		"NextGI: Reflection traces",
		"NextGI: Integration",
		"NextGI: Denoise",
		"< NextGI",
	};
	static const char *expected_lines[NextGI::PASS_SLOT_MAX] = { "", "2a", "1a", "1b", "2b", "3", "4", "5", "6", "7", "8", "" };

	CHECK_EQ(int(NextGI::PASS_SLOT_MAX), 12);
	HashSet<String> names;
	HashSet<String> lines;
	for (int i = 0; i < NextGI::PASS_SLOT_MAX; i++) {
		CHECK_EQ(String(NextGI::PASS_SLOTS[i].name), String(expected_names[i]));
		CHECK_EQ(String(NextGI::PASS_SLOTS[i].line), String(expected_lines[i]));
		names.insert(NextGI::PASS_SLOTS[i].name);
		if (String(NextGI::PASS_SLOTS[i].line).is_empty()) {
			// Only the block markers have no line, and the profiler nests what they enclose.
			CHECK((String(NextGI::PASS_SLOTS[i].name).begins_with("> ") || String(NextGI::PASS_SLOTS[i].name).begins_with("< ")));
		} else {
			lines.insert(NextGI::PASS_SLOTS[i].line);
		}
	}
	CHECK_EQ(names.size(), 12u);
	// One slot per §2.1 line 1a-8.
	CHECK_EQ(lines.size(), 10u);
	CHECK_FALSE(names.has("Render GI"));

	// The enum names the same slots as the table.
	CHECK_EQ(String(NextGI::PASS_SLOTS[NextGI::PASS_SLOT_AS_BUILDS].line), "1b");
	CHECK_EQ(String(NextGI::PASS_SLOTS[NextGI::PASS_SLOT_SRC_CAPTURE].line), "2a");
	CHECK_EQ(String(NextGI::PASS_SLOTS[NextGI::PASS_SLOT_INTEGRATION].line), "7");
	CHECK_EQ(String(NextGI::PASS_SLOTS[NextGI::PASS_SLOT_BEGIN].name), "> NextGI");
	CHECK_EQ(String(NextGI::PASS_SLOTS[NextGI::PASS_SLOT_END].name), "< NextGI");
}

TEST_CASE("[NextGI] Frame block: HWRT views with nextgi_enabled only, never reflection probes") {
	CHECK(NextGI::view_uses_frame_block(NextGI::TIER_HWRT, false, true, false));

	// Reflection-probe renders never enter the block.
	CHECK_FALSE(NextGI::view_uses_frame_block(NextGI::TIER_HWRT, true, true, false));
	// The Environment has nextgi_enabled off, or there is no Environment.
	CHECK_FALSE(NextGI::view_uses_frame_block(NextGI::TIER_HWRT, false, false, false));
	// The unshaded debug draw.
	CHECK_FALSE(NextGI::view_uses_frame_block(NextGI::TIER_HWRT, false, true, true));

	// No other tier runs the block. The fallback tier keeps SDFGI and process_gi.
	for (int tier = 0; tier < NextGI::TIER_MAX; tier++) {
		if (tier == NextGI::TIER_HWRT) {
			continue;
		}
		CHECK_FALSE(NextGI::view_uses_frame_block(NextGI::Tier(tier), false, true, false));
		CHECK_FALSE(NextGI::view_uses_frame_block(NextGI::Tier(tier), true, true, false));
	}

	// Mocked devices: an RTX GPU on Vulkan runs the block, Metal without ray query (the M5 Max
	// until #47) and D3D12 without DXR do not.
	CHECK(NextGI::view_uses_frame_block(evaluate_mocked_device(true, "Vulkan", RenderingDeviceEnums::DEVICE_TYPE_DISCRETE_GPU).tier, false, true, false));
	CHECK_FALSE(NextGI::view_uses_frame_block(evaluate_mocked_device(false, "Metal", RenderingDeviceEnums::DEVICE_TYPE_INTEGRATED_GPU).tier, false, true, false));
	CHECK_FALSE(NextGI::view_uses_frame_block(evaluate_mocked_device(false, "D3D12", RenderingDeviceEnums::DEVICE_TYPE_DISCRETE_GPU).tier, false, true, false));
}

TEST_CASE("[NextGI] Frame block: RB_TEX_AMBIENT has the internal size, halved under half resolution") {
	CHECK_EQ(NextGI::get_gi_buffer_size(Size2i(1920, 1080), false), Size2i(1920, 1080));
	CHECK_EQ(NextGI::get_gi_buffer_size(Size2i(1920, 1080), true), Size2i(960, 540));
	// Odd sizes round down, as in GI::process_gi.
	CHECK_EQ(NextGI::get_gi_buffer_size(Size2i(1281, 721), true), Size2i(640, 360));
	CHECK_EQ(NextGI::get_gi_buffer_size(Size2i(1281, 721), false), Size2i(1281, 721));
}

TEST_CASE("[NextGI] Frame block: GI buffers reset as in process_gi, and when they cannot be cleared") {
	const uint32_t usage = NextGI::get_gi_buffer_usage_bits();
	// process_gi's sampling and storage bits, plus CAN_COPY_TO for texture_clear.
	CHECK((usage & RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT) != 0);
	CHECK((usage & RenderingDevice::TEXTURE_USAGE_STORAGE_BIT) != 0);
	CHECK((usage & RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT) != 0);

	// No buffers yet, same mode: nothing to drop.
	CHECK_FALSE(NextGI::gi_buffers_need_reset(false, 0, false, false));
	// Our own buffers in the same mode are kept.
	CHECK_FALSE(NextGI::gi_buffers_need_reset(true, usage, false, false));
	CHECK_FALSE(NextGI::gi_buffers_need_reset(true, usage, true, true));
	// gi/use_half_resolution changed.
	CHECK(NextGI::gi_buffers_need_reset(true, usage, false, true));
	CHECK(NextGI::gi_buffers_need_reset(true, usage, true, false));
	CHECK(NextGI::gi_buffers_need_reset(false, 0, true, false));
	// process_gi's buffers lack CAN_COPY_TO.
	const uint32_t process_gi_usage = RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_STORAGE_BIT;
	CHECK(NextGI::gi_buffers_need_reset(true, process_gi_usage, false, false));
}

TEST_CASE("[NextGI] Frame block: the shadow batch flag follows _render_shadow_begin and _render_shadow_end") {
	NextGI next_gi;
	CHECK_FALSE(next_gi.is_shadow_batch_open());
	next_gi.notify_shadow_batch_begin();
	CHECK(next_gi.is_shadow_batch_open());
	next_gi.notify_shadow_batch_end();
	CHECK_FALSE(next_gi.is_shadow_batch_open());

	// The omni cube batch opens on pass 0 and closes on pass 5, before the directional batch.
	next_gi.notify_shadow_batch_begin();
	next_gi.notify_shadow_batch_end();
	next_gi.notify_shadow_batch_begin();
	CHECK(next_gi.is_shadow_batch_open());
	next_gi.notify_shadow_batch_end();
	CHECK_FALSE(next_gi.is_shadow_batch_open());
}

// The RT scene v0 contracts and pure helpers (#28, plan D13 and §6.3). No GPU: the arena
// allocator, the decode and the part split are CPU logic; a mocked device covers the non-RT gate.

TEST_CASE("[NextGI] RT scene: eps_rt is min(max_extent/256, rt_max_error)") {
	// 40 m: max_extent/256 is 15.6 cm, so the 2 cm setting binds.
	CHECK(Math::is_equal_approx(RendererRD::nextgi_rt_compute_eps(40.0f, 0.02f), 0.02f));
	// 1 m: max_extent/256 is 3.9 mm and binds.
	CHECK(Math::is_equal_approx(RendererRD::nextgi_rt_compute_eps(1.0f, 0.02f), 1.0f / 256.0f));
}

TEST_CASE("[NextGI] RT scene: the coarsest LOD with edge_length <= eps_rt wins") {
	const float edge_lengths[] = { 0.001f, 0.01f, 0.1f };
	CHECK_EQ(RendererRD::nextgi_rt_select_lod(edge_lengths, 3, 0.02f), 1);
	CHECK_EQ(RendererRD::nextgi_rt_select_lod(edge_lengths, 3, 0.1f), 2);
	CHECK_EQ(RendererRD::nextgi_rt_select_lod(edge_lengths, 3, 0.0005f), -1); // Base indices.
	CHECK_EQ(RendererRD::nextgi_rt_select_lod(nullptr, 0, 1.0f), -1);
}

TEST_CASE("[NextGI] RT scene: 300k triangles split into 3 BLAS parts, never coarsened") {
	const uint32_t triangle_counts[] = { 300000 };
	const uint32_t chunks[] = { 0 };
	LocalVector<RendererRD::NextGIRTPartPlan> parts;
	RendererRD::nextgi_rt_split_parts(triangle_counts, chunks, 1, RendererRD::NEXTGI_RT_MAX_TRIANGLES_PER_BLAS, parts);
	REQUIRE_EQ(parts.size(), 3u);
	CHECK_EQ(parts[0].triangle_count, 131072u);
	CHECK_EQ(parts[1].triangle_count, 131072u);
	CHECK_EQ(parts[2].triangle_count, 37856u);
	uint32_t total = 0;
	for (const RendererRD::NextGIRTPartPlan &part : parts) {
		total += part.triangle_count;
		CHECK(part.triangle_count <= uint32_t(RendererRD::NEXTGI_RT_MAX_TRIANGLES_PER_BLAS));
	}
	CHECK_EQ(total, 300000u);
	CHECK_EQ(parts[1].geometries[0].first_triangle, 131072u);
}

TEST_CASE("[NextGI] RT scene: whole surfaces pack first and parts never span chunks") {
	const uint32_t triangle_counts[] = { 100000, 100000, 5000, 10 };
	const uint32_t chunks[] = { 0, 0, 0, 1 };
	LocalVector<RendererRD::NextGIRTPartPlan> parts;
	RendererRD::nextgi_rt_split_parts(triangle_counts, chunks, 4, RendererRD::NEXTGI_RT_MAX_TRIANGLES_PER_BLAS, parts);
	REQUIRE_EQ(parts.size(), 3u);
	CHECK_EQ(parts[0].geometries.size(), 1u);
	CHECK_EQ(parts[1].geometries.size(), 2u); // Surfaces 1 and 2 share one BLAS.
	CHECK_EQ(parts[1].first_surface, 1u);
	CHECK_EQ(parts[1].surface_count, 2u);
	CHECK_EQ(parts[2].chunk, 1u);
}

TEST_CASE("[NextGI] RT scene: arena ranges never move, frees coalesce, an unfit range opens a chunk") {
	RendererRD::NextGIArenaAllocator allocator(256);
	const RendererRD::NextGIArenaAllocator::Allocation a = allocator.allocate(100);
	const RendererRD::NextGIArenaAllocator::Allocation b = allocator.allocate(100);
	REQUIRE(a.is_valid());
	REQUIRE(b.is_valid());
	CHECK_EQ(a.chunk, 0u);
	CHECK_EQ(a.offset, 0u);
	CHECK_EQ(b.offset, 112u); // 16 B aligned.
	CHECK(a.new_chunk);
	CHECK_FALSE(b.new_chunk);

	allocator.free_range(a);
	const RendererRD::NextGIArenaAllocator::Allocation c = allocator.allocate(50);
	CHECK_EQ(c.offset, 0u); // The freed range is reused; b kept its offset.
	CHECK_EQ(b.offset, 112u);

	// Freeing both neighbors coalesces into a range that takes a 200 B allocation in chunk 0.
	allocator.free_range(c);
	allocator.free_range(b);
	const RendererRD::NextGIArenaAllocator::Allocation d = allocator.allocate(200);
	CHECK_EQ(d.chunk, 0u);
	CHECK_EQ(d.offset, 0u);

	// Nothing in chunk 0 fits another 100 B now, so a second chunk opens.
	const RendererRD::NextGIArenaAllocator::Allocation e = allocator.allocate(100);
	CHECK_EQ(e.chunk, 1u);
	CHECK(e.new_chunk);
	CHECK_EQ(allocator.get_chunk_count(), 2u);

	// Larger than one chunk never allocates.
	ERR_PRINT_OFF;
	CHECK_FALSE(allocator.allocate(257).is_valid());
	ERR_PRINT_ON;
}

TEST_CASE("[NextGI] RT scene: record layout, origin offset and index caps hold the §6.3 contract") {
	CHECK_EQ(sizeof(RendererRD::NextGIInstanceRecord), 80u);
	CHECK_EQ(sizeof(RendererRD::NextGISurfaceRecord), 16u);
	CHECK_EQ(RendererRD::NEXTGI_RT_TLAS_CAPACITY, 100000);
	CHECK(RendererRD::NEXTGI_RT_RECORD_INDEX_MAX < (1 << 24));
	CHECK_EQ(RendererRD::NEXTGI_RT_CHUNK_SIZE, 256 * 1024 * 1024);

	CHECK(Math::is_equal_approx(RendererRD::nextgi_rt_origin_offset(0.001f, 1.0f), 0.004f));
	CHECK(Math::is_equal_approx(RendererRD::nextgi_rt_origin_offset(0.02f, 10.0f), 0.05f)); // Capped.

	RendererRD::NextGIInstanceRecord record;
	CHECK_EQ(record.card_set, UINT32_MAX);
	CHECK_EQ(record.page_table, UINT32_MAX);
}

// GodotNext: nextgi_as_slots
TEST_CASE("[NextGI] RT scene: the TLAS and BLAS phases split across the AS slots with a one-frame lag") {
	// The frame block runs "AS updates" (line 1a) before "AS builds" (line 1b), so
	// rebuild_tlas can only admit BLASes an earlier frame's build_dirty_blas made.
	CHECK(NextGI::PASS_SLOT_AS_UPDATES < NextGI::PASS_SLOT_AS_BUILDS);
	CHECK_EQ(String(NextGI::PASS_SLOTS[NextGI::PASS_SLOT_AS_UPDATES].name), "NextGI: AS updates");
	CHECK_EQ(String(NextGI::PASS_SLOTS[NextGI::PASS_SLOT_AS_UPDATES].line), "1a");
	CHECK_EQ(String(NextGI::PASS_SLOTS[NextGI::PASS_SLOT_AS_BUILDS].name), "NextGI: AS builds");
	CHECK_EQ(String(NextGI::PASS_SLOTS[NextGI::PASS_SLOT_AS_BUILDS].line), "1b");

	// A part enters the TLAS only with a BLAS that exists and is not awaiting a rebuild.
	CHECK(RendererRD::nextgi_rt_part_enters_tlas(false, true));
	CHECK_FALSE(RendererRD::nextgi_rt_part_enters_tlas(true, true)); // Dirty: even a stale BLAS waits.
	CHECK_FALSE(RendererRD::nextgi_rt_part_enters_tlas(true, false)); // Dirty and never built.
	CHECK_FALSE(RendererRD::nextgi_rt_part_enters_tlas(false, false)); // No BLAS (build failed or ineligible).

	// The two-frame walk of one new mesh through the split phases: frame 1's TLAS phase
	// skips it, frame 1's builds phase makes its BLAS, frame 2's TLAS phase admits it.
	bool blas_dirty = true;
	bool blas_valid = false;
	CHECK_FALSE(RendererRD::nextgi_rt_part_enters_tlas(blas_dirty, blas_valid)); // Frame 1, AS updates.
	blas_valid = true; // Frame 1, AS builds.
	blas_dirty = false;
	CHECK(RendererRD::nextgi_rt_part_enters_tlas(blas_dirty, blas_valid)); // Frame 2, AS updates.

	// Both phases are callable on their own, in slot order, and stay no-ops before init
	// (no TLAS, no device), leaving the stats at zero.
	RendererRD::NextGIRTScene rt_scene;
	RenderDataRD *render_data = memnew(RenderDataRD);
	rt_scene.rebuild_tlas(render_data);
	CHECK_EQ(rt_scene.get_stats().fed_instance_count, 0u);
	CHECK_EQ(rt_scene.get_stats().instance_count, 0u);
	CHECK_EQ(rt_scene.get_stats().tlas_build_count, 0u);
	rt_scene.build_dirty_blas(render_data);
	CHECK_EQ(rt_scene.get_stats().blas_build_count, 0u);
	memdelete(render_data);
}

TEST_CASE("[NextGI] RT scene: a mocked device without ray query enables no arena and no BLAS") {
	RenderingDevice *rd = memnew(RenderingDevice);
	rd->set_feature_mock(true, 0);
	CHECK_FALSE(rd->has_feature(RenderingDevice::SUPPORTS_RAY_QUERY));
	CHECK_FALSE(RendererRD::nextgi_rt_should_enable(true, rd->has_feature(RenderingDevice::SUPPORTS_RAY_QUERY)));
	rd->set_feature_mock(true, ray_query_feature_bits(true));
	CHECK(RendererRD::nextgi_rt_should_enable(true, rd->has_feature(RenderingDevice::SUPPORTS_RAY_QUERY)));
	CHECK_FALSE(RendererRD::nextgi_rt_should_enable(false, rd->has_feature(RenderingDevice::SUPPORTS_RAY_QUERY)));
	memdelete(rd);
}

// Builds one indexed triangle soup through the canonical surface-data path, optionally compressed.
static RenderingServerTypes::SurfaceData rt_test_surface_data(const PackedVector3Array &p_vertices, const PackedInt32Array &p_indices, bool p_compressed) {
	Array arrays;
	arrays.resize(RSE::ARRAY_MAX);
	arrays[RSE::ARRAY_VERTEX] = p_vertices;
	arrays[RSE::ARRAY_INDEX] = p_indices;
	RenderingServerTypes::SurfaceData surface_data;
	const uint64_t compress_format = p_compressed ? uint64_t(RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES) : 0;
	const Error err = RenderingServer::get_singleton()->mesh_create_surface_data_from_arrays(&surface_data, RSE::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(), compress_format);
	REQUIRE_EQ(err, OK);
	return surface_data;
}

static void check_rt_decode_matches_arrays(bool p_compressed) {
	PackedVector3Array vertices;
	vertices.push_back(Vector3(-2.0f, 0.0f, 1.0f));
	vertices.push_back(Vector3(3.0f, -1.0f, 0.5f));
	vertices.push_back(Vector3(0.25f, 4.0f, -3.0f));
	vertices.push_back(Vector3(1.0f, 1.0f, 1.0f));
	PackedInt32Array indices;
	indices.push_back(0);
	indices.push_back(1);
	indices.push_back(2);
	indices.push_back(0);
	indices.push_back(2);
	indices.push_back(3);

	const RenderingServerTypes::SurfaceData surface_data = rt_test_surface_data(vertices, indices, p_compressed);
	if (p_compressed) {
		CHECK((surface_data.format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES) != 0);
	}

	Vector<float> positions;
	REQUIRE(RendererRD::nextgi_rt_decode_positions(surface_data.format, surface_data.vertex_data, surface_data.vertex_count, surface_data.aabb, positions));
	REQUIRE_EQ(positions.size(), 12);

	// The reference is the engine's own decode of the same bytes.
	const Array arrays = RenderingServer::get_singleton()->mesh_create_arrays_from_surface_data(surface_data);
	const PackedVector3Array reference = arrays[RSE::ARRAY_VERTEX];
	REQUIRE_EQ(reference.size(), 4);
	const double tolerance = 1e-6 * double(surface_data.aabb.get_longest_axis_size());
	for (int i = 0; i < 4; i++) {
		CHECK(Math::abs(double(positions[i * 3 + 0]) - double(reference[i].x)) <= tolerance);
		CHECK(Math::abs(double(positions[i * 3 + 1]) - double(reference[i].y)) <= tolerance);
		CHECK(Math::abs(double(positions[i * 3 + 2]) - double(reference[i].z)) <= tolerance);
	}
}

TEST_CASE("[NextGI] RT scene: decoded positions match mesh_create_arrays_from_surface_data") {
	REQUIRE(RenderingServer::get_singleton() != nullptr);
	check_rt_decode_matches_arrays(false);
	check_rt_decode_matches_arrays(true);
}

TEST_CASE("[NextGI] RT scene: record to surface to chunk plus offset reproduces 1000 seeded triangles") {
	REQUIRE(RenderingServer::get_singleton() != nullptr);
	// A deterministic LCG soup of 1000 triangles.
	uint32_t seed = 0x12345678u;
	auto next_float = [&seed]() {
		seed = seed * 1664525u + 1013904223u;
		return float((seed >> 8) & 0xFFFFFF) / float(0xFFFFFF) * 20.0f - 10.0f;
	};
	PackedVector3Array vertices;
	PackedInt32Array indices;
	for (int i = 0; i < 1000; i++) {
		for (int v = 0; v < 3; v++) {
			vertices.push_back(Vector3(next_float(), next_float(), next_float()));
			indices.push_back(i * 3 + v);
		}
	}
	const RenderingServerTypes::SurfaceData surface_data = rt_test_surface_data(vertices, indices, false);

	Vector<float> positions;
	REQUIRE(RendererRD::nextgi_rt_decode_positions(surface_data.format, surface_data.vertex_data, surface_data.vertex_count, surface_data.aabb, positions));
	Vector<uint32_t> widened;
	const bool is_index_16 = surface_data.vertex_count <= 65536 && surface_data.vertex_count > 0;
	RendererRD::nextgi_rt_widen_indices(surface_data.index_data, surface_data.index_count, is_index_16, surface_data.vertex_count, widened);
	REQUIRE_EQ(widened.size(), 3000);

	// Write the decoded geometry into a CPU mirror of an arena chunk at the allocator's offsets.
	RendererRD::NextGIArenaAllocator allocator(1 << 20);
	const uint32_t vertex_bytes = uint32_t(positions.size()) * sizeof(float);
	const uint32_t index_bytes = uint32_t(widened.size()) * sizeof(uint32_t);
	const RendererRD::NextGIArenaAllocator::Allocation allocation = allocator.allocate(vertex_bytes + index_bytes);
	REQUIRE(allocation.is_valid());
	Vector<uint8_t> chunk;
	chunk.resize(1 << 20);
	memcpy(chunk.ptrw() + allocation.offset, positions.ptr(), vertex_bytes);
	memcpy(chunk.ptrw() + allocation.offset + vertex_bytes, widened.ptr(), index_bytes);

	// Read it back the way a hit shader does: instance record -> surface record -> chunk + offset.
	RendererRD::NextGISurfaceRecord surface_record;
	surface_record.vertex_offset = allocation.offset;
	surface_record.index_offset = allocation.offset + vertex_bytes;
	surface_record.triangle_count = 1000;
	RendererRD::NextGIInstanceRecord instance_record;
	instance_record.surface_base = 0;
	instance_record.surface_count = 1;
	instance_record.arena_chunk = allocation.chunk;
	CHECK_EQ(instance_record.arena_chunk, 0u);

	const float *chunk_positions = reinterpret_cast<const float *>(chunk.ptr() + surface_record.vertex_offset);
	const uint32_t *chunk_indices = reinterpret_cast<const uint32_t *>(chunk.ptr() + surface_record.index_offset);
	int wrong = 0;
	for (uint32_t t = 0; t < surface_record.triangle_count; t++) {
		for (int corner = 0; corner < 3; corner++) {
			const uint32_t index = chunk_indices[t * 3 + corner];
			if (index != uint32_t(indices[t * 3 + corner])) {
				wrong++;
				continue;
			}
			const Vector3 expected = vertices[index];
			if (chunk_positions[index * 3 + 0] != expected.x || chunk_positions[index * 3 + 1] != expected.y || chunk_positions[index * 3 + 2] != expected.z) {
				wrong++;
			}
		}
	}
	CHECK_EQ(wrong, 0);
}

// SRC Stage-1 cards (#30, plan §8.3 and D6). Clustering, packing and coverage are CPU logic; the
// capture itself runs on the GPU (godotnext/testbed/tests/src_cards).

struct SRCTestMesh {
	LocalVector<float> positions;
	LocalVector<uint32_t> indices;

	uint32_t add_vertex(const Vector3 &p_position) {
		positions.push_back(float(p_position.x));
		positions.push_back(float(p_position.y));
		positions.push_back(float(p_position.z));
		return positions.size() / 3 - 1;
	}

	// p0..p2 counter-clockwise around the outward normal, emitted clockwise: Godot's front faces
	// wind clockwise.
	void add_triangle(const Vector3 &p_0, const Vector3 &p_1, const Vector3 &p_2) {
		const uint32_t a = add_vertex(p_0);
		const uint32_t b = add_vertex(p_1);
		const uint32_t c = add_vertex(p_2);
		indices.push_back(a);
		indices.push_back(c);
		indices.push_back(b);
	}

	// p0..p3 counter-clockwise around the outward normal, emitted clockwise as two triangles.
	void add_quad(const Vector3 &p_0, const Vector3 &p_1, const Vector3 &p_2, const Vector3 &p_3) {
		const uint32_t a = add_vertex(p_0);
		const uint32_t b = add_vertex(p_1);
		const uint32_t c = add_vertex(p_2);
		const uint32_t d = add_vertex(p_3);
		indices.push_back(a);
		indices.push_back(c);
		indices.push_back(b);
		indices.push_back(a);
		indices.push_back(d);
		indices.push_back(c);
	}

	// A rectangle around p_center; p_u x p_v is its outward normal.
	void add_face(const Vector3 &p_center, const Vector3 &p_u, const Vector3 &p_v, real_t p_half_u, real_t p_half_v) {
		add_quad(p_center - p_u * p_half_u - p_v * p_half_v, p_center + p_u * p_half_u - p_v * p_half_v, p_center + p_u * p_half_u + p_v * p_half_v, p_center - p_u * p_half_u + p_v * p_half_v);
	}

	void add_box(const Vector3 &p_center, const Vector3 &p_half_size) {
		const Vector3 x(1, 0, 0);
		const Vector3 y(0, 1, 0);
		const Vector3 z(0, 0, 1);
		add_face(p_center + x * p_half_size.x, y, z, p_half_size.y, p_half_size.z);
		add_face(p_center - x * p_half_size.x, z, y, p_half_size.z, p_half_size.y);
		add_face(p_center + y * p_half_size.y, z, x, p_half_size.z, p_half_size.x);
		add_face(p_center - y * p_half_size.y, x, z, p_half_size.x, p_half_size.z);
		add_face(p_center + z * p_half_size.z, x, y, p_half_size.x, p_half_size.y);
		add_face(p_center - z * p_half_size.z, y, x, p_half_size.y, p_half_size.x);
	}

	// A prism over a counter-clockwise polygon in XY, from z = 0 to p_depth. Caps are fans.
	void add_prism(const LocalVector<Vector2> &p_polygon, real_t p_depth) {
		for (uint32_t i = 0; i < p_polygon.size(); i++) {
			const Vector2 a = p_polygon[i];
			const Vector2 b = p_polygon[(i + 1) % p_polygon.size()];
			add_quad(Vector3(a.x, a.y, 0), Vector3(b.x, b.y, 0), Vector3(b.x, b.y, p_depth), Vector3(a.x, a.y, p_depth));
		}
	}

	void transform(const Transform3D &p_transform) {
		for (uint32_t i = 0; i < positions.size(); i += 3) {
			const Vector3 p = p_transform.xform(Vector3(positions[i], positions[i + 1], positions[i + 2]));
			positions[i] = float(p.x);
			positions[i + 1] = float(p.y);
			positions[i + 2] = float(p.z);
		}
	}

	void cluster(RendererRD::NextGICardClusterResult &r_result) const {
		NextGI::cluster_cards(positions.ptr(), positions.size() / 3, indices.ptr(), indices.size(), r_result);
	}
};

static bool src_vectors_equal(const Vector3 &p_a, const Vector3 &p_b) {
	return (p_a - p_b).length() < 1e-5;
}

static void check_src_card_limits(const RendererRD::NextGICardClusterResult &p_result) {
	CHECK(p_result.cards.size() <= uint32_t(RendererRD::NEXTGI_SRC_MAX_CARDS_PER_MESH));
	for (const RendererRD::NextGICardDesc &card : p_result.cards) {
		CHECK(card.texels_u >= uint32_t(RendererRD::NEXTGI_SRC_CARD_MIN_TEXELS));
		CHECK(card.texels_v >= uint32_t(RendererRD::NEXTGI_SRC_CARD_MIN_TEXELS));
		CHECK(card.texels_u <= uint32_t(RendererRD::NEXTGI_SRC_CARD_MAX_TEXELS));
		CHECK(card.texels_v <= uint32_t(RendererRD::NEXTGI_SRC_CARD_MAX_TEXELS));
		// A right-handed frame, so the capture camera culls like VoxelGI's look_at cameras.
		CHECK(Math::is_equal_approx(card.get_transform().basis.determinant(), real_t(1.0), real_t(1e-5)));
		CHECK(card.depth_range > 0.0f);
	}
}

TEST_CASE("[NextGI] SRC cards: the GPU card records keep the §8.3 layout") {
	CHECK_EQ(sizeof(RendererRD::NextGICard), 80u);
	CHECK_EQ(sizeof(RendererRD::NextGICardSet), 16u);
	CHECK_EQ(RendererRD::NEXTGI_SRC_MAX_CARDS_PER_MESH, 12);
	CHECK_EQ(RendererRD::NEXTGI_SRC_CARD_MAX_TEXELS, 128);
	// Until the SRC exists, records name no card set.
	CHECK_EQ(RendererRD::NextGISRC::get_mesh_card_set(RID()), UINT32_MAX);
	CHECK_EQ(RendererRD::NextGIInstanceRecord().card_set, UINT32_MAX);
}

TEST_CASE("[NextGI] SRC cards: an axis-aligned box gives exactly 6 axis cards with exact rects") {
	const Vector3 center(0.5, 0.0, 0.0);
	const Vector3 half_size(1.0, 0.5, 0.25);
	SRCTestMesh mesh;
	mesh.add_box(center, half_size);
	RendererRD::NextGICardClusterResult result;
	mesh.cluster(result);

	REQUIRE_EQ(result.cards.size(), 6u);
	CHECK(Math::is_equal_approx(result.max_extent, 2.0f));
	const float texel = result.texel_size;
	CHECK(Math::is_equal_approx(texel, 2.0f / 128.0f));
	check_src_card_limits(result);

	// Seed order: +X, -X, +Y, -Y, +Z, -Z.
	static const Vector3 axes[6] = { Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 1, 0), Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(0, 0, -1) };
	LocalVector<Vector2i> sizes;
	for (uint32_t i = 0; i < 6; i++) {
		const RendererRD::NextGICardDesc &card = result.cards[i];
		CHECK(src_vectors_equal(card.axis_n, axes[i]));
		CHECK_EQ(card.triangle_count, 2u);
		const real_t face_distance = half_size.dot(axes[i].abs());
		const real_t half_u = half_size.dot(card.axis_u.abs());
		const real_t half_v = half_size.dot(card.axis_v.abs());
		CHECK(Math::is_equal_approx(real_t(card.half_u), half_u));
		CHECK(Math::is_equal_approx(real_t(card.half_v), half_v));
		CHECK(Math::is_equal_approx(card.area, float(4.0 * half_u * half_v)));
		// The camera sits one texel out from the face and reaches one texel past the far side.
		CHECK(src_vectors_equal(card.origin, center + axes[i] * (face_distance + texel)));
		CHECK(Math::is_equal_approx(real_t(card.depth_range), real_t(face_distance * 2.0 + texel * 2.0)));
		CHECK_EQ(card.texels_u, uint32_t(Math::round(half_u * 2.0 / texel)));
		CHECK_EQ(card.texels_v, uint32_t(Math::round(half_v * 2.0 / texel)));
		sizes.push_back(Vector2i(int(card.texels_u), int(card.texels_v)));
	}
	// The +X face is 1 x 0.5, the +Y face 2 x 0.5 and the +Z face 2 x 1.
	CHECK_EQ(result.cards[0].texels_u * result.cards[0].texels_v, 64u * 32u);
	CHECK_EQ(result.cards[2].texels_u * result.cards[2].texels_v, 128u * 32u);
	CHECK_EQ(result.cards[4].texels_u * result.cards[4].texels_v, 128u * 64u);

	// The packed atlas rects are exactly the card sizes, inside the atlas and apart by the gutter.
	RendererRD::NextGIShelfPacker packer;
	LocalVector<Rect2i> rects;
	REQUIRE(packer.allocate_all(sizes, rects));
	REQUIRE_EQ(rects.size(), 6u);
	const Rect2i atlas(0, 0, RendererRD::NEXTGI_SRC_ATLAS_SIZE, RendererRD::NEXTGI_SRC_ATLAS_SIZE);
	for (uint32_t i = 0; i < 6; i++) {
		CHECK_EQ(rects[i].size, sizes[i]);
		CHECK(atlas.encloses(rects[i]));
		for (uint32_t j = i + 1; j < 6; j++) {
			CHECK_FALSE(rects[i].grow_individual(0, 0, RendererRD::NEXTGI_SRC_ATLAS_GUTTER, RendererRD::NEXTGI_SRC_ATLAS_GUTTER).intersects(rects[j]));
		}
	}
}

TEST_CASE("[NextGI] SRC cards: the card frame maps capture-view coordinates to object space") {
	SRCTestMesh mesh;
	mesh.add_box(Vector3(0.0, 2.0, 0.0), Vector3(1.0, 1.0, 1.0));
	RendererRD::NextGICardClusterResult result;
	mesh.cluster(result);
	REQUIRE_EQ(result.cards.size(), 6u);
	for (const RendererRD::NextGICardDesc &card : result.cards) {
		const Transform3D frame = card.get_transform();
		CHECK(src_vectors_equal(frame.xform(Vector3()), card.origin));
		CHECK(src_vectors_equal(frame.basis.get_column(0), card.axis_u));
		CHECK(src_vectors_equal(frame.basis.get_column(1), card.axis_v));
		CHECK(src_vectors_equal(frame.basis.get_column(2), card.axis_n));
		// The camera looks along -n: the face's center is one texel (the depth) in front of it.
		const Vector3 face_center = Vector3(0.0, 2.0, 0.0) + card.axis_n;
		CHECK(src_vectors_equal(frame.xform(Vector3(0, 0, -result.texel_size)), face_center));
	}
}

TEST_CASE("[NextGI] SRC cards: an L-shaped concave mesh gives at most 12 cards") {
	// An L with a chamfered outer corner, extruded along Z, plus its two caps.
	LocalVector<Vector2> polygon;
	polygon.push_back(Vector2(0.0, 0.0));
	polygon.push_back(Vector2(1.6, 0.0));
	polygon.push_back(Vector2(2.0, 0.4));
	polygon.push_back(Vector2(2.0, 1.0));
	polygon.push_back(Vector2(1.0, 1.0));
	polygon.push_back(Vector2(1.0, 2.0));
	polygon.push_back(Vector2(0.0, 2.0));
	SRCTestMesh mesh;
	mesh.add_prism(polygon, 0.5);
	const Vector3 x(1, 0, 0);
	const Vector3 y(0, 1, 0);
	// Caps from three rectangles and a triangle by the chamfer; +Z at z = 0.5, -Z at z = 0.
	mesh.add_face(Vector3(0.8, 0.5, 0.5), x, y, 0.8, 0.5);
	mesh.add_face(Vector3(0.5, 1.5, 0.5), x, y, 0.5, 0.5);
	mesh.add_face(Vector3(1.8, 0.7, 0.5), x, y, 0.2, 0.3);
	mesh.add_triangle(Vector3(1.6, 0.0, 0.5), Vector3(2.0, 0.4, 0.5), Vector3(1.6, 0.4, 0.5));
	mesh.add_face(Vector3(0.8, 0.5, 0.0), y, x, 0.5, 0.8);
	mesh.add_face(Vector3(0.5, 1.5, 0.0), y, x, 0.5, 0.5);
	mesh.add_face(Vector3(1.8, 0.7, 0.0), y, x, 0.3, 0.2);
	mesh.add_triangle(Vector3(1.6, 0.0, 0.0), Vector3(1.6, 0.4, 0.0), Vector3(2.0, 0.4, 0.0));

	RendererRD::NextGICardClusterResult result;
	mesh.cluster(result);
	check_src_card_limits(result);
	// Six axis cards, and the chamfer (about 3% of the area) keeps one of its own.
	CHECK_EQ(result.cards.size(), 7u);
	CHECK(Math::is_equal_approx(result.texel_size, result.max_extent / 128.0f));
	CHECK(Math::is_equal_approx(result.max_extent, 2.0f));
	bool has_chamfer = false;
	for (const RendererRD::NextGICardDesc &card : result.cards) {
		has_chamfer = has_chamfer || src_vectors_equal(card.axis_n, Vector3(1, -1, 0).normalized());
		// The +X card covers the outer face at x = 2 and the inner one at x = 1, a unit deeper.
		if (src_vectors_equal(card.axis_n, x)) {
			CHECK(Math::is_equal_approx(real_t(card.origin.x), real_t(2.0 + result.texel_size)));
			CHECK(Math::is_equal_approx(real_t(card.depth_range), real_t(2.0 + 2.0 * result.texel_size)));
			CHECK_EQ(card.triangle_count, 4u);
		}
	}
	CHECK(has_chamfer);
}

TEST_CASE("[NextGI] SRC cards: a sphere clusters into at most 12 cards") {
	SRCTestMesh mesh;
	const int rings = 16;
	const int segments = 32;
	auto point = [](int p_ring, int p_segment) {
		const real_t theta = Math::PI * real_t(p_ring) / real_t(rings);
		const real_t phi = Math::TAU * real_t(p_segment) / real_t(segments);
		return Vector3(Math::sin(theta) * Math::cos(phi), Math::cos(theta), -Math::sin(theta) * Math::sin(phi)) * 3.0;
	};
	for (int ring = 0; ring < rings; ring++) {
		for (int segment = 0; segment < segments; segment++) {
			mesh.add_quad(point(ring, segment), point(ring + 1, segment), point(ring + 1, segment + 1), point(ring, segment + 1));
		}
	}
	RendererRD::NextGICardClusterResult result;
	mesh.cluster(result);
	check_src_card_limits(result);
	CHECK(result.cards.size() > 6u);
	CHECK(Math::is_equal_approx(result.texel_size, result.max_extent / 128.0f));
	// Every card faces outward: its normal points the same way as its origin from the center.
	for (const RendererRD::NextGICardDesc &card : result.cards) {
		CHECK(card.origin.normalized().dot(card.axis_n) > 0.5);
	}
}

TEST_CASE("[NextGI] SRC cards: no card exceeds 128x128 texels") {
	// A 4 m plank turned 45 degrees: its own card is wider than the AABB's longest axis.
	SRCTestMesh mesh;
	mesh.add_box(Vector3(), Vector3(2.0, 0.05, 0.05));
	mesh.transform(Transform3D(Basis(Vector3(0, 0, 1), Math::PI / 4.0), Vector3()));
	RendererRD::NextGICardClusterResult result;
	mesh.cluster(result);
	check_src_card_limits(result);
	CHECK(result.max_extent < 4.0f);
	uint32_t largest = 0;
	for (const RendererRD::NextGICardDesc &card : result.cards) {
		largest = MAX(largest, MAX(card.texels_u, card.texels_v));
	}
	CHECK_EQ(largest, uint32_t(RendererRD::NEXTGI_SRC_CARD_MAX_TEXELS));
}

TEST_CASE("[NextGI] SRC cards: clusters under 2% of the area merge into their nearest neighbor") {
	// A unit box plus a 45-degree sliver of 0.17% of the area: it gets a seed, then merges.
	SRCTestMesh mesh;
	mesh.add_box(Vector3(), Vector3(0.5, 0.5, 0.5));
	const Vector3 diagonal = Vector3(1, 1, 0).normalized();
	const Vector3 across = Vector3(-1, 1, 0).normalized(); // across x Z = diagonal.
	mesh.add_face(Vector3(0.6, 0.6, 0.0), across, Vector3(0, 0, 1), 0.05, 0.05);
	RendererRD::NextGICardClusterResult result;
	mesh.cluster(result);
	check_src_card_limits(result);
	CHECK_EQ(result.cards.size(), 6u);
	uint32_t triangles = 0;
	for (const RendererRD::NextGICardDesc &card : result.cards) {
		triangles += card.triangle_count;
		CHECK_FALSE(src_vectors_equal(card.axis_n, diagonal));
	}
	CHECK_EQ(triangles, 14u);

	// The same sliver at 10 times the size (14% of the area) keeps a card.
	SRCTestMesh large;
	large.add_box(Vector3(), Vector3(0.5, 0.5, 0.5));
	large.add_face(Vector3(0.8, 0.8, 0.0), across, Vector3(0, 0, 1), 0.5, 0.5);
	large.cluster(result);
	check_src_card_limits(result);
	CHECK_EQ(result.cards.size(), 7u);
}

TEST_CASE("[NextGI] SRC cards: degenerate and empty input gives no cards") {
	RendererRD::NextGICardClusterResult result;
	NextGI::cluster_cards(nullptr, 0, nullptr, 0, result);
	CHECK(result.cards.is_empty());

	SRCTestMesh mesh;
	mesh.add_quad(Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(2, 0, 0), Vector3(3, 0, 0));
	mesh.cluster(result);
	CHECK(result.cards.is_empty());

	// Out-of-range indices are skipped.
	const float positions[9] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
	const uint32_t indices[3] = { 0, 1, 5 };
	NextGI::cluster_cards(positions, 3, indices, 3, result);
	CHECK(result.cards.is_empty());
}

TEST_CASE("[NextGI] SRC cards: the shelf packer fills the atlas and places a card set whole or not at all") {
	RendererRD::NextGIShelfPacker packer;
	Rect2i rect;
	// 128 + 1 gutter: 15 cards per shelf and 15 shelves in 2048.
	for (int i = 0; i < 225; i++) {
		REQUIRE(packer.allocate(128, 128, rect));
	}
	CHECK_EQ(rect, Rect2i(14 * 129, 14 * 129, 128, 128));
	CHECK_FALSE(packer.allocate(128, 128, rect));
	CHECK_EQ(packer.get_used_texels(), uint64_t(225) * 128 * 128);
	// Smaller cards still fit the right edge and the bottom.
	CHECK(packer.allocate(64, 64, rect));
	CHECK_EQ(rect.position.x, 15 * 129);

	RendererRD::NextGIShelfPacker small(300, 140);
	LocalVector<Vector2i> sizes;
	sizes.push_back(Vector2i(128, 128));
	sizes.push_back(Vector2i(128, 128));
	sizes.push_back(Vector2i(128, 128));
	LocalVector<Rect2i> rects;
	CHECK_FALSE(small.allocate_all(sizes, rects));
	CHECK_EQ(small.get_used_texels(), 0u);
	sizes.resize(2);
	CHECK(small.allocate_all(sizes, rects));
	CHECK_EQ(rects[0], Rect2i(0, 0, 128, 128));
	CHECK_EQ(rects[1], Rect2i(129, 0, 128, 128));

	// Tallest first, but the rects come back in input order.
	RendererRD::NextGIShelfPacker ordered;
	sizes.clear();
	sizes.push_back(Vector2i(16, 8));
	sizes.push_back(Vector2i(16, 32));
	CHECK(ordered.allocate_all(sizes, rects));
	CHECK_EQ(rects[1], Rect2i(0, 0, 16, 32));
	CHECK_EQ(rects[0], Rect2i(17, 0, 16, 8));
}

TEST_CASE("[NextGI] SRC cards: atlas memory is under the 128 MiB High budget") {
	// D16 framebuffer depth: 26 bytes per texel at 2048x2048 is 104 MiB.
	CHECK_EQ(RendererRD::nextgi_src_atlas_memory_bytes(2048, 2), uint64_t(104) * 1024 * 1024);
	CHECK(RendererRD::nextgi_src_atlas_memory_bytes(RendererRD::NEXTGI_SRC_ATLAS_SIZE, 2) <= uint64_t(RendererRD::NEXTGI_SRC_MATERIAL_BUDGET_HIGH));
	// The X8_D24 fallback: 112 MiB.
	CHECK(RendererRD::nextgi_src_atlas_memory_bytes(RendererRD::NEXTGI_SRC_ATLAS_SIZE, 4) <= uint64_t(RendererRD::NEXTGI_SRC_MATERIAL_BUDGET_HIGH));
}

TEST_CASE("[NextGI] SRC cards: coverage counts card texels with depth above 0") {
	float depth[8 * 8] = {};
	// 12 of the 16 texels of the rect (2, 2, 4, 4) are covered; texels outside it never count.
	for (int y = 2; y < 6; y++) {
		for (int x = 2; x < 6; x++) {
			depth[y * 8 + x] = y < 5 ? 0.25f : 0.0f;
		}
	}
	depth[0] = 1.0f;
	CHECK(Math::is_equal_approx(RendererRD::nextgi_src_card_coverage(depth, 8, 8, Rect2i(2, 2, 4, 4)), 0.75f));
	CHECK(Math::is_equal_approx(RendererRD::nextgi_src_card_coverage(depth, 8, 8, Rect2i(2, 2, 4, 3)), 1.0f));
	ERR_PRINT_OFF;
	CHECK_EQ(RendererRD::nextgi_src_card_coverage(depth, 8, 8, Rect2i(6, 6, 4, 4)), 0.0f);
	ERR_PRINT_ON;
}

TEST_CASE("[NextGI] SRC relight: DISABLED never enters and STATIC is dropped only for lightmapped receivers") {
	using namespace RendererRD;
	CHECK_EQ(sizeof(NextGILight), 64);
	CHECK_EQ(nextgi_positional_light_list(true, true, false, RSE::LIGHT_BAKE_DISABLED), NEXTGI_POSITIONAL_LIGHT_NONE);
	CHECK_EQ(nextgi_positional_light_list(true, true, true, RSE::LIGHT_BAKE_STATIC), NEXTGI_POSITIONAL_LIGHT_NONE);
	CHECK_EQ(nextgi_positional_light_list(true, true, false, RSE::LIGHT_BAKE_DYNAMIC), NEXTGI_POSITIONAL_LIGHT_DYNAMIC);
	CHECK_EQ(nextgi_positional_light_list(true, true, false, RSE::LIGHT_BAKE_STATIC), NEXTGI_POSITIONAL_LIGHT_STATIC);
	CHECK_EQ(nextgi_positional_light_list(false, true, false, RSE::LIGHT_BAKE_STATIC), NEXTGI_POSITIONAL_LIGHT_NONE);
	CHECK_EQ(nextgi_positional_light_list(true, false, false, RSE::LIGHT_BAKE_STATIC), NEXTGI_POSITIONAL_LIGHT_NONE);

	LocalVector<int> dynamic_lights;
	LocalVector<int> static_lights;
	nextgi_sync_positional_light(dynamic_lights, static_lights, 7, nextgi_positional_light_list(true, true, false, RSE::LIGHT_BAKE_STATIC));
	REQUIRE_EQ(static_lights.size(), 1);
	CHECK(dynamic_lights.is_empty());
	// Hidden, then shown again.
	nextgi_sync_positional_light(dynamic_lights, static_lights, 7, nextgi_positional_light_list(false, true, false, RSE::LIGHT_BAKE_STATIC));
	CHECK(static_lights.is_empty());
	nextgi_sync_positional_light(dynamic_lights, static_lights, 7, nextgi_positional_light_list(true, true, false, RSE::LIGHT_BAKE_STATIC));
	CHECK_EQ(static_lights.size(), 1);
	// Leaving the scenario drops it. Entering another scenario puts it back.
	nextgi_sync_positional_light(dynamic_lights, static_lights, 7, nextgi_positional_light_list(true, false, false, RSE::LIGHT_BAKE_STATIC));
	CHECK(static_lights.is_empty());
	nextgi_sync_positional_light(dynamic_lights, static_lights, 7, nextgi_positional_light_list(true, true, false, RSE::LIGHT_BAKE_STATIC));
	CHECK_EQ(static_lights[0], 7);
	// Bake mode moves it onto the dynamic list and off the static one.
	nextgi_sync_positional_light(dynamic_lights, static_lights, 7, nextgi_positional_light_list(true, true, false, RSE::LIGHT_BAKE_DYNAMIC));
	CHECK(static_lights.is_empty());
	REQUIRE_EQ(dynamic_lights.size(), 1);
	CHECK_EQ(dynamic_lights[0], 7);
	nextgi_sync_positional_light(dynamic_lights, static_lights, 7, nextgi_positional_light_list(true, true, false, RSE::LIGHT_BAKE_DISABLED));
	CHECK(dynamic_lights.is_empty());
	CHECK(static_lights.is_empty());

	CHECK(nextgi_light_reaches_receiver(true, 0));
	CHECK_FALSE(nextgi_light_reaches_receiver(true, NEXTGI_RT_FLAG_LIGHTMAPPED));
	CHECK(nextgi_light_reaches_receiver(false, NEXTGI_RT_FLAG_LIGHTMAPPED));
}

TEST_CASE("[NextGI] SRC relight: page lights, the High schedule and the two mask slots") {
	using namespace RendererRD;
	NextGIPageLightCandidate lights[10];
	for (uint32_t i = 0; i < 10; i++) {
		lights[i].light_index = i;
		lights[i].energy = 10.0f - float(i);
		lights[i].has_shadow = i != 1;
	}
	LocalVector<uint32_t> shadowed;
	LocalVector<uint32_t> unshadowed;
	nextgi_select_page_lights(lights, 10, NEXTGI_SRC_MAX_SHADOWED_LIGHTS, shadowed, unshadowed);
	REQUIRE_EQ(shadowed.size(), 8);
	CHECK_EQ(shadowed[0], 0);
	CHECK_EQ(shadowed[1], 2); // The brighter unshadowed light does not take a shadow slot.
	CHECK_EQ(unshadowed[0], 1);
	CHECK_EQ(unshadowed[unshadowed.size() - 1], 9);
	CHECK_EQ(shadowed.size() + unshadowed.size(), 10);

	uint32_t slot_a = 0;
	uint32_t slot_b = 0;
	nextgi_mask_slots(0, 0, slot_a, slot_b);
	CHECK_EQ(slot_a, UINT32_MAX);
	nextgi_mask_slots(0, 4, slot_a, slot_b);
	CHECK_EQ(slot_a, 0);
	CHECK_EQ(slot_b, 1);
	nextgi_mask_slots(1, 4, slot_a, slot_b);
	CHECK_EQ(slot_a, 2);
	CHECK_EQ(slot_b, 3);
	nextgi_mask_slots(1, 1, slot_a, slot_b);
	CHECK_EQ(slot_a, 0);
	CHECK_EQ(slot_b, 0);

	uint32_t texels[5] = { 100000, 100000, 100000, 100000, 100000 };
	uint32_t indices[5] = {};
	uint32_t cursor = 0;
	CHECK_EQ(nextgi_schedule_relight(texels, 5, cursor, NEXTGI_SRC_RELIGHT_TEXEL_CAP, indices, 5), 2);
	CHECK_EQ(indices[0], 0);
	CHECK_EQ(indices[1], 1);
	CHECK_EQ(cursor, 2);
	CHECK_EQ(nextgi_schedule_relight(texels, 5, cursor, NEXTGI_SRC_RELIGHT_TEXEL_CAP, indices, 5), 2);
	CHECK_EQ(indices[0], 2);
	CHECK_EQ(cursor, 4);
	CHECK_EQ(nextgi_schedule_relight(texels, 5, cursor, NEXTGI_SRC_RELIGHT_TEXEL_CAP, indices, 5), 2);
	CHECK_EQ(indices[0], 4);
	CHECK_EQ(indices[1], 0);
	CHECK_EQ(cursor, 1);

	CHECK(nextgi_omni_attenuation(0.0f, 0.1f, 1.0f) > 1.0f);
	CHECK_EQ(nextgi_omni_attenuation(10.0f, 0.1f, 1.0f), 0.0f);
}

TEST_CASE("[NextGI] velocity plan follows the upscaler table") {
	const NextGIVelocityMode modes[4] = { NEXTGI_VELOCITY_NONE, NEXTGI_VELOCITY_TAA, NEXTGI_VELOCITY_SENTINEL, NEXTGI_VELOCITY_DENSE };
	const NextGIVelocityScale scales[4] = { NEXTGI_VELOCITY_SCALE_NONE, NEXTGI_VELOCITY_SCALE_NONE, NEXTGI_VELOCITY_SCALE_FSR2, NEXTGI_VELOCITY_SCALE_MFX };
	for (int i = 0; i < 4; i++) {
		const NextGIVelocityPlan off = nextgi_velocity_plan(scales[i], modes[i], modes[i] == NEXTGI_VELOCITY_TAA, false);
		CHECK_FALSE(off.active);
		CHECK_FALSE(off.prepass_writes);
		CHECK_FALSE(off.color_pass_loads);
		CHECK_FALSE(off.skip_clear_pass);
		CHECK_FALSE(off.mfx_preserve);
	}

	const NextGIVelocityPlan none = nextgi_velocity_plan(NEXTGI_VELOCITY_SCALE_NONE, NEXTGI_VELOCITY_NONE, false, true);
	CHECK(none.active);
	CHECK(none.prepass_writes);
	CHECK_FALSE(none.color_pass_loads);
	CHECK_FALSE(none.skip_clear_pass);
	CHECK_FALSE(none.mfx_preserve);
	CHECK_EQ(none.pre_clear_r, 0.0f);

	const NextGIVelocityPlan taa = nextgi_velocity_plan(NEXTGI_VELOCITY_SCALE_NONE, NEXTGI_VELOCITY_TAA, true, true);
	CHECK(taa.color_pass_loads);
	CHECK(taa.prepass_writes);
	CHECK_FALSE(taa.skip_clear_pass);
	CHECK_EQ(taa.pre_clear_r, 0.0f);

	const NextGIVelocityPlan fsr2 = nextgi_velocity_plan(NEXTGI_VELOCITY_SCALE_FSR2, NEXTGI_VELOCITY_SENTINEL, true, true);
	CHECK(fsr2.skip_clear_pass);
	CHECK_FALSE(fsr2.color_pass_loads);
	CHECK_FALSE(fsr2.mfx_preserve);
	CHECK_EQ(fsr2.pre_clear_r, -1.0f);
	CHECK_EQ(fsr2.pre_clear_g, -1.0f);

	const NextGIVelocityPlan mfx = nextgi_velocity_plan(NEXTGI_VELOCITY_SCALE_MFX, NEXTGI_VELOCITY_DENSE, false, true);
	CHECK(mfx.mfx_preserve);
	CHECK_FALSE(mfx.skip_clear_pass);
	CHECK_FALSE(mfx.color_pass_loads);
	CHECK_EQ(mfx.pre_clear_r, -1.0f);
	CHECK_EQ(mfx.pre_clear_g, -1.0f);

	CHECK(nextgi_velocity_texel_is_preclear(-1.0f, -1.0f));
	CHECK_FALSE(nextgi_velocity_texel_is_preclear(-1.0001f, -1.2f));
	CHECK_FALSE(nextgi_velocity_texel_is_preclear(-1.0f, -0.999f));
}

TEST_CASE("[NextGI] diffuse gather ray count is 1/16 of a 1080p frame") {
	using namespace RendererRD;
	CHECK_EQ(nextgi_gather_probe_count_axis(1920), 120);
	CHECK_EQ(nextgi_gather_probe_count_axis(1080), 68);
	CHECK_EQ(nextgi_gather_ray_count(1920, 1080), 130560);
	CHECK_EQ(nextgi_gather_ray_count(16, 16), 16);
	CHECK(nextgi_gather_history_resets(1.0f, 0.0f));
	CHECK_FALSE(nextgi_gather_history_resets(1.0f, 1.05f));
	CHECK(nextgi_gather_history_resets(1.0f, 1.2f));
	CHECK_EQ(nextgi_rt_origin_offset(0.02f, 1.0f), 0.05f);
	CHECK(ProjectSettings::get_singleton()->has_setting("rendering/global_illumination/nextgi/debug/coverage_counter"));
}

TEST_CASE("[NextGI] SRC cards: the material atlas dump setting is registered and off") {
	const String setting = "rendering/global_illumination/nextgi/debug/dump_material_atlas";
	REQUIRE(ProjectSettings::get_singleton()->has_setting(setting));
	CHECK_FALSE(bool(ProjectSettings::get_singleton()->property_get_revert(setting)));
	CHECK_EQ(RendererRD::NextGISRC::get_atlas_target_name(RendererRD::NextGISRC::ATLAS_DEPTH), "depth");
	CHECK(ProjectSettings::get_singleton()->has_setting("rendering/global_illumination/nextgi/debug/read_page_radiance"));
	CHECK(ProjectSettings::get_singleton()->has_setting("rendering/global_illumination/nextgi/debug/read_shadow_mask"));
	CHECK_FALSE(bool(ProjectSettings::get_singleton()->property_get_revert("rendering/global_illumination/nextgi/debug/read_page_radiance")));
	CHECK_FALSE(bool(ProjectSettings::get_singleton()->property_get_revert("rendering/global_illumination/nextgi/debug/read_shadow_mask")));
}

TEST_CASE("[NextGI] RT registry: cull, cap, eligibility and refit") {
	using namespace RendererRD;
	CHECK(nextgi_rt_instance_eligible(true, true, true, false));
	CHECK_FALSE(nextgi_rt_instance_eligible(true, true, true, true));
	CHECK_FALSE(nextgi_rt_instance_eligible(true, false, true, false));
	CHECK_FALSE(nextgi_rt_instance_eligible(false, true, true, false));
	CHECK_FALSE(nextgi_rt_instance_eligible(true, true, false, false));

	// 10 m, inside the radius and inside the 30 m angular cull: included, frustum or not.
	CHECK_EQ(nextgi_rt_cull_instance(10.0f, 1.0f, 200.0f), NEXTGI_RT_INCLUDED);
	CHECK_EQ(nextgi_rt_cull_instance(250.0f, 10.0f, 200.0f), NEXTGI_RT_NOT_IN_SCENE);
	// 100 m and a 1 cm radius is under half a degree.
	CHECK_EQ(nextgi_rt_cull_instance(100.0f, 0.01f, 200.0f), NEXTGI_RT_NOT_IN_SCENE);
	CHECK(nextgi_rt_angular_radius(0.0f, 1.0f) > 1.0f);

	float angular[3] = { 0.1f, 0.5f, 0.2f };
	NextGIRTExclusion reasons[3] = { NEXTGI_RT_INCLUDED, NEXTGI_RT_INCLUDED, NEXTGI_RT_INCLUDED };
	nextgi_rt_apply_instance_cap(angular, reasons, 3, 2);
	CHECK_EQ(reasons[0], NEXTGI_RT_INSTANCE_CAP);
	CHECK_EQ(reasons[1], NEXTGI_RT_INCLUDED);
	CHECK_EQ(reasons[2], NEXTGI_RT_INCLUDED);

	const NextGIDirtyKind kinds[4] = { NEXTGI_DIRTY_MOVED, NEXTGI_DIRTY_FREED, NEXTGI_DIRTY_SCENARIO, NEXTGI_DIRTY_HIDDEN };
	for (NextGIDirtyKind kind : kinds) {
		const NextGIDirtyRegion region = nextgi_rt_dirty_region(kind);
		CHECK(region.has_old);
		CHECK_EQ(region.has_new, kind == NEXTGI_DIRTY_MOVED);
	}

	const uint32_t frames[4] = { 8, 7, 6, 5 };
	CHECK(nextgi_rt_surface_is_dynamic(8, frames));
	const uint32_t gap[4] = { 8, 7, 5, 4 };
	CHECK_FALSE(nextgi_rt_surface_is_dynamic(8, gap));
	CHECK_EQ(nextgi_rt_refit_or_exclude(150000, 60000, NEXTGI_RT_REFIT_TRIANGLE_CAP), NEXTGI_RT_DYNAMIC_EXCLUDED);
	CHECK_EQ(nextgi_rt_refit_or_exclude(100000, 50000, NEXTGI_RT_REFIT_TRIANGLE_CAP), NEXTGI_RT_INCLUDED);

	NextGIBuildItem builds[3];
	builds[0].id = 0;
	builds[0].triangles = 40000;
	builds[0].is_new = false;
	builds[0].angular_radius = 0.9f;
	builds[1].id = 1;
	builds[1].triangles = 100000;
	builds[1].is_new = true;
	builds[1].angular_radius = 0.1f;
	builds[2].id = 2;
	builds[2].triangles = 100000;
	builds[2].is_new = false;
	builds[2].angular_radius = 0.2f;
	uint32_t accepted[3] = {};
	uint32_t triangles = 0;
	CHECK_EQ(nextgi_rt_schedule_builds(builds, 3, 150000, accepted, 3, triangles), 2u);
	CHECK_EQ(accepted[0], 1u);
	CHECK_EQ(accepted[1], 0u);
	CHECK_EQ(triangles, 140000u);

	CHECK(nextgi_rt_chunk_needs_defrag(80, 256));
	CHECK_FALSE(nextgi_rt_chunk_needs_defrag(10, 256));
}

TEST_CASE("[NextGI] RT registry: light events, one delivery across two views, arena churn and AS memory") {
	using namespace RendererRD;
	for (int change = NEXTGI_LIGHT_ADD; change < NEXTGI_LIGHT_CHANGE_MAX; change++) {
		CHECK(nextgi_rt_light_event_emits(NEXTGI_LIGHT_DYNAMIC));
		CHECK(nextgi_rt_light_event_emits(NEXTGI_LIGHT_STATIC));
		CHECK_FALSE(nextgi_rt_light_event_emits(NEXTGI_LIGHT_DISABLED));
		CHECK_EQ(change >= NEXTGI_LIGHT_ADD, true);
	}

	NextGIRTEventBuffer<int> events;
	events.push(1);
	events.push(2);
	events.push(3);
	events.swap(1);
	CHECK_EQ(events.read().size(), events.read().size());
	CHECK_EQ(events.read().size(), 3);
	CHECK_EQ(events.read()[0], 1);
	CHECK_EQ(events.read()[2], 3);
	events.swap(2);
	CHECK_EQ(events.read().size(), 0);

	NextGIArenaAllocator allocator(1 << 20);
	for (int i = 0; i < 10000; i++) {
		const NextGIArenaAllocator::Allocation allocation = allocator.allocate(64);
		CHECK(allocation.is_valid());
		allocator.free_range(allocation);
	}
	const NextGIArenaAllocator::Allocation again = allocator.allocate(64);
	CHECK_EQ(again.chunk, 0u);
	CHECK_EQ(again.offset, 0u);
	CHECK_EQ(allocator.get_chunk_count(), 1u);

	Vector<float> positions;
	positions.resize(3);
	positions.write[0] = 1.0f;
	positions.write[1] = 2.0f;
	positions.write[2] = 3.0f;
	const float replacement[3] = { 4.0f, 5.0f, 6.0f };
	nextgi_rt_patch_positions(positions, 0, reinterpret_cast<const uint8_t *>(replacement), sizeof(replacement));
	CHECK_EQ(positions[0], 4.0f);
	CHECK_EQ(positions[2], 6.0f);

	const uint64_t mebibyte = 1024ull * 1024ull;
	NextGIASMesh meshes[3];
	meshes[0].id = 7;
	meshes[0].bytes = 40 * mebibyte;
	meshes[0].frames_absent = 200;
	meshes[1].id = 8;
	meshes[1].bytes = 40 * mebibyte;
	meshes[1].frames_absent = 0;
	meshes[2].id = 9;
	meshes[2].bytes = 40 * mebibyte;
	meshes[2].frames_absent = 0;
	uint32_t evict[3] = {};
	uint32_t evict_count = 0;
	const bool over = nextgi_rt_evict_as_memory(meshes, 3, 64 * mebibyte, NEXTGI_RT_ABSENT_FRAMES, evict, evict_count);
	CHECK(over);
	CHECK_EQ(evict_count, 1u);
	CHECK_EQ(evict[0], 7u);

	NextGI next_gi;
	String line;
	NextGI::TierInputs as_memory = hwrt_inputs();
	as_memory.as_memory_ok = false;
	REQUIRE(next_gi.update_tier(as_memory, NextGI::QUALITY_AUTO, line));
	CHECK(line.contains("reason=AS_MEMORY"));
	CHECK_EQ(line.count("NextGI:"), 1);
	line = String();
	CHECK_FALSE(next_gi.update_tier(as_memory, NextGI::QUALITY_AUTO, line));
	CHECK(line.is_empty());
}

TEST_CASE("[NextGI] reference path tracer furnace equilibrium and HWRT shader gate") {
	CHECK(Math::is_equal_approx(RendererRD::nextgi_reference_furnace_equilibrium(1.0f, 0.8f), 5.0f));
	CHECK(RendererRD::nextgi_reference_shader_allowed(true));
	CHECK_FALSE(RendererRD::nextgi_reference_shader_allowed(false));
	CHECK(ProjectSettings::get_singleton()->has_setting("rendering/global_illumination/nextgi/debug/reference_mode"));
	CHECK(ProjectSettings::get_singleton()->has_setting("rendering/global_illumination/nextgi/debug/reference_seed"));
	CHECK(ProjectSettings::get_singleton()->has_setting("rendering/global_illumination/nextgi/debug/gather_hit_radiance"));
	CHECK_EQ(int(ProjectSettings::get_singleton()->get_setting("rendering/global_illumination/nextgi/debug/reference_mode")), 0);
}

TEST_CASE("[NextGI] SRC production: atlas budget, material sharing, capture cap, buddy and LRU") {
	const RendererRD::NextGIAtlasBudget high = RendererRD::nextgi_src_atlas_budget(false);
	const RendererRD::NextGIAtlasBudget epic = RendererRD::nextgi_src_atlas_budget(true);
	CHECK_EQ(high.radiance_bytes, uint64_t(64 * 1024 * 1024));
	CHECK_EQ(high.material_bytes, uint64_t(128 * 1024 * 1024));
	CHECK_EQ(epic.radiance_bytes, uint64_t(128 * 1024 * 1024));
	CHECK_EQ(epic.material_bytes, uint64_t(256 * 1024 * 1024));
	CHECK_EQ(epic.radiance_width, 4096u);
	CHECK_EQ(epic.radiance_height, 8192u);

	const uint64_t shared = RendererRD::nextgi_src_material_key(7, 0, nullptr, 0);
	RendererRD::NextGIMaterialCaptures captures;
	bool created = false;
	for (int i = 0; i < 100; i++) {
		captures.capture(shared, created);
		CHECK_EQ(created, i == 0);
	}
	CHECK_EQ(captures.get_count(), 1u);
	const uint64_t surfaces[1] = { 3 };
	const uint64_t overridden = RendererRD::nextgi_src_material_key(7, 9, surfaces, 1);
	captures.capture(overridden, created);
	CHECK(created);
	CHECK_EQ(captures.get_count(), 2u);

	CHECK_EQ(RendererRD::nextgi_src_page_level(Math::PI, 128), 0u);
	CHECK(RendererRD::nextgi_src_page_level(0.0f, 128) <= 3u);

	RendererRD::NextGICaptureCandidate cards[3];
	cards[0] = { 0, 0.1f, 100, true };
	cards[1] = { 1, 0.9f, 100, true };
	cards[2] = { 2, 1.0f, 100, false };
	uint32_t accepted[3] = {};
	CHECK_EQ(RendererRD::nextgi_src_select_captures(cards, 3, accepted, 2, 1000000), 2u);
	CHECK_EQ(accepted[0], 1u);
	CHECK_EQ(accepted[1], 0u);

	RendererRD::NextGIBuddyAllocator buddy(64, 16);
	Rect2i a, b, c, d;
	REQUIRE(buddy.allocate(16, 16, a));
	REQUIRE(buddy.allocate(16, 16, b));
	REQUIRE(buddy.allocate(16, 16, c));
	REQUIRE(buddy.allocate(16, 16, d));
	CHECK(a != b);
	CHECK(buddy.free_block(a));
	CHECK(buddy.free_block(b));
	CHECK(buddy.free_block(c));
	CHECK(buddy.free_block(d));
	Rect2i merged;
	REQUIRE(buddy.allocate(64, 64, merged));
	CHECK_EQ(merged, Rect2i(0, 0, 64, 64));
	CHECK(buddy.free_block(merged));

	RendererRD::NextGIBuddyAllocator stress(256, 16);
	LocalVector<Rect2i> live;
	uint32_t rng = 1;
	for (int step = 0; step < 10000; step++) {
		rng = rng * 1664525u + 1013904223u;
		if (!live.is_empty() && (rng & 3u) == 0u) {
			const uint32_t index = (rng >> 8) % live.size();
			REQUIRE(stress.free_block(live[index]));
			live.remove_at(index);
		} else {
			const uint32_t side = 16u << ((rng >> 16) & 3u);
			Rect2i rect;
			if (stress.allocate(side, side, rect)) {
				live.push_back(rect);
			}
		}
		for (uint32_t i = 0; i < live.size(); i++) {
			for (uint32_t j = i + 1; j < live.size(); j++) {
				CHECK_FALSE(live[i].intersects(live[j]));
			}
		}
	}

	RendererRD::NextGIPageUse pages[3] = { { 4, 30 }, { 5, 10 }, { 6, 20 } };
	uint32_t evict[3] = {};
	CHECK_EQ(RendererRD::nextgi_src_lru_evict(pages, 3, 2, evict), 2u);
	CHECK_EQ(evict[0], 5u);
	CHECK_EQ(evict[1], 6u);
}

} // namespace TestNextGI

#endif // RD_ENABLED
