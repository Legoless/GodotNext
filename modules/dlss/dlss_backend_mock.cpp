/**************************************************************************/
/*  dlss_backend_mock.cpp                                                 */
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

#include "dlss_backend_mock.h"

#include "core/math/math_funcs.h"

namespace {
constexpr uint32_t VENDOR_ID_NVIDIA = 0x10DE;
}

DLSSBackendMock::DLSSBackendMock() {
	reset_script();
}

void DLSSBackendMock::reset_script() {
	scripted_init_status = OK;
	scripted_availability_status = OK;
	scripted_create_status = OK;
	scripted_evaluate_status = OK;
	scripted_result_code = 1;
	per_feature_memory_bytes = 64u * 1024u * 1024u;
	scripted_instance_extensions.clear();
	scripted_device_extensions.clear();
	scripted_runtime_info = LoadedRuntimeInfo();
	initialized = false;
	init_call_count = 0;
	shutdown_call_count = 0;
	next_feature_handle = 1;
	active_features.clear();
	evaluate_records.clear();
}

Vector<CharString> DLSSBackendMock::get_instance_extensions() {
	return scripted_instance_extensions;
}

Vector<CharString> DLSSBackendMock::get_device_extensions(uint64_t p_instance_handle, uint64_t p_physical_device_handle) {
	return scripted_device_extensions;
}

DLSSBackend::Status DLSSBackendMock::init(const InitParams &p_params) {
	if (p_params.project_id.is_empty() && p_params.application_id == 0) {
		return PROJECT_ID_MISSING;
	}
	init_call_count++;
	if (scripted_init_status != OK) {
		initialized = false;
		return scripted_init_status;
	}
	initialized = true;
	return OK;
}

DLSSBackend::Status DLSSBackendMock::query_availability(uint32_t p_vendor_id, bool p_extensions_satisfied) {
	if (p_vendor_id != 0 && p_vendor_id != VENDOR_ID_NVIDIA) {
		return NOT_NVIDIA_RTX;
	}
	if (!p_extensions_satisfied) {
		return EXTENSION_MISSING;
	}
	return scripted_availability_status;
}

DLSSBackend::Status DLSSBackendMock::get_optimal_settings(const Size2i &p_output_size, QualityMode p_quality_mode, OptimalSettings &r_settings) {
	float scale = 1.0f;
	switch (p_quality_mode) {
		case QUALITY_ULTRA_PERFORMANCE:
			scale = 1.0f / 3.0f;
			break;
		case QUALITY_MAX_PERFORMANCE:
			scale = 0.5f;
			break;
		case QUALITY_BALANCED:
			scale = 0.58f;
			break;
		case QUALITY_MAX_QUALITY:
			scale = 2.0f / 3.0f;
			break;
		case QUALITY_DLAA:
		default:
			scale = 1.0f;
			break;
	}
	r_settings.optimal_size = Size2i(
			MAX(1, int(Math::round(float(p_output_size.width) * scale))),
			MAX(1, int(Math::round(float(p_output_size.height) * scale))));
	r_settings.min_size = Size2i(
			MAX(1, int(Math::round(float(p_output_size.width) * 0.30f))),
			MAX(1, int(Math::round(float(p_output_size.height) * 0.30f))));
	r_settings.max_size = p_output_size;
	r_settings.sharpness = 0.0f;
	return OK;
}

DLSSBackend::Status DLSSBackendMock::create_feature(const FeatureCreateParams &p_params, uint64_t &r_feature_handle) {
	r_feature_handle = 0;
	if (!initialized) {
		return FEATURE_CREATE_FAILED;
	}
	if (scripted_create_status != OK) {
		return scripted_create_status;
	}
	const uint64_t handle = next_feature_handle++;
	active_features[handle] = p_params;
	r_feature_handle = handle;
	return OK;
}

DLSSBackend::Status DLSSBackendMock::evaluate(const EvaluateParams &p_params) {
	if (!initialized || !active_features.has(p_params.feature_handle)) {
		return FEATURE_CREATE_FAILED;
	}
	if (scripted_evaluate_status != OK) {
		return scripted_evaluate_status;
	}
	EvaluateRecord rec;
	rec.feature_handle = p_params.feature_handle;
	rec.render_size = p_params.render_size;
	rec.output_size = p_params.output_size;
	rec.jitter_offset = p_params.jitter_offset;
	rec.mv_scale = p_params.mv_scale;
	rec.pre_exposure = p_params.pre_exposure;
	rec.reset = p_params.reset;
	rec.ray_reconstruction = p_params.ray_reconstruction;
	evaluate_records.push_back(rec);
	return OK;
}

void DLSSBackendMock::release_feature(uint64_t p_feature_handle) {
	active_features.erase(p_feature_handle);
}

void DLSSBackendMock::shutdown() {
	shutdown_call_count++;
	active_features.clear();
	initialized = false;
}

uint64_t DLSSBackendMock::get_memory_bytes() const {
	return uint64_t(active_features.size()) * per_feature_memory_bytes;
}
