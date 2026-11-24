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

#include "core/string/ustring.h"
#include "core/version.h"

#include "modules/dlss/dlss_backend_mock.h"
#include "modules/dlss/register_types.h"

namespace TestDLSS {

DLSSBackend::InitParams make_test_init_params(const String &p_project_id);

TEST_CASE("[DLSS][Mock] Module version config carries .dlss and active backend exists") {
	const String mod_cfg = String(GODOT_VERSION_MODULE_CONFIG);
	CHECK(mod_cfg.contains(".dlss"));

	DLSSBackend *active = dlss_get_active_backend();
	REQUIRE(active != nullptr);
#ifdef DLSS_BACKEND_NGX
	CHECK(active->get_backend_name() == "ngx");
#else
	CHECK(active->get_backend_name() == "mock");
#endif
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

} // namespace TestDLSS
