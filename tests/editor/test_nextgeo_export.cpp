/**************************************************************************/
/*  test_nextgeo_export.cpp                                               */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_nextgeo_export)

#if defined(TOOLS_ENABLED)

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "editor/export/editor_export_preset.h"
#include "editor/export/nextgeo_export_plugin.h"
#include "tests/test_utils.h"

#ifndef _3D_DISABLED
#include "scene/resources/3d/virtual_geometry_data.h"
#include "scene/resources/material.h"
#endif

// Preset setters call EditorExport::save_presets(). Tests write the private fields instead.
class NextGeoExportTestAccess {
public:
	static void set_encryption(EditorExportPreset *p_preset, bool p_enabled, const Vector<String> &p_include, const Vector<String> &p_exclude) {
		p_preset->enc_pck = p_enabled;
		p_preset->enc_in_filters = p_include;
		p_preset->enc_ex_filters = p_exclude;
	}

	static void set_option(EditorExportPreset *p_preset, const StringName &p_name, const Variant &p_value) {
		p_preset->values[p_name] = p_value;
	}
};

static void write_text(const String &p_path, const String &p_text) {
	DirAccess::make_dir_recursive_absolute(p_path.get_base_dir());
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	REQUIRE(file.is_valid());
	file->store_string(p_text);
}

static String manifest_json(const Vector<String> &p_paths) {
	String entries;
	for (int i = 0; i < p_paths.size(); i++) {
		if (i > 0) {
			entries += ",";
		}
		entries += "{\"ngm\":\"" + p_paths[i] + "\"}";
	}
	return "{\"version\":1,\"entries\":[" + entries + "]}";
}

static Vector<String> filters(const String &p_filter) {
	Vector<String> out;
	out.push_back(p_filter);
	return out;
}

static String encryption_message(int p_count) {
	return vformat("Encrypted PCK would encrypt %d .ngm file(s); add *.ngm to the encryption exclude filters.", p_count);
}

TEST_CASE("[NextGeo][NGMExport] Encryption filters match the preset loops") {
	Ref<EditorExportPreset> preset;
	preset.instantiate();
	const String ngm_res = "res://.godot/imported/box.ngm";
	const String ngm_plain = ".godot/imported/box.ngm";
	const String other = "res://tests/export/saved_mesh.res";

	NextGeoExportTestAccess::set_encryption(preset.ptr(), false, filters("*"), Vector<String>());
	CHECK_FALSE(preset->is_path_encrypted(ngm_res));
	CHECK_FALSE(preset->is_path_encrypted(other));

	NextGeoExportTestAccess::set_encryption(preset.ptr(), true, Vector<String>(), Vector<String>());
	CHECK_FALSE(preset->is_path_encrypted(ngm_res));

	NextGeoExportTestAccess::set_encryption(preset.ptr(), true, filters(""), Vector<String>());
	CHECK_FALSE(preset->is_path_encrypted(ngm_res));

	NextGeoExportTestAccess::set_encryption(preset.ptr(), true, filters("*"), Vector<String>());
	CHECK(preset->is_path_encrypted(ngm_res));
	CHECK(preset->is_path_encrypted(ngm_plain));
	CHECK(preset->is_path_encrypted(other));

	NextGeoExportTestAccess::set_encryption(preset.ptr(), true, filters("*"), filters("*.ngm"));
	CHECK_FALSE(preset->is_path_encrypted(ngm_res));
	CHECK_FALSE(preset->is_path_encrypted(ngm_plain));
	CHECK(preset->is_path_encrypted(other));
}

TEST_CASE("[NextGeo][NGMExport] Encryption pre-check counts manifest files") {
	const String root = TestUtils::get_temp_path("ngm66_enc");
	const String existing_a = root.path_join("a.ngm");
	const String existing_b = root.path_join("b.ngm");
	const String missing = root.path_join("missing.ngm");
	write_text(existing_a, "a");
	write_text(existing_b, "b");
	Vector<String> listed;
	listed.push_back(existing_a);
	listed.push_back(existing_b);
	listed.push_back(missing);
	write_text(root.path_join(".godot/imported/room.ngm_manifest.json"), manifest_json(listed));

	Ref<EditorExportPreset> preset;
	preset.instantiate();
	String error;

	NextGeoExportTestAccess::set_encryption(preset.ptr(), true, filters("*"), Vector<String>());
	CHECK(NextGeoExportPlugin::validate_encryption(preset, error, root) == FAILED);
	CHECK(error == encryption_message(2));

	const String one_root = TestUtils::get_temp_path("ngm66_enc_one");
	const String one_file = one_root.path_join("only.ngm");
	write_text(one_file, "only");
	Vector<String> one_listed;
	one_listed.push_back(one_file);
	one_listed.push_back(one_root.path_join("gone.ngm"));
	write_text(one_root.path_join("mesh.ngm_manifest.json"), manifest_json(one_listed));
	CHECK(NextGeoExportPlugin::validate_encryption(preset, error, one_root) == FAILED);
	CHECK(error == encryption_message(1));

	NextGeoExportTestAccess::set_encryption(preset.ptr(), true, filters("*"), filters("*.ngm"));
	CHECK(NextGeoExportPlugin::validate_encryption(preset, error, root) == OK);
	CHECK(error.is_empty());

	NextGeoExportTestAccess::set_encryption(preset.ptr(), false, filters("*"), Vector<String>());
	CHECK(NextGeoExportPlugin::validate_encryption(preset, error, root) == OK);

	const String empty_root = TestUtils::get_temp_path("ngm66_enc_empty");
	DirAccess::make_dir_recursive_absolute(empty_root);
	NextGeoExportTestAccess::set_encryption(preset.ptr(), true, filters("*"), Vector<String>());
	CHECK(NextGeoExportPlugin::validate_encryption(preset, error, empty_root) == OK);
	CHECK(error.is_empty());

	CHECK(NextGeoExportPlugin::validate_encryption(Ref<EditorExportPreset>(), error, root) == OK);
}

TEST_CASE("[NextGeo][NGMExport] Manifest scan sees hidden imports and skips nested projects") {
	const String root = TestUtils::get_temp_path("ngm66_scan");
	const String hidden_ngm = root.path_join(".godot/imported/room.ngm");
	const String loose_ngm = root.path_join("loose.ngm");
	const String nested_ngm = root.path_join("nested/secret.ngm");
	Vector<String> hidden_paths;
	hidden_paths.push_back(hidden_ngm);
	Vector<String> loose_paths;
	loose_paths.push_back(loose_ngm);
	Vector<String> nested_paths;
	nested_paths.push_back(nested_ngm);
	write_text(root.path_join(".godot/imported/room.ngm_manifest.json"), manifest_json(hidden_paths));
	write_text(root.path_join("loose.ngm_manifest.json"), manifest_json(loose_paths));
	write_text(root.path_join("nested/project.godot"), "config_version=5\n");
	write_text(root.path_join("nested/side.ngm_manifest.json"), manifest_json(nested_paths));

	const Vector<String> found = NextGeoExportPlugin::collect_manifest_ngm_paths(root);
	CHECK(found.has(hidden_ngm));
	CHECK(found.has(loose_ngm));
	CHECK_FALSE(found.has(nested_ngm));
	CHECK(NextGeoExportPlugin::directory_has_ngm_manifest(root));

	const String empty_entries = TestUtils::get_temp_path("ngm66_scan_empty");
	write_text(empty_entries.path_join("none.ngm_manifest.json"), "{\"version\":1,\"entries\":[]}");
	CHECK_FALSE(NextGeoExportPlugin::directory_has_ngm_manifest(empty_entries));

	const String missing_only = TestUtils::get_temp_path("ngm66_scan_missing");
	Vector<String> missing_paths;
	missing_paths.push_back(missing_only.path_join("absent.ngm"));
	write_text(missing_only.path_join("gone.ngm_manifest.json"), manifest_json(missing_paths));
	CHECK(NextGeoExportPlugin::directory_has_ngm_manifest(missing_only));
	CHECK_FALSE(FileAccess::exists(missing_only.path_join("absent.ngm")));
}

TEST_CASE("[NextGeo][NGMExport] Desktop tag and strip decisions") {
	CHECK(NextGeoExportPlugin::os_gets_default_nextgeo_tag("Windows"));
	CHECK(NextGeoExportPlugin::os_gets_default_nextgeo_tag("Linux"));
	CHECK(NextGeoExportPlugin::os_gets_default_nextgeo_tag("macOS"));
	CHECK_FALSE(NextGeoExportPlugin::os_gets_default_nextgeo_tag("Android"));
	CHECK_FALSE(NextGeoExportPlugin::os_gets_default_nextgeo_tag("Web"));
	CHECK_FALSE(NextGeoExportPlugin::os_gets_default_nextgeo_tag("iOS"));

	CHECK(NextGeoExportPlugin::should_export_nextgeo_feature("Linux", true, true));
	CHECK(NextGeoExportPlugin::should_export_nextgeo_feature("Windows", true, true));
	CHECK(NextGeoExportPlugin::should_export_nextgeo_feature("macOS", true, true));
	CHECK_FALSE(NextGeoExportPlugin::should_export_nextgeo_feature("Linux", false, true));
	CHECK_FALSE(NextGeoExportPlugin::should_export_nextgeo_feature("Linux", true, false));
	CHECK_FALSE(NextGeoExportPlugin::should_export_nextgeo_feature("Android", true, true));
	CHECK_FALSE(NextGeoExportPlugin::should_export_nextgeo_feature("Web", true, true));

	CHECK(NextGeoExportPlugin::custom_features_include_nextgeo("nextgeo"));
	CHECK(NextGeoExportPlugin::custom_features_include_nextgeo("foo, nextgeo , bar"));
	CHECK_FALSE(NextGeoExportPlugin::custom_features_include_nextgeo("nextgeo_extra"));
	CHECK_FALSE(NextGeoExportPlugin::custom_features_include_nextgeo("NextGeo"));
	CHECK_FALSE(NextGeoExportPlugin::custom_features_include_nextgeo(""));

	CHECK_FALSE(NextGeoExportPlugin::should_strip_resources("Linux", false, true, true));
	CHECK(NextGeoExportPlugin::should_strip_resources("Android", true, false, true));
	CHECK_FALSE(NextGeoExportPlugin::should_strip_resources("Android", true, true, false));
	CHECK(NextGeoExportPlugin::should_strip_resources("Linux", true, true, false));
	CHECK_FALSE(NextGeoExportPlugin::should_strip_resources("Linux", true, false, false));
	CHECK_FALSE(NextGeoExportPlugin::should_strip_resources("Web", true, true, false));
	CHECK(NextGeoExportPlugin::should_strip_resources("macOS", true, true, false));

	CHECK(NextGeoExportPlugin::strip_option_enabled(Ref<EditorExportPreset>()));
	Ref<EditorExportPreset> preset;
	preset.instantiate();
	CHECK(NextGeoExportPlugin::strip_option_enabled(preset));
	NextGeoExportTestAccess::set_option(preset.ptr(), "nextgeo/strip_authored_surfaces", false);
	CHECK_FALSE(NextGeoExportPlugin::strip_option_enabled(preset));
	NextGeoExportTestAccess::set_option(preset.ptr(), "nextgeo/strip_authored_surfaces", true);
	CHECK(NextGeoExportPlugin::strip_option_enabled(preset));
}

#ifndef _3D_DISABLED

static Array triangle_arrays(const Vector3 &p_origin) {
	PackedVector3Array vertices;
	vertices.push_back(p_origin);
	vertices.push_back(p_origin + Vector3(1, 0, 0));
	vertices.push_back(p_origin + Vector3(0, 1, 0));
	PackedVector3Array normals;
	normals.push_back(Vector3(0, 0, 1));
	normals.push_back(Vector3(0, 0, 1));
	normals.push_back(Vector3(0, 0, 1));
	PackedInt32Array indices;
	indices.push_back(0);
	indices.push_back(1);
	indices.push_back(2);
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_INDEX] = indices;
	return arrays;
}

static PackedVector3Array surface_vertices(const Ref<ArrayMesh> &p_mesh, int p_surface) {
	const Array arrays = p_mesh->surface_get_arrays(p_surface);
	return arrays[Mesh::ARRAY_VERTEX];
}

static bool same_origin(const PackedVector3Array &p_vertices, const Vector3 &p_origin) {
	return p_vertices.size() == 3 && p_vertices[0].is_equal_approx(p_origin);
}

TEST_CASE("[NextGeo][NGMExport] Stripping keeps the coarse fallback and legacy surfaces") {
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->set_name("body");
	const Array authored = triangle_arrays(Vector3(0, 0, 0));
	const Array legacy = triangle_arrays(Vector3(2, 0, 0));
	const Array coarse = triangle_arrays(Vector3(5, 0, 0));
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, authored, TypedArray<Array>(), Dictionary(), Mesh::ARRAY_FLAG_USE_DYNAMIC_UPDATE);
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, legacy, TypedArray<Array>(), Dictionary(), Mesh::ARRAY_FLAG_USE_DYNAMIC_UPDATE);
	Ref<StandardMaterial3D> material;
	material.instantiate();
	mesh->surface_set_material(1, material);
	mesh->surface_set_name(1, "legacy");

	Ref<VirtualGeometryData> data;
	data.instantiate();
	data->set_ngm_path("res://tests/export/saved_mesh.res.ngm");
	PackedByteArray eligibility;
	eligibility.push_back(1);
	eligibility.push_back(0);
	data->set_surface_eligibility(eligibility);
	TypedArray<Array> coarse_surfaces;
	coarse_surfaces.push_back(coarse);
	coarse_surfaces.push_back(Array());
	data->set_coarse_fallback(coarse_surfaces);
	mesh->set_virtual_geometry(data);

	Ref<ArrayMesh> shadow;
	shadow.instantiate();
	shadow->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, triangle_arrays(Vector3(9, 0, 0)));
	mesh->set_shadow_mesh(shadow);

	Ref<ArrayMesh> stripped = NextGeoExportPlugin::make_stripped_mesh(mesh);
	REQUIRE(stripped.is_valid());
	CHECK(stripped->get_surface_count() == 2);
	CHECK(same_origin(surface_vertices(stripped, 0), Vector3(5, 0, 0)));
	CHECK(same_origin(surface_vertices(stripped, 1), Vector3(2, 0, 0)));
	CHECK(stripped->surface_get_material(1) == material);
	CHECK(stripped->surface_get_name(1) == "legacy");
	CHECK(stripped->surface_get_format(0).has_flag(Mesh::ARRAY_FLAG_USE_DYNAMIC_UPDATE));
	CHECK(stripped->surface_get_format(1).has_flag(Mesh::ARRAY_FLAG_USE_DYNAMIC_UPDATE));
	CHECK(stripped->get_shadow_mesh() == shadow);
	CHECK(stripped->get_name() == "body");

	Ref<VirtualGeometryData> copy = stripped->get_virtual_geometry();
	REQUIRE(copy.is_valid());
	CHECK(copy != data);
	CHECK(copy->get_coarse_fallback().is_empty());
	CHECK(copy->get_ngm_path() == "res://tests/export/saved_mesh.res.ngm");
	const PackedByteArray copy_eligibility = copy->get_surface_eligibility();
	CHECK(copy_eligibility.size() == 2);
	CHECK(copy_eligibility[0] == 1);
	CHECK(copy_eligibility[1] == 0);

	CHECK(same_origin(surface_vertices(mesh, 0), Vector3(0, 0, 0)));
	const Array original_coarse = data->get_coarse_fallback()[0];
	const PackedVector3Array original_vertices = original_coarse[Mesh::ARRAY_VERTEX];
	CHECK(same_origin(original_vertices, Vector3(5, 0, 0)));

	Ref<ArrayMesh> plain;
	plain.instantiate();
	plain->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, authored);
	CHECK_FALSE(NextGeoExportPlugin::make_stripped_mesh(plain).is_valid());

	Ref<ArrayMesh> legacy_only;
	legacy_only.instantiate();
	legacy_only->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, authored);
	Ref<VirtualGeometryData> legacy_data;
	legacy_data.instantiate();
	PackedByteArray zeros;
	zeros.push_back(0);
	legacy_data->set_surface_eligibility(zeros);
	TypedArray<Array> usable;
	usable.push_back(coarse);
	legacy_data->set_coarse_fallback(usable);
	legacy_only->set_virtual_geometry(legacy_data);
	CHECK_FALSE(NextGeoExportPlugin::make_stripped_mesh(legacy_only).is_valid());

	Ref<ArrayMesh> bad_coarse;
	bad_coarse.instantiate();
	bad_coarse->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, authored);
	Ref<VirtualGeometryData> bad_data;
	bad_data.instantiate();
	PackedByteArray one;
	one.push_back(1);
	bad_data->set_surface_eligibility(one);
	Array short_arrays;
	short_arrays.push_back(authored[Mesh::ARRAY_VERTEX]);
	TypedArray<Array> bad_surfaces;
	bad_surfaces.push_back(short_arrays);
	bad_data->set_coarse_fallback(bad_surfaces);
	bad_coarse->set_virtual_geometry(bad_data);
	CHECK_FALSE(NextGeoExportPlugin::make_stripped_mesh(bad_coarse).is_valid());

	Array nil_vertices;
	nil_vertices.resize(Mesh::ARRAY_MAX);
	TypedArray<Array> nil_surfaces;
	nil_surfaces.push_back(nil_vertices);
	bad_data->set_coarse_fallback(nil_surfaces);
	CHECK_FALSE(NextGeoExportPlugin::make_stripped_mesh(bad_coarse).is_valid());

	Ref<StandardMaterial3D> not_a_mesh;
	not_a_mesh.instantiate();
	CHECK_FALSE(NextGeoExportPlugin::strip_exported_resource(not_a_mesh).is_valid());

	Ref<ArrayMesh> shaped;
	shaped.instantiate();
	shaped->add_blend_shape("fat");
	// Normals imply a tangent channel. Blend shapes must carry the same vertex, normal and tangent arrays.
	PackedFloat32Array tangents;
	for (int i = 0; i < 3; i++) {
		tangents.push_back(1.0f);
		tangents.push_back(0.0f);
		tangents.push_back(0.0f);
		tangents.push_back(1.0f);
	}
	Array shape_arrays;
	shape_arrays.resize(Mesh::ARRAY_MAX);
	shape_arrays[Mesh::ARRAY_VERTEX] = authored[Mesh::ARRAY_VERTEX];
	shape_arrays[Mesh::ARRAY_NORMAL] = authored[Mesh::ARRAY_NORMAL];
	shape_arrays[Mesh::ARRAY_TANGENT] = tangents;
	TypedArray<Array> shapes;
	shapes.push_back(shape_arrays);
	shaped->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, authored, shapes);
	Ref<VirtualGeometryData> shaped_data;
	shaped_data.instantiate();
	shaped_data->set_surface_eligibility(one);
	TypedArray<Array> shaped_coarse;
	shaped_coarse.push_back(coarse);
	shaped_data->set_coarse_fallback(shaped_coarse);
	shaped->set_virtual_geometry(shaped_data);
	Ref<ArrayMesh> shaped_stripped = NextGeoExportPlugin::make_stripped_mesh(shaped);
	REQUIRE(shaped_stripped.is_valid());
	CHECK(same_origin(surface_vertices(shaped_stripped, 0), Vector3(0, 0, 0)));
	CHECK(shaped_stripped->get_blend_shape_count() == 1);
	CHECK(shaped_stripped->get_blend_shape_name(0) == "fat");
}

#endif // _3D_DISABLED

#endif // TOOLS_ENABLED
