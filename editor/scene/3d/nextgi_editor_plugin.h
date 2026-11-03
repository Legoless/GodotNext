/**************************************************************************/
/*  nextgi_editor_plugin.h                                                */
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

#include "editor/plugins/editor_plugin.h"

class ConfirmationDialog;

// One-time Vulkan switch for existing d3d12 projects. #34 adds the inspector status panel to this plugin.
class NextGIEditorPlugin : public EditorPlugin {
	GDCLASS(NextGIEditorPlugin, EditorPlugin);

	bool nextgi_was_enabled = false;

	ConfirmationDialog *vulkan_switch_dialog = nullptr;
	ConfirmationDialog *restart_dialog = nullptr;

	void _on_project_settings_changed();
	void _offer_vulkan_switch();
	void _vulkan_switch_confirmed();
	void _restart_confirmed();

protected:
	void _notification(int p_what);

public:
	// True only on a false-to-true flip of nextgi/enabled, while Windows builds use d3d12 and this
	// project has not been asked before. The editor's host OS does not matter: driver.windows is
	// what a Windows export runs.
	static bool should_offer_vulkan_switch(bool p_was_enabled, bool p_is_enabled, const String &p_driver_windows, bool p_already_offered);
	// Sets driver.windows to vulkan, keeps fallback_to_d3d12, and saves project.godot.
	static void apply_vulkan_switch();

	static String get_vulkan_switch_dialog_title();
	static String get_vulkan_switch_dialog_text();

	virtual String get_plugin_name() const override { return "NextGI"; }

	NextGIEditorPlugin();
};
