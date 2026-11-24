/**************************************************************************/
/*  test_dlss.cpp                                                         */
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

#include "modules/dlss/dlss_backend.h"
#include "modules/dlss/dlss_backend_mock.h"

#ifdef VULKAN_ENABLED
#include "modules/dlss/dlss_vulkan_extensions.h"
#endif

namespace TestDLSS {

DLSSBackend::InitParams make_test_init_params(const String &p_project_id) {
	DLSSBackend::InitParams params;
	params.api_name = "Vulkan";
	params.project_id = p_project_id;
	params.application_id = 0;
	params.engine_version = "4.8.0-godotnext";
	params.allow_ota_updates = false;
	return params;
}

bool is_ngx_backend_compiled() {
#ifdef DLSS_BACKEND_NGX
	return true;
#else
	return false;
#endif
}

#ifdef VULKAN_ENABLED
TEST_CASE("[DLSS][Mock] Vendor-gated Vulkan device extensions and object-attributed validation allowlist") {
	Vector<CharString> raw_exts;
	raw_exts.push_back(CharString("VK_NVX_binary_import"));
	raw_exts.push_back(CharString("VK_NVX_image_view_handle"));

	// Device extensions are returned only on vendor 0x10DE (NVIDIA).
	CHECK(DLSSVulkanExtensionProvider::filter_device_extensions_for_vendor(0x1002, raw_exts).is_empty());
	CHECK(DLSSVulkanExtensionProvider::filter_device_extensions_for_vendor(0x8086, raw_exts).is_empty());
	CHECK(DLSSVulkanExtensionProvider::filter_device_extensions_for_vendor(0x10DE, raw_exts).size() == 2);

	// Allowlist matches only exact (VUID, DLL SHA-256, driver) tuples; default table is empty.
	DLSSBackendMock mock;
	DLSSVulkanExtensionProvider provider(&mock);
	provider.set_loaded_dll_sha256("AABBCCDD");
	provider.set_driver_version_for_tests("595.99.02");

	VkDebugUtilsMessengerCallbackDataEXT cb = {};
	cb.pMessageIdName = "VUID-Test-Synthetic";
	CHECK(provider.filter_validation_message(&cb) == VulkanExtensionProvider::VALIDATION_DEFAULT);

	Vector<DLSSVulkanExtensionProvider::AllowlistRow> rows;
	rows.push_back({ "VUID-Test-Synthetic", "aabbccdd", "595.99.02" });
	provider.set_test_allowlist(rows);
	CHECK(provider.filter_validation_message(&cb) == VulkanExtensionProvider::VALIDATION_ALLOWLISTED);

	// Different driver or different SHA-256 must NOT match.
	provider.set_driver_version_for_tests("621.01");
	CHECK(provider.filter_validation_message(&cb) == VulkanExtensionProvider::VALIDATION_DEFAULT);
	provider.set_driver_version_for_tests("595.99.02");
	provider.set_loaded_dll_sha256("11223344");
	CHECK(provider.filter_validation_message(&cb) == VulkanExtensionProvider::VALIDATION_DEFAULT);

	// Companion runtime SHA-256 (e.g. DLSS-RR alongside DLSS-SR) matches when added.
	provider.add_loaded_dll_sha256("AABBCCDD");
	CHECK(provider.filter_validation_message(&cb) == VulkanExtensionProvider::VALIDATION_ALLOWLISTED);
}
#endif // VULKAN_ENABLED

#ifdef DLSS_BACKEND_NGX
TEST_CASE("[DLSS][NGX] Parameter mapping sets IsHDR, DepthInverted, MVLowRes, AutoExposure, clears MVJittered, sets PreExposure=1") {
	DLSSBackend::FeatureCreateParams create_params;
	create_params.render_size = Size2i(1280, 720);
	create_params.output_size = Size2i(1920, 1080);
	create_params.quality_mode = DLSSBackend::QUALITY_MAX_QUALITY;
	create_params.preset = DLSSBackend::PRESET_K;
	create_params.is_hdr = true;
	create_params.depth_inverted = true;
	create_params.mv_low_res = true;
	create_params.mv_jittered = false;
	create_params.auto_exposure = true;

	DLSSBackend::EvaluateParams eval_params;
	eval_params.render_size = Size2i(1280, 720);
	eval_params.output_size = Size2i(1920, 1080);
	eval_params.jitter_offset = Vector2(0.25f, -0.375f);
	eval_params.mv_scale = Vector2(1280.0f, 720.0f);
	eval_params.pre_exposure = 1.0f;
	eval_params.exposure_scale = 1.0f;
	eval_params.reset = true;

	const DLSSNGXParameterMappingCheck check = dlss_ngx_vk_check_parameter_mapping(create_params, eval_params);
	CHECK(check.is_hdr);
	CHECK(check.depth_inverted);
	CHECK(check.mv_low_res);
	CHECK(!check.mv_jittered);
	CHECK(check.auto_exposure);
	CHECK(check.pre_exposure == doctest::Approx(1.0f));
	CHECK(check.exposure_scale == doctest::Approx(1.0f));
	CHECK(check.mv_scale_x == doctest::Approx(1280.0f));
	CHECK(check.mv_scale_y == doctest::Approx(720.0f));
	CHECK(check.jitter_x == doctest::Approx(0.25f));
	CHECK(check.jitter_y == doctest::Approx(-0.375f));
	CHECK(check.reset == 1);
	CHECK(check.preset_k_value == 11u);
	CHECK(!check.has_exposure_texture);
	CHECK(!check.has_transparency_mask);
}
#endif // DLSS_BACKEND_NGX

} // namespace TestDLSS
