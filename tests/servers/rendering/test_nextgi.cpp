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

#include "servers/rendering/dummy/rasterizer_scene_dummy.h"

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

} // namespace TestNextGI

#ifdef RD_ENABLED

#include "core/templates/hash_set.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/rendering_device.h"

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

} // namespace TestNextGI

#endif // RD_ENABLED
