/**************************************************************************/
/*  dlss_backend.h                                                        */
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

// GodotNext: pu_dlss_module_build (#99).
// Pure MIT backend seam for DLSS integration: no Vulkan, Direct3D 12, or proprietary SDK headers.
// All native API objects cross this boundary as opaque uint64_t handles.

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"

#include <cstdint>

class DLSSBackend {
public:
	enum Status {
		OK = 0,
		NOT_NVIDIA_RTX,
		DRIVER_TOO_OLD,
		RUNTIME_MISSING,
		EXTENSION_MISSING,
		PROJECT_ID_MISSING,
		FEATURE_CREATE_FAILED,
	};

	enum QualityMode {
		QUALITY_MAX_PERFORMANCE = 0,
		QUALITY_BALANCED = 1,
		QUALITY_MAX_QUALITY = 2,
		QUALITY_ULTRA_PERFORMANCE = 3,
		QUALITY_DLAA = 4,
	};

	enum Preset {
		PRESET_DEFAULT = 0,
		PRESET_J = 1,
		PRESET_K = 2,
		PRESET_L = 3,
		PRESET_M = 4,
	};

	struct TextureResource {
		uint64_t image_handle = 0;
		uint64_t image_view_handle = 0;
		uint32_t format = 0;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t aspect_mask = 0;
		uint32_t base_mip_level = 0;
		uint32_t mip_levels = 1;
		uint32_t base_array_layer = 0;
		uint32_t array_layers = 1;
		bool read_write = false;
	};

	struct InitParams {
		String api_name;
		String project_id;
		uint64_t application_id = 0;
		String engine_version;
		String application_data_path;
		String runtime_search_path;
		uint64_t instance_handle = 0;
		uint64_t physical_device_handle = 0;
		uint64_t device_handle = 0;
		uint64_t gipa_handle = 0;
		uint64_t gdpa_handle = 0;
		bool allow_ota_updates = false;
	};

	struct OptimalSettings {
		Size2i optimal_size;
		Size2i min_size;
		Size2i max_size;
		float sharpness = 0.0f;
	};

	struct FeatureCreateParams {
		uint64_t command_buffer_handle = 0;
		Size2i render_size;
		Size2i output_size;
		QualityMode quality_mode = QUALITY_MAX_QUALITY;
		Preset preset = PRESET_DEFAULT;
		bool is_hdr = true;
		bool depth_inverted = true;
		bool mv_low_res = true;
		bool mv_jittered = false;
		bool auto_exposure = true;
		bool ray_reconstruction = false;
	};

	struct EvaluateParams {
		uint64_t command_buffer_handle = 0;
		uint64_t feature_handle = 0;
		TextureResource color;
		TextureResource depth;
		TextureResource velocity;
		TextureResource output;
		TextureResource diffuse_albedo;
		TextureResource specular_albedo;
		TextureResource normal_roughness;
		TextureResource specular_hit_distance;
		Size2i render_size;
		Size2i output_size;
		Vector2 jitter_offset;
		Vector2 mv_scale;
		float pre_exposure = 1.0f;
		float exposure_scale = 1.0f;
		float frame_time_delta_ms = 0.0f;
		bool reset = false;
		bool ray_reconstruction = false;
		Transform3D world_to_view;
		Projection view_to_clip;
	};

	struct LoadedRuntimeInfo {
		String path;
		String version;
		String sha256;
		String manifest_sha256;
		bool sha256_matches = false;
	};

	virtual ~DLSSBackend() = default;

	virtual String get_backend_name() const = 0;
	virtual Vector<CharString> get_instance_extensions() = 0;
	virtual Vector<CharString> get_device_extensions(uint64_t p_instance_handle, uint64_t p_physical_device_handle) = 0;

	virtual Status init(const InitParams &p_params) = 0;
	virtual Status query_availability(uint32_t p_vendor_id, bool p_extensions_satisfied) = 0;
	virtual Status get_optimal_settings(const Size2i &p_output_size, QualityMode p_quality_mode, OptimalSettings &r_settings) = 0;
	virtual Status create_feature(const FeatureCreateParams &p_params, uint64_t &r_feature_handle) = 0;
	virtual Status evaluate(const EvaluateParams &p_params) = 0;
	virtual void release_feature(uint64_t p_feature_handle) = 0;
	virtual void shutdown() = 0;

	virtual uint64_t get_memory_bytes() const = 0;
	virtual int32_t get_last_result_code() const { return 0; }
	virtual LoadedRuntimeInfo get_loaded_runtime_info() const { return LoadedRuntimeInfo(); }
};

#ifdef DLSS_BACKEND_NGX
DLSSBackend *dlss_backend_create_ngx();
#endif
