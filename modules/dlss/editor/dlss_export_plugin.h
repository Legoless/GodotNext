/**************************************************************************/
/*  dlss_export_plugin.h                                                  */
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

#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "editor/export/editor_export_plugin.h"

struct DLSSRuntimeManifest {
	struct FileEntry {
		String name;
		String sha256;
		String variant;
	};

	String sdk_tag;
	String pinned_tag;
	String variant;
	String license;
	String notices;
	Vector<FileEntry> files;
};

class DLSSEditorExportPlugin : public EditorExportPlugin {
	GDCLASS(DLSSEditorExportPlugin, EditorExportPlugin);

public:
	enum ExportMode {
		EXPORT_MODE_AUTO = 0,
		EXPORT_MODE_YES = 1,
		EXPORT_MODE_NO = 2,
	};

	static constexpr const char *EXPORT_OPTION_NAME = "application/export_dlss";
	static constexpr const char *MANIFEST_FILENAME = "dlss_runtime.json";
	static constexpr const char *PROJECT_ID_SETTING = "rendering/scaling_3d/dlss/project_id";

private:
	bool export_active = false;
	bool export_debug = false;
	String export_path;
	String export_template_dir;

	static String _resolve_template_dir(const Ref<EditorExportPlatform> &p_export_platform, const Ref<EditorExportPreset> &p_preset, bool p_debug);
	static Vector<String> _collect_candidate_template_dirs(const Ref<EditorExportPlatform> &p_export_platform, const Ref<EditorExportPreset> &p_preset);
	static String _resolve_aux_file_path(const String &p_entry, const String &p_template_dir);
	static bool _is_safe_basename(const String &p_name);
	static bool _is_valid_sha256_hex(const String &p_hex);
	static bool _is_excluded_runtime_name(const String &p_name);
	static int _get_export_mode(const Ref<EditorExportPreset> &p_preset);
	static String _get_project_id(const Ref<EditorExportPreset> &p_preset);

protected:
	virtual void _get_export_options(const Ref<EditorExportPlatform> &p_export_platform, List<EditorExportPlatform::ExportOption> *r_options) const override;
	virtual String _get_export_option_warning(const Ref<EditorExportPlatform> &p_export_platform, const String &p_option_name) const override;
	virtual void _export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags) override;
	virtual void _export_end() override;

public:
	virtual String get_name() const override { return "DLSSEditorExportPlugin"; }
	virtual bool supports_platform(const Ref<EditorExportPlatform> &p_export_platform) const override;
	virtual bool _blocks_export(const Ref<EditorExportPlatform> &p_export_platform, const Ref<EditorExportPreset> &p_preset, String &r_error) const override;

	static bool should_export_dlss(int p_export_mode, bool p_manifest_exists, const String &p_project_id);
	static Error parse_manifest(const String &p_json_text, DLSSRuntimeManifest &r_manifest, String &r_error);
	static bool validate_manifest_for_export(const DLSSRuntimeManifest &p_manifest, const String &p_template_dir, String &r_error);
	static Error copy_validated_manifest_files(const DLSSRuntimeManifest &p_manifest, const String &p_template_dir, const String &p_export_dir, String &r_error);
	static String get_distributor_duty_warning();
};
