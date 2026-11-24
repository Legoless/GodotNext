/**************************************************************************/
/*  frame_generation_extension.cpp                                        */
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

#include "frame_generation_extension.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "servers/display/display_server.h"
#include "servers/rendering/rendering_device.h"

#ifdef D3D12_ENABLED
#include "core/object/callable_mp.h"
#include "drivers/d3d12/d3d12_hooks.h"

#include <dxgi.h>
#endif

// FrameGenerationInputs

void FrameGenerationInputs::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_hudless_color"), &FrameGenerationInputs::get_hudless_color);
	ClassDB::bind_method(D_METHOD("set_hudless_color", "texture"), &FrameGenerationInputs::set_hudless_color);
	ADD_PROPERTY(PropertyInfo(Variant::RID, "hudless_color"), "set_hudless_color", "get_hudless_color");

	ClassDB::bind_method(D_METHOD("get_ui_color"), &FrameGenerationInputs::get_ui_color);
	ClassDB::bind_method(D_METHOD("set_ui_color", "texture"), &FrameGenerationInputs::set_ui_color);
	ADD_PROPERTY(PropertyInfo(Variant::RID, "ui_color"), "set_ui_color", "get_ui_color");

	ClassDB::bind_method(D_METHOD("get_depth"), &FrameGenerationInputs::get_depth);
	ClassDB::bind_method(D_METHOD("set_depth", "texture"), &FrameGenerationInputs::set_depth);
	ADD_PROPERTY(PropertyInfo(Variant::RID, "depth"), "set_depth", "get_depth");

	ClassDB::bind_method(D_METHOD("get_velocity"), &FrameGenerationInputs::get_velocity);
	ClassDB::bind_method(D_METHOD("set_velocity", "texture"), &FrameGenerationInputs::set_velocity);
	ADD_PROPERTY(PropertyInfo(Variant::RID, "velocity"), "set_velocity", "get_velocity");

	ClassDB::bind_method(D_METHOD("get_motion_vector_scale"), &FrameGenerationInputs::get_motion_vector_scale);
	ClassDB::bind_method(D_METHOD("set_motion_vector_scale", "scale"), &FrameGenerationInputs::set_motion_vector_scale);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "motion_vector_scale"), "set_motion_vector_scale", "get_motion_vector_scale");

	ClassDB::bind_method(D_METHOD("get_render_size"), &FrameGenerationInputs::get_render_size);
	ClassDB::bind_method(D_METHOD("set_render_size", "size"), &FrameGenerationInputs::set_render_size);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "render_size"), "set_render_size", "get_render_size");

	ClassDB::bind_method(D_METHOD("get_output_size"), &FrameGenerationInputs::get_output_size);
	ClassDB::bind_method(D_METHOD("set_output_size", "size"), &FrameGenerationInputs::set_output_size);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "output_size"), "set_output_size", "get_output_size");

	ClassDB::bind_method(D_METHOD("get_view_to_clip"), &FrameGenerationInputs::get_view_to_clip);
	ClassDB::bind_method(D_METHOD("set_view_to_clip", "projection"), &FrameGenerationInputs::set_view_to_clip);
	ADD_PROPERTY(PropertyInfo(Variant::PROJECTION, "view_to_clip"), "set_view_to_clip", "get_view_to_clip");

	ClassDB::bind_method(D_METHOD("get_clip_to_prev_clip"), &FrameGenerationInputs::get_clip_to_prev_clip);
	ClassDB::bind_method(D_METHOD("set_clip_to_prev_clip", "projection"), &FrameGenerationInputs::set_clip_to_prev_clip);
	ADD_PROPERTY(PropertyInfo(Variant::PROJECTION, "clip_to_prev_clip"), "set_clip_to_prev_clip", "get_clip_to_prev_clip");

	ClassDB::bind_method(D_METHOD("get_camera_transform"), &FrameGenerationInputs::get_camera_transform);
	ClassDB::bind_method(D_METHOD("set_camera_transform", "transform"), &FrameGenerationInputs::set_camera_transform);
	ADD_PROPERTY(PropertyInfo(Variant::TRANSFORM3D, "camera_transform"), "set_camera_transform", "get_camera_transform");

	ClassDB::bind_method(D_METHOD("get_z_near"), &FrameGenerationInputs::get_z_near);
	ClassDB::bind_method(D_METHOD("set_z_near", "z_near"), &FrameGenerationInputs::set_z_near);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "z_near"), "set_z_near", "get_z_near");

	ClassDB::bind_method(D_METHOD("get_z_far"), &FrameGenerationInputs::get_z_far);
	ClassDB::bind_method(D_METHOD("set_z_far", "z_far"), &FrameGenerationInputs::set_z_far);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "z_far"), "set_z_far", "get_z_far");

	ClassDB::bind_method(D_METHOD("get_fovy"), &FrameGenerationInputs::get_fovy);
	ClassDB::bind_method(D_METHOD("set_fovy", "fovy"), &FrameGenerationInputs::set_fovy);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fovy"), "set_fovy", "get_fovy");

	ClassDB::bind_method(D_METHOD("get_aspect"), &FrameGenerationInputs::get_aspect);
	ClassDB::bind_method(D_METHOD("set_aspect", "aspect"), &FrameGenerationInputs::set_aspect);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "aspect"), "set_aspect", "get_aspect");

	ClassDB::bind_method(D_METHOD("get_jitter"), &FrameGenerationInputs::get_jitter);
	ClassDB::bind_method(D_METHOD("set_jitter", "jitter"), &FrameGenerationInputs::set_jitter);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "jitter"), "set_jitter", "get_jitter");

	ClassDB::bind_method(D_METHOD("get_reset"), &FrameGenerationInputs::get_reset);
	ClassDB::bind_method(D_METHOD("set_reset", "reset"), &FrameGenerationInputs::set_reset);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "reset"), "set_reset", "get_reset");

	ClassDB::bind_method(D_METHOD("get_frame_index"), &FrameGenerationInputs::get_frame_index);
	ClassDB::bind_method(D_METHOD("set_frame_index", "frame_index"), &FrameGenerationInputs::set_frame_index);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "frame_index"), "set_frame_index", "get_frame_index");

	ClassDB::bind_method(D_METHOD("get_swap_chain"), &FrameGenerationInputs::get_swap_chain);
	ClassDB::bind_method(D_METHOD("set_swap_chain", "swap_chain"), &FrameGenerationInputs::set_swap_chain);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "swap_chain"), "set_swap_chain", "get_swap_chain");
}

Projection FrameGenerationInputs::compute_clip_to_prev_clip(const Projection &p_projection, const Transform3D &p_transform, const Projection &p_prev_projection, const Transform3D &p_prev_transform) {
	// clip -> view -> world (this frame), then world -> view -> clip (previous frame).
	return p_prev_projection * Projection(p_prev_transform.affine_inverse() * p_transform) * p_projection.inverse();
}

// FrameGenerationExtension

void FrameGenerationExtension::_bind_methods() {
	GDVIRTUAL_BIND(_get_name);
	GDVIRTUAL_BIND(_is_available);
	GDVIRTUAL_BIND(_set_mode, "mode");
	GDVIRTUAL_BIND(_on_frame_inputs, "inputs");
	GDVIRTUAL_BIND(_on_present, "inputs", "before");
	GDVIRTUAL_BIND(_reset);

	BIND_ENUM_CONSTANT(REASON_OK);
	BIND_ENUM_CONSTANT(REASON_API_UNSUPPORTED);
	BIND_ENUM_CONSTANT(REASON_FG_GPU_UNSUPPORTED);
	BIND_ENUM_CONSTANT(REASON_MFG_REQUIRES_RTX50);
	BIND_ENUM_CONSTANT(REASON_NOT_NVIDIA_RTX);
	BIND_ENUM_CONSTANT(REASON_NOT_MAIN_WINDOW);
	BIND_ENUM_CONSTANT(REASON_PROVIDER_ALREADY_ACTIVE);
	BIND_ENUM_CONSTANT(REASON_PROVIDER_UNAVAILABLE);
	// GodotNext: pu_streamline_reflex (#209) / pu_dlss_mfg (#201).
	BIND_ENUM_CONSTANT(REASON_STREAMLINE_UNAVAILABLE);
	BIND_ENUM_CONSTANT(REASON_DYNAMIC_MFG_UNSUPPORTED);
	BIND_ENUM_CONSTANT(REASON_MAX);
}

String FrameGenerationExtension::get_provider_name() const {
	String ret;
	GDVIRTUAL_CALL(_get_name, ret);
	return ret;
}

FrameGenerationExtension::FrameGenerationReason FrameGenerationExtension::is_available() const {
	int ret = REASON_PROVIDER_UNAVAILABLE;
	GDVIRTUAL_CALL(_is_available, ret);
	if (ret < 0 || ret >= REASON_MAX) {
		return REASON_PROVIDER_UNAVAILABLE;
	}
	return FrameGenerationReason(ret);
}

void FrameGenerationExtension::set_mode(const Dictionary &p_mode) {
	GDVIRTUAL_CALL(_set_mode, p_mode);
}

void FrameGenerationExtension::on_frame_inputs(const Ref<FrameGenerationInputs> &p_inputs) {
	GDVIRTUAL_CALL(_on_frame_inputs, p_inputs);
}

void FrameGenerationExtension::on_present(Object *p_inputs, bool p_before) {
	GDVIRTUAL_CALL(_on_present, p_inputs, p_before);
}

void FrameGenerationExtension::reset() {
	GDVIRTUAL_CALL(_reset);
}

String FrameGenerationExtension::get_reason_token(FrameGenerationReason p_reason) {
	switch (p_reason) {
		case REASON_OK:
			return "OK";
		case REASON_API_UNSUPPORTED:
			return "API_UNSUPPORTED";
		case REASON_FG_GPU_UNSUPPORTED:
			return "FG_GPU_UNSUPPORTED";
		case REASON_MFG_REQUIRES_RTX50:
			return "MFG_REQUIRES_RTX50";
		case REASON_NOT_NVIDIA_RTX:
			return "NOT_NVIDIA_RTX";
		case REASON_NOT_MAIN_WINDOW:
			return "NOT_MAIN_WINDOW";
		case REASON_PROVIDER_ALREADY_ACTIVE:
			return "PROVIDER_ALREADY_ACTIVE";
		case REASON_PROVIDER_UNAVAILABLE:
			return "PROVIDER_UNAVAILABLE";
		case REASON_STREAMLINE_UNAVAILABLE:
			return "STREAMLINE_UNAVAILABLE";
		case REASON_DYNAMIC_MFG_UNSUPPORTED:
			return "DYNAMIC_MFG_UNSUPPORTED";
		case REASON_MAX:
			break;
	}
	return "UNKNOWN";
}

// FrameGenerationRegistry

using FGReason = FrameGenerationExtension::FrameGenerationReason;

namespace {

const char *FG_PROVIDER_SETTING = "rendering/frame_generation/provider";
const char *FG_MULTIPLIER_SETTING = "rendering/frame_generation/multiplier";
const char *FG_DYNAMIC_SETTING = "rendering/frame_generation/dynamic";
const char *FG_DYNAMIC_TARGET_FPS_SETTING = "rendering/frame_generation/dynamic_target_fps";

Mutex fg_mutex;
HashMap<String, Ref<FrameGenerationExtension>> fg_providers;
uint64_t fg_revision = 1;

// The settings part of read_context(), re-read when ProjectSettings changes.
uint32_t fg_settings_version = 0;
String fg_settings_provider;
int fg_settings_multiplier = 2;
bool fg_settings_dynamic = false;
int fg_settings_dynamic_target_fps = 0;

// The resolved state.
bool fg_resolved = false;
FrameGenerationRegistry::Context fg_context;
uint64_t fg_resolved_revision = 0;
Ref<FrameGenerationExtension> fg_active;
String fg_active_name;
// The first provider that activated in this process; only it may activate again.
String fg_locked_name;
FGReason fg_reason = FrameGenerationExtension::REASON_OK;
int fg_multiplier = 1;
// What _set_mode last received, and from which provider (plain values: no Variant at file scope).
ObjectID fg_mode_target;
int fg_mode_multiplier = 0;
bool fg_mode_dynamic = false;
int fg_mode_dynamic_target_fps = -1;
// The last fallback that was logged, so each change logs one line.
String fg_logged_provider;
FGReason fg_logged_reason = FrameGenerationExtension::REASON_OK;
Ref<FrameGenerationInputs> fg_inputs;
bool fg_present_hook = false;
// A provider object became active: its first frame inputs carry a reset.
bool fg_activation_reset = false;

template <typename T>
T _fg_setting(const char *p_name, const T &p_default) {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (settings == nullptr || !settings->has_setting(p_name)) {
		return p_default;
	}
	return settings->get_setting_with_override(p_name);
}

void _fg_log(const String &p_provider, FGReason p_reason, int p_requested_multiplier, const String &p_locked_name) {
	const String token = FrameGenerationExtension::get_reason_token(p_reason);
	switch (p_reason) {
		case FrameGenerationExtension::REASON_PROVIDER_ALREADY_ACTIVE:
			ERR_PRINT(vformat("Frame generation: provider \"%s\" was not activated (%s): provider \"%s\" already activated in this process; switching providers needs a restart.", p_provider, token, p_locked_name));
			break;
		case FrameGenerationExtension::REASON_MFG_REQUIRES_RTX50:
			WARN_PRINT(vformat("Frame generation: provider \"%s\" runs at 2x instead of %dx (%s).", p_provider, p_requested_multiplier, token));
			break;
		case FrameGenerationExtension::REASON_DYNAMIC_MFG_UNSUPPORTED:
			WARN_PRINT(vformat("Frame generation: provider \"%s\" runs at fixed %dx instead of dynamic (%s).", p_provider, p_requested_multiplier, token));
			break;
		case FrameGenerationExtension::REASON_NOT_MAIN_WINDOW:
			WARN_PRINT(vformat("Frame generation: provider \"%s\" is off (%s): frame generation runs only in the main window of a running game, never in the editor or XR.", p_provider, token));
			break;
		case FrameGenerationExtension::REASON_PROVIDER_UNAVAILABLE:
			WARN_PRINT(vformat("Frame generation: provider \"%s\" is off (%s): it is not registered, or it reports that it cannot run.", p_provider, token));
			break;
		default:
			WARN_PRINT(vformat("Frame generation: provider \"%s\" is off (%s).", p_provider, token));
			break;
	}
}

} // namespace

bool FrameGenerationRegistry::Context::operator==(const Context &p_other) const {
	return provider == p_other.provider && multiplier == p_other.multiplier && dynamic == p_other.dynamic && dynamic_target_fps == p_other.dynamic_target_fps && editor == p_other.editor && xr_main_window == p_other.xr_main_window && rendering_device == p_other.rendering_device;
}

int FrameGenerationRegistry::clamp_multiplier(int p_multiplier) {
	return CLAMP(p_multiplier, MULTIPLIER_MIN, MULTIPLIER_MAX);
}

FrameGenerationRegistry::Context FrameGenerationRegistry::read_context() {
	Context context;
	ProjectSettings *settings = ProjectSettings::get_singleton();
	{
		MutexLock lock(fg_mutex);
		const uint32_t version = settings != nullptr ? settings->get_version() : 0;
		if (version == 0 || version != fg_settings_version) {
			fg_settings_version = version;
			fg_settings_provider = _fg_setting<String>(FG_PROVIDER_SETTING, String());
			fg_settings_multiplier = _fg_setting<int>(FG_MULTIPLIER_SETTING, MULTIPLIER_MIN);
			fg_settings_dynamic = _fg_setting<bool>(FG_DYNAMIC_SETTING, false);
			fg_settings_dynamic_target_fps = MAX(_fg_setting<int>(FG_DYNAMIC_TARGET_FPS_SETTING, 0), 0);
		}
		context.provider = fg_settings_provider;
		context.multiplier = fg_settings_multiplier;
		context.dynamic = fg_settings_dynamic;
		context.dynamic_target_fps = fg_settings_dynamic_target_fps;
	}
	context.editor = Engine::get_singleton()->is_editor_hint();
	context.rendering_device = RenderingDevice::get_singleton() != nullptr && OS::get_singleton()->get_current_rendering_method() != "gl_compatibility";
	return context;
}

void FrameGenerationRegistry::register_extension(const Ref<FrameGenerationExtension> &p_extension) {
	ERR_FAIL_COND(p_extension.is_null());
	const String name = p_extension->get_provider_name();
	ERR_FAIL_COND_MSG(name.is_empty(), "FrameGenerationExtension::_get_name() must return a non-empty name.");
	MutexLock lock(fg_mutex);
	if (fg_active.is_valid() && name == fg_active_name && fg_active != p_extension) {
		ERR_FAIL_MSG(vformat("Frame generation: a second provider named \"%s\" was not registered (%s): the provider of that name is active in this process; switching providers needs a restart.", name, FrameGenerationExtension::get_reason_token(FrameGenerationExtension::REASON_PROVIDER_ALREADY_ACTIVE)));
	}
	// One entry per object: an object registered under an older name moves to the new one.
	LocalVector<String> stale;
	for (const KeyValue<String, Ref<FrameGenerationExtension>> &E : fg_providers) {
		if (E.value == p_extension && E.key != name) {
			stale.push_back(E.key);
		}
	}
	for (const String &key : stale) {
		fg_providers.erase(key);
	}
	fg_providers[name] = p_extension;
	fg_revision++;
}

void FrameGenerationRegistry::unregister_extension(ObjectID p_extension_id) {
	Ref<FrameGenerationExtension> released;
	{
		MutexLock lock(fg_mutex);
		String key;
		for (const KeyValue<String, Ref<FrameGenerationExtension>> &E : fg_providers) {
			if (E.value.is_valid() && E.value->get_instance_id() == p_extension_id) {
				key = E.key;
				break;
			}
		}
		if (key.is_empty()) {
			return;
		}
		released = fg_providers[key];
		fg_providers.erase(key);
		if (fg_active == released) {
			fg_active.unref();
			fg_active_name = String();
			fg_multiplier = 1;
		}
		if (fg_mode_target == p_extension_id) {
			fg_mode_target = ObjectID();
		}
		fg_revision++;
	}
	// The last reference may go here, outside the lock.
	released.unref();
}

void FrameGenerationRegistry::update(const Context &p_context) {
	Ref<FrameGenerationExtension> candidate;
	bool send_mode = false;
	Dictionary mode;
	const int requested_multiplier = clamp_multiplier(p_context.multiplier);
	const int requested_dynamic_target_fps = MAX(p_context.dynamic_target_fps, 0);
	{
		MutexLock lock(fg_mutex);
		if (fg_resolved && p_context == fg_context && fg_resolved_revision == fg_revision) {
			return;
		}
		fg_resolved = true;
		fg_context = p_context;
		fg_resolved_revision = fg_revision;
		const Ref<FrameGenerationExtension> *entry = fg_providers.getptr(p_context.provider);
		if (entry != nullptr) {
			candidate = *entry;
		}
		mode["multiplier"] = requested_multiplier;
		mode["dynamic"] = p_context.dynamic;
		mode["dynamic_target_fps"] = requested_dynamic_target_fps;
		send_mode = candidate.is_valid() && (fg_mode_target != candidate->get_instance_id() || fg_mode_multiplier != requested_multiplier || fg_mode_dynamic != p_context.dynamic || fg_mode_dynamic_target_fps != requested_dynamic_target_fps);
	}

	FGReason reason = FrameGenerationExtension::REASON_OK;
	Ref<FrameGenerationExtension> active;
	int multiplier = 1;
	bool keep_active = false;
	bool mode_sent = false;
	String locked_name;
	{
		MutexLock lock(fg_mutex);
		locked_name = fg_locked_name;
	}

	if (p_context.provider.is_empty()) {
		// Frame generation is off: no fallback, nothing to log.
	} else if (p_context.editor || p_context.xr_main_window) {
		reason = FrameGenerationExtension::REASON_NOT_MAIN_WINDOW;
	} else if (!p_context.rendering_device) {
		reason = FrameGenerationExtension::REASON_API_UNSUPPORTED;
	} else if (candidate.is_null()) {
		reason = FrameGenerationExtension::REASON_PROVIDER_UNAVAILABLE;
	} else if (!locked_name.is_empty() && locked_name != p_context.provider) {
		// The provider that holds the process keeps running.
		reason = FrameGenerationExtension::REASON_PROVIDER_ALREADY_ACTIVE;
		keep_active = true;
	} else {
		// Script and GDExtension calls run outside the lock.
		if (send_mode) {
			candidate->set_mode(mode);
			mode_sent = true;
		}
		const FGReason available = candidate->is_available();
		if (available == FrameGenerationExtension::REASON_OK) {
			active = candidate;
			multiplier = requested_multiplier;
		} else if (available == FrameGenerationExtension::REASON_MFG_REQUIRES_RTX50) {
			// MFG falls back to FG 2x and says why.
			active = candidate;
			multiplier = 2;
			reason = available;
		} else if (available == FrameGenerationExtension::REASON_DYNAMIC_MFG_UNSUPPORTED) {
			// Dynamic MFG unsupported: stay active at the fixed requested multiplier and say why.
			active = candidate;
			multiplier = requested_multiplier;
			reason = available;
		} else {
			reason = available;
		}
	}

	bool log = false;
	{
		MutexLock lock(fg_mutex);
		if (mode_sent) {
			fg_mode_target = candidate->get_instance_id();
			fg_mode_multiplier = requested_multiplier;
			fg_mode_dynamic = p_context.dynamic;
			fg_mode_dynamic_target_fps = requested_dynamic_target_fps;
		}
		if (!keep_active) {
			if (active.is_valid() && active != fg_active) {
				fg_activation_reset = true;
			}
			fg_active = active;
			fg_active_name = active.is_valid() ? p_context.provider : String();
			fg_multiplier = active.is_valid() ? multiplier : 1;
		}
		if (active.is_valid()) {
			fg_locked_name = p_context.provider;
		}
		fg_reason = reason;
		if (reason == FrameGenerationExtension::REASON_OK) {
			fg_logged_provider = String();
			fg_logged_reason = FrameGenerationExtension::REASON_OK;
		} else if (reason != fg_logged_reason || p_context.provider != fg_logged_provider) {
			fg_logged_provider = p_context.provider;
			fg_logged_reason = reason;
			log = true;
		}
	}
	if (log) {
		_fg_log(p_context.provider, reason, requested_multiplier, locked_name);
	}
}

Dictionary FrameGenerationRegistry::get_status() {
	Dictionary status;
	Ref<FrameGenerationExtension> active_provider;
	{
		MutexLock lock(fg_mutex);
		status["provider"] = fg_context.provider;
		status["requested"] = !fg_context.provider.is_empty();
		status["active"] = fg_active.is_valid();
		status["active_provider"] = fg_active_name;
		status["reason"] = int(fg_reason);
		status["multiplier"] = fg_active.is_valid() ? fg_multiplier : 1;
		status["requested_multiplier"] = clamp_multiplier(fg_context.multiplier);
		status["dynamic"] = fg_context.dynamic;
		status["dynamic_target_fps"] = MAX(fg_context.dynamic_target_fps, 0);
		status["present_hook"] = has_present_hook();
		active_provider = fg_active;
	}
	// GodotNext: pu_dlss_fg (#200, #201). Query C++ provider capabilities outside fg_mutex.
	int max_multiplier = 1;
	bool dynamic_supported = false;
	int64_t num_frames_presented = 0;
	int64_t vram_bytes = 0;
	if (active_provider.is_valid()) {
		const Dictionary caps = active_provider->get_provider_capabilities();
		max_multiplier = int(caps.get("max_multiplier", int(status["multiplier"])));
		dynamic_supported = bool(caps.get("dynamic_supported", false));
		num_frames_presented = int64_t(caps.get("num_frames_presented", int64_t(0)));
		vram_bytes = int64_t(caps.get("vram_bytes", int64_t(0)));
	}
	status["max_multiplier"] = max_multiplier;
	status["dynamic_supported"] = dynamic_supported;
	status["num_frames_presented"] = num_frames_presented;
	status["vram_bytes"] = vram_bytes;
	return status;
}

FrameGenerationExtension::FrameGenerationReason FrameGenerationRegistry::get_reason() {
	MutexLock lock(fg_mutex);
	return fg_reason;
}

bool FrameGenerationRegistry::is_active() {
	MutexLock lock(fg_mutex);
	return fg_active.is_valid();
}

int FrameGenerationRegistry::get_active_multiplier() {
	MutexLock lock(fg_mutex);
	return fg_active.is_valid() ? fg_multiplier : 1;
}

Ref<FrameGenerationExtension> FrameGenerationRegistry::get_active_extension() {
	MutexLock lock(fg_mutex);
	return fg_active;
}

bool FrameGenerationRegistry::is_viewport_eligible(DisplayServerEnums::WindowID p_screen, bool p_use_xr, uint32_t p_view_count, bool p_editor) {
	return p_screen == DisplayServerEnums::MAIN_WINDOW_ID && !p_use_xr && p_view_count == 1 && !p_editor;
}

Ref<FrameGenerationInputs> FrameGenerationRegistry::get_inputs() {
	MutexLock lock(fg_mutex);
	if (fg_inputs.is_null()) {
		fg_inputs.instantiate();
	}
	return fg_inputs;
}

void FrameGenerationRegistry::forget_split_textures(RID p_hudless_color) {
	Ref<FrameGenerationInputs> inputs;
	{
		MutexLock lock(fg_mutex);
		inputs = fg_inputs;
	}
	if (inputs.is_valid() && p_hudless_color.is_valid() && inputs->get_hudless_color() == p_hudless_color) {
		inputs->set_hudless_color(RID());
		inputs->set_ui_color(RID());
		inputs->set_depth(RID());
		inputs->set_velocity(RID());
	}
}

void FrameGenerationRegistry::dispatch_frame_inputs() {
	const Ref<FrameGenerationExtension> active = get_active_extension();
	if (active.is_null()) {
		return;
	}
	const Ref<FrameGenerationInputs> inputs = get_inputs();
	bool activation_reset = false;
	{
		MutexLock lock(fg_mutex);
		activation_reset = fg_activation_reset;
		fg_activation_reset = false;
	}
	inputs->set_swap_chain(0);
	if (activation_reset) {
		inputs->set_reset(true);
	}
	if (inputs->get_reset()) {
		active->reset();
	}
	active->on_frame_inputs(inputs);
}

void FrameGenerationRegistry::dispatch_present(uint64_t p_swap_chain, uint64_t p_frame_index, bool p_before) {
	const Ref<FrameGenerationExtension> active = get_active_extension();
	if (active.is_null()) {
		return;
	}
	const Ref<FrameGenerationInputs> inputs = get_inputs();
	inputs->set_swap_chain(p_swap_chain);
	inputs->set_frame_index(p_frame_index);
	active->on_present(inputs.ptr(), p_before);
}

bool FrameGenerationRegistry::has_present_hook() {
	// Only D3D12 has a present hook (#120); Vulkan and Metal have none.
	return fg_present_hook && RenderingDevice::get_singleton() != nullptr && RenderingDevice::get_singleton()->get_device_api_name() == "D3D12";
}

#ifdef D3D12_ENABLED
// The main window's HWND, read once: it does not change while the engine runs.
static int64_t fg_main_window_handle = 0;

static void _fg_d3d12_present(int64_t p_swap_chain, int64_t p_frame_index, int p_stage) {
	if (p_swap_chain == 0 || !FrameGenerationRegistry::is_active()) {
		return;
	}
	if (fg_main_window_handle == 0) {
		DisplayServer *display_server = DisplayServer::get_singleton();
		if (display_server == nullptr) {
			return;
		}
		fg_main_window_handle = display_server->window_get_native_handle(DisplayServerEnums::WINDOW_HANDLE, DisplayServerEnums::MAIN_WINDOW_ID);
	}
	// The main window's swap chain only: secondary windows never get frame generation.
	IDXGISwapChain *swap_chain = reinterpret_cast<IDXGISwapChain *>(uintptr_t(p_swap_chain));
	DXGI_SWAP_CHAIN_DESC desc = {};
	if (FAILED(swap_chain->GetDesc(&desc)) || desc.OutputWindow == nullptr || int64_t(uintptr_t(desc.OutputWindow)) != fg_main_window_handle) {
		return;
	}
	FrameGenerationRegistry::dispatch_present(uint64_t(p_swap_chain), uint64_t(p_frame_index), p_stage == D3D12Hooks::PRESENT_STAGE_BEFORE);
}
#endif

void FrameGenerationRegistry::install_present_hook() {
#ifdef D3D12_ENABLED
	if (fg_present_hook || Engine::get_singleton()->is_editor_hint() || OS::get_singleton()->get_current_rendering_driver_name() != "d3d12") {
		return;
	}
	if (_fg_setting<String>(FG_PROVIDER_SETTING, String()).is_empty()) {
		// Frame generation is off: D3D12 presents exactly as before.
		return;
	}
	if (D3D12Hooks::has_present_callback()) {
		WARN_PRINT("Frame generation: a module registered the D3D12 present callback, so FrameGenerationExtension._on_present() is not called.");
		return;
	}
	fg_present_hook = D3D12Hooks::set_present_callback(callable_mp_static(&_fg_d3d12_present)) == OK;
#endif
}

void FrameGenerationRegistry::clear() {
	HashMap<String, Ref<FrameGenerationExtension>> released;
	Ref<FrameGenerationExtension> released_active;
	Ref<FrameGenerationInputs> released_inputs;
	{
		MutexLock lock(fg_mutex);
		released = fg_providers;
		fg_providers.clear();
		released_active = fg_active;
		fg_active.unref();
		released_inputs = fg_inputs;
		fg_inputs.unref();
		fg_revision++;
		fg_settings_version = 0;
		fg_settings_dynamic_target_fps = 0;
		fg_resolved = false;
		fg_context = Context();
		fg_resolved_revision = 0;
		fg_active_name = String();
		fg_locked_name = String();
		fg_reason = FrameGenerationExtension::REASON_OK;
		fg_multiplier = 1;
		fg_mode_target = ObjectID();
		fg_mode_multiplier = 0;
		fg_mode_dynamic = false;
		fg_mode_dynamic_target_fps = -1;
		fg_logged_provider = String();
		fg_logged_reason = FrameGenerationExtension::REASON_OK;
		fg_activation_reset = false;
	}
	// The last references may go here, outside the lock.
	released.clear();
	released_active.unref();
	released_inputs.unref();
}
