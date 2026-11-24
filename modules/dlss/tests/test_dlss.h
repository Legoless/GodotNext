/**************************************************************************/
/*  test_dlss.h                                                           */
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

#pragma once

#include "tests/test_macros.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/latency_markers.h"
#include "core/os/os.h"
#include "core/string/ustring.h"
#include "core/version.h"
#include "servers/rendering/renderer_rd/effects/denoiser_bypass.h"
#include "servers/rendering/renderer_rd/effects/temporal_upscaler.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/rendering_context_driver.h"
#include "servers/rendering/storage/frame_generation_extension.h"

#include "modules/dlss/dlss_backend_mock.h"
#include "modules/dlss/dlss_frame_generation.h"
#include "modules/dlss/dlss_ngx_d3d12.h"
#include "modules/dlss/dlss_streamline_d3d12.h"
#include "modules/dlss/dlss_upscaler.h"
#include "modules/dlss/register_types.h"

#ifdef TOOLS_ENABLED
#include "editor/gui/editor_about.h"
#include "modules/dlss/editor/dlss_editor_plugin.h"
#include "modules/dlss/editor/dlss_export_plugin.h"
#endif

namespace TestDLSS {

DLSSBackend::InitParams make_test_init_params(const String &p_project_id);
bool is_ngx_backend_compiled();

class MockFrameGenOps : public DLSSFrameGenBackendOps {
public:
	String api_name = "D3D12";
	String vendor_name = "NVIDIA Corporation";
	bool streamline_loaded = true;
	bool streamline_init_failed = false;
	bool composition_swapchain = false;
	bool scene_tree_paused = false;
	bool dynamic_resolution = false;
	bool feature_supported = true;
	String feature_result_name = "eOk";

	StateSnapshot next_state;
	uint32_t set_constants_count = 0;
	ConstantsSnapshot last_constants;
	uint32_t set_tags_count = 0;
	Vector<TagSnapshot> last_tags;
	uint32_t set_options_count = 0;
	Vector<OptionsSnapshot> sent_options;
	bool notified_fg_active = false;
	bool notified_fg_dynamic = false;

	MockFrameGenOps() {
		next_state.ok = true;
		next_state.status_code = 0;
		next_state.status_name = "eOk";
		next_state.num_frames_presented = 2;
		next_state.num_frames_to_generate_max = 1;
		next_state.dynamic_mfg_supported = false;
		next_state.estimated_vram_bytes = 96u * 1024u * 1024u;
	}

	String get_api_name() const override { return api_name; }
	String get_vendor_name() const override { return vendor_name; }
	bool is_streamline_loaded() const override { return streamline_loaded; }
	bool is_streamline_init_failed() const override { return streamline_init_failed; }
	bool is_composition_swapchain() const override { return composition_swapchain; }
	bool is_scene_tree_paused() const override { return scene_tree_paused; }
	bool is_dynamic_resolution_enabled() const override { return dynamic_resolution; }

	bool query_feature_supported(String &r_result_name) const override {
		r_result_name = feature_result_name;
		return feature_supported;
	}

	bool set_constants(uint64_t p_frame_index, const ConstantsSnapshot &p_consts) override {
		(void)p_frame_index;
		set_constants_count++;
		last_constants = p_consts;
		return true;
	}

	bool set_tags(uint64_t p_frame_index, const Vector<TagSnapshot> &p_tags, uint64_t p_cmd_list_handle) override {
		(void)p_frame_index;
		(void)p_cmd_list_handle;
		set_tags_count++;
		last_tags = p_tags;
		return true;
	}

	bool set_options(const OptionsSnapshot &p_opts) override {
		set_options_count++;
		sent_options.push_back(p_opts);
		return true;
	}

	bool get_state(const OptionsSnapshot *p_opts, StateSnapshot &r_state) override {
		(void)p_opts;
		r_state = next_state;
		return next_state.ok;
	}

	void notify_fg_active(bool p_active, bool p_dynamic) override {
		notified_fg_active = p_active;
		notified_fg_dynamic = p_dynamic;
	}
};

TEST_CASE("[DLSS][Mock] Module version config carries .dlss and active backend exists") {
	const String mod_cfg = String(GODOT_VERSION_MODULE_CONFIG);
	CHECK(mod_cfg.contains(".dlss"));

	DLSSBackend *active = dlss_get_active_backend();
	REQUIRE(active != nullptr);
	if (is_ngx_backend_compiled()) {
		CHECK(active->get_backend_name() == "ngx");
	} else {
		CHECK(active->get_backend_name() == "mock");
	}
}

TEST_CASE("[DLSS][Mock] Every status code and error code can be scripted") {
	DLSSBackendMock mock;

	// Empty project_id and zero application_id returns PROJECT_ID_MISSING without incrementing init count.
	DLSSBackend::InitParams empty_id = make_test_init_params("");
	CHECK(mock.init(empty_id) == DLSSBackend::PROJECT_ID_MISSING);
	CHECK(mock.get_init_call_count() == 0);

	const DLSSBackend::Status all_statuses[] = {
		DLSSBackend::OK,
		DLSSBackend::NOT_NVIDIA_RTX,
		DLSSBackend::DRIVER_TOO_OLD,
		DLSSBackend::RUNTIME_MISSING,
		DLSSBackend::EXTENSION_MISSING,
		DLSSBackend::PROJECT_ID_MISSING,
		DLSSBackend::FEATURE_CREATE_FAILED,
	};

	for (DLSSBackend::Status st : all_statuses) {
		mock.reset_script();
		mock.set_scripted_init_status(st);
		CHECK(mock.init(make_test_init_params("a0f57b54-1daf-4934-90ae-c4035c19df04")) == st);
		CHECK(mock.get_init_call_count() == 1);
	}

	mock.reset_script();
	mock.set_scripted_result_code(static_cast<int32_t>(0xBAD00001u));
	CHECK(mock.get_last_result_code() == static_cast<int32_t>(0xBAD00001u));

	// Vendor and extension checks in query_availability.
	CHECK(mock.query_availability(0x1002, true) == DLSSBackend::NOT_NVIDIA_RTX);
	CHECK(mock.query_availability(0x8086, true) == DLSSBackend::NOT_NVIDIA_RTX);
	CHECK(mock.query_availability(0x10DE, false) == DLSSBackend::EXTENSION_MISSING);
	mock.set_scripted_availability_status(DLSSBackend::DRIVER_TOO_OLD);
	CHECK(mock.query_availability(0x10DE, true) == DLSSBackend::DRIVER_TOO_OLD);
	mock.set_scripted_availability_status(DLSSBackend::RUNTIME_MISSING);
	CHECK(mock.query_availability(0x10DE, true) == DLSSBackend::RUNTIME_MISSING);
	mock.set_scripted_availability_status(DLSSBackend::OK);
	CHECK(mock.query_availability(0x10DE, true) == DLSSBackend::OK);
}

TEST_CASE("[DLSS][Mock] Extension lists, feature lifecycle, memory, and evaluate bookkeeping") {
	DLSSBackendMock mock;
	Vector<CharString> inst_exts;
	inst_exts.push_back(CharString("VK_KHR_get_physical_device_properties2"));
	inst_exts.push_back(CharString("VK_KHR_external_memory_capabilities"));
	Vector<CharString> dev_exts;
	dev_exts.push_back(CharString("VK_NVX_binary_import"));
	dev_exts.push_back(CharString("VK_KHR_push_descriptor"));

	mock.set_scripted_instance_extensions(inst_exts);
	mock.set_scripted_device_extensions(dev_exts);
	CHECK(mock.get_instance_extensions().size() == 2);
	CHECK(String(mock.get_instance_extensions()[0].get_data()) == "VK_KHR_get_physical_device_properties2");
	CHECK(mock.get_device_extensions(1, 2).size() == 2);
	CHECK(String(mock.get_device_extensions(1, 2)[0].get_data()) == "VK_NVX_binary_import");

	CHECK(mock.init(make_test_init_params("a0f57b54-1daf-4934-90ae-c4035c19df04")) == DLSSBackend::OK);
	CHECK(mock.is_initialized());
	CHECK(mock.get_memory_bytes() == 0);

	DLSSBackend::FeatureCreateParams create_params;
	create_params.render_size = Size2i(1280, 720);
	create_params.output_size = Size2i(1920, 1080);
	create_params.quality_mode = DLSSBackend::QUALITY_MAX_QUALITY;
	create_params.preset = DLSSBackend::PRESET_K;

	uint64_t handle = 0;
	CHECK(mock.create_feature(create_params, handle) == DLSSBackend::OK);
	CHECK(handle != 0);
	CHECK(mock.get_active_feature_count() == 1);
	CHECK(mock.get_memory_bytes() == 64u * 1024u * 1024u);

	DLSSBackend::EvaluateParams eval_params;
	eval_params.feature_handle = handle;
	eval_params.render_size = Size2i(1280, 720);
	eval_params.output_size = Size2i(1920, 1080);
	eval_params.jitter_offset = Vector2(0.25f, -0.125f);
	eval_params.mv_scale = Vector2(1280.0f, 720.0f);
	eval_params.pre_exposure = 1.0f;
	eval_params.reset = true;

	CHECK(mock.evaluate(eval_params) == DLSSBackend::OK);
	REQUIRE(mock.get_evaluate_records().size() == 1);
	CHECK(mock.get_evaluate_records()[0].feature_handle == handle);
	CHECK(mock.get_evaluate_records()[0].render_size == Size2i(1280, 720));
	CHECK(mock.get_evaluate_records()[0].output_size == Size2i(1920, 1080));
	CHECK(mock.get_evaluate_records()[0].reset == true);

	mock.release_feature(handle);
	CHECK(mock.get_active_feature_count() == 0);
	CHECK(mock.get_memory_bytes() == 0);
	CHECK(mock.evaluate(eval_params) == DLSSBackend::FEATURE_CREATE_FAILED);

	mock.shutdown();
	CHECK(!mock.is_initialized());
	CHECK(mock.get_shutdown_call_count() == 1);
}

TEST_CASE("[DLSS][Mock] Scale snapping, preset mapping, and status-to-reason mapping") {
	CHECK(DLSSUpscaler::snap_scale_to_quality(1.00f) == DLSSBackend::QUALITY_DLAA);
	CHECK(DLSSUpscaler::snap_scale_to_quality(0.99f) == DLSSBackend::QUALITY_DLAA);
	CHECK(DLSSUpscaler::snap_scale_to_quality(0.67f) == DLSSBackend::QUALITY_MAX_QUALITY);
	CHECK(DLSSUpscaler::snap_scale_to_quality(0.58f) == DLSSBackend::QUALITY_BALANCED);
	CHECK(DLSSUpscaler::snap_scale_to_quality(0.50f) == DLSSBackend::QUALITY_MAX_PERFORMANCE);
	CHECK(DLSSUpscaler::snap_scale_to_quality(0.33f) == DLSSBackend::QUALITY_ULTRA_PERFORMANCE);

	CHECK(DLSSUpscaler::preset_from_setting(0) == DLSSBackend::PRESET_DEFAULT);
	CHECK(DLSSUpscaler::preset_from_setting(1) == DLSSBackend::PRESET_J);
	CHECK(DLSSUpscaler::preset_from_setting(2) == DLSSBackend::PRESET_K);
	CHECK(DLSSUpscaler::preset_from_setting(3) == DLSSBackend::PRESET_L);
	CHECK(DLSSUpscaler::preset_from_setting(4) == DLSSBackend::PRESET_M);

	CHECK(DLSSUpscaler::rr_preset_from_setting(0) == DLSSBackend::RR_PRESET_DEFAULT);
	CHECK(DLSSUpscaler::rr_preset_from_setting(1) == DLSSBackend::RR_PRESET_D);
	CHECK(DLSSUpscaler::rr_preset_from_setting(2) == DLSSBackend::RR_PRESET_E);
	CHECK(DLSSUpscaler::rr_preset_from_setting(3) == DLSSBackend::RR_PRESET_F);

	CHECK(DLSSUpscaler::map_status_to_reason(DLSSBackend::OK) == RSE::VIEWPORT_SCALING_3D_REASON_NONE);
	CHECK(DLSSUpscaler::map_status_to_reason(DLSSBackend::NOT_NVIDIA_RTX) == RSE::VIEWPORT_SCALING_3D_REASON_NOT_NVIDIA_RTX);
	CHECK(DLSSUpscaler::map_status_to_reason(DLSSBackend::DRIVER_TOO_OLD) == RSE::VIEWPORT_SCALING_3D_REASON_DRIVER_TOO_OLD);
	CHECK(DLSSUpscaler::map_status_to_reason(DLSSBackend::RUNTIME_MISSING) == RSE::VIEWPORT_SCALING_3D_REASON_RUNTIME_MISSING);
	CHECK(DLSSUpscaler::map_status_to_reason(DLSSBackend::EXTENSION_MISSING) == RSE::VIEWPORT_SCALING_3D_REASON_EXTENSION_MISSING);
	CHECK(DLSSUpscaler::map_status_to_reason(DLSSBackend::PROJECT_ID_MISSING) == RSE::VIEWPORT_SCALING_3D_REASON_PROJECT_ID_MISSING);
	CHECK(DLSSUpscaler::map_status_to_reason(DLSSBackend::FEATURE_CREATE_FAILED) == RSE::VIEWPORT_SCALING_3D_REASON_FEATURE_CREATE_FAILED);
}

TEST_CASE("[DLSS][Mock] Upscaler properties, non-NVIDIA capability check, FEATURE_CREATE_FAILED latch, and per-view features") {
	DLSSBackendMock mock;
	DLSSUpscaler upscaler(&mock);

	CHECK(upscaler.get_display_name() == "NVIDIA DLSS (Temporal - NVIDIA RTX only)");
	CHECK(upscaler.get_scale_range() == Vector2(0.30f, 1.0f));
	CHECK(upscaler.velocity_mode() == TemporalUpscaler::VELOCITY_DENSE);
	CHECK(upscaler.mip_bias_offset() == doctest::Approx(-1.0f));
	CHECK(upscaler.supports_api("Vulkan"));
	CHECK(!upscaler.supports_api("Metal"));
	CHECK(!upscaler.supports_multiview());

	// GU-3 non-NVIDIA mocked-capability check (§23.2): AMD/Intel vendor ID maps to NOT_NVIDIA_RTX.
	const DLSSBackend::Status non_nv = mock.query_availability(0x8086, true);
	CHECK(non_nv == DLSSBackend::NOT_NVIDIA_RTX);
	const RSE::ViewportScaling3DReason resolved = nextgi_resolve_dlss_reason(
			true,
			true,
			DLSSUpscaler::map_status_to_reason(non_nv),
			0.67f,
			1,
			1920,
			1080);
	CHECK(resolved == RSE::VIEWPORT_SCALING_3D_REASON_NOT_NVIDIA_RTX);

	// One feature per (RenderSceneBuffersRD*, view) + deferred retirement after frame_delay frames.
	CHECK(mock.init(make_test_init_params("a0f57b54-1daf-4934-90ae-c4035c19df04")) == DLSSBackend::OK);
	RenderSceneBuffersRD *vp_a = reinterpret_cast<RenderSceneBuffersRD *>(uintptr_t(0x1000));
	RenderSceneBuffersRD *vp_b = reinterpret_cast<RenderSceneBuffersRD *>(uintptr_t(0x2000));

	uint64_t h_a0 = 0, h_a1 = 0, h_b0 = 0, h_a0_again = 0, h_a0_resized = 0;
	CHECK(upscaler.test_ensure_feature_for_view(vp_a, 0, Size2i(1280, 720), Size2i(1920, 1080), 0.67f, 2, h_a0) == DLSSBackend::OK);
	CHECK(upscaler.test_ensure_feature_for_view(vp_a, 1, Size2i(1280, 720), Size2i(1920, 1080), 0.67f, 2, h_a1) == DLSSBackend::OK);
	CHECK(upscaler.test_ensure_feature_for_view(vp_b, 0, Size2i(960, 540), Size2i(1920, 1080), 0.50f, 2, h_b0) == DLSSBackend::OK);
	CHECK(h_a0 != 0);
	CHECK(h_a1 != 0);
	CHECK(h_b0 != 0);
	CHECK(h_a0 != h_a1);
	CHECK(h_a0 != h_b0);
	CHECK(upscaler.get_active_feature_count() == 3);

	// Reuse unchanged feature slot.
	CHECK(upscaler.test_ensure_feature_for_view(vp_a, 0, Size2i(1280, 720), Size2i(1920, 1080), 0.67f, 2, h_a0_again) == DLSSBackend::OK);
	CHECK(h_a0_again == h_a0);

	// Reconfigure retires previous handle until frame_delay passes.
	CHECK(upscaler.test_ensure_feature_for_view(vp_a, 0, Size2i(1600, 900), Size2i(2560, 1440), 0.67f, 2, h_a0_resized) == DLSSBackend::OK);
	CHECK(h_a0_resized != h_a0);
	CHECK(upscaler.get_retired_feature_count() == 1);
	upscaler.test_advance_frame(1);
	CHECK(upscaler.get_retired_feature_count() == 1);
	upscaler.test_advance_frame(2);
	CHECK(upscaler.get_retired_feature_count() == 0);

	// Memory report source ("NGX") reports backend memory.
	CHECK(mock.get_memory_bytes() == 3u * 64u * 1024u * 1024u);

	// Failed create latches FEATURE_CREATE_FAILED on that RenderSceneBuffersRD*.
	mock.set_scripted_create_status(DLSSBackend::FEATURE_CREATE_FAILED);
	uint64_t fail_handle = 0;
	RenderSceneBuffersRD *vp_fail = reinterpret_cast<RenderSceneBuffersRD *>(uintptr_t(0x3000));
	CHECK(upscaler.test_ensure_feature_for_view(vp_fail, 0, Size2i(1280, 720), Size2i(1920, 1080), 0.67f, 2, fail_handle) == DLSSBackend::FEATURE_CREATE_FAILED);
	CHECK(upscaler.get_context_error(vp_fail) == RSE::VIEWPORT_SCALING_3D_REASON_FEATURE_CREATE_FAILED);

	upscaler.free_contexts(vp_a);
	upscaler.free_contexts(vp_b);
	upscaler.free_contexts(vp_fail);
	CHECK(upscaler.get_active_feature_count() == 0);
	CHECK(upscaler.get_context_error(vp_fail) == RSE::VIEWPORT_SCALING_3D_REASON_NONE);
	upscaler.shutdown();
	CHECK(upscaler.get_retired_feature_count() == 0);
}

TEST_CASE("[DLSS][D3D12] Backend selection, non-RTX check, and both D3D12 barrier paths (#119, #204)") {
	DLSSBackendMock vk_mock;
	DLSSBackendMock d3d_mock;

	CHECK(DLSSUpscaler::select_backend_for_api("Vulkan", &vk_mock, &d3d_mock) == &vk_mock);
	CHECK(DLSSUpscaler::select_backend_for_api("D3D12", &vk_mock, &d3d_mock) == &d3d_mock);
	CHECK(DLSSUpscaler::select_backend_for_api("D3D12", &vk_mock, nullptr) == &vk_mock);

	// RR maturity label: Vulkan is "beta", D3D12 carries no maturity label (#206, #204).
	CHECK(DLSSUpscaler::rr_maturity_label_for_api("Vulkan") == "beta");
	CHECK(DLSSUpscaler::rr_maturity_label_for_api("D3D12").is_empty());

	// Enhanced barrier path vs forced-legacy barrier switch (GODOT_D3D12_FORCE_LEGACY_BARRIERS=1).
	const DLSSD3D12BarrierSelection enhanced = dlss_d3d12_select_barrier_path(true, true, "");
	CHECK(enhanced.path == DLSS_D3D12_BARRIER_PATH_ENHANCED);
	CHECK(!enhanced.forced_legacy);

	const DLSSD3D12BarrierSelection forced_legacy = dlss_d3d12_select_barrier_path(true, true, "1");
	CHECK(forced_legacy.path == DLSS_D3D12_BARRIER_PATH_LEGACY);
	CHECK(forced_legacy.forced_legacy);

	// Non-debug builds ignore the debug-only force-legacy switch.
	const DLSSD3D12BarrierSelection release_no_override = dlss_d3d12_select_barrier_path(true, false, "1");
	CHECK(release_no_override.path == DLSS_D3D12_BARRIER_PATH_ENHANCED);
	CHECK(!release_no_override.forced_legacy);

	// Hardware without enhanced barriers always chooses legacy.
	const DLSSD3D12BarrierSelection hw_legacy = dlss_d3d12_select_barrier_path(false, true, "");
	CHECK(hw_legacy.path == DLSS_D3D12_BARRIER_PATH_LEGACY);

	// Mocked non-RTX D3D12 adapter gives NOT_NVIDIA_RTX.
	CHECK(d3d_mock.query_availability(0x1002, true) == DLSSBackend::NOT_NVIDIA_RTX);
	CHECK(DLSSUpscaler::map_status_to_reason(d3d_mock.query_availability(0x8086, true)) == RSE::VIEWPORT_SCALING_3D_REASON_NOT_NVIDIA_RTX);
}

TEST_CASE("[DLSS][RR] Activation rules, >=32 jitter phases at every scale, denoiser bypass, and RR feature switching (#206)") {
	// RR activates only on DLSS + NextGI HWRT + explicit Epic + single view + backend available.
	const RRActivationResult ok = nextgi_resolve_rr_activation(true, true, true, true, false, 1, true);
	CHECK(ok.active);
	CHECK(ok.reason == RSE::VIEWPORT_SCALING_3D_REASON_NONE);

	// quality = auto NEVER enables RR, even if auto resolved to Epic.
	const RRActivationResult auto_epic = nextgi_resolve_rr_activation(true, true, true, true, true, 1, true);
	CHECK(!auto_epic.active);
	CHECK(auto_epic.reason == RSE::VIEWPORT_SCALING_3D_REASON_RR_REQUIRES_NEXTGI_EPIC);

	// SWRT or High/Medium gives RR_REQUIRES_NEXTGI_EPIC.
	CHECK(nextgi_resolve_rr_activation(true, true, false, true, false, 1, true).reason == RSE::VIEWPORT_SCALING_3D_REASON_RR_REQUIRES_NEXTGI_EPIC);
	CHECK(nextgi_resolve_rr_activation(true, true, true, false, false, 1, true).reason == RSE::VIEWPORT_SCALING_3D_REASON_RR_REQUIRES_NEXTGI_EPIC);

	// Multiview, DLSS inactive, or backend unavailable gives RR_UNAVAILABLE.
	CHECK(nextgi_resolve_rr_activation(true, true, true, true, false, 2, true).reason == RSE::VIEWPORT_SCALING_3D_REASON_RR_UNAVAILABLE);
	CHECK(nextgi_resolve_rr_activation(true, false, true, true, false, 1, true).reason == RSE::VIEWPORT_SCALING_3D_REASON_RR_UNAVAILABLE);
	CHECK(nextgi_resolve_rr_activation(true, true, true, true, false, 1, false).reason == RSE::VIEWPORT_SCALING_3D_REASON_RR_UNAVAILABLE);

	// RR requires >= 32 jitter phases at every scale (DLAA 1.0 through Ultra Performance 0.33).
	const float scales[] = { 1.0f, 0.77f, 0.67f, 0.58f, 0.50f, 0.33f };
	for (float sc : scales) {
		const Size2i render_sz(MAX(1, int(Math::round(1920.0f * sc))), MAX(1, int(Math::round(1080.0f * sc))));
		const uint32_t base_phases = nextgi_temporal_jitter_phase_count(Size2i(1920, 1080), render_sz, true);
		CHECK(nextgi_rr_jitter_phase_count(base_phases, true) >= 32u);
	}

	// Denoiser bypass contract reports nextgi_line8 and suppresses line 8 only while bypassed.
	RendererRD::NextGI ngi_probe;
	const Vector<StringName> denoisers = DenoiserBypass::get_registered_denoisers();
	CHECK(denoisers.has(SNAME("nextgi_line8")));
	CHECK(!RendererRD::NextGI::should_emit_pass_slot(RendererRD::NextGI::PASS_SLOT_DENOISE, true));
	CHECK(RendererRD::NextGI::should_emit_pass_slot(RendererRD::NextGI::PASS_SLOT_INTEGRATION, true));
	CHECK(RendererRD::NextGI::should_emit_pass_slot(RendererRD::NextGI::PASS_SLOT_DENOISE, false));

	// Switching a viewport from SR to RR creates a new feature and retires the old one.
	DLSSBackendMock mock;
	CHECK(mock.init(make_test_init_params("a0f57b54-1daf-4934-90ae-c4035c19df04")) == DLSSBackend::OK);
	DLSSUpscaler upscaler(&mock);
	RenderSceneBuffersRD *vp = reinterpret_cast<RenderSceneBuffersRD *>(uintptr_t(0x4000));
	uint64_t sr_h = 0, rr_h = 0;
	CHECK(upscaler.test_ensure_feature_for_view(vp, 0, Size2i(1280, 720), Size2i(1920, 1080), 0.67f, 2, sr_h, false, 0) == DLSSBackend::OK);
	CHECK(upscaler.test_ensure_feature_for_view(vp, 0, Size2i(1280, 720), Size2i(1920, 1080), 0.67f, 2, rr_h, true, 2) == DLSSBackend::OK);
	CHECK(rr_h != sr_h);
	CHECK(upscaler.get_retired_feature_count() == 1);
	upscaler.free_contexts(vp);
	upscaler.shutdown();
}

#ifdef TOOLS_ENABLED
TEST_CASE("[DLSS][Export] Manifest parsing, auto/yes/no modes, vetoes, GUID generation, and Streamline guard (#97, #208)") {
	// Project ID format & uniqueness (#97).
	const String id1 = DLSSEditorPlugin::generate_project_id();
	const String id2 = DLSSEditorPlugin::generate_project_id();
	CHECK(DLSSEditorPlugin::is_valid_project_id_format(id1));
	CHECK(DLSSEditorPlugin::is_valid_project_id_format(id2));
	CHECK(id1 != id2);
	CHECK(!DLSSEditorPlugin::is_valid_project_id_format("not-a-guid"));

	// Attribution line registered via generic EditorAbout hook.
	CHECK(!DLSSEditorPlugin::get_attribution_line().is_empty());
	EditorAbout::add_attribution_line(DLSSEditorPlugin::get_attribution_line());
	CHECK(EditorAbout::get_attribution_lines().has(DLSSEditorPlugin::get_attribution_line()));

	// Auto / Yes / No export decisions.
	CHECK(DLSSEditorExportPlugin::should_export_dlss(DLSSEditorExportPlugin::EXPORT_MODE_AUTO, true, id1));
	CHECK(!DLSSEditorExportPlugin::should_export_dlss(DLSSEditorExportPlugin::EXPORT_MODE_AUTO, true, ""));
	CHECK(!DLSSEditorExportPlugin::should_export_dlss(DLSSEditorExportPlugin::EXPORT_MODE_AUTO, false, id1));
	CHECK(DLSSEditorExportPlugin::should_export_dlss(DLSSEditorExportPlugin::EXPORT_MODE_YES, false, ""));
	CHECK(!DLSSEditorExportPlugin::should_export_dlss(DLSSEditorExportPlugin::EXPORT_MODE_NO, true, id1));

	// Synthetic directory with rel runtime, license, and notices (no proprietary filenames!).
	const String base_tmp = OS::get_singleton()->get_temp_path().path_join("godotnext_dlss_export_test_" + itos(OS::get_singleton()->get_ticks_usec()));
	const String tpl_dir = base_tmp.path_join("tpl");
	const String out_dir = base_tmp.path_join("out");
	REQUIRE(DirAccess::make_dir_recursive_absolute(tpl_dir) == OK);
	REQUIRE(DirAccess::make_dir_recursive_absolute(out_dir) == OK);

	const String rt_path = tpl_dir.path_join("vendor_runtime.dll");
	const String lic_path = tpl_dir.path_join("LICENSE.txt");
	const String not_path = tpl_dir.path_join("NOTICES.txt");
	{
		Ref<FileAccess> f = FileAccess::open(rt_path, FileAccess::WRITE);
		REQUIRE(f.is_valid());
		f->store_string("synthetic-rel-binary-payload");
	}
	{
		Ref<FileAccess> f = FileAccess::open(lic_path, FileAccess::WRITE);
		REQUIRE(f.is_valid());
		f->store_string("synthetic-license");
	}
	{
		Ref<FileAccess> f = FileAccess::open(not_path, FileAccess::WRITE);
		REQUIRE(f.is_valid());
		f->store_string("synthetic-notices");
	}
	const String rt_sha = FileAccess::get_sha256(rt_path).to_lower();
	REQUIRE(rt_sha.length() == 64);

	auto build_json = [&](const String &p_variant, const String &p_file_name, const String &p_file_sha, const String &p_file_var, const String &p_lic, const String &p_not) {
		return vformat(
				"{\"sdk_tag\":\"v310.9.16\",\"pinned_tag\":\"v310.9.16\",\"variant\":\"%s\",\"license\":\"%s\",\"notices\":\"%s\","
				"\"files\":[{\"name\":\"%s\",\"sha256\":\"%s\",\"variant\":\"%s\"}]}",
				p_variant, p_lic, p_not, p_file_name, p_file_sha, p_file_var);
	};

	DLSSRuntimeManifest manifest;
	String err;

	// 1. Valid rel manifest passes validation and copies unmodified bytes.
	REQUIRE(DLSSEditorExportPlugin::parse_manifest(build_json("rel", "vendor_runtime.dll", rt_sha, "rel", "LICENSE.txt", "NOTICES.txt"), manifest, err) == OK);
	CHECK(DLSSEditorExportPlugin::validate_manifest_for_export(manifest, tpl_dir, err));
	CHECK(DLSSEditorExportPlugin::copy_validated_manifest_files(manifest, tpl_dir, out_dir, err) == OK);
	CHECK(FileAccess::exists(out_dir.path_join("vendor_runtime.dll")));
	CHECK(FileAccess::get_sha256(out_dir.path_join("vendor_runtime.dll")).to_lower() == rt_sha);
	CHECK(FileAccess::exists(out_dir.path_join("LICENSE.txt")));
	CHECK(FileAccess::exists(out_dir.path_join("NOTICES.txt")));

	// 2. Dev runtime manifest veto leaves empty destination untouched.
	const String empty_dir = base_tmp.path_join("out_dev");
	REQUIRE(DirAccess::make_dir_recursive_absolute(empty_dir) == OK);
	REQUIRE(DLSSEditorExportPlugin::parse_manifest(build_json("dev", "vendor_runtime.dll", rt_sha, "dev", "LICENSE.txt", "NOTICES.txt"), manifest, err) == OK);
	CHECK(!DLSSEditorExportPlugin::validate_manifest_for_export(manifest, tpl_dir, err));
	CHECK(DLSSEditorExportPlugin::copy_validated_manifest_files(manifest, tpl_dir, empty_dir, err) != OK);
	CHECK(!FileAccess::exists(empty_dir.path_join("vendor_runtime.dll")));

	// 3. SHA-256 mismatch veto.
	const String bad_sha = "0000000000000000000000000000000000000000000000000000000000000000";
	REQUIRE(DLSSEditorExportPlugin::parse_manifest(build_json("rel", "vendor_runtime.dll", bad_sha, "rel", "LICENSE.txt", "NOTICES.txt"), manifest, err) == OK);
	CHECK(!DLSSEditorExportPlugin::validate_manifest_for_export(manifest, tpl_dir, err));

	// 4. Missing notices veto.
	REQUIRE(DLSSEditorExportPlugin::parse_manifest(build_json("rel", "vendor_runtime.dll", rt_sha, "rel", "LICENSE.txt", ""), manifest, err) == OK);
	CHECK(!DLSSEditorExportPlugin::validate_manifest_for_export(manifest, tpl_dir, err));

	// 5. Excluded nvperf runtime veto (#208).
	const String nvperf_path = tpl_dir.path_join("vendor_nvperf.dll");
	{
		Ref<FileAccess> f = FileAccess::open(nvperf_path, FileAccess::WRITE);
		REQUIRE(f.is_valid());
		f->store_string("excluded-perf-runtime");
	}
	const String nvperf_sha = FileAccess::get_sha256(nvperf_path).to_lower();
	REQUIRE(DLSSEditorExportPlugin::parse_manifest(build_json("rel", "vendor_nvperf.dll", nvperf_sha, "rel", "LICENSE.txt", "NOTICES.txt"), manifest, err) == OK);
	CHECK(!DLSSEditorExportPlugin::validate_manifest_for_export(manifest, tpl_dir, err));

	// Cleanup temporary test directory.
	Ref<DirAccess> da = DirAccess::open(base_tmp);
	if (da.is_valid()) {
		da->erase_contents_recursive();
	}
	DirAccess::remove_absolute(base_tmp);
}
#endif // TOOLS_ENABLED

TEST_CASE("[DLSS][Reflex] Streamline load gating, Reflex resolution, forced On with FG, and ordered latency markers (#209)") {
	// Streamline loads ONLY in non-editor D3D12 games when DLSS-G or Reflex is enabled.
	CHECK(!dlss_streamline_should_load(true, "d3d12", "DLSS-G", 1));
	CHECK(!dlss_streamline_should_load(false, "vulkan", "DLSS-G", 1));
	CHECK(!dlss_streamline_should_load(false, "d3d12", "", 0));
	CHECK(dlss_streamline_should_load(false, "d3d12", "DLSS-G", 0));
	CHECK(dlss_streamline_should_load(false, "d3d12", "", 1));

	// Vulkan / Metal -> API_UNSUPPORTED.
	const DLSSReflexResolution vk_res = dlss_resolve_reflex_state(1, false, false, "Vulkan", false, false, 0);
	CHECK(vk_res.effective_mode == 0);
	CHECK(vk_res.reason == "API_UNSUPPORTED");

	const DLSSReflexResolution mtl_res = dlss_resolve_reflex_state(2, false, false, "Metal", false, false, 0);
	CHECK(mtl_res.effective_mode == 0);
	CHECK(mtl_res.reason == "API_UNSUPPORTED");

	// D3D12 without low-latency support -> REFLEX_UNAVAILABLE.
	const DLSSReflexResolution unavail = dlss_resolve_reflex_state(1, false, false, "D3D12", true, false, 0);
	CHECK(unavail.effective_mode == 0);
	CHECK(unavail.reason == "REFLEX_UNAVAILABLE");

	// D3D12 with Reflex Off + FG active -> forced On (mode=1, FORCED_ON_WITH_FG) + frameLimitUs from max_fps.
	const DLSSReflexResolution forced = dlss_resolve_reflex_state(0, true, false, "D3D12", true, true, 120);
	CHECK(forced.effective_mode == 1);
	CHECK(forced.forced_by_fg);
	CHECK(forced.reason == "FORCED_ON_WITH_FG");
	CHECK(forced.frame_limit_us == 8333u);

	// D3D12 with Reflex On + Boost + FG active keeps Boost (mode=2).
	const DLSSReflexResolution boost = dlss_resolve_reflex_state(2, true, true, "D3D12", true, true, 240);
	CHECK(boost.effective_mode == 2);
	CHECK(!boost.forced_by_fg);
	CHECK(boost.reason == "OK");
	CHECK(boost.frame_limit_us == 4167u);

	// Latency markers fire once per frame in strict U5 order (SLEEP -> SIM_START -> INPUT_SAMPLE -> SIM_END -> RENDER_SUBMIT_START -> RENDER_SUBMIT_END -> PRESENT_START -> PRESENT_END).
	reset_latency_markers_for_tests();
	DLSSBackendMock mock;
	dlss_streamline_install_mock_marker_callback(&mock);

	const uint64_t f1 = begin_latency_frame();
	emit_latency_marker(LATENCY_MARKER_SLEEP, f1);
	emit_latency_marker(LATENCY_MARKER_SIMULATION_START, f1);
	emit_latency_input_sample_once(f1);
	emit_latency_input_sample_once(f1); // Duplicate flush in same frame must not double-emit.
	emit_latency_marker(LATENCY_MARKER_SIMULATION_END, f1);
	emit_latency_marker(LATENCY_MARKER_RENDER_SUBMIT_START, f1);
	emit_latency_marker(LATENCY_MARKER_RENDER_SUBMIT_END, f1);
	emit_latency_marker(LATENCY_MARKER_PRESENT_START, f1);
	emit_latency_marker(LATENCY_MARKER_PRESENT_END, f1);

	REQUIRE(mock.get_marker_records().size() == 8);
	CHECK(mock.get_marker_records()[0].marker == LATENCY_MARKER_SLEEP);
	CHECK(mock.get_marker_records()[1].marker == LATENCY_MARKER_SIMULATION_START);
	CHECK(mock.get_marker_records()[2].marker == LATENCY_MARKER_INPUT_SAMPLE);
	CHECK(mock.get_marker_records()[3].marker == LATENCY_MARKER_SIMULATION_END);
	CHECK(mock.get_marker_records()[4].marker == LATENCY_MARKER_RENDER_SUBMIT_START);
	CHECK(mock.get_marker_records()[5].marker == LATENCY_MARKER_RENDER_SUBMIT_END);
	CHECK(mock.get_marker_records()[6].marker == LATENCY_MARKER_PRESENT_START);
	CHECK(mock.get_marker_records()[7].marker == LATENCY_MARKER_PRESENT_END);
	for (int i = 0; i < 8; i++) {
		CHECK(mock.get_marker_records()[i].frame_id == f1);
	}

	dlss_streamline_install_mock_marker_callback(nullptr);
	reset_latency_markers_for_tests();
}

TEST_CASE("[DLSS][FG] Reason precedence, constants, tags, eOff after reset, and GU+-7 mocked capability checks (#200)") {
	MockFrameGenOps ops;
	Ref<DLSSFrameGeneration> fg;
	fg.instantiate();
	fg->set_backend_ops(&ops);

	// Reason precedence: API -> non-NVIDIA -> Streamline failed -> provider/composition -> FG GPU unsupported -> OK.
	ops.api_name = "Vulkan";
	CHECK(fg->is_available() == FrameGenerationExtension::REASON_API_UNSUPPORTED);
	ops.api_name = "Metal";
	CHECK(fg->is_available() == FrameGenerationExtension::REASON_API_UNSUPPORTED);
	ops.api_name = "D3D12";

	// GU+-7 mocked non-NVIDIA adapter (Intel/AMD) -> NOT_NVIDIA_RTX.
	ops.vendor_name = "Intel Corporation";
	CHECK(fg->is_available() == FrameGenerationExtension::REASON_NOT_NVIDIA_RTX);
	ops.vendor_name = "NVIDIA Corporation";

	// Streamline init failed -> STREAMLINE_UNAVAILABLE.
	ops.streamline_init_failed = true;
	CHECK(fg->is_available() == FrameGenerationExtension::REASON_STREAMLINE_UNAVAILABLE);
	ops.streamline_init_failed = false;

	// Composition swapchain (per-pixel transparency) -> PROVIDER_UNAVAILABLE.
	ops.composition_swapchain = true;
	CHECK(fg->is_available() == FrameGenerationExtension::REASON_PROVIDER_UNAVAILABLE);
	ops.composition_swapchain = false;

	// GU+-7 mocked RTX 30-class (slIsFeatureSupported fails) -> FG_GPU_UNSUPPORTED.
	ops.feature_supported = false;
	ops.feature_result_name = "eErrorAdapterNotSupported";
	CHECK(fg->is_available() == FrameGenerationExtension::REASON_FG_GPU_UNSUPPORTED);
	ops.feature_supported = true;
	ops.feature_result_name = "eOk";

	CHECK(fg->is_available() == FrameGenerationExtension::REASON_OK);

	// Constants & tags on frame inputs.
	Ref<FrameGenerationInputs> inputs;
	inputs.instantiate();
	inputs->set_depth(RID::from_uint64(101));
	inputs->set_velocity(RID::from_uint64(102));
	inputs->set_hudless_color(RID::from_uint64(103));
	inputs->set_ui_color(RID::from_uint64(104));
	inputs->set_render_size(Size2i(1280, 720));
	inputs->set_output_size(Size2i(1920, 1080));
	inputs->set_motion_vector_scale(Vector2(1280.0f, 720.0f));
	inputs->set_jitter(Vector2(0.125f, -0.25f));
	inputs->set_z_near(0.1f);
	inputs->set_z_far(1000.0f);
	inputs->set_fovy(60.0f);
	inputs->set_aspect(16.0f / 9.0f);
	inputs->set_reset(true);
	inputs->set_frame_index(1);

	fg->on_frame_inputs(inputs);
	CHECK(ops.set_constants_count == 1u);
	CHECK(ops.last_constants.depth_inverted);
	CHECK(ops.last_constants.camera_motion_included);
	CHECK(!ops.last_constants.motion_vectors_3d);
	CHECK(ops.last_constants.reset);
	CHECK(ops.set_tags_count == 1u);
	REQUIRE(ops.last_tags.size() == 4);
	CHECK(ops.last_tags[0].kind == DLSSFrameGenBackendOps::TAG_DEPTH);
	CHECK(ops.last_tags[0].width == 1280u);
	CHECK(ops.last_tags[1].kind == DLSSFrameGenBackendOps::TAG_MOTION_VECTORS);
	CHECK(ops.last_tags[2].kind == DLSSFrameGenBackendOps::TAG_HUDLESS_COLOR);
	CHECK(ops.last_tags[2].width == 1920u);
	CHECK(ops.last_tags[3].kind == DLSSFrameGenBackendOps::TAG_UI_COLOR_AND_ALPHA);
	CHECK(ops.last_tags[3].valid_until_present);

	// First present after reset sends MODE_OFF; next steady-state present sends MODE_ON (FG 2x -> numFramesToGenerate = 1).
	fg->on_present(inputs.ptr(), true);
	REQUIRE(ops.sent_options.size() == 1);
	CHECK(ops.sent_options[0].mode == DLSSFrameGenBackendOps::MODE_OFF);
	CHECK(ops.sent_options[0].retain_resources_when_off);
	fg->on_present(inputs.ptr(), false);

	inputs->set_reset(false);
	inputs->set_frame_index(2);
	fg->on_frame_inputs(inputs);
	fg->on_present(inputs.ptr(), true);
	REQUIRE(ops.sent_options.size() == 2);
	CHECK(ops.sent_options[1].mode == DLSSFrameGenBackendOps::MODE_ON);
	CHECK(ops.sent_options[1].num_frames_to_generate == 1u);
	CHECK(ops.notified_fg_active);
	fg->on_present(inputs.ptr(), false);

	const Dictionary caps = fg->get_provider_capabilities();
	CHECK(int(caps["max_multiplier"]) == 2);
	CHECK(int64_t(caps["num_frames_presented"]) == 2);
	CHECK(int64_t(caps["vram_bytes"]) == int64_t(96u * 1024u * 1024u));
}

TEST_CASE("[DLSS][MFG] Ada fallback (max 1 + 4x -> FG 2x), RTX 50 6x, Dynamic MFG, and live multiplier update (#201)") {
	MockFrameGenOps ops;
	Ref<DLSSFrameGeneration> fg;
	fg.instantiate();
	fg->set_backend_ops(&ops);

	Ref<FrameGenerationInputs> inputs;
	inputs.instantiate();
	inputs->set_render_size(Size2i(1920, 1080));
	inputs->set_output_size(Size2i(3840, 2160));
	inputs->set_reset(false);

	// 1. Ada GPU (numFramesToGenerateMax = 1) requesting 4x -> MFG_REQUIRES_RTX50, runs FG 2x (numFramesToGenerate = 1).
	ops.next_state.num_frames_to_generate_max = 1;
	ops.next_state.dynamic_mfg_supported = false;
	Dictionary mode_4x;
	mode_4x["multiplier"] = 4;
	mode_4x["dynamic"] = false;
	mode_4x["dynamic_target_fps"] = 0;
	fg->set_mode(mode_4x);
	CHECK(fg->is_available() == FrameGenerationExtension::REASON_MFG_REQUIRES_RTX50);

	// Flush initial skip_next_present, then verify steady-state options clamp to 1 (FG 2x).
	fg->on_present(inputs.ptr(), true);
	fg->on_present(inputs.ptr(), false);
	fg->on_present(inputs.ptr(), true);
	REQUIRE(!ops.sent_options.is_empty());
	CHECK(ops.sent_options[ops.sent_options.size() - 1].mode == DLSSFrameGenBackendOps::MODE_ON);
	CHECK(ops.sent_options[ops.sent_options.size() - 1].num_frames_to_generate == 1u);

	// 2. RTX 50-class GPU (numFramesToGenerateMax = 5) requesting 6x -> REASON_OK, numFramesToGenerate = 5; live change re-sends options once.
	ops.next_state.num_frames_to_generate_max = 5;
	ops.next_state.dynamic_mfg_supported = false;
	fg->on_present(inputs.ptr(), false); // Refreshes cached_state.
	Dictionary mode_6x;
	mode_6x["multiplier"] = 6;
	mode_6x["dynamic"] = false;
	mode_6x["dynamic_target_fps"] = 0;
	fg->set_mode(mode_6x);
	CHECK(fg->is_available() == FrameGenerationExtension::REASON_OK);
	const uint32_t before_6x = ops.set_options_count;
	fg->on_present(inputs.ptr(), true);
	CHECK(ops.set_options_count == before_6x + 1u);
	CHECK(ops.sent_options[ops.sent_options.size() - 1].mode == DLSSFrameGenBackendOps::MODE_ON);
	CHECK(ops.sent_options[ops.sent_options.size() - 1].num_frames_to_generate == 5u);
	// Second unchanged present does not re-send options redundantly.
	fg->on_present(inputs.ptr(), true);
	CHECK(ops.set_options_count == before_6x + 1u);

	// 3. Dynamic MFG unsupported on multi-frame GPU -> REASON_DYNAMIC_MFG_UNSUPPORTED, keeps fixed multiplier.
	Dictionary mode_dyn;
	mode_dyn["multiplier"] = 4;
	mode_dyn["dynamic"] = true;
	mode_dyn["dynamic_target_fps"] = 240;
	fg->set_mode(mode_dyn);
	CHECK(fg->is_available() == FrameGenerationExtension::REASON_DYNAMIC_MFG_UNSUPPORTED);
	fg->on_present(inputs.ptr(), true);
	CHECK(ops.sent_options[ops.sent_options.size() - 1].mode == DLSSFrameGenBackendOps::MODE_ON);
	CHECK(ops.sent_options[ops.sent_options.size() - 1].num_frames_to_generate == 3u);

	// 4. Dynamic MFG supported -> REASON_OK, sends MODE_DYNAMIC with dynamicTargetFrameRate = 240.
	ops.next_state.dynamic_mfg_supported = true;
	fg->on_present(inputs.ptr(), false);
	CHECK(fg->is_available() == FrameGenerationExtension::REASON_OK);
	fg->on_present(inputs.ptr(), true);
	CHECK(ops.sent_options[ops.sent_options.size() - 1].mode == DLSSFrameGenBackendOps::MODE_DYNAMIC);
	CHECK(ops.sent_options[ops.sent_options.size() - 1].dynamic_target_fps == doctest::Approx(240.0f));
	CHECK(ops.notified_fg_active);
	CHECK(ops.notified_fg_dynamic);
	CHECK(bool(fg->get_provider_capabilities()["dynamic_supported"]));
	CHECK(int(fg->get_provider_capabilities()["max_multiplier"]) == 6);
}

} // namespace TestDLSS
