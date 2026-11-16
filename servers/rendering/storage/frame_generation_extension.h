/**************************************************************************/
/*  frame_generation_extension.h                                          */
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

// GodotNext: pu_fg_present_path (#203). The vendor-neutral frame-generation seam (plan §21.5 U2, U5):
// a provider (DLSS-G in #200; FSR FG or XeSS-FG later, from their vendors) receives the HUD-less 3D
// colour, the UI layer, depth, velocity and the camera of the main window, and the D3D12 present.

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/object/gdvirtual.gen.h"
#include "core/object/ref_counted.h"
#include "core/string/ustring.h"
#include "core/templates/rid.h"
#include "core/variant/dictionary.h"
#include "servers/display/display_server_enums.h"

class FrameGenerationInputs : public RefCounted {
	GDCLASS(FrameGenerationInputs, RefCounted);

	RID hudless_color;
	RID ui_color;
	RID depth;
	RID velocity;
	Vector2 motion_vector_scale;
	Size2i render_size;
	Size2i output_size;
	Projection view_to_clip;
	Projection clip_to_prev_clip;
	Transform3D camera_transform;
	float z_near = 0.0f;
	float z_far = 0.0f;
	float fovy = 0.0f;
	float aspect = 0.0f;
	Vector2 jitter;
	bool reset = false;
	uint64_t frame_index = 0;
	uint64_t swap_chain = 0;

protected:
	static void _bind_methods();

public:
	RID get_hudless_color() const { return hudless_color; }
	void set_hudless_color(RID p_texture) { hudless_color = p_texture; }

	RID get_ui_color() const { return ui_color; }
	void set_ui_color(RID p_texture) { ui_color = p_texture; }

	RID get_depth() const { return depth; }
	void set_depth(RID p_texture) { depth = p_texture; }

	RID get_velocity() const { return velocity; }
	void set_velocity(RID p_texture) { velocity = p_texture; }

	Vector2 get_motion_vector_scale() const { return motion_vector_scale; }
	void set_motion_vector_scale(const Vector2 &p_scale) { motion_vector_scale = p_scale; }

	Size2i get_render_size() const { return render_size; }
	void set_render_size(const Size2i &p_size) { render_size = p_size; }

	Size2i get_output_size() const { return output_size; }
	void set_output_size(const Size2i &p_size) { output_size = p_size; }

	Projection get_view_to_clip() const { return view_to_clip; }
	void set_view_to_clip(const Projection &p_projection) { view_to_clip = p_projection; }

	Projection get_clip_to_prev_clip() const { return clip_to_prev_clip; }
	void set_clip_to_prev_clip(const Projection &p_projection) { clip_to_prev_clip = p_projection; }

	Transform3D get_camera_transform() const { return camera_transform; }
	void set_camera_transform(const Transform3D &p_transform) { camera_transform = p_transform; }

	float get_z_near() const { return z_near; }
	void set_z_near(float p_z_near) { z_near = p_z_near; }

	float get_z_far() const { return z_far; }
	void set_z_far(float p_z_far) { z_far = p_z_far; }

	float get_fovy() const { return fovy; }
	void set_fovy(float p_fovy) { fovy = p_fovy; }

	float get_aspect() const { return aspect; }
	void set_aspect(float p_aspect) { aspect = p_aspect; }

	Vector2 get_jitter() const { return jitter; }
	void set_jitter(const Vector2 &p_jitter) { jitter = p_jitter; }

	bool get_reset() const { return reset; }
	void set_reset(bool p_reset) { reset = p_reset; }

	uint64_t get_frame_index() const { return frame_index; }
	void set_frame_index(uint64_t p_frame_index) { frame_index = p_frame_index; }

	uint64_t get_swap_chain() const { return swap_chain; }
	void set_swap_chain(uint64_t p_swap_chain) { swap_chain = p_swap_chain; }

	// The previous frame's clip space from this frame's, for a camera that moved from
	// (p_prev_projection, p_prev_transform) to (p_projection, p_transform).
	static Projection compute_clip_to_prev_clip(const Projection &p_projection, const Transform3D &p_transform, const Projection &p_prev_projection, const Transform3D &p_prev_transform);
};

class FrameGenerationExtension : public RefCounted {
	GDCLASS(FrameGenerationExtension, RefCounted);

public:
	// Append-only: the values are part of the status and the GDExtension API.
	enum FrameGenerationReason {
		REASON_OK,
		REASON_API_UNSUPPORTED,
		REASON_FG_GPU_UNSUPPORTED,
		REASON_MFG_REQUIRES_RTX50,
		REASON_NOT_NVIDIA_RTX,
		REASON_NOT_MAIN_WINDOW,
		REASON_PROVIDER_ALREADY_ACTIVE,
		REASON_PROVIDER_UNAVAILABLE,
		REASON_MAX,
	};

protected:
	static void _bind_methods();

	GDVIRTUAL0RC(String, _get_name)
	GDVIRTUAL0RC(int, _is_available)
	GDVIRTUAL1(_set_mode, Dictionary)
	GDVIRTUAL1(_on_frame_inputs, Ref<FrameGenerationInputs>)
	GDVIRTUAL2(_on_present, Object *, bool)
	GDVIRTUAL0(_reset)

public:
	String get_provider_name() const;
	// REASON_PROVIDER_UNAVAILABLE without an override, or for a value outside the enum.
	FrameGenerationReason is_available() const;
	void set_mode(const Dictionary &p_mode);
	void on_frame_inputs(const Ref<FrameGenerationInputs> &p_inputs);
	void on_present(Object *p_inputs, bool p_before);
	void reset();

	// The reason's name without the REASON_ prefix, as the log lines print it.
	static String get_reason_token(FrameGenerationReason p_reason);
};

VARIANT_ENUM_CAST(FrameGenerationExtension::FrameGenerationReason);

// One frame-generation provider per process (§21.5 U2): the first provider that activates keeps the
// process; switching providers needs a restart. Everything here runs on the rendering server's
// thread, between frames or inside the frame being drawn.
class FrameGenerationRegistry {
public:
	// What decides the status, besides the registered providers.
	struct Context {
		String provider;
		int multiplier = 2;
		bool dynamic = false;
		bool editor = false;
		bool xr_main_window = false;
		bool rendering_device = true;

		bool operator==(const Context &p_other) const;
		bool operator!=(const Context &p_other) const { return !(*this == p_other); }
	};

	static constexpr int MULTIPLIER_MIN = 2;
	static constexpr int MULTIPLIER_MAX = 6;
	static int clamp_multiplier(int p_multiplier);

	// The settings (re-read only when ProjectSettings changes), the editor hint and whether a
	// RenderingDevice renders. The caller sets xr_main_window.
	static Context read_context();

	// Registering a different object under the active provider's name fails with
	// PROVIDER_ALREADY_ACTIVE and one error. Registering the same object again re-reads its
	// availability on the next update().
	static void register_extension(const Ref<FrameGenerationExtension> &p_extension);
	static void unregister_extension(ObjectID p_extension_id);

	// Resolves the status for p_context. It calls the provider only when the context or the
	// registrations changed, and logs one line for each change to a fallback reason.
	static void update(const Context &p_context);

	static Dictionary get_status();
	static FrameGenerationExtension::FrameGenerationReason get_reason();
	static bool is_active();
	static int get_active_multiplier();
	static Ref<FrameGenerationExtension> get_active_extension();

	// FG runs on the main window of a game only: never the editor, a secondary window, XR or stereo.
	static bool is_viewport_eligible(DisplayServerEnums::WindowID p_screen, bool p_use_xr, uint32_t p_view_count, bool p_editor);

	// The inputs object the split fills each frame; the same object is passed to _on_present.
	static Ref<FrameGenerationInputs> get_inputs();
	// Empties the inputs' texture RIDs when the split that filled them (its HUD-less colour is
	// p_hudless_color) frees its textures; another viewport's inputs stay.
	static void forget_split_textures(RID p_hudless_color);
	// Calls _reset() first when the inputs carry a reset, then _on_frame_inputs(). The first inputs
	// after a provider object becomes active always carry a reset.
	static void dispatch_frame_inputs();
	// Calls the active provider's _on_present(inputs, p_before) for one present of the main window.
	static void dispatch_present(uint64_t p_swap_chain, uint64_t p_frame_index, bool p_before);

	static bool has_present_hook();
	// D3D12 only: routes D3D12Hooks' present callback to dispatch_present() when the project asks
	// for a provider. Call it before the rendering context initializes.
	static void install_present_hook();

	// Drops the providers, the status and the process lock (exit, and between tests).
	static void clear();
};
