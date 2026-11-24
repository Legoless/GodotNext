/**************************************************************************/
/*  dlss_editor_plugin.h                                                  */
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

#include "dlss_export_plugin.h"

#include "editor/plugins/editor_plugin.h"

class DLSSEditorPlugin : public EditorPlugin {
	GDCLASS(DLSSEditorPlugin, EditorPlugin);

private:
	Ref<DLSSEditorExportPlugin> export_plugin;
	bool export_plugin_registered = false;

	void _register_export_plugin();
	void _unregister_export_plugin();
	static void _ensure_project_id();

protected:
	void _notification(int p_what);

public:
	static constexpr const char *PROJECT_ID_SETTING = "rendering/scaling_3d/dlss/project_id";

	static String get_attribution_line();
	static String generate_project_id();
	static bool is_valid_project_id_format(const String &p_project_id);

	virtual String get_plugin_name() const override { return "DLSS"; }

	DLSSEditorPlugin();
	~DLSSEditorPlugin();
};
