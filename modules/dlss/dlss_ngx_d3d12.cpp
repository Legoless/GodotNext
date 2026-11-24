/**************************************************************************/
/*  dlss_ngx_d3d12.cpp                                                    */
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

#include "dlss_ngx_d3d12.h"

#if defined(DLSS_BACKEND_NGX) && defined(D3D12_ENABLED)

// GodotNext: pu_dlss_d3d12 (#119), pu_rr_d3d12 (#204).
// Direct3D 12 NGX backend supporting DLSS Super Resolution / DLAA and DLSS Ray Reconstruction (DLSSD).

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/memory.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "drivers/d3d12/rendering_device_driver_d3d12.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <psapi.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wshadow"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_d3d.h>
#include <nvsdk_ngx_helpers_dlssd_d3d.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

static_assert(sizeof(wchar_t) == sizeof(char16_t), "DLSS D3D12 backend expects 16-bit wchar_t on Windows.");

namespace {

constexpr uint32_t VENDOR_ID_NVIDIA = 0x10DE;
constexpr unsigned int SINGLE_GPU_NODE_MASK = 1u;

Mutex g_unwrap_mutex;
DLSSD3D12NativeUnwrapFn g_native_device_unwrap_fn = nullptr;
DLSSD3D12NativeUnwrapFn g_native_command_list_unwrap_fn = nullptr;

int32_t ngx_result_to_i32(NVSDK_NGX_Result p_res) {
	return static_cast<int32_t>(static_cast<uint32_t>(p_res));
}

NVSDK_NGX_PerfQuality_Value to_ngx_quality(DLSSBackend::QualityMode p_mode) {
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

NVSDK_NGX_DLSS_Hint_Render_Preset to_ngx_sr_preset(DLSSBackend::Preset p_preset) {
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

const char *to_ngx_sr_preset_param(DLSSBackend::QualityMode p_mode) {
	switch (p_mode) {
		case DLSSBackend::QUALITY_DLAA:
			return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA;
		case DLSSBackend::QUALITY_MAX_QUALITY:
			return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality;
		case DLSSBackend::QUALITY_BALANCED:
			return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced;
		case DLSSBackend::QUALITY_MAX_PERFORMANCE:
			return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance;
		case DLSSBackend::QUALITY_ULTRA_PERFORMANCE:
			return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance;
		default:
			return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality;
	}
}

const char *to_ngx_rr_preset_param(DLSSBackend::QualityMode p_mode) {
	switch (p_mode) {
		case DLSSBackend::QUALITY_DLAA:
			return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA;
		case DLSSBackend::QUALITY_MAX_QUALITY:
			return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality;
		case DLSSBackend::QUALITY_BALANCED:
			return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced;
		case DLSSBackend::QUALITY_MAX_PERFORMANCE:
			return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance;
		case DLSSBackend::QUALITY_ULTRA_PERFORMANCE:
			return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance;
		default:
			return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality;
	}
}

DLSSBackend::Status map_ngx_error(NVSDK_NGX_Result p_res, DLSSBackend::Status p_fallback) {
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
			return DLSSBackend::RUNTIME_MISSING;
		default:
			return p_fallback;
	}
}

ID3D12Resource *to_d3d12_resource(const DLSSBackend::TextureResource &p_res) {
	return reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(p_res.image_handle));
}

void projection_to_float16(const Projection &p_proj, float r_out[16]) {
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			r_out[c * 4 + r] = static_cast<float>(p_proj.columns[c][r]);
		}
	}
}

bool find_loaded_module_path(const char *p_target_lower_name, String &r_path) {
	r_path = String();
	HANDLE process = GetCurrentProcess();
	DWORD cb_needed = 0;
	LocalVector<HMODULE> modules;
	modules.resize(256);

	if (!EnumProcessModules(process, modules.ptr(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &cb_needed)) {
		return false;
	}
	if (cb_needed > static_cast<DWORD>(modules.size() * sizeof(HMODULE))) {
		modules.resize(static_cast<uint32_t>(cb_needed / sizeof(HMODULE)));
		if (!EnumProcessModules(process, modules.ptr(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &cb_needed)) {
			return false;
		}
	}

	const uint32_t module_count = static_cast<uint32_t>(cb_needed / sizeof(HMODULE));
	const String target_name = String(p_target_lower_name);
	WCHAR path_buf[4096];

	for (uint32_t i = 0; i < module_count; i++) {
		const DWORD len = GetModuleFileNameExW(process, modules[i], path_buf, static_cast<DWORD>(sizeof(path_buf) / sizeof(path_buf[0])));
		if (len == 0) {
			continue;
		}
		path_buf[MIN(len, static_cast<DWORD>((sizeof(path_buf) / sizeof(path_buf[0])) - 1))] = L'\0';
		const String full_path = String::utf16(reinterpret_cast<const char16_t *>(path_buf));
		if (full_path.get_file().to_lower() == target_name) {
			r_path = full_path;
			return true;
		}
	}
	return false;
}

bool read_manifest_entry(const Vector<String> &p_search_dirs, const String &p_file_name_lower, String &r_version, String &r_manifest_sha256) {
	r_version = String();
	r_manifest_sha256 = String();

	for (const String &search_dir : p_search_dirs) {
		if (search_dir.is_empty()) {
			continue;
		}
		const String manifest_path = search_dir.path_join("dlss_runtime.json");
		if (!FileAccess::exists(manifest_path)) {
			continue;
		}
		Error err = OK;
		const String text = FileAccess::get_file_as_string(manifest_path, &err);
		if (err != OK || text.is_empty()) {
			continue;
		}
		const Variant parsed = JSON::parse_string(text);
		if (parsed.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary root = parsed;
		if (root.has("sdk_tag")) {
			r_version = String(root["sdk_tag"]);
		} else if (root.has("pinned_tag")) {
			r_version = String(root["pinned_tag"]);
		}
		if (root.has("sha256") && root["sha256"].get_type() == Variant::DICTIONARY) {
			const Dictionary sha_dict = root["sha256"];
			const Array keys = sha_dict.keys();
			for (int k = 0; k < keys.size(); k++) {
				const String key = String(keys[k]);
				if (key.to_lower() == p_file_name_lower) {
					r_manifest_sha256 = String(sha_dict[keys[k]]).to_lower();
					return true;
				}
			}
		}
		if (root.has("files") && root["files"].get_type() == Variant::ARRAY) {
			const Array files = root["files"];
			for (int f = 0; f < files.size(); f++) {
				if (files[f].get_type() != Variant::DICTIONARY) {
					continue;
				}
				const Dictionary entry = files[f];
				if (String(entry.get("name", "")).to_lower() == p_file_name_lower) {
					r_manifest_sha256 = String(entry.get("sha256", "")).to_lower();
					return true;
				}
			}
		}
	}
	return false;
}

class DLSSBackendNGXD3D12 : public DLSSBackend {
public:
	typedef void *(*NativeUnwrapFn)(void *);

	static void set_native_device_unwrap(NativeUnwrapFn p_fn) {
		MutexLock lock(g_unwrap_mutex);
		g_native_device_unwrap_fn = p_fn;
	}

	static void set_native_command_list_unwrap(NativeUnwrapFn p_fn) {
		MutexLock lock(g_unwrap_mutex);
		g_native_command_list_unwrap_fn = p_fn;
	}

private:
	struct FeatureEntry {
		NVSDK_NGX_Handle *handle = nullptr;
		NVSDK_NGX_Parameter *eval_params = nullptr;
		bool owns_params = false;
		bool ray_reconstruction = false;
	};

	mutable Mutex ngx_mutex;
	ID3D12Device *device = nullptr;
	NVSDK_NGX_Parameter *capability_params = nullptr;
	bool initialized = false;
	bool allow_ota_updates = false;
	String project_id;
	uint64_t application_id = 0;
	String engine_version;
	String runtime_search_path;
	int32_t last_result = ngx_result_to_i32(NVSDK_NGX_Result_Success);
	uint64_t allocated_memory_bytes = 0;
	LoadedRuntimeInfo loaded_runtime;
	HashMap<uint64_t, FeatureEntry> features;

	static ID3D12Device *_unwrap_device(uint64_t p_handle) {
		void *ptr = reinterpret_cast<void *>(static_cast<uintptr_t>(p_handle));
		if (ptr == nullptr) {
			return nullptr;
		}
		NativeUnwrapFn unwrap_fn = nullptr;
		{
			MutexLock lock(g_unwrap_mutex);
			unwrap_fn = g_native_device_unwrap_fn;
		}
		if (unwrap_fn != nullptr) {
			void *unwrapped = unwrap_fn(ptr);
			if (unwrapped != nullptr) {
				ptr = unwrapped;
			}
		}
		return static_cast<ID3D12Device *>(ptr);
	}

	static ID3D12GraphicsCommandList *_unwrap_command_list(uint64_t p_handle) {
		void *ptr = reinterpret_cast<void *>(static_cast<uintptr_t>(p_handle));
		if (ptr == nullptr) {
			return nullptr;
		}
		NativeUnwrapFn unwrap_fn = nullptr;
		{
			MutexLock lock(g_unwrap_mutex);
			unwrap_fn = g_native_command_list_unwrap_fn;
		}
		if (unwrap_fn != nullptr) {
			void *unwrapped = unwrap_fn(ptr);
			if (unwrapped != nullptr) {
				ptr = unwrapped;
			}
		}
		return static_cast<ID3D12GraphicsCommandList *>(ptr);
	}

	void _refresh_memory_bytes_locked() {
		if (!initialized || capability_params == nullptr || features.is_empty()) {
			allocated_memory_bytes = 0;
			return;
		}
		bool has_sr = false;
		bool has_rr = false;
		for (const KeyValue<uint64_t, FeatureEntry> &E : features) {
			if (E.value.ray_reconstruction) {
				has_rr = true;
			} else {
				has_sr = true;
			}
		}
		uint64_t total_bytes = 0;
		if (has_sr) {
			unsigned long long sr_bytes = 0;
			if (NVSDK_NGX_SUCCEED(NGX_DLSS_GET_STATS(capability_params, &sr_bytes))) {
				total_bytes += static_cast<uint64_t>(sr_bytes);
			}
		}
		if (has_rr) {
			unsigned long long rr_bytes = 0;
			if (NVSDK_NGX_SUCCEED(NGX_DLSSD_GET_STATS(capability_params, &rr_bytes))) {
				total_bytes += static_cast<uint64_t>(rr_bytes);
			}
		}
		allocated_memory_bytes = total_bytes;
	}

	void _discover_loaded_runtime_locked(bool p_prefer_rr) {
		String dll_path;
		const char *primary_name = p_prefer_rr ? "nvngx_dlssd.dll" : "nvngx_dlss.dll";
		const char *secondary_name = p_prefer_rr ? "nvngx_dlss.dll" : "nvngx_dlssd.dll";
		if (!find_loaded_module_path(primary_name, dll_path)) {
			if (!find_loaded_module_path(secondary_name, dll_path)) {
				return;
			}
		}

		LoadedRuntimeInfo info;
		info.path = dll_path;
		info.sha256 = FileAccess::get_sha256(dll_path).to_lower();

		Vector<String> manifest_dirs;
		manifest_dirs.push_back(dll_path.get_base_dir());
		if (!runtime_search_path.is_empty()) {
			manifest_dirs.push_back(runtime_search_path);
		}
		if (OS::get_singleton() != nullptr) {
			const String exe_dir = OS::get_singleton()->get_executable_path().get_base_dir();
			if (!exe_dir.is_empty()) {
				manifest_dirs.push_back(exe_dir);
			}
		}

		read_manifest_entry(manifest_dirs, dll_path.get_file().to_lower(), info.version, info.manifest_sha256);
		info.sha256_matches = !info.sha256.is_empty() && !info.manifest_sha256.is_empty() && info.sha256 == info.manifest_sha256;
		loaded_runtime = info;
	}

	bool _has_runtime_file_on_disk_locked(const char *p_dll_name) const {
		if (!runtime_search_path.is_empty() && FileAccess::exists(runtime_search_path.path_join(p_dll_name))) {
			return true;
		}
		if (OS::get_singleton() != nullptr) {
			const String exe_dir = OS::get_singleton()->get_executable_path().get_base_dir();
			if (!exe_dir.is_empty() && FileAccess::exists(exe_dir.path_join(p_dll_name))) {
				return true;
			}
		}
		String loaded_path;
		return find_loaded_module_path(p_dll_name, loaded_path);
	}

	void _shutdown_locked() {
		for (const KeyValue<uint64_t, FeatureEntry> &E : features) {
			if (E.value.handle != nullptr) {
				NVSDK_NGX_D3D12_ReleaseFeature(E.value.handle);
			}
			if (E.value.owns_params && E.value.eval_params != nullptr) {
				NVSDK_NGX_D3D12_DestroyParameters(E.value.eval_params);
			}
		}
		features.clear();

		if (capability_params != nullptr) {
			NVSDK_NGX_D3D12_DestroyParameters(capability_params);
			capability_params = nullptr;
		}

		if (initialized) {
			const NVSDK_NGX_Result ngx_res = NVSDK_NGX_D3D12_Shutdown1(device);
			last_result = ngx_result_to_i32(ngx_res);
		}

		device = nullptr;
		initialized = false;
		allocated_memory_bytes = 0;
	}

public:
	~DLSSBackendNGXD3D12() override {
		shutdown();
	}

	String get_backend_name() const override { return "ngx"; }

	Vector<CharString> get_instance_extensions() override {
		return Vector<CharString>();
	}

	Vector<CharString> get_device_extensions(uint64_t p_instance_handle, uint64_t p_physical_device_handle) override {
		(void)p_instance_handle;
		(void)p_physical_device_handle;
		return Vector<CharString>();
	}

	Status init(const InitParams &p_params) override {
		MutexLock lock(ngx_mutex);

		if (initialized) {
			_shutdown_locked();
		}

		if (p_params.project_id.is_empty() && p_params.application_id == 0) {
			return PROJECT_ID_MISSING;
		}

		ID3D12Device *d3d_device = _unwrap_device(p_params.device_handle);
		if (d3d_device == nullptr) {
			last_result = ngx_result_to_i32(NVSDK_NGX_Result_FAIL_InvalidParameter);
			return FEATURE_CREATE_FAILED;
		}

		String app_data_path = p_params.application_data_path;
		if (app_data_path.is_empty() && OS::get_singleton() != nullptr) {
			app_data_path = OS::get_singleton()->get_user_data_dir().path_join("ngx");
		}
		if (app_data_path.is_empty()) {
			app_data_path = ".";
		} else if (app_data_path != ".") {
			DirAccess::make_dir_recursive_absolute(app_data_path);
		}
		const Char16String app_data_u16 = app_data_path.utf16();
		const wchar_t *app_data_w = reinterpret_cast<const wchar_t *>(app_data_u16.get_data());

		// Keep UTF-16 path strings alive for the duration of the NGX init call.
		LocalVector<Char16String> search_path_u16s;
		if (!p_params.runtime_search_path.is_empty()) {
			search_path_u16s.push_back(p_params.runtime_search_path.utf16());
		}
		if (OS::get_singleton() != nullptr) {
			const String exe_dir = OS::get_singleton()->get_executable_path().get_base_dir();
			if (!exe_dir.is_empty() && exe_dir != p_params.runtime_search_path) {
				search_path_u16s.push_back(exe_dir.utf16());
			}
		}

		LocalVector<const wchar_t *> search_path_ptrs;
		search_path_ptrs.resize(search_path_u16s.size());
		for (uint32_t i = 0; i < search_path_u16s.size(); i++) {
			search_path_ptrs[i] = reinterpret_cast<const wchar_t *>(search_path_u16s[i].get_data());
		}

		NVSDK_NGX_FeatureCommonInfo common_info = {};
		if (!search_path_ptrs.is_empty()) {
			common_info.PathListInfo.Path = search_path_ptrs.ptr();
			common_info.PathListInfo.Length = static_cast<unsigned int>(search_path_ptrs.size());
		}
		common_info.LoggingInfo.LoggingCallback = nullptr;
		common_info.LoggingInfo.MinimumLoggingLevel = (OS::get_singleton() != nullptr && OS::get_singleton()->is_stdout_verbose())
				? NVSDK_NGX_LOGGING_LEVEL_VERBOSE
				: NVSDK_NGX_LOGGING_LEVEL_ON;
		common_info.LoggingInfo.DisableOtherLoggingSinks = false;

		const String resolved_engine_ver = p_params.engine_version.is_empty() ? String("4.8") : p_params.engine_version;
		NVSDK_NGX_Result ngx_res = NVSDK_NGX_Result_Fail;
		if (p_params.application_id != 0) {
			ngx_res = NVSDK_NGX_D3D12_Init(
					static_cast<unsigned long long>(p_params.application_id),
					app_data_w,
					d3d_device,
					&common_info,
					NVSDK_NGX_Version_API);
		} else {
			const CharString project_id_utf8 = p_params.project_id.utf8();
			const CharString engine_ver_utf8 = resolved_engine_ver.utf8();
			ngx_res = NVSDK_NGX_D3D12_Init_with_ProjectID(
					project_id_utf8.get_data(),
					NVSDK_NGX_ENGINE_TYPE_CUSTOM,
					engine_ver_utf8.get_data(),
					app_data_w,
					d3d_device,
					&common_info,
					NVSDK_NGX_Version_API);
		}

		last_result = ngx_result_to_i32(ngx_res);
		if (NVSDK_NGX_FAILED(ngx_res)) {
			if (ngx_res == NVSDK_NGX_Result_FAIL_InvalidParameter && p_params.application_id == 0) {
				return PROJECT_ID_MISSING;
			}
			return map_ngx_error(ngx_res, RUNTIME_MISSING);
		}

		NVSDK_NGX_Parameter *cap_params = nullptr;
		const NVSDK_NGX_Result cap_res = NVSDK_NGX_D3D12_GetCapabilityParameters(&cap_params);
		last_result = ngx_result_to_i32(cap_res);
		if (NVSDK_NGX_FAILED(cap_res) || cap_params == nullptr) {
			NVSDK_NGX_D3D12_Shutdown1(d3d_device);
			return map_ngx_error(cap_res, DRIVER_TOO_OLD);
		}

		device = d3d_device;
		capability_params = cap_params;
		initialized = true;
		allow_ota_updates = p_params.allow_ota_updates;
		project_id = p_params.project_id;
		application_id = p_params.application_id;
		engine_version = resolved_engine_ver;
		runtime_search_path = p_params.runtime_search_path;
		allocated_memory_bytes = 0;

		_discover_loaded_runtime_locked(false);
		return OK;
	}

	Status query_availability(uint32_t p_vendor_id, bool p_extensions_satisfied) override {
		MutexLock lock(ngx_mutex);

		if (p_vendor_id != 0 && p_vendor_id != VENDOR_ID_NVIDIA) {
			return NOT_NVIDIA_RTX;
		}
		if (!p_extensions_satisfied) {
			return EXTENSION_MISSING;
		}
		if (!initialized || capability_params == nullptr) {
			return map_ngx_error(static_cast<NVSDK_NGX_Result>(static_cast<uint32_t>(last_result)), RUNTIME_MISSING);
		}

		int needs_updated_driver = 0;
		unsigned int min_driver_major = 0;
		unsigned int min_driver_minor = 0;
		const NVSDK_NGX_Result res_driver = NVSDK_NGX_Parameter_GetI(
				capability_params,
				NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
				&needs_updated_driver);
		NVSDK_NGX_Parameter_GetUI(capability_params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &min_driver_major);
		NVSDK_NGX_Parameter_GetUI(capability_params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &min_driver_minor);
		if (NVSDK_NGX_SUCCEED(res_driver) && needs_updated_driver != 0) {
			print_verbose(vformat("DLSS D3D12: NVIDIA driver update required (minimum %u.%u).", min_driver_major, min_driver_minor));
			return DRIVER_TOO_OLD;
		}

		int dlss_available = 0;
		const NVSDK_NGX_Result res_avail = NVSDK_NGX_Parameter_GetI(
				capability_params,
				NVSDK_NGX_Parameter_SuperSampling_Available,
				&dlss_available);
		if (NVSDK_NGX_FAILED(res_avail) || dlss_available == 0) {
			int init_code_i = 0;
			if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(
						capability_params,
						NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult,
						&init_code_i)) &&
					init_code_i != 0) {
				const NVSDK_NGX_Result init_code = static_cast<NVSDK_NGX_Result>(static_cast<uint32_t>(init_code_i));
				last_result = ngx_result_to_i32(init_code);
				if (init_code == NVSDK_NGX_Result_FAIL_OutOfDate) {
					return DRIVER_TOO_OLD;
				}
				if (init_code == NVSDK_NGX_Result_FAIL_FeatureNotSupported) {
					return NOT_NVIDIA_RTX;
				}
				if (init_code == NVSDK_NGX_Result_FAIL_UnableToInitializeFeature || init_code == NVSDK_NGX_Result_FAIL_FeatureNotFound) {
					return RUNTIME_MISSING;
				}
			}
			if (!_has_runtime_file_on_disk_locked("nvngx_dlss.dll")) {
				return RUNTIME_MISSING;
			}
			return NOT_NVIDIA_RTX;
		}

		_discover_loaded_runtime_locked(false);
		return OK;
	}

	Status get_optimal_settings(const Size2i &p_output_size, QualityMode p_quality_mode, OptimalSettings &r_settings) override {
		MutexLock lock(ngx_mutex);

		if (!initialized || capability_params == nullptr) {
			return FEATURE_CREATE_FAILED;
		}
		if (p_output_size.width <= 0 || p_output_size.height <= 0) {
			return FEATURE_CREATE_FAILED;
		}

		unsigned int opt_w = 0;
		unsigned int opt_h = 0;
		unsigned int max_w = 0;
		unsigned int max_h = 0;
		unsigned int min_w = 0;
		unsigned int min_h = 0;
		float sharpness = 0.0f;

		const NVSDK_NGX_Result ngx_res = NGX_DLSS_GET_OPTIMAL_SETTINGS(
				capability_params,
				static_cast<unsigned int>(p_output_size.width),
				static_cast<unsigned int>(p_output_size.height),
				to_ngx_quality(p_quality_mode),
				&opt_w,
				&opt_h,
				&max_w,
				&max_h,
				&min_w,
				&min_h,
				&sharpness);
		last_result = ngx_result_to_i32(ngx_res);
		if (NVSDK_NGX_FAILED(ngx_res)) {
			if (ngx_res == NVSDK_NGX_Result_FAIL_OutOfDate && !_has_runtime_file_on_disk_locked("nvngx_dlss.dll")) {
				return RUNTIME_MISSING;
			}
			return map_ngx_error(ngx_res, FEATURE_CREATE_FAILED);
		}

		r_settings.optimal_size = Size2i(static_cast<int>(opt_w), static_cast<int>(opt_h));
		r_settings.min_size = Size2i(static_cast<int>(min_w), static_cast<int>(min_h));
		r_settings.max_size = Size2i(static_cast<int>(max_w), static_cast<int>(max_h));
		r_settings.sharpness = sharpness;
		return OK;
	}

	Status create_feature(const FeatureCreateParams &p_params, uint64_t &r_feature_handle) override {
		MutexLock lock(ngx_mutex);

		r_feature_handle = 0;
		if (!initialized || capability_params == nullptr) {
			return FEATURE_CREATE_FAILED;
		}
		if (p_params.render_size.width <= 0 || p_params.render_size.height <= 0 ||
				p_params.output_size.width <= 0 || p_params.output_size.height <= 0) {
			return FEATURE_CREATE_FAILED;
		}

		ID3D12GraphicsCommandList *cmd_list = _unwrap_command_list(p_params.command_buffer_handle);
		if (cmd_list == nullptr) {
			last_result = ngx_result_to_i32(NVSDK_NGX_Result_FAIL_InvalidParameter);
			return FEATURE_CREATE_FAILED;
		}

		if (p_params.ray_reconstruction) {
			int rr_needs_driver = 0;
			const NVSDK_NGX_Result res_rr_drv = NVSDK_NGX_Parameter_GetI(
					capability_params,
					NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver,
					&rr_needs_driver);
			if (NVSDK_NGX_SUCCEED(res_rr_drv) && rr_needs_driver != 0) {
				return DRIVER_TOO_OLD;
			}

			int rr_available = 0;
			const NVSDK_NGX_Result res_rr_avail = NVSDK_NGX_Parameter_GetI(
					capability_params,
					NVSDK_NGX_Parameter_SuperSamplingDenoising_Available,
					&rr_available);
			if (NVSDK_NGX_FAILED(res_rr_avail) || rr_available == 0) {
				int rr_init_code_i = 0;
				if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(
							capability_params,
							NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult,
							&rr_init_code_i)) &&
						rr_init_code_i != 0) {
					const NVSDK_NGX_Result rr_init_code = static_cast<NVSDK_NGX_Result>(static_cast<uint32_t>(rr_init_code_i));
					last_result = ngx_result_to_i32(rr_init_code);
					return map_ngx_error(rr_init_code, RUNTIME_MISSING);
				}
				return _has_runtime_file_on_disk_locked("nvngx_dlssd.dll") ? NOT_NVIDIA_RTX : RUNTIME_MISSING;
			}
		}

		if (allow_ota_updates) {
			NVSDK_NGX_Application_Identifier app_id = {};
			CharString project_id_utf8;
			CharString engine_ver_utf8;
			if (application_id != 0) {
				app_id.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Application_Id;
				app_id.v.ApplicationId = static_cast<unsigned long long>(application_id);
			} else {
				project_id_utf8 = project_id.utf8();
				engine_ver_utf8 = engine_version.utf8();
				app_id.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
				app_id.v.ProjectDesc.ProjectId = project_id_utf8.get_data();
				app_id.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
				app_id.v.ProjectDesc.EngineVersion = engine_ver_utf8.get_data();
			}
			NVSDK_NGX_UpdateFeature(
					&app_id,
					p_params.ray_reconstruction ? NVSDK_NGX_Feature_RayReconstruction : NVSDK_NGX_Feature_SuperSampling);
		}

		NVSDK_NGX_Parameter *feature_params = nullptr;
		bool owns_params = false;
		if (NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_AllocateParameters(&feature_params)) && feature_params != nullptr) {
			owns_params = true;
		} else {
			feature_params = capability_params;
		}

		int create_flags = NVSDK_NGX_DLSS_Feature_Flags_None;
		if (p_params.is_hdr) {
			create_flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
		}
		if (p_params.depth_inverted) {
			create_flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
		}
		if (p_params.mv_low_res) {
			create_flags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
		}
		if (p_params.mv_jittered) {
			create_flags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
		}
		if (p_params.auto_exposure) {
			create_flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
		}

		NVSDK_NGX_Parameter_SetI(feature_params, NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);

		const NVSDK_NGX_PerfQuality_Value perf_quality = to_ngx_quality(p_params.quality_mode);
		NVSDK_NGX_Handle *ngx_handle = nullptr;
		NVSDK_NGX_Result ngx_res = NVSDK_NGX_Result_Fail;

		if (!p_params.ray_reconstruction) {
			NVSDK_NGX_Parameter_SetUI(
					feature_params,
					to_ngx_sr_preset_param(p_params.quality_mode),
					static_cast<unsigned int>(to_ngx_sr_preset(p_params.preset)));

			NVSDK_NGX_DLSS_Create_Params dlss_create = {};
			dlss_create.Feature.InWidth = static_cast<unsigned int>(p_params.render_size.width);
			dlss_create.Feature.InHeight = static_cast<unsigned int>(p_params.render_size.height);
			dlss_create.Feature.InTargetWidth = static_cast<unsigned int>(p_params.output_size.width);
			dlss_create.Feature.InTargetHeight = static_cast<unsigned int>(p_params.output_size.height);
			dlss_create.Feature.InPerfQualityValue = perf_quality;
			dlss_create.InFeatureCreateFlags = create_flags;
			dlss_create.InEnableOutputSubrects = false;

			ngx_res = NGX_D3D12_CREATE_DLSS_EXT(
					cmd_list,
					SINGLE_GPU_NODE_MASK,
					SINGLE_GPU_NODE_MASK,
					&ngx_handle,
					feature_params,
					&dlss_create);
		} else {
			NVSDK_NGX_Parameter_SetUI(
					feature_params,
					to_ngx_rr_preset_param(p_params.quality_mode),
					static_cast<unsigned int>(NVSDK_NGX_RayReconstruction_Hint_Render_Preset_Default));

			NVSDK_NGX_DLSSD_Create_Params dlssd_create = {};
			dlssd_create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
			dlssd_create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
			dlssd_create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
			dlssd_create.InWidth = static_cast<unsigned int>(p_params.render_size.width);
			dlssd_create.InHeight = static_cast<unsigned int>(p_params.render_size.height);
			dlssd_create.InTargetWidth = static_cast<unsigned int>(p_params.output_size.width);
			dlssd_create.InTargetHeight = static_cast<unsigned int>(p_params.output_size.height);
			dlssd_create.InPerfQualityValue = perf_quality;
			dlssd_create.InFeatureCreateFlags = create_flags;
			dlssd_create.InEnableOutputSubrects = false;

			ngx_res = NGX_D3D12_CREATE_DLSSD_EXT(
					cmd_list,
					SINGLE_GPU_NODE_MASK,
					SINGLE_GPU_NODE_MASK,
					&ngx_handle,
					feature_params,
					&dlssd_create);
		}

		last_result = ngx_result_to_i32(ngx_res);
		if (NVSDK_NGX_FAILED(ngx_res) || ngx_handle == nullptr) {
			if (owns_params && feature_params != nullptr) {
				NVSDK_NGX_D3D12_DestroyParameters(feature_params);
			}
			return map_ngx_error(ngx_res, FEATURE_CREATE_FAILED);
		}

		const uint64_t handle_id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ngx_handle));
		FeatureEntry entry;
		entry.handle = ngx_handle;
		entry.eval_params = feature_params;
		entry.owns_params = owns_params;
		entry.ray_reconstruction = p_params.ray_reconstruction;
		features[handle_id] = entry;

		_refresh_memory_bytes_locked();
		_discover_loaded_runtime_locked(p_params.ray_reconstruction);

		r_feature_handle = handle_id;
		return OK;
	}

	Status evaluate(const EvaluateParams &p_params) override {
		MutexLock lock(ngx_mutex);

		if (!initialized) {
			return FEATURE_CREATE_FAILED;
		}

		FeatureEntry *entry = features.getptr(p_params.feature_handle);
		if (entry == nullptr || entry->handle == nullptr) {
			last_result = ngx_result_to_i32(NVSDK_NGX_Result_FAIL_FeatureNotFound);
			return FEATURE_CREATE_FAILED;
		}

		ID3D12GraphicsCommandList *cmd_list = _unwrap_command_list(p_params.command_buffer_handle);
		ID3D12Resource *color_res = to_d3d12_resource(p_params.color);
		ID3D12Resource *depth_res = to_d3d12_resource(p_params.depth);
		ID3D12Resource *mv_res = to_d3d12_resource(p_params.velocity);
		ID3D12Resource *output_res = to_d3d12_resource(p_params.output);
		if (cmd_list == nullptr || color_res == nullptr || depth_res == nullptr || mv_res == nullptr || output_res == nullptr) {
			last_result = ngx_result_to_i32(NVSDK_NGX_Result_FAIL_MissingInput);
			return FEATURE_CREATE_FAILED;
		}

		NVSDK_NGX_Parameter *eval_params = entry->eval_params != nullptr ? entry->eval_params : capability_params;
		if (eval_params == nullptr) {
			last_result = ngx_result_to_i32(NVSDK_NGX_Result_FAIL_NotInitialized);
			return FEATURE_CREATE_FAILED;
		}
		if (entry->owns_params) {
			eval_params->Reset();
		}

		const float pre_exposure = p_params.pre_exposure > 0.0f ? p_params.pre_exposure : 1.0f;
		const float exposure_scale = p_params.exposure_scale > 0.0f ? p_params.exposure_scale : 1.0f;
		const unsigned int render_w = static_cast<unsigned int>(MAX(0, p_params.render_size.width));
		const unsigned int render_h = static_cast<unsigned int>(MAX(0, p_params.render_size.height));
		NVSDK_NGX_Result ngx_res = NVSDK_NGX_Result_Fail;

		if (!entry->ray_reconstruction) {
			NVSDK_NGX_D3D12_DLSS_Eval_Params eval = {};
			eval.Feature.pInColor = color_res;
			eval.Feature.pInOutput = output_res;
			eval.Feature.InSharpness = 0.0f;
			eval.pInDepth = depth_res;
			eval.pInMotionVectors = mv_res;
			eval.InJitterOffsetX = p_params.jitter_offset.x;
			eval.InJitterOffsetY = p_params.jitter_offset.y;
			eval.InRenderSubrectDimensions.Width = render_w;
			eval.InRenderSubrectDimensions.Height = render_h;
			eval.InReset = p_params.reset ? 1 : 0;
			eval.InMVScaleX = p_params.mv_scale.x;
			eval.InMVScaleY = p_params.mv_scale.y;
			eval.InPreExposure = pre_exposure;
			eval.InExposureScale = exposure_scale;
			eval.InFrameTimeDeltaInMsec = p_params.frame_time_delta_ms;

			ngx_res = NGX_D3D12_EVALUATE_DLSS_EXT(cmd_list, entry->handle, eval_params, &eval);
		} else {
			float world_to_view_mat[16];
			float view_to_clip_mat[16];
			projection_to_float16(Projection(p_params.world_to_view), world_to_view_mat);
			projection_to_float16(p_params.view_to_clip, view_to_clip_mat);

			NVSDK_NGX_D3D12_DLSSD_Eval_Params eval = {};
			eval.pInDiffuseAlbedo = to_d3d12_resource(p_params.diffuse_albedo);
			eval.pInSpecularAlbedo = to_d3d12_resource(p_params.specular_albedo);
			eval.pInNormals = to_d3d12_resource(p_params.normal_roughness);
			if (eval.pInDiffuseAlbedo == nullptr || eval.pInSpecularAlbedo == nullptr || eval.pInNormals == nullptr) {
				last_result = ngx_result_to_i32(NVSDK_NGX_Result_FAIL_MissingInput);
				return FEATURE_CREATE_FAILED;
			}
			eval.pInRoughness = nullptr; // Packed in normals.w (NVSDK_NGX_DLSS_Roughness_Mode_Packed).
			eval.pInColor = color_res;
			eval.pInOutput = output_res;
			eval.pInDepth = depth_res;
			eval.pInMotionVectors = mv_res;
			eval.pInSpecularHitDistance = to_d3d12_resource(p_params.specular_hit_distance);
			eval.InJitterOffsetX = p_params.jitter_offset.x;
			eval.InJitterOffsetY = p_params.jitter_offset.y;
			eval.InRenderSubrectDimensions.Width = render_w;
			eval.InRenderSubrectDimensions.Height = render_h;
			eval.InReset = p_params.reset ? 1 : 0;
			eval.InMVScaleX = p_params.mv_scale.x;
			eval.InMVScaleY = p_params.mv_scale.y;
			eval.pInWorldToViewMatrix = world_to_view_mat;
			eval.pInViewToClipMatrix = view_to_clip_mat;
			eval.InPreExposure = pre_exposure;
			eval.InExposureScale = exposure_scale;
			eval.InFrameTimeDeltaInMsec = p_params.frame_time_delta_ms;

			ngx_res = NGX_D3D12_EVALUATE_DLSSD_EXT(cmd_list, entry->handle, eval_params, &eval);
		}

		last_result = ngx_result_to_i32(ngx_res);
		return NVSDK_NGX_SUCCEED(ngx_res) ? OK : FEATURE_CREATE_FAILED;
	}

	void release_feature(uint64_t p_feature_handle) override {
		MutexLock lock(ngx_mutex);

		FeatureEntry *entry = features.getptr(p_feature_handle);
		if (entry == nullptr) {
			return;
		}
		if (entry->handle != nullptr) {
			const NVSDK_NGX_Result ngx_res = NVSDK_NGX_D3D12_ReleaseFeature(entry->handle);
			last_result = ngx_result_to_i32(ngx_res);
		}
		if (entry->owns_params && entry->eval_params != nullptr) {
			NVSDK_NGX_D3D12_DestroyParameters(entry->eval_params);
		}
		features.erase(p_feature_handle);
		_refresh_memory_bytes_locked();
	}

	void shutdown() override {
		MutexLock lock(ngx_mutex);
		_shutdown_locked();
	}

	uint64_t get_memory_bytes() const override {
		MutexLock lock(ngx_mutex);
		return allocated_memory_bytes;
	}

	int32_t get_last_result_code() const override {
		MutexLock lock(ngx_mutex);
		return last_result;
	}

	LoadedRuntimeInfo get_loaded_runtime_info() const override {
		MutexLock lock(ngx_mutex);
		return loaded_runtime;
	}
};

} // namespace

DLSSBackend *dlss_backend_create_ngx_d3d12() {
	return memnew(DLSSBackendNGXD3D12);
}

void dlss_ngx_d3d12_set_native_device_unwrap(DLSSD3D12NativeUnwrapFn p_fn) {
	DLSSBackendNGXD3D12::set_native_device_unwrap(p_fn);
}

void dlss_ngx_d3d12_set_native_command_list_unwrap(DLSSD3D12NativeUnwrapFn p_fn) {
	DLSSBackendNGXD3D12::set_native_command_list_unwrap(p_fn);
}

void dlss_ngx_d3d12_prepare_callback_textures(
		RenderingDeviceDriver *p_driver,
		RDD::CommandBufferID p_cmd_buffer,
		VectorView<RDD::TextureID> p_textures,
		VectorView<bool> p_read_write) {
	if (p_driver == nullptr || p_driver->get_capabilities().device_family != RDD::DEVICE_DIRECTX) {
		return;
	}
	RenderingDeviceDriverD3D12 *d3d12_driver = static_cast<RenderingDeviceDriverD3D12 *>(p_driver);
	d3d12_driver->driver_callback_transition_textures(p_cmd_buffer, p_textures, p_read_write);
}

void dlss_ngx_d3d12_finish_callback_textures(
		RenderingDeviceDriver *p_driver,
		RDD::CommandBufferID p_cmd_buffer,
		VectorView<RDD::TextureID> p_textures,
		VectorView<bool> p_read_write) {
	if (p_driver == nullptr || p_driver->get_capabilities().device_family != RDD::DEVICE_DIRECTX) {
		return;
	}
	RenderingDeviceDriverD3D12 *d3d12_driver = static_cast<RenderingDeviceDriverD3D12 *>(p_driver);
	d3d12_driver->driver_callback_restore_textures(p_cmd_buffer, p_textures, p_read_write);
	p_driver->command_buffer_invalidate_cached_state(p_cmd_buffer);
}

#endif // DLSS_BACKEND_NGX && D3D12_ENABLED
