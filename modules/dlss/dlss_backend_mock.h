/**************************************************************************/
/*  dlss_backend_mock.h                                                   */
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

#include "dlss_backend.h"

#include "core/templates/hash_map.h"

class DLSSBackendMock : public DLSSBackend {
public:
	struct EvaluateRecord {
		uint64_t feature_handle = 0;
		Size2i render_size;
		Size2i output_size;
		Vector2 jitter_offset;
		Vector2 mv_scale;
		float pre_exposure = 1.0f;
		bool reset = false;
		bool ray_reconstruction = false;
	};

private:
	Status scripted_init_status = OK;
	Status scripted_availability_status = OK;
	Status scripted_create_status = OK;
	Status scripted_evaluate_status = OK;
	int32_t scripted_result_code = 1;
	uint64_t per_feature_memory_bytes = 64u * 1024u * 1024u;

	Vector<CharString> scripted_instance_extensions;
	Vector<CharString> scripted_device_extensions;
	LoadedRuntimeInfo scripted_runtime_info;

	bool initialized = false;
	uint32_t init_call_count = 0;
	uint32_t shutdown_call_count = 0;
	uint64_t next_feature_handle = 1;
	HashMap<uint64_t, FeatureCreateParams> active_features;
	Vector<EvaluateRecord> evaluate_records;

public:
	DLSSBackendMock();
	~DLSSBackendMock() override = default;

	// Test scripting controls.
	void reset_script();
	void set_scripted_init_status(Status p_status) { scripted_init_status = p_status; }
	void set_scripted_availability_status(Status p_status) { scripted_availability_status = p_status; }
	void set_scripted_create_status(Status p_status) { scripted_create_status = p_status; }
	void set_scripted_evaluate_status(Status p_status) { scripted_evaluate_status = p_status; }
	void set_scripted_result_code(int32_t p_code) { scripted_result_code = p_code; }
	void set_per_feature_memory_bytes(uint64_t p_bytes) { per_feature_memory_bytes = p_bytes; }
	void set_scripted_instance_extensions(const Vector<CharString> &p_exts) { scripted_instance_extensions = p_exts; }
	void set_scripted_device_extensions(const Vector<CharString> &p_exts) { scripted_device_extensions = p_exts; }
	void set_scripted_runtime_info(const LoadedRuntimeInfo &p_info) { scripted_runtime_info = p_info; }

	bool is_initialized() const { return initialized; }
	uint32_t get_init_call_count() const { return init_call_count; }
	uint32_t get_shutdown_call_count() const { return shutdown_call_count; }
	uint32_t get_active_feature_count() const { return active_features.size(); }
	const Vector<EvaluateRecord> &get_evaluate_records() const { return evaluate_records; }
	void clear_evaluate_records() { evaluate_records.clear(); }

	// DLSSBackend interface.
	String get_backend_name() const override { return "mock"; }
	Vector<CharString> get_instance_extensions() override;
	Vector<CharString> get_device_extensions(uint64_t p_instance_handle, uint64_t p_physical_device_handle) override;

	Status init(const InitParams &p_params) override;
	Status query_availability(uint32_t p_vendor_id, bool p_extensions_satisfied) override;
	Status get_optimal_settings(const Size2i &p_output_size, QualityMode p_quality_mode, OptimalSettings &r_settings) override;
	Status create_feature(const FeatureCreateParams &p_params, uint64_t &r_feature_handle) override;
	Status evaluate(const EvaluateParams &p_params) override;
	void release_feature(uint64_t p_feature_handle) override;
	void shutdown() override;

	uint64_t get_memory_bytes() const override;
	int32_t get_last_result_code() const override { return scripted_result_code; }
	LoadedRuntimeInfo get_loaded_runtime_info() const override { return scripted_runtime_info; }
};
