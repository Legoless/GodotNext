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

#include "dlss_ngx_d3d12.h"

#ifdef VULKAN_ENABLED
#include "dlss_vulkan_extensions.h"
#endif

#include "core/config/project_settings.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/version.h"
#include "servers/rendering/renderer_rd/effects/denoiser_bypass.h"
#include "servers/rendering/renderer_rd/effects/rr_guides.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
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

String dlss_current_api_name() {
	if (RD::get_singleton() != nullptr) {
		return RD::get_singleton()->get_device_api_name();
	}
#ifdef D3D12_ENABLED
	if (OS::get_singleton() != nullptr && OS::get_singleton()->get_current_rendering_driver_name() == "d3d12") {
		return "D3D12";
	}
#endif
	return "Vulkan";
}

} // namespace

uint64_t DLSSUpscaler::_memory_report_callback(void *p_userdata) {
	DLSSUpscaler *self = static_cast<DLSSUpscaler *>(p_userdata);
	if (self == nullptr) {
		return 0;
	}
	MutexLock lock(self->mutex);
	uint64_t total = 0;
	if (self->backend != nullptr) {
		total += self->backend->get_memory_bytes();
	}
	if (self->d3d12_backend != nullptr && self->d3d12_backend != self->backend) {
		total += self->d3d12_backend->get_memory_bytes();
	}
	return total;
}

DLSSUpscaler::DLSSUpscaler(DLSSBackend *p_backend, DLSSBackend *p_d3d12_backend) :
		backend(p_backend), d3d12_backend(p_d3d12_backend) {
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
	DLSSBackend *owner = initialized_backend != nullptr ? initialized_backend : backend;
	for (const KeyValue<FeatureKey, FeatureSlot> &E : features) {
		if (owner != nullptr && E.value.handle != 0) {
			owner->release_feature(E.value.handle);
		}
	}
	features.clear();
	if (owner != nullptr && backend_init_attempted) {
		owner->shutdown();
	}
	backend = p_backend;
	initialized_backend = nullptr;
	backend_init_attempted = false;
	backend_init_status = DLSSBackend::OK;
	runtime_info_logged = false;
}

void DLSSUpscaler::set_d3d12_backend(DLSSBackend *p_d3d12_backend) {
	MutexLock lock(mutex);
	d3d12_backend = p_d3d12_backend;
}

DLSSBackend *DLSSUpscaler::select_backend_for_api(const String &p_api_name, DLSSBackend *p_primary_backend, DLSSBackend *p_d3d12_backend) {
	if (p_api_name == "D3D12") {
		return p_d3d12_backend != nullptr ? p_d3d12_backend : p_primary_backend;
	}
	return p_primary_backend;
}

String DLSSUpscaler::rr_maturity_label_for_api(const String &p_api_name) {
	if (p_api_name == "Vulkan") {
		return "beta";
	}
	return String();
}

DLSSBackend *DLSSUpscaler::_resolve_backend_locked(const String &p_api_name) const {
	return select_backend_for_api(p_api_name, backend, d3d12_backend);
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

DLSSBackend::RRPreset DLSSUpscaler::rr_preset_from_setting(int p_setting_value) {
	switch (p_setting_value) {
		case 1:
			return DLSSBackend::RR_PRESET_D;
		case 2:
			return DLSSBackend::RR_PRESET_E;
		case 3:
			return DLSSBackend::RR_PRESET_F;
		default:
			return DLSSBackend::RR_PRESET_DEFAULT;
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
	DLSSBackend *active_be = initialized_backend != nullptr ? initialized_backend : _resolve_backend_locked(dlss_current_api_name());
	return active_be != nullptr ? active_be->get_loaded_runtime_info() : DLSSBackend::LoadedRuntimeInfo();
}

void DLSSUpscaler::_sync_rr_guides_request_locked() const {
	bool any_active = false;
	for (const KeyValue<const RenderSceneBuffersRD *, RRViewportState> &E : rr_states) {
		if (E.value.active) {
			any_active = true;
			break;
		}
	}
	RendererRD::RRGuides::set_requested(any_active);
}

RRActivationResult DLSSUpscaler::_evaluate_rr_for_rb_locked(const RenderSceneBuffersRD *p_rb) const {
	const bool rr_requested = dlss_read_setting<bool>("rendering/scaling_3d/dlss/ray_reconstruction", false);
	if (!rr_requested) {
		RRViewportState *prev = rr_states.getptr(p_rb);
		if (prev != nullptr) {
			const bool was_active = prev->active;
			rr_states.erase(p_rb);
			if (was_active) {
				if (p_rb != nullptr) {
					DenoiserBypass::set_bypassed(p_rb, false);
				}
				_sync_rr_guides_request_locked();
			}
		}
		return RRActivationResult();
	}

	const String api_name = dlss_current_api_name();
	DLSSBackend *active_be = initialized_backend != nullptr ? initialized_backend : _resolve_backend_locked(api_name);

	bool dlss_active = (active_be != nullptr) && supports_api(api_name) &&
			(!backend_init_attempted || backend_init_status == DLSSBackend::OK);
	if (p_rb != nullptr && get_context_error(p_rb) != RSE::VIEWPORT_SCALING_3D_REASON_NONE) {
		dlss_active = false;
	}

	uint32_t view_count = 1;
	bool msaa_on = false;
	if (p_rb != nullptr) {
		view_count = p_rb->get_view_count();
		msaa_on = p_rb->get_msaa_3d() != RSE::VIEWPORT_MSAA_DISABLED;
	}

	const RendererRD::NextGI *ngi = RendererRD::NextGI::get_singleton();
	const bool nextgi_hwrt = (ngi != nullptr && ngi->get_tier_result().tier == RendererRD::NextGI::TIER_HWRT);
	const bool nextgi_auto = (ngi == nullptr || ngi->get_quality_setting() == RendererRD::NextGI::QUALITY_AUTO);
	const bool nextgi_explicit_epic = (ngi != nullptr && ngi->get_quality_setting() == RendererRD::NextGI::QUALITY_EPIC);

	const RRViewportState *prev_state = rr_states.getptr(p_rb);
	const bool prev_failed = (prev_state != nullptr && prev_state->failed);
	bool rr_backend_ok = !prev_failed && !msaa_on && active_be != nullptr &&
			(!backend_init_attempted || active_be->query_rr_availability() == DLSSBackend::OK);

	const RRActivationResult res = nextgi_resolve_rr_activation(
			rr_requested,
			dlss_active,
			nextgi_hwrt,
			nextgi_explicit_epic,
			nextgi_auto,
			view_count,
			rr_backend_ok);

	RRViewportState state;
	state.active = res.active;
	state.reason = res.reason;
	state.failed = prev_failed;
	rr_states[p_rb] = state;

	if (p_rb != nullptr) {
		DenoiserBypass::set_bypassed(p_rb, res.active);
	}
	_sync_rr_guides_request_locked();

	if (res.active) {
		if (api_name == "Vulkan" && !rr_vulkan_beta_logged) {
			rr_vulkan_beta_logged = true;
			print_line("DLSS Ray Reconstruction active (Beta on Vulkan); bypassing built-in line-8 denoiser.");
		}
	} else if (res.reason != RSE::VIEWPORT_SCALING_3D_REASON_NONE) {
		const uint32_t bit = 1u << static_cast<uint32_t>(res.reason);
		if ((rr_logged_fallback_mask & bit) == 0) {
			rr_logged_fallback_mask |= bit;
			const char *reason_text = (res.reason == RSE::VIEWPORT_SCALING_3D_REASON_RR_REQUIRES_NEXTGI_EPIC)
					? "RR_REQUIRES_NEXTGI_EPIC (requires NextGI HWRT at explicit Epic; quality=auto never enables RR)"
					: "RR_UNAVAILABLE";
			WARN_PRINT(vformat("DLSS Ray Reconstruction inactive (%s). Falling back to DLSS SR + built-in denoiser.", reason_text));
		}
	}

	return res;
}

void DLSSUpscaler::_flush_retired_features_locked(bool p_force_all) {
	if (retired_features.is_empty()) {
		return;
	}
	const uint64_t delay = (RD::get_singleton() != nullptr) ? uint64_t(RD::get_singleton()->get_frame_delay()) : 2u;
	for (int i = int(retired_features.size()) - 1; i >= 0; i--) {
		const RetiredFeature &rf = retired_features[i];
		if (p_force_all || frame_counter >= rf.retire_frame + delay) {
			DLSSBackend *owner = rf.owner_backend != nullptr ? rf.owner_backend : (initialized_backend != nullptr ? initialized_backend : backend);
			if (owner != nullptr && rf.handle != 0) {
				owner->release_feature(rf.handle);
			}
			retired_features.remove_at_unordered(i);
		}
	}
}

DLSSBackend::Status DLSSUpscaler::_ensure_backend_initialized_locked(const String &p_api_name, const String &p_project_id, uint64_t p_app_id, bool p_allow_ota) {
	DLSSBackend *target_backend = _resolve_backend_locked(p_api_name);
	if (target_backend == nullptr) {
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
		const DLSSBackend::Status avail = target_backend->query_availability(vk_provider->get_vendor_id(), vk_provider->are_extensions_satisfied());
		if (avail != DLSSBackend::OK) {
			backend_init_attempted = true;
			backend_init_status = avail;
			initialized_backend = target_backend;
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
	initialized_backend = target_backend;
	backend_init_status = target_backend->init(init_params);
	if (backend_init_status == DLSSBackend::OK && p_api_name == "D3D12") {
		const DLSSBackend::Status post_avail = target_backend->query_availability(0, true);
		if (post_avail != DLSSBackend::OK) {
			backend_init_status = post_avail;
		}
	}
	return backend_init_status;
}

Size2i DLSSUpscaler::get_render_size(const Size2i &p_target_size, float p_scale) const {
	const DLSSBackend::QualityMode quality = snap_scale_to_quality(p_scale);
	MutexLock lock(mutex);
	DLSSBackend *active_be = initialized_backend != nullptr ? initialized_backend : _resolve_backend_locked(dlss_current_api_name());
	if (active_be != nullptr && backend_init_attempted && backend_init_status == DLSSBackend::OK) {
		DLSSBackend::OptimalSettings opt;
		if (active_be->get_optimal_settings(p_target_size, quality, opt) == DLSSBackend::OK && opt.optimal_size.width > 0 && opt.optimal_size.height > 0) {
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
#ifdef VULKAN_ENABLED
	if (p_api_name == "Vulkan") {
		return true;
	}
#endif
#ifdef D3D12_ENABLED
	if (p_api_name == "D3D12") {
		return true;
	}
#endif
	return false;
}

uint32_t DLSSUpscaler::jitter_phase_count(const Size2i &p_target_size, const Size2i &p_render_size) const {
	const uint32_t base = nextgi_temporal_jitter_phase_count(p_target_size, p_render_size, true);
	MutexLock lock(mutex);
	bool any_rr = false;
	for (const KeyValue<const RenderSceneBuffersRD *, RRViewportState> &E : rr_states) {
		if (E.value.active) {
			any_rr = true;
			break;
		}
	}
	return nextgi_rr_jitter_phase_count(base, any_rr);
}

bool DLSSUpscaler::is_ray_reconstruction_active(const RenderSceneBuffersRD *p_rb) const {
	MutexLock lock(mutex);
	return _evaluate_rr_for_rb_locked(p_rb).active;
}

String DLSSUpscaler::get_rr_maturity_label() const {
	return rr_maturity_label_for_api(dlss_current_api_name());
}

RSE::ViewportScaling3DReason DLSSUpscaler::get_rr_reason(const RenderSceneBuffersRD *p_rb) const {
	MutexLock lock(mutex);
	return _evaluate_rr_for_rb_locked(p_rb).reason;
}

RSE::ViewportScaling3DReason DLSSUpscaler::get_availability(uint32_t p_view_count, float p_scale, float &r_effective_scale) const {
	(void)p_view_count;
	r_effective_scale = p_scale;
	const String project_id = dlss_read_setting<String>("rendering/scaling_3d/dlss/project_id", String());
	const int64_t app_id = dlss_read_setting<int64_t>("rendering/scaling_3d/dlss/application_id", 0);
	const bool allow_ota = dlss_read_setting<bool>("rendering/scaling_3d/dlss/allow_ota_updates", false);
	const String api_name = dlss_current_api_name();

	MutexLock lock(mutex);
	DLSSBackend *active_be = _resolve_backend_locked(api_name);
	if (active_be == nullptr) {
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
	if (api_name == "D3D12" && RD::get_singleton() != nullptr) {
		const String vendor_name = RD::get_singleton()->get_device_vendor_name().to_lower();
		if (!vendor_name.is_empty() && !vendor_name.contains("nvidia")) {
			return RSE::VIEWPORT_SCALING_3D_REASON_NOT_NVIDIA_RTX;
		}
	}

	const DLSSBackend::Status pre_avail = active_be->query_availability(vendor_id, exts_ok);
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
	(void)p_view;
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
	const int rr_preset_val = dlss_read_setting<int>("rendering/scaling_3d/dlss/rr_preset", 0);
	const String api_name = dlss_current_api_name();

	MutexLock lock(mutex);
	frame_counter++;
	_flush_retired_features_locked(false);

	const DLSSBackend::Status init_st = _ensure_backend_initialized_locked(api_name, project_id, uint64_t(MAX<int64_t>(0, app_id)), allow_ota);
	if (init_st != DLSSBackend::OK) {
		return init_st;
	}
	DLSSBackend *active_be = initialized_backend != nullptr ? initialized_backend : _resolve_backend_locked(api_name);
	if (active_be == nullptr) {
		return DLSSBackend::RUNTIME_MISSING;
	}

	const uint64_t cmd_handle = p_driver != nullptr ? p_driver->command_buffer_get_native_handle(p_cmd_buffer) : 0;
	const float ratio = p_args.output_size.width > 0 ? float(p_args.internal_size.width) / float(p_args.output_size.width) : 1.0f;
	const DLSSBackend::QualityMode quality = snap_scale_to_quality(ratio);
	const DLSSBackend::Preset preset = preset_from_setting(preset_val);
	const DLSSBackend::RRPreset rr_preset = rr_preset_from_setting(rr_preset_val);

	bool use_rr = p_args.ray_reconstruction;
	if (use_rr) {
		const RRViewportState *rr_st = rr_states.getptr(p_args.render_buffers);
		if (rr_st != nullptr && rr_st->failed) {
			use_rr = false;
		}
	}

#if defined(DLSS_BACKEND_NGX) && defined(D3D12_ENABLED)
	RDD::TextureID d3d_tex_ids[8];
	bool d3d_tex_rw[8];
	uint32_t d3d_tex_count = 0;
	const bool is_d3d12 = (api_name == "D3D12" && p_driver != nullptr && p_driver->get_capabilities().device_family == RDD::DEVICE_DIRECTX);
	if (is_d3d12) {
		auto add_d3d_tex = [&](const RDD::TextureNativeInfo &p_info, bool p_rw) {
			if (p_info.driver_id.id != 0 && d3d_tex_count < 8) {
				d3d_tex_ids[d3d_tex_count] = p_info.driver_id;
				d3d_tex_rw[d3d_tex_count] = p_rw;
				d3d_tex_count++;
			}
		};
		add_d3d_tex(p_args.color_info, false);
		add_d3d_tex(p_args.depth_info, false);
		add_d3d_tex(p_args.velocity_info, false);
		add_d3d_tex(p_args.output_info, true);
		if (use_rr) {
			add_d3d_tex(p_args.diffuse_albedo_info, false);
			add_d3d_tex(p_args.specular_albedo_info, false);
			add_d3d_tex(p_args.normal_roughness_info, false);
			if (p_args.has_specular_hit_distance) {
				add_d3d_tex(p_args.specular_hit_distance_info, false);
			}
		}
		dlss_ngx_d3d12_prepare_callback_textures(
				p_driver,
				p_cmd_buffer,
				VectorView<RDD::TextureID>(d3d_tex_ids, d3d_tex_count),
				VectorView<bool>(d3d_tex_rw, d3d_tex_count));
	}
#endif

	const FeatureKey key{ p_args.render_buffers, p_args.view };
	FeatureSlot *slot = features.getptr(key);
	const bool needs_create = slot == nullptr ||
			slot->handle == 0 ||
			slot->render_size != p_args.internal_size ||
			slot->output_size != p_args.output_size ||
			slot->quality_mode != quality ||
			slot->preset != preset ||
			slot->rr_preset != rr_preset ||
			slot->ray_reconstruction != use_rr;

	bool force_reset = p_args.reset;
	DLSSBackend::Status final_status = DLSSBackend::OK;
	if (needs_create) {
		if (slot != nullptr && slot->handle != 0) {
			retired_features.push_back({ slot->handle, frame_counter, active_be });
			slot->handle = 0;
		}
		DLSSBackend::FeatureCreateParams create_params;
		create_params.command_buffer_handle = cmd_handle;
		create_params.render_size = p_args.internal_size;
		create_params.output_size = p_args.output_size;
		create_params.quality_mode = quality;
		create_params.preset = preset;
		create_params.rr_preset = rr_preset;
		create_params.is_hdr = true;
		create_params.depth_inverted = true;
		create_params.mv_low_res = true;
		create_params.mv_jittered = false;
		create_params.auto_exposure = true;
		create_params.ray_reconstruction = use_rr;

		uint64_t new_handle = 0;
		DLSSBackend::Status create_st = active_be->create_feature(create_params, new_handle);
		if ((create_st != DLSSBackend::OK || new_handle == 0) && use_rr) {
			// RR creation failed: latch RR failure for this viewport, disable bypass, and fall back to SR.
			RRViewportState &rr_st = rr_states[p_args.render_buffers];
			rr_st.active = false;
			rr_st.failed = true;
			rr_st.reason = RSE::VIEWPORT_SCALING_3D_REASON_RR_UNAVAILABLE;
			if (p_args.render_buffers != nullptr) {
				DenoiserBypass::set_bypassed(p_args.render_buffers, false);
			}
			_sync_rr_guides_request_locked();
			const uint32_t bit = 1u << static_cast<uint32_t>(RSE::VIEWPORT_SCALING_3D_REASON_RR_UNAVAILABLE);
			if ((rr_logged_fallback_mask & bit) == 0) {
				rr_logged_fallback_mask |= bit;
				WARN_PRINT("DLSS Ray Reconstruction feature creation failed (RR_UNAVAILABLE). Falling back to DLSS SR + built-in denoiser.");
			}
			use_rr = false;
			create_params.ray_reconstruction = false;
			create_st = active_be->create_feature(create_params, new_handle);
		}
		if (create_st != DLSSBackend::OK || new_handle == 0) {
			final_status = create_st != DLSSBackend::OK ? create_st : DLSSBackend::FEATURE_CREATE_FAILED;
		} else {
			FeatureSlot updated;
			updated.handle = new_handle;
			updated.render_size = p_args.internal_size;
			updated.output_size = p_args.output_size;
			updated.quality_mode = quality;
			updated.preset = preset;
			updated.rr_preset = rr_preset;
			updated.ray_reconstruction = use_rr;
			features[key] = updated;
			slot = features.getptr(key);
			force_reset = true;

			if (!runtime_info_logged) {
				runtime_info_logged = true;
				const DLSSBackend::LoadedRuntimeInfo rt = active_be->get_loaded_runtime_info();
#ifdef VULKAN_ENABLED
				if (vk_provider != nullptr) {
					if (!rt.sha256.is_empty()) {
						vk_provider->set_loaded_dll_sha256(rt.sha256);
					}
					for (const String &extra_sha : rt.additional_sha256s) {
						vk_provider->add_loaded_dll_sha256(extra_sha);
					}
				}
#endif
				if (!rt.path.is_empty()) {
					print_verbose(vformat("DLSS runtime loaded: path=%s sha256=%s manifest_match=%s", rt.path, rt.sha256, rt.sha256_matches ? "true" : "false"));
				}
			}
		}
	}

	if (final_status == DLSSBackend::OK && slot != nullptr && slot->handle != 0) {
		DLSSBackend::EvaluateParams eval_params;
		eval_params.command_buffer_handle = cmd_handle;
		eval_params.feature_handle = slot->handle;
		eval_params.color = convert_native_texture(p_args.color_info, false);
		eval_params.depth = convert_native_texture(p_args.depth_info, false);
		eval_params.depth.aspect_mask = 0x2u; // VK_IMAGE_ASPECT_DEPTH_BIT.
		eval_params.velocity = convert_native_texture(p_args.velocity_info, false);
		eval_params.output = convert_native_texture(p_args.output_info, true);
		if (use_rr) {
			eval_params.diffuse_albedo = convert_native_texture(p_args.diffuse_albedo_info, false);
			eval_params.specular_albedo = convert_native_texture(p_args.specular_albedo_info, false);
			eval_params.normal_roughness = convert_native_texture(p_args.normal_roughness_info, false);
			if (p_args.has_specular_hit_distance) {
				eval_params.specular_hit_distance = convert_native_texture(p_args.specular_hit_distance_info, false);
			}
		}
		eval_params.render_size = p_args.internal_size;
		eval_params.output_size = p_args.output_size;
		eval_params.jitter_offset = p_args.jitter;
		eval_params.mv_scale = (p_args.motion_vector_scale.x != 0.0f || p_args.motion_vector_scale.y != 0.0f) ? p_args.motion_vector_scale : Vector2(float(p_args.internal_size.width), float(p_args.internal_size.height));
		eval_params.pre_exposure = 1.0f;
		eval_params.exposure_scale = 1.0f;
		eval_params.frame_time_delta_ms = p_args.delta_time_ms;
		eval_params.reset = force_reset;
		eval_params.ray_reconstruction = use_rr;
		eval_params.world_to_view = p_args.world_to_view;
		eval_params.view_to_clip = p_args.view_to_clip;

		final_status = active_be->evaluate(eval_params);
	}

#if defined(DLSS_BACKEND_NGX) && defined(D3D12_ENABLED)
	if (is_d3d12) {
		dlss_ngx_d3d12_finish_callback_textures(
				p_driver,
				p_cmd_buffer,
				VectorView<RDD::TextureID>(d3d_tex_ids, d3d_tex_count),
				VectorView<bool>(d3d_tex_rw, d3d_tex_count));
	}
#else
	if (p_driver != nullptr) {
		p_driver->command_buffer_invalidate_cached_state(p_cmd_buffer);
	}
#endif

	return final_status;
}

void DLSSUpscaler::process(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Inputs &p_inputs) {
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL(rd);
	ERR_FAIL_COND(p_render_buffers.is_null());

	CallbackArgs *args = callback_args_allocator.alloc();
	*args = CallbackArgs();
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

	bool use_rr = p_inputs.rr_active &&
			p_inputs.rr_diffuse_albedo.is_valid() &&
			p_inputs.rr_specular_albedo.is_valid() &&
			p_inputs.rr_normal_roughness.is_valid();
	if (use_rr) {
		const Error err_diff = rd->texture_get_native_info(p_inputs.rr_diffuse_albedo, args->diffuse_albedo_info);
		const Error err_spec = rd->texture_get_native_info(p_inputs.rr_specular_albedo, args->specular_albedo_info);
		const Error err_norm = rd->texture_get_native_info(p_inputs.rr_normal_roughness, args->normal_roughness_info);
		if (err_diff != OK || err_spec != OK || err_norm != OK) {
			use_rr = false;
		} else if (p_inputs.rr_specular_hit_distance.is_valid()) {
			if (rd->texture_get_native_info(p_inputs.rr_specular_hit_distance, args->specular_hit_distance_info) == OK) {
				args->has_specular_hit_distance = true;
			}
		}
	}

	args->internal_size = p_inputs.internal_size;
	args->output_size = p_inputs.output_size;
	args->motion_vector_scale = Vector2(float(p_inputs.motion_vector_scale.width), float(p_inputs.motion_vector_scale.height));
	args->jitter = p_inputs.jitter;
	args->delta_time_ms = p_inputs.delta_time * 1000.0f;
	args->reset = p_inputs.reset;
	args->ray_reconstruction = use_rr;
	args->world_to_view = p_inputs.world_to_view;
	args->view_to_clip = p_inputs.view_to_clip;

	RD::CallbackResource res[8];
	uint32_t res_count = 0;
	res[res_count++] = { p_inputs.color, RD::CALLBACK_RESOURCE_TYPE_TEXTURE, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE };
	res[res_count++] = { p_inputs.depth, RD::CALLBACK_RESOURCE_TYPE_TEXTURE, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE };
	res[res_count++] = { p_inputs.velocity, RD::CALLBACK_RESOURCE_TYPE_TEXTURE, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE };
	// NGX may clear and write the output texture inside Evaluate; GENERAL synchronizes both transfer/clear and storage writes.
	res[res_count++] = { p_inputs.output, RD::CALLBACK_RESOURCE_TYPE_TEXTURE, RD::CALLBACK_RESOURCE_USAGE_GENERAL };
	if (use_rr) {
		res[res_count++] = { p_inputs.rr_diffuse_albedo, RD::CALLBACK_RESOURCE_TYPE_TEXTURE, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE };
		res[res_count++] = { p_inputs.rr_specular_albedo, RD::CALLBACK_RESOURCE_TYPE_TEXTURE, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE };
		res[res_count++] = { p_inputs.rr_normal_roughness, RD::CALLBACK_RESOURCE_TYPE_TEXTURE, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE };
		if (args->has_specular_hit_distance) {
			res[res_count++] = { p_inputs.rr_specular_hit_distance, RD::CALLBACK_RESOURCE_TYPE_TEXTURE, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE };
		}
	}

	const Error err = rd->driver_callback_add(_driver_callback_run, args, VectorView<RD::CallbackResource>(res, res_count), RDD::PIPELINE_STAGE_ALL_COMMANDS_BIT);
	if (err != OK) {
		callback_args_allocator.free(args);
		latch_context_error(p_render_buffers.ptr(), RSE::VIEWPORT_SCALING_3D_REASON_FEATURE_CREATE_FAILED);
	}
}

void DLSSUpscaler::free_contexts(RenderSceneBuffersRD *p_render_buffers) {
	MutexLock lock(mutex);
	DLSSBackend *owner = initialized_backend != nullptr ? initialized_backend : backend;
	LocalVector<FeatureKey> to_remove;
	for (const KeyValue<FeatureKey, FeatureSlot> &E : features) {
		if (E.key.render_buffers == p_render_buffers) {
			if (E.value.handle != 0) {
				retired_features.push_back({ E.value.handle, frame_counter, owner });
			}
			to_remove.push_back(E.key);
		}
	}
	for (const FeatureKey &k : to_remove) {
		features.erase(k);
	}
	if (rr_states.erase(p_render_buffers)) {
		if (p_render_buffers != nullptr) {
			DenoiserBypass::set_bypassed(p_render_buffers, false);
		}
		_sync_rr_guides_request_locked();
	}
	clear_context_error(p_render_buffers);
}

void DLSSUpscaler::shutdown() {
	MutexLock lock(mutex);
	_flush_retired_features_locked(true);
	DLSSBackend *owner = initialized_backend != nullptr ? initialized_backend : backend;
	for (const KeyValue<FeatureKey, FeatureSlot> &E : features) {
		if (owner != nullptr && E.value.handle != 0) {
			owner->release_feature(E.value.handle);
		}
	}
	features.clear();
	for (const KeyValue<const RenderSceneBuffersRD *, RRViewportState> &E : rr_states) {
		if (E.key != nullptr) {
			DenoiserBypass::set_bypassed(E.key, false);
		}
	}
	rr_states.clear();
	RendererRD::RRGuides::set_requested(false);
	if (owner != nullptr && backend_init_attempted) {
		owner->shutdown();
	}
	initialized_backend = nullptr;
	backend_init_attempted = false;
	backend_init_status = DLSSBackend::OK;
	runtime_info_logged = false;
}

DLSSBackend::Status DLSSUpscaler::test_ensure_feature_for_view(RenderSceneBuffersRD *p_render_buffers, uint32_t p_view, const Size2i &p_render_size, const Size2i &p_output_size, float p_scale, int p_preset, uint64_t &r_handle, bool p_ray_reconstruction, int p_rr_preset) {
	r_handle = 0;
	MutexLock lock(mutex);
	DLSSBackend *active_be = initialized_backend != nullptr ? initialized_backend : backend;
	if (active_be == nullptr) {
		return DLSSBackend::RUNTIME_MISSING;
	}
	const DLSSBackend::QualityMode quality = snap_scale_to_quality(p_scale);
	const DLSSBackend::Preset preset = preset_from_setting(p_preset);
	const DLSSBackend::RRPreset rr_preset = rr_preset_from_setting(p_rr_preset);
	const FeatureKey key{ p_render_buffers, p_view };
	FeatureSlot *slot = features.getptr(key);
	if (slot != nullptr && slot->handle != 0 && slot->render_size == p_render_size && slot->output_size == p_output_size && slot->quality_mode == quality && slot->preset == preset && slot->rr_preset == rr_preset && slot->ray_reconstruction == p_ray_reconstruction) {
		r_handle = slot->handle;
		return DLSSBackend::OK;
	}
	if (slot != nullptr && slot->handle != 0) {
		retired_features.push_back({ slot->handle, frame_counter, active_be });
		slot->handle = 0;
	}
	DLSSBackend::FeatureCreateParams params;
	params.render_size = p_render_size;
	params.output_size = p_output_size;
	params.quality_mode = quality;
	params.preset = preset;
	params.rr_preset = rr_preset;
	params.is_hdr = true;
	params.depth_inverted = true;
	params.mv_low_res = true;
	params.mv_jittered = false;
	params.auto_exposure = true;
	params.ray_reconstruction = p_ray_reconstruction;
	uint64_t handle = 0;
	const DLSSBackend::Status st = active_be->create_feature(params, handle);
	if (st != DLSSBackend::OK || handle == 0) {
		const DLSSBackend::Status err_st = st != DLSSBackend::OK ? st : DLSSBackend::FEATURE_CREATE_FAILED;
		latch_context_error(p_render_buffers, map_status_to_reason(err_st));
		return err_st;
	}
	features[key] = { handle, p_render_size, p_output_size, quality, preset, rr_preset, p_ray_reconstruction };
	r_handle = handle;
	return DLSSBackend::OK;
}

void DLSSUpscaler::test_advance_frame(uint32_t p_frame_delay) {
	MutexLock lock(mutex);
	frame_counter += p_frame_delay;
	_flush_retired_features_locked(false);
}
