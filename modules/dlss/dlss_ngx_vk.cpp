/**************************************************************************/
/*  dlss_ngx_vk.cpp                                                       */
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

// GodotNext: pu_dlss_sr_effect (#100).
// NGX Vulkan backend implementation. This is the only Vulkan translation unit that includes
// NVIDIA NGX SDK headers (resolved from dlss_sdk_path at build time).

#include "dlss_backend.h"
#include "dlss_ngx_rr.h"

#ifdef DLSS_BACKEND_NGX

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/memory.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/templates/hash_map.h"
#include "core/templates/pair.h"
#include "core/version.h"

#include <drivers/vulkan/godot_vulkan.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_vk.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#ifdef WINDOWS_ENABLED
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <psapi.h>
#endif

#include <cstring>
#include <fstream>
#include <string>

namespace {

constexpr uint32_t DLSS_VENDOR_NVIDIA = 0x10DE;

std::wstring string_to_wstring(const String &p_str) {
	std::wstring out;
	const int len = p_str.length();
	out.reserve(len);
	for (int i = 0; i < len; i++) {
		out.push_back(static_cast<wchar_t>(p_str[i]));
	}
	return out;
}

String wstring_to_string(const wchar_t *p_wstr) {
	if (p_wstr == nullptr) {
		return String();
	}
	String out;
	while (*p_wstr != 0) {
		out += static_cast<char32_t>(*p_wstr);
		p_wstr++;
	}
	return out;
}

NVSDK_NGX_PerfQuality_Value quality_to_ngx(DLSSBackend::QualityMode p_mode) {
	switch (p_mode) {
		case DLSSBackend::QUALITY_MAX_PERFORMANCE:
			return NVSDK_NGX_PerfQuality_Value_MaxPerf;
		case DLSSBackend::QUALITY_BALANCED:
			return NVSDK_NGX_PerfQuality_Value_Balanced;
		case DLSSBackend::QUALITY_MAX_QUALITY:
			return NVSDK_NGX_PerfQuality_Value_MaxQuality;
		case DLSSBackend::QUALITY_ULTRA_PERFORMANCE:
			return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
		case DLSSBackend::QUALITY_DLAA:
			return NVSDK_NGX_PerfQuality_Value_DLAA;
		default:
			return NVSDK_NGX_PerfQuality_Value_MaxQuality;
	}
}

NVSDK_NGX_DLSS_Hint_Render_Preset preset_to_ngx(DLSSBackend::Preset p_preset) {
	switch (p_preset) {
		case DLSSBackend::PRESET_J:
			return NVSDK_NGX_DLSS_Hint_Render_Preset_J;
		case DLSSBackend::PRESET_K:
			return NVSDK_NGX_DLSS_Hint_Render_Preset_K;
		case DLSSBackend::PRESET_L:
			return NVSDK_NGX_DLSS_Hint_Render_Preset_L;
		case DLSSBackend::PRESET_M:
			return NVSDK_NGX_DLSS_Hint_Render_Preset_M;
		case DLSSBackend::PRESET_DEFAULT:
		default:
			return NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
	}
}

DLSSBackend::Status map_ngx_result_to_status(NVSDK_NGX_Result p_res, bool p_for_init) {
	if (NVSDK_NGX_SUCCEED(p_res)) {
		return DLSSBackend::OK;
	}
	switch (p_res) {
		case NVSDK_NGX_Result_FAIL_FeatureNotSupported:
			return DLSSBackend::NOT_NVIDIA_RTX;
		case NVSDK_NGX_Result_FAIL_OutOfDate:
			return DLSSBackend::DRIVER_TOO_OLD;
		case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature:
		case NVSDK_NGX_Result_FAIL_FeatureNotFound:
			return p_for_init ? DLSSBackend::RUNTIME_MISSING : DLSSBackend::FEATURE_CREATE_FAILED;
		case NVSDK_NGX_Result_FAIL_PlatformError:
			return p_for_init ? DLSSBackend::RUNTIME_MISSING : DLSSBackend::FEATURE_CREATE_FAILED;
		default:
			return DLSSBackend::FEATURE_CREATE_FAILED;
	}
}

void NVSDK_CONV ngx_app_log_callback(const char *p_message, NVSDK_NGX_Logging_Level p_level, NVSDK_NGX_Feature p_source) {
	if (p_message == nullptr || p_message[0] == '\0') {
		return;
	}
	String msg = String::utf8(p_message).strip_edges();
	if (!msg.is_empty()) {
		print_verbose(vformat("NGX[lvl=%d src=%d]: %s", int(p_level), int(p_source), msg));
	}
}

NVSDK_NGX_Resource_VK to_ngx_vk_resource(const DLSSBackend::TextureResource &p_tex) {
	VkImageSubresourceRange range = {};
	VkImageAspectFlags aspect = 0;
	if (p_tex.aspect_mask & 0x1u) {
		aspect |= VK_IMAGE_ASPECT_COLOR_BIT;
	}
	if (p_tex.aspect_mask & 0x2u) {
		aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
	}
	if (p_tex.aspect_mask & 0x4u) {
		aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
	}
	if (aspect == 0) {
		aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	}
	range.aspectMask = aspect;
	range.baseMipLevel = p_tex.base_mip_level;
	range.levelCount = MAX(1u, p_tex.mip_levels);
	range.baseArrayLayer = p_tex.base_array_layer;
	range.layerCount = MAX(1u, p_tex.array_layers);

	return NVSDK_NGX_Create_ImageView_Resource_VK(
			reinterpret_cast<VkImageView>(p_tex.image_view_handle),
			reinterpret_cast<VkImage>(p_tex.image_handle),
			range,
			static_cast<VkFormat>(p_tex.format),
			p_tex.width,
			p_tex.height,
			p_tex.read_write);
}

NVSDK_NGX_DLSS_Create_Params build_dlss_create_params(const DLSSBackend::FeatureCreateParams &p_params) {
	NVSDK_NGX_DLSS_Create_Params out = {};
	out.Feature.InWidth = uint32_t(MAX(1, p_params.render_size.width));
	out.Feature.InHeight = uint32_t(MAX(1, p_params.render_size.height));
	out.Feature.InTargetWidth = uint32_t(MAX(1, p_params.output_size.width));
	out.Feature.InTargetHeight = uint32_t(MAX(1, p_params.output_size.height));
	out.Feature.InPerfQualityValue = quality_to_ngx(p_params.quality_mode);

	int flags = NVSDK_NGX_DLSS_Feature_Flags_None;
	if (p_params.is_hdr) {
		flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
	}
	if (p_params.depth_inverted) {
		flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
	}
	if (p_params.mv_low_res) {
		flags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
	}
	if (p_params.mv_jittered) {
		flags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
	}
	if (p_params.auto_exposure) {
		flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
	}
	out.InFeatureCreateFlags = flags;
	out.InEnableOutputSubrects = false;
	return out;
}

NVSDK_NGX_VK_DLSS_Eval_Params build_vk_dlss_eval_params(
		const DLSSBackend::EvaluateParams &p_params,
		NVSDK_NGX_Resource_VK *p_color,
		NVSDK_NGX_Resource_VK *p_depth,
		NVSDK_NGX_Resource_VK *p_mv,
		NVSDK_NGX_Resource_VK *p_output) {
	NVSDK_NGX_VK_DLSS_Eval_Params out = {};
	out.Feature.pInColor = p_color;
	out.Feature.pInOutput = p_output;
	out.Feature.InSharpness = 0.0f;
	out.pInDepth = p_depth;
	out.pInMotionVectors = p_mv;
	out.InJitterOffsetX = p_params.jitter_offset.x;
	out.InJitterOffsetY = p_params.jitter_offset.y;
	out.InRenderSubrectDimensions.Width = uint32_t(MAX(1, p_params.render_size.width));
	out.InRenderSubrectDimensions.Height = uint32_t(MAX(1, p_params.render_size.height));
	out.InReset = p_params.reset ? 1 : 0;
	out.InMVScaleX = p_params.mv_scale.x;
	out.InMVScaleY = p_params.mv_scale.y;
	out.pInTransparencyMask = nullptr;
	out.pInExposureTexture = nullptr;
	out.pInBiasCurrentColorMask = nullptr;
	out.InPreExposure = p_params.pre_exposure;
	out.InExposureScale = p_params.exposure_scale;
	out.InFrameTimeDeltaInMsec = p_params.frame_time_delta_ms;
	return out;
}

String pick_writable_app_data_dir(const String &p_preferred, const String &p_fallback) {
	String chosen = p_preferred;
	if (chosen.is_empty()) {
		chosen = p_fallback;
	}
	if (!chosen.is_empty()) {
		DirAccess::make_dir_recursive_absolute(chosen);
		return chosen;
	}
#ifdef UNIX_ENABLED
	return "/tmp";
#else
	return ".";
#endif
}

struct ManifestRuntimeEntry {
	String basename;
	String sha256;
	String sdk_tag;
	Vector<Pair<String, String>> all_files;
};

ManifestRuntimeEntry read_sr_runtime_from_manifest(const String &p_search_dir) {
	ManifestRuntimeEntry entry;
	if (p_search_dir.is_empty()) {
		return entry;
	}
	const String manifest_path = p_search_dir.path_join("dlss_runtime.json");
	if (!FileAccess::exists(manifest_path)) {
		return entry;
	}
	const String text = FileAccess::get_file_as_string(manifest_path);
	if (text.is_empty()) {
		return entry;
	}
	JSON json;
	if (json.parse(text) != OK) {
		return entry;
	}
	const Variant parsed = json.get_data();
	if (parsed.get_type() != Variant::DICTIONARY) {
		return entry;
	}
	const Dictionary root = parsed;
	entry.sdk_tag = root.get("sdk_tag", String());

	const Variant files_var = root.get("files", Variant());
	if (files_var.get_type() == Variant::ARRAY) {
		const Array files = files_var;
		for (int i = 0; i < files.size(); i++) {
			if (files[i].get_type() != Variant::DICTIONARY) {
				continue;
			}
			const Dictionary item = files[i];
			const String name = item.get("name", String());
			const String sha = String(item.get("sha256", String())).to_lower();
			if (!name.is_empty()) {
				entry.all_files.push_back(Pair<String, String>(name, sha));
			}
			// Prefer the SR runtime (does not carry "dlssd" or "dlssg" suffix).
			if (entry.basename.is_empty() && !name.is_empty() && !name.contains("dlssd") && !name.contains("dlssg") && !name.begins_with("sl.")) {
				entry.basename = name;
				entry.sha256 = sha;
			}
		}
		if (!entry.basename.is_empty()) {
			return entry;
		}
	}

	const Variant sha_map_var = root.get("sha256", Variant());
	if (sha_map_var.get_type() == Variant::DICTIONARY) {
		const Dictionary sha_map = sha_map_var;
		const Array keys = sha_map.keys();
		for (int i = 0; i < keys.size(); i++) {
			const String name = keys[i];
			const String sha = String(sha_map[name]).to_lower();
			if (!name.is_empty()) {
				entry.all_files.push_back(Pair<String, String>(name, sha));
			}
			if (entry.basename.is_empty() && !name.is_empty() && !name.contains("dlssd") && !name.contains("dlssg") && !name.begins_with("sl.")) {
				entry.basename = name;
				entry.sha256 = sha;
			}
		}
	}
	return entry;
}

String find_loaded_module_by_basename(const String &p_basename) {
	if (p_basename.is_empty()) {
		return String();
	}
	const String target_lower = p_basename.to_lower();

#if defined(WINDOWS_ENABLED)
	HMODULE modules[1024];
	DWORD needed = 0;
	HANDLE proc = GetCurrentProcess();
	if (EnumProcessModules(proc, modules, sizeof(modules), &needed)) {
		const DWORD count = MIN<DWORD>(needed / sizeof(HMODULE), 1024);
		wchar_t path_buf[MAX_PATH * 4];
		for (DWORD i = 0; i < count; i++) {
			const DWORD len = GetModuleFileNameW(modules[i], path_buf, uint32_t(sizeof(path_buf) / sizeof(path_buf[0])));
			if (len == 0) {
				continue;
			}
			const String full_path = wstring_to_string(path_buf);
			if (full_path.get_file().to_lower() == target_lower) {
				return full_path;
			}
		}
	}
#elif defined(LINUXBSD_ENABLED)
	std::ifstream maps("/proc/self/maps");
	std::string raw_line;
	while (std::getline(maps, raw_line)) {
		const size_t slash_pos = raw_line.find('/');
		if (slash_pos == std::string::npos) {
			continue;
		}
		String mapped_path = String::utf8(raw_line.c_str() + slash_pos).strip_edges();
		if (mapped_path.begins_with("/memfd:")) {
			mapped_path = mapped_path.substr(7);
		}
		if (mapped_path.ends_with(" (deleted)")) {
			mapped_path = mapped_path.substr(0, mapped_path.length() - 10);
		}
		const String file_lower = mapped_path.get_file().to_lower();
		if (file_lower == target_lower || file_lower.begins_with(target_lower)) {
			return mapped_path;
		}
	}
#endif
	return String();
}

class DLSSBackendNGXVulkan : public DLSSBackend {
	mutable Mutex mutex;

	bool initialized = false;
	bool ota_updates_allowed = false;
	bool ota_update_attempted = false;
	bool ota_rr_update_attempted = false;
	VkDevice vk_device = VK_NULL_HANDLE;
	NVSDK_NGX_Parameter *capability_params = nullptr;
	NVSDK_NGX_Parameter *eval_params = nullptr;
	NVSDK_NGX_Application_Identifier app_identifier = {};
	CharString project_id_utf8;
	CharString engine_version_utf8;
	std::wstring app_data_wstr;
	std::wstring search_path_wstr;
	const wchar_t *search_paths[1] = { nullptr };
	NVSDK_NGX_FeatureCommonInfo common_info = {};
	String runtime_search_dir;

	// Maps NVSDK_NGX_Handle* -> true when the handle is a Ray Reconstruction (DLSSD) feature.
	HashMap<uint64_t, bool> live_handles;
	uint64_t cached_memory_bytes = 0;
	int32_t last_result = static_cast<int32_t>(NVSDK_NGX_Result_Success);
	LoadedRuntimeInfo loaded_runtime;

	void _prepare_discovery_locked(NVSDK_NGX_FeatureDiscoveryInfo &r_info, std::wstring &r_app_data, std::wstring &r_search_dir, const wchar_t *&r_search_ptr, NVSDK_NGX_FeatureCommonInfo &r_common, NVSDK_NGX_Feature p_feature = NVSDK_NGX_Feature_SuperSampling) const {
		String user_dir;
		String exe_dir;
		if (OS::get_singleton() != nullptr) {
			user_dir = OS::get_singleton()->get_user_data_dir();
			exe_dir = OS::get_singleton()->get_executable_path().get_base_dir();
		}
		r_app_data = string_to_wstring(pick_writable_app_data_dir(user_dir, exe_dir));
		r_search_dir = string_to_wstring(exe_dir);
		r_search_ptr = r_search_dir.empty() ? nullptr : r_search_dir.c_str();

		r_common = {};
		if (r_search_ptr != nullptr) {
			r_common.PathListInfo.Path = &r_search_ptr;
			r_common.PathListInfo.Length = 1;
		}
		r_common.LoggingInfo.LoggingCallback = ngx_app_log_callback;
		r_common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
		r_common.LoggingInfo.DisableOtherLoggingSinks = false;

		r_info = {};
		r_info.SDKVersion = NVSDK_NGX_Version_API;
		r_info.FeatureID = p_feature;
		r_info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
		r_info.Identifier.v.ProjectDesc.ProjectId = "00000000-0000-0000-0000-000000000000";
		r_info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
		r_info.Identifier.v.ProjectDesc.EngineVersion = GODOT_VERSION_FULL_BUILD;
		r_info.ApplicationDataPath = r_app_data.c_str();
		r_info.FeatureInfo = &r_common;
	}

	void _refresh_vram_locked() {
		if (!initialized || capability_params == nullptr || live_handles.is_empty()) {
			cached_memory_bytes = 0;
			return;
		}
		bool has_sr = false;
		bool has_rr = false;
		for (const KeyValue<uint64_t, bool> &E : live_handles) {
			if (E.value) {
				has_rr = true;
			} else {
				has_sr = true;
			}
		}
		uint64_t total_bytes = 0;
		if (has_sr) {
			void *callback = nullptr;
			NVSDK_NGX_Parameter_GetVoidPointer(capability_params, NVSDK_NGX_Parameter_DLSSGetStatsCallback, &callback);
			if (callback != nullptr) {
				PFN_NVSDK_NGX_DLSS_GetStatsCallback fn = reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetStatsCallback>(callback);
				const NVSDK_NGX_Result res = fn(capability_params);
				if (NVSDK_NGX_SUCCEED(res)) {
					unsigned long long vram_bytes = 0;
					if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetULL(capability_params, NVSDK_NGX_Parameter_SizeInBytes, &vram_bytes))) {
						total_bytes += static_cast<uint64_t>(vram_bytes);
					}
				}
			}
		}
		if (has_rr) {
			total_bytes += dlss_ngx_vk_rr_get_vram_bytes(capability_params);
		}
		cached_memory_bytes = total_bytes;
	}

	void _refresh_loaded_runtime_info_locked() {
		if (!loaded_runtime.path.is_empty()) {
			return;
		}
		const ManifestRuntimeEntry manifest = read_sr_runtime_from_manifest(runtime_search_dir);
		loaded_runtime.version = manifest.sdk_tag;
		loaded_runtime.manifest_sha256 = manifest.sha256;

		String found = find_loaded_module_by_basename(manifest.basename);
		if (found.is_empty() && !manifest.basename.is_empty()) {
			const String candidate = runtime_search_dir.path_join(manifest.basename);
			if (FileAccess::exists(candidate)) {
				found = candidate;
			}
		}
		if (!found.is_empty() && FileAccess::exists(found)) {
			loaded_runtime.path = found;
			loaded_runtime.sha256 = FileAccess::get_sha256(found).to_lower();
			loaded_runtime.sha256_matches = !manifest.sha256.is_empty() && loaded_runtime.sha256 == manifest.sha256;
		} else {
			print_verbose(vformat("DLSS runtime lookup miss: dir=%s manifest_basename=%s found=%s", runtime_search_dir, manifest.basename, found));
		}

		// Record SHA-256 hashes of companion NGX runtimes (e.g. DLSS-RR) verified on disk
		// so the Vulkan validation allowlist matches exact loaded runtime binaries.
		loaded_runtime.additional_sha256s.clear();
		for (const Pair<String, String> &file_entry : manifest.all_files) {
			if (file_entry.first == manifest.basename) {
				continue;
			}
			String extra_found = find_loaded_module_by_basename(file_entry.first);
			if (extra_found.is_empty() && !file_entry.first.is_empty()) {
				const String candidate = runtime_search_dir.path_join(file_entry.first);
				if (FileAccess::exists(candidate)) {
					extra_found = candidate;
				}
			}
			if (!extra_found.is_empty() && FileAccess::exists(extra_found)) {
				const String disk_sha = FileAccess::get_sha256(extra_found).to_lower();
				if (!disk_sha.is_empty() && !loaded_runtime.additional_sha256s.has(disk_sha)) {
					loaded_runtime.additional_sha256s.push_back(disk_sha);
				}
			}
		}
	}

	void _cleanup_locked() {
		if (vk_device != VK_NULL_HANDLE && (initialized || !live_handles.is_empty())) {
			vkDeviceWaitIdle(vk_device);
		}
		for (const KeyValue<uint64_t, bool> &E : live_handles) {
			NVSDK_NGX_Handle *handle = reinterpret_cast<NVSDK_NGX_Handle *>(E.key);
			if (handle != nullptr) {
				NVSDK_NGX_VULKAN_ReleaseFeature(handle);
			}
		}
		live_handles.clear();

		if (eval_params != nullptr) {
			NVSDK_NGX_VULKAN_DestroyParameters(eval_params);
			eval_params = nullptr;
		}
		if (capability_params != nullptr) {
			NVSDK_NGX_VULKAN_DestroyParameters(capability_params);
			capability_params = nullptr;
		}
		if (initialized) {
			NVSDK_NGX_VULKAN_Shutdown1(vk_device);
			initialized = false;
		}
		vk_device = VK_NULL_HANDLE;
		cached_memory_bytes = 0;
		ota_update_attempted = false;
		ota_rr_update_attempted = false;
	}

public:
	~DLSSBackendNGXVulkan() override {
		shutdown();
	}

	String get_backend_name() const override { return "ngx"; }

	Vector<CharString> get_instance_extensions() override {
		MutexLock lock(mutex);
		Vector<CharString> result;
		const NVSDK_NGX_Feature features_to_query[2] = {
			NVSDK_NGX_Feature_SuperSampling,
			NVSDK_NGX_Feature_RayReconstruction,
		};
		for (NVSDK_NGX_Feature feat : features_to_query) {
			NVSDK_NGX_FeatureDiscoveryInfo discovery = {};
			std::wstring app_data;
			std::wstring search_dir;
			const wchar_t *search_ptr = nullptr;
			NVSDK_NGX_FeatureCommonInfo common = {};
			_prepare_discovery_locked(discovery, app_data, search_dir, search_ptr, common, feat);

			uint32_t ext_count = 0;
			VkExtensionProperties *ext_props = nullptr;
			const NVSDK_NGX_Result ngx_res = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&discovery, &ext_count, &ext_props);
			if (feat == NVSDK_NGX_Feature_SuperSampling) {
				last_result = static_cast<int32_t>(ngx_res);
			}
			if (NVSDK_NGX_SUCCEED(ngx_res) && ext_props != nullptr) {
				for (uint32_t i = 0; i < ext_count; i++) {
					if (ext_props[i].extensionName[0] != '\0') {
						const CharString name(ext_props[i].extensionName);
						bool duplicate = false;
						for (const CharString &existing : result) {
							if (std::strcmp(existing.get_data(), name.get_data()) == 0) {
								duplicate = true;
								break;
							}
						}
						if (!duplicate) {
							result.push_back(name);
						}
					}
				}
			}
		}
		return result;
	}

	Vector<CharString> get_device_extensions(uint64_t p_instance_handle, uint64_t p_physical_device_handle) override {
		MutexLock lock(mutex);
		Vector<CharString> result;
		VkInstance inst = reinterpret_cast<VkInstance>(p_instance_handle);
		VkPhysicalDevice phys = reinterpret_cast<VkPhysicalDevice>(p_physical_device_handle);
		if (inst == VK_NULL_HANDLE || phys == VK_NULL_HANDLE) {
			return result;
		}

		const NVSDK_NGX_Feature features_to_query[2] = {
			NVSDK_NGX_Feature_SuperSampling,
			NVSDK_NGX_Feature_RayReconstruction,
		};
		for (NVSDK_NGX_Feature feat : features_to_query) {
			NVSDK_NGX_FeatureDiscoveryInfo discovery = {};
			std::wstring app_data;
			std::wstring search_dir;
			const wchar_t *search_ptr = nullptr;
			NVSDK_NGX_FeatureCommonInfo common = {};
			_prepare_discovery_locked(discovery, app_data, search_dir, search_ptr, common, feat);

			uint32_t ext_count = 0;
			VkExtensionProperties *ext_props = nullptr;
			const NVSDK_NGX_Result ngx_res = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(inst, phys, &discovery, &ext_count, &ext_props);
			if (feat == NVSDK_NGX_Feature_SuperSampling) {
				last_result = static_cast<int32_t>(ngx_res);
				print_verbose(vformat("DLSS NGX GetFeatureDeviceExtensionRequirements: res=0x%08x count=%d", uint32_t(ngx_res), int(ext_count)));
			}
			if (NVSDK_NGX_SUCCEED(ngx_res) && ext_props != nullptr) {
				for (uint32_t i = 0; i < ext_count; i++) {
					if (ext_props[i].extensionName[0] != '\0') {
						const CharString name(ext_props[i].extensionName);
						bool duplicate = false;
						for (const CharString &existing : result) {
							if (std::strcmp(existing.get_data(), name.get_data()) == 0) {
								duplicate = true;
								break;
							}
						}
						if (!duplicate) {
							result.push_back(name);
						}
					}
				}
			}
		}
		if (result.is_empty()) {
			unsigned int req_inst_count = 0;
			const char **req_inst_exts = nullptr;
			unsigned int req_dev_count = 0;
			const char **req_dev_exts = nullptr;
			const NVSDK_NGX_Result req_res = NVSDK_NGX_VULKAN_RequiredExtensions(&req_inst_count, &req_inst_exts, &req_dev_count, &req_dev_exts);
			print_verbose(vformat("DLSS NGX RequiredExtensions fallback: res=0x%08x inst=%d dev=%d", uint32_t(req_res), int(req_inst_count), int(req_dev_count)));
			if (NVSDK_NGX_SUCCEED(req_res) && req_dev_exts != nullptr) {
				for (unsigned int i = 0; i < req_dev_count; i++) {
					if (req_dev_exts[i] != nullptr && req_dev_exts[i][0] != '\0') {
						result.push_back(CharString(req_dev_exts[i]));
					}
				}
			}
		}
		// Vulkan forbids enabling both VK_KHR_buffer_device_address (which Godot enables when supported)
		// and the superseded VK_EXT_buffer_device_address (VUID-VkDeviceCreateInfo-ppEnabledExtensionNames-03328).
		bool has_khr_bda = false;
		uint32_t avail_count = 0;
		if (vkEnumerateDeviceExtensionProperties(phys, nullptr, &avail_count, nullptr) == VK_SUCCESS && avail_count > 0) {
			Vector<VkExtensionProperties> avail;
			avail.resize(avail_count);
			if (vkEnumerateDeviceExtensionProperties(phys, nullptr, &avail_count, avail.ptrw()) == VK_SUCCESS) {
				for (uint32_t i = 0; i < avail_count; i++) {
					if (std::strcmp(avail[i].extensionName, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) == 0) {
						has_khr_bda = true;
						break;
					}
				}
			}
		}
		if (has_khr_bda) {
			Vector<CharString> filtered;
			for (const CharString &ext : result) {
				if (std::strcmp(ext.get_data(), VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) != 0) {
					filtered.push_back(ext);
				}
			}
			result = filtered;
		}
		return result;
	}

	Status query_availability(uint32_t p_vendor_id, bool p_extensions_satisfied) override {
		if (p_vendor_id != 0 && p_vendor_id != DLSS_VENDOR_NVIDIA) {
			return NOT_NVIDIA_RTX;
		}
		if (!p_extensions_satisfied) {
			return EXTENSION_MISSING;
		}
		return OK;
	}

	Status query_rr_availability() override {
		MutexLock lock(mutex);
		if (!initialized || capability_params == nullptr) {
			return RUNTIME_MISSING;
		}
		return dlss_ngx_vk_rr_query_availability(capability_params, last_result);
	}

	Status init(const InitParams &p_params) override {
		MutexLock lock(mutex);
		if (initialized) {
			return OK;
		}
		if (p_params.project_id.is_empty() && p_params.application_id == 0) {
			return PROJECT_ID_MISSING;
		}

		VkInstance instance = reinterpret_cast<VkInstance>(p_params.instance_handle);
		VkPhysicalDevice physical_device = reinterpret_cast<VkPhysicalDevice>(p_params.physical_device_handle);
		VkDevice device = reinterpret_cast<VkDevice>(p_params.device_handle);
		if (instance == VK_NULL_HANDLE || physical_device == VK_NULL_HANDLE || device == VK_NULL_HANDLE) {
			return FEATURE_CREATE_FAILED;
		}

		PFN_vkGetInstanceProcAddr gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(p_params.gipa_handle);
		PFN_vkGetDeviceProcAddr gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(p_params.gdpa_handle);

		runtime_search_dir = p_params.runtime_search_path;
		app_data_wstr = string_to_wstring(pick_writable_app_data_dir(p_params.application_data_path, p_params.runtime_search_path));
		search_path_wstr = string_to_wstring(p_params.runtime_search_path);
		search_paths[0] = search_path_wstr.empty() ? nullptr : search_path_wstr.c_str();

		common_info = {};
		if (search_paths[0] != nullptr) {
			common_info.PathListInfo.Path = search_paths;
			common_info.PathListInfo.Length = 1;
		}
		common_info.LoggingInfo.LoggingCallback = ngx_app_log_callback;
		common_info.LoggingInfo.MinimumLoggingLevel = (OS::get_singleton() != nullptr && OS::get_singleton()->is_stdout_verbose()) ? NVSDK_NGX_LOGGING_LEVEL_VERBOSE : NVSDK_NGX_LOGGING_LEVEL_ON;
		common_info.LoggingInfo.DisableOtherLoggingSinks = false;

		project_id_utf8 = p_params.project_id.utf8();
		engine_version_utf8 = (p_params.engine_version.is_empty() ? String(GODOT_VERSION_FULL_BUILD) : p_params.engine_version).utf8();
		ota_updates_allowed = p_params.allow_ota_updates;
		ota_update_attempted = false;

		app_identifier = {};
		NVSDK_NGX_Result res = NVSDK_NGX_Result_Fail;
		if (p_params.application_id != 0) {
			app_identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Application_Id;
			app_identifier.v.ApplicationId = p_params.application_id;
			res = NVSDK_NGX_VULKAN_Init(
					p_params.application_id,
					app_data_wstr.c_str(),
					instance,
					physical_device,
					device,
					gipa,
					gdpa,
					&common_info,
					NVSDK_NGX_Version_API);
		} else {
			app_identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
			app_identifier.v.ProjectDesc.ProjectId = project_id_utf8.get_data();
			app_identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
			app_identifier.v.ProjectDesc.EngineVersion = engine_version_utf8.get_data();
			res = NVSDK_NGX_VULKAN_Init_with_ProjectID(
					project_id_utf8.get_data(),
					NVSDK_NGX_ENGINE_TYPE_CUSTOM,
					engine_version_utf8.get_data(),
					app_data_wstr.c_str(),
					instance,
					physical_device,
					device,
					gipa,
					gdpa,
					&common_info,
					NVSDK_NGX_Version_API);
		}
		last_result = static_cast<int32_t>(res);
		if (NVSDK_NGX_FAILED(res)) {
			print_verbose(vformat("DLSS NGX Vulkan init failed: 0x%08x (%s)", uint32_t(res), wstring_to_string(GetNGXResultAsString(res))));
			return map_ngx_result_to_status(res, true);
		}

		initialized = true;
		vk_device = device;

		res = NVSDK_NGX_VULKAN_GetCapabilityParameters(&capability_params);
		last_result = static_cast<int32_t>(res);
		if (NVSDK_NGX_FAILED(res) || capability_params == nullptr) {
			_cleanup_locked();
			return map_ngx_result_to_status(res, true);
		}

		int needs_updated_driver = 0;
		unsigned int min_driver_major = 0;
		unsigned int min_driver_minor = 0;
		if (NVSDK_NGX_SUCCEED(capability_params->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_updated_driver)) && needs_updated_driver != 0) {
			capability_params->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &min_driver_major);
			capability_params->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &min_driver_minor);
			print_verbose(vformat("DLSS NGX reports driver too old (minimum %d.%02d).", int(min_driver_major), int(min_driver_minor)));
			_cleanup_locked();
			return DRIVER_TOO_OLD;
		}

		int dlss_available = 0;
		const NVSDK_NGX_Result avail_query = capability_params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &dlss_available);
		if (NVSDK_NGX_FAILED(avail_query) || dlss_available == 0) {
			int feature_init_res = static_cast<int>(NVSDK_NGX_Result_FAIL_UnableToInitializeFeature);
			capability_params->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &feature_init_res);
			last_result = feature_init_res;
			const NVSDK_NGX_Result init_res_enum = static_cast<NVSDK_NGX_Result>(feature_init_res);
			print_verbose(vformat("DLSS NGX SuperSampling unavailable: FeatureInitResult=0x%08x (%s)", uint32_t(feature_init_res), wstring_to_string(GetNGXResultAsString(init_res_enum))));
			_cleanup_locked();
			if (init_res_enum == NVSDK_NGX_Result_FAIL_OutOfDate) {
				return DRIVER_TOO_OLD;
			}
			if (init_res_enum == NVSDK_NGX_Result_FAIL_FeatureNotSupported) {
				return NOT_NVIDIA_RTX;
			}
			return RUNTIME_MISSING;
		}

		res = NVSDK_NGX_VULKAN_AllocateParameters(&eval_params);
		last_result = static_cast<int32_t>(res);
		if (NVSDK_NGX_FAILED(res) || eval_params == nullptr) {
			_cleanup_locked();
			return map_ngx_result_to_status(res, true);
		}

		return OK;
	}

	Status get_optimal_settings(const Size2i &p_output_size, QualityMode p_quality_mode, OptimalSettings &r_settings) override {
		MutexLock lock(mutex);
		if (!initialized || capability_params == nullptr) {
			return FEATURE_CREATE_FAILED;
		}
		void *callback = nullptr;
		NVSDK_NGX_Parameter_GetVoidPointer(capability_params, NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &callback);
		if (callback == nullptr) {
			return DRIVER_TOO_OLD;
		}

		NVSDK_NGX_Parameter_SetUI(capability_params, NVSDK_NGX_Parameter_Width, uint32_t(MAX(1, p_output_size.width)));
		NVSDK_NGX_Parameter_SetUI(capability_params, NVSDK_NGX_Parameter_Height, uint32_t(MAX(1, p_output_size.height)));
		NVSDK_NGX_Parameter_SetI(capability_params, NVSDK_NGX_Parameter_PerfQualityValue, quality_to_ngx(p_quality_mode));
		NVSDK_NGX_Parameter_SetI(capability_params, NVSDK_NGX_Parameter_RTXValue, 0);

		PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback fn = reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback>(callback);
		const NVSDK_NGX_Result res = fn(capability_params);
		last_result = static_cast<int32_t>(res);
		if (NVSDK_NGX_FAILED(res)) {
			return map_ngx_result_to_status(res, false);
		}

		unsigned int opt_w = 0, opt_h = 0;
		NVSDK_NGX_Parameter_GetUI(capability_params, NVSDK_NGX_Parameter_OutWidth, &opt_w);
		NVSDK_NGX_Parameter_GetUI(capability_params, NVSDK_NGX_Parameter_OutHeight, &opt_h);

		unsigned int min_w = opt_w, min_h = opt_h, max_w = opt_w, max_h = opt_h;
		NVSDK_NGX_Parameter_GetUI(capability_params, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, &min_w);
		NVSDK_NGX_Parameter_GetUI(capability_params, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, &min_h);
		NVSDK_NGX_Parameter_GetUI(capability_params, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, &max_w);
		NVSDK_NGX_Parameter_GetUI(capability_params, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, &max_h);

		float sharpness = 0.0f;
		NVSDK_NGX_Parameter_GetF(capability_params, NVSDK_NGX_Parameter_Sharpness, &sharpness);

		r_settings.optimal_size = Size2i(int(opt_w), int(opt_h));
		r_settings.min_size = Size2i(int(min_w), int(min_h));
		r_settings.max_size = Size2i(int(max_w), int(max_h));
		r_settings.sharpness = sharpness;
		return OK;
	}

	Status create_feature(const FeatureCreateParams &p_params, uint64_t &r_feature_handle) override {
		r_feature_handle = 0;
		MutexLock lock(mutex);
		if (!initialized || vk_device == VK_NULL_HANDLE) {
			return FEATURE_CREATE_FAILED;
		}
		VkCommandBuffer cmd_buf = reinterpret_cast<VkCommandBuffer>(p_params.command_buffer_handle);
		if (cmd_buf == VK_NULL_HANDLE) {
			return FEATURE_CREATE_FAILED;
		}

		if (p_params.ray_reconstruction) {
			if (ota_updates_allowed && !ota_rr_update_attempted) {
				ota_rr_update_attempted = true;
				const NVSDK_NGX_Result ota_res = NVSDK_NGX_UpdateFeature(&app_identifier, NVSDK_NGX_Feature_RayReconstruction);
				print_verbose(vformat("DLSS NGX UpdateFeature(RayReconstruction): 0x%08x", uint32_t(ota_res)));
			}
			uint64_t rr_handle = 0;
			const Status rr_st = dlss_ngx_vk_rr_create_feature(
					reinterpret_cast<uint64_t>(vk_device),
					p_params.command_buffer_handle,
					p_params,
					rr_handle,
					last_result);
			if (rr_st != OK || rr_handle == 0) {
				return rr_st != OK ? rr_st : FEATURE_CREATE_FAILED;
			}
			live_handles[rr_handle] = true;
			r_feature_handle = rr_handle;
			_refresh_vram_locked();
			_refresh_loaded_runtime_info_locked();
			return OK;
		}

		if (ota_updates_allowed && !ota_update_attempted) {
			ota_update_attempted = true;
			const NVSDK_NGX_Result ota_res = NVSDK_NGX_UpdateFeature(&app_identifier, NVSDK_NGX_Feature_SuperSampling);
			print_verbose(vformat("DLSS NGX UpdateFeature(SuperSampling): 0x%08x", uint32_t(ota_res)));
		}

		NVSDK_NGX_Parameter *create_bag = nullptr;
		NVSDK_NGX_Result res = NVSDK_NGX_VULKAN_AllocateParameters(&create_bag);
		last_result = static_cast<int32_t>(res);
		if (NVSDK_NGX_FAILED(res) || create_bag == nullptr) {
			return FEATURE_CREATE_FAILED;
		}

		const unsigned int preset_val = static_cast<unsigned int>(preset_to_ngx(p_params.preset));
		NVSDK_NGX_Parameter_SetUI(create_bag, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, preset_val);
		NVSDK_NGX_Parameter_SetUI(create_bag, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, preset_val);
		NVSDK_NGX_Parameter_SetUI(create_bag, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, preset_val);
		NVSDK_NGX_Parameter_SetUI(create_bag, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, preset_val);
		NVSDK_NGX_Parameter_SetUI(create_bag, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, preset_val);
		NVSDK_NGX_Parameter_SetUI(create_bag, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, preset_val);
		NVSDK_NGX_Parameter_SetI(create_bag, NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);

		NVSDK_NGX_DLSS_Create_Params dlss_create = build_dlss_create_params(p_params);
		NVSDK_NGX_Handle *ngx_handle = nullptr;
		res = NGX_VULKAN_CREATE_DLSS_EXT1(vk_device, cmd_buf, 1, 1, &ngx_handle, create_bag, &dlss_create);
		last_result = static_cast<int32_t>(res);
		NVSDK_NGX_VULKAN_DestroyParameters(create_bag);

		if (NVSDK_NGX_FAILED(res) || ngx_handle == nullptr) {
			print_verbose(vformat("DLSS NGX create_feature failed: 0x%08x (%s)", uint32_t(res), wstring_to_string(GetNGXResultAsString(res))));
			return map_ngx_result_to_status(res, false);
		}

		const uint64_t handle_val = reinterpret_cast<uint64_t>(ngx_handle);
		live_handles[handle_val] = false;
		r_feature_handle = handle_val;

		_refresh_vram_locked();
		_refresh_loaded_runtime_info_locked();
		return OK;
	}

	Status evaluate(const EvaluateParams &p_params) override {
		MutexLock lock(mutex);
		const bool *is_rr_ptr = live_handles.getptr(p_params.feature_handle);
		if (!initialized || eval_params == nullptr || p_params.feature_handle == 0 || is_rr_ptr == nullptr) {
			return FEATURE_CREATE_FAILED;
		}
		if (*is_rr_ptr) {
			return dlss_ngx_vk_rr_evaluate(
					p_params.command_buffer_handle,
					p_params.feature_handle,
					eval_params,
					p_params,
					last_result);
		}
		VkCommandBuffer cmd_buf = reinterpret_cast<VkCommandBuffer>(p_params.command_buffer_handle);
		if (cmd_buf == VK_NULL_HANDLE) {
			return FEATURE_CREATE_FAILED;
		}

		NVSDK_NGX_Resource_VK color_res = to_ngx_vk_resource(p_params.color);
		NVSDK_NGX_Resource_VK depth_res = to_ngx_vk_resource(p_params.depth);
		NVSDK_NGX_Resource_VK mv_res = to_ngx_vk_resource(p_params.velocity);
		NVSDK_NGX_Resource_VK out_res = to_ngx_vk_resource(p_params.output);

		NVSDK_NGX_VK_DLSS_Eval_Params dlss_eval = build_vk_dlss_eval_params(p_params, &color_res, &depth_res, &mv_res, &out_res);
		eval_params->Reset();
		const NVSDK_NGX_Result res = NGX_VULKAN_EVALUATE_DLSS_EXT(
				cmd_buf,
				reinterpret_cast<NVSDK_NGX_Handle *>(p_params.feature_handle),
				eval_params,
				&dlss_eval);
		last_result = static_cast<int32_t>(res);
		if (NVSDK_NGX_FAILED(res)) {
			print_verbose(vformat("DLSS NGX evaluate failed: 0x%08x (%s)", uint32_t(res), wstring_to_string(GetNGXResultAsString(res))));
			return map_ngx_result_to_status(res, false);
		}
		return OK;
	}

	void release_feature(uint64_t p_feature_handle) override {
		MutexLock lock(mutex);
		if (p_feature_handle == 0 || !live_handles.has(p_feature_handle)) {
			return;
		}
		NVSDK_NGX_VULKAN_ReleaseFeature(reinterpret_cast<NVSDK_NGX_Handle *>(p_feature_handle));
		live_handles.erase(p_feature_handle);
		_refresh_vram_locked();
	}

	void shutdown() override {
		MutexLock lock(mutex);
		_cleanup_locked();
	}

	uint64_t get_memory_bytes() const override {
		MutexLock lock(mutex);
		return cached_memory_bytes;
	}

	int32_t get_last_result_code() const override {
		MutexLock lock(mutex);
		return last_result;
	}

	LoadedRuntimeInfo get_loaded_runtime_info() const override {
		MutexLock lock(mutex);
		return loaded_runtime;
	}
};

} // namespace

DLSSBackend *dlss_backend_create_ngx() {
	return memnew(DLSSBackendNGXVulkan);
}

DLSSNGXParameterMappingCheck dlss_ngx_vk_check_parameter_mapping(const DLSSBackend::FeatureCreateParams &p_create, const DLSSBackend::EvaluateParams &p_eval) {
	const NVSDK_NGX_DLSS_Create_Params c = build_dlss_create_params(p_create);
	NVSDK_NGX_Resource_VK color_res = to_ngx_vk_resource(p_eval.color);
	NVSDK_NGX_Resource_VK depth_res = to_ngx_vk_resource(p_eval.depth);
	NVSDK_NGX_Resource_VK mv_res = to_ngx_vk_resource(p_eval.velocity);
	NVSDK_NGX_Resource_VK out_res = to_ngx_vk_resource(p_eval.output);
	const NVSDK_NGX_VK_DLSS_Eval_Params e = build_vk_dlss_eval_params(p_eval, &color_res, &depth_res, &mv_res, &out_res);

	DLSSNGXParameterMappingCheck out;
	out.is_hdr = (c.InFeatureCreateFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0;
	out.depth_inverted = (c.InFeatureCreateFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
	out.mv_low_res = (c.InFeatureCreateFlags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
	out.mv_jittered = (c.InFeatureCreateFlags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0;
	out.auto_exposure = (c.InFeatureCreateFlags & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) != 0;
	out.pre_exposure = e.InPreExposure;
	out.exposure_scale = e.InExposureScale;
	out.mv_scale_x = e.InMVScaleX;
	out.mv_scale_y = e.InMVScaleY;
	out.jitter_x = e.InJitterOffsetX;
	out.jitter_y = e.InJitterOffsetY;
	out.reset = e.InReset;
	out.preset_k_value = static_cast<uint32_t>(preset_to_ngx(DLSSBackend::PRESET_K));
	out.has_exposure_texture = e.pInExposureTexture != nullptr;
	out.has_transparency_mask = e.pInTransparencyMask != nullptr;
	return out;
}

#endif // DLSS_BACKEND_NGX
