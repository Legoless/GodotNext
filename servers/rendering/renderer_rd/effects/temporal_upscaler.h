/**************************************************************************/
/*  temporal_upscaler.h                                                   */
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

#include "core/math/math_funcs.h"
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2i.h"
#include "core/object/ref_counted.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "servers/rendering/rendering_server_enums.h"
#include "servers/rendering/storage/temporal_upscaler_extension.h"

class RenderSceneBuffersRD;

inline float nextgi_temporal_mip_bias(float p_scale, float p_user_bias, float p_offset) {
	return Math::log2(MIN(p_scale, 1.0f)) + p_user_bias + p_offset;
}

inline uint32_t nextgi_temporal_jitter_phase_count(const Size2i &p_target_size, const Size2i &p_render_size, bool p_round) {
	const float phases = 8.0f * Math::pow(float(p_target_size.width) / p_render_size.width, 2.0f);
	return uint32_t(p_round ? Math::floor(phases + 0.5f) : phases);
}

class TemporalHistoryReset {
	bool requested = false;

public:
	void request() { requested = true; }
	bool consume() {
		const bool result = requested;
		requested = false;
		return result;
	}
};

class TemporalUpscaler {
public:
	enum VelocityMode {
		VELOCITY_DENSE,
	};

	struct Inputs {
		RID color;
		RID depth;
		RID velocity;
		RID reactive;
		RID output;
		Size2i internal_size;
		Size2i max_internal_size;
		Size2i output_size;
		Size2i motion_vector_scale;
		Vector2 jitter;
		Transform3D world_to_view;
		Projection view_to_clip;
		float z_near = 0.0f;
		float z_far = 0.0f;
		float fovy = 0.0f;
		float delta_time = 0.0f;
		bool reset = false;
	};

private:
	HashMap<const RenderSceneBuffersRD *, RSE::ViewportScaling3DReason> context_errors;

public:
	virtual ~TemporalUpscaler() = default;

	virtual String get_display_name() const = 0;
	virtual const Span<char> get_label() const = 0;
	virtual Size2i get_render_size(const Size2i &p_target_size, float p_scale) const {
		return Size2i(MAX(p_target_size.width * p_scale, 1.0f), MAX(p_target_size.height * p_scale, 1.0f));
	}
	virtual Vector2 get_scale_range() const { return Vector2(0.30f, 1.0f); }
	virtual VelocityMode velocity_mode() const { return VELOCITY_DENSE; }
	virtual bool supports_api(const String &p_api_name) const { return p_api_name == "Vulkan"; }
	virtual bool supports_multiview() const { return false; }
	virtual float mip_bias_offset() const { return -0.5f; }
	virtual uint32_t jitter_phase_count(const Size2i &p_target_size, const Size2i &p_render_size) const {
		return nextgi_temporal_jitter_phase_count(p_target_size, p_render_size, true);
	}
	virtual RSE::ViewportScaling3DReason ensure_context(RenderSceneBuffersRD *p_render_buffers, uint32_t p_view) = 0;
	virtual void process(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Inputs &p_inputs) = 0;
	virtual void free_contexts(RenderSceneBuffersRD *p_render_buffers) = 0;
	virtual void shutdown() {}

	void latch_context_error(RenderSceneBuffersRD *p_render_buffers, RSE::ViewportScaling3DReason p_reason) {
		if (p_reason != RSE::VIEWPORT_SCALING_3D_REASON_NONE) {
			context_errors[p_render_buffers] = p_reason;
		}
	}
	RSE::ViewportScaling3DReason get_context_error(const RenderSceneBuffersRD *p_render_buffers) const {
		const RSE::ViewportScaling3DReason *reason = context_errors.getptr(p_render_buffers);
		return reason != nullptr ? *reason : RSE::VIEWPORT_SCALING_3D_REASON_NONE;
	}
	void clear_context_error(RenderSceneBuffersRD *p_render_buffers) { context_errors.erase(p_render_buffers); }

	// NONE when usable. Otherwise the reason the mode cannot run on this
	// device/view; r_effective_scale may be clamped by the implementation.
	virtual RSE::ViewportScaling3DReason get_availability(uint32_t p_view_count, float p_scale, float &r_effective_scale) const {
		r_effective_scale = p_scale;
		return RSE::VIEWPORT_SCALING_3D_REASON_NONE;
	}
};

// GodotNext: pu_dlss_scaling_mode (#103). The §3.2 precedence in one pure
// helper so the doctests and the viewport resolver agree.
inline RSE::ViewportScaling3DReason nextgi_resolve_dlss_reason(bool p_api_supported, bool p_module_registered, RSE::ViewportScaling3DReason p_module_reason, float p_scale, uint32_t p_view_count, int p_width, int p_height, bool p_supports_multiview = false) {
	if (!p_api_supported) {
		return RSE::VIEWPORT_SCALING_3D_REASON_API_UNSUPPORTED;
	}
	if (!p_module_registered) {
		return RSE::VIEWPORT_SCALING_3D_REASON_MODULE_DISABLED;
	}
	if (p_module_reason != RSE::VIEWPORT_SCALING_3D_REASON_NONE) {
		return p_module_reason;
	}
	if (p_scale < 0.30f || p_scale > 1.0f || p_width < 32 || p_height < 32) {
		return RSE::VIEWPORT_SCALING_3D_REASON_SCALE_UNSUPPORTED;
	}
	if (!p_supports_multiview && p_view_count > 1) {
		return RSE::VIEWPORT_SCALING_3D_REASON_VIEW_UNSUPPORTED;
	}
	return RSE::VIEWPORT_SCALING_3D_REASON_NONE;
}

// GodotNext: pt_temporal_upscaler_extension (#192). SCALING_3D_MODE_CUSTOM
// selection (GT-1) in one pure helper, so the doctests and the viewport
// resolver agree: an available extension keeps CUSTOM and clamps the scale
// to its range. Otherwise the viewport falls back to TSR, with
// CUSTOM_UPSCALER_UNAVAILABLE, or VIEW_UNSUPPORTED for a stereo viewport the
// extension cannot run.
struct CustomUpscalerResolution {
	RSE::ViewportScaling3DMode mode = RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM;
	RSE::ViewportScaling3DReason reason = RSE::VIEWPORT_SCALING_3D_REASON_NONE;
	float scale = 1.0f;
	bool scale_clamped = false;
};

inline CustomUpscalerResolution nextgi_resolve_custom_upscaler(bool p_available, uint32_t p_view_count, bool p_supports_multiview, RSE::ViewportScaling3DReason p_context_error, const Vector2 &p_scale_range, float p_scale) {
	CustomUpscalerResolution result;
	result.scale = p_scale;
	if (!p_available || p_context_error != RSE::VIEWPORT_SCALING_3D_REASON_NONE) {
		result.mode = RSE::VIEWPORT_SCALING_3D_MODE_TSR;
		result.reason = RSE::VIEWPORT_SCALING_3D_REASON_CUSTOM_UPSCALER_UNAVAILABLE;
		return result;
	}
	if (p_view_count > 1 && !p_supports_multiview) {
		result.mode = RSE::VIEWPORT_SCALING_3D_MODE_TSR;
		result.reason = RSE::VIEWPORT_SCALING_3D_REASON_VIEW_UNSUPPORTED;
		return result;
	}
	const float clamped = CLAMP(p_scale, p_scale_range.x, p_scale_range.y);
	result.scale_clamped = clamped != p_scale;
	result.scale = clamped;
	return result;
}

#ifdef RD_ENABLED
class TemporalUpscalerExtensionAdapter : public TemporalUpscaler {
	Ref<TemporalUpscalerExtension> extension;
	String display_name;
	CharString label_utf8;
	HashMap<RenderSceneBuffersRD *, uint32_t> contexts;

public:
	explicit TemporalUpscalerExtensionAdapter(const Ref<TemporalUpscalerExtension> &p_extension);
	~TemporalUpscalerExtensionAdapter() override { shutdown(); }

	const Ref<TemporalUpscalerExtension> &get_extension() const { return extension; }

	String get_display_name() const override { return display_name; }
	const Span<char> get_label() const override { return label_utf8.span(); }
	Vector2 get_scale_range() const override;
	bool supports_api(const String &p_api_name) const override;
	bool supports_multiview() const override { return false; }
	float mip_bias_offset() const override;
	uint32_t jitter_phase_count(const Size2i &p_target_size, const Size2i &p_render_size) const override;
	RSE::ViewportScaling3DReason ensure_context(RenderSceneBuffersRD *p_render_buffers, uint32_t p_view) override;
	void process(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Inputs &p_inputs) override;
	void free_contexts(RenderSceneBuffersRD *p_render_buffers) override;
	void shutdown() override;
};
#endif

class TemporalUpscalerRegistry {
public:
	static void register_upscaler(RSE::ViewportScaling3DMode p_mode, TemporalUpscaler *p_upscaler);
	static void unregister_upscaler(RSE::ViewportScaling3DMode p_mode);
	static TemporalUpscaler *get(RSE::ViewportScaling3DMode p_mode);
	static void clear();
	static void free_contexts(RenderSceneBuffersRD *p_render_buffers);
	static void shutdown();
	static void initialize_dev_copy_upscaler();

	// GodotNext: pt_temporal_upscaler_extension (#192).
	static void register_extension(const Ref<TemporalUpscalerExtension> &p_extension);
	static void unregister_extension(const Ref<TemporalUpscalerExtension> &p_extension);
	// By identity, so a queued command does not have to own a reference.
	static void unregister_extension(ObjectID p_extension_id);
	static Ref<TemporalUpscalerExtension> get_selected_extension();
	// The published CUSTOM adapter and its id. Ids are never reused, so a
	// cached id cannot match a different adapter at a recycled address.
	static TemporalUpscaler *get_custom_upscaler(uint64_t *r_id);
	static uint64_t get_custom_upscaler_id();
	static bool is_custom_upscaler_available();
	static void sync_custom_upscaler();
	static void flush_retired_extensions();
	static void clear_extensions();
	static uint64_t get_custom_upscaler_revision();
	static uint32_t get_retired_extension_count();
	// One log line per fallback kind until the latches reset.
	static void report_custom_unavailable();
	static void report_custom_scale_clamped(const String &p_name, const Vector2 &p_scale_range);
	static void reset_custom_warning_latches();
	// Frees this render buffers' contexts and context errors on every
	// registered and retired extension adapter.
	static void free_extension_contexts(RenderSceneBuffersRD *p_render_buffers);

	// Builds the ProjectSettings enum hint. DLSS is appended only while a
	// DLSS upscaler is registered, so a default build's settings are unchanged.
	static String build_mode_hint(bool p_metal);

private:
	static TemporalUpscaler *create_extension_adapter(const Ref<TemporalUpscalerExtension> &p_extension);
	static void destroy_extension_adapter(TemporalUpscaler *p_adapter);

	static TemporalUpscaler *upscalers[RSE::VIEWPORT_SCALING_3D_MODE_MAX];
};
