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

#include "dlss_backend.h"

#ifdef DLSS_BACKEND_NGX

#include "core/os/memory.h"
#include "core/os/os.h"

#include <drivers/vulkan/godot_vulkan.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wshadow"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif

#include <nvsdk_ngx_vk.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cstring>

namespace {

class DLSSBackendNGXVulkan : public DLSSBackend {
	int32_t last_result = static_cast<int32_t>(NVSDK_NGX_Result_Success);

public:
	String get_backend_name() const override { return "ngx"; }

	Vector<CharString> get_instance_extensions() override {
		Vector<CharString> result;
		NVSDK_NGX_FeatureDiscoveryInfo discovery = {};
		discovery.SDKVersion = NVSDK_NGX_Version_API;
		discovery.FeatureID = NVSDK_NGX_Feature_SuperSampling;
		discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
		discovery.Identifier.v.ProjectDesc.ProjectId = "00000000-0000-0000-0000-000000000000";
		discovery.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
		discovery.Identifier.v.ProjectDesc.EngineVersion = "4.8";
		discovery.ApplicationDataPath = L".";

		uint32_t ext_count = 0;
		VkExtensionProperties *ext_props = nullptr;
		NVSDK_NGX_Result ngx_res = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&discovery, &ext_count, &ext_props);
		last_result = static_cast<int32_t>(ngx_res);
		if (NVSDK_NGX_SUCCEED(ngx_res) && ext_props != nullptr) {
			for (uint32_t i = 0; i < ext_count; i++) {
				if (ext_props[i].extensionName[0] != '\0') {
					result.push_back(CharString(ext_props[i].extensionName));
				}
			}
		}
		return result;
	}

	Vector<CharString> get_device_extensions(uint64_t p_instance_handle, uint64_t p_physical_device_handle) override {
		return Vector<CharString>();
	}

	Status init(const InitParams &p_params) override {
		return FEATURE_CREATE_FAILED;
	}

	Status query_availability(uint32_t p_vendor_id, bool p_extensions_satisfied) override {
		return FEATURE_CREATE_FAILED;
	}

	Status get_optimal_settings(const Size2i &p_output_size, QualityMode p_quality_mode, OptimalSettings &r_settings) override {
		return FEATURE_CREATE_FAILED;
	}

	Status create_feature(const FeatureCreateParams &p_params, uint64_t &r_feature_handle) override {
		r_feature_handle = 0;
		return FEATURE_CREATE_FAILED;
	}

	Status evaluate(const EvaluateParams &p_params) override {
		return FEATURE_CREATE_FAILED;
	}

	void release_feature(uint64_t p_feature_handle) override {}

	void shutdown() override {}

	uint64_t get_memory_bytes() const override { return 0; }

	int32_t get_last_result_code() const override { return last_result; }
};

} // namespace

DLSSBackend *dlss_backend_create_ngx() {
	return memnew(DLSSBackendNGXVulkan);
}

#endif // DLSS_BACKEND_NGX
