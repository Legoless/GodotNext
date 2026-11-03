/**************************************************************************/
/*  nextgeo_export_plugin.h                                               */
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

#include "core/templates/hash_set.h"
#include "editor/export/editor_export_plugin.h"
#include "scene/resources/mesh.h"

// Adds each `.ngm` sidecar once and, on NextGeo desktop exports, replaces virtualized
// surfaces with the coarse fallback. `.ngm` is not a resource, so the stock export loop
// never sees it. This plugin calls `add_file()` and never `skip()`.
class NextGeoExportPlugin : public EditorExportPlugin {
	GDSOFTCLASS(NextGeoExportPlugin, EditorExportPlugin);

	// Part of the customization configuration hash, so bumping it invalidates cached exports.
	// 2: #242 fixed the dummy renderer dropping VoxelGI data, so scenes cached by version 1 may
	// lack their GI data.
	static constexpr uint32_t PLUGIN_VERSION = 2;

	HashSet<String> exported_ngm;
	bool strip_resources = false;

	static String _project_root();
	bool _strip_this_export(const Ref<EditorExportPlatform> &p_platform, const Vector<String> &p_features) const;
	void _add_ngm_once(const String &p_path);
	void _add_manifest_ngms(const String &p_manifest_path);

protected:
	String get_name() const override { return "NextGeo"; }

	void _export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags) override;
	void _export_file(const String &p_path, const String &p_type, const HashSet<String> &p_features) override;

	PackedStringArray _get_export_features(const Ref<EditorExportPlatform> &p_platform, bool p_debug) const override;
	void _get_export_options(const Ref<EditorExportPlatform> &p_export_platform, List<EditorExportPlatform::ExportOption> *r_options) const override;
	String _get_export_option_warning(const Ref<EditorExportPlatform> &p_export_platform, const String &p_option_name) const override;

	uint64_t _get_customization_configuration_hash() const override;
	bool _begin_customize_resources(const Ref<EditorExportPlatform> &p_platform, const Vector<String> &p_features) override;
	Ref<Resource> _customize_resource(const Ref<Resource> &p_resource, const String &p_path) override;
	void _end_customize_resources() override;

public:
	// Windows, Linux and macOS. Android, Web and every other OS stay off.
	static bool os_gets_default_nextgeo_tag(const String &p_os);
	// The tag this plugin itself appends. A hand-added custom feature is separate.
	static bool should_export_nextgeo_feature(const String &p_os, bool p_strip_option, bool p_has_manifest);
	// Comma-separated preset custom features. The token must be exact, so `nextgeo_extra` does not count.
	static bool custom_features_include_nextgeo(const String &p_custom_features);
	// The strip option gates this. A hand-added `nextgeo` feature strips on any OS. Otherwise only the
	// desktop tag does. Plugin features are appended after customization begins, so the desktop tag is
	// decided here instead of being read back from `p_features`.
	static bool should_strip_resources(const String &p_os, bool p_strip_option, bool p_has_manifest, bool p_features_have_nextgeo);
	// Missing `nextgeo/strip_authored_surfaces` keeps the registered default, which is on.
	static bool strip_option_enabled(const Ref<EditorExportPreset> &p_preset);

	// Walks `p_root`, including hidden directories such as `.godot/imported`. A subdirectory
	// that contains `project.godot` is a nested project and is not scanned.
	static bool directory_has_ngm_manifest(const String &p_root);
	static Vector<String> collect_manifest_ngm_paths(const String &p_root);

	// `p_scan_root` empty means the open project. Fails before any pack byte is written when
	// an encrypted preset would encrypt a manifest `.ngm`.
	static Error validate_encryption(const Ref<EditorExportPreset> &p_preset, String &r_error, const String &p_scan_root = String());

	// Null when `p_mesh` has no virtualized surface to replace. The original mesh is left as it is.
	static Ref<ArrayMesh> make_stripped_mesh(const Ref<ArrayMesh> &p_mesh);
	// ArrayMesh goes through `make_stripped_mesh`. Every other resource is left alone.
	static Ref<Resource> strip_exported_resource(const Ref<Resource> &p_resource);
};
