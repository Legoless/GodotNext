/**************************************************************************/
/*  nextgeo_export_plugin.cpp                                             */
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

#include "nextgeo_export_plugin.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/resource_importer.h"
#include "core/templates/hashfuncs.h"
#include "scene/resources/3d/virtual_geometry_data.h"

static const char *NGM_MANIFEST_SUFFIX = ".ngm_manifest.json";
static const char *STRIP_OPTION = "nextgeo/strip_authored_surfaces";

static Vector<String> _read_manifest_ngm_paths(const String &p_manifest_path) {
	Vector<String> paths;
	const String text = FileAccess::get_file_as_string(p_manifest_path);
	if (text.is_empty()) {
		return paths;
	}
	const Variant parsed = JSON::parse_string(text);
	if (parsed.get_type() != Variant::DICTIONARY) {
		return paths;
	}
	const Dictionary root = parsed;
	const Variant entries_var = root.get("entries", Variant());
	if (entries_var.get_type() != Variant::ARRAY) {
		return paths;
	}
	const Array entries = entries_var;
	for (int i = 0; i < entries.size(); i++) {
		if (entries[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary entry = entries[i];
		const String ngm = entry.get("ngm", String());
		if (!ngm.is_empty()) {
			paths.push_back(ngm);
		}
	}
	return paths;
}

static void _collect_manifests(const String &p_dir, Vector<String> &r_manifests) {
	Ref<DirAccess> dir = DirAccess::open(p_dir);
	if (dir.is_null()) {
		return;
	}
	dir->set_include_hidden(true);
	dir->list_dir_begin();
	for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
		if (name == "." || name == "..") {
			continue;
		}
		const String path = p_dir.path_join(name);
		if (dir->current_is_dir()) {
			if (FileAccess::exists(path.path_join("project.godot"))) {
				continue;
			}
			_collect_manifests(path, r_manifests);
		} else if (name.ends_with(NGM_MANIFEST_SUFFIX)) {
			r_manifests.push_back(path);
		}
	}
	dir->list_dir_end();
}

static bool _features_have_nextgeo(const Vector<String> &p_features) {
	for (const String &feature : p_features) {
		if (feature == "nextgeo") {
			return true;
		}
	}
	return false;
}

static bool _coarse_fallback_is_usable(const Variant &p_fallback) {
	if (p_fallback.get_type() != Variant::ARRAY) {
		return false;
	}
	const Array arrays = p_fallback;
	return arrays.size() == Mesh::ARRAY_MAX && arrays[Mesh::ARRAY_VERTEX].get_type() != Variant::NIL;
}

static bool _append_surface(const Ref<ArrayMesh> &p_dst, const Ref<ArrayMesh> &p_src, int p_index, const Array &p_arrays, const TypedArray<Array> &p_blend_shapes, const Dictionary &p_lods) {
	const int before = p_dst->get_surface_count();
	p_dst->add_surface_from_arrays(p_src->surface_get_primitive_type(p_index), p_arrays, p_blend_shapes, p_lods, p_src->surface_get_format(p_index));
	if (p_dst->get_surface_count() == before) {
		return false;
	}
	const int added = p_dst->get_surface_count() - 1;
	p_dst->surface_set_material(added, p_src->surface_get_material(p_index));
	p_dst->surface_set_name(added, p_src->surface_get_name(p_index));
	return true;
}

String NextGeoExportPlugin::_project_root() {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (settings == nullptr) {
		return String();
	}
	return settings->get_resource_path();
}

bool NextGeoExportPlugin::os_gets_default_nextgeo_tag(const String &p_os) {
	return p_os == "Windows" || p_os == "Linux" || p_os == "macOS";
}

bool NextGeoExportPlugin::should_export_nextgeo_feature(const String &p_os, bool p_strip_option, bool p_has_manifest) {
	return os_gets_default_nextgeo_tag(p_os) && p_strip_option && p_has_manifest;
}

bool NextGeoExportPlugin::custom_features_include_nextgeo(const String &p_custom_features) {
	for (const String &part : p_custom_features.split(",")) {
		if (part.strip_edges() == "nextgeo") {
			return true;
		}
	}
	return false;
}

bool NextGeoExportPlugin::should_strip_resources(const String &p_os, bool p_strip_option, bool p_has_manifest, bool p_features_have_nextgeo) {
	if (!p_strip_option) {
		return false;
	}
	// A hand-added `nextgeo` feature is already in the export feature set. The tag this plugin
	// appends is not: export writes plugin features after customization begins.
	if (p_features_have_nextgeo) {
		return true;
	}
	return should_export_nextgeo_feature(p_os, true, p_has_manifest);
}

bool NextGeoExportPlugin::strip_option_enabled(const Ref<EditorExportPreset> &p_preset) {
	if (p_preset.is_null() || !p_preset->has(STRIP_OPTION)) {
		return true;
	}
	return bool(p_preset->get(STRIP_OPTION));
}

bool NextGeoExportPlugin::directory_has_ngm_manifest(const String &p_root) {
	return !collect_manifest_ngm_paths(p_root).is_empty();
}

Vector<String> NextGeoExportPlugin::collect_manifest_ngm_paths(const String &p_root) {
	Vector<String> manifests;
	if (!p_root.is_empty()) {
		_collect_manifests(p_root, manifests);
	}
	HashSet<String> seen;
	Vector<String> paths;
	for (const String &manifest : manifests) {
		for (const String &ngm : _read_manifest_ngm_paths(manifest)) {
			const String path = ngm.simplify_path();
			if (path.is_empty() || seen.has(path)) {
				continue;
			}
			seen.insert(path);
			paths.push_back(path);
		}
	}
	return paths;
}

Error NextGeoExportPlugin::validate_encryption(const Ref<EditorExportPreset> &p_preset, String &r_error, const String &p_scan_root) {
	r_error = String();
	if (p_preset.is_null() || !p_preset->get_enc_pck()) {
		return OK;
	}
	const String root = p_scan_root.is_empty() ? _project_root() : p_scan_root;
	int count = 0;
	for (const String &path : collect_manifest_ngm_paths(root)) {
		if (FileAccess::exists(path) && p_preset->is_path_encrypted(path)) {
			count++;
		}
	}
	if (count > 0) {
		r_error = vformat("Encrypted PCK would encrypt %d .ngm file(s); add *.ngm to the encryption exclude filters.", count);
		return FAILED;
	}
	return OK;
}

Ref<ArrayMesh> NextGeoExportPlugin::make_stripped_mesh(const Ref<ArrayMesh> &p_mesh) {
#ifndef _3D_DISABLED
	if (p_mesh.is_null() || p_mesh->get_virtual_geometry().is_null()) {
		return Ref<ArrayMesh>();
	}
	const Ref<VirtualGeometryData> data = p_mesh->get_virtual_geometry();
	const PackedByteArray eligibility = data->get_surface_eligibility();
	const TypedArray<Array> coarse = data->get_coarse_fallback();
	bool any_virtualized = false;
	for (int i = 0; i < eligibility.size() && i < p_mesh->get_surface_count(); i++) {
		if (eligibility[i] == 1 && i < coarse.size() && _coarse_fallback_is_usable(coarse[i])) {
			any_virtualized = true;
			break;
		}
	}
	if (!any_virtualized) {
		return Ref<ArrayMesh>();
	}

	Ref<ArrayMesh> stripped;
	stripped.instantiate();
	stripped->set_name(p_mesh->get_name());
	for (int i = 0; i < p_mesh->get_blend_shape_count(); i++) {
		stripped->add_blend_shape(p_mesh->get_blend_shape_name(i));
	}
	stripped->set_blend_shape_mode(p_mesh->get_blend_shape_mode());

	for (int i = 0; i < p_mesh->get_surface_count(); i++) {
		const bool virtualized = i < eligibility.size() && eligibility[i] == 1 && i < coarse.size() && _coarse_fallback_is_usable(coarse[i]);
		// Coarse cuts have no blend shapes. A virtualized surface is ineligible when the
		// source has them, so a non-zero count keeps the authored surface instead of failing.
		if (virtualized && p_mesh->get_blend_shape_count() == 0) {
			if (!_append_surface(stripped, p_mesh, i, coarse[i], TypedArray<Array>(), Dictionary())) {
				return Ref<ArrayMesh>();
			}
		} else if (!_append_surface(stripped, p_mesh, i, p_mesh->surface_get_arrays(i), p_mesh->surface_get_blend_shape_arrays(i), p_mesh->surface_get_lods(i))) {
			return Ref<ArrayMesh>();
		}
	}

	const Ref<ArrayMesh> shadow = p_mesh->get_shadow_mesh();
	if (shadow.is_valid() && shadow != p_mesh) {
		stripped->set_shadow_mesh(shadow);
	}

	Ref<VirtualGeometryData> copy = data->duplicate(true);
	if (copy.is_null()) {
		return Ref<ArrayMesh>();
	}
	copy->set_coarse_fallback(TypedArray<Array>());
	stripped->set_virtual_geometry(copy);
	return stripped;
#else
	(void)p_mesh;
	return Ref<ArrayMesh>();
#endif
}

Ref<Resource> NextGeoExportPlugin::strip_exported_resource(const Ref<Resource> &p_resource) {
	const Ref<ArrayMesh> mesh = p_resource;
	if (mesh.is_null()) {
		return Ref<Resource>();
	}
	return make_stripped_mesh(mesh);
}

bool NextGeoExportPlugin::_strip_this_export(const Ref<EditorExportPlatform> &p_platform, const Vector<String> &p_features) const {
	const String os = p_platform.is_valid() ? p_platform->get_os_name() : String();
	return should_strip_resources(os, strip_option_enabled(get_export_preset()), directory_has_ngm_manifest(_project_root()), _features_have_nextgeo(p_features));
}

void NextGeoExportPlugin::_add_ngm_once(const String &p_path) {
	const String path = p_path.simplify_path();
	if (path.is_empty() || exported_ngm.has(path) || !FileAccess::exists(path)) {
		return;
	}
	exported_ngm.insert(path);
	add_file(path, FileAccess::get_file_as_bytes(path), false);
}

void NextGeoExportPlugin::_add_manifest_ngms(const String &p_manifest_path) {
	if (!FileAccess::exists(p_manifest_path)) {
		return;
	}
	for (const String &ngm : _read_manifest_ngm_paths(p_manifest_path)) {
		_add_ngm_once(ngm);
	}
}

void NextGeoExportPlugin::_export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags) {
	(void)p_features;
	(void)p_debug;
	(void)p_path;
	(void)p_flags;
	exported_ngm.clear();
}

void NextGeoExportPlugin::_export_file(const String &p_path, const String &p_type, const HashSet<String> &p_features) {
	(void)p_type;
	(void)p_features;
	if (FileAccess::exists(p_path + ".import")) {
		ResourceFormatImporter *importer = ResourceFormatImporter::get_singleton();
		if (importer != nullptr) {
			_add_manifest_ngms(importer->get_import_base_path(p_path) + NGM_MANIFEST_SUFFIX);
		}
	}
	// save_to_file meshes keep the sidecar beside the saved resource (`mesh.res.ngm`).
	_add_ngm_once(p_path + ".ngm");
}

PackedStringArray NextGeoExportPlugin::_get_export_features(const Ref<EditorExportPlatform> &p_platform, bool p_debug) const {
	(void)p_debug;
	PackedStringArray features;
	if (p_platform.is_null()) {
		return features;
	}
	const Ref<EditorExportPreset> preset = get_export_preset();
	if (preset.is_valid() && custom_features_include_nextgeo(preset->get_custom_features())) {
		return features;
	}
	if (should_export_nextgeo_feature(p_platform->get_os_name(), strip_option_enabled(get_export_preset()), directory_has_ngm_manifest(_project_root()))) {
		features.push_back("nextgeo");
	}
	return features;
}

void NextGeoExportPlugin::_get_export_options(const Ref<EditorExportPlatform> &p_export_platform, List<EditorExportPlatform::ExportOption> *r_options) const {
	(void)p_export_platform;
	r_options->push_back(EditorExportPlatform::ExportOption(PropertyInfo(Variant::BOOL, STRIP_OPTION), true));
}

String NextGeoExportPlugin::_get_export_option_warning(const Ref<EditorExportPlatform> &p_export_platform, const String &p_option_name) const {
	(void)p_export_platform;
	if (p_option_name != STRIP_OPTION) {
		return String();
	}
	String error;
	if (validate_encryption(get_export_preset(), error) != OK) {
		return error;
	}
	return String();
}

uint64_t NextGeoExportPlugin::_get_customization_configuration_hash() const {
	const bool strip = strip_option_enabled(get_export_preset());
	uint32_t hash = hash_murmur3_one_32(PLUGIN_VERSION);
	hash = hash_murmur3_one_32(strip ? 1u : 0u, hash);
	return hash_fmix32(hash);
}

bool NextGeoExportPlugin::_begin_customize_resources(const Ref<EditorExportPlatform> &p_platform, const Vector<String> &p_features) {
	strip_resources = _strip_this_export(p_platform, p_features);
	return strip_resources;
}

Ref<Resource> NextGeoExportPlugin::_customize_resource(const Ref<Resource> &p_resource, const String &p_path) {
	(void)p_path;
	if (!strip_resources) {
		return Ref<Resource>();
	}
	return strip_exported_resource(p_resource);
}

void NextGeoExportPlugin::_end_customize_resources() {
	strip_resources = false;
}
