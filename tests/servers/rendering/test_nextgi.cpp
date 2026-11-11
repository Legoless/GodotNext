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

#ifdef RD_ENABLED
#include "modules/modules_enabled.gen.h"
// GodotNext: shader_baker_ray_query
#include "editor/export/shader_baker/shader_baker_export_plugin.h"
#ifdef METAL_ENABLED
#include "drivers/metal/rendering_shader_container_metal.h"
#endif // METAL_ENABLED
#ifdef MODULE_GLSLANG_ENABLED
#include "modules/glslang/shader_compile.h"
#endif // MODULE_GLSLANG_ENABLED
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_gather.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_reference.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_rq_selftest.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_selftest.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_src_relight.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_wrc_update.glsl.gen.h"
// GodotNext: denoise_reset_regions
#include "servers/rendering/renderer_rd/effects/denoise.h"
#endif // RD_ENABLED

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
	CHECK(text.contains("software tier"));
	CHECK(text.contains("Direct3D 12 fallback"));
	CHECK(text.contains("HDR output"));
	CHECK(text.contains("DComp transparency"));
	CHECK(text.contains("restart"));
}

TEST_CASE("[NextGI][Editor] View Information tier readout") {
	Dictionary swrt;
	swrt["tier"] = "SWRT";
	swrt["reason"] = "NO_RAY_QUERY";
	CHECK_EQ(NextGIEditorPlugin::format_tier_readout(swrt), "NextGI: SWRT (NO_RAY_QUERY)");

	Dictionary hwrt;
	hwrt["tier"] = "HWRT";
	hwrt["reason"] = "NONE";
	CHECK_EQ(NextGIEditorPlugin::format_tier_readout(hwrt), "NextGI: HWRT");

	Dictionary disabled;
	disabled["tier"] = "DISABLED";
	CHECK_EQ(NextGIEditorPlugin::format_tier_readout(disabled), "NextGI: SDFGI-legacy");

	Dictionary lite;
	lite["tier"] = "SWRT";
	lite["reason"] = "SETTING_OFF";
	lite["swrt_mode"] = "Global";
	lite["lite_beta"] = true;
	lite["swrt_reduced"] = true;
	lite["experimental_toggle"] = true;
	CHECK_EQ(NextGIEditorPlugin::format_tier_readout(lite), "NextGI: SWRT (SETTING_OFF) mode=Global SWRT_REDUCED Lite Beta toggle=EXPERIMENTAL");
	CHECK_FALSE(NextGIEditorPlugin::format_status_panel(lite).contains("SDFGI-legacy"));
	CHECK(NextGIEditorPlugin::format_status_panel(lite).contains("SWRT"));
	CHECK(NextGIEditorPlugin::format_status_panel(lite).contains("mode=Global"));
	CHECK(NextGIEditorPlugin::format_status_panel(lite).contains("Lite Beta"));
	CHECK(NextGIEditorPlugin::format_status_panel(lite).contains("toggle=EXPERIMENTAL"));
	// The label names the switch, so a live HWRT readout never reads as "HWRT EXPERIMENTAL".
	Dictionary hwrt_toggle;
	hwrt_toggle["tier"] = "HWRT";
	hwrt_toggle["reason"] = "NONE";
	hwrt_toggle["experimental_toggle"] = true;
	CHECK_EQ(NextGIEditorPlugin::format_tier_readout(hwrt_toggle), "NextGI: HWRT toggle=EXPERIMENTAL");
	Dictionary legacy;
	legacy["tier"] = "DISABLED";
	legacy["reason"] = "NONE";
	legacy["quality"] = "NONE";
	legacy["quality_reason"] = "NONE";
	legacy["driver"] = "metal";
	CHECK(NextGIEditorPlugin::format_status_panel(legacy).begins_with("NextGI SDFGI-legacy"));

	// The readout is off while nextgi/enabled is false (the default).
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String enabled_setting = "rendering/global_illumination/nextgi/enabled";
	const bool had_enabled = ps->has_setting(enabled_setting);
	const Variant previous_enabled = ps->get_setting(enabled_setting);
	ps->set_setting(enabled_setting, false);
	CHECK(NextGIEditorPlugin::get_tier_readout().is_empty());
	restore_setting(enabled_setting, had_enabled, previous_enabled);
}

TEST_CASE("[NextGI][Editor] Upscaler status readout") {
	Dictionary fallback;
	fallback["requested_mode"] = RSE::VIEWPORT_SCALING_3D_MODE_DLSS;
	fallback["active_mode"] = RSE::VIEWPORT_SCALING_3D_MODE_FSR2;
	fallback["reason"] = RSE::VIEWPORT_SCALING_3D_REASON_API_UNSUPPORTED;
	fallback["effective_scale"] = 0.5f;
	fallback["preset"] = 0;
	CHECK_EQ(NextGIEditorPlugin::format_upscaler_readout(fallback), "Upscaler: DLSS → FSR2 (0, API_UNSUPPORTED)");
	fallback["active_mode"] = RSE::VIEWPORT_SCALING_3D_MODE_DLSS;
	fallback["reason"] = RSE::VIEWPORT_SCALING_3D_REASON_NONE;
	fallback["effective_scale"] = 1.0f;
	CHECK_EQ(NextGIEditorPlugin::format_upscaler_readout(fallback), "Upscaler: DLAA → DLAA (0, NONE)");
}

#ifdef RD_ENABLED
// GodotNext: shader_baker_ray_query
TEST_CASE("[NextGI][Editor] Shader baker keeps ray-query versions only for AS containers") {
	CHECK(ShaderBakerExportPlugin::should_bake_version(false, false));
	CHECK(ShaderBakerExportPlugin::should_bake_version(false, true));
	CHECK_FALSE(ShaderBakerExportPlugin::should_bake_version(true, false));
	CHECK(ShaderBakerExportPlugin::should_bake_version(true, true));
}

TEST_CASE("[NextGI][Editor] Every ray-query ShaderRD is marked for the shader baker") {
	CHECK(NextgiGatherShaderRD().requires_ray_query());
	CHECK(NextgiSrcRelightShaderRD().requires_ray_query());
	CHECK(NextgiWrcUpdateShaderRD().requires_ray_query());
	CHECK(NextgiReferenceShaderRD().requires_ray_query());
	CHECK(NextgiSelftestShaderRD().requires_ray_query());
	CHECK(NextgiRqSelftestShaderRD().requires_ray_query());
}

#if defined(METAL_ENABLED) && defined(MODULE_GLSLANG_ENABLED)
static bool metal_container_compiles(ShaderRD &p_shader) {
	p_shader.initialize({ "" });
	const RID version = p_shader.version_create(false);
	if (!version.is_valid()) {
		return false;
	}
	const Vector<String> sources = p_shader.version_build_variant_stage_sources(version, 0);
	p_shader.version_free(version);
	if (sources.size() <= RenderingDeviceCommons::SHADER_STAGE_COMPUTE) {
		return false;
	}
	String error;
	const Vector<uint8_t> spirv = compile_glslang_shader(
			RenderingDeviceCommons::SHADER_STAGE_COMPUTE,
			sources[RenderingDeviceCommons::SHADER_STAGE_COMPUTE],
			RenderingDeviceCommons::SHADER_LANGUAGE_VULKAN_VERSION_1_1,
			RenderingDeviceCommons::SHADER_SPIRV_VERSION_1_6,
			&error);
	if (spirv.is_empty()) {
		print_line("metal container: glslang failed: " + error);
		return false;
	}
	RenderingDeviceCommons::ShaderStageSPIRVData stage;
	stage.shader_stage = RenderingDeviceCommons::SHADER_STAGE_COMPUTE;
	stage.spirv = spirv;
	Vector<RenderingDeviceCommons::ShaderStageSPIRVData> stages;
	stages.push_back(stage);

	const MetalDeviceProfile *profile = MetalDeviceProfile::get_profile(MetalDeviceProfile::Platform::macOS, MetalDeviceProfile::GPU::Apple7, os_version::MACOS_13_0);
	if (profile == nullptr) {
		return false;
	}
	RenderingShaderContainerFormatMetal format(profile, true);
	Ref<RenderingShaderContainer> container = format.create_container();
	return container.is_valid() && container->set_code_from_spirv(p_shader.get_name(), stages);
}

TEST_CASE("[NextGI][Metal] The Metal shader container compiles the ray-query ShaderRDs") {
	{
		NextgiGatherShaderRD shader;
		CHECK(metal_container_compiles(shader));
	}
	{
		NextgiSrcRelightShaderRD shader;
		CHECK(metal_container_compiles(shader));
	}
	{
		NextgiWrcUpdateShaderRD shader;
		CHECK(metal_container_compiles(shader));
	}
	{
		NextgiReferenceShaderRD shader;
		CHECK(metal_container_compiles(shader));
	}
}
#endif // METAL_ENABLED && MODULE_GLSLANG_ENABLED
#endif // RD_ENABLED

TEST_CASE("[NextGI][Denoise] RR hook override parses the documented keys") {
	const RendererRD::NextGIDenoise::RRHooks defaults = RendererRD::NextGIDenoise::parse_rr_hooks("");
	CHECK(defaults.stabilizer_enabled);
	CHECK_EQ(defaults.probe_temporal_max_history, 16u);

	const RendererRD::NextGIDenoise::RRHooks both = RendererRD::NextGIDenoise::parse_rr_hooks("stabilizer=0,probe_history=4");
	CHECK_FALSE(both.stabilizer_enabled);
	CHECK_EQ(both.probe_temporal_max_history, 4u);

	// 0 is the pass-through cap the temporal pass reads.
	CHECK_EQ(RendererRD::NextGIDenoise::parse_rr_hooks("probe_history=0").probe_temporal_max_history, 0u);

	// Unknown keys, missing values and junk fall back to the defaults; later entries win.
	const RendererRD::NextGIDenoise::RRHooks junk = RendererRD::NextGIDenoise::parse_rr_hooks("stabilizer=maybe,probe_history=,junk=3,,probe_history=abc");
	CHECK(junk.stabilizer_enabled);
	CHECK_EQ(junk.probe_temporal_max_history, 16u);
	CHECK_FALSE(RendererRD::NextGIDenoise::parse_rr_hooks("stabilizer=1,stabilizer=0").stabilizer_enabled);
	CHECK(RendererRD::NextGIDenoise::parse_rr_hooks(" stabilizer = 0 ").stabilizer_enabled == false);
	// Out-of-range caps clamp instead of wrapping.
	CHECK_EQ(RendererRD::NextGIDenoise::parse_rr_hooks("probe_history=99999").probe_temporal_max_history, 4096u);
}

} // namespace TestNextGI

#endif // TOOLS_ENABLED

// Renderer-independent cases: renderers without NextGI keep SDFGI, and the dummy tier info.

#include "scene/3d/world_environment.h"
#include "scene/resources/environment.h"
#include "servers/rendering/dummy/rasterizer_scene_dummy.h"
#include "servers/rendering/rendering_server.h"

namespace TestNextGI {

// GodotNext: ps_tier_rule (#187). A renderer without NextGI never replaces SDFGI, so an
// Environment with nextgi_enabled keeps its own SDFGI setting (§21.2 L1: NextGI is fully off).
TEST_CASE("[NextGI] Without NextGI, an Environment with nextgi_enabled keeps its own SDFGI") {
	RasterizerSceneDummy scene_render;
	const RID env = scene_render.environment_allocate();
	scene_render.environment_initialize(env);

	CHECK_FALSE(scene_render.environment_get_sdfgi_enabled(env));
	scene_render.environment_set_nextgi_enabled(env, true);
	CHECK_FALSE(scene_render.environment_get_sdfgi_enabled(env));

	ERR_PRINT_OFF; // SDFGI warns once outside Forward+.
	scene_render.environment_set_sdfgi(env, true, 4, 0.2, RSE::ENV_SDFGI_Y_SCALE_75_PERCENT, false, 0.5, true, 1.0, 1.1, 1.1);
	ERR_PRINT_ON;
	CHECK(scene_render.environment_get_nextgi_enabled(env));
	CHECK(scene_render.environment_get_sdfgi_enabled(env));
	CHECK_EQ(scene_render.environment_get_sdfgi_cascades(env), 4);

	scene_render.environment_set_nextgi_enabled(env, false);
	CHECK(scene_render.environment_get_sdfgi_enabled(env));
	CHECK_EQ(scene_render.environment_get_sdfgi_cascades(env), 4);

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

	CHECK(Math::is_equal_approx(rs->environment_get_nextgi_max_roughness_to_trace(env), 0.4f));

	rs->environment_set_nextgi(env, true, 0.35f, false, 0.25f, false);
	CHECK(rs->environment_get_nextgi_enabled(env));
	CHECK(Math::is_equal_approx(rs->environment_get_nextgi_energy(env), 0.35f));
	CHECK_FALSE(rs->environment_get_nextgi_read_sky_light(env));
	CHECK(Math::is_equal_approx(rs->environment_get_nextgi_max_roughness_to_trace(env), 0.25f));
	CHECK_FALSE(rs->environment_get_nextgi_fog_on_reflection_hits(env));

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

TEST_CASE("[NextGI] Dummy status agrees with the tier info, and counters stay zero") {
	RenderingServer *rs = RenderingServer::get_singleton();
	const Dictionary status = rs->nextgi_get_status();
	const Dictionary tier_info = rs->nextgi_get_tier_info();
	CHECK_EQ(String(status["tier"]), String(tier_info["tier"]));
	CHECK_EQ(String(status["reason"]), String(tier_info["reason"]));
	CHECK_EQ(String(status["tier"]), "UNSUPPORTED_RENDERER");
	CHECK_EQ(String(status["reason"]), "NONE");
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
		"rt_coverage_numerator",
		"rt_coverage_denominator",
		"rt_not_in_scene",
		"rt_instance_cap",
		"rt_dynamic_excluded",
		"rt_blas_pending",
		"blas_bytes_compacted",
		"blas_bytes_uncompacted",
		"rt_arena_bytes",
		"skinned_refit_triangles",
		"rays_per_pass",
		"src_capture_texels",
		"src_relight_texels",
		"src_shadow_rays",
		"src_dirty_pages",
		"src_max_relight_age_frames",
		"dirty_aabb",
		"src_page_updates",
		"clamped_energy_fraction",
		"proxy_blas_bytes_compacted",
		"proxy_blas_bytes_uncompacted",
		"proxy_triangles",
		"proxy_blas_parts",
		"src_stage2_cards_captured",
		"voxel_lighting_retrace",
		"voxel_lighting_coverage_ppm",
		"voxel_lighting_volume_bytes",
		"voxel_lighting_stored_hits",
		"voxel_lighting_resampled_nonzero",
		"swrt_trace_steps",
		"swrt_trace_hits",
		"swrt_trace_misses",
		"swrt_shadow_rays",
		"swrt_mode",
	};
	CHECK_EQ(counters.size(), 36);
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
		if (warning.contains("UNSUPPORTED_RENDERER") && warning.contains("NONE")) {
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
#include "core/math/mesh_sdf_builder.h"
#include "core/math/random_number_generator.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/mesh.h"
#include "servers/rendering/renderer_rd/environment/next_gi_reference.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_scene.h"
#include "servers/rendering/renderer_rd/environment/next_gi_sdf.h"
// GodotNext: nextgi_as_slots
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
// GodotNext: nextgi_src_cards
#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/environment/next_gi_gather.h"
#include "servers/rendering/renderer_rd/environment/next_gi_src.h"
#include "servers/rendering/renderer_rd/environment/next_gi_src_relight.h"
#include "servers/rendering/renderer_rd/environment/next_gi_wrc.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgi_velocity.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"
// GodotNext: ps_lumen_lite (#183)
#include "servers/rendering/renderer_rd/environment/next_gi_irradiance_field.h"
#include "servers/rendering/renderer_rd/environment/next_gi_relight.h"

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
	check_tier(gtx, NextGI::TIER_SOFTWARE, NextGI::NOT_HW_ACCELERATED);
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
	check_tier(radv, NextGI::TIER_SOFTWARE, NextGI::NOT_HW_ACCELERATED);
}

TEST_CASE("[NextGI] Tier rule: lavapipe gives CPU_DEVICE, or NO_RAY_QUERY without ray query") {
	NextGI::TierInputs lavapipe = hwrt_inputs();
	lavapipe.cpu_device = true;
	lavapipe.ray_query_hw = false;
	check_tier(lavapipe, NextGI::TIER_SOFTWARE, NextGI::CPU_DEVICE);

	// The CPU check comes before the hardware flag, so a CPU device never reads as hardware.
	lavapipe.ray_query_hw = true;
	check_tier(lavapipe, NextGI::TIER_SOFTWARE, NextGI::CPU_DEVICE);

	NextGI::TierInputs lavapipe_no_rq = no_ray_query_inputs(NextGI::NO_RAY_QUERY);
	lavapipe_no_rq.cpu_device = true;
	check_tier(lavapipe_no_rq, NextGI::TIER_SOFTWARE, NextGI::NO_RAY_QUERY);
}

TEST_CASE("[NextGI] Tier rule: Apple7 and Apple8 give NOT_HW_ACCELERATED") {
	// The Metal ray-tracing API exists on M1 and M2, but there is no RT hardware (#48 feeds this).
	NextGI::TierInputs apple = hwrt_inputs();
	apple.ray_query_hw = false;
	check_tier(apple, NextGI::TIER_SOFTWARE, NextGI::NOT_HW_ACCELERATED);
}

TEST_CASE("[NextGI] Tier rule: D3D12 without DXR gives D3D12_NO_DXR") {
	check_tier(no_ray_query_inputs(NextGI::D3D12_NO_DXR), NextGI::TIER_SOFTWARE, NextGI::D3D12_NO_DXR);
}

TEST_CASE("[NextGI] Tier rule: D3D12 under Force stays D3D12_NO_DXR, with the Force warning") {
	NextGI::TierInputs d3d12 = no_ray_query_inputs(NextGI::D3D12_NO_DXR);
	d3d12.hwrt_setting = NextGI::HARDWARE_RAY_TRACING_FORCE;
	check_tier(d3d12, NextGI::TIER_SOFTWARE, NextGI::D3D12_NO_DXR, true);
}

TEST_CASE("[NextGI] Tier rule: Intel UHD gives NO_RAY_QUERY") {
	check_tier(no_ray_query_inputs(NextGI::NO_RAY_QUERY), NextGI::TIER_SOFTWARE, NextGI::NO_RAY_QUERY);
}

TEST_CASE("[NextGI] Tier rule: Metal without Barriers sync mode gives METAL_SYNC_MODE") {
	check_tier(no_ray_query_inputs(NextGI::METAL_SYNC_MODE), NextGI::TIER_SOFTWARE, NextGI::METAL_SYNC_MODE);
}

TEST_CASE("[NextGI] Tier rule: Metal without argument buffers gives METAL_NO_ARGUMENT_BUFFERS") {
	check_tier(no_ray_query_inputs(NextGI::METAL_NO_ARGUMENT_BUFFERS), NextGI::TIER_SOFTWARE, NextGI::METAL_NO_ARGUMENT_BUFFERS);
}

TEST_CASE("[NextGI] Tier rule: a driver without a reason code gives NO_RAY_QUERY") {
	check_tier(no_ray_query_inputs(NextGI::TIER_REASON_NONE), NextGI::TIER_SOFTWARE, NextGI::NO_RAY_QUERY);
	// Only driver reason codes pass through.
	check_tier(no_ray_query_inputs(NextGI::AS_MEMORY), NextGI::TIER_SOFTWARE, NextGI::NO_RAY_QUERY);
}

TEST_CASE("[NextGI] Tier rule: hardware_ray_tracing Off gives SETTING_OFF") {
	NextGI::TierInputs off = hwrt_inputs();
	off.hwrt_setting = NextGI::HARDWARE_RAY_TRACING_OFF;
	check_tier(off, NextGI::TIER_SOFTWARE, NextGI::SETTING_OFF);

	// Off is checked before the device.
	NextGI::TierInputs off_no_rq = no_ray_query_inputs(NextGI::D3D12_NO_DXR);
	off_no_rq.hwrt_setting = NextGI::HARDWARE_RAY_TRACING_OFF;
	check_tier(off_no_rq, NextGI::TIER_SOFTWARE, NextGI::SETTING_OFF);
}

TEST_CASE("[NextGI] Tier rule: a failed self-test gives SELFTEST_FAILED, also under Force") {
	NextGI::TierInputs failed = hwrt_inputs();
	failed.selftest_passed = false;
	check_tier(failed, NextGI::TIER_SOFTWARE, NextGI::SELFTEST_FAILED);
	failed.hwrt_setting = NextGI::HARDWARE_RAY_TRACING_FORCE;
	check_tier(failed, NextGI::TIER_SOFTWARE, NextGI::SELFTEST_FAILED);
}

TEST_CASE("[NextGI] Tier rule: a latched HWRT fault gives HWRT_FAULTED") {
	NextGI::TierInputs faulted = hwrt_inputs();
	faulted.fault_latched = true;
	check_tier(faulted, NextGI::TIER_SOFTWARE, NextGI::HWRT_FAULTED);
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
	check_tier(as_memory, NextGI::TIER_SOFTWARE, NextGI::AS_MEMORY);
	as_memory.hwrt_setting = NextGI::HARDWARE_RAY_TRACING_FORCE;
	check_tier(as_memory, NextGI::TIER_SOFTWARE, NextGI::AS_MEMORY);
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
	check_tier(inputs, NextGI::TIER_SOFTWARE, NextGI::NOT_HW_ACCELERATED);
	inputs.ray_query_hw = true;
	check_tier(inputs, NextGI::TIER_SOFTWARE, NextGI::SELFTEST_FAILED);
	inputs.selftest_passed = true;
	check_tier(inputs, NextGI::TIER_SOFTWARE, NextGI::HWRT_FAULTED);
	inputs.fault_latched = false;
	check_tier(inputs, NextGI::TIER_SOFTWARE, NextGI::AS_MEMORY);
	inputs.as_memory_ok = true;
	check_tier(inputs, NextGI::TIER_HWRT, NextGI::TIER_REASON_NONE);
}

TEST_CASE("[NextGI] Tier and reason names are unique and cover the tier reasons") {
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
	// NONE plus the §6.3 reasons and SWRT_REDUCED.
	CHECK_EQ(names.size(), 12u);
	CHECK_EQ(NextGI::get_tier_name(NextGI::TIER_SOFTWARE), "SWRT");
	for (const char *reason : { "NO_RAY_QUERY", "NOT_HW_ACCELERATED", "CPU_DEVICE", "D3D12_NO_DXR", "METAL_SYNC_MODE", "METAL_NO_ARGUMENT_BUFFERS", "SETTING_OFF", "AS_MEMORY", "SELFTEST_FAILED", "HWRT_FAULTED", "SWRT_REDUCED" }) {
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
	CHECK_EQ(result.tier, NextGI::TIER_SOFTWARE);
	CHECK_EQ(result.reason, NextGI::NO_RAY_QUERY);

	// --rendering-driver d3d12: no DXR until PD.
	result = evaluate_mocked_device(false, "D3D12", RenderingDeviceEnums::DEVICE_TYPE_DISCRETE_GPU);
	CHECK_EQ(result.tier, NextGI::TIER_SOFTWARE);
	CHECK_EQ(result.reason, NextGI::D3D12_NO_DXR);
	CHECK_FALSE(result.warn_force);

	// d3d12 under Force: the same reason, with the warning.
	result = evaluate_mocked_device(false, "D3D12", RenderingDeviceEnums::DEVICE_TYPE_DISCRETE_GPU, NextGI::HARDWARE_RAY_TRACING_FORCE);
	CHECK_EQ(result.reason, NextGI::D3D12_NO_DXR);
	CHECK(result.warn_force);

	// A mocked Metal device without ray query. The live M5 Max reports the same until #47
	// flips the acceleration-structure gate. The [NextGI][Metal] cases feed the helper.
	result = evaluate_mocked_device(false, "Metal", RenderingDeviceEnums::DEVICE_TYPE_INTEGRATED_GPU);
	CHECK_EQ(result.tier, NextGI::TIER_SOFTWARE);
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
	CHECK_EQ(line, "NextGI: tier=SWRT reason=D3D12_NO_DXR quality=HIGH (auto selection pending)");
	CHECK_EQ(line.count("NextGI:"), 1);

	// Re-evaluating the same inputs prints nothing.
	line = String();
	CHECK_FALSE(next_gi.update_tier(d3d12, NextGI::QUALITY_AUTO, line));
	CHECK(line.is_empty());

	// A changed result prints again.
	NextGI::TierInputs off = d3d12;
	off.hwrt_setting = NextGI::HARDWARE_RAY_TRACING_OFF;
	REQUIRE(next_gi.update_tier(off, NextGI::QUALITY_EPIC, line));
	CHECK_EQ(line, "NextGI: tier=SWRT reason=SETTING_OFF quality=EPIC");

	// Disabling prints nothing. Enabling again is a new selection, so it prints.
	line = String();
	CHECK_FALSE(next_gi.update_tier(disabled, NextGI::QUALITY_AUTO, line));
	CHECK(line.is_empty());
	REQUIRE(next_gi.update_tier(d3d12, NextGI::QUALITY_AUTO, line));
	CHECK_EQ(line, "NextGI: tier=SWRT reason=D3D12_NO_DXR quality=HIGH (auto selection pending)");
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

TEST_CASE("[NextGI] SWRT auto quality maps the mocked benchmark and the compute-unit class") {
	CHECK_EQ(NextGI::swrt_auto_quality(1200, 16), NextGI::QUALITY_HIGH);
	CHECK_EQ(NextGI::swrt_auto_quality(5000, 40), NextGI::QUALITY_HIGH);
	CHECK_EQ(NextGI::swrt_auto_quality(5001, 40), NextGI::QUALITY_MEDIUM);
	CHECK_EQ(NextGI::swrt_auto_quality(0, 40), NextGI::QUALITY_MEDIUM);
	// A fast trace on a small GPU stays Medium.
	CHECK_EQ(NextGI::swrt_auto_quality(1000, 8), NextGI::QUALITY_MEDIUM);
	CHECK_EQ(NextGI::swrt_auto_quality(1000, 15), NextGI::QUALITY_MEDIUM);

	CHECK_EQ(NextGI::swrt_compute_unit_class(24, 0), 24u);
	CHECK_EQ(NextGI::swrt_compute_unit_class(0, 0), NextGI::SWRT_AUTO_COMPUTE_UNITS);
	CHECK_EQ(NextGI::swrt_compute_unit_class(0, NextGI::SWRT_AUTO_MEMORY_FLOOR), NextGI::SWRT_AUTO_COMPUTE_UNITS);
	CHECK_EQ(NextGI::swrt_compute_unit_class(0, NextGI::SWRT_AUTO_MEMORY_FLOOR - 1), 8u);

	const String high = NextGI::format_swrt_auto_reason(2500, 16, NextGI::QUALITY_HIGH);
	CHECK(high.begins_with("auto: sdf benchmark 2.50 ms, 16 compute units, HIGH"));
	const String medium = NextGI::format_swrt_auto_reason(800, 8, NextGI::QUALITY_MEDIUM);
	CHECK(medium.contains("MEDIUM"));
	CHECK(medium.contains("8 compute units"));
}

TEST_CASE("[NextGI] SWRT drops global SDF levels to a floor of 3") {
	const NextGI::SWRTLevelBudget full = NextGI::resolve_swrt_levels(6, 100, 600);
	CHECK_EQ(full.levels, 6u);
	CHECK_FALSE(full.reduced);

	const NextGI::SWRTLevelBudget unknown = NextGI::resolve_swrt_levels(6, 0, 0);
	CHECK_EQ(unknown.levels, 6u);
	CHECK_FALSE(unknown.reduced);

	const NextGI::SWRTLevelBudget dropped = NextGI::resolve_swrt_levels(6, 100, 450);
	CHECK_EQ(dropped.levels, 4u);
	CHECK(dropped.reduced);

	const NextGI::SWRTLevelBudget floor = NextGI::resolve_swrt_levels(6, 100, 50);
	CHECK_EQ(floor.levels, NextGI::SWRT_MIN_LEVELS);
	CHECK(floor.reduced);

	const NextGI::SWRTLevelBudget clamped = NextGI::resolve_swrt_levels(0, 0, 0);
	CHECK_EQ(clamped.levels, 1u);
	CHECK_FALSE(clamped.reduced);

	NextGI::TierResult reduced_line;
	reduced_line.tier = NextGI::TIER_SOFTWARE;
	reduced_line.reason = NextGI::NO_RAY_QUERY;
	reduced_line.swrt_reduced = true;
	const String line = NextGI::format_tier_line(reduced_line, NextGI::QUALITY_HIGH);
	CHECK(line.contains("reason=NO_RAY_QUERY"));
	CHECK(line.contains("SWRT_REDUCED"));
	CHECK_FALSE(line.contains("reason=SWRT_REDUCED"));
	CHECK_EQ(NextGI::get_reason_name(NextGI::SWRT_REDUCED), "SWRT_REDUCED");
}

struct NextGIWarningCapture {
	ErrorHandlerList handler;
	int matches = 0;
	NextGIWarningCapture() {
		handler.errfunc = _capture;
		handler.userdata = this;
		add_error_handler(&handler);
	}
	~NextGIWarningCapture() {
		remove_error_handler(&handler);
	}
	static void _capture(void *p_self, const char *p_function, const char *p_file, int p_line, const char *p_error, const char *p_message, bool p_editor_notify, ErrorHandlerType p_type) {
		(void)p_function;
		(void)p_file;
		(void)p_line;
		(void)p_editor_notify;
		if (p_type != ERR_HANDLER_WARNING) {
			return;
		}
		const char *text = (p_message != nullptr && *p_message != 0) ? p_message : p_error;
		const String message = String::utf8(text != nullptr ? text : "");
		if (message.contains("EXPERIMENTAL") && message.contains("restart")) {
			static_cast<NextGIWarningCapture *>(p_self)->matches++;
		}
	}
};

// Sets a project setting for one test and restores it, also when a CHECK fails.
struct NextGISettingOverride {
	String name;
	bool existed = false;
	Variant previous;
	NextGISettingOverride(const String &p_name, const Variant &p_value) :
			name(p_name) {
		ProjectSettings *ps = ProjectSettings::get_singleton();
		existed = ps->has_setting(p_name);
		previous = existed ? ps->get_setting(p_name) : Variant();
		ps->set_setting(p_name, p_value);
	}
	~NextGISettingOverride() {
		ProjectSettings *ps = ProjectSettings::get_singleton();
		if (existed) {
			ps->set_setting(name, previous);
		} else if (ps->has_setting(name)) {
			ps->clear(name);
		}
	}
};

static const char *NEXTGI_KEEP_WHEN_HWRT = "rendering/global_illumination/mesh_sdf/keep_when_hwrt";

TEST_CASE("[NextGI] Runtime toggle round-trips HWRT, SWRT and HWRT while keep_when_hwrt is true") {
	NextGISettingOverride keep(NEXTGI_KEEP_WHEN_HWRT, true);
	NextGI next_gi;
	String line;
	REQUIRE(next_gi.update_tier(hwrt_inputs(), NextGI::QUALITY_HIGH, line));
	REQUIRE_EQ(next_gi.get_tier_result().tier, NextGI::TIER_HWRT);
	CHECK(bool(next_gi.get_tier_info()["experimental_toggle"]));

	NextGIWarningCapture warnings;
	next_gi.set_hardware_ray_tracing(NextGI::HARDWARE_RAY_TRACING_OFF);
	CHECK_EQ(next_gi.get_tier_result().tier, NextGI::TIER_SOFTWARE);
	CHECK_EQ(next_gi.get_tier_result().reason, NextGI::SETTING_OFF);
	CHECK_EQ(next_gi.get_tier_inputs().hwrt_setting, NextGI::HARDWARE_RAY_TRACING_OFF);
	CHECK(bool(next_gi.get_tier_info()["experimental_toggle"]));

	next_gi.set_hardware_ray_tracing(NextGI::HARDWARE_RAY_TRACING_AUTO);
	CHECK_EQ(next_gi.get_tier_result().tier, NextGI::TIER_HWRT);
	CHECK_EQ(next_gi.get_tier_result().reason, NextGI::TIER_REASON_NONE);
	CHECK_FALSE(next_gi.get_tier_info().has("hardware_ray_tracing_pending"));
	CHECK_EQ(warnings.matches, 0);
}

TEST_CASE("[NextGI] Runtime toggle stays pending until a restart when keep_when_hwrt is false") {
	NextGISettingOverride keep(NEXTGI_KEEP_WHEN_HWRT, false);
	NextGI next_gi;
	String line;
	REQUIRE(next_gi.update_tier(hwrt_inputs(), NextGI::QUALITY_HIGH, line));
	REQUIRE_EQ(next_gi.get_tier_result().tier, NextGI::TIER_HWRT);
	// No runtime switch applies, so nothing carries its EXPERIMENTAL label.
	CHECK_FALSE(bool(next_gi.get_tier_info()["experimental_toggle"]));
	CHECK_FALSE(next_gi.get_tier_info().has("hardware_ray_tracing_pending"));

	NextGIWarningCapture warnings;
	next_gi.set_hardware_ray_tracing(NextGI::HARDWARE_RAY_TRACING_OFF);
	CHECK_EQ(next_gi.get_tier_result().tier, NextGI::TIER_HWRT);
	CHECK_EQ(next_gi.get_tier_inputs().hwrt_setting, NextGI::HARDWARE_RAY_TRACING_AUTO);
	CHECK_EQ(warnings.matches, 1);
	Dictionary info = next_gi.get_tier_info();
	REQUIRE(info.has("hardware_ray_tracing_pending"));
	CHECK_EQ(int(info["hardware_ray_tracing_pending"]), int(NextGI::HARDWARE_RAY_TRACING_OFF));

	// A second request replaces the pending mode, with no second warning.
	next_gi.set_hardware_ray_tracing(NextGI::HARDWARE_RAY_TRACING_FORCE);
	CHECK_EQ(next_gi.get_tier_result().tier, NextGI::TIER_HWRT);
	CHECK_EQ(int(next_gi.get_tier_info()["hardware_ray_tracing_pending"]), int(NextGI::HARDWARE_RAY_TRACING_FORCE));
	CHECK_EQ(warnings.matches, 1);

	// Asking for the running mode again ends the pending request.
	next_gi.set_hardware_ray_tracing(NextGI::HARDWARE_RAY_TRACING_AUTO);
	CHECK_EQ(next_gi.get_tier_result().tier, NextGI::TIER_HWRT);
	CHECK_FALSE(next_gi.get_tier_info().has("hardware_ray_tracing_pending"));
	CHECK_EQ(warnings.matches, 1);
}

// The EXPERIMENTAL label comes from the live status, not only from hand-built dictionaries.
TEST_CASE("[NextGI] Status labels the runtime toggle EXPERIMENTAL only on an active tier with keep_when_hwrt") {
	NextGI next_gi;
	String line;
	{
		NextGISettingOverride keep(NEXTGI_KEEP_WHEN_HWRT, true);
		NextGI::TierInputs disabled = hwrt_inputs();
		disabled.project_enabled = false;
		next_gi.update_tier(disabled, NextGI::QUALITY_AUTO, line);
		CHECK_FALSE(bool(next_gi.get_status()["experimental_toggle"]));
#ifdef TOOLS_ENABLED
		const String panel = NextGIEditorPlugin::format_status_panel(next_gi.get_status());
		CHECK(panel.begins_with("NextGI SDFGI-legacy (NONE)"));
		CHECK_FALSE(panel.contains("EXPERIMENTAL"));
#endif // TOOLS_ENABLED

		NextGI::TierInputs mobile = hwrt_inputs();
		mobile.forward_plus = false;
		next_gi.update_tier(mobile, NextGI::QUALITY_AUTO, line);
		CHECK_FALSE(bool(next_gi.get_status()["experimental_toggle"]));

		next_gi.update_tier(no_ray_query_inputs(NextGI::NO_RAY_QUERY), NextGI::QUALITY_AUTO, line);
		CHECK(bool(next_gi.get_status()["experimental_toggle"]));
		next_gi.update_tier(hwrt_inputs(), NextGI::QUALITY_AUTO, line);
		CHECK(bool(next_gi.get_tier_info()["experimental_toggle"]));
#ifdef TOOLS_ENABLED
		CHECK(NextGIEditorPlugin::format_status_panel(next_gi.get_status()).ends_with(" toggle=EXPERIMENTAL"));
#endif // TOOLS_ENABLED
	}
	{
		NextGISettingOverride keep(NEXTGI_KEEP_WHEN_HWRT, false);
		CHECK_FALSE(bool(next_gi.get_status()["experimental_toggle"]));
		next_gi.update_tier(no_ray_query_inputs(NextGI::NO_RAY_QUERY), NextGI::QUALITY_AUTO, line);
		CHECK_FALSE(bool(next_gi.get_tier_info()["experimental_toggle"]));
	}
}

// The dummy scene renderer with a copy of RendererSceneRenderRD's one-line override: the SDFGI
// gate (RendererSceneRender::environment_get_sdfgi_enabled()) reads a real NextGI tier, selected
// from the project settings by init() where no device is needed. The RD renderer's own override
// needs a device; frame_block_check --sdfgi covers it on the GPU: no "Render SDFGI" under
// --expect=active (HWRT or SWRT), and SDFGI runs under --expect=inactive (DISABLED).
class NextGIGateSceneRender : public RasterizerSceneDummy {
public:
	NextGI next_gi;

protected:
	virtual bool _nextgi_active() const override { return next_gi.is_active(); }
};

TEST_CASE("[NextGI] NextGI replaces SDFGI only where the project and the Environment both enable it") {
	NextGIGateSceneRender scene_render;
	const RID env = scene_render.environment_allocate();
	scene_render.environment_initialize(env);
	ERR_PRINT_OFF; // SDFGI warns once outside Forward+.
	scene_render.environment_set_sdfgi(env, true, 4, 0.2, RSE::ENV_SDFGI_Y_SCALE_75_PERCENT, false, 0.5, true, 1.0, 1.1, 1.1);
	ERR_PRINT_ON;
	scene_render.environment_set_nextgi_enabled(env, true);
	const RID plain_env = scene_render.environment_allocate();
	scene_render.environment_initialize(plain_env);
	ERR_PRINT_OFF;
	scene_render.environment_set_sdfgi(plain_env, true, 4, 0.2, RSE::ENV_SDFGI_Y_SCALE_75_PERCENT, false, 0.5, true, 1.0, 1.1, 1.1);
	ERR_PRINT_ON;

	const char *enabled_setting = "rendering/global_illumination/nextgi/enabled";
	const char *hwrt_setting = "rendering/global_illumination/nextgi/hardware_ray_tracing";
	const char *quality_setting = "rendering/global_illumination/nextgi/quality";
	{
		// nextgi/enabled=false, the default: NextGI is fully off, so SDFGI runs as upstream (L1).
		NextGISettingOverride enabled(enabled_setting, false);
		scene_render.next_gi.init(true);
		REQUIRE_EQ(scene_render.next_gi.get_tier_result().tier, NextGI::TIER_DISABLED);
		CHECK(scene_render.environment_get_sdfgi_enabled(env));
		CHECK_EQ(scene_render.environment_get_sdfgi_cascades(env), 4);
		CHECK(scene_render.environment_get_sdfgi_enabled(plain_env));

		// Mobile and other non-Forward+ RD renderers never run NextGI either.
		scene_render.next_gi.init(false);
		REQUIRE_EQ(scene_render.next_gi.get_tier_result().tier, NextGI::TIER_UNSUPPORTED_RENDERER);
		CHECK(scene_render.environment_get_sdfgi_enabled(env));
	}
	{
		// NextGI on with hardware_ray_tracing=Off: the software tier replaces SDFGI for that
		// Environment only. A preset quality keeps the auto benchmark out of this test.
		NextGISettingOverride enabled(enabled_setting, true);
		NextGISettingOverride hwrt(hwrt_setting, int(NextGI::HARDWARE_RAY_TRACING_OFF));
		NextGISettingOverride quality(quality_setting, int(NextGI::QUALITY_HIGH));
		scene_render.next_gi.init(true);
		REQUIRE_EQ(scene_render.next_gi.get_tier_result().tier, NextGI::TIER_SOFTWARE);
		CHECK_FALSE(scene_render.environment_get_sdfgi_enabled(env));
		CHECK(scene_render.environment_get_sdfgi_enabled(plain_env));
	}
	String line;
	scene_render.next_gi.update_tier(hwrt_inputs(), NextGI::QUALITY_HIGH, line);
	REQUIRE_EQ(scene_render.next_gi.get_tier_result().tier, NextGI::TIER_HWRT);
	CHECK_FALSE(scene_render.environment_get_sdfgi_enabled(env));
	CHECK(scene_render.environment_get_sdfgi_enabled(plain_env));

	// Turning the Environment's NextGI off brings its SDFGI back with its cascades.
	scene_render.environment_set_nextgi_enabled(env, false);
	CHECK(scene_render.environment_get_sdfgi_enabled(env));
	CHECK_EQ(scene_render.environment_get_sdfgi_cascades(env), 4);

	scene_render.environment_free(plain_env);
	scene_render.environment_free(env);
}

TEST_CASE("[NextGI] Ray-query ShaderRDs initialize only under the HWRT tier") {
	NextGI next_gi;
	String line;

	// The M5 Max today: software tier, so no ray-query ShaderRD may initialize.
	next_gi.update_tier(no_ray_query_inputs(NextGI::NO_RAY_QUERY), NextGI::QUALITY_AUTO, line);
	CHECK_FALSE(next_gi.can_initialize_ray_query_shaders());
	CHECK(next_gi.is_active());
	ERR_PRINT_OFF;
	next_gi.notify_ray_query_shader_initialized();
	ERR_PRINT_ON;
	Dictionary info = next_gi.get_tier_info();
	CHECK_EQ(String(info["tier"]), "SWRT");
	CHECK_EQ(String(info["reason"]), "NO_RAY_QUERY");
	CHECK_EQ(String(info["quality"]), "HIGH");
	CHECK_EQ(int(info["ray_query_shaders_initialized"]), 0);

	// HWRT: ray-query ShaderRDs may initialize, and the count is reported.
	next_gi.update_tier(hwrt_inputs(), NextGI::QUALITY_AUTO, line);
	CHECK(next_gi.can_initialize_ray_query_shaders());
	CHECK(next_gi.is_active());
	next_gi.notify_ray_query_shader_initialized();
	info = next_gi.get_tier_info();
	CHECK_EQ(String(info["tier"]), "HWRT");
	CHECK_EQ(int(info["ray_query_shaders_initialized"]), 1);

	// Disabled: no ray-query shaders, and NextGI is inactive, so SDFGI follows the Environment.
	NextGI::TierInputs disabled = hwrt_inputs();
	disabled.project_enabled = false;
	next_gi.update_tier(disabled, NextGI::QUALITY_AUTO, line);
	CHECK_FALSE(next_gi.can_initialize_ray_query_shaders());
	CHECK_FALSE(next_gi.is_active());
	CHECK_EQ(String(next_gi.get_tier_info()["quality"]), "NONE");
}

TEST_CASE("[NextGI] Status reason stays a tier reason, and the tier view tints NO_RAY_QUERY") {
	NextGI next_gi;
	String line;
	next_gi.update_tier(no_ray_query_inputs(NextGI::NO_RAY_QUERY), NextGI::QUALITY_AUTO, line);

	const Dictionary status = next_gi.get_status();
	CHECK_EQ(String(status["tier"]), "SWRT");
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
	CHECK(tier_view.label.contains("SWRT"));

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
		check_metal_tier(caps, NextGI::TIER_SOFTWARE, NextGI::NOT_HW_ACCELERATED);
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
	check_metal_tier(caps, NextGI::TIER_SOFTWARE, NextGI::METAL_SYNC_MODE);

	// Barriers is checked before argument buffers.
	const RenderingDeviceDriver::MetalRayQueryCaps both = metal_family(1010, false, false);
	CHECK_EQ(both.reason, RenderingDeviceDriver::RAY_QUERY_REASON_METAL_SYNC_MODE);
	check_metal_tier(both, NextGI::TIER_SOFTWARE, NextGI::METAL_SYNC_MODE);
}

TEST_CASE("[NextGI][Metal] Argument buffers off gives METAL_NO_ARGUMENT_BUFFERS") {
	const RenderingDeviceDriver::MetalRayQueryCaps caps = metal_family(1010, true, false);
	CHECK_FALSE(caps.ray_query);
	CHECK_EQ(caps.reason, RenderingDeviceDriver::RAY_QUERY_REASON_METAL_NO_ARGUMENT_BUFFERS);
	check_metal_tier(caps, NextGI::TIER_SOFTWARE, NextGI::METAL_NO_ARGUMENT_BUFFERS);
}

TEST_CASE("[NextGI][Metal] macOS 13 gives NO_RAY_QUERY") {
	const RenderingDeviceDriver::MetalRayQueryCaps caps = metal_family(1010, true, true, true, 13);
	CHECK_FALSE(caps.ray_query);
	CHECK_FALSE(caps.hardware);
	CHECK_EQ(caps.reason, RenderingDeviceDriver::RAY_QUERY_REASON_NO_RAY_QUERY);
	check_metal_tier(caps, NextGI::TIER_SOFTWARE, NextGI::NO_RAY_QUERY);
}

TEST_CASE("[NextGI][Metal] A missing ray-query capability gives NO_RAY_QUERY") {
	CHECK_EQ(metal_family(1010, true, true, false).reason, RenderingDeviceDriver::RAY_QUERY_REASON_NO_RAY_QUERY);
	CHECK_EQ(metal_family(1010, true, true, true, 14, false).reason, RenderingDeviceDriver::RAY_QUERY_REASON_NO_RAY_QUERY);
	CHECK_EQ(metal_family(1010, true, true, true, 14, true, 20300).reason, RenderingDeviceDriver::RAY_QUERY_REASON_NO_RAY_QUERY);
	CHECK_EQ(metal_family(1010, true, true, true, 14, true, RenderingDeviceDriver::METAL_RAY_QUERY_MIN_MSL, false).reason, RenderingDeviceDriver::RAY_QUERY_REASON_NO_RAY_QUERY);
	check_metal_tier(metal_family(1009, true, true, false), NextGI::TIER_SOFTWARE, NextGI::NO_RAY_QUERY);
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

TEST_CASE("[NextGI] Frame block: active-tier views with nextgi_enabled only, never reflection probes") {
	// GodotNext: ps_swrt_trace (#186). Both active tiers run the block (plan §21.2.4).
	CHECK(NextGI::view_uses_frame_block(NextGI::TIER_HWRT, false, true, false, 1));
	CHECK(NextGI::view_uses_frame_block(NextGI::TIER_SOFTWARE, false, true, false, 1));

	for (const NextGI::Tier tier : { NextGI::TIER_HWRT, NextGI::TIER_SOFTWARE }) {
		// Reflection-probe renders never enter the block.
		CHECK_FALSE(NextGI::view_uses_frame_block(tier, true, true, false, 1));
		// The Environment has nextgi_enabled off, or there is no Environment.
		CHECK_FALSE(NextGI::view_uses_frame_block(tier, false, false, false, 1));
		// The unshaded debug draw.
		CHECK_FALSE(NextGI::view_uses_frame_block(tier, false, true, true, 1));
	}

	// Multiview (XR): the software trace binds one depth and one normal-roughness layer, so its
	// views stay outside the block. The hardware gather has no such guard (a known gap).
	CHECK_FALSE(NextGI::view_uses_frame_block(NextGI::TIER_SOFTWARE, false, true, false, 2));
	CHECK(NextGI::view_uses_frame_block(NextGI::TIER_HWRT, false, true, false, 2));

	// No inactive tier runs it.
	for (const NextGI::Tier tier : { NextGI::TIER_DISABLED, NextGI::TIER_UNSUPPORTED_RENDERER }) {
		CHECK_FALSE(NextGI::view_uses_frame_block(tier, false, true, false, 1));
		CHECK_FALSE(NextGI::view_uses_frame_block(tier, true, true, false, 1));
	}

	// Mocked devices: an RTX GPU on Vulkan runs the block on HWRT, Metal without ray query and
	// D3D12 without DXR run it on the software tier.
	const NextGI::TierResult vulkan = evaluate_mocked_device(true, "Vulkan", RenderingDeviceEnums::DEVICE_TYPE_DISCRETE_GPU);
	const NextGI::TierResult metal = evaluate_mocked_device(false, "Metal", RenderingDeviceEnums::DEVICE_TYPE_INTEGRATED_GPU);
	const NextGI::TierResult d3d12 = evaluate_mocked_device(false, "D3D12", RenderingDeviceEnums::DEVICE_TYPE_DISCRETE_GPU);
	CHECK_EQ(vulkan.tier, NextGI::TIER_HWRT);
	CHECK_EQ(metal.tier, NextGI::TIER_SOFTWARE);
	CHECK_EQ(d3d12.tier, NextGI::TIER_SOFTWARE);
	CHECK(NextGI::view_uses_frame_block(vulkan.tier, false, true, false, 1));
	CHECK(NextGI::view_uses_frame_block(metal.tier, false, true, false, 1));
	CHECK(NextGI::view_uses_frame_block(d3d12.tier, false, true, false, 1));
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

TEST_CASE("[NextGI][FarField] Six axis cards bound a proxy cell") {
	const AABB bounds(Vector3(-2, -1, -3), Vector3(4, 2, 6));
	RendererRD::NextGICardClusterResult result;
	RendererRD::nextgi_src_axis_cards(bounds, result);
	REQUIRE_EQ(result.cards.size(), 6u);
	CHECK_EQ(result.max_extent, 6.0f);
	for (uint32_t i = 0; i < result.cards.size(); i++) {
		const RendererRD::NextGICardDesc &card = result.cards[i];
		CHECK(card.axis_u.cross(card.axis_v).is_equal_approx(card.axis_n));
		CHECK(Math::is_equal_approx(card.origin.distance_to(bounds.get_center()), bounds.size[i / 2] * 0.5f + result.texel_size));
		CHECK(Math::is_equal_approx(card.depth_range, bounds.size[i / 2] + result.texel_size * 2.0f));
		CHECK(card.texels_u >= 4u);
		CHECK(card.texels_v >= 4u);
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

	RendererRD::NextGIShelfPacker reused(65, 65);
	Rect2i first;
	Rect2i second;
	REQUIRE(reused.allocate(64, 32, first));
	REQUIRE(reused.allocate(64, 32, second));
	REQUIRE_FALSE(reused.allocate(32, 32, rect));
	reused.free(first);
	REQUIRE(reused.allocate(32, 32, rect));
	CHECK_EQ(rect, Rect2i(0, 0, 32, 32));
	CHECK_EQ(reused.get_used_texels(), 64u * 32u + 32u * 32u);
	reused.free(rect);
	REQUIRE(reused.allocate(64, 32, rect));
	CHECK_EQ(rect, first);
	CHECK_EQ(reused.get_used_texels(), 64u * 64u);
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

	const NextGIVelocityPlan external = nextgi_velocity_plan(NEXTGI_VELOCITY_SCALE_EXTERNAL, NEXTGI_VELOCITY_DENSE, false, true);
	CHECK(external.active);
	CHECK_EQ(external.pre_clear_r, mfx.pre_clear_r);
	CHECK_EQ(external.pre_clear_g, mfx.pre_clear_g);
	CHECK_EQ(external.prepass_writes, mfx.prepass_writes);
	CHECK_EQ(external.color_pass_loads, mfx.color_pass_loads);
	CHECK_EQ(external.skip_clear_pass, mfx.skip_clear_pass);
	CHECK_EQ(external.mfx_preserve, mfx.mfx_preserve);
	const NextGIVelocityPlan external_off = nextgi_velocity_plan(NEXTGI_VELOCITY_SCALE_EXTERNAL, NEXTGI_VELOCITY_DENSE, false, false);
	const NextGIVelocityPlan mfx_off = nextgi_velocity_plan(NEXTGI_VELOCITY_SCALE_MFX, NEXTGI_VELOCITY_DENSE, false, false);
	CHECK_EQ(external_off.active, mfx_off.active);
	CHECK_EQ(external_off.prepass_writes, mfx_off.prepass_writes);
	CHECK_EQ(external_off.mfx_preserve, mfx_off.mfx_preserve);

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
	// #59: skinned actors enter through their posed slots, refitted every frame.
	CHECK(nextgi_rt_instance_eligible(true, true, true, true));
	CHECK_FALSE(nextgi_rt_instance_eligible(true, false, true, true));
	CHECK_FALSE(nextgi_rt_instance_eligible(false, true, true, true));
	CHECK_FALSE(nextgi_rt_instance_eligible(true, true, false, true));

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

TEST_CASE("[NextGI] WRC layout, relocation and diffuse handoff") {
	CHECK_EQ(RendererRD::nextgi_wrc_bytes(), 14ull * 1024ull * 1024ull);
	CHECK_EQ(RendererRD::NEXTGI_WRC_PROBE_COUNT, 16384u);
	CHECK(Math::is_equal_approx(RendererRD::nextgi_wrc_spacing(0), 1.0f));
	CHECK(Math::is_equal_approx(RendererRD::nextgi_wrc_spacing(1), 2.0f));
	CHECK(Math::is_equal_approx(RendererRD::nextgi_wrc_spacing(3), 8.0f));
	CHECK_EQ(RendererRD::nextgi_wrc_level_for_distance(3.0f), 0u);
	CHECK_EQ(RendererRD::nextgi_wrc_level_for_distance(12.0f), 1u);
	CHECK(Math::is_equal_approx(RendererRD::nextgi_wrc_handoff_distance(3.0f), 1.5f));
	CHECK(Math::is_equal_approx(RendererRD::nextgi_wrc_handoff_distance(12.0f), 3.0f));

	const Vector3 probe(1.0f, 2.0f, 3.0f);
	CHECK_EQ(RendererRD::nextgi_wrc_relocate(probe, Vector3(10, 0, 0), 0.2f, 2.0f), probe);
	const Vector3 moved = RendererRD::nextgi_wrc_relocate(probe, Vector3(10, 0, 0), 0.5f, 2.0f);
	CHECK(Math::is_equal_approx(moved.x, 1.8f));
	CHECK(Math::is_equal_approx(moved.distance_to(probe), 0.8f));
	CHECK(moved.distance_to(probe) <= 0.4f * 2.0f + 0.0001f);

	CHECK(Math::is_equal_approx(RendererRD::nextgi_sample_confidence(true, false, false, 0.2f), 1.0f));
	CHECK(Math::is_equal_approx(RendererRD::nextgi_sample_confidence(false, true, true, 0.2f), 1.0f));
	CHECK(Math::is_equal_approx(RendererRD::nextgi_sample_confidence(false, false, true, 0.4f), 0.4f));
	CHECK(Math::is_equal_approx(RendererRD::nextgi_sample_confidence(false, false, true, 2.0f), 0.99f));
	CHECK(Math::is_equal_approx(RendererRD::nextgi_sample_confidence(false, false, false, 1.0f), 0.0f));

	const Vector3 snapped = RendererRD::nextgi_wrc_snap_origin(Vector3(1.2f, -0.4f, 3.9f), 2.0f);
	CHECK(Math::is_equal_approx(snapped.x, 0.0f));
	CHECK(Math::is_equal_approx(snapped.y, -2.0f));
	CHECK(Math::is_equal_approx(snapped.z, 2.0f));
}

TEST_CASE("[NextGI] MultiMesh classification, element expansion and Q18") {
	RendererRD::NextGIMultiMeshChange change;
	for (uint64_t frame = 1; frame <= 7; frame++) {
		RendererRD::nextgi_rt_note_multimesh_change(change, frame);
		CHECK_FALSE(RendererRD::nextgi_rt_multimesh_is_dynamic(change));
	}
	RendererRD::nextgi_rt_note_multimesh_change(change, 8);
	CHECK(RendererRD::nextgi_rt_multimesh_is_dynamic(change));
	RendererRD::nextgi_rt_note_multimesh_change(change, 8);
	CHECK_EQ(change.change_streak, 8);
	RendererRD::nextgi_rt_note_multimesh_change(change, 20);
	CHECK_FALSE(RendererRD::nextgi_rt_multimesh_is_dynamic(change));
	CHECK_EQ(change.change_streak, 1);

	const float rows[12] = { 1, 0, 0, 2, 0, 1, 0, 3, 0, 0, 1, 4 };
	const Transform3D world = RendererRD::nextgi_rt_element_world(Transform3D(Basis(), Vector3(10, 0, 0)), rows);
	CHECK(Math::is_equal_approx(world.origin.x, 12.0f));
	CHECK(Math::is_equal_approx(world.origin.y, 3.0f));
	CHECK(Math::is_equal_approx(world.origin.z, 4.0f));

	float angular[3] = { 0.2f, 0.9f, 0.4f };
	RendererRD::NextGIRTExclusion reasons[3] = { RendererRD::NEXTGI_RT_INCLUDED, RendererRD::NEXTGI_RT_INCLUDED, RendererRD::NEXTGI_RT_INCLUDED };
	RendererRD::nextgi_rt_apply_instance_cap(angular, reasons, 3, 2);
	CHECK_EQ(reasons[0], RendererRD::NEXTGI_RT_INSTANCE_CAP);
	CHECK_EQ(reasons[1], RendererRD::NEXTGI_RT_INCLUDED);
	CHECK_EQ(reasons[2], RendererRD::NEXTGI_RT_INCLUDED);

	RendererRD::NextGIRTCoverage coverage;
	coverage.denominator = 100;
	coverage.numerator = 95;
	coverage.not_in_scene = 3;
	coverage.instance_cap = 1;
	coverage.dynamic_excluded = 1;
	coverage.blas_pending = 0;
	CHECK_EQ(RendererRD::nextgi_rt_coverage_percent(coverage), 95);
	coverage.denominator = 0;
	CHECK_EQ(RendererRD::nextgi_rt_coverage_percent(coverage), 0);
}

TEST_CASE("[NextGI] SRC work streams: sweeps, bake filter and both caps") {
	const AABB swept(Vector3(0, 0, 0), Vector3(1, 1, 1));
	const AABB sweep = RendererRD::nextgi_src_directional_sweep(swept, Vector3(0, 0, 1), 10.0f);
	CHECK(sweep.position.z < -9.0f);
	CHECK(sweep.intersects(AABB(Vector3(0, 0, -5), Vector3(1, 1, 1))));

	const AABB page(Vector3(2, 0, 0), Vector3(0.2f, 0.2f, 0.2f));
	CHECK(RendererRD::nextgi_src_local_light_dirties_page(page, swept, Vector3(0, 0, 0), 4.0f));
	CHECK_FALSE(RendererRD::nextgi_src_local_light_dirties_page(page, swept, Vector3(0, 0, 0), 0.5f));

	const AABB merged = RendererRD::nextgi_src_event_bounds(AABB(Vector3(0, 0, 0), Vector3(1, 1, 1)), true, AABB(Vector3(2, 0, 0), Vector3(1, 1, 1)), true);
	CHECK(merged.size.x > 2.5f);
	CHECK(RendererRD::nextgi_light_reaches_receiver(false, RendererRD::NEXTGI_RT_FLAG_LIGHTMAPPED));
	CHECK_FALSE(RendererRD::nextgi_light_reaches_receiver(true, RendererRD::NEXTGI_RT_FLAG_LIGHTMAPPED));
	CHECK(RendererRD::nextgi_src_scale_changed(1.0f, 1.02f));
	CHECK_FALSE(RendererRD::nextgi_src_scale_changed(1.0f, 1.005f));

	RendererRD::NextGISrcSchedulePage pages[4];
	for (int i = 0; i < 4; i++) {
		pages[i].texels = 100000;
		pages[i].ray_estimate = 100000;
		pages[i].last_relit_frame = uint32_t(i);
	}
	pages[0].dirty_direct = true;
	pages[0].priority = 1.0f;
	pages[1].dirty_direct = true;
	pages[1].priority = 5.0f;
	uint32_t order[4] = {};
	const uint32_t scheduled = RendererRD::nextgi_src_schedule_streams(pages, 4, 150000, 150000, order, 4);
	CHECK_EQ(scheduled, 1u);
	CHECK_EQ(order[0], 1u);
	pages[0].dirty_direct = false;
	pages[1].dirty_direct = false;
	pages[0].far_field = true;
	CHECK_EQ(RendererRD::nextgi_src_schedule_streams(pages, 4, 400000, 400000, order, 4), 4u);
	CHECK_EQ(order[0], 1u);
	CHECK_EQ(order[3], 0u);
	const uint32_t high = RendererRD::nextgi_src_budget_for_quality(2);
	const uint32_t medium = RendererRD::nextgi_src_budget_for_quality(1);
	const uint32_t epic = RendererRD::nextgi_src_budget_for_quality(3);
	CHECK_EQ(high, 256u * 1024u);
	CHECK_EQ(medium, 128u * 1024u);
	CHECK_EQ(epic, 512u * 1024u);
}

TEST_CASE("[NextGI][Denoise] Reset-region selection marks exactly the dirty probes") {
	// The temporal pass rejects history inside the dirty AABBs from the RT registry (#39). The mask
	// is one byte per probe so it can be uploaded as-is.
	const PackedByteArray empty = RendererRD::NextGIDenoise::select_reset_probes(Vector<Rect2i>(), Size2i(4, 3));
	REQUIRE(empty.size() == 12);
	for (int i = 0; i < empty.size(); i++) {
		CHECK_EQ(empty[i], 0);
	}

	const Vector<Rect2i> single = { Rect2i(1, 1, 2, 1) };
	const PackedByteArray mask = RendererRD::NextGIDenoise::select_reset_probes(single, Size2i(4, 3));
	REQUIRE(mask.size() == 12);
	for (int y = 0; y < 3; y++) {
		for (int x = 0; x < 4; x++) {
			const bool inside = (x == 1 || x == 2) && y == 1;
			CHECK_EQ(mask[y * 4 + x], inside ? 1 : 0);
		}
	}

	// Overlapping regions union, and parts outside the grid are clipped.
	const Vector<Rect2i> overlapping = { Rect2i(2, 0, 4, 2), Rect2i(-1, 2, 2, 4) };
	const PackedByteArray clipped = RendererRD::NextGIDenoise::select_reset_probes(overlapping, Size2i(4, 3));
	REQUIRE(clipped.size() == 12);
	for (int y = 0; y < 3; y++) {
		for (int x = 0; x < 4; x++) {
			const bool inside = (x >= 2 && y <= 1) || (x <= 0 && y == 2);
			CHECK_EQ(clipped[y * 4 + x], inside ? 1 : 0);
		}
	}

	// A fully off-grid region leaves the mask clear, and an empty grid is handled.
	const Vector<Rect2i> away = { Rect2i(10, 10, 2, 2) };
	const PackedByteArray off_grid = RendererRD::NextGIDenoise::select_reset_probes(away, Size2i(4, 3));
	for (int i = 0; i < off_grid.size(); i++) {
		CHECK_EQ(off_grid[i], 0);
	}
	CHECK_EQ(RendererRD::NextGIDenoise::select_reset_probes(Vector<Rect2i>(), Size2i()).size(), 0);
}

TEST_CASE("[NextGI][Denoise] Every denoise mode expands to its own compute stage") {
	// The raw stage source keeps every #ifdef branch; the real check is that each variant compiles
	// with its own mode define, which is what the shader compiler sees.
	struct ModeCase {
		RendererRD::NextGIDenoise::Mode mode;
		const char *token;
		const char *define;
	};
	const ModeCase cases[] = {
		{ RendererRD::NextGIDenoise::MODE_PROBE_SPATIAL, "NEXTGI_DENOISE_KERNEL", "MODE_PROBE_SPATIAL" },
		{ RendererRD::NextGIDenoise::MODE_PROBE_TEMPORAL, "history_length_image", "MODE_PROBE_TEMPORAL" },
		{ RendererRD::NextGIDenoise::MODE_STABILIZE, "stabilizer_enabled", "MODE_STABILIZE" },
	};

	for (const ModeCase &mode_case : cases) {
		const String source = RendererRD::NextGIDenoise::build_shader_source(mode_case.mode);
		REQUIRE_FALSE(source.is_empty());
		CHECK(source.contains("local_size_x = 8"));
		CHECK(source.contains(mode_case.token));
		CHECK(source.contains(mode_case.define));
#ifdef MODULE_GLSLANG_ENABLED
		String error;
		const Vector<uint8_t> spirv = compile_glslang_shader(
				RenderingDeviceCommons::SHADER_STAGE_COMPUTE,
				source,
				RenderingDeviceCommons::SHADER_LANGUAGE_VULKAN_VERSION_1_1,
				RenderingDeviceCommons::SHADER_SPIRV_VERSION_1_6,
				&error);
		CHECK_MESSAGE(!spirv.is_empty(), error);
#endif // MODULE_GLSLANG_ENABLED
	}
}

TEST_CASE("[NextGI][Denoise] The five denoise views map and label") {
	struct ViewCase {
		RSE::ViewportDebugDraw draw;
		NextGI::DebugViewId view;
		const char *label;
	};
	const ViewCase cases[] = {
		{ RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_DENOISE_RAW, NextGI::DEBUG_VIEW_DENOISE_RAW, "raw" },
		{ RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_DENOISE_VARIANCE, NextGI::DEBUG_VIEW_DENOISE_VARIANCE, "variance" },
		{ RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_DENOISE_HISTORY, NextGI::DEBUG_VIEW_DENOISE_HISTORY, "history length" },
		{ RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_DENOISE_REJECTED, NextGI::DEBUG_VIEW_DENOISE_REJECTED, "rejected history" },
		{ RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_DENOISE_DENOISED, NextGI::DEBUG_VIEW_DENOISE_DENOISED, "result" },
	};

	NextGI::TierResult tier;
	tier.tier = NextGI::TIER_HWRT;
	tier.reason = NextGI::TIER_REASON_NONE;

	for (const ViewCase &view_case : cases) {
		CHECK_EQ(NextGI::debug_view_from_viewport(view_case.draw), view_case.view);
		const NextGI::DebugView view = NextGI::resolve_debug_view(view_case.view, tier);
		CHECK_FALSE(view.has_data);
		CHECK(view.tint.is_equal_approx(NextGI::debug_tint_no_data()));
		CHECK(view.label.contains("denoise"));
		CHECK(view.label.contains(view_case.label));
	}

	// The five views are contiguous, which resolve_debug_view relies on.
	CHECK_EQ(int(NextGI::DEBUG_VIEW_DENOISE_DENOISED) - int(NextGI::DEBUG_VIEW_DENOISE_RAW), 4);
	CHECK_EQ(NextGI::debug_view_from_viewport(RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_FALLBACK_REASONS), NextGI::DEBUG_VIEW_FALLBACK_REASONS);
	CHECK_EQ(NextGI::debug_view_from_viewport(RSE::VIEWPORT_DEBUG_DRAW_DISABLED), NextGI::DEBUG_VIEW_MAX);
}

TEST_CASE("[NextGI][RT] The GPU instance encoder compiles for both API layouts") {
	struct LayoutCase {
		RendererRD::NextGIRTScene::InstanceLayout layout;
		const char *marker;
	};
	const LayoutCase cases[] = {
		{ RendererRD::NextGIRTScene::INSTANCE_LAYOUT_VULKAN_64, "(params.counts.z + index) * 16u" },
		{ RendererRD::NextGIRTScene::INSTANCE_LAYOUT_METAL_72, "(params.counts.z + index) * 18u" },
	};

	for (const LayoutCase &layout_case : cases) {
		const String source = RendererRD::NextGIRTScene::build_instances_shader_source(layout_case.layout);
		REQUIRE_FALSE(source.is_empty());
		CHECK(source.contains("local_size_x = 64"));
		CHECK(source.contains(layout_case.marker));
		CHECK(source.contains("nextgi_instance_mask"));
		// The inactive-element guard catches particles parked at -INF.
		CHECK(source.contains("3.0e38"));
#ifdef MODULE_GLSLANG_ENABLED
		String error;
		const Vector<uint8_t> spirv = compile_glslang_shader(
				RenderingDeviceCommons::SHADER_STAGE_COMPUTE,
				source,
				RenderingDeviceCommons::SHADER_LANGUAGE_VULKAN_VERSION_1_1,
				RenderingDeviceCommons::SHADER_SPIRV_VERSION_1_6,
				&error);
		CHECK_MESSAGE(!spirv.is_empty(), error);
#endif // MODULE_GLSLANG_ENABLED
	}
}

TEST_CASE("[NextGI][RT] Dynamic instance budget shares the TLAS capacity") {
	// Dynamic sources take what the CPU-fed instances leave of the 100k cap.
	CHECK_EQ(RendererRD::NextGIRTScene::nextgi_rt_dynamic_budget(100000, 0), 100000u);
	CHECK_EQ(RendererRD::NextGIRTScene::nextgi_rt_dynamic_budget(100000, 50000), 50000u);
	CHECK_EQ(RendererRD::NextGIRTScene::nextgi_rt_dynamic_budget(100000, 99999), 1u);
	CHECK_EQ(RendererRD::NextGIRTScene::nextgi_rt_dynamic_budget(100000, 100000), 0u);
	CHECK_EQ(RendererRD::NextGIRTScene::nextgi_rt_dynamic_budget(100000, 120000), 0u);
}

} // namespace TestNextGI

// GodotNext: nextgi_skinned_actors (#59).
TEST_CASE("[NextGI][Skinned] Refit schedule and triangle budget") {
	using Build = RendererRD::NextGISkinnedBuild;
	// Budget per GPU class.
	CHECK_EQ(RendererRD::nextgi_rt_skinned_triangle_budget(true), 200000u);
	CHECK_EQ(RendererRD::nextgi_rt_skinned_triangle_budget(false), 100000u);
	// Over-budget instances are skipped.
	CHECK(RendererRD::nextgi_rt_skinned_build(3, 10, 50000, 160000, 200000) == Build::NEXTGI_SKINNED_SKIP);
	CHECK(RendererRD::nextgi_rt_skinned_build(3, 10, 40000, 160000, 200000) == Build::NEXTGI_SKINNED_REFIT);
	// (frame + instance) % 32 == 0 rebuilds, otherwise refits.
	CHECK(RendererRD::nextgi_rt_skinned_build(4, 28, 1000, 0, 200000) == Build::NEXTGI_SKINNED_REBUILD);
	CHECK(RendererRD::nextgi_rt_skinned_build(4, 27, 1000, 0, 200000) == Build::NEXTGI_SKINNED_REFIT);
	CHECK(RendererRD::nextgi_rt_skinned_build(0, 0, 1000, 0, 200000) == Build::NEXTGI_SKINNED_REBUILD);
	// The 16-byte surface record packs the posed flag and the bind-pose offset for D6.
	const uint32_t bind_offset = 4096;
	const uint32_t flags = RendererRD::NEXTGI_RT_SURFACE_FLAG_SKINNED | ((bind_offset / RendererRD::NEXTGI_RT_ALIGNMENT) << 8);
	CHECK_EQ(flags & 1u, uint32_t(RendererRD::NEXTGI_RT_SURFACE_FLAG_SKINNED));
	CHECK_EQ((flags >> 8) * RendererRD::NEXTGI_RT_ALIGNMENT, bind_offset);
	CHECK_EQ(sizeof(RendererRD::NextGISurfaceRecord), size_t(16));
	CHECK_EQ(sizeof(RendererRD::NextGIInstanceRecord), size_t(80));
}

// GodotNext: ps_mesh_sdf_atlas (#185).
TEST_CASE("[NextGI][SDF] Brick pool allocation") {
	NextGISDFPool pool(64);
	const uint32_t capacity = pool.get_capacity();
	CHECK_EQ(capacity, uint32_t(64 * 1024 * 1024 / NextGISDFPool::BRICK_BYTES));
	CHECK_EQ(pool.get_used_count(), uint32_t(0));
	CHECK_EQ(pool.get_free_count(), capacity);

	LocalVector<uint32_t> first;
	CHECK(pool.allocate(100, first));
	CHECK_EQ(first.size(), uint32_t(100));
	CHECK_EQ(pool.get_used_count(), uint32_t(100));
	for (uint32_t index = 0; index < first.size(); index++) {
		CHECK(pool.is_allocated(first[index]));
		for (uint32_t other = index + 1; other < first.size(); other++) {
			CHECK_NE(first[index], first[other]);
		}
	}

	// All-or-nothing: an over-capacity request changes nothing.
	const uint32_t free_before = pool.get_free_count();
	LocalVector<uint32_t> too_many;
	CHECK_FALSE(pool.allocate(free_before + 1, too_many));
	CHECK_EQ(pool.get_free_count(), free_before);
	CHECK(too_many.is_empty());

	pool.free(first);
	CHECK_EQ(pool.get_used_count(), uint32_t(0));
	CHECK_EQ(pool.get_free_count(), capacity);
}

TEST_CASE("[NextGI][SDF] Brick pool churn over 10,000 operations") {
	NextGISDFPool pool(64);
	const uint32_t capacity = pool.get_capacity();
	LocalVector<uint8_t> owned;
	owned.resize(capacity);
	memset(owned.ptr(), 0, capacity);
	LocalVector<uint32_t> live_slots;
	RandomNumberGenerator rng;
	rng.set_seed(0x5DF5EED);
	uint32_t live = 0;

	for (int step = 0; step < 10000; step++) {
		const bool want_allocate = live == 0 || rng.randf() < 0.55f;
		if (want_allocate) {
			const uint32_t count = 1 + rng.randi() % 8;
			LocalVector<uint32_t> slots;
			if (!pool.allocate(count, slots)) {
				continue;
			}
			CHECK_EQ(slots.size(), count);
			for (const uint32_t &slot : slots) {
				CHECK_EQ(owned[slot], 0);
				CHECK(pool.is_allocated(slot));
				owned[slot] = 1;
				live_slots.push_back(slot);
				live++;
			}
		} else {
			const uint32_t index = rng.randi() % live_slots.size();
			const uint32_t slot = live_slots[index];
			live_slots.remove_at(index);
			pool.free_slot(slot);
			CHECK_EQ(owned[slot], 1);
			CHECK_FALSE(pool.is_allocated(slot));
			owned[slot] = 0;
			live--;
		}
	}

	CHECK_EQ(pool.get_used_count(), live);
	CHECK_EQ(pool.get_free_count(), capacity - live);

	// Draining the pool returns every remaining slot exactly once.
	LocalVector<uint32_t> rest;
	CHECK(pool.allocate(pool.get_free_count(), rest));
	CHECK_EQ(rest.size(), capacity - live);
	for (const uint32_t &slot : rest) {
		CHECK_EQ(owned[slot], 0);
		owned[slot] = 1;
	}
	for (const uint32_t &slot : live_slots) {
		CHECK(pool.is_allocated(slot));
	}
	pool.free(live_slots);
	pool.free(rest);
	CHECK_EQ(pool.get_used_count(), uint32_t(0));
	CHECK_EQ(pool.get_free_count(), capacity);
}

// GodotNext: ps_mesh_sdf_atlas (#185).
static Ref<MeshDistanceField> _make_test_sdf() {
	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size(Vector3(1.0f, 1.0f, 1.0f));
	MeshSDFSettings settings;
	settings.voxels_per_meter = 8.0f;
	MeshSDFData data;
	const Error err = MeshSDFBuilder::build(box->get_faces(), settings, data);
	CHECK(err == OK);
	CHECK(data.is_valid());
	Ref<MeshDistanceField> field;
	field.instantiate();
	field->set_data(data);
	return field;
}

TEST_CASE("[NextGI][SDF] Atlas page tables and release") {
	NextGISDFAtlas atlas(64);
	const Ref<MeshDistanceField> field = _make_test_sdf();
	REQUIRE(field.is_valid());
	REQUIRE(field->get_mip_count() > 0);
	const RID mesh = RID::from_uint64(9001);
	CHECK(atlas.register_mesh(mesh, field));
	CHECK(atlas.has_mesh(mesh));

	const NextGISDFAtlas::MeshPages *pages = atlas.get_pages(mesh);
	REQUIRE(pages != nullptr);
	CHECK_EQ(int(pages->mip_count), field->get_mip_count());
	CHECK_EQ(pages->page_tables.size(), size_t(field->get_mip_count()));
	CHECK(pages->resident_bytes > 0);
	// Registration allocates every mip's slots but uploads mip 0 only.
	CHECK(atlas.get_pool_used() >= uint32_t(pages->resident_bytes / NextGISDFPool::BRICK_BYTES));

	const TypedArray<PackedInt32Array> source = field->get_page_tables();
	for (int mip = 0; mip < field->get_mip_count(); mip++) {
		const PackedInt32Array &gpu = pages->page_tables[mip];
		const PackedInt32Array &cpu = source[mip];
		CHECK_EQ(gpu.size(), cpu.size());
		LocalVector<uint8_t> used_slots;
		used_slots.resize(atlas.get_pool_capacity());
		memset(used_slots.ptr(), 0, atlas.get_pool_capacity());
		for (int index = 0; index < gpu.size(); index++) {
			if (cpu[index] < 0) {
				CHECK_EQ(gpu[index], cpu[index]);
				continue;
			}
			CHECK(gpu[index] >= 0);
			CHECK(gpu[index] < int32_t(atlas.get_pool_capacity()));
			CHECK_EQ(used_slots[gpu[index]], 0);
			used_slots[gpu[index]] = 1;
		}
	}

	// The same field replaces its previous pages without leaking slots.
	const uint32_t used = atlas.get_pool_used();
	CHECK(atlas.register_mesh(mesh, field));
	CHECK_EQ(atlas.get_pool_used(), used);

	// Streaming a higher mip loads it and evicts it after 60 idle frames.
	const uint32_t id = atlas.get_mesh_id(mesh);
	CHECK_NE(id, UINT32_MAX);
	CHECK(atlas.get_mip_count(id) >= 2);
	const uint64_t resident_before = atlas.get_resident_bytes();
	const uint32_t used_before = atlas.get_pool_used();
	LocalVector<uint32_t> ids;
	LocalVector<uint32_t> mips;
	ids.push_back(id);
	mips.push_back(1);
	atlas.stream_mips(ids, mips, 1, 4 * 1024 * 1024);
	CHECK(atlas.get_resident_bytes() > resident_before);
	CHECK(atlas.get_pool_used() > used_before);
	LocalVector<uint32_t> none_ids;
	LocalVector<uint32_t> none_mips;
	atlas.stream_mips(none_ids, none_mips, 70, 4 * 1024 * 1024);
	CHECK_EQ(atlas.get_resident_bytes(), resident_before);

	atlas.unregister_mesh(mesh);
	CHECK_FALSE(atlas.has_mesh(mesh));
	CHECK_EQ(atlas.get_pool_used(), uint32_t(0));
}

TEST_CASE("[NextGI][SDF] Instance table reasons and world-to-local") {
	NextGISDFAtlas atlas(64);
	const Ref<MeshDistanceField> field = _make_test_sdf();
	const RID mesh = RID::from_uint64(7700);
	REQUIRE(atlas.register_mesh(mesh, field));
	const uint32_t sdf_id = atlas.get_mesh_id(mesh);
	CHECK_NE(sdf_id, UINT32_MAX);

	NextGISDFInstances instances;
	instances.begin_frame();
	const Transform3D transform(Basis(Vector3(0.0f, 1.0f, 0.0f), 0.5f), Vector3(1.0f, 2.0f, 3.0f));
	const uint32_t included = instances.add(atlas, mesh, transform, false, false, 1.5f, true);
	CHECK_EQ(included, uint32_t(0));
	CHECK_EQ(instances.get_record(0).reason, uint32_t(NEXTGI_SDF_INCLUDED));
	CHECK_EQ(instances.get_record(0).sdf_id, sdf_id);
	CHECK_EQ(instances.get_record(0).world_position[3], 1.5f);
	CHECK_EQ((instances.get_record(0).flags & NextGISDFInstances::INSTANCE_VISIBLE), uint32_t(NextGISDFInstances::INSTANCE_VISIBLE));
	// world_to_local maps the instance origin back to the local origin.
	const float *rows = &instances.get_record(0).world_to_local[0][0];
	const Vector3 origin(1.0f, 2.0f, 3.0f);
	const Vector3 local(
			rows[0] * origin.x + rows[1] * origin.y + rows[2] * origin.z + rows[3],
			rows[4] * origin.x + rows[5] * origin.y + rows[6] * origin.z + rows[7],
			rows[8] * origin.x + rows[9] * origin.y + rows[10] * origin.z + rows[11]);
	CHECK(local.length() < 1e-5f);

	const uint32_t skinned = instances.add(atlas, mesh, transform, true, false, 1.0f, true);
	CHECK_EQ(instances.get_record(skinned).reason, uint32_t(NEXTGI_SDF_UNSUPPORTED_DEFORMATION));
	const uint32_t no_field = instances.add(atlas, RID::from_uint64(7701), transform, false, false, 1.0f, true);
	CHECK_EQ(instances.get_record(no_field).reason, uint32_t(NEXTGI_SDF_NO_FIELD));
	CHECK_EQ(instances.get_reason_count(NEXTGI_SDF_INCLUDED), uint32_t(1));
	CHECK_EQ(instances.get_reason_count(NEXTGI_SDF_UNSUPPORTED_DEFORMATION), uint32_t(1));
	CHECK_EQ(instances.get_reason_count(NEXTGI_SDF_NO_FIELD), uint32_t(1));
	CHECK_EQ(instances.get_count(), uint32_t(3));

	instances.clear();
	CHECK_EQ(instances.get_count(), uint32_t(0));
	CHECK(instances.get_buffer().is_null());

	// free_all releases every brick and forgets the mesh ids.
	atlas.free_all();
	CHECK_EQ(atlas.get_pool_used(), uint32_t(0));
	CHECK_EQ(atlas.get_mesh_id(mesh), UINT32_MAX);
	CHECK_FALSE(atlas.has_mesh(mesh));
}

TEST_CASE("[NextGI][SDF] Atlas churn leaves no resident bricks") {
	NextGISDFAtlas atlas(64);
	const Ref<MeshDistanceField> field = _make_test_sdf();
	REQUIRE(field.is_valid());
	for (int step = 0; step < 100; step++) {
		const RID mesh = RID::from_uint64(10000 + step);
		CHECK(atlas.register_mesh(mesh, field));
		CHECK(atlas.has_mesh(mesh));
		CHECK(atlas.get_pool_used() > 0);
		atlas.unregister_mesh(mesh);
		CHECK_FALSE(atlas.has_mesh(mesh));
	}
	CHECK_EQ(atlas.get_pool_used(), uint32_t(0));
}

// GodotNext: nextgi_quality_presets (#55).
TEST_CASE("[NextGI][Quality] Preset budgets and atlas sizes") {
	using Q = RendererRD::NextGI::Quality;
	CHECK_EQ(RendererRD::NextGI::nextgi_quality_total_us(Q::QUALITY_MEDIUM, false), 870u);
	CHECK_EQ(RendererRD::NextGI::nextgi_quality_total_us(Q::QUALITY_HIGH, false), 1600u);
	CHECK_EQ(RendererRD::NextGI::nextgi_quality_total_us(Q::QUALITY_EPIC, false), 3200u);
	CHECK_EQ(RendererRD::NextGI::nextgi_quality_total_us(Q::QUALITY_MEDIUM, true), 2175u);
	CHECK_EQ(RendererRD::NextGI::nextgi_quality_total_us(Q::QUALITY_HIGH, true), 4000u);
	CHECK_EQ(RendererRD::NextGI::nextgi_quality_total_us(Q::QUALITY_EPIC, true), 8000u);

	const RendererRD::NextGI::QualityParams &medium = RendererRD::NextGI::get_quality_params(Q::QUALITY_MEDIUM);
	const RendererRD::NextGI::QualityParams &high = RendererRD::NextGI::get_quality_params(Q::QUALITY_HIGH);
	const RendererRD::NextGI::QualityParams &epic = RendererRD::NextGI::get_quality_params(Q::QUALITY_EPIC);
	CHECK_EQ(medium.src_radiance_mib, 64u);
	CHECK_EQ(medium.src_material_mib, 128u);
	CHECK_EQ(epic.src_radiance_mib, 128u);
	CHECK_EQ(epic.src_material_mib, 256u);
	CHECK_EQ(medium.reflection_divisor, 4u);
	CHECK_EQ(high.reflection_divisor, 2u);
	CHECK_EQ(epic.reflection_divisor, 1u);
	CHECK(std::abs(medium.max_trace_roughness - 0.25f) < 1e-6f);
	CHECK_EQ(medium.relight_texels, 128u * 1024u);
	CHECK_EQ(epic.relight_shadow_rays, 512u * 1024u);
	// The startup benchmark maps to High at or under 5 ms, else Medium.
	CHECK(RendererRD::NextGI::nextgi_auto_quality_from_usec(1200) == Q::QUALITY_HIGH);
	CHECK(RendererRD::NextGI::nextgi_auto_quality_from_usec(5000) == Q::QUALITY_HIGH);
	CHECK(RendererRD::NextGI::nextgi_auto_quality_from_usec(5001) == Q::QUALITY_MEDIUM);
	CHECK(RendererRD::NextGI::nextgi_auto_quality_from_usec(0) == Q::QUALITY_MEDIUM);
	// Medium is High x 0.5 except line 1a.
	CHECK_EQ(medium.ada_lines_us[0], high.ada_lines_us[0]);
	CHECK_EQ(medium.apple_lines_us[0], high.apple_lines_us[0]);
	for (int i = 1; i < 9; i++) {
		CHECK_EQ(medium.ada_lines_us[i] * 2, high.ada_lines_us[i]);
		CHECK_EQ(medium.apple_lines_us[i] * 2, high.apple_lines_us[i]);
	}
}

// GodotNext: ps_lumen_lite (#183).
TEST_CASE("[NextGI][Lite] Irradiance field keeps the traced Medium path") {
	using Q = RendererRD::NextGI::Quality;
	using Gather = RendererRD::NextGI::MediumGather;
	CHECK(RendererRD::NextGI::is_lumen_lite(Q::QUALITY_MEDIUM, Gather::MEDIUM_GATHER_IRRADIANCE_FIELD));
	CHECK_FALSE(RendererRD::NextGI::is_lumen_lite(Q::QUALITY_MEDIUM, Gather::MEDIUM_GATHER_SCREEN_PROBES));
	CHECK_FALSE(RendererRD::NextGI::is_lumen_lite(Q::QUALITY_HIGH, Gather::MEDIUM_GATHER_IRRADIANCE_FIELD));
	CHECK_FALSE(RendererRD::NextGI::is_lumen_lite(Q::QUALITY_AUTO, Gather::MEDIUM_GATHER_IRRADIANCE_FIELD));
	CHECK(RendererRD::NextGI::medium_gather_from_setting(0) == Gather::MEDIUM_GATHER_IRRADIANCE_FIELD);
	CHECK(RendererRD::NextGI::medium_gather_from_setting(1) == Gather::MEDIUM_GATHER_SCREEN_PROBES);
	CHECK(RendererRD::NextGI::medium_gather_from_setting(9) == Gather::MEDIUM_GATHER_IRRADIANCE_FIELD);

	const RendererRD::NextGI::QualityParams &screen = RendererRD::NextGI::get_quality_params(Q::QUALITY_MEDIUM);
	CHECK_EQ(screen.diffuse_rays_per_probe, 8u);
	CHECK_EQ(screen.reflection_divisor, 4u);
	CHECK(std::abs(screen.max_trace_roughness - 0.25f) < 1e-6f);
	CHECK_EQ(screen.relight_texels, 128u * 1024u);
	CHECK_EQ(screen.relight_shadow_rays, 128u * 1024u);
	CHECK_EQ(screen.src_radiance_mib, 64u);
	CHECK_EQ(screen.src_material_mib, 128u);
	CHECK_EQ(RendererRD::NextGI::nextgi_quality_total_us(Q::QUALITY_MEDIUM, false), 870u);
	CHECK_EQ(RendererRD::NextGI::nextgi_quality_total_us(Q::QUALITY_MEDIUM, true), 2175u);

	CHECK_FALSE(RendererRD::NextGI::lite_traces_skinned(true));
	CHECK(RendererRD::NextGI::lite_traces_skinned(false));

	// "½ size" SRC atlases: half of High's texels and bytes, inside the 64 MiB Lite budget.
	const RendererRD::NextGIAtlasBudget lite_budget = RendererRD::nextgi_src_atlas_budget(false, true);
	const RendererRD::NextGIAtlasBudget high_budget = RendererRD::nextgi_src_atlas_budget(false);
	const RendererRD::NextGIAtlasBudget epic_budget = RendererRD::nextgi_src_atlas_budget(true);
	CHECK_EQ(lite_budget.radiance_width, 4096u);
	CHECK_EQ(lite_budget.radiance_height, 2048u);
	CHECK_EQ(lite_budget.radiance_bytes * 2, high_budget.radiance_bytes);
	CHECK_EQ(uint64_t(lite_budget.material_width) * lite_budget.material_height * 2, uint64_t(high_budget.material_width) * high_budget.material_height);
	CHECK_EQ(lite_budget.material_bytes, 64ull * 1024ull * 1024ull);
	CHECK(RendererRD::nextgi_src_atlas_memory_bytes(lite_budget.material_width, lite_budget.material_height, 4) <= lite_budget.material_bytes);
	CHECK_EQ(high_budget.radiance_width, 4096u);
	CHECK_EQ(high_budget.radiance_height, 4096u);
	CHECK_EQ(high_budget.material_width, uint32_t(RendererRD::NEXTGI_SRC_ATLAS_SIZE));
	CHECK_EQ(high_budget.material_bytes, 128ull * 1024ull * 1024ull);
	CHECK_EQ(epic_budget.radiance_height, 8192u);
	CHECK_EQ(epic_budget.material_bytes, 256ull * 1024ull * 1024ull);

	CHECK(RendererRD::NextGI::debug_view_from_viewport(RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_LITE_PROBES) == RendererRD::NextGI::DEBUG_VIEW_LITE_PROBES);
	CHECK_EQ(int(RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_LITE_PROBES), int(RSE::VIEWPORT_DEBUG_DRAW_ML_MATERIAL) + 1);
	const RendererRD::NextGI::DebugView lite_view = RendererRD::NextGI::resolve_debug_view(RendererRD::NextGI::DEBUG_VIEW_LITE_PROBES, RendererRD::NextGI::TierResult());
	CHECK(lite_view.label == "lite probes");

	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String gather_setting = "rendering/global_illumination/nextgi/medium_gather";
	const bool had_gather = ps->has_setting(gather_setting);
	const Variant previous_gather = had_gather ? ps->get_setting(gather_setting) : Variant();
	ps->set_setting(gather_setting, 1);
	CHECK(RendererRD::NextGI::get_medium_gather() == Gather::MEDIUM_GATHER_SCREEN_PROBES);
	CHECK_FALSE(RendererRD::NextGI::is_lumen_lite(Q::QUALITY_MEDIUM, RendererRD::NextGI::get_medium_gather()));
	ps->set_setting(gather_setting, 0);
	CHECK(RendererRD::NextGI::get_medium_gather() == Gather::MEDIUM_GATHER_IRRADIANCE_FIELD);
	if (had_gather) {
		ps->set_setting(gather_setting, previous_gather);
	}
}

// GodotNext: ps_lumen_lite (#183). Fake instance keys: the page table never dereferences them.
static RenderGeometryInstance *lite_test_instance(uint32_t p_index) {
	return reinterpret_cast<RenderGeometryInstance *>(uintptr_t(0x10000u + p_index * 64u));
}

static RendererRD::NextGICard lite_test_card(uint32_t p_side) {
	RendererRD::NextGICard card = {};
	card.atlas_rect[2] = p_side;
	card.atlas_rect[3] = p_side;
	return card;
}

TEST_CASE("[NextGI][Lite] SRC radiance pages stay inside the preset's radiance atlas") {
	struct Preset {
		bool epic;
		bool lite;
	};
	const Preset presets[] = { { false, false }, { true, false }, { false, true } };
	for (const Preset &preset : presets) {
		const RendererRD::NextGIAtlasBudget budget = RendererRD::nextgi_src_atlas_budget(preset.epic, preset.lite);
		const Rect2i texture(0, 0, int(budget.radiance_width), int(budget.radiance_height));

		// The allocator covers exactly the texture: 4096x8192 and 4096x2048 use their whole area.
		RendererRD::NextGIBuddyAllocator buddy;
		buddy.set_extent(budget.radiance_width, budget.radiance_height);
		uint64_t texels = 0;
		bool blocks_inside = true;
		Rect2i block;
		while (buddy.allocate(256, 256, block)) {
			blocks_inside = blocks_inside && texture.encloses(block);
			texels += uint64_t(block.size.x) * uint64_t(block.size.y);
		}
		CHECK(blocks_inside);
		CHECK_EQ(texels, uint64_t(budget.radiance_width) * budget.radiance_height);

		// Relight sizes its page allocator from the same budget as its radiance texture. Cards
		// are placed until the radiance or shadow-mask atlas is full; every page must fit.
		RendererRD::NextGIRelight relight;
		relight.set_page_extent(budget);
		relight.begin_record_pass();
		const RendererRD::NextGICard cards[4] = { lite_test_card(128), lite_test_card(64), lite_test_card(128), lite_test_card(32) };
		uint32_t instance = 0;
		while (instance < 4096) {
			RendererRD::NextGIInstanceRecord record;
			record.card_set = instance;
			record.tlas_index = instance;
			const uint32_t page_table = relight.assign_card_pages(lite_test_instance(instance), record, UINT32_MAX, instance * 4, cards, 4);
			if (page_table == 0x80000000u) {
				break;
			}
			instance++;
		}
		CHECK(instance > 64u);
		LocalVector<uint32_t> words;
		relight.build_page_rect_words(words);
		REQUIRE_EQ(words.size(), instance * 4 * 2);
		bool pages_inside = true;
		for (uint32_t page = 0; page < words.size() / 2; page++) {
			const Rect2i rect(int(words[page * 2] & 0xFFFFu), int(words[page * 2] >> 16), int(words[page * 2 + 1] & 0xFFFFu), int(words[page * 2 + 1] >> 16));
			pages_inside = pages_inside && texture.encloses(rect);
		}
		CHECK_MESSAGE(pages_inside, vformat("Every relight page fits the %dx%d radiance texture.", budget.radiance_width, budget.radiance_height));
	}
}

TEST_CASE("[NextGI][Lite] Relight schedules only page sets from the current records") {
	RendererRD::NextGIRelight relight;
	relight.set_page_extent(RendererRD::nextgi_src_atlas_budget(false, true));
	const RendererRD::NextGICard cards[3] = { lite_test_card(64), lite_test_card(32), lite_test_card(16) };

	relight.begin_record_pass();
	RendererRD::NextGIInstanceRecord a;
	a.card_set = 0;
	a.tlas_index = 3;
	RendererRD::NextGIInstanceRecord b;
	b.card_set = 1;
	b.tlas_index = 4;
	CHECK_EQ(relight.assign_card_pages(lite_test_instance(0), a, UINT32_MAX, 0, cards, 2), 0u);
	CHECK_EQ(relight.assign_card_pages(lite_test_instance(1), b, UINT32_MAX, 2, cards, 1), 2u);
	CHECK_EQ(a.page_table, 0u);
	CHECK_EQ(b.page_table, 2u);
	LocalVector<uint32_t> current;
	relight.collect_current_page_sets(current);
	CHECK_EQ(current.size(), 2u);

	// Next frame's records hold only B, at another index. A's record_index (3) now names some
	// other instance, which may have no card set, so A must not be relit.
	relight.begin_record_pass();
	b.tlas_index = 0;
	CHECK_EQ(relight.assign_card_pages(lite_test_instance(1), b, UINT32_MAX, 2, cards, 1), 2u);
	relight.collect_current_page_sets(current);
	REQUIRE_EQ(current.size(), 1u);
	const RendererRD::NextGIRelight::PageSetInfo b_info = relight.get_page_set_info(current[0]);
	CHECK_EQ(b_info.record_index, 0u);
	CHECK_EQ(b_info.card_set, 1u);
	CHECK_EQ(relight.get_page_set_count(), 2u);

	// The same key with another card set (a reused instance address, a new material key) gets
	// pages cut for the new cards instead of the old set's pages.
	relight.begin_record_pass();
	RendererRD::NextGIInstanceRecord a2;
	a2.card_set = 5;
	a2.tlas_index = 1;
	const uint32_t a2_table = relight.assign_card_pages(lite_test_instance(0), a2, UINT32_MAX, 9, cards, 3);
	CHECK_EQ(a2_table, 3u);
	relight.collect_current_page_sets(current);
	REQUIRE_EQ(current.size(), 1u);
	const RendererRD::NextGIRelight::PageSetInfo a2_info = relight.get_page_set_info(current[0]);
	CHECK_EQ(a2_info.card_set, 5u);
	CHECK_EQ(a2_info.page_table, 3u);
	CHECK_EQ(a2_info.radiance_rects.size(), 3u);
	CHECK_EQ(relight.get_page_set_count(), 2u);
}

// GodotNext: nextgi_async_counters (#186)
TEST_CASE("[NextGI] Relight ray fractions land on the scheduled cards by page key") {
	// The traced-ray count arrives frames after the schedule. Meanwhile an earlier page set can be
	// freed, which shifts every later set's index: the fraction must follow the page key.
	RendererRD::NextGIRelight relight;
	relight.set_page_extent(RendererRD::nextgi_src_atlas_budget(false, true));
	const RendererRD::NextGICard cards[3] = { lite_test_card(64), lite_test_card(32), lite_test_card(16) };
	relight.begin_record_pass();
	RendererRD::NextGIInstanceRecord a;
	a.card_set = 0;
	a.tlas_index = 0;
	RendererRD::NextGIInstanceRecord b;
	b.card_set = 1;
	b.tlas_index = 1;
	REQUIRE_EQ(relight.assign_card_pages(lite_test_instance(0), a, UINT32_MAX, 0, cards, 2), 0u);
	REQUIRE_EQ(relight.assign_card_pages(lite_test_instance(1), b, UINT32_MAX, 2, cards, 1), 2u);

	LocalVector<RendererRD::NextGIRelightRayRequest> schedule;
	RendererRD::NextGIRelightRayRequest request;
	request.page_key = RendererRD::NextGIRelight::get_page_key(lite_test_instance(1));
	request.card = 0;
	request.ray_estimate = 300;
	schedule.push_back(request);
	request.page_key = RendererRD::NextGIRelight::get_page_key(lite_test_instance(0));
	request.card = 1;
	request.ray_estimate = 100;
	schedule.push_back(request);

	// A's set leaves before the count arrives; B's set moves from index 1 to index 0.
	relight.release_instance(lite_test_instance(0));
	REQUIRE_EQ(relight.get_page_set_count(), 1u);
	CHECK_EQ(relight.get_page_set_info(0).card_set, 1u);
	relight.apply_ray_fraction(schedule, 200);
	CHECK(relight.get_card_ray_fraction(lite_test_instance(1), UINT32_MAX, 0) == doctest::Approx(0.5f));
	CHECK(relight.get_card_ray_fraction(lite_test_instance(0), UINT32_MAX, 1) < 0.0f);

	// The pure rule: traced / estimated, clamped, and nothing applied without an estimate.
	CHECK(RendererRD::nextgi_relight_ray_fraction(schedule.ptr(), schedule.size(), 1000) == doctest::Approx(1.0f));
	CHECK(RendererRD::nextgi_relight_ray_fraction(schedule.ptr(), schedule.size(), 0) == doctest::Approx(0.0f));
	CHECK(RendererRD::nextgi_relight_ray_fraction(schedule.ptr(), 0, 50) < 0.0f);
	relight.apply_ray_fraction(LocalVector<RendererRD::NextGIRelightRayRequest>(), 50);
	CHECK(relight.get_card_ray_fraction(lite_test_instance(1), UINT32_MAX, 0) == doctest::Approx(0.5f));
}

// GodotNext: nextgi_frame_tier (#186). The helpers' rule only: render_frame()'s AS-builds slot,
// which must report the budget through note_as_memory_budget() rather than change the tier, needs
// a device. swrt_gi_check --check=budget covers that call site on the GPU.
TEST_CASE("[NextGI] Frame tier: an AS budget change applies at the next frame") {
	RendererRD::NextGI next_gi;
	String line;
	next_gi.update_tier(TestNextGI::hwrt_inputs(), RendererRD::NextGI::QUALITY_HIGH, line);
	REQUIRE_EQ(next_gi.get_tier_result().tier, RendererRD::NextGI::TIER_HWRT);
	next_gi.begin_frame(4);
	const uint32_t changes = next_gi.get_frame_tier_changes();
	CHECK_EQ(changes, 1u);

	// Frame 5's AS-builds slot finds AS memory exceeded: the rest of frame 5 stays HWRT.
	next_gi.begin_frame(5);
	next_gi.note_as_memory_budget(false, 5);
	CHECK(next_gi.is_budget_change_pending());
	CHECK_EQ(next_gi.get_tier_result().tier, RendererRD::NextGI::TIER_HWRT);
	next_gi.begin_frame(5);
	CHECK_EQ(next_gi.get_tier_result().tier, RendererRD::NextGI::TIER_HWRT);
	CHECK_EQ(next_gi.get_frame_tier_changes(), changes);

	// Frame 6 starts on the software tier, and the tier-change cleanup runs once.
	next_gi.begin_frame(6);
	CHECK_EQ(next_gi.get_tier_result().tier, RendererRD::NextGI::TIER_SOFTWARE);
	CHECK_EQ(next_gi.get_tier_result().reason, RendererRD::NextGI::AS_MEMORY);
	CHECK_FALSE(next_gi.is_budget_change_pending());
	CHECK_EQ(next_gi.get_frame_tier_changes(), changes + 1);
	next_gi.begin_frame(6);
	CHECK_EQ(next_gi.get_frame_tier_changes(), changes + 1);

	// A budget that recovers before the next frame cancels the pending change.
	RendererRD::NextGI recovering;
	recovering.update_tier(TestNextGI::hwrt_inputs(), RendererRD::NextGI::QUALITY_HIGH, line);
	recovering.begin_frame(10);
	recovering.note_as_memory_budget(false, 10);
	recovering.note_as_memory_budget(true, 10);
	recovering.begin_frame(11);
	CHECK_EQ(recovering.get_tier_result().tier, RendererRD::NextGI::TIER_HWRT);
}

TEST_CASE("[NextGI][Lite] The page rect count covers only uploaded rects") {
	// The record pass assigns page tables before relight() uploads their rects, and relight() can
	// return before the upload (for example while the scene is not traceable). The gather, the WRC and voxel
	// lighting bound their page rect reads by get_page_rect_count(), so it must not count page
	// tables whose rects are not in the buffer yet (a read past the buffer on the GPU).
	RendererRD::NextGIRelight relight;
	relight.set_page_extent(RendererRD::nextgi_src_atlas_budget(false, true));
	const RendererRD::NextGICard cards[2] = { lite_test_card(64), lite_test_card(32) };
	relight.begin_record_pass();
	RendererRD::NextGIInstanceRecord record;
	record.card_set = 0;
	record.tlas_index = 0;
	CHECK_EQ(relight.assign_card_pages(lite_test_instance(0), record, UINT32_MAX, 0, cards, 2), 0u);
	LocalVector<uint32_t> words;
	relight.build_page_rect_words(words);
	CHECK_EQ(words.size(), 4u);
	CHECK_FALSE(relight.get_page_rect_buffer().is_valid());
	CHECK_EQ(relight.get_page_rect_count(), 0u);
	// A new extent drops every page table; the count stays empty until the next upload.
	relight.set_page_extent(RendererRD::nextgi_src_atlas_budget(false, false));
	CHECK_EQ(relight.get_page_rect_count(), 0u);
}

TEST_CASE("[NextGI][Lite] A record whose pages are evicted later in the same pass is marked evicted") {
	RendererRD::NextGIRelight relight;
	// One 4096x2048 Lite atlas holds 128 sets of four 128-texel cards.
	relight.set_page_extent(RendererRD::nextgi_src_atlas_budget(false, true));
	const RendererRD::NextGICard cards[4] = { lite_test_card(128), lite_test_card(128), lite_test_card(128), lite_test_card(128) };
	relight.begin_record_pass();
	LocalVector<RendererRD::NextGIInstanceRecord> records;
	for (uint32_t i = 0; i < 130; i++) {
		RendererRD::NextGIInstanceRecord record;
		record.card_set = i;
		record.tlas_index = i;
		record.page_table = relight.assign_card_pages(lite_test_instance(i), record, UINT32_MAX, i * 4, cards, 4);
		CHECK((record.page_table & 0x80000000u) == 0);
		records.push_back(record);
	}
	// The atlas is full after 128 sets, so sets 128 and 129 evicted the two least recently
	// claimed sets, 0 and 1, whose records already hold page tables.
	LocalVector<uint32_t> current;
	relight.collect_current_page_sets(current);
	CHECK_EQ(current.size(), 128u);
	CHECK((records[0].page_table & 0x80000000u) == 0);
	relight.finish_record_pass(records);
	CHECK_EQ(records[0].page_table, 0x80000000u);
	CHECK_EQ(records[1].page_table, 0x80000000u);
	CHECK((records[2].page_table & 0x80000000u) == 0);
	CHECK((records[129].page_table & 0x80000000u) == 0);
	// The surviving records still name their own pages: the WRC reads them through page_table.
	LocalVector<uint32_t> words;
	relight.build_page_rect_words(words);
	CHECK_EQ(words[records[2].page_table * 2 + 1], 128u | (128u << 16));
	// Set 0 owned stable ids 0-3; their rects are now empty.
	CHECK_EQ(words[0], 0u);
	CHECK_EQ(words[1], 0u);
}

TEST_CASE("[NextGI][SDF] The SDF instance buffer grows geometrically") {
	// NextGI Perf streams in about 70 instances per frame past the first 25000. Growing to the
	// exact count reallocated the buffer every frame, and the SDF stream then freed the uniform
	// set its old buffer had already freed (an "Attempted to free invalid ID" error per frame).
	using Instances = NextGISDFInstances;
	uint32_t capacity = 0;
	uint32_t reallocations = 0;
	for (uint32_t required = 1; required <= Instances::MAX_INSTANCES; required += 72) {
		const uint32_t grown = Instances::grown_capacity(capacity, required);
		CHECK(grown >= required);
		if (grown != capacity) {
			reallocations++;
			capacity = grown;
		}
	}
	CHECK(reallocations <= 4u);
	CHECK_EQ(Instances::grown_capacity(0, 10), Instances::MAX_INSTANCES / 4);
	CHECK_EQ(Instances::grown_capacity(Instances::MAX_INSTANCES / 4, Instances::MAX_INSTANCES / 4 + 1), Instances::MAX_INSTANCES / 2);
	CHECK_EQ(Instances::grown_capacity(Instances::MAX_INSTANCES, Instances::MAX_INSTANCES), Instances::MAX_INSTANCES);
	CHECK_EQ(Instances::grown_capacity(Instances::MAX_INSTANCES / 2 + 1, Instances::MAX_INSTANCES), Instances::MAX_INSTANCES);
}

TEST_CASE("[NextGI][Lite] Params reconstruct world positions and snap to the WRC grid") {
	Projection projection;
	projection.set_perspective(70.0, 16.0 / 9.0, 0.05, 400.0);
	// Pitch, yaw and roll: no axis of the camera lines up with a world axis.
	const Transform3D camera = Transform3D(Basis::from_euler(Vector3(-0.3, 0.7, 0.4)), Vector3(3.4, 1.7, -12.6));
	const Size2i gi_size(1280, 720);
	const bool wrc_field = true;
	const RendererRD::NextGIIrradianceField::Params params = RendererRD::NextGIIrradianceField::build_params(projection, camera, true, Vector2(), gi_size, gi_size / 2, gi_size, true, true, wrc_field, camera.origin);

	// The scene UBO draws depth with this projection (RenderSceneDataRD::update_ubo).
	Projection correction;
	correction.set_depth_correction(true);
	const Projection drawn = correction * projection * Projection(camera.affine_inverse());
	Projection inverse;
	for (int column = 0; column < 4; column++) {
		for (int row = 0; row < 4; row++) {
			inverse.columns[column][row] = params.inv_view_proj[column * 4 + row];
		}
	}
	const Vector3 forward = -camera.basis.get_column(2);
	const Vector3 right = camera.basis.get_column(0);
	const Vector3 up = camera.basis.get_column(1);
	const Vector3 points[] = { camera.origin + forward * 2.0f, camera.origin + forward * 5.0f + right * 1.0f, camera.origin + forward * 20.0f - up * 3.0f + right * 4.0f };
	for (const Vector3 &point : points) {
		const Vector4 clip = drawn.xform(Vector4(point.x, point.y, point.z, 1.0f));
		const Vector3 ndc = Vector3(clip.x, clip.y, clip.z) / clip.w;
		// Reverse-Z: a visible point has depth in (0, 1], larger when closer.
		CHECK(ndc.z > 0.0f);
		CHECK(ndc.z <= 1.0f);
		// nextgi_irradiance_field.glsl lite_unproject(): vec4(uv * 2 - 1, depth, 1) with uv = ndc * 0.5 + 0.5.
		const Vector4 world_h = inverse.xform(Vector4(ndc.x, ndc.y, ndc.z, 1.0f));
		const Vector3 world = Vector3(world_h.x, world_h.y, world_h.z) / world_h.w;
		CHECK_MESSAGE(world.distance_to(point) < 1e-3f * MAX(1.0f, point.distance_to(camera.origin)), vformat("%s reconstructs as %s.", point, world));
	}
	// The top of the image (uv.y = 0) is the camera's up side: flip_y matches the GI buffers.
	const Vector4 top = drawn.xform(Vector4(points[0].x + up.x, points[0].y + up.y, points[0].z + up.z, 1.0f));
	CHECK(top.y / top.w < 0.0f);

	for (uint32_t level = 0; level < RendererRD::NextGI::LITE_PROBE_LEVELS; level++) {
		const float spacing = RendererRD::nextgi_wrc_spacing(level);
		const Vector3 origin = RendererRD::nextgi_wrc_snap_origin(camera.origin, spacing);
		CHECK(Vector3(params.level_origin[level][0], params.level_origin[level][1], params.level_origin[level][2]).is_equal_approx(origin));
		CHECK(params.level_origin[level][3] == doctest::Approx(spacing));
	}
	CHECK(params.level_origin[1][3] == doctest::Approx(2.0f));
	CHECK(params.rules[0] == doctest::Approx(RendererRD::NextGI::LITE_INSIDE_BACKFACE));
	CHECK(params.rules[1] == doctest::Approx(RendererRD::NextGI::LITE_PUSH_SPACING));
	CHECK(params.rules[2] == doctest::Approx(RendererRD::NextGI::LITE_SSR_ROUGHNESS));
	CHECK(params.rules[3] == doctest::Approx(RendererRD::NextGI::LITE_MIP2_ROUGHNESS));
	CHECK(params.screen[2] == doctest::Approx(640.0f));

	// The prepass writes view-space normals (the forward shader encodes modelview_normal, the
	// world normal times the view matrix). The shader's nextgi_view_world_normal() multiplies
	// them by the view_to_world columns, which must give the world normal back for any camera
	// rotation.
	const Basis view_to_world(Vector3(params.view_to_world[0], params.view_to_world[1], params.view_to_world[2]),
			Vector3(params.view_to_world[4], params.view_to_world[5], params.view_to_world[6]),
			Vector3(params.view_to_world[8], params.view_to_world[9], params.view_to_world[10]));
	const Basis view_matrix = camera.affine_inverse().basis;
	const Vector3 world_normals[] = { Vector3(0.0f, 1.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f), Vector3(0.3f, -0.5f, 0.81f).normalized() };
	for (const Vector3 &world_normal : world_normals) {
		const Vector3 view_normal = view_matrix.xform(world_normal);
		// A view-space normal is not a world-space one for this camera.
		CHECK(view_normal.dot(world_normal) < 0.99f);
		CHECK_MESSAGE(view_to_world.xform(view_normal).is_equal_approx(world_normal), vformat("%s decodes as %s.", world_normal, view_to_world.xform(view_normal)));
	}
}

TEST_CASE("[NextGI][Lite] p_wrc_field sets camera.w exactly when the WRC field is bound") {
	Projection projection;
	projection.set_perspective(70.0, 16.0 / 9.0, 0.05, 400.0);
	const Transform3D camera = Transform3D(Basis::from_euler(Vector3(-0.3, 0.7, 0.4)), Vector3(3.4, 1.7, -12.6));
	const Size2i gi_size(1280, 720);
	// nextgi_irradiance_field.glsl reads the WRC moments, offsets and mips only when camera.w > 0.5.
	for (const bool wrc_field : { true, false }) {
		for (const bool normal_valid : { true, false }) {
			const RendererRD::NextGIIrradianceField::Params params = RendererRD::NextGIIrradianceField::build_params(projection, camera, true, Vector2(), gi_size, gi_size / 2, gi_size, true, normal_valid, wrc_field, camera.origin);
			CHECK_MESSAGE(params.camera[3] == doctest::Approx(wrc_field ? 1.0f : 0.0f), vformat("wrc_field %s, normal_valid %s: camera.w is %f.", wrc_field, normal_valid, params.camera[3]));
			// The flag is camera.w alone: the normal flag and the camera position stay as given.
			CHECK(params.depth_info[3] == doctest::Approx(normal_valid ? 1.0f : 0.0f));
			CHECK(Vector3(params.camera[0], params.camera[1], params.camera[2]).is_equal_approx(camera.origin));
		}
	}
}

TEST_CASE("[NextGI][Lite] A secondary view reads the probes on the WRC's grid") {
	// The WRC updates once per frame, from the frame's first view, and the Lite pack copies its
	// probes by grid cell. A second view 1.4 m away snaps to other level origins, so it must
	// take the WRC's grid camera or every probe it reads sits one cell off.
	Projection projection;
	projection.set_perspective(60.0, 16.0 / 9.0, 0.05, 400.0);
	const Vector3 first_view(-0.7f, 1.2f, 0.3f);
	const Transform3D second_view(Basis::from_euler(Vector3(-1.2, 0.4, 0.0)), Vector3(0.7f, 1.2f, 0.3f));
	const Size2i gi_size(960, 540);
	const RendererRD::NextGIIrradianceField::Params params = RendererRD::NextGIIrradianceField::build_params(projection, second_view, true, Vector2(), gi_size, gi_size / 2, gi_size, true, true, true, first_view);
	CHECK(Vector3(params.camera[0], params.camera[1], params.camera[2]).is_equal_approx(second_view.origin));
	for (uint32_t level = 0; level < RendererRD::NextGI::LITE_PROBE_LEVELS; level++) {
		const float spacing = RendererRD::nextgi_wrc_spacing(level);
		// Precondition: the two cameras snap to different origins on this level.
		CHECK_FALSE(RendererRD::nextgi_wrc_snap_origin(first_view, spacing).is_equal_approx(RendererRD::nextgi_wrc_snap_origin(second_view.origin, spacing)));
		const Vector3 origin(params.level_origin[level][0], params.level_origin[level][1], params.level_origin[level][2]);
		// nextgi_wrc_update_inc.glsl: the level-0 origin snaps the camera to 1 m, each level
		// snaps that origin to its own spacing, and probe (x, y, z) sits at
		// origin + (cell - 7.5) * spacing. Lite's lite_probe_position() uses the same formula.
		const Vector3 origin0 = RendererRD::nextgi_wrc_snap_origin(first_view, 1.0f);
		const Vector3 wrc_origin = Vector3(Math::floor(origin0.x / spacing), Math::floor(origin0.y / spacing), Math::floor(origin0.z / spacing)) * spacing;
		CHECK_MESSAGE(origin.is_equal_approx(wrc_origin), vformat("level %d: Lite's grid origin %s, the WRC's %s.", level, origin, wrc_origin));
		CHECK(params.level_origin[level][3] == doctest::Approx(spacing));
	}
}

// GodotNext: nextgi_multiview (#58).
TEST_CASE("[NextGI][MultiView] Anchor selection and scenario scheduling") {
	using Candidate = RendererRD::NextGI::ViewAnchorCandidate;
	Candidate views[4];
	views[0].valid = true;
	views[0].area = 900;
	views[0].order = 0;
	views[1].valid = true;
	views[1].screen_attached = true;
	views[1].area = 100;
	views[1].order = 1;
	views[2].valid = true;
	views[2].editor_hint = true;
	views[2].area = 500;
	views[2].order = 2;
	views[3].valid = false;
	views[3].area = 1000;

	// A screen-attached viewport wins over a larger one.
	CHECK(RendererRD::NextGI::nextgi_multiview_choose_anchor(views, 4, -1) == 1);
	// Last frame's anchor is kept while it still renders.
	CHECK(RendererRD::NextGI::nextgi_multiview_choose_anchor(views, 4, 0) == 0);
	views[1].screen_attached = false;
	// Without an attached viewport the editor hint wins.
	CHECK(RendererRD::NextGI::nextgi_multiview_choose_anchor(views, 4, -1) == 2);
	views[2].editor_hint = false;
	// Otherwise the largest area wins.
	CHECK(RendererRD::NextGI::nextgi_multiview_choose_anchor(views, 4, -1) == 0);
	views[0].valid = false;
	// With neither, the largest remaining area wins.
	CHECK(RendererRD::NextGI::nextgi_multiview_choose_anchor(views, 4, -1) == 2);
	// A stale previous anchor is replaced.
	CHECK(RendererRD::NextGI::nextgi_multiview_choose_anchor(views, 4, 0) == 2);
	// The scenario runs once per frame.
	CHECK_FALSE(RendererRD::NextGI::nextgi_multiview_should_run_scenario(7, 7));
	CHECK(RendererRD::NextGI::nextgi_multiview_should_run_scenario(6, 7));
	// Secondary views get their own timestamp names.
	CHECK(RendererRD::NextGI::nextgi_multiview_pass_name("NextGI: Diffuse gather", 0, false) == "NextGI: Diffuse gather");
	CHECK(RendererRD::NextGI::nextgi_multiview_pass_name("NextGI: Diffuse gather", 1, true) == "NextGI:View1:Diffuse gather");
	CHECK(RendererRD::NextGI::nextgi_multiview_pass_name("> NextGI", 2, true) == "NextGI:View2:> NextGI");
}

// GodotNext: nextgi_alpha_policy (#61).
#include "scene/resources/material.h"

TEST_CASE("[NextGI][Alpha] BaseMaterial3D nextgi_rt_treat_as_opaque round trip") {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	CHECK_FALSE(material->get_flag(BaseMaterial3D::FLAG_NEXTGI_RT_OPAQUE));
	material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR);
	material->set_flag(BaseMaterial3D::FLAG_NEXTGI_RT_OPAQUE, true);
	CHECK(material->get_flag(BaseMaterial3D::FLAG_NEXTGI_RT_OPAQUE));
	material->set_flag(BaseMaterial3D::FLAG_NEXTGI_RT_OPAQUE, false);
	CHECK_FALSE(material->get_flag(BaseMaterial3D::FLAG_NEXTGI_RT_OPAQUE));
}

TEST_CASE("[NextGI][NextGeo] VGD proxy planner packs Stage-2 group tables, outside-mask surfaces, and frees cleanly") {
	using namespace RendererRD;

	RenderingServerTypes::VirtualGeometryDesc desc;
	desc.eligible_surface_mask = 1u; // surface 0 virtualized, surface 1 outside mask, surface 2 alpha-excluded
	desc.rt_proxy_positions = PackedFloat32Array({
			0.0f,
			0.0f,
			0.0f,
			1.0f,
			0.0f,
			0.0f,
			0.0f,
			1.0f,
			0.0f,
	});
	desc.rt_proxy_indices = PackedByteArray({
			0,
			0,
			0,
			0,
			1,
			0,
			0,
			0,
			2,
			0,
			0,
			0,
	});
	desc.rt_proxy_parts = PackedInt32Array({ 0, 3, 0, 3 });
	// 3 surfaces in VGD ranges: surface 0 has [0, 3], surface 1 has [0, 0], surface 2 has [0, 3] (alpha-excluded).
	desc.rt_proxy_surface_ranges = PackedInt32Array({ 0, 3, 0, 0, 0, 3 });
	desc.rt_proxy_triangle_groups = PackedByteArray({ 5, 0 });
	desc.card_descriptors.resize(32);
	desc.card_descriptors.fill(0);
	desc.rt_proxy_eps = 0.01f;

	const float leg_pos[9] = { 2.0f, 0.0f, 0.0f, 3.0f, 0.0f, 0.0f, 2.0f, 1.0f, 0.0f };
	const uint32_t leg_idx[3] = { 0, 1, 2 };
	NextGIRTLegacySurfaceInput leg_surfaces[3];
	leg_surfaces[1].positions = leg_pos;
	leg_surfaces[1].vertex_count = 3;
	leg_surfaces[1].indices = leg_idx;
	leg_surfaces[1].index_count = 3;
	leg_surfaces[1].eps = 0.02f;

	const uint8_t alpha_flags[3] = { 0, 0, 1 };

	NextGIArenaAllocator allocator(64 * 1024 * 1024);
	LocalVector<NextGIRTProxyPartUpload> uploads;
	uint32_t alpha_excluded = 0;

	REQUIRE(nextgi_rt_plan_vgd_proxy(true, true, 1, desc, 3, alpha_flags, leg_surfaces, allocator, uploads, alpha_excluded));
	CHECK_EQ(alpha_excluded, 1u);
	REQUIRE_EQ(uploads.size(), 1u);
	// Part 0 combines virtualized surface 0 (Stage-2 cards) and outside-mask legacy surface 1 (no cards).
	REQUIRE_EQ(uploads[0].geometries.size(), 2u);
	CHECK_EQ(uploads[0].geometries[0].surface, 0u);
	CHECK(uploads[0].geometries[0].has_stage2_cards);
	CHECK_EQ(uploads[0].geometries[0].group_offset & 15u, 0u);
	CHECK_EQ(uploads[0].geometries[1].surface, 1u);
	CHECK_FALSE(uploads[0].geometries[1].has_stage2_cards);
	CHECK_EQ(uploads[0].triangle_count, 2u);
	CHECK_GT(allocator.get_allocated_bytes(), uint64_t(0));

	for (const NextGIRTProxyPartUpload &u : uploads) {
		allocator.free_range(u.allocation);
	}
	CHECK_EQ(allocator.get_allocated_bytes(), uint64_t(0));
}

#endif // RD_ENABLED

TEST_CASE("[NextGI] Max roughness to trace caps the quality preset") {
	using namespace RendererRD;
	CHECK(Math::is_equal_approx(NextGI::nextgi_effective_max_roughness(0.4f, 0.4f), 0.4f));
	// Medium keeps its 0.25 even with the default Environment value.
	CHECK(Math::is_equal_approx(NextGI::nextgi_effective_max_roughness(0.25f, 0.4f), 0.25f));
	// A lower Environment value refines any preset.
	CHECK(Math::is_equal_approx(NextGI::nextgi_effective_max_roughness(0.4f, 0.2f), 0.2f));
	// The cap never goes above the preset.
	CHECK(Math::is_equal_approx(NextGI::nextgi_effective_max_roughness(0.4f, 2.0f), 0.4f));
	CHECK(Math::is_equal_approx(NextGI::nextgi_effective_max_roughness(0.4f, -1.0f), 0.0f));
}

TEST_CASE("[NextGI] Reflections-only mode resolves from the setting") {
	using namespace RendererRD;
	CHECK(NextGI::nextgi_mode_from_setting(0) == NextGI::MODE_GI_AND_REFLECTIONS);
	CHECK(NextGI::nextgi_mode_from_setting(1) == NextGI::MODE_REFLECTIONS_ONLY);
	CHECK(NextGI::nextgi_mode_from_setting(99) == NextGI::MODE_GI_AND_REFLECTIONS);
	CHECK_EQ(String(NextGI::get_mode_name(NextGI::MODE_GI_AND_REFLECTIONS)), String("gi_and_reflections"));
	CHECK_EQ(String(NextGI::get_mode_name(NextGI::MODE_REFLECTIONS_ONLY)), String("reflections_only"));
}

TEST_CASE("[NextGI] Reflections setting is readable") {
	using namespace RendererRD;
	// The default enables the specular trace; the reflections-off fallback is
	// covered by the nextgi_mode testbed scene.
	CHECK(NextGI::is_reflections_enabled());
}

TEST_CASE("[NextGI] Rough reflection filtering follows roughness") {
	using namespace RendererRD;
	CHECK(Math::is_equal_approx(NextGI::nextgi_roughness_to_lod(0.0f), 0.0f));
	CHECK(Math::is_equal_approx(NextGI::nextgi_roughness_to_lod(0.5f), 1.5f));
	CHECK(Math::is_equal_approx(NextGI::nextgi_roughness_to_lod(1.0f), 6.0f));
	CHECK(Math::is_equal_approx(NextGI::nextgi_roughness_to_lod(-1.0f), 0.0f));
	CHECK(NextGI::is_reflections_enabled());
}

TEST_CASE("[NextGI] Translucency probe mapping and temporal history") {
	CHECK_EQ(RendererRD::nextgi_translucency_probe_index(Vector3(0.5, 0.5, 0.5), Vector3(), Vector3()), 2184);
	CHECK_EQ(RendererRD::nextgi_translucency_probe_index(Vector3(9, 0, 0), Vector3(), Vector3()), 6284);
	CHECK_EQ(RendererRD::nextgi_translucency_probe_index(Vector3(17, 0, 0), Vector3(), Vector3()), -1);
	CHECK_EQ(RendererRD::nextgi_translucency_probe_index(Vector3(-15, 0, 0), Vector3(), Vector3()), 6272);
	CHECK_EQ(RendererRD::nextgi_translucency_probe_index(Vector3(4, 4, 4), Vector3(), Vector3()), 3276);
	CHECK_EQ(RendererRD::nextgi_translucency_volume_level(8.0f, true), 1u);
	CHECK_EQ(RendererRD::nextgi_translucency_volume_level(32.0f, true), 3u);
	CHECK_EQ(RendererRD::nextgi_translucency_volume_level(8.0f, false), 0u);
	CHECK(Math::is_equal_approx(RendererRD::nextgi_translucency_history_weight(true, Vector2(0.5, 0.5)), 0.9f));
	CHECK(Math::is_equal_approx(RendererRD::nextgi_translucency_history_weight(false, Vector2(0.5, 0.5)), 0.0f));
	CHECK(Math::is_equal_approx(RendererRD::nextgi_translucency_history_weight(true, Vector2(-0.1, 0.5)), 0.0f));
	CHECK(Math::is_equal_approx(RendererRD::nextgi_translucency_history_weight(true, Vector2(0.5, 1.1)), 0.0f));
}
