/**************************************************************************/
/*  dlss_frame_generation.cpp                                             */
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

#include "dlss_frame_generation.h"

#include "core/math/math_funcs.h"
#include "core/string/print_string.h"
#include "servers/rendering/rendering_device.h"

namespace {

constexpr uint32_t D3D12_STATE_ALL_SHADER_RESOURCE = 0x40u | 0x80u;

struct DLSSFGTagCallbackArgs {
	DLSSFrameGenBackendOps *ops = nullptr;
	uint64_t frame_index = 0;
	Vector<DLSSFrameGenBackendOps::TagSnapshot> tags;
};

void _dlss_fg_tag_callback(RenderingDeviceDriver *p_driver, RDD::CommandBufferID p_cmd_buffer, void *p_userdata) {
	DLSSFGTagCallbackArgs *args = static_cast<DLSSFGTagCallbackArgs *>(p_userdata);
	if (args == nullptr) {
		return;
	}
	if (args->ops != nullptr && !args->tags.is_empty()) {
		// All DLSS-G tags use eValidUntilPresent and are already transitioned to SHADER_RESOURCE by
		// RenderingDeviceGraph via CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE; pass 0 for cmd_list so
		// Streamline records no commands onto Godot's command list during tagging.
		(void)p_driver;
		(void)p_cmd_buffer;
		args->ops->set_tags(args->frame_index, args->tags, 0);
	}
	memdelete(args);
}

} // namespace

DLSSFrameGeneration::DLSSFrameGeneration(DLSSFrameGenBackendOps *p_ops) :
		ops(p_ops) {
}

void DLSSFrameGeneration::set_backend_ops(DLSSFrameGenBackendOps *p_ops) {
	MutexLock lock(mutex);
	ops = p_ops;
	has_sent_options = false;
	options_dirty = true;
	state_queried = false;
	cached_state = DLSSFrameGenBackendOps::StateSnapshot();
	skip_next_present = true;
	runtime_disabled = false;
	logged_reason_mask = 0;
	runtime_error_logged = false;
	fg_unsupported_log_count = 0;
	mfg_fallback_log_count = 0;
	dynamic_fallback_log_count = 0;
}

void DLSSFrameGeneration::_log_availability_once_locked(FrameGenerationReason p_reason, const String &p_detail) const {
	const uint32_t bit = 1u << static_cast<uint32_t>(p_reason);
	if ((logged_reason_mask & bit) != 0) {
		return;
	}
	logged_reason_mask |= bit;
	if (p_detail.is_empty()) {
		WARN_PRINT(vformat("DLSS-G: unavailable (%s).", get_reason_token(p_reason)));
	} else {
		WARN_PRINT(vformat("DLSS-G: unavailable (%s): %s", get_reason_token(p_reason), p_detail));
	}
}

void DLSSFrameGeneration::_refresh_capabilities_locked() const {
	if (ops == nullptr || !ops->is_streamline_loaded()) {
		return;
	}
	DLSSFrameGenBackendOps::StateSnapshot st;
	if (ops->get_state(nullptr, st) && st.ok) {
		const_cast<DLSSFrameGeneration *>(this)->cached_state = st;
		const_cast<DLSSFrameGeneration *>(this)->state_queried = true;
	}
}

FrameGenerationExtension::FrameGenerationReason DLSSFrameGeneration::is_available() const {
	MutexLock lock(mutex);
	if (ops == nullptr) {
		_log_availability_once_locked(REASON_PROVIDER_UNAVAILABLE, "no Streamline backend ops configured");
		return REASON_PROVIDER_UNAVAILABLE;
	}

	// #200 step 2 precedence: first match wins, 1 log line each.
	const String api = ops->get_api_name();
	if (api != "D3D12") {
		_log_availability_once_locked(REASON_API_UNSUPPORTED, vformat("rendering API '%s' is not D3D12", api));
		return REASON_API_UNSUPPORTED;
	}

	const String vendor = ops->get_vendor_name().to_lower();
	if (!vendor.is_empty() && !vendor.contains("nvidia")) {
		_log_availability_once_locked(REASON_NOT_NVIDIA_RTX, vformat("adapter vendor '%s' is not NVIDIA", ops->get_vendor_name()));
		return REASON_NOT_NVIDIA_RTX;
	}

	if (ops->is_streamline_init_failed()) {
		_log_availability_once_locked(REASON_STREAMLINE_UNAVAILABLE, "Streamline initialization failed");
		return REASON_STREAMLINE_UNAVAILABLE;
	}

	if (!ops->is_streamline_loaded() || ops->is_composition_swapchain() || runtime_disabled) {
		_log_availability_once_locked(REASON_PROVIDER_UNAVAILABLE, ops->is_composition_swapchain() ? "per-pixel transparency composition swap chain is unsupported" : "Streamline is not loaded");
		return REASON_PROVIDER_UNAVAILABLE;
	}

	String sl_result_name;
	if (!ops->query_feature_supported(sl_result_name)) {
		const_cast<DLSSFrameGeneration *>(this)->fg_unsupported_log_count++;
		_log_availability_once_locked(REASON_FG_GPU_UNSUPPORTED, vformat("Streamline feature query returned %s", sl_result_name.is_empty() ? String("eErrorFeatureNotSupported") : sl_result_name));
		return REASON_FG_GPU_UNSUPPORTED;
	}

	_refresh_capabilities_locked();
	const uint32_t max_gen = MAX(1u, cached_state.num_frames_to_generate_max);
	const int max_mult = int(max_gen) + 1;

	// #201 steps 1-2: MFG (3x-6x) and Dynamic MFG capability gating against runtime state (never hard-coded GPU list).
	if (requested_dynamic) {
		if (!cached_state.dynamic_mfg_supported) {
			if (max_gen <= 1u) {
				const_cast<DLSSFrameGeneration *>(this)->mfg_fallback_log_count++;
				return REASON_MFG_REQUIRES_RTX50;
			}
			const_cast<DLSSFrameGeneration *>(this)->dynamic_fallback_log_count++;
			return REASON_DYNAMIC_MFG_UNSUPPORTED;
		}
		return REASON_OK;
	}

	if (requested_multiplier > max_mult) {
		const_cast<DLSSFrameGeneration *>(this)->mfg_fallback_log_count++;
		return REASON_MFG_REQUIRES_RTX50;
	}

	return REASON_OK;
}

void DLSSFrameGeneration::set_mode(const Dictionary &p_mode) {
	MutexLock lock(mutex);
	const int new_mult = FrameGenerationRegistry::clamp_multiplier(int(p_mode.get("multiplier", 2)));
	const bool new_dyn = bool(p_mode.get("dynamic", false));
	const int new_target_fps = MAX(0, int(p_mode.get("dynamic_target_fps", 0)));
	if (new_mult != requested_multiplier || new_dyn != requested_dynamic || new_target_fps != requested_dynamic_target_fps) {
		requested_multiplier = new_mult;
		requested_dynamic = new_dyn;
		requested_dynamic_target_fps = new_target_fps;
		options_dirty = true;
	}
}

void DLSSFrameGeneration::reset() {
	MutexLock lock(mutex);
	skip_next_present = true;
	options_dirty = true;
}

void DLSSFrameGeneration::on_frame_inputs(const Ref<FrameGenerationInputs> &p_inputs) {
	if (p_inputs.is_null()) {
		return;
	}
	MutexLock lock(mutex);
	if (ops == nullptr || !ops->is_streamline_loaded() || runtime_disabled) {
		return;
	}

	const Size2i render_size = p_inputs->get_render_size();
	const Size2i output_size = p_inputs->get_output_size();
	const uint64_t swap_chain = p_inputs->get_swap_chain();
	const bool resized = (last_output_size != Size2i() && output_size != last_output_size) ||
			(last_swap_chain != 0 && swap_chain != 0 && swap_chain != last_swap_chain);
	if (resized || p_inputs->get_reset()) {
		skip_next_present = true;
	}
	last_render_size = render_size;
	last_output_size = output_size;
	if (swap_chain != 0) {
		last_swap_chain = swap_chain;
	}

	// #200 step 3: common constants.
	DLSSFrameGenBackendOps::ConstantsSnapshot consts;
	consts.view_to_clip = p_inputs->get_view_to_clip();
	consts.clip_to_view = consts.view_to_clip.inverse();
	consts.clip_to_prev_clip = p_inputs->get_clip_to_prev_clip();
	consts.prev_clip_to_clip = consts.clip_to_prev_clip.inverse();
	consts.jitter_offset = p_inputs->get_jitter();
	const Vector2 mvs = p_inputs->get_motion_vector_scale();
	consts.mv_scale = (mvs.x != 0.0f || mvs.y != 0.0f) ? mvs : Vector2(float(MAX(1, render_size.width)), float(MAX(1, render_size.height)));

	const Transform3D cam = p_inputs->get_camera_transform();
	consts.camera_pos = cam.origin;
	consts.camera_right = cam.basis.get_column(0).normalized();
	consts.camera_up = cam.basis.get_column(1).normalized();
	consts.camera_fwd = (-cam.basis.get_column(2)).normalized();
	consts.z_near = p_inputs->get_z_near();
	consts.z_far = p_inputs->get_z_far();
	consts.fovy_rad = float(Math::deg_to_rad(double(p_inputs->get_fovy())));
	consts.aspect = p_inputs->get_aspect() > 0.0f ? p_inputs->get_aspect() : (output_size.height > 0 ? float(output_size.width) / float(output_size.height) : 1.0f);
	consts.depth_inverted = true;
	consts.camera_motion_included = true;
	consts.motion_vectors_3d = false;
	consts.reset = p_inputs->get_reset();

	const uint64_t frame_index = p_inputs->get_frame_index();
	ops->set_constants(frame_index, consts);

	// #200 step 4: tag depth, motion vectors, HUD-less color, and UI color+alpha (eValidUntilPresent).
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr) {
		DLSSFGTagCallbackArgs *cb_args = memnew(DLSSFGTagCallbackArgs);
		cb_args->ops = ops;
		cb_args->frame_index = frame_index;

		RD::CallbackResource cb_res[4];
		uint32_t cb_res_count = 0;

		auto append_rd_tag = [&](RID p_rid, DLSSFrameGenBackendOps::BufferTagKind p_kind, const Size2i &p_extent) {
			if (!p_rid.is_valid()) {
				return;
			}
			RDD::TextureNativeInfo info;
			if (rd->texture_get_native_info(p_rid, info) != OK || info.image == 0) {
				return;
			}
			DLSSFrameGenBackendOps::TagSnapshot tag;
			tag.kind = p_kind;
			tag.resource_handle = info.image;
			tag.width = uint32_t(MAX(1, p_extent.width > 0 ? p_extent.width : int(info.width)));
			tag.height = uint32_t(MAX(1, p_extent.height > 0 ? p_extent.height : int(info.height)));
			tag.d3d12_state = D3D12_STATE_ALL_SHADER_RESOURCE;
			tag.valid_until_present = true;
			cb_args->tags.push_back(tag);
			if (cb_res_count < 4) {
				cb_res[cb_res_count++] = { p_rid, RD::CALLBACK_RESOURCE_TYPE_TEXTURE, RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE };
			}
		};

		append_rd_tag(p_inputs->get_depth(), DLSSFrameGenBackendOps::TAG_DEPTH, render_size);
		append_rd_tag(p_inputs->get_velocity(), DLSSFrameGenBackendOps::TAG_MOTION_VECTORS, render_size);
		append_rd_tag(p_inputs->get_hudless_color(), DLSSFrameGenBackendOps::TAG_HUDLESS_COLOR, output_size);
		append_rd_tag(p_inputs->get_ui_color(), DLSSFrameGenBackendOps::TAG_UI_COLOR_AND_ALPHA, output_size);

		if (cb_res_count > 0) {
			if (rd->driver_callback_add(_dlss_fg_tag_callback, cb_args, VectorView<RD::CallbackResource>(cb_res, cb_res_count), RDD::PIPELINE_STAGE_ALL_COMMANDS_BIT) != OK) {
				memdelete(cb_args);
			}
		} else {
			memdelete(cb_args);
		}
	} else {
		// Headless/mock test path without a live RenderingDevice: pass RID numeric IDs as synthetic handles.
		Vector<DLSSFrameGenBackendOps::TagSnapshot> tags;
		auto append_mock_tag = [&](RID p_rid, DLSSFrameGenBackendOps::BufferTagKind p_kind, const Size2i &p_extent) {
			if (!p_rid.is_valid()) {
				return;
			}
			DLSSFrameGenBackendOps::TagSnapshot tag;
			tag.kind = p_kind;
			tag.resource_handle = p_rid.get_id();
			tag.width = uint32_t(MAX(1, p_extent.width));
			tag.height = uint32_t(MAX(1, p_extent.height));
			tag.d3d12_state = D3D12_STATE_ALL_SHADER_RESOURCE;
			tag.valid_until_present = true;
			tags.push_back(tag);
		};
		append_mock_tag(p_inputs->get_depth(), DLSSFrameGenBackendOps::TAG_DEPTH, render_size);
		append_mock_tag(p_inputs->get_velocity(), DLSSFrameGenBackendOps::TAG_MOTION_VECTORS, render_size);
		append_mock_tag(p_inputs->get_hudless_color(), DLSSFrameGenBackendOps::TAG_HUDLESS_COLOR, output_size);
		append_mock_tag(p_inputs->get_ui_color(), DLSSFrameGenBackendOps::TAG_UI_COLOR_AND_ALPHA, output_size);
		if (!tags.is_empty()) {
			ops->set_tags(frame_index, tags, 0);
		}
	}
}

void DLSSFrameGeneration::on_present(Object *p_inputs, bool p_before) {
	MutexLock lock(mutex);
	if (ops == nullptr || !ops->is_streamline_loaded()) {
		return;
	}

	const FrameGenerationInputs *inputs = Object::cast_to<FrameGenerationInputs>(p_inputs);

	if (p_before) {
		if (!state_queried) {
			_refresh_capabilities_locked();
		}
		const bool paused = ops->is_scene_tree_paused();
		const bool force_off = runtime_disabled || skip_next_present || paused;
		skip_next_present = false;

		const uint32_t max_gen = MAX(1u, cached_state.num_frames_to_generate_max);

		DLSSFrameGenBackendOps::OptionsSnapshot opts;
		opts.retain_resources_when_off = true;
		opts.enable_ui_recomposition = true;

		if (force_off) {
			opts.mode = DLSSFrameGenBackendOps::MODE_OFF;
			opts.num_frames_to_generate = 1;
		} else if (requested_dynamic && cached_state.dynamic_mfg_supported) {
			opts.mode = DLSSFrameGenBackendOps::MODE_DYNAMIC;
			opts.num_frames_to_generate = 1;
			opts.dynamic_target_fps = float(MAX(0, requested_dynamic_target_fps));
		} else {
			opts.mode = DLSSFrameGenBackendOps::MODE_ON;
			const uint32_t desired_gen = uint32_t(MAX(1, requested_multiplier - 1));
			opts.num_frames_to_generate = CLAMP(desired_gen, 1u, max_gen);
		}

		const Size2i out_sz = (inputs != nullptr && inputs->get_output_size().width > 0 && inputs->get_output_size().height > 0) ? inputs->get_output_size() : last_output_size;
		const Size2i ren_sz = (inputs != nullptr && inputs->get_render_size().width > 0 && inputs->get_render_size().height > 0) ? inputs->get_render_size() : last_render_size;
		if (out_sz.width > 0 && out_sz.height > 0) {
			opts.color_width = uint32_t(out_sz.width);
			opts.color_height = uint32_t(out_sz.height);
		}
		if (ren_sz.width > 0 && ren_sz.height > 0) {
			opts.mvec_depth_width = uint32_t(ren_sz.width);
			opts.mvec_depth_height = uint32_t(ren_sz.height);
		}

		if (ops->is_dynamic_resolution_enabled() && inputs != nullptr) {
			const Size2i rs = inputs->get_render_size();
			if (rs.width > 0 && rs.height > 0) {
				opts.dynamic_resolution_enabled = true;
				opts.dynamic_res_width = uint32_t(rs.width);
				opts.dynamic_res_height = uint32_t(rs.height);
			}
		}

		if (!has_sent_options || options_dirty || opts != last_sent_options) {
			ops->set_options(opts);
			last_sent_options = opts;
			has_sent_options = true;
			options_dirty = false;
		}
		ops->notify_fg_active(opts.mode != DLSSFrameGenBackendOps::MODE_OFF, opts.mode == DLSSFrameGenBackendOps::MODE_DYNAMIC);
	} else {
		DLSSFrameGenBackendOps::StateSnapshot st;
		if (ops->get_state(has_sent_options ? &last_sent_options : nullptr, st) && st.ok) {
			cached_state = st;
			state_queried = true;
			if (st.status_code != 0) {
				runtime_disabled = true;
				if (!runtime_error_logged) {
					runtime_error_logged = true;
					WARN_PRINT(vformat("DLSS-G: disabling frame generation due to runtime status %s (0x%x).",
							st.status_name.is_empty() ? String("error") : st.status_name,
							st.status_code));
				}
				DLSSFrameGenBackendOps::OptionsSnapshot off_opts = last_sent_options;
				off_opts.mode = DLSSFrameGenBackendOps::MODE_OFF;
				ops->set_options(off_opts);
				last_sent_options = off_opts;
				has_sent_options = true;
				ops->notify_fg_active(false, false);
			}
		}
	}
}

Dictionary DLSSFrameGeneration::get_provider_capabilities() const {
	MutexLock lock(mutex);
	if (!state_queried) {
		_refresh_capabilities_locked();
	}
	Dictionary caps;
	const uint32_t max_gen = MAX(1u, cached_state.num_frames_to_generate_max);
	caps["max_multiplier"] = int(max_gen) + 1;
	caps["dynamic_supported"] = cached_state.dynamic_mfg_supported;
	caps["num_frames_presented"] = int64_t(cached_state.num_frames_presented);
	caps["vram_bytes"] = int64_t(cached_state.estimated_vram_bytes);
	return caps;
}
