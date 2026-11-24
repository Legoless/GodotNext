/**************************************************************************/
/*  dlss_upscaler.cpp                                                     */
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

#include "dlss_upscaler.h"

#ifdef VULKAN_ENABLED
#include "dlss_vulkan_extensions.h"
#endif

#include "core/config/project_settings.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/version.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/rendering_context_driver.h"

#ifdef VULKAN_ENABLED
#include <drivers/vulkan/godot_vulkan.h>
#endif

namespace {

template <typename T>
T dlss_read_setting(const char *p_name, const T &p_default) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps == nullptr || !ps->has_setting(p_name)) {
		return p_default;
	}
	return ps->get_setting_with_override(p_name);
}

} // namespace

uint64_t DLSSUpscaler::_memory_report_callback(void *p_userdata) {
	DLSSUpscaler *self = static_cast<DLSSUpscaler *>(p_userdata);
	if (self == nullptr) {
		return 0;
	}
	MutexLock lock(self->mutex);
	return self->backend != nullptr ? self->backend->get_memory_bytes() : 0;
}

DLSSUpscaler::DLSSUpscaler(DLSSBackend *p_backend) :
		backend(p_backend) {
	RenderingContextDriver::add_memory_report_source("NGX", _memory_report_callback, this);
}

DLSSUpscaler::~DLSSUpscaler() {
	RenderingContextDriver::remove_memory_report_source("NGX");
	shutdown();
}

#ifdef VULKAN_ENABLED
void DLSSUpscaler::set_vulkan_extension_provider(DLSSVulkanExtensionProvider *p_provider) {
	MutexLock lock(mutex);
	vk_provider = p_provider;
}
#endif

void DLSSUpscaler::set_backend(DLSSBackend *p_backend) {
	MutexLock lock(mutex);
	if (backend == p_backend) {
		return;
	}
	_flush_retired_features_locked(true);
	for (const KeyValue<FeatureKey, FeatureSlot> &E : features) {
		if (backend != nullptr && E.value.handle != 0) {
			backend->release_feature(E.value.handle);
		}
	}
	features.clear();
	if (backend != nullptr && backend_init_attempted) {
		backend->shutdown();
	}
	backend = p_backend;
	backend_init_attempted = false;
	backend_init_status = DLSSBackend::OK;
	runtime_info_logged = false;
}

DLSSBackend::QualityMode DLSSUpscaler::snap_scale_to_quality(float p_scale) {
	if (p_scale >= 0.99f) {
		return DLSSBackend::QUALITY_DLAA;
	}
	if (p_scale >= 0.62f) {
		return DLSSBackend::QUALITY_MAX_QUALITY;
	}
	if (p_scale >= 0.54f) {
		return DLSSBackend::QUALITY_BALANCED;
	}
	if (p_scale >= 0.42f) {
		return DLSSBackend::QUALITY_MAX_PERFORMANCE;
	}
	return DLSSBackend::QUALITY_ULTRA_PERFORMANCE;
}

DLSSBackend::Preset DLSSUpscaler::preset_from_setting(int p_setting_value) {
	switch (p_setting_value) {
		case 1:
			return DLSSBackend::PRESET_J;
		case 2:
			return DLSSBackend::PRESET_K;
		case 3:
			return DLSSBackend::PRESET_L;
		case 4:
			return DLSSBackend::PRESET_M;
		default:
			return DLSSBackend::PRESET_DEFAULT;
	}
}

RSE::ViewportScaling3DReason DLSSUpscaler::map_status_to_reason(DLSSBackend::Status p_status) {
	switch (p_status) {
		case DLSSBackend::OK:
			return RSE::VIEWPORT_SCALING_3D_REASON_NONE;
		case DLSSBackend::NOT_NVIDIA_RTX:
			return RSE::VIEWPORT_SCALING_3D_REASON_NOT_NVIDIA_RTX;
		case DLSSBackend::DRIVER_TOO_OLD:
			return RSE::VIEWPORT_SCALING_3D_REASON_DRIVER_TOO_OLD;
		case DLSSBackend::RUNTIME_MISSING:
			return RSE::VIEWPORT_SCALING_3D_REASON_RUNTIME_MISSING;
		case DLSSBackend::EXTENSION_MISSING:
			return RSE::VIEWPORT_SCALING_3D_REASON_EXTENSION_MISSING;
		case DLSSBackend::PROJECT_ID_MISSING:
			return RSE::VIEWPORT_SCALING_3D_REASON_PROJECT_ID_MISSING;
		case DLSSBackend::FEATURE_CREATE_FAILED:
		default:
			return RSE::VIEWPORT_SCALING_3D_REASON_FEATURE_CREATE_FAILED;
	}
}

DLSSBackend::TextureResource DLSSUpscaler::convert_native_texture(const RDD::TextureNativeInfo &p_info, bool p_read_write) {
	DLSSBackend::TextureResource res;
	res.image_handle = p_info.image;
	res.image_view_handle = p_info.view;
	res.format = static_cast<uint32_t>(p_info.format);
	res.width = p_info.width;
	res.height = p_info.height;
	res.base_mip_level = p_info.base_mipmap;
	res.mip_levels = MAX(1u, p_info.mipmap_count);
	res.base_array_layer = p_info.base_layer;
	res.array_layers = MAX(1u, p_info.layer_count);
	res.read_write = p_read_write;
	uint32_t mask = 0;
	if (p_info.aspect.has_flag(RDD::TEXTURE_ASPECT_COLOR_BIT)) {
		mask |= 0x1u;
	}
	if (p_info.aspect.has_flag(RDD::TEXTURE_ASPECT_DEPTH_BIT)) {
		mask |= 0x2u;
	}
	if (p_info.aspect.has_flag(RDD::TEXTURE_ASPECT_STENCIL_BIT)) {
		mask |= 0x4u;
	}
	res.aspect_mask = mask != 0 ? mask : 0x1u;
	return res;
}

uint32_t DLSSUpscaler::get_active_feature_count() const {
	MutexLock lock(mutex);
	return features.size();
}

uint32_t DLSSUpscaler::get_retired_feature_count() const {
	MutexLock lock(mutex);
	return retired_features.size();
}

DLSSBackend::LoadedRuntimeInfo DLSSUpscaler::get_loaded_runtime_info() const {
	MutexLock lock(mutex);
	return backend != nullptr ? backend->get_loaded_runtime_info() : DLSSBackend::LoadedRuntimeInfo();
}

void DLSSUpscaler::_flush_retired_features_locked(bool p_force_all) {
	if (retired_features.is_empty()) {
		return;
	}
	const uint64_t delay = (RD::get_singleton() != nullptr) ? uint64_t(RD::get_singleton()->get_frame_delay()) : 2u;
	for (int i = int(retired_features.size()) - 1; i >= 0; i--) {
		const RetiredFeature &rf = retired_features[i];
		if (p_force_all || frame_counter >= rf.retire_frame + delay) {
			if (backend != nullptr && rf.handle != 0) {
				backend->release_feature(rf.handle);
			}
			retired_features.remove_at_unordered(i);
		}
	}
}

DLSSBackend::Status DLSSUpscaler::_ensure_backend_initialized_locked(const String &p_api_name, const String &p_project_id, uint64_t p_app_id, bool p_allow_ota) {
	if (backend == nullptr) {
		return DLSSBackend::RUNTIME_MISSING;
	}
	if (backend_init_attempted) {
		return backend_init_status;
	}

	if (p_project_id.is_empty() && p_app_id == 0) {
		// Do not permanently latch if the project sets project_id later; do not invoke backend init.
		return DLSSBackend::PROJECT_ID_MISSING;
	}

#ifdef VULKAN_ENABLED
	if (p_api_name == "Vulkan" && vk_provider != nullptr) {
		const DLSSBackend::Status avail = backend->query_availability(vk_provider->get_vendor_id(), vk_provider->are_extensions_satisfied());
		if (avail != DLSSBackend::OK) {
			backend_init_attempted = true;
			backend_init_status = avail;
			return avail;
		}
	}
#endif

	DLSSBackend::InitParams init_params;
	init_params.api_name = p_api_name;
	init_params.project_id = p_project_id;
	init_params.application_id = p_app_id;
	init_params.engine_version = String(GODOT_VERSION_FULL_BUILD);
	init_params.allow_ota_updates = p_allow_ota;

	if (OS::get_singleton() != nullptr) {
		init_params.runtime_search_path = OS::get_singleton()->get_executable_path().get_base_dir();
		init_params.application_data_path = OS::get_singleton()->get_user_data_dir();
		if (init_params.application_data_path.is_empty()) {
			init_params.application_data_path = init_params.runtime_search_path;
		}
	}

	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr) {
		init_params.instance_handle = rd->get_driver_resource(RD::DRIVER_RESOURCE_TOPMOST_OBJECT, RID(), 0);
		init_params.physical_device_handle = rd->get_driver_resource(RD::DRIVER_RESOURCE_PHYSICAL_DEVICE, RID(), 0);
		init_params.device_handle = rd->get_driver_resource(RD::DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0);
#ifdef VULKAN_ENABLED
		if (p_api_name == "Vulkan") {
			init_params.gipa_handle = reinterpret_cast<uint64_t>(vkGetInstanceProcAddr);
			init_params.gdpa_handle = reinterpret_cast<uint64_t>(vkGetDeviceProcAddr);
		}
#endif
	}

	backend_init_attempted = true;
	backend_init_status = backend->init(init_params);
	return backend_init_status;
}

Size2i DLSSUpscaler::get_render_size(const Size2i &p_target_size, float p_scale) const {
	const DLSSBackend::QualityMode quality = snap_scale_to_quality(p_scale);
	MutexLock lock(mutex);
	if (backend != nullptr && backend_init_attempted && backend_init_status == DLSSBackend::OK) {
		DLSSBackend::OptimalSettings opt;
		if (backend->get_optimal_settings(p_target_size, quality, opt) == DLSSBackend::OK && opt.optimal_size.width > 0 && opt.optimal_size.height > 0) {
			return opt.optimal_size;
		}
	}
	if (quality == DLSSBackend::QUALITY_DLAA) {
		return p_target_size;
	}
	return Size2i(
			MAX(int(Math::round(float(p_target_size.width) * p_scale)), 1),
			MAX(int(Math::round(float(p_target_size.height) * p_scale)), 1));
}

bool DLSSUpscaler::supports_api(const String &p_api_name) const {
	return p_api_name == "Vulkan";
}

uint32_t DLSSUpscaler::jitter_phase_count(const Size2i &p_target_size, const Size2i &p_render_size) const {
	return nextgi_temporal_jitter_phase_count(p_target_size, p_render_size, true);
}

RSE::ViewportScaling3DReason DLSSUpscaler::get_availability(uint32_t p_view_count, float p_scale, float &r_effective_scale) const {
	r_effective_scale = p_scale;
	const String project_id = dlss_read_setting<String>("rendering/scaling_3d/dlss/project_id", String());
	const int64_t app_id = dlss_read_setting<int64_t>("rendering/scaling_3d/dlss/application_id", 0);
	const bool allow_ota = dlss_read_setting<bool>("rendering/scaling_3d/dlss/allow_ota_updates", false);

	String api_name = "Vulkan";
	if (RD::get_singleton() != nullptr) {
		api_name = RD::get_singleton()->get_device_api_name();
	}

	MutexLock lock(mutex);
	if (backend == nullptr) {
		return RSE::VIEWPORT_SCALING_3D_REASON_MODULE_DISABLED;
	}

	uint32_t vendor_id = 0;
	bool exts_ok = true;
#ifdef VULKAN_ENABLED
	if (api_name == "Vulkan" && vk_provider != nullptr && vk_provider->is_device_created()) {
		vendor_id = vk_provider->get_vendor_id();
		exts_ok = vk_provider->are_extensions_satisfied();
	}
#endif
	const DLSSBackend::Status pre_avail = backend->query_availability(vendor_id, exts_ok);
	if (pre_avail != DLSSBackend::OK) {
		return map_status_to_reason(pre_avail);
	}

	if (project_id.is_empty() && app_id <= 0) {
		return RSE::VIEWPORT_SCALING_3D_REASON_PROJECT_ID_MISSING;
	}

	// Attempt lazy backend initialization on the render thread when a RenderingDevice is live.
	if (!backend_init_attempted && RD::get_singleton() != nullptr) {
		const_cast<DLSSUpscaler *>(this)->_ensure_backend_initialized_locked(api_name, project_id, uint64_t(MAX<int64_t>(0, app_id)), allow_ota);
	}

	if (backend_init_attempted && backend_init_status != DLSSBackend::OK) {
		return map_status_to_reason(backend_init_status);
	}

	return RSE::VIEWPORT_SCALING_3D_REASON_NONE;
}

RSE::ViewportScaling3DReason DLSSUpscaler::ensure_context(RenderSceneBuffersRD *p_render_buffers, uint32_t p_view) {
	float eff = 1.0f;
	const RSE::ViewportScaling3DReason avail = get_availability(1, 1.0f, eff);
	if (avail != RSE::VIEWPORT_SCALING_3D_REASON_NONE) {
		return avail;
	}
	return get_context_error(p_render_buffers);
}

void DLSSUpscaler::_driver_callback_run(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_cmd_buffer, void *p_userdata) {
	CallbackArgs *args = static_cast<CallbackArgs *>(p_userdata);
	if (args == nullptr || args->upscaler == nullptr) {
		return;
	}
	DLSSUpscaler *upscaler = args->upscaler;
	const DLSSBackend::Status st = upscaler->_execute_callback(p_driver, p_cmd_buffer, *args);
	if (st != DLSSBackend::OK && args->render_buffers != nullptr) {
		upscaler->latch_context_error(args->render_buffers, map_status_to_reason(st));
	}
	upscaler->callback_args_allocator.free(args);
}

DLSSBackend::Status DLSSUpscaler::_execute_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_cmd_buffer, const CallbackArgs &p_args) {
	const String project_id = dlss_read_setting<String>("rendering/scaling_3d/dlss/project_id", String());
	const int64_t app_id = dlss_read_setting<int64_t>("rendering/scaling_3d/dlss/application_id", 0);
	const bool allow_ota = dlss_read_setting<bool>("rendering/scaling_3d/dlss/allow_ota_updates", false);
	const int preset_val = dlss_read_setting<int>("rendering/scaling_3d/dlss/preset", 0);
	const String api_name = (RD::get_singleton() != nullptr) ? RD::get_singleton()->get_device_api_name() : String("Vulkan");

	MutexLock lock(mutex);
	frame_counter++;
	_flush_retired_features_locked(false);

	const DLSSBackend::Status init_st = _ensure_backend_initialized_locked(api_name, project_id, uint64_t(MAX<int64_t>(0, app_id)), allow_ota);
	if (init_st != DLSSBackend::OK) {
		return init_st;
	}

	const uint64_t cmd_handle = p_driver != nullptr ? p_driver->command_buffer_get_native_handle(p_cmd_buffer) : 0;
	const float ratio = p_args.output_size.width > 0 ? float(p_args.internal_size.width) / float(p_args.output_size.width) : 1.0f;
	const DLSSBackend::QualityMode quality = snap_scale_to_quality(ratio);
	const DLSSBackend::Preset preset = preset_from_setting(preset_val);

	const FeatureKey key{ p_args.render_buffers, p_args.view };
	FeatureSlot *slot = features.getptr(key);
	const bool needs_create = slot == nullptr ||
			slot->handle == 0 ||
			slot->render_size != p_args.internal_size ||
			slot->output_size != p_args.output_size ||
			slot->quality_mode != quality ||
			slot->preset != preset ||
			slot->ray_reconstruction != p_args.ray_reconstruction;

	bool force_reset = p_args.reset;
	if (needs_create) {
		if (slot != nullptr && slot->handle != 0) {
			retired_features.push_back({ slot->handle, frame_counter });
			slot->handle = 0;
		}
		DLSSBackend::FeatureCreateParams create_params;
		create_params.command_buffer_handle = cmd_handle;
		create_params.render_size = p_args.internal_size;
		create_params.output_size = p_args.output_size;
		create_params.quality_mode = quality;
		create_params.preset = preset;
		create_params.is_hdr = true;
		create_params.depth_inverted = true;
		create_params.mv_low_res = true;
		create_params.mv_jittered = false;
		create_params.auto_exposure = true;
		create_params.ray_reconstruction = p_args.ray_reconstruction;

		uint64_t new_handle = 0;
		const DLSSBackend::Status create_st = backend->create_feature(create_params, new_handle);
		if (create_st != DLSSBackend::OK || new_handle == 0) {
			return create_st != DLSSBackend::OK ? create_st : DLSSBackend::FEATURE_CREATE_FAILED;
		}

		FeatureSlot updated;
		updated.handle = new_handle;
		updated.render_size = p_args.internal_size;
		updated.output_size = p_args.output_size;
		updated.quality_mode = quality;
		updated.preset = preset;
		updated.ray_reconstruction = p_args.ray_reconstruction;
		features[key] = updated;
		slot = features.getptr(key);
		force_reset = true;

		if (!runtime_info_logged) {
			runtime_info_logged = true;
			const DLSSBackend::LoadedRuntimeInfo rt = backend->get_loaded_runtime_info();
#ifdef VULKAN_ENABLED
			if (vk_provider != nullptr && !rt.sha256.is_empty()) {
				vk_provider->set_loaded_dll_sha256(rt.sha256);
			}
#endif
			if (!rt.path.is_empty()) {
				print_verbose(vformat("DLSS runtime loaded: path=%s sha256=%s manifest_match=%s", rt.path, rt.sha256, rt.sha256_matches ? "true" : "false"));
			}
		}
	}

	DLSSBackend::EvaluateParams eval_params;
	eval_params.command_buffer_handle = cmd_handle;
	eval_params.feature_handle = slot->handle;
	eval_params.color = convert_native_texture(p_args.color_info, false);
	eval_params.depth = convert_native_texture(p_args.depth_info, false);
	eval_params.depth.aspect_mask = 0x2u; // VK_IMAGE_ASPECT_DEPTH_BIT.
	eval_params.velocity = convert_native_texture(p_args.velocity_info, false);
	eval_params.output = convert_native_texture(p_args.output_info, true);
	eval_params.render_size = p_args.internal_size;
	eval_params.output_size = p_args.output_size;
	eval_params.jitter_offset = p_args.jitter;
	eval_params.mv_scale = (p_args.motion_vector_scale.x != 0.0f || p_args.motion_vector_scale.y != 0.0f) ? p_args.motion_vector_scale : Vector2(float(p_args.internal_size.width), float(p_args.internal_size.height));
	eval_params.pre_exposure = 1.0f;
	eval_params.exposure_scale = 1.0f;
	eval_params.frame_time_delta_ms = p_args.delta_time_ms;
	eval_params.reset = force_reset;
	eval_params.ray_reconstruction = p_args.ray_reconstruction;
	eval_params.world_to_view = p_args.world_to_view;
	eval_params.view_to_clip = p_args.view_to_clip;

	return backend->evaluate(eval_params);
}

void DLSSUpscaler::process(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Inputs &p_inputs) {
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL(rd);
	ERR_FAIL_COND(p_render_buffers.is_null());

	CallbackArgs *args = callback_args_allocator.alloc();
	args->upscaler = this;
	args->render_buffers = p_render_buffers.ptr();
	args->view = p_view;
	const Error err_color = rd->texture_get_native_info(p_inputs.color, args->color_info);
	const Error err_depth = rd->texture_get_native_info(p_inputs.depth, args->depth_info);
	const Error err_vel = rd->texture_get_native_info(p_inputs.velocity, args->velocity_info);
	const Error err_out = rd->texture_get_native_info(p_inputs.output, args->output_info);
	if (err_color != OK || err_depth != OK || err_vel != OK || err_out != OK) {
		callback_args_allocator.free(args);
		latch_context_error(p_render_buffers.ptr(), RSE::VIEWPORT_SCALING_3D_REASON_FEATURE_CREATE_FAILED);
		return;
	}
	args->internal_size = p_inputs.internal_size;
	args->output_size = p_inputs.output_size;
	args->motion_vector_scale = Vector2(float(p_inputs.motion_vector_scale.width), float(p_inputs.motion_vector_scale.height));
	args->jitter = p_inputs.jitter;
	args->delta_time_ms = p_inputs.delta_time * 1000.0f;
	args->reset = p_inputs.reset;
	args->ray_reconstruction = false;
	args->world_to_view = p_inputs.world_to_view;
	args->view_to_clip = p_inputs.view_to_clip;

	RD::CallbackResource res[4] = {
		{ p_inputs.color, RD::CALLBACK_RESOURCE_TYPE_TEXTURE, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ p_inputs.depth, RD::CALLBACK_RESOURCE_TYPE_TEXTURE, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		{ p_inputs.velocity, RD::CALLBACK_RESOURCE_TYPE_TEXTURE, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE },
		// NGX may clear and write the output texture inside Evaluate; GENERAL synchronizes both transfer/clear and storage writes.
		{ p_inputs.output, RD::CALLBACK_RESOURCE_TYPE_TEXTURE, RD::CALLBACK_RESOURCE_USAGE_GENERAL },
	};

	const Error err = rd->driver_callback_add(_driver_callback_run, args, VectorView<RD::CallbackResource>(res, 4), RDD::PIPELINE_STAGE_ALL_COMMANDS_BIT);
	if (err != OK) {
		callback_args_allocator.free(args);
		latch_context_error(p_render_buffers.ptr(), RSE::VIEWPORT_SCALING_3D_REASON_FEATURE_CREATE_FAILED);
	}
}

void DLSSUpscaler::free_contexts(RenderSceneBuffersRD *p_render_buffers) {
	MutexLock lock(mutex);
	LocalVector<FeatureKey> to_remove;
	for (const KeyValue<FeatureKey, FeatureSlot> &E : features) {
		if (E.key.render_buffers == p_render_buffers) {
			if (E.value.handle != 0) {
				retired_features.push_back({ E.value.handle, frame_counter });
			}
			to_remove.push_back(E.key);
		}
	}
	for (const FeatureKey &k : to_remove) {
		features.erase(k);
	}
	clear_context_error(p_render_buffers);
}

void DLSSUpscaler::shutdown() {
	MutexLock lock(mutex);
	_flush_retired_features_locked(true);
	for (const KeyValue<FeatureKey, FeatureSlot> &E : features) {
		if (backend != nullptr && E.value.handle != 0) {
			backend->release_feature(E.value.handle);
		}
	}
	features.clear();
	if (backend != nullptr && backend_init_attempted) {
		backend->shutdown();
	}
	backend_init_attempted = false;
	backend_init_status = DLSSBackend::OK;
	runtime_info_logged = false;
}

DLSSBackend::Status DLSSUpscaler::test_ensure_feature_for_view(RenderSceneBuffersRD *p_render_buffers, uint32_t p_view, const Size2i &p_render_size, const Size2i &p_output_size, float p_scale, int p_preset, uint64_t &r_handle) {
	r_handle = 0;
	MutexLock lock(mutex);
	if (backend == nullptr) {
		return DLSSBackend::RUNTIME_MISSING;
	}
	const DLSSBackend::QualityMode quality = snap_scale_to_quality(p_scale);
	const DLSSBackend::Preset preset = preset_from_setting(p_preset);
	const FeatureKey key{ p_render_buffers, p_view };
	FeatureSlot *slot = features.getptr(key);
	if (slot != nullptr && slot->handle != 0 && slot->render_size == p_render_size && slot->output_size == p_output_size && slot->quality_mode == quality && slot->preset == preset) {
		r_handle = slot->handle;
		return DLSSBackend::OK;
	}
	if (slot != nullptr && slot->handle != 0) {
		retired_features.push_back({ slot->handle, frame_counter });
		slot->handle = 0;
	}
	DLSSBackend::FeatureCreateParams params;
	params.render_size = p_render_size;
	params.output_size = p_output_size;
	params.quality_mode = quality;
	params.preset = preset;
	params.is_hdr = true;
	params.depth_inverted = true;
	params.mv_low_res = true;
	params.mv_jittered = false;
	params.auto_exposure = true;
	uint64_t handle = 0;
	const DLSSBackend::Status st = backend->create_feature(params, handle);
	if (st != DLSSBackend::OK || handle == 0) {
		const DLSSBackend::Status err_st = st != DLSSBackend::OK ? st : DLSSBackend::FEATURE_CREATE_FAILED;
		latch_context_error(p_render_buffers, map_status_to_reason(err_st));
		return err_st;
	}
	features[key] = { handle, p_render_size, p_output_size, quality, preset, false };
	r_handle = handle;
	return DLSSBackend::OK;
}

void DLSSUpscaler::test_advance_frame(uint32_t p_frame_delay) {
	MutexLock lock(mutex);
	frame_counter += p_frame_delay;
	_flush_retired_features_locked(false);
}
