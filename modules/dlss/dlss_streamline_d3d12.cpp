/**************************************************************************/
/*  dlss_streamline_d3d12.cpp                                             */
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

// GodotNext: pu_streamline_reflex (#209), pu_dlss_fg (#200), pu_dlss_mfg (#201).
// Only this translation unit includes Streamline SDK headers (when DLSS_STREAMLINE_ENABLED && D3D12_ENABLED).

#include "dlss_streamline_d3d12.h"

#include "dlss_backend_mock.h"
#include "dlss_frame_generation.h"
#include "dlss_ngx_d3d12.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/math/math_funcs.h"
#include "core/os/latency_markers.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/version.h"
#include "scene/main/scene_tree.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"

#if defined(DLSS_STREAMLINE_ENABLED) && defined(D3D12_ENABLED)

#include "drivers/d3d12/d3d12_hooks.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif

#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <string>

#endif // DLSS_STREAMLINE_ENABLED && D3D12_ENABLED

namespace {

Mutex g_sl_mutex;
bool g_sl_loaded = false;
bool g_sl_init_failed = false;
bool g_fg_active = false;
bool g_fg_dynamic = false;
bool g_reflex_forced_logged = false;
bool g_reflex_api_unsupported_logged = false;
bool g_reflex_unavailable_logged = false;
bool g_reflex_owns_fps_cap = false;

DLSSBackendMock *g_mock_marker_target = nullptr;

template <typename T>
T sl_read_setting(const char *p_name, const T &p_default) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps == nullptr || !ps->has_setting(p_name)) {
		return p_default;
	}
	return ps->get_setting_with_override(p_name);
}

String sl_current_api_name() {
	if (RD::get_singleton() != nullptr) {
		return RD::get_singleton()->get_device_api_name();
	}
	if (OS::get_singleton() != nullptr) {
		const String drv = OS::get_singleton()->get_current_rendering_driver_name();
		if (drv == "d3d12") {
			return "D3D12";
		}
		if (drv == "metal") {
			return "Metal";
		}
	}
	return "Vulkan";
}

void _mock_latency_marker_cb(LatencyMarker p_marker, uint64_t p_frame_id) {
	MutexLock lock(g_sl_mutex);
	if (g_mock_marker_target != nullptr) {
		g_mock_marker_target->record_latency_marker(int(p_marker), p_frame_id);
	}
}

#if defined(DLSS_STREAMLINE_ENABLED) && defined(D3D12_ENABLED)

uint64_t g_sl_token_frame_id = UINT64_MAX;
sl::FrameToken *g_sl_current_token = nullptr;
bool g_sl_device_set = false;
LUID g_sl_adapter_luid = {};
bool g_sl_has_adapter_luid = false;
bool g_sl_reflex_low_latency_available = false;
bool g_sl_reflex_state_queried = false;
int g_sl_applied_reflex_mode = -1;
uint32_t g_sl_applied_frame_limit_us = UINT32_MAX;
std::wstring g_sl_plugin_dir_w;
std::wstring g_sl_app_data_w;
const wchar_t *g_sl_plugin_paths[1] = { nullptr };
CharString g_sl_project_id_utf8;
CharString g_sl_engine_version_utf8;

std::wstring sl_string_to_wstring(const String &p_str) {
	std::wstring out;
	out.reserve(p_str.length());
	for (int i = 0; i < p_str.length(); i++) {
		out.push_back(static_cast<wchar_t>(p_str[i]));
	}
	return out;
}

const char *sl_result_to_name(sl::Result p_res) {
	switch (p_res) {
		case sl::Result::eOk:
			return "eOk";
		case sl::Result::eErrorIO:
			return "eErrorIO";
		case sl::Result::eErrorDriverOutOfDate:
			return "eErrorDriverOutOfDate";
		case sl::Result::eErrorOSOutOfDate:
			return "eErrorOSOutOfDate";
		case sl::Result::eErrorOSDisabledHWS:
			return "eErrorOSDisabledHWS";
		case sl::Result::eErrorDeviceNotCreated:
			return "eErrorDeviceNotCreated";
		case sl::Result::eErrorNoSupportedAdapterFound:
			return "eErrorNoSupportedAdapterFound";
		case sl::Result::eErrorAdapterNotSupported:
			return "eErrorAdapterNotSupported";
		case sl::Result::eErrorNoPlugins:
			return "eErrorNoPlugins";
		case sl::Result::eErrorVulkanAPI:
			return "eErrorVulkanAPI";
		case sl::Result::eErrorDXGIAPI:
			return "eErrorDXGIAPI";
		case sl::Result::eErrorD3DAPI:
			return "eErrorD3DAPI";
		case sl::Result::eErrorNVAPI:
			return "eErrorNVAPI";
		case sl::Result::eErrorReflexAPI:
			return "eErrorReflexAPI";
		case sl::Result::eErrorNGXFailed:
			return "eErrorNGXFailed";
		case sl::Result::eErrorMissingProxy:
			return "eErrorMissingProxy";
		case sl::Result::eErrorMissingResourceState:
			return "eErrorMissingResourceState";
		case sl::Result::eErrorInvalidIntegration:
			return "eErrorInvalidIntegration";
		case sl::Result::eErrorMissingInputParameter:
			return "eErrorMissingInputParameter";
		case sl::Result::eErrorNotInitialized:
			return "eErrorNotInitialized";
		case sl::Result::eErrorInitNotCalled:
			return "eErrorInitNotCalled";
		case sl::Result::eErrorInvalidParameter:
			return "eErrorInvalidParameter";
		case sl::Result::eErrorFeatureMissing:
			return "eErrorFeatureMissing";
		case sl::Result::eErrorFeatureNotSupported:
			return "eErrorFeatureNotSupported";
		case sl::Result::eErrorFeatureMissingHooks:
			return "eErrorFeatureMissingHooks";
		case sl::Result::eErrorFeatureFailedToLoad:
			return "eErrorFeatureFailedToLoad";
		case sl::Result::eWarnOutOfVRAM:
			return "eWarnOutOfVRAM";
		default:
			return "eErrorUnknown";
	}
}

String sl_dlssg_status_to_name(sl::DLSSGStatus p_status) {
	if (p_status == sl::DLSSGStatus::eOk) {
		return "eOk";
	}
	String out;
	auto add = [&](const char *p_tok) {
		if (!out.is_empty()) {
			out += "|";
		}
		out += p_tok;
	};
	if (p_status & sl::DLSSGStatus::eFailResolutionTooLow) {
		add("eFailResolutionTooLow");
	}
	if (p_status & sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime) {
		add("eFailReflexNotDetectedAtRuntime");
	}
	if (p_status & sl::DLSSGStatus::eFailHDRFormatNotSupported) {
		add("eFailHDRFormatNotSupported");
	}
	if (p_status & sl::DLSSGStatus::eFailCommonConstantsInvalid) {
		add("eFailCommonConstantsInvalid");
	}
	if (p_status & sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled) {
		add("eFailGetCurrentBackBufferIndexNotCalled");
	}
	return out.is_empty() ? String("eErrorUnknown") : out;
}

void sl_log_callback(sl::LogType p_type, const char *p_msg) {
	if (p_msg == nullptr || p_msg[0] == '\0') {
		return;
	}
	const String msg = String::utf8(p_msg).strip_edges();
	if (msg.is_empty()) {
		return;
	}
	if (p_type == sl::LogType::eError) {
		print_verbose(vformat("Streamline[ERROR]: %s", msg));
	} else if (p_type == sl::LogType::eWarn) {
		print_verbose(vformat("Streamline[WARN]: %s", msg));
	} else {
		print_verbose(vformat("Streamline[INFO]: %s", msg));
	}
}

void *sl_unwrap_native_interface(void *p_proxy) {
	if (!g_sl_loaded || p_proxy == nullptr) {
		return p_proxy;
	}
	void *native = nullptr;
	if (slGetNativeInterface(p_proxy, &native) == sl::Result::eOk && native != nullptr) {
		return native;
	}
	return p_proxy;
}

sl::FrameToken *sl_get_or_create_token_locked(uint64_t p_frame_id) {
	if (!g_sl_loaded) {
		return nullptr;
	}
	if (g_sl_current_token != nullptr && g_sl_token_frame_id == p_frame_id) {
		return g_sl_current_token;
	}
	uint32_t frame_idx = static_cast<uint32_t>(p_frame_id & 0xFFFFFFFFu);
	sl::FrameToken *token = nullptr;
	if (slGetNewFrameToken(token, &frame_idx) == sl::Result::eOk && token != nullptr) {
		g_sl_current_token = token;
		g_sl_token_frame_id = p_frame_id;
		return token;
	}
	return g_sl_current_token;
}

sl::float4x4 projection_to_sl_matrix(const Projection &p_proj) {
	sl::float4x4 out;
	for (int c = 0; c < 4; c++) {
		out.row[c] = sl::float4(
				float(p_proj.columns[c][0]),
				float(p_proj.columns[c][1]),
				float(p_proj.columns[c][2]),
				float(p_proj.columns[c][3]));
	}
	return out;
}

void _sl_on_d3d12_device_created(ID3D12Device *p_device, void *p_userdata) {
	(void)p_userdata;
	MutexLock lock(g_sl_mutex);
	if (!g_sl_loaded || p_device == nullptr || g_sl_device_set) {
		return;
	}
	const sl::Result res = slSetD3DDevice(static_cast<void *>(p_device));
	if (res == sl::Result::eOk) {
		g_sl_device_set = true;
		g_sl_adapter_luid = p_device->GetAdapterLuid();
		g_sl_has_adapter_luid = true;
		print_verbose("Streamline: slSetD3DDevice succeeded.");
	} else {
		WARN_PRINT(vformat("Streamline: slSetD3DDevice failed (%s).", sl_result_to_name(res)));
	}
}

void _sl_sync_reflex_options_locked() {
	if (!g_sl_loaded || !g_sl_device_set || RD::get_singleton() == nullptr) {
		return;
	}
	if (!g_sl_reflex_state_queried) {
		sl::ReflexState st;
		if (slReflexGetState(st) == sl::Result::eOk) {
			g_sl_reflex_low_latency_available = st.lowLatencyAvailable;
			g_sl_reflex_state_queried = true;
		}
	}

	const int req_mode = CLAMP(sl_read_setting<int>("rendering/latency/reflex_mode", 0), 0, 2);
	const int max_fps = MAX(0, sl_read_setting<int>("application/run/max_fps", 0));
	const String api_name = sl_current_api_name();
	const DLSSReflexResolution res = dlss_resolve_reflex_state(
			req_mode,
			g_fg_active,
			g_fg_dynamic,
			api_name,
			g_sl_loaded,
			g_sl_reflex_low_latency_available,
			max_fps);

	if (res.forced_by_fg && !g_reflex_forced_logged) {
		g_reflex_forced_logged = true;
		print_line("NVIDIA Reflex forced On while DLSS Frame Generation is active.");
	}
	if (res.reason == "REFLEX_UNAVAILABLE" && !g_reflex_unavailable_logged) {
		g_reflex_unavailable_logged = true;
		WARN_PRINT("NVIDIA Reflex is unavailable on this adapter/driver (REFLEX_UNAVAILABLE).");
	}
	if (res.reason == "API_UNSUPPORTED" && !g_reflex_api_unsupported_logged) {
		g_reflex_api_unsupported_logged = true;
		WARN_PRINT("NVIDIA Reflex is supported on D3D12 only (API_UNSUPPORTED).");
	}

	if (res.effective_mode != g_sl_applied_reflex_mode || res.frame_limit_us != g_sl_applied_frame_limit_us) {
		sl::ReflexOptions opts;
		if (res.effective_mode == 2) {
			opts.mode = sl::ReflexMode::eLowLatencyWithBoost;
		} else if (res.effective_mode == 1) {
			opts.mode = sl::ReflexMode::eLowLatency;
		} else {
			opts.mode = sl::ReflexMode::eOff;
		}
		opts.frameLimitUs = res.frame_limit_us;
		opts.useMarkersToOptimize = false;
		if (slReflexSetOptions(opts) == sl::Result::eOk) {
			g_sl_applied_reflex_mode = res.effective_mode;
			g_sl_applied_frame_limit_us = res.frame_limit_us;
			if (res.frame_limit_us > 0) {
				RD::get_singleton()->_set_max_fps(0);
				g_reflex_owns_fps_cap = true;
			} else if (g_reflex_owns_fps_cap) {
				RD::get_singleton()->_set_max_fps(max_fps);
				g_reflex_owns_fps_cap = false;
			}
		}
	}
}

void _sl_latency_marker_callback(LatencyMarker p_marker, uint64_t p_frame_id) {
	if (!g_sl_loaded || !g_sl_device_set || RD::get_singleton() == nullptr) {
		return;
	}
	MutexLock lock(g_sl_mutex);
	sl::FrameToken *token = sl_get_or_create_token_locked(p_frame_id);
	if (token == nullptr) {
		return;
	}
	switch (p_marker) {
		case LATENCY_MARKER_SLEEP: {
			_sl_sync_reflex_options_locked();
			if (g_sl_applied_reflex_mode > 0) {
				slReflexSleep(*token);
			}
		} break;
		case LATENCY_MARKER_SIMULATION_START:
			slPCLSetMarker(sl::PCLMarker::eSimulationStart, *token);
			break;
		case LATENCY_MARKER_SIMULATION_END:
			slPCLSetMarker(sl::PCLMarker::eSimulationEnd, *token);
			break;
		case LATENCY_MARKER_INPUT_SAMPLE:
			slPCLSetMarker(sl::PCLMarker::eControllerInputSample, *token);
			break;
		case LATENCY_MARKER_RENDER_SUBMIT_START:
			slPCLSetMarker(sl::PCLMarker::eRenderSubmitStart, *token);
			break;
		case LATENCY_MARKER_RENDER_SUBMIT_END:
			slPCLSetMarker(sl::PCLMarker::eRenderSubmitEnd, *token);
			break;
		case LATENCY_MARKER_PRESENT_START:
			slPCLSetMarker(sl::PCLMarker::ePresentStart, *token);
			break;
		case LATENCY_MARKER_PRESENT_END:
			slPCLSetMarker(sl::PCLMarker::ePresentEnd, *token);
			break;
	}
}

class DLSSStreamlineFGOps : public DLSSFrameGenBackendOps {
public:
	String get_api_name() const override { return sl_current_api_name(); }

	String get_vendor_name() const override {
		if (RD::get_singleton() != nullptr) {
			return RD::get_singleton()->get_device_vendor_name();
		}
		return String();
	}

	bool is_streamline_loaded() const override { return g_sl_loaded; }
	bool is_streamline_init_failed() const override { return g_sl_init_failed; }

	bool is_composition_swapchain() const override {
		return OS::get_singleton() != nullptr && OS::get_singleton()->is_layered_allowed();
	}

	bool is_scene_tree_paused() const override {
		SceneTree *tree = SceneTree::get_singleton();
		return tree != nullptr && tree->is_paused();
	}

	bool is_dynamic_resolution_enabled() const override {
		return sl_read_setting<bool>("rendering/scaling_3d/dynamic_resolution/enabled", false);
	}

	bool query_feature_supported(String &r_result_name) const override {
		r_result_name = String();
		if (!g_sl_loaded || !g_sl_device_set) {
			r_result_name = "eErrorNotInitialized";
			return false;
		}
		sl::AdapterInfo adapter;
		LUID luid_copy = g_sl_adapter_luid;
		if (g_sl_has_adapter_luid) {
			adapter.deviceLUID = reinterpret_cast<uint8_t *>(&luid_copy);
			adapter.deviceLUIDSizeInBytes = sizeof(luid_copy);
		}
		const sl::Result res = slIsFeatureSupported(sl::kFeatureDLSS_G, adapter);
		r_result_name = String(sl_result_to_name(res));
		return res == sl::Result::eOk;
	}

	bool set_constants(uint64_t p_frame_index, const ConstantsSnapshot &p_consts) override {
		MutexLock lock(g_sl_mutex);
		if (!g_sl_loaded || !g_sl_device_set) {
			return false;
		}
		sl::FrameToken *token = sl_get_or_create_token_locked(p_frame_index);
		if (token == nullptr) {
			return false;
		}
		sl::Constants slc;
		slc.cameraViewToClip = projection_to_sl_matrix(p_consts.view_to_clip);
		slc.clipToCameraView = projection_to_sl_matrix(p_consts.clip_to_view);
		slc.clipToPrevClip = projection_to_sl_matrix(p_consts.clip_to_prev_clip);
		slc.prevClipToClip = projection_to_sl_matrix(p_consts.prev_clip_to_clip);
		slc.jitterOffset = sl::float2(p_consts.jitter_offset.x, p_consts.jitter_offset.y);
		slc.mvecScale = sl::float2(p_consts.mv_scale.x, p_consts.mv_scale.y);
		slc.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
		slc.cameraPos = sl::float3(p_consts.camera_pos.x, p_consts.camera_pos.y, p_consts.camera_pos.z);
		slc.cameraUp = sl::float3(p_consts.camera_up.x, p_consts.camera_up.y, p_consts.camera_up.z);
		slc.cameraRight = sl::float3(p_consts.camera_right.x, p_consts.camera_right.y, p_consts.camera_right.z);
		slc.cameraFwd = sl::float3(p_consts.camera_fwd.x, p_consts.camera_fwd.y, p_consts.camera_fwd.z);
		slc.cameraNear = p_consts.z_near;
		slc.cameraFar = p_consts.z_far;
		slc.cameraFOV = p_consts.fovy_rad;
		slc.cameraAspectRatio = p_consts.aspect;
		slc.depthInverted = p_consts.depth_inverted ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		slc.cameraMotionIncluded = p_consts.camera_motion_included ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		slc.motionVectors3D = p_consts.motion_vectors_3d ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		slc.reset = p_consts.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		slc.orthographicProjection = sl::Boolean::eFalse;
		slc.motionVectorsDilated = sl::Boolean::eFalse;
		slc.motionVectorsJittered = sl::Boolean::eFalse;

		const sl::ViewportHandle viewport(0u);
		return slSetConstants(slc, *token, viewport) == sl::Result::eOk;
	}

	bool set_tags(uint64_t p_frame_index, const Vector<TagSnapshot> &p_tags, uint64_t p_cmd_list_handle) override {
		MutexLock lock(g_sl_mutex);
		if (!g_sl_loaded || !g_sl_device_set) {
			return false;
		}
		sl::FrameToken *token = sl_get_or_create_token_locked(p_frame_index);
		if (token == nullptr || p_tags.is_empty()) {
			return false;
		}
		const uint32_t count = MIN<uint32_t>(p_tags.size(), 4u);
		sl::Resource sl_resources[4] = {
			sl::Resource(sl::ResourceType::eTex2d, nullptr, 0),
			sl::Resource(sl::ResourceType::eTex2d, nullptr, 0),
			sl::Resource(sl::ResourceType::eTex2d, nullptr, 0),
			sl::Resource(sl::ResourceType::eTex2d, nullptr, 0),
		};
		sl::Extent extents[4];
		sl::ResourceTag sl_tags[4] = {
			sl::ResourceTag(nullptr, 0, sl::eValidUntilPresent),
			sl::ResourceTag(nullptr, 0, sl::eValidUntilPresent),
			sl::ResourceTag(nullptr, 0, sl::eValidUntilPresent),
			sl::ResourceTag(nullptr, 0, sl::eValidUntilPresent),
		};

		bool all_valid_until_present = true;
		for (uint32_t i = 0; i < count; i++) {
			const TagSnapshot &t = p_tags[i];
			if (!t.valid_until_present) {
				all_valid_until_present = false;
			}
			void *native_res = reinterpret_cast<void *>(static_cast<uintptr_t>(t.resource_handle));
			sl_resources[i] = sl::Resource(sl::ResourceType::eTex2d, native_res, t.d3d12_state);
			extents[i].top = 0;
			extents[i].left = 0;
			extents[i].width = t.width;
			extents[i].height = t.height;
			sl_tags[i] = sl::ResourceTag(
					&sl_resources[i],
					static_cast<sl::BufferType>(t.kind),
					t.valid_until_present ? sl::eValidUntilPresent : sl::eOnlyValidNow,
					&extents[i]);
		}

		// sl_core_api.h: cmdBuffer can be null when all tags use eValidUntilPresent.
		void *cmd_ptr = all_valid_until_present ? nullptr : reinterpret_cast<void *>(static_cast<uintptr_t>(p_cmd_list_handle));
		if (cmd_ptr != nullptr) {
			cmd_ptr = sl_unwrap_native_interface(cmd_ptr);
		}
		const sl::ViewportHandle viewport(0u);
		return slSetTagForFrame(*token, viewport, sl_tags, count, static_cast<sl::CommandBuffer *>(cmd_ptr)) == sl::Result::eOk;
	}

	static sl::DLSSGOptions _to_sl_options(const OptionsSnapshot &p_opts) {
		sl::DLSSGOptions slo;
		if (p_opts.mode == MODE_DYNAMIC) {
			slo.mode = sl::DLSSGMode::eDynamic;
		} else if (p_opts.mode == MODE_ON) {
			slo.mode = sl::DLSSGMode::eOn;
		} else {
			slo.mode = sl::DLSSGMode::eOff;
		}
		slo.numFramesToGenerate = p_opts.num_frames_to_generate;
		sl::DLSSGFlags flags{};
		if (p_opts.color_width >= 4 && p_opts.color_height >= 4) {
			flags |= sl::DLSSGFlags::eRequestVRAMEstimate;
			slo.colorWidth = p_opts.color_width;
			slo.colorHeight = p_opts.color_height;
			slo.mvecDepthWidth = p_opts.mvec_depth_width >= 4 ? p_opts.mvec_depth_width : p_opts.color_width;
			slo.mvecDepthHeight = p_opts.mvec_depth_height >= 4 ? p_opts.mvec_depth_height : p_opts.color_height;
			slo.numBackBuffers = 3;
			slo.colorBufferFormat = 28; // DXGI_FORMAT_R8G8B8A8_UNORM
		}
		if (p_opts.retain_resources_when_off) {
			flags |= sl::DLSSGFlags::eRetainResourcesWhenOff;
		}
		if (p_opts.dynamic_resolution_enabled) {
			flags |= sl::DLSSGFlags::eDynamicResolutionEnabled;
			slo.dynamicResWidth = p_opts.dynamic_res_width;
			slo.dynamicResHeight = p_opts.dynamic_res_height;
		}
		slo.flags = flags;
		slo.enableUserInterfaceRecomposition = p_opts.enable_ui_recomposition ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		slo.dynamicTargetFrameRate = p_opts.dynamic_target_fps;
		return slo;
	}

	bool set_options(const OptionsSnapshot &p_opts) override {
		if (!g_sl_loaded || !g_sl_device_set) {
			return false;
		}
		const sl::ViewportHandle viewport(0u);
		const sl::DLSSGOptions slo = _to_sl_options(p_opts);
		return slDLSSGSetOptions(viewport, slo) == sl::Result::eOk;
	}

	bool get_state(const OptionsSnapshot *p_opts, StateSnapshot &r_state) override {
		r_state = StateSnapshot();
		if (!g_sl_loaded || !g_sl_device_set) {
			return false;
		}
		const sl::ViewportHandle viewport(0u);
		sl::DLSSGOptions slo;
		const sl::DLSSGOptions *opt_ptr = nullptr;
		if (p_opts != nullptr) {
			slo = _to_sl_options(*p_opts);
			opt_ptr = &slo;
		}
		sl::DLSSGState st;
		if (slDLSSGGetState(viewport, st, opt_ptr) != sl::Result::eOk) {
			return false;
		}
		r_state.ok = true;
		r_state.status_code = static_cast<uint32_t>(st.status);
		r_state.status_name = sl_dlssg_status_to_name(st.status);
		r_state.num_frames_presented = st.numFramesActuallyPresented;
		r_state.num_frames_to_generate_max = MAX(1u, st.numFramesToGenerateMax);
		r_state.dynamic_mfg_supported = (st.bIsDynamicMFGSupported == sl::Boolean::eTrue);
		r_state.estimated_vram_bytes = st.estimatedVRAMUsageInBytes;
		return true;
	}

	void notify_fg_active(bool p_active, bool p_dynamic) override {
		dlss_streamline_set_fg_active(p_active, p_dynamic);
	}
};

DLSSStreamlineFGOps g_sl_fg_ops;

#endif // DLSS_STREAMLINE_ENABLED && D3D12_ENABLED

} // namespace

bool dlss_streamline_should_load(bool p_is_editor, const String &p_driver_name, const String &p_fg_provider, int p_reflex_mode) {
	if (p_is_editor) {
		return false;
	}
	if (p_driver_name != "d3d12") {
		return false;
	}
	return p_fg_provider == DLSSFrameGeneration::PROVIDER_NAME || p_reflex_mode != 0;
}

DLSSReflexResolution dlss_resolve_reflex_state(
		int p_requested_mode,
		bool p_fg_active,
		bool p_fg_dynamic,
		const String &p_api_name,
		bool p_streamline_loaded,
		bool p_low_latency_available,
		int p_max_fps) {
	DLSSReflexResolution res;
	const int req = CLAMP(p_requested_mode, 0, 2);
	if (!p_fg_active && req == 0) {
		res.effective_mode = 0;
		res.reason = "OFF";
		return res;
	}
	if (p_api_name != "D3D12") {
		res.effective_mode = 0;
		res.reason = "API_UNSUPPORTED";
		return res;
	}
	if (!p_streamline_loaded || !p_low_latency_available) {
		res.effective_mode = 0;
		res.reason = "REFLEX_UNAVAILABLE";
		return res;
	}
	if (p_fg_active) {
		res.effective_mode = (req == 2) ? 2 : 1;
		res.forced_by_fg = (req == 0);
		res.reason = res.forced_by_fg ? "FORCED_ON_WITH_FG" : "OK";
	} else {
		res.effective_mode = req;
		res.reason = "OK";
	}
	if (p_max_fps > 0 && (res.effective_mode > 0 || p_fg_dynamic)) {
		res.frame_limit_us = static_cast<uint32_t>(MAX(1.0, Math::round(1000000.0 / double(p_max_fps))));
	}
	return res;
}

bool dlss_streamline_verify_interposer_manifest(const String &p_dir, String &r_interposer_path, String &r_error) {
	r_interposer_path = String();
	r_error = String();
	if (p_dir.is_empty()) {
		r_error = "empty executable directory";
		return false;
	}
	const String manifest_path = p_dir.path_join("dlss_runtime.json");
	if (!FileAccess::exists(manifest_path)) {
		r_error = "dlss_runtime.json not found next to executable";
		return false;
	}
	const String text = FileAccess::get_file_as_string(manifest_path);
	JSON json;
	if (json.parse(text) != OK || json.get_data().get_type() != Variant::DICTIONARY) {
		r_error = "invalid dlss_runtime.json";
		return false;
	}
	const Dictionary root = json.get_data();
	String interposer_name = String(root.get("interposer", String())).strip_edges();
	String expected_sha;

	const Variant files_var = root.get("files", Variant());
	if (files_var.get_type() == Variant::ARRAY) {
		const Array files = files_var;
		for (int i = 0; i < files.size(); i++) {
			if (files[i].get_type() != Variant::DICTIONARY) {
				continue;
			}
			const Dictionary item = files[i];
			const String name = String(item.get("name", String())).strip_edges();
			if (name.to_lower().contains("nvperf")) {
				continue;
			}
			const bool matches_explicit = (!interposer_name.is_empty() && name.to_lower() == interposer_name.to_lower());
			const bool matches_suffix = (interposer_name.is_empty() && name.to_lower().ends_with("interposer.dll"));
			if (matches_explicit || matches_suffix) {
				interposer_name = name;
				expected_sha = String(item.get("sha256", String())).strip_edges().to_lower();
				break;
			}
		}
	}

	if (interposer_name.is_empty() || expected_sha.length() != 64) {
		r_error = "interposer entry missing from dlss_runtime.json";
		return false;
	}

	const String candidate = p_dir.path_join(interposer_name);
	if (!FileAccess::exists(candidate)) {
		r_error = vformat("interposer binary '%s' missing next to executable", interposer_name);
		return false;
	}

	const String actual_sha = FileAccess::get_sha256(candidate).to_lower();
	if (actual_sha != expected_sha) {
		r_error = vformat("interposer SHA-256 mismatch for '%s'", interposer_name);
		return false;
	}

	r_interposer_path = candidate;
	return true;
}

bool dlss_streamline_init_if_requested() {
	MutexLock lock(g_sl_mutex);
	if (g_sl_loaded) {
		return true;
	}

	const bool is_editor = Engine::get_singleton() != nullptr && Engine::get_singleton()->is_editor_hint();
	const String driver = OS::get_singleton() != nullptr ? OS::get_singleton()->get_current_rendering_driver_name() : String();
	const String fg_provider = sl_read_setting<String>("rendering/frame_generation/provider", String());
	const int reflex_mode = CLAMP(sl_read_setting<int>("rendering/latency/reflex_mode", 0), 0, 2);

	RenderingServer::set_latency_status_callback(dlss_streamline_get_latency_status);

	if (reflex_mode != 0 && driver != "d3d12" && !is_editor && !g_reflex_api_unsupported_logged) {
		g_reflex_api_unsupported_logged = true;
		WARN_PRINT("NVIDIA Reflex is supported on D3D12 only (API_UNSUPPORTED).");
	}

	if (!dlss_streamline_should_load(is_editor, driver, fg_provider, reflex_mode)) {
		return false;
	}

#if defined(DLSS_STREAMLINE_ENABLED) && defined(D3D12_ENABLED)
	const String exe_dir = OS::get_singleton() != nullptr ? OS::get_singleton()->get_executable_path().get_base_dir() : String();
	String interposer_path;
	String verify_err;
	if (!dlss_streamline_verify_interposer_manifest(exe_dir, interposer_path, verify_err)) {
		g_sl_init_failed = true;
		WARN_PRINT(vformat("Streamline unavailable (STREAMLINE_UNAVAILABLE): %s", verify_err));
		return false;
	}

	const Error ovr_err = D3D12Hooks::set_library_override(interposer_path, interposer_path);
	if (ovr_err != OK) {
		g_sl_init_failed = true;
		WARN_PRINT(vformat("Streamline unavailable (STREAMLINE_UNAVAILABLE): D3D12Hooks::set_library_override failed (%d).", int(ovr_err)));
		return false;
	}

	String app_data_dir = OS::get_singleton() != nullptr ? OS::get_singleton()->get_user_data_dir().path_join("streamline") : String();
	if (!app_data_dir.is_empty()) {
		DirAccess::make_dir_recursive_absolute(app_data_dir);
	}
	g_sl_plugin_dir_w = sl_string_to_wstring(exe_dir);
	g_sl_app_data_w = sl_string_to_wstring(app_data_dir);
	g_sl_plugin_paths[0] = g_sl_plugin_dir_w.c_str();

	const String project_id = sl_read_setting<String>("rendering/scaling_3d/dlss/project_id", String()).strip_edges();
	const int64_t app_id = sl_read_setting<int64_t>("rendering/scaling_3d/dlss/application_id", 0);
	const bool allow_ota = sl_read_setting<bool>("rendering/scaling_3d/dlss/allow_ota_updates", false);
	g_sl_project_id_utf8 = project_id.utf8();
	g_sl_engine_version_utf8 = String(GODOT_VERSION_FULL_BUILD).utf8();

	static const sl::Feature features_to_load[] = {
		sl::kFeatureDLSS_G,
		sl::kFeatureReflex,
		sl::kFeaturePCL,
	};

	sl::Preferences pref;
	pref.showConsole = false;
	pref.logLevel = (OS::get_singleton() != nullptr && OS::get_singleton()->is_stdout_verbose()) ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
	pref.pathsToPlugins = g_sl_plugin_paths;
	pref.numPathsToPlugins = 1;
	pref.pathToLogsAndData = g_sl_app_data_w.empty() ? nullptr : g_sl_app_data_w.c_str();
	pref.logMessageCallback = sl_log_callback;
	pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
	if (allow_ota) {
		pref.flags |= sl::PreferenceFlags::eAllowOTA | sl::PreferenceFlags::eLoadDownloadedPlugins;
	}
	pref.featuresToLoad = features_to_load;
	pref.numFeaturesToLoad = 3;
	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = g_sl_engine_version_utf8.get_data();
	if (app_id > 0) {
		pref.applicationId = static_cast<uint32_t>(app_id);
	} else if (!project_id.is_empty()) {
		pref.projectId = g_sl_project_id_utf8.get_data();
	}
	pref.renderAPI = sl::RenderAPI::eD3D12;

	const sl::Result init_res = slInit(pref, sl::kSDKVersion);
	if (init_res != sl::Result::eOk) {
		D3D12Hooks::set_library_override(String(), String());
		g_sl_init_failed = true;
		WARN_PRINT(vformat("Streamline unavailable (STREAMLINE_UNAVAILABLE): slInit failed (%s).", sl_result_to_name(init_res)));
		return false;
	}

	g_sl_loaded = true;
	g_sl_init_failed = false;
	g_sl_device_set = false;
	g_sl_has_adapter_luid = false;
	D3D12Hooks::set_device_created_callback(_sl_on_d3d12_device_created, nullptr);
	dlss_ngx_d3d12_set_native_device_unwrap(sl_unwrap_native_interface);
	dlss_ngx_d3d12_set_native_command_list_unwrap(sl_unwrap_native_interface);
	set_latency_marker_callback(_sl_latency_marker_callback);
	print_verbose("Streamline initialized on D3D12 (DLSS-G, Reflex, PCL).");
	return true;
#else
	g_sl_init_failed = true;
	WARN_PRINT("Streamline unavailable (STREAMLINE_UNAVAILABLE): Streamline SDK was not compiled into this build.");
	return false;
#endif
}

void dlss_streamline_shutdown() {
	MutexLock lock(g_sl_mutex);
	if (RenderingServer::get_latency_status_callback() == dlss_streamline_get_latency_status) {
		RenderingServer::set_latency_status_callback(nullptr);
	}
	g_mock_marker_target = nullptr;
#if defined(DLSS_STREAMLINE_ENABLED) && defined(D3D12_ENABLED)
	if (get_latency_marker_callback() == _sl_latency_marker_callback) {
		set_latency_marker_callback(nullptr);
	}
	dlss_ngx_d3d12_set_native_device_unwrap(nullptr);
	dlss_ngx_d3d12_set_native_command_list_unwrap(nullptr);
	if (g_sl_loaded) {
		slShutdown();
		g_sl_loaded = false;
	}
	if (!D3D12Hooks::is_registration_locked()) {
		D3D12Hooks::set_device_created_callback(nullptr, nullptr);
		D3D12Hooks::set_library_override(String(), String());
	}
	g_sl_device_set = false;
	g_sl_has_adapter_luid = false;
	g_sl_current_token = nullptr;
	g_sl_token_frame_id = UINT64_MAX;
	g_sl_reflex_state_queried = false;
	g_sl_reflex_low_latency_available = false;
	g_sl_applied_reflex_mode = -1;
	g_sl_applied_frame_limit_us = UINT32_MAX;
#endif
	g_sl_init_failed = false;
	g_fg_active = false;
	g_fg_dynamic = false;
	g_reflex_owns_fps_cap = false;
}

bool dlss_streamline_is_loaded() {
	MutexLock lock(g_sl_mutex);
	return g_sl_loaded;
}

bool dlss_streamline_init_failed() {
	MutexLock lock(g_sl_mutex);
	return g_sl_init_failed;
}

void dlss_streamline_set_fg_active(bool p_active, bool p_dynamic) {
	MutexLock lock(g_sl_mutex);
	g_fg_active = p_active;
	g_fg_dynamic = p_dynamic;
#if defined(DLSS_STREAMLINE_ENABLED) && defined(D3D12_ENABLED)
	_sl_sync_reflex_options_locked();
#endif
}

void dlss_streamline_sync_reflex_options() {
	MutexLock lock(g_sl_mutex);
#if defined(DLSS_STREAMLINE_ENABLED) && defined(D3D12_ENABLED)
	_sl_sync_reflex_options_locked();
#endif
}

Dictionary dlss_streamline_get_latency_status() {
	MutexLock lock(g_sl_mutex);
	const int req_mode = CLAMP(sl_read_setting<int>("rendering/latency/reflex_mode", 0), 0, 2);
	const int max_fps = MAX(0, sl_read_setting<int>("application/run/max_fps", 0));
	const String api_name = sl_current_api_name();
	bool low_latency_avail = false;
#if defined(DLSS_STREAMLINE_ENABLED) && defined(D3D12_ENABLED)
	if (g_sl_loaded && RD::get_singleton() != nullptr) {
		_sl_sync_reflex_options_locked();
		low_latency_avail = g_sl_reflex_low_latency_available;
	}
#endif
	const DLSSReflexResolution res = dlss_resolve_reflex_state(
			req_mode,
			g_fg_active,
			g_fg_dynamic,
			api_name,
			g_sl_loaded,
			low_latency_avail,
			max_fps);
	Dictionary out;
	out["mode"] = res.effective_mode;
	out["reason"] = res.reason;
	return out;
}

DLSSFrameGenBackendOps *dlss_streamline_get_fg_ops() {
#if defined(DLSS_STREAMLINE_ENABLED) && defined(D3D12_ENABLED)
	return &g_sl_fg_ops;
#else
	return nullptr;
#endif
}

void dlss_streamline_install_mock_marker_callback(DLSSBackendMock *p_mock) {
	MutexLock lock(g_sl_mutex);
	g_mock_marker_target = p_mock;
	set_latency_marker_callback(p_mock != nullptr ? _mock_latency_marker_cb : nullptr);
}

void dlss_streamline_reset_log_latches_for_tests() {
	MutexLock lock(g_sl_mutex);
	g_reflex_forced_logged = false;
	g_reflex_api_unsupported_logged = false;
	g_reflex_unavailable_logged = false;
}
