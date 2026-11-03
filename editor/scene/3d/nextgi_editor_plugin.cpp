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
#include "core/object/callable_mp.h"
#include "editor/editor_node.h"
#include "editor/settings/editor_settings.h"
#include "scene/gui/dialogs.h"
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

String NextGIEditorPlugin::get_vulkan_switch_dialog_title() {
	return TTR("Use Vulkan on Windows for hardware RT");
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
}
