/**************************************************************************/
/*  dlss_editor_plugin.cpp                                                */
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

#include "dlss_editor_plugin.h"

#include "core/config/project_settings.h"
#include "core/crypto/crypto_core.h"
#include "editor/export/editor_export.h"
#include "editor/gui/editor_about.h"
#include "editor/gui/editor_toaster.h"

String DLSSEditorPlugin::get_attribution_line() {
	return "NVIDIA, the NVIDIA logo, and NVIDIA DLSS are trademarks and/or registered trademarks of NVIDIA Corporation in the U.S. and other countries.";
}

String DLSSEditorPlugin::generate_project_id() {
	uint8_t bytes[16] = {};
	const Error err = CryptoCore::generate_random(bytes, sizeof(bytes));
	ERR_FAIL_COND_V_MSG(err != OK, String(), "DLSS: Failed to generate random bytes for project_id.");

	// Format as an RFC 4122 version 4 UUID (8-4-4-4-12 lowercase hex).
	bytes[6] = (bytes[6] & 0x0fu) | 0x40u;
	bytes[8] = (bytes[8] & 0x3fu) | 0x80u;

	const String hex = String::hex_encode_buffer(bytes, sizeof(bytes)).to_lower();
	return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" + hex.substr(20, 12);
}

bool DLSSEditorPlugin::is_valid_project_id_format(const String &p_project_id) {
	if (p_project_id.length() != 36) {
		return false;
	}
	for (int i = 0; i < 36; i++) {
		const char32_t c = p_project_id[i];
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (c != '-') {
				return false;
			}
		} else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
			return false;
		}
	}
	return true;
}

void DLSSEditorPlugin::_register_export_plugin() {
	if (export_plugin_registered || EditorExport::get_singleton() == nullptr) {
		return;
	}
	if (export_plugin.is_null()) {
		export_plugin.instantiate();
	}
	add_export_plugin(export_plugin);
	export_plugin_registered = true;
}

void DLSSEditorPlugin::_unregister_export_plugin() {
	if (!export_plugin_registered) {
		return;
	}
	if (export_plugin.is_valid() && EditorExport::get_singleton() != nullptr) {
		remove_export_plugin(export_plugin);
	}
	export_plugin_registered = false;
}

void DLSSEditorPlugin::_ensure_project_id() {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps == nullptr) {
		return;
	}

	String current_id;
	if (ps->has_setting(PROJECT_ID_SETTING)) {
		current_id = String(ps->get_setting(PROJECT_ID_SETTING)).strip_edges();
	}
	if (!current_id.is_empty()) {
		return;
	}

	const String new_id = generate_project_id();
	if (new_id.is_empty()) {
		return;
	}

	ps->set_setting(PROJECT_ID_SETTING, new_id);
	const Error save_err = ps->save();
	if (save_err != OK) {
		WARN_PRINT(vformat("DLSS: Generated project_id '%s', but failed to save project settings (error %d).", new_id, save_err));
	}

	if (EditorToaster::get_singleton() != nullptr) {
		EditorToaster::get_singleton()->popup_str(
				vformat(TTR("Generated DLSS Project ID: %s (saved to Project Settings -> rendering/scaling_3d/dlss/project_id)."), new_id),
				EditorToaster::SEVERITY_INFO);
	}
}

void DLSSEditorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			_register_export_plugin();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			_unregister_export_plugin();
		} break;
		case NOTIFICATION_READY: {
			_ensure_project_id();
		} break;
	}
}

DLSSEditorPlugin::DLSSEditorPlugin() {
	EditorAbout::add_attribution_line(get_attribution_line());
	_register_export_plugin();
}

DLSSEditorPlugin::~DLSSEditorPlugin() {
	_unregister_export_plugin();
	export_plugin.unref();
}
