/**************************************************************************/
/*  dlss_upscaler.h                                                       */
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

// GodotNext: pu_dlss_sr_effect (#100), pu_dlss_d3d12 (#119), pu_dlss_rr (#206), pu_rr_d3d12 (#204).
// TemporalUpscaler implementation for modules/dlss (SR/DLAA + Ray Reconstruction on Vulkan & D3D12).

#include "dlss_backend.h"

#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/paged_allocator.h"
#include "servers/rendering/renderer_rd/effects/temporal_upscaler.h"
#include "servers/rendering/rendering_device.h"

#ifdef VULKAN_ENABLED
class DLSSVulkanExtensionProvider;
#endif

class DLSSUpscaler : public TemporalUpscaler {
public:
	struct FeatureKey {
		const RenderSceneBuffersRD *render_buffers = nullptr;
		uint32_t view = 0;

		bool operator==(const FeatureKey &p_other) const {
			return render_buffers == p_other.render_buffers && view == p_other.view;
		}
	};

	struct FeatureKeyHasher {
		static _FORCE_INLINE_ uint32_t hash(const FeatureKey &p_key) {
			uint64_t h = reinterpret_cast<uintptr_t>(p_key.render_buffers);
			h ^= (uint64_t(p_key.view) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
			return uint32_t(h ^ (h >> 32));
		}
	};

	struct FeatureSlot {
		uint64_t handle = 0;
		Size2i render_size;
		Size2i output_size;
		DLSSBackend::QualityMode quality_mode = DLSSBackend::QUALITY_MAX_QUALITY;
		DLSSBackend::Preset preset = DLSSBackend::PRESET_DEFAULT;
		DLSSBackend::RRPreset rr_preset = DLSSBackend::RR_PRESET_DEFAULT;
		bool ray_reconstruction = false;
	};

	struct RetiredFeature {
		uint64_t handle = 0;
		uint64_t retire_frame = 0;
		DLSSBackend *owner_backend = nullptr;
	};

	struct RRViewportState {
		bool active = false;
		RSE::ViewportScaling3DReason reason = RSE::VIEWPORT_SCALING_3D_REASON_NONE;
		bool failed = false;
	};

	struct CallbackArgs {
		DLSSUpscaler *upscaler = nullptr;
		RenderSceneBuffersRD *render_buffers = nullptr;
		uint32_t view = 0;
		RDD::TextureNativeInfo color_info;
		RDD::TextureNativeInfo depth_info;
		RDD::TextureNativeInfo velocity_info;
		RDD::TextureNativeInfo output_info;
		RDD::TextureNativeInfo diffuse_albedo_info;
		RDD::TextureNativeInfo specular_albedo_info;
		RDD::TextureNativeInfo normal_roughness_info;
		RDD::TextureNativeInfo specular_hit_distance_info;
		bool has_specular_hit_distance = false;
		Size2i internal_size;
		Size2i output_size;
		Vector2 motion_vector_scale;
		Vector2 jitter;
		float delta_time_ms = 0.0f;
		bool reset = false;
		bool ray_reconstruction = false;
		Transform3D world_to_view;
		Projection view_to_clip;
	};

private:
	mutable Mutex mutex;
	DLSSBackend *backend = nullptr;
	DLSSBackend *d3d12_backend = nullptr;
	DLSSBackend *initialized_backend = nullptr;
#ifdef VULKAN_ENABLED
	DLSSVulkanExtensionProvider *vk_provider = nullptr;
#endif
	bool backend_init_attempted = false;
	DLSSBackend::Status backend_init_status = DLSSBackend::OK;
	bool runtime_info_logged = false;

	HashMap<FeatureKey, FeatureSlot, FeatureKeyHasher> features;
	LocalVector<RetiredFeature> retired_features;
	mutable HashMap<const RenderSceneBuffersRD *, RRViewportState> rr_states;
	mutable bool rr_vulkan_beta_logged = false;
	mutable uint32_t rr_logged_fallback_mask = 0;

	uint64_t frame_counter = 0;
	PagedAllocator<CallbackArgs, true> callback_args_allocator;

	static uint64_t _memory_report_callback(void *p_userdata);
	static void _driver_callback_run(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_cmd_buffer, void *p_userdata);

	DLSSBackend *_resolve_backend_locked(const String &p_api_name) const;
	void _sync_rr_guides_request_locked() const;
	RRActivationResult _evaluate_rr_for_rb_locked(const RenderSceneBuffersRD *p_rb) const;
	void _flush_retired_features_locked(bool p_force_all);
	DLSSBackend::Status _ensure_backend_initialized_locked(const String &p_api_name, const String &p_project_id, uint64_t p_app_id, bool p_allow_ota);
	DLSSBackend::Status _execute_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_cmd_buffer, const CallbackArgs &p_args);

public:
	explicit DLSSUpscaler(DLSSBackend *p_backend, DLSSBackend *p_d3d12_backend = nullptr);
	~DLSSUpscaler() override;

#ifdef VULKAN_ENABLED
	void set_vulkan_extension_provider(DLSSVulkanExtensionProvider *p_provider);
#endif
	void set_backend(DLSSBackend *p_backend);
	void set_d3d12_backend(DLSSBackend *p_d3d12_backend);
	DLSSBackend *get_backend() const { return backend; }
	DLSSBackend *get_d3d12_backend() const { return d3d12_backend; }

	// Pure helpers shared with doctests.
	static DLSSBackend *select_backend_for_api(const String &p_api_name, DLSSBackend *p_primary_backend, DLSSBackend *p_d3d12_backend);
	static String rr_maturity_label_for_api(const String &p_api_name);
	static DLSSBackend::QualityMode snap_scale_to_quality(float p_scale);
	static DLSSBackend::Preset preset_from_setting(int p_setting_value);
	static DLSSBackend::RRPreset rr_preset_from_setting(int p_setting_value);
	static RSE::ViewportScaling3DReason map_status_to_reason(DLSSBackend::Status p_status);
	static DLSSBackend::TextureResource convert_native_texture(const RDD::TextureNativeInfo &p_info, bool p_read_write);

	// Test/diagnostic inspection.
	uint32_t get_active_feature_count() const;
	uint32_t get_retired_feature_count() const;
	DLSSBackend::LoadedRuntimeInfo get_loaded_runtime_info() const;
	// Pure CPU-side multi-viewport feature lifecycle entry point for mock doctests.
	DLSSBackend::Status test_ensure_feature_for_view(RenderSceneBuffersRD *p_render_buffers, uint32_t p_view, const Size2i &p_render_size, const Size2i &p_output_size, float p_scale, int p_preset, uint64_t &r_handle, bool p_ray_reconstruction = false, int p_rr_preset = 0);
	void test_advance_frame(uint32_t p_frame_delay);

	// TemporalUpscaler overrides.
	String get_display_name() const override { return "NVIDIA DLSS (Temporal - NVIDIA RTX only)"; }
	const Span<char> get_label() const override { return Span<char>("DLSS", 4); }
	Size2i get_render_size(const Size2i &p_target_size, float p_scale) const override;
	Vector2 get_scale_range() const override { return Vector2(0.30f, 1.0f); }
	VelocityMode velocity_mode() const override { return VELOCITY_DENSE; }
	bool supports_api(const String &p_api_name) const override;
	bool supports_multiview() const override { return false; }
	float mip_bias_offset() const override { return -1.0f; }
	uint32_t jitter_phase_count(const Size2i &p_target_size, const Size2i &p_render_size) const override;
	bool is_ray_reconstruction_active(const RenderSceneBuffersRD *p_rb) const override;
	String get_rr_maturity_label() const override;
	RSE::ViewportScaling3DReason get_rr_reason(const RenderSceneBuffersRD *p_rb) const override;
	RSE::ViewportScaling3DReason ensure_context(RenderSceneBuffersRD *p_render_buffers, uint32_t p_view) override;
	void process(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Inputs &p_inputs) override;
	void free_contexts(RenderSceneBuffersRD *p_render_buffers) override;
	void shutdown() override;
	RSE::ViewportScaling3DReason get_availability(uint32_t p_view_count, float p_scale, float &r_effective_scale) const override;
};
