/**************************************************************************/
/*  dlss_frame_generation.h                                               */
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

// GodotNext: pu_dlss_fg (#200), pu_dlss_mfg (#201).
// Pure MIT FrameGenerationExtension implementation for DLSS-G (FG 2x, MFG 3x-6x, Dynamic MFG).
// Contains no Streamline SDK or D3D12 headers and no proprietary runtime filenames.

#include "core/math/projection.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/os/mutex.h"
#include "core/templates/vector.h"
#include "servers/rendering/storage/frame_generation_extension.h"

#include <cstdint>

class DLSSFrameGenBackendOps {
public:
	enum BufferTagKind : uint32_t {
		TAG_DEPTH = 0,
		TAG_MOTION_VECTORS = 1,
		TAG_HUDLESS_COLOR = 2,
		TAG_UI_COLOR_AND_ALPHA = 23,
	};

	enum OptionsMode : uint32_t {
		MODE_OFF = 0,
		MODE_ON = 1,
		MODE_DYNAMIC = 2,
	};

	struct ConstantsSnapshot {
		Projection view_to_clip;
		Projection clip_to_view;
		Projection clip_to_prev_clip;
		Projection prev_clip_to_clip;
		Vector2 jitter_offset;
		Vector2 mv_scale;
		Vector3 camera_pos;
		Vector3 camera_up;
		Vector3 camera_right;
		Vector3 camera_fwd;
		float z_near = 0.0f;
		float z_far = 0.0f;
		float fovy_rad = 0.0f;
		float aspect = 1.0f;
		bool depth_inverted = true;
		bool camera_motion_included = true;
		bool motion_vectors_3d = false;
		bool reset = false;
	};

	struct TagSnapshot {
		BufferTagKind kind = TAG_DEPTH;
		uint64_t resource_handle = 0;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t d3d12_state = 0;
		bool valid_until_present = true;
	};

	struct OptionsSnapshot {
		OptionsMode mode = MODE_OFF;
		uint32_t num_frames_to_generate = 1;
		bool retain_resources_when_off = true;
		bool dynamic_resolution_enabled = false;
		uint32_t dynamic_res_width = 0;
		uint32_t dynamic_res_height = 0;
		uint32_t color_width = 0;
		uint32_t color_height = 0;
		uint32_t mvec_depth_width = 0;
		uint32_t mvec_depth_height = 0;
		bool enable_ui_recomposition = true;
		float dynamic_target_fps = 0.0f;

		bool operator==(const OptionsSnapshot &p_other) const {
			return mode == p_other.mode &&
					num_frames_to_generate == p_other.num_frames_to_generate &&
					retain_resources_when_off == p_other.retain_resources_when_off &&
					dynamic_resolution_enabled == p_other.dynamic_resolution_enabled &&
					dynamic_res_width == p_other.dynamic_res_width &&
					dynamic_res_height == p_other.dynamic_res_height &&
					color_width == p_other.color_width &&
					color_height == p_other.color_height &&
					mvec_depth_width == p_other.mvec_depth_width &&
					mvec_depth_height == p_other.mvec_depth_height &&
					enable_ui_recomposition == p_other.enable_ui_recomposition &&
					dynamic_target_fps == p_other.dynamic_target_fps;
		}
		bool operator!=(const OptionsSnapshot &p_other) const { return !(*this == p_other); }
	};

	struct StateSnapshot {
		bool ok = false;
		uint32_t status_code = 0;
		String status_name;
		uint32_t num_frames_presented = 0;
		uint32_t num_frames_to_generate_max = 1;
		bool dynamic_mfg_supported = false;
		uint64_t estimated_vram_bytes = 0;
	};

	virtual ~DLSSFrameGenBackendOps() = default;

	virtual String get_api_name() const = 0;
	virtual String get_vendor_name() const = 0;
	virtual bool is_streamline_loaded() const = 0;
	virtual bool is_streamline_init_failed() const = 0;
	virtual bool is_composition_swapchain() const = 0;
	virtual bool is_scene_tree_paused() const = 0;
	virtual bool is_dynamic_resolution_enabled() const = 0;
	virtual bool query_feature_supported(String &r_result_name) const = 0;
	virtual bool set_constants(uint64_t p_frame_index, const ConstantsSnapshot &p_consts) = 0;
	virtual bool set_tags(uint64_t p_frame_index, const Vector<TagSnapshot> &p_tags, uint64_t p_cmd_list_handle) = 0;
	virtual bool set_options(const OptionsSnapshot &p_opts) = 0;
	virtual bool get_state(const OptionsSnapshot *p_opts, StateSnapshot &r_state) = 0;
	virtual void notify_fg_active(bool p_active, bool p_dynamic) = 0;
};

class DLSSFrameGeneration : public FrameGenerationExtension {
	GDCLASS(DLSSFrameGeneration, FrameGenerationExtension);

	mutable Mutex mutex;
	DLSSFrameGenBackendOps *ops = nullptr;

	int requested_multiplier = 2;
	bool requested_dynamic = false;
	int requested_dynamic_target_fps = 0;

	bool has_sent_options = false;
	bool options_dirty = true;
	DLSSFrameGenBackendOps::OptionsSnapshot last_sent_options;
	DLSSFrameGenBackendOps::StateSnapshot cached_state;
	bool state_queried = false;

	bool skip_next_present = true;
	bool runtime_disabled = false;
	Size2i last_render_size;
	Size2i last_output_size;
	uint64_t last_swap_chain = 0;

	mutable uint32_t logged_reason_mask = 0;
	bool runtime_error_logged = false;
	uint32_t fg_unsupported_log_count = 0;
	uint32_t mfg_fallback_log_count = 0;
	uint32_t dynamic_fallback_log_count = 0;

	void _log_availability_once_locked(FrameGenerationReason p_reason, const String &p_detail) const;
	void _refresh_capabilities_locked() const;

public:
	static constexpr const char *PROVIDER_NAME = "DLSS-G";

	explicit DLSSFrameGeneration(DLSSFrameGenBackendOps *p_ops = nullptr);
	~DLSSFrameGeneration() override = default;

	void set_backend_ops(DLSSFrameGenBackendOps *p_ops);
	DLSSFrameGenBackendOps *get_backend_ops() const { return ops; }

	// Diagnostic counters for doctests.
	uint32_t get_mfg_fallback_log_count() const { return mfg_fallback_log_count; }
	uint32_t get_dynamic_fallback_log_count() const { return dynamic_fallback_log_count; }
	bool is_runtime_disabled() const { return runtime_disabled; }

	// FrameGenerationExtension overrides.
	String get_provider_name() const override { return PROVIDER_NAME; }
	FrameGenerationReason is_available() const override;
	void set_mode(const Dictionary &p_mode) override;
	void on_frame_inputs(const Ref<FrameGenerationInputs> &p_inputs) override;
	void on_present(Object *p_inputs, bool p_before) override;
	void reset() override;
	Dictionary get_provider_capabilities() const override;
};
