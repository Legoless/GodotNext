/**************************************************************************/
/*  temporal_upscaler.cpp                                                 */
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

#include "servers/rendering/renderer_rd/effects/temporal_upscaler.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/os/mutex.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"

// TemporalUpscalerInputs

void TemporalUpscalerInputs::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_color"), &TemporalUpscalerInputs::get_color);
	ClassDB::bind_method(D_METHOD("set_color", "color"), &TemporalUpscalerInputs::set_color);
	ADD_PROPERTY(PropertyInfo(Variant::RID, "color"), "set_color", "get_color");

	ClassDB::bind_method(D_METHOD("get_depth"), &TemporalUpscalerInputs::get_depth);
	ClassDB::bind_method(D_METHOD("set_depth", "depth"), &TemporalUpscalerInputs::set_depth);
	ADD_PROPERTY(PropertyInfo(Variant::RID, "depth"), "set_depth", "get_depth");

	ClassDB::bind_method(D_METHOD("get_velocity"), &TemporalUpscalerInputs::get_velocity);
	ClassDB::bind_method(D_METHOD("set_velocity", "velocity"), &TemporalUpscalerInputs::set_velocity);
	ADD_PROPERTY(PropertyInfo(Variant::RID, "velocity"), "set_velocity", "get_velocity");

	ClassDB::bind_method(D_METHOD("get_reactive"), &TemporalUpscalerInputs::get_reactive);
	ClassDB::bind_method(D_METHOD("set_reactive", "reactive"), &TemporalUpscalerInputs::set_reactive);
	ADD_PROPERTY(PropertyInfo(Variant::RID, "reactive"), "set_reactive", "get_reactive");

	ClassDB::bind_method(D_METHOD("get_output"), &TemporalUpscalerInputs::get_output);
	ClassDB::bind_method(D_METHOD("set_output", "output"), &TemporalUpscalerInputs::set_output);
	ADD_PROPERTY(PropertyInfo(Variant::RID, "output"), "set_output", "get_output");

	ClassDB::bind_method(D_METHOD("get_internal_size"), &TemporalUpscalerInputs::get_internal_size);
	ClassDB::bind_method(D_METHOD("set_internal_size", "internal_size"), &TemporalUpscalerInputs::set_internal_size);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "internal_size"), "set_internal_size", "get_internal_size");

	ClassDB::bind_method(D_METHOD("get_max_internal_size"), &TemporalUpscalerInputs::get_max_internal_size);
	ClassDB::bind_method(D_METHOD("set_max_internal_size", "max_internal_size"), &TemporalUpscalerInputs::set_max_internal_size);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "max_internal_size"), "set_max_internal_size", "get_max_internal_size");

	ClassDB::bind_method(D_METHOD("get_output_size"), &TemporalUpscalerInputs::get_output_size);
	ClassDB::bind_method(D_METHOD("set_output_size", "output_size"), &TemporalUpscalerInputs::set_output_size);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "output_size"), "set_output_size", "get_output_size");

	ClassDB::bind_method(D_METHOD("get_motion_vector_scale"), &TemporalUpscalerInputs::get_motion_vector_scale);
	ClassDB::bind_method(D_METHOD("set_motion_vector_scale", "motion_vector_scale"), &TemporalUpscalerInputs::set_motion_vector_scale);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "motion_vector_scale"), "set_motion_vector_scale", "get_motion_vector_scale");

	ClassDB::bind_method(D_METHOD("get_jitter"), &TemporalUpscalerInputs::get_jitter);
	ClassDB::bind_method(D_METHOD("set_jitter", "jitter"), &TemporalUpscalerInputs::set_jitter);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "jitter"), "set_jitter", "get_jitter");

	ClassDB::bind_method(D_METHOD("get_world_to_view"), &TemporalUpscalerInputs::get_world_to_view);
	ClassDB::bind_method(D_METHOD("set_world_to_view", "world_to_view"), &TemporalUpscalerInputs::set_world_to_view);
	ADD_PROPERTY(PropertyInfo(Variant::TRANSFORM3D, "world_to_view"), "set_world_to_view", "get_world_to_view");

	ClassDB::bind_method(D_METHOD("get_view_to_clip"), &TemporalUpscalerInputs::get_view_to_clip);
	ClassDB::bind_method(D_METHOD("set_view_to_clip", "view_to_clip"), &TemporalUpscalerInputs::set_view_to_clip);
	ADD_PROPERTY(PropertyInfo(Variant::PROJECTION, "view_to_clip"), "set_view_to_clip", "get_view_to_clip");

	ClassDB::bind_method(D_METHOD("get_z_near"), &TemporalUpscalerInputs::get_z_near);
	ClassDB::bind_method(D_METHOD("set_z_near", "z_near"), &TemporalUpscalerInputs::set_z_near);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "z_near"), "set_z_near", "get_z_near");

	ClassDB::bind_method(D_METHOD("get_z_far"), &TemporalUpscalerInputs::get_z_far);
	ClassDB::bind_method(D_METHOD("set_z_far", "z_far"), &TemporalUpscalerInputs::set_z_far);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "z_far"), "set_z_far", "get_z_far");

	ClassDB::bind_method(D_METHOD("get_fovy"), &TemporalUpscalerInputs::get_fovy);
	ClassDB::bind_method(D_METHOD("set_fovy", "fovy"), &TemporalUpscalerInputs::set_fovy);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fovy"), "set_fovy", "get_fovy");

	ClassDB::bind_method(D_METHOD("get_delta_time"), &TemporalUpscalerInputs::get_delta_time);
	ClassDB::bind_method(D_METHOD("set_delta_time", "delta_time"), &TemporalUpscalerInputs::set_delta_time);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "delta_time"), "set_delta_time", "get_delta_time");

	ClassDB::bind_method(D_METHOD("get_reset"), &TemporalUpscalerInputs::get_reset);
	ClassDB::bind_method(D_METHOD("set_reset", "reset"), &TemporalUpscalerInputs::set_reset);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "reset"), "set_reset", "get_reset");

	ClassDB::bind_method(D_METHOD("get_view_index"), &TemporalUpscalerInputs::get_view_index);
	ClassDB::bind_method(D_METHOD("set_view_index", "view_index"), &TemporalUpscalerInputs::set_view_index);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "view_index"), "set_view_index", "get_view_index");

	ClassDB::bind_method(D_METHOD("get_render_scene_buffers"), &TemporalUpscalerInputs::get_render_scene_buffers);
	ClassDB::bind_method(D_METHOD("set_render_scene_buffers", "render_scene_buffers"), &TemporalUpscalerInputs::set_render_scene_buffers);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "render_scene_buffers", PROPERTY_HINT_RESOURCE_TYPE, "RenderSceneBuffers"), "set_render_scene_buffers", "get_render_scene_buffers");
}

// TemporalUpscalerExtension

void TemporalUpscalerExtension::_bind_methods() {
	GDVIRTUAL_BIND(_get_name);
	GDVIRTUAL_BIND(_is_available);
	GDVIRTUAL_BIND(_get_scale_range);
	GDVIRTUAL_BIND(_get_mip_bias_offset);
	GDVIRTUAL_BIND(_get_jitter_phase_count, "target_size", "render_size");
	GDVIRTUAL_BIND(_process, "inputs");
	GDVIRTUAL_BIND(_reset_history);
}

String TemporalUpscalerExtension::get_upscaler_name() const {
	String ret;
	GDVIRTUAL_CALL(_get_name, ret);
	return ret;
}

bool TemporalUpscalerExtension::is_available() const {
	bool ret = false;
	GDVIRTUAL_CALL(_is_available, ret);
	return ret;
}

Vector2 TemporalUpscalerExtension::get_scale_range() const {
	Vector2 range(0.25f, 2.0f);
	if (GDVIRTUAL_CALL(_get_scale_range, range)) {
		// Order the bounds and keep the minimum positive.
		const float lo = MAX(MIN(range.x, range.y), 0.01f);
		const float hi = MAX(MAX(range.x, range.y), lo);
		return Vector2(lo, hi);
	}
	return range;
}

float TemporalUpscalerExtension::get_mip_bias_offset() const {
	float offset = -0.5f;
	GDVIRTUAL_CALL(_get_mip_bias_offset, offset);
	return offset;
}

uint32_t TemporalUpscalerExtension::get_jitter_phase_count(const Size2i &p_target_size, const Size2i &p_render_size) const {
	int phases = -1;
	if (GDVIRTUAL_CALL(_get_jitter_phase_count, Vector2i(p_target_size), Vector2i(p_render_size), phases) && phases >= 0) {
		return uint32_t(phases);
	}
	return nextgi_temporal_jitter_phase_count(p_target_size, p_render_size, true);
}

void TemporalUpscalerExtension::process(const Ref<TemporalUpscalerInputs> &p_inputs) {
	GDVIRTUAL_CALL(_process, p_inputs);
}

void TemporalUpscalerExtension::reset_history() {
	GDVIRTUAL_CALL(_reset_history);
}

// TemporalUpscalerRegistry

namespace {

struct CustomUpscalerEntry {
	String name;
	Ref<TemporalUpscalerExtension> extension;
	TemporalUpscaler *adapter = nullptr;
	uint64_t id = 0;
};

Mutex custom_upscaler_mutex;
HashMap<String, CustomUpscalerEntry> custom_upscaler_entries;
Vector<CustomUpscalerEntry> retired_custom_upscalers;
uint64_t custom_upscaler_revision = 1;
uint64_t next_custom_upscaler_id = 1;
// The id of the adapter published in upscalers[CUSTOM]; 0 when it is empty.
uint64_t published_custom_upscaler_id = 0;
bool custom_unavailable_warned = false;
bool custom_scale_clamp_warned = false;

String _configured_custom_upscaler_name() {
	if (ProjectSettings::get_singleton() == nullptr || !ProjectSettings::get_singleton()->has_setting("rendering/scaling_3d/custom_upscaler")) {
		return String();
	}
	return String(ProjectSettings::get_singleton()->get_setting("rendering/scaling_3d/custom_upscaler"));
}

} // namespace

TemporalUpscaler *TemporalUpscalerRegistry::upscalers[RSE::VIEWPORT_SCALING_3D_MODE_MAX] = {};

#ifndef RD_ENABLED
TemporalUpscaler *TemporalUpscalerRegistry::create_extension_adapter(const Ref<TemporalUpscalerExtension> &p_extension) {
	(void)p_extension;
	return nullptr;
}

void TemporalUpscalerRegistry::destroy_extension_adapter(TemporalUpscaler *p_adapter) {
	(void)p_adapter;
}
#endif

void TemporalUpscalerRegistry::register_upscaler(RSE::ViewportScaling3DMode p_mode, TemporalUpscaler *p_upscaler) {
	ERR_FAIL_INDEX(p_mode, RSE::VIEWPORT_SCALING_3D_MODE_MAX);
	ERR_FAIL_COND_MSG(p_mode == RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM, "SCALING_3D_MODE_CUSTOM is resolved from registered TemporalUpscalerExtensions; use register_extension().");
	upscalers[p_mode] = p_upscaler;
}

void TemporalUpscalerRegistry::unregister_upscaler(RSE::ViewportScaling3DMode p_mode) {
	ERR_FAIL_INDEX(p_mode, RSE::VIEWPORT_SCALING_3D_MODE_MAX);
	ERR_FAIL_COND_MSG(p_mode == RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM, "SCALING_3D_MODE_CUSTOM is resolved from registered TemporalUpscalerExtensions; use unregister_extension().");
	upscalers[p_mode] = nullptr;
}

TemporalUpscaler *TemporalUpscalerRegistry::get(RSE::ViewportScaling3DMode p_mode) {
	ERR_FAIL_INDEX_V(p_mode, RSE::VIEWPORT_SCALING_3D_MODE_MAX, nullptr);
	if (p_mode == RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM) {
		MutexLock lock(custom_upscaler_mutex);
		return upscalers[p_mode];
	}
	return upscalers[p_mode];
}

// Script and GDExtension calls (_get_name, _is_available) never run while
// custom_upscaler_mutex is held, so a slow or re-entrant extension cannot
// stall the render thread on the lock.
void TemporalUpscalerRegistry::register_extension(const Ref<TemporalUpscalerExtension> &p_extension) {
	ERR_FAIL_COND(p_extension.is_null());
	const String name = p_extension->get_upscaler_name();
	ERR_FAIL_COND_MSG(name.is_empty(), "TemporalUpscalerExtension::_get_name() must return a non-empty name.");

	TemporalUpscaler *adapter = nullptr;
	{
		MutexLock lock(custom_upscaler_mutex);
		const CustomUpscalerEntry *existing = custom_upscaler_entries.getptr(name);
		if (existing != nullptr && existing->extension == p_extension) {
			// Re-registering the same object re-reads its virtuals on the next configure.
			custom_upscaler_revision++;
			adapter = existing->adapter;
		}
	}
	if (adapter == nullptr) {
		adapter = create_extension_adapter(p_extension);
		MutexLock lock(custom_upscaler_mutex);
		// Retire the entry this name replaces, and this object's entry under an
		// older name; both are freed at frame end, like an unregister.
		LocalVector<String> replaced;
		for (const KeyValue<String, CustomUpscalerEntry> &E : custom_upscaler_entries) {
			if (E.key == name || E.value.extension == p_extension) {
				replaced.push_back(E.key);
			}
		}
		for (const String &key : replaced) {
			const CustomUpscalerEntry existing = custom_upscaler_entries[key];
			if (upscalers[RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM] == existing.adapter) {
				upscalers[RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM] = nullptr;
				published_custom_upscaler_id = 0;
			}
			retired_custom_upscalers.push_back(existing);
			custom_upscaler_entries.erase(key);
		}
		CustomUpscalerEntry entry;
		entry.name = name;
		entry.extension = p_extension;
		entry.adapter = adapter;
		entry.id = next_custom_upscaler_id++;
		custom_upscaler_entries[name] = entry;
		custom_upscaler_revision++;
	}
	sync_custom_upscaler();
}

void TemporalUpscalerRegistry::unregister_extension(const Ref<TemporalUpscalerExtension> &p_extension) {
	ERR_FAIL_COND(p_extension.is_null());
	unregister_extension(p_extension->get_instance_id());
}

void TemporalUpscalerRegistry::unregister_extension(ObjectID p_extension_id) {
	{
		MutexLock lock(custom_upscaler_mutex);
		String matched_key;
		for (const KeyValue<String, CustomUpscalerEntry> &E : custom_upscaler_entries) {
			if (E.value.extension.is_valid() && E.value.extension->get_instance_id() == p_extension_id) {
				matched_key = E.key;
				break;
			}
		}
		if (matched_key.is_empty()) {
			return;
		}
		const CustomUpscalerEntry removed = custom_upscaler_entries[matched_key];
		custom_upscaler_entries.erase(matched_key);
		if (upscalers[RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM] == removed.adapter) {
			upscalers[RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM] = nullptr;
			published_custom_upscaler_id = 0;
		}
		// Defer dropping the extension Ref and adapter until frame end.
		retired_custom_upscalers.push_back(removed);
		custom_upscaler_revision++;
	}
	sync_custom_upscaler();
}

Ref<TemporalUpscalerExtension> TemporalUpscalerRegistry::get_selected_extension() {
	const String name = _configured_custom_upscaler_name();
	if (name.is_empty()) {
		return Ref<TemporalUpscalerExtension>();
	}
	MutexLock lock(custom_upscaler_mutex);
	const CustomUpscalerEntry *entry = custom_upscaler_entries.getptr(name);
	return entry != nullptr ? entry->extension : Ref<TemporalUpscalerExtension>();
}

bool TemporalUpscalerRegistry::is_custom_upscaler_available() {
	const Ref<TemporalUpscalerExtension> selected = get_selected_extension();
	return selected.is_valid() && selected->is_available();
}

void TemporalUpscalerRegistry::sync_custom_upscaler() {
	const String name = _configured_custom_upscaler_name();
	Ref<TemporalUpscalerExtension> selected;
	TemporalUpscaler *adapter = nullptr;
	uint64_t id = 0;
	if (!name.is_empty()) {
		MutexLock lock(custom_upscaler_mutex);
		const CustomUpscalerEntry *entry = custom_upscaler_entries.getptr(name);
		if (entry != nullptr) {
			selected = entry->extension;
			adapter = entry->adapter;
			id = entry->id;
		}
	}
	const bool available = selected.is_valid() && adapter != nullptr && selected->is_available();

	MutexLock lock(custom_upscaler_mutex);
	// Publish only if the entry did not change while _is_available ran.
	const CustomUpscalerEntry *entry = name.is_empty() ? nullptr : custom_upscaler_entries.getptr(name);
	const bool unchanged = (entry == nullptr) ? adapter == nullptr : entry->adapter == adapter;
	if (unchanged) {
		upscalers[RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM] = available ? adapter : nullptr;
		published_custom_upscaler_id = available ? id : 0;
	}
}

TemporalUpscaler *TemporalUpscalerRegistry::get_custom_upscaler(uint64_t *r_id) {
	MutexLock lock(custom_upscaler_mutex);
	if (r_id != nullptr) {
		*r_id = published_custom_upscaler_id;
	}
	return upscalers[RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM];
}

uint64_t TemporalUpscalerRegistry::get_custom_upscaler_id() {
	MutexLock lock(custom_upscaler_mutex);
	return published_custom_upscaler_id;
}

void TemporalUpscalerRegistry::flush_retired_extensions() {
	Vector<CustomUpscalerEntry> to_free;
	{
		MutexLock lock(custom_upscaler_mutex);
		if (retired_custom_upscalers.is_empty()) {
			return;
		}
		to_free = retired_custom_upscalers;
		retired_custom_upscalers.clear();
	}
	for (CustomUpscalerEntry &entry : to_free) {
		if (entry.adapter != nullptr) {
			destroy_extension_adapter(entry.adapter);
			entry.adapter = nullptr;
		}
		entry.extension.unref();
	}
}

void TemporalUpscalerRegistry::clear_extensions() {
	Vector<CustomUpscalerEntry> to_free;
	{
		MutexLock lock(custom_upscaler_mutex);
		upscalers[RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM] = nullptr;
		published_custom_upscaler_id = 0;
		for (const KeyValue<String, CustomUpscalerEntry> &E : custom_upscaler_entries) {
			to_free.push_back(E.value);
		}
		custom_upscaler_entries.clear();
		for (const CustomUpscalerEntry &entry : retired_custom_upscalers) {
			to_free.push_back(entry);
		}
		retired_custom_upscalers.clear();
		custom_unavailable_warned = false;
		custom_scale_clamp_warned = false;
		custom_upscaler_revision++;
	}
	for (CustomUpscalerEntry &entry : to_free) {
		if (entry.adapter != nullptr) {
			destroy_extension_adapter(entry.adapter);
			entry.adapter = nullptr;
		}
		entry.extension.unref();
	}
}

uint64_t TemporalUpscalerRegistry::get_custom_upscaler_revision() {
	MutexLock lock(custom_upscaler_mutex);
	return custom_upscaler_revision;
}

uint32_t TemporalUpscalerRegistry::get_retired_extension_count() {
	MutexLock lock(custom_upscaler_mutex);
	return uint32_t(retired_custom_upscalers.size());
}

void TemporalUpscalerRegistry::report_custom_unavailable() {
	{
		MutexLock lock(custom_upscaler_mutex);
		if (custom_unavailable_warned) {
			return;
		}
		custom_unavailable_warned = true;
	}
	WARN_PRINT("Custom 3D temporal upscaler is unavailable. Falling back to TSR.");
}

void TemporalUpscalerRegistry::report_custom_scale_clamped(const String &p_name, const Vector2 &p_scale_range) {
	{
		MutexLock lock(custom_upscaler_mutex);
		if (custom_scale_clamp_warned) {
			return;
		}
		custom_scale_clamp_warned = true;
	}
	WARN_PRINT(vformat("Custom temporal upscaler '%s' scale must be between %.3f and %.3f. Clamping the scale.", p_name, p_scale_range.x, p_scale_range.y));
}

void TemporalUpscalerRegistry::reset_custom_warning_latches() {
	MutexLock lock(custom_upscaler_mutex);
	custom_unavailable_warned = false;
	custom_scale_clamp_warned = false;
}

void TemporalUpscalerRegistry::free_extension_contexts(RenderSceneBuffersRD *p_render_buffers) {
	// Both calls are idempotent, so every adapter is covered whichever one is
	// published right now.
	MutexLock lock(custom_upscaler_mutex);
	for (KeyValue<String, CustomUpscalerEntry> &E : custom_upscaler_entries) {
		if (E.value.adapter != nullptr) {
			E.value.adapter->free_contexts(p_render_buffers);
			E.value.adapter->clear_context_error(p_render_buffers);
		}
	}
	for (const CustomUpscalerEntry &entry : retired_custom_upscalers) {
		if (entry.adapter != nullptr) {
			entry.adapter->free_contexts(p_render_buffers);
			entry.adapter->clear_context_error(p_render_buffers);
		}
	}
}

void TemporalUpscalerRegistry::clear() {
	clear_extensions();
	for (int mode = 0; mode < RSE::VIEWPORT_SCALING_3D_MODE_MAX; mode++) {
		upscalers[mode] = nullptr;
	}
	MutexLock lock(custom_upscaler_mutex);
	custom_unavailable_warned = false;
	custom_scale_clamp_warned = false;
}

String TemporalUpscalerRegistry::build_mode_hint(bool p_metal) {
	Vector<String> hints = { "Nearest (Fastest):5", "Bilinear (Fastest):0", "FSR 1.0 (Fast):1", "FSR 2.2 (Slow):2" };
	if (p_metal) {
		hints.push_back("MetalFX (Spatial - Fast):3");
		hints.push_back("MetalFX (Temporal - Slow):4");
	}
	if (get(RSE::VIEWPORT_SCALING_3D_MODE_DLSS) != nullptr) {
		hints.push_back("DLSS (Slow):6");
	}
	hints.push_back("TSR:7");
	hints.push_back("Custom:8");
	return String(",").join(hints);
}
