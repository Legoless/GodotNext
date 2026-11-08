/**************************************************************************/
/*  nextgi_editor_plugin.cpp                                              */
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

#include "nextgi_editor_plugin.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "editor/editor_node.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/settings/editor_settings.h"
#include "scene/3d/world_environment.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/label.h"
#include "scene/resources/environment.h"
#include "scene/scene_string_names.h"
#include "servers/rendering/rendering_server.h"

void NextGIEditorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			// Reading the setting works before it is registered. A missing key is off.
			nextgi_was_enabled = ProjectSettings::get_singleton()->get_setting("rendering/global_illumination/nextgi/enabled", false);
			ProjectSettings::get_singleton()->connect("settings_changed", callable_mp(this, &NextGIEditorPlugin::_on_project_settings_changed));
		} break;
		case NOTIFICATION_EXIT_TREE: {
			ProjectSettings *ps = ProjectSettings::get_singleton();
			if (ps && ps->is_connected("settings_changed", callable_mp(this, &NextGIEditorPlugin::_on_project_settings_changed))) {
				ps->disconnect("settings_changed", callable_mp(this, &NextGIEditorPlugin::_on_project_settings_changed));
			}
		} break;
	}
}

void NextGIEditorPlugin::_on_project_settings_changed() {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		return;
	}

	const bool now_enabled = ps->get_setting("rendering/global_illumination/nextgi/enabled", false);
	EditorSettings *editor_settings = EditorSettings::get_singleton();
	const bool already_offered = editor_settings && bool(editor_settings->get_project_metadata("nextgi", "vulkan_switch_offered", false));
	const String driver_windows = ps->get_setting("rendering/rendering_device/driver.windows", "vulkan");

	if (should_offer_vulkan_switch(nextgi_was_enabled, now_enabled, driver_windows, already_offered)) {
		_offer_vulkan_switch();
	}
	// GodotNext: mesh_sdf_offer
	const bool mesh_sdf_generate = ps->get_setting("rendering/global_illumination/mesh_sdf/generate", false);
	const bool mesh_sdf_offered = editor_settings && bool(editor_settings->get_project_metadata("mesh_sdf", "generate_offered", false));
	if (should_offer_mesh_sdf(nextgi_was_enabled, now_enabled, mesh_sdf_generate, mesh_sdf_offered)) {
		_offer_mesh_sdf();
	}
	nextgi_was_enabled = now_enabled;
}

void NextGIEditorPlugin::_offer_vulkan_switch() {
	// Record the offer when the dialog is shown, whatever the user answers, so a later toggle or
	// an editor restart does not ask again. Cancel leaves the d3d12 project unchanged.
	EditorSettings *editor_settings = EditorSettings::get_singleton();
	if (editor_settings) {
		editor_settings->set_project_metadata("nextgi", "vulkan_switch_offered", true);
		editor_settings->save_project_metadata();
	}
	vulkan_switch_dialog->popup_centered();
}

void NextGIEditorPlugin::_vulkan_switch_confirmed() {
	apply_vulkan_switch();
	restart_dialog->popup_centered();
}

void NextGIEditorPlugin::_restart_confirmed() {
	EditorNode *editor = EditorNode::get_singleton();
	ERR_FAIL_NULL(editor);
	editor->restart_editor();
}

bool NextGIEditorPlugin::should_offer_vulkan_switch(bool p_was_enabled, bool p_is_enabled, const String &p_driver_windows, bool p_already_offered) {
	return !p_was_enabled && p_is_enabled && p_driver_windows == "d3d12" && !p_already_offered;
}

void NextGIEditorPlugin::apply_vulkan_switch() {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	ERR_FAIL_NULL(ps);
	ps->set_setting("rendering/rendering_device/driver.windows", "vulkan");
	ps->set_setting("rendering/rendering_device/fallback_to_d3d12", true);
	const Error err = ps->save();
	ERR_FAIL_COND_MSG(err != OK, vformat("Couldn't save project.godot after switching the Windows driver to Vulkan (error %d).", err));
}

// GodotNext: mesh_sdf_offer
static void _collect_reimport_files(EditorFileSystemDirectory *p_dir, Vector<String> &r_files) {
	if (p_dir == nullptr) {
		return;
	}
	for (int i = 0; i < p_dir->get_file_count(); i++) {
		String path = p_dir->get_file_path(i);
		if (p_dir->get_file_import_is_valid(i) && FileAccess::exists(path + ".import")) {
			r_files.push_back(path);
		}
	}
	for (int i = 0; i < p_dir->get_subdir_count(); i++) {
		_collect_reimport_files(p_dir->get_subdir(i), r_files);
	}
}

void NextGIEditorPlugin::_offer_mesh_sdf() {
	EditorSettings *editor_settings = EditorSettings::get_singleton();
	if (editor_settings) {
		editor_settings->set_project_metadata("mesh_sdf", "generate_offered", true);
		editor_settings->save_project_metadata();
	}
	mesh_sdf_dialog->popup_centered();
}

void NextGIEditorPlugin::_mesh_sdf_confirmed() {
	apply_mesh_sdf_enable();
	EditorFileSystem *efs = EditorFileSystem::get_singleton();
	if (efs != nullptr) {
		Vector<String> files;
		_collect_reimport_files(efs->get_filesystem(), files);
		if (!files.is_empty()) {
			efs->reimport_files(files);
		}
	}
}

bool NextGIEditorPlugin::should_offer_mesh_sdf(bool p_was_enabled, bool p_is_enabled, bool p_mesh_sdf_generate, bool p_already_offered) {
	return !p_was_enabled && p_is_enabled && !p_mesh_sdf_generate && !p_already_offered;
}

void NextGIEditorPlugin::apply_mesh_sdf_enable() {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	ERR_FAIL_NULL(ps);
	ps->set_setting("rendering/global_illumination/mesh_sdf/generate", true);
	const Error err = ps->save();
	ERR_FAIL_COND_MSG(err != OK, vformat("Couldn't save project.godot after enabling mesh distance field generation (error %d).", err));
}

String NextGIEditorPlugin::get_vulkan_switch_dialog_title() {
	return TTR("Use Vulkan on Windows for hardware RT");
}

bool NextGIEditorPlugin::should_show_inspector_vulkan_button(const String &p_host_os, const String &p_driver_windows) {
	return p_host_os == "Windows" && p_driver_windows == "d3d12";
}

String NextGIEditorPlugin::format_status_panel(const Dictionary &p_status) {
	return vformat("NextGI %s (%s), quality %s (%s), driver %s", String(p_status.get("tier", String())), String(p_status.get("reason", String())), String(p_status.get("quality", String())), String(p_status.get("quality_reason", String())), String(p_status.get("driver", String())));
}

String NextGIEditorPlugin::get_vulkan_switch_dialog_text() {
	// P0 copy. Until the software tier lands, a d3d12 project with NextGI on still runs SDFGI.
	return TTR("NextGI hardware RT is not yet available on Direct3D 12, so SDFGI is used; switching sets the Windows driver to Vulkan and keeps the Direct3D 12 fallback; Windows builds lose HDR output and DComp transparency; a restart is required.");
}

String NextGIEditorPlugin::format_tier_readout(const Dictionary &p_tier_info) {
	const String tier = p_tier_info.get("tier", String());
	const String reason = p_tier_info.get("reason", String());
	if (reason.is_empty() || reason == "NONE") {
		return vformat("NextGI: %s", tier);
	}
	return vformat("NextGI: %s (%s)", tier, reason);
}

String NextGIEditorPlugin::get_tier_readout() {
	if (!GLOBAL_GET_CACHED(bool, "rendering/global_illumination/nextgi/enabled")) {
		return String();
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	ERR_FAIL_NULL_V(rs, String());
	return format_tier_readout(rs->nextgi_get_tier_info());
}

static String _upscaler_mode_name(int p_mode, float p_scale) {
	if (p_mode == RSE::VIEWPORT_SCALING_3D_MODE_DLSS) {
		return Math::is_equal_approx(p_scale, 1.0f) ? String("DLAA") : String("DLSS");
	}
	switch (p_mode) {
		case RSE::VIEWPORT_SCALING_3D_MODE_BILINEAR:
			return "Bilinear";
		case RSE::VIEWPORT_SCALING_3D_MODE_FSR:
			return "FSR1";
		case RSE::VIEWPORT_SCALING_3D_MODE_FSR2:
			return "FSR2";
		case RSE::VIEWPORT_SCALING_3D_MODE_METALFX_SPATIAL:
			return "MetalFX Spatial";
		case RSE::VIEWPORT_SCALING_3D_MODE_METALFX_TEMPORAL:
			return "MetalFX Temporal";
		case RSE::VIEWPORT_SCALING_3D_MODE_NEAREST:
			return "Nearest";
		case RSE::VIEWPORT_SCALING_3D_MODE_TSR:
			return "TSR";
		case RSE::VIEWPORT_SCALING_3D_MODE_CUSTOM:
			return "Custom";
		default:
			return "Off";
	}
}

static const char *_upscaler_reason_name(int p_reason) {
	switch (p_reason) {
		case RSE::VIEWPORT_SCALING_3D_REASON_NONE:
			return "NONE";
		case RSE::VIEWPORT_SCALING_3D_REASON_RENDERER_UNSUPPORTED:
			return "RENDERER_UNSUPPORTED";
		case RSE::VIEWPORT_SCALING_3D_REASON_API_UNSUPPORTED:
			return "API_UNSUPPORTED";
		case RSE::VIEWPORT_SCALING_3D_REASON_MODULE_DISABLED:
			return "MODULE_DISABLED";
		case RSE::VIEWPORT_SCALING_3D_REASON_NOT_NVIDIA_RTX:
			return "NOT_NVIDIA_RTX";
		case RSE::VIEWPORT_SCALING_3D_REASON_DRIVER_TOO_OLD:
			return "DRIVER_TOO_OLD";
		case RSE::VIEWPORT_SCALING_3D_REASON_RUNTIME_MISSING:
			return "RUNTIME_MISSING";
		case RSE::VIEWPORT_SCALING_3D_REASON_EXTENSION_MISSING:
			return "EXTENSION_MISSING";
		case RSE::VIEWPORT_SCALING_3D_REASON_PROJECT_ID_MISSING:
			return "PROJECT_ID_MISSING";
		case RSE::VIEWPORT_SCALING_3D_REASON_FEATURE_CREATE_FAILED:
			return "FEATURE_CREATE_FAILED";
		case RSE::VIEWPORT_SCALING_3D_REASON_SCALE_UNSUPPORTED:
			return "SCALE_UNSUPPORTED";
		case RSE::VIEWPORT_SCALING_3D_REASON_VIEW_UNSUPPORTED:
			return "VIEW_UNSUPPORTED";
		case RSE::VIEWPORT_SCALING_3D_REASON_CUSTOM_UPSCALER_UNAVAILABLE:
			return "CUSTOM_UPSCALER_UNAVAILABLE";
		default:
			return "UNKNOWN";
	}
}

String NextGIEditorPlugin::format_upscaler_readout(const Dictionary &p_status) {
	const int requested = int(p_status.get("requested_mode", RSE::VIEWPORT_SCALING_3D_MODE_BILINEAR));
	const int active = int(p_status.get("active_mode", RSE::VIEWPORT_SCALING_3D_MODE_BILINEAR));
	const float scale = p_status.get("effective_scale", 1.0f);
	return vformat("Upscaler: %s → %s (%d, %s)", _upscaler_mode_name(requested, scale), _upscaler_mode_name(active, scale), int(p_status.get("preset", 0)), _upscaler_reason_name(int(p_status.get("reason", RSE::VIEWPORT_SCALING_3D_REASON_NONE))));
}

String NextGIEditorPlugin::get_upscaler_readout(RID p_viewport) {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr || p_viewport.is_null()) {
		return String();
	}
	const Dictionary status = rs->viewport_get_scaling_3d_status(p_viewport);
	return status.is_empty() ? String() : format_upscaler_readout(status);
}

NextGIEditorPlugin::NextGIEditorPlugin() {
	vulkan_switch_dialog = memnew(ConfirmationDialog);
	vulkan_switch_dialog->set_title(get_vulkan_switch_dialog_title());
	vulkan_switch_dialog->set_text(get_vulkan_switch_dialog_text());
	vulkan_switch_dialog->set_autowrap(true);
	vulkan_switch_dialog->set_min_size(Size2i(560, 180));
	vulkan_switch_dialog->connect(SceneStringName(confirmed), callable_mp(this, &NextGIEditorPlugin::_vulkan_switch_confirmed));
	add_child(vulkan_switch_dialog);

	restart_dialog = memnew(ConfirmationDialog);
	restart_dialog->set_title(TTR("Restart Editor"));
	restart_dialog->set_text(TTR("The Windows driver is now Vulkan, and the Direct3D 12 fallback stays on. Restart the editor to apply the change."));
	restart_dialog->set_autowrap(true);
	restart_dialog->set_ok_button_text(TTR("Restart"));
	restart_dialog->connect(SceneStringName(confirmed), callable_mp(this, &NextGIEditorPlugin::_restart_confirmed));
	add_child(restart_dialog);

	// GodotNext: mesh_sdf_offer
	mesh_sdf_dialog = memnew(ConfirmationDialog);
	mesh_sdf_dialog->set_title(TTR("Generate Mesh Distance Fields"));
	mesh_sdf_dialog->set_text(TTR("NextGI's software tier traces against per-mesh distance fields. Enable \"rendering/global_illumination/mesh_sdf/generate\" and reimport project files so existing meshes get distance fields? Cancel leaves the project unchanged."));
	mesh_sdf_dialog->set_autowrap(true);
	mesh_sdf_dialog->set_min_size(Size2i(560, 180));
	mesh_sdf_dialog->set_ok_button_text(TTR("Enable and Reimport"));
	mesh_sdf_dialog->connect(SceneStringName(confirmed), callable_mp(this, &NextGIEditorPlugin::_mesh_sdf_confirmed));
	add_child(mesh_sdf_dialog);

	// GodotNext: nextgi_user_surface
	Ref<EditorInspectorPluginNextGI> inspector_plugin;
	inspector_plugin.instantiate();
	add_inspector_plugin(inspector_plugin);
}

// GodotNext: nextgi_user_surface
// The status panel and the Windows-only Vulkan button. The button never switches by itself:
// confirm applies the #23 helper, then a second dialog offers the restart.

class NextGIStatusPanel : public VBoxContainer {
	GDCLASS(NextGIStatusPanel, VBoxContainer);

	ConfirmationDialog *switch_dialog = nullptr;
	ConfirmationDialog *restart_dialog = nullptr;

	void _switch_pressed() {
		switch_dialog->popup_centered();
	}

	void _switch_confirmed() {
		NextGIEditorPlugin::apply_vulkan_switch();
		restart_dialog->popup_centered();
	}

	void _restart_confirmed() {
		EditorNode *editor = EditorNode::get_singleton();
		ERR_FAIL_NULL(editor);
		editor->restart_editor();
	}

protected:
	static void _bind_methods() {}

public:
	NextGIStatusPanel() {
		Label *status = memnew(Label);
		status->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
		status->set_text(NextGIEditorPlugin::format_status_panel(RenderingServer::get_singleton()->nextgi_get_status()));
		add_child(status);

		ProjectSettings *ps = ProjectSettings::get_singleton();
		const String driver = ps ? String(ps->get_setting("rendering/rendering_device/driver.windows", "vulkan")) : String("vulkan");
		const String host_os = OS::get_singleton() ? OS::get_singleton()->get_name() : String();
		if (!NextGIEditorPlugin::should_show_inspector_vulkan_button(host_os, driver)) {
			return;
		}

		Button *button = memnew(Button);
		button->set_text(NextGIEditorPlugin::get_vulkan_switch_dialog_title());
		button->connect(SceneStringName(pressed), callable_mp(this, &NextGIStatusPanel::_switch_pressed));
		add_child(button);

		switch_dialog = memnew(ConfirmationDialog);
		switch_dialog->set_title(NextGIEditorPlugin::get_vulkan_switch_dialog_title());
		switch_dialog->set_text(NextGIEditorPlugin::get_vulkan_switch_dialog_text());
		switch_dialog->set_autowrap(true);
		switch_dialog->connect(SceneStringName(confirmed), callable_mp(this, &NextGIStatusPanel::_switch_confirmed));
		add_child(switch_dialog);

		restart_dialog = memnew(ConfirmationDialog);
		restart_dialog->set_title(TTR("Restart Editor"));
		restart_dialog->set_text(TTR("The Windows driver is now Vulkan, and the Direct3D 12 fallback stays on. Restart the editor to apply the change."));
		restart_dialog->set_autowrap(true);
		restart_dialog->set_ok_button_text(TTR("Restart"));
		restart_dialog->connect(SceneStringName(confirmed), callable_mp(this, &NextGIStatusPanel::_restart_confirmed));
		add_child(restart_dialog);
	}
};

bool EditorInspectorPluginNextGI::can_handle(Object *p_object) {
	return Object::cast_to<Environment>(p_object) != nullptr || Object::cast_to<WorldEnvironment>(p_object) != nullptr;
}

void EditorInspectorPluginNextGI::parse_begin(Object *p_object) {
	ERR_FAIL_NULL(p_object);
	add_custom_control(memnew(NextGIStatusPanel));
}
