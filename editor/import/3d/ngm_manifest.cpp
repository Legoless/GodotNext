/**************************************************************************/
/*  ngm_manifest.cpp                                                      */
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

#include "ngm_manifest.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/resource_importer.h"
#include "core/templates/hash_set.h"
#include "scene/resources/3d/virtual_geometry_data.h"

static const char *NGM_MANIFEST_SUFFIX = ".ngm_manifest.json";

String ngm_embedded_path(const String &p_base_path, const String &p_key) {
	return p_base_path + "-" + p_key.md5_text().substr(0, 16) + ".ngm";
}

static String _manifest_path(const String &p_base_path) {
	return p_base_path + NGM_MANIFEST_SUFFIX;
}

static bool _is_imported_sidecar(const String &p_path) {
	// `res://.godot/imported/` is `//.godot`, so the slash before `.godot` is not required.
	return p_path.contains(".godot/imported/");
}

static Array _read_entries(const String &p_manifest_path) {
	if (!FileAccess::exists(p_manifest_path)) {
		return Array();
	}
	const String text = FileAccess::get_file_as_string(p_manifest_path);
	Ref<JSON> json;
	json.instantiate();
	const Error err = json->parse(text);
	if (err != OK) {
		WARN_PRINT(vformat("Could not parse virtual geometry manifest '%s'.", p_manifest_path));
		return Array();
	}
	const Dictionary root = json->get_data();
	const Variant entries = root.get("entries", Variant());
	if (entries.get_type() != Variant::ARRAY) {
		return Array();
	}
	return entries;
}

static HashSet<String> _entry_paths(const Array &p_entries) {
	HashSet<String> paths;
	for (int i = 0; i < p_entries.size(); i++) {
		if (p_entries[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary entry = p_entries[i];
		const String path = entry.get("ngm", String());
		if (!path.is_empty()) {
			paths.insert(path.simplify_path());
		}
	}
	return paths;
}

static void _delete_stale(const Array &p_old_entries, const HashSet<String> &p_keep, bool p_imported_only) {
	for (int i = 0; i < p_old_entries.size(); i++) {
		if (p_old_entries[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary entry = p_old_entries[i];
		const String path = String(entry.get("ngm", String()));
		if (path.is_empty()) {
			continue;
		}
		if (p_imported_only && !_is_imported_sidecar(path)) {
			continue;
		}
		if (p_keep.has(path.simplify_path())) {
			continue;
		}
		DirAccess::remove_absolute(path);
	}
}

Error ngm_build_mesh(const Ref<ImporterMesh> &p_mesh, const String &p_ngm_path, const String &p_key, bool p_strip_authored, Array &r_entries, bool p_skinned) {
	ERR_FAIL_COND_V(p_mesh.is_null(), ERR_INVALID_PARAMETER);
	const Error err = p_mesh->build_virtual_geometry(p_ngm_path, p_skinned);
	if (err != OK) {
		WARN_PRINT(vformat("Virtual geometry build failed for '%s' (error %d). The mesh will import without virtual geometry.", p_key, err));
		return OK;
	}
	const Ref<VirtualGeometryData> data = p_mesh->get_virtual_geometry();
	if (data.is_null()) {
		return OK;
	}
	if (p_strip_authored) {
		p_mesh->strip_authored_surfaces_to_coarse();
	}
	const PackedByteArray sha = data->get_ngm_sha256();
	Dictionary entry;
	entry["key"] = p_key;
	entry["ngm"] = p_ngm_path;
	entry["sha256"] = String::hex_encode_buffer(sha.ptr(), sha.size());
	entry["source_triangles"] = data->get_source_triangle_count();
	entry["source_vertex_index_bytes"] = data->get_source_vertex_index_bytes();
	r_entries.push_back(entry);
	return OK;
}

void ngm_manifest_commit(const String &p_base_path, const String &p_source, const Array &p_entries, List<String> *r_gen_files, bool p_generate) {
	const String manifest_path = _manifest_path(p_base_path);
	const bool existed = FileAccess::exists(manifest_path);
	if (!p_generate && !existed && p_entries.is_empty()) {
		return;
	}

	const Array old_entries = _read_entries(manifest_path);
	_delete_stale(old_entries, _entry_paths(p_entries), false);

	if (!p_generate && p_entries.is_empty()) {
		DirAccess::remove_absolute(manifest_path);
		return;
	}

	Dictionary root;
	root["version"] = 1;
	root["source"] = p_source;
	root["entries"] = p_entries;
	DirAccess::make_dir_recursive_absolute(manifest_path.get_base_dir());
	Ref<FileAccess> file = FileAccess::open(manifest_path, FileAccess::WRITE);
	if (file.is_null()) {
		WARN_PRINT(vformat("Could not write virtual geometry manifest '%s'.", manifest_path));
		return;
	}
	file->store_string(JSON::stringify(root, "", true));
	file->store_8('\n');

	if (r_gen_files) {
		for (int i = 0; i < p_entries.size(); i++) {
			if (p_entries[i].get_type() != Variant::DICTIONARY) {
				continue;
			}
			const Dictionary entry = p_entries[i];
			const String ngm = entry.get("ngm", String());
			if (!ngm.is_empty()) {
				r_gen_files->push_back(ngm);
			}
		}
		r_gen_files->push_back(manifest_path);
	}
}

void ngm_delete_imported_sidecars(const String &p_source_file) {
	if (!FileAccess::exists(p_source_file + ".import")) {
		return;
	}
	List<String> internal_paths;
	ResourceFormatImporter::get_singleton()->get_internal_resource_path_list(p_source_file, &internal_paths);
	for (const String &internal : internal_paths) {
		const String manifest_path = internal.get_basename() + NGM_MANIFEST_SUFFIX;
		if (!FileAccess::exists(manifest_path)) {
			continue;
		}
		const Array entries = _read_entries(manifest_path);
		_delete_stale(entries, HashSet<String>(), true);
		if (_is_imported_sidecar(manifest_path)) {
			DirAccess::remove_absolute(manifest_path);
		}
	}
}
