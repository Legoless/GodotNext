/**************************************************************************/
/*  dlss_export_plugin.cpp                                                */
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

#include "dlss_export_plugin.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/version.h"
#include "editor/export/editor_export_platform_pc.h"
#include "editor/file_system/editor_paths.h"

static String _extract_path_field(const Dictionary &p_root, const String &p_key) {
	if (!p_root.has(p_key)) {
		return String();
	}
	const Variant val = p_root[p_key];
	if (val.get_type() == Variant::STRING) {
		return String(val).strip_edges();
	}
	if (val.get_type() == Variant::DICTIONARY) {
		const Dictionary dict = val;
		if (dict.has("name")) {
			return String(dict["name"]).strip_edges();
		}
		if (dict.has("path")) {
			return String(dict["path"]).strip_edges();
		}
	}
	return String();
}

bool DLSSEditorExportPlugin::_is_safe_basename(const String &p_name) {
	if (p_name.is_empty() || p_name == "." || p_name == "..") {
		return false;
	}
	if (p_name.contains_char('/') || p_name.contains_char('\\') || p_name.contains("..")) {
		return false;
	}
	return true;
}

bool DLSSEditorExportPlugin::_is_valid_sha256_hex(const String &p_hex) {
	if (p_hex.length() != 64) {
		return false;
	}
	for (int i = 0; i < p_hex.length(); i++) {
		const char32_t c = p_hex[i];
		const bool is_digit = (c >= '0' && c <= '9');
		const bool is_lower_hex = (c >= 'a' && c <= 'f');
		const bool is_upper_hex = (c >= 'A' && c <= 'F');
		if (!is_digit && !is_lower_hex && !is_upper_hex) {
			return false;
		}
	}
	return true;
}

bool DLSSEditorExportPlugin::_is_excluded_runtime_name(const String &p_name) {
	return p_name.to_lower().contains("nvperf");
}

String DLSSEditorExportPlugin::_resolve_aux_file_path(const String &p_entry, const String &p_template_dir) {
	const String trimmed = p_entry.strip_edges();
	if (trimmed.is_empty()) {
		return String();
	}
	if (trimmed.is_absolute_path()) {
		return trimmed;
	}
	if (trimmed.contains("..")) {
		return String();
	}
	if (p_template_dir.is_empty()) {
		return trimmed;
	}
	return p_template_dir.path_join(trimmed);
}

int DLSSEditorExportPlugin::_get_export_mode(const Ref<EditorExportPreset> &p_preset) {
	if (p_preset.is_null() || !p_preset->has(EXPORT_OPTION_NAME)) {
		return EXPORT_MODE_AUTO;
	}
	return int(p_preset->get(EXPORT_OPTION_NAME));
}

String DLSSEditorExportPlugin::_get_project_id(const Ref<EditorExportPreset> &p_preset) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps == nullptr || !ps->has_setting(PROJECT_ID_SETTING)) {
		return String();
	}
	if (p_preset.is_valid()) {
		return String(p_preset->get_project_setting(PROJECT_ID_SETTING)).strip_edges();
	}
	return String(ps->get_setting(PROJECT_ID_SETTING)).strip_edges();
}

String DLSSEditorExportPlugin::_resolve_template_dir(const Ref<EditorExportPlatform> &p_export_platform, const Ref<EditorExportPreset> &p_preset, bool p_debug) {
	if (p_export_platform.is_null() || p_preset.is_null()) {
		return String();
	}

	const String custom_key = p_debug ? "custom_template/debug" : "custom_template/release";
	String template_path;
	if (p_preset->has(custom_key)) {
		template_path = String(p_preset->get(custom_key)).strip_edges();
	}

	if (template_path.is_empty()) {
		const EditorExportPlatformPC *pc_platform = Object::cast_to<EditorExportPlatformPC>(p_export_platform.ptr());
		if (pc_platform != nullptr && EditorPaths::get_singleton() != nullptr) {
			const String arch = p_preset->has("binary_format/architecture") ? String(p_preset->get("binary_format/architecture")) : String();
			const String template_file_name = pc_platform->get_template_file_name(p_debug ? "debug" : "release", arch);
			if (!template_file_name.is_empty()) {
				const String candidate = EditorPaths::get_singleton()->get_export_templates_dir().path_join(GODOT_VERSION_FULL_CONFIG).path_join(template_file_name);
				if (FileAccess::exists(candidate)) {
					template_path = candidate;
				}
			}
		}
	}

	if (template_path.is_empty()) {
		return String();
	}
	return template_path.get_base_dir();
}

Vector<String> DLSSEditorExportPlugin::_collect_candidate_template_dirs(const Ref<EditorExportPlatform> &p_export_platform, const Ref<EditorExportPreset> &p_preset) {
	Vector<String> dirs;
	const bool debug_modes[2] = { false, true };
	for (bool is_debug : debug_modes) {
		const String dir = _resolve_template_dir(p_export_platform, p_preset, is_debug);
		if (!dir.is_empty() && !dirs.has(dir)) {
			dirs.push_back(dir);
		}
	}
	return dirs;
}

bool DLSSEditorExportPlugin::supports_platform(const Ref<EditorExportPlatform> &p_export_platform) const {
	if (p_export_platform.is_null()) {
		return false;
	}
	const String name = p_export_platform->get_name();
	return name == "Windows Desktop" || name == "Linux";
}

bool DLSSEditorExportPlugin::should_export_dlss(int p_export_mode, bool p_manifest_exists, const String &p_project_id) {
	if (p_export_mode == EXPORT_MODE_NO) {
		return false;
	}
	if (p_export_mode == EXPORT_MODE_YES) {
		return true;
	}
	return p_manifest_exists && !p_project_id.strip_edges().is_empty();
}

Error DLSSEditorExportPlugin::parse_manifest(const String &p_json_text, DLSSRuntimeManifest &r_manifest, String &r_error) {
	r_manifest = DLSSRuntimeManifest();
	r_error = String();

	if (p_json_text.strip_edges().is_empty()) {
		r_error = TTR("DLSS runtime manifest is empty.");
		return ERR_PARSE_ERROR;
	}

	JSON json;
	const Error err = json.parse(p_json_text);
	if (err != OK) {
		r_error = vformat(TTR("Failed to parse DLSS runtime manifest JSON at line %d: %s"), json.get_error_line(), json.get_error_message());
		return ERR_PARSE_ERROR;
	}

	const Variant root_var = json.get_data();
	if (root_var.get_type() != Variant::DICTIONARY) {
		r_error = TTR("DLSS runtime manifest root must be a JSON object.");
		return ERR_PARSE_ERROR;
	}

	const Dictionary root = root_var;
	r_manifest.sdk_tag = String(root.get("sdk_tag", String())).strip_edges();
	r_manifest.pinned_tag = String(root.get("pinned_tag", String())).strip_edges();
	r_manifest.variant = String(root.get("variant", String())).strip_edges().to_lower();
	r_manifest.license = _extract_path_field(root, "license");
	r_manifest.notices = _extract_path_field(root, "notices");

	if (!root.has("files") || root["files"].get_type() != Variant::ARRAY) {
		r_error = TTR("DLSS runtime manifest is missing a valid 'files' array.");
		return ERR_INVALID_DATA;
	}

	const Array files_arr = root["files"];
	for (int i = 0; i < files_arr.size(); i++) {
		if (files_arr[i].get_type() != Variant::DICTIONARY) {
			r_error = vformat(TTR("DLSS runtime manifest 'files[%d]' must be a JSON object."), i);
			return ERR_INVALID_DATA;
		}
		const Dictionary entry_dict = files_arr[i];
		DLSSRuntimeManifest::FileEntry entry;
		entry.name = String(entry_dict.get("name", String())).strip_edges();
		if (entry.name.is_empty() && entry_dict.has("basename")) {
			entry.name = String(entry_dict.get("basename", String())).strip_edges();
		}
		if (entry.name.is_empty()) {
			r_error = vformat(TTR("DLSS runtime manifest 'files[%d]' is missing a non-empty 'name'."), i);
			return ERR_INVALID_DATA;
		}
		entry.sha256 = String(entry_dict.get("sha256", String())).strip_edges().to_lower();
		if (entry_dict.has("variant")) {
			entry.variant = String(entry_dict.get("variant", String())).strip_edges().to_lower();
		} else {
			entry.variant = r_manifest.variant;
		}
		r_manifest.files.push_back(entry);
	}

	return OK;
}

bool DLSSEditorExportPlugin::validate_manifest_for_export(const DLSSRuntimeManifest &p_manifest, const String &p_template_dir, String &r_error) {
	r_error = String();

	if (p_manifest.variant != "rel") {
		r_error = vformat(
				TTR("DLSS export blocked: runtime manifest variant is \"%s\" (only \"rel\" release runtimes may be exported; dev runtimes must never be distributed)."),
				p_manifest.variant);
		return false;
	}

	if (p_manifest.files.is_empty()) {
		r_error = TTR("DLSS export blocked: runtime manifest lists no runtime files.");
		return false;
	}

	for (const DLSSRuntimeManifest::FileEntry &entry : p_manifest.files) {
		if (!_is_safe_basename(entry.name)) {
			r_error = vformat(TTR("DLSS export blocked: invalid runtime filename \"%s\" in manifest."), entry.name);
			return false;
		}
		if (_is_excluded_runtime_name(entry.name)) {
			r_error = vformat(TTR("DLSS export blocked: excluded runtime \"%s\" is not permitted for redistribution."), entry.name);
			return false;
		}
		if (entry.variant != "rel") {
			r_error = vformat(
					TTR("DLSS export blocked: runtime \"%s\" has variant \"%s\" (only \"rel\" release runtimes may be exported)."),
					entry.name, entry.variant);
			return false;
		}
		if (!_is_valid_sha256_hex(entry.sha256)) {
			r_error = vformat(TTR("DLSS export blocked: runtime \"%s\" has a missing or invalid SHA-256 digest in manifest."), entry.name);
			return false;
		}

		const String file_path = p_template_dir.path_join(entry.name);
		if (!FileAccess::exists(file_path)) {
			r_error = vformat(TTR("DLSS export blocked: listed runtime \"%s\" was not found in template directory \"%s\"."), entry.name, p_template_dir);
			return false;
		}

		const String actual_sha256 = FileAccess::get_sha256(file_path).to_lower();
		if (actual_sha256.is_empty() || actual_sha256 != entry.sha256.to_lower()) {
			r_error = vformat(
					TTR("DLSS export blocked: SHA-256 mismatch for runtime \"%s\" (expected %s, got %s)."),
					entry.name, entry.sha256.to_lower(), actual_sha256);
			return false;
		}
	}

	if (p_manifest.license.is_empty()) {
		r_error = TTR("DLSS export blocked: runtime manifest is missing the required 'license' file entry.");
		return false;
	}
	const String license_path = _resolve_aux_file_path(p_manifest.license, p_template_dir);
	if (license_path.is_empty() || _is_excluded_runtime_name(license_path.get_file()) || !FileAccess::exists(license_path)) {
		r_error = vformat(TTR("DLSS export blocked: required license file \"%s\" was not found."), p_manifest.license);
		return false;
	}

	if (p_manifest.notices.is_empty()) {
		r_error = TTR("DLSS export blocked: runtime manifest is missing the required 'notices' file entry.");
		return false;
	}
	const String notices_path = _resolve_aux_file_path(p_manifest.notices, p_template_dir);
	if (notices_path.is_empty() || _is_excluded_runtime_name(notices_path.get_file()) || !FileAccess::exists(notices_path)) {
		r_error = vformat(TTR("DLSS export blocked: required notices file \"%s\" was not found."), p_manifest.notices);
		return false;
	}

	return true;
}

Error DLSSEditorExportPlugin::copy_validated_manifest_files(const DLSSRuntimeManifest &p_manifest, const String &p_template_dir, const String &p_export_dir, String &r_error) {
	if (!validate_manifest_for_export(p_manifest, p_template_dir, r_error)) {
		return ERR_INVALID_DATA;
	}

	if (p_export_dir.is_empty() || !DirAccess::exists(p_export_dir)) {
		r_error = vformat(TTR("DLSS export failed: destination directory \"%s\" does not exist."), p_export_dir);
		return ERR_FILE_BAD_PATH;
	}

	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	if (da.is_null()) {
		r_error = TTR("DLSS export failed: could not access filesystem.");
		return ERR_CANT_OPEN;
	}

	// Copy each rel runtime byte-for-byte via DirAccess::copy so Authenticode signatures stay intact.
	for (const DLSSRuntimeManifest::FileEntry &entry : p_manifest.files) {
		const String src = p_template_dir.path_join(entry.name);
		const String dst = p_export_dir.path_join(entry.name);
		const Error err = da->copy(src, dst);
		if (err != OK) {
			r_error = vformat(TTR("DLSS export failed: could not copy runtime \"%s\" to \"%s\" (error %d)."), src, dst, err);
			return err;
		}
	}

	const String license_src = _resolve_aux_file_path(p_manifest.license, p_template_dir);
	const String license_dst = p_export_dir.path_join(license_src.get_file());
	Error err = da->copy(license_src, license_dst);
	if (err != OK) {
		r_error = vformat(TTR("DLSS export failed: could not copy license file \"%s\" to \"%s\" (error %d)."), license_src, license_dst, err);
		return err;
	}

	const String notices_src = _resolve_aux_file_path(p_manifest.notices, p_template_dir);
	const String notices_dst = p_export_dir.path_join(notices_src.get_file());
	err = da->copy(notices_src, notices_dst);
	if (err != OK) {
		r_error = vformat(TTR("DLSS export failed: could not copy notices file \"%s\" to \"%s\" (error %d)."), notices_src, notices_dst, err);
		return err;
	}

	return OK;
}

String DLSSEditorExportPlugin::get_distributor_duty_warning() {
	return TTR("Distributing NVIDIA DLSS runtimes makes you the distributor under the NVIDIA RTX SDKs License and DLSS SDK Supplement. "
			   "Before commercial or public release, verify your obligations under License §2(c) (protective end-user terms) and §2(d) (non-compliance notice), "
			   "and Supplement §1 (identification), §3 (cloud/non-vGPU restriction), §4 (pre-release notification at https://developer.nvidia.com/sw-notification), "
			   "§6.1 (over-the-air updates), §7.1(b) (binding NVIDIA Marks attribution), and §7.2(c) (trademark/editor distribution approval).");
}

void DLSSEditorExportPlugin::_get_export_options(const Ref<EditorExportPlatform> &p_export_platform, List<EditorExportPlatform::ExportOption> *r_options) const {
	if (!supports_platform(p_export_platform)) {
		return;
	}
	r_options->push_back(EditorExportPlatform::ExportOption(
			PropertyInfo(Variant::INT, EXPORT_OPTION_NAME, PROPERTY_HINT_ENUM, "Auto,Yes,No"),
			EXPORT_MODE_AUTO));
}

String DLSSEditorExportPlugin::_get_export_option_warning(const Ref<EditorExportPlatform> &p_export_platform, const String &p_option_name) const {
	if (p_option_name != EXPORT_OPTION_NAME || !supports_platform(p_export_platform)) {
		return String();
	}

	const Ref<EditorExportPreset> preset = get_export_preset();
	const int export_mode = _get_export_mode(preset);
	if (export_mode == EXPORT_MODE_NO) {
		return String();
	}

	const Vector<String> template_dirs = _collect_candidate_template_dirs(p_export_platform, preset);
	bool any_manifest = false;
	for (const String &dir : template_dirs) {
		if (FileAccess::exists(dir.path_join(MANIFEST_FILENAME))) {
			any_manifest = true;
			break;
		}
	}

	const String project_id = _get_project_id(preset);
	if (!should_export_dlss(export_mode, any_manifest, project_id)) {
		return String();
	}

	String warning = get_distributor_duty_warning();
	if (export_mode == EXPORT_MODE_YES && !any_manifest) {
		warning += "\n" + TTR("No dlss_runtime.json manifest was found next to the export template; no DLSS runtime files will be copied and the exported build will report RUNTIME_MISSING if DLSS is requested.");
	}
	return warning;
}

bool DLSSEditorExportPlugin::_blocks_export(const Ref<EditorExportPlatform> &p_export_platform, const Ref<EditorExportPreset> &p_preset, String &r_error) const {
	r_error = String();
	if (!supports_platform(p_export_platform) || p_preset.is_null()) {
		return false;
	}

	const int export_mode = _get_export_mode(p_preset);
	if (export_mode == EXPORT_MODE_NO) {
		return false;
	}

	const String project_id = _get_project_id(p_preset);
	const Vector<String> template_dirs = _collect_candidate_template_dirs(p_export_platform, p_preset);

	for (const String &template_dir : template_dirs) {
		const String manifest_path = template_dir.path_join(MANIFEST_FILENAME);
		const bool manifest_exists = FileAccess::exists(manifest_path);
		if (!manifest_exists) {
			continue;
		}
		if (!should_export_dlss(export_mode, true, project_id)) {
			continue;
		}

		const String json_text = FileAccess::get_file_as_string(manifest_path);
		DLSSRuntimeManifest manifest;
		String parse_err;
		if (parse_manifest(json_text, manifest, parse_err) != OK) {
			r_error = parse_err;
			return true;
		}

		String validate_err;
		if (!validate_manifest_for_export(manifest, template_dir, validate_err)) {
			r_error = validate_err;
			return true;
		}
	}

	return false;
}

void DLSSEditorExportPlugin::_export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags) {
	(void)p_features;
	(void)p_flags;
	export_active = false;
	export_debug = p_debug;
	export_path = p_path;
	export_template_dir = String();

	const Ref<EditorExportPlatform> platform = get_export_platform();
	const Ref<EditorExportPreset> preset = get_export_preset();
	if (platform.is_null() || preset.is_null() || !supports_platform(platform)) {
		return;
	}

	const int export_mode = _get_export_mode(preset);
	if (export_mode == EXPORT_MODE_NO) {
		return;
	}

	const String template_dir = _resolve_template_dir(platform, preset, p_debug);
	const String manifest_path = template_dir.is_empty() ? String() : template_dir.path_join(MANIFEST_FILENAME);
	const bool manifest_exists = !manifest_path.is_empty() && FileAccess::exists(manifest_path);
	const String project_id = _get_project_id(preset);

	if (!should_export_dlss(export_mode, manifest_exists, project_id)) {
		return;
	}

	if (!manifest_exists) {
		platform->add_message(
				EditorExportPlatform::EXPORT_MESSAGE_WARNING,
				TTR("DLSS Export"),
				TTR("DLSS export is enabled, but no dlss_runtime.json manifest was found next to the export template. No DLSS runtime files were copied; the exported build will report RUNTIME_MISSING if DLSS is requested."));
		return;
	}

	export_active = true;
	export_template_dir = template_dir;
	platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_WARNING, TTR("DLSS Export"), get_distributor_duty_warning());
}

void DLSSEditorExportPlugin::_export_end() {
	const bool active = export_active;
	const String target_path = export_path;
	const String template_dir = export_template_dir;

	export_active = false;
	export_debug = false;
	export_path = String();
	export_template_dir = String();

	if (!active || target_path.is_empty() || template_dir.is_empty()) {
		return;
	}

	if (!FileAccess::exists(target_path)) {
		return;
	}

	const Ref<EditorExportPlatform> platform = get_export_platform();
	const String manifest_path = template_dir.path_join(MANIFEST_FILENAME);
	if (!FileAccess::exists(manifest_path)) {
		if (platform.is_valid()) {
			platform->add_message(
					EditorExportPlatform::EXPORT_MESSAGE_WARNING,
					TTR("DLSS Export"),
					TTR("DLSS runtime manifest disappeared before export completion; no runtime files were copied."));
		}
		return;
	}

	DLSSRuntimeManifest manifest;
	String err_msg;
	if (parse_manifest(FileAccess::get_file_as_string(manifest_path), manifest, err_msg) != OK) {
		if (platform.is_valid()) {
			platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("DLSS Export"), err_msg);
		}
		return;
	}

	const String dst_dir = target_path.get_base_dir();
	if (copy_validated_manifest_files(manifest, template_dir, dst_dir, err_msg) != OK) {
		if (platform.is_valid()) {
			platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, TTR("DLSS Export"), err_msg);
		}
	}
}
