/**************************************************************************/
/*  test_importer_mesh_virtual_geometry.cpp                               */
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

TEST_FORCE_LINK(test_importer_mesh_virtual_geometry)

#ifndef _3D_DISABLED

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "scene/resources/3d/importer_mesh.h"
#include "scene/resources/3d/virtual_geometry_fallback.h"
#include "scene/resources/material.h"
#include "scene/resources/shader.h"
#include "tests/test_utils.h"

#include "modules/modules_enabled.gen.h"

#ifdef MODULE_NEXTGEO_ENABLED
#include "core/config/project_settings.h"
#include "scene/resources/3d/virtual_geometry_data.h"

#include "modules/nextgeo/nextgeo_rt_proxy.h"
#endif

#ifdef TOOLS_ENABLED
#include "core/io/json.h"
#include "editor/import/3d/ngm_manifest.h"
#include "editor/import/3d/resource_importer_obj.h"
#include "editor/import/3d/resource_importer_scene.h"
#endif

namespace TestImporterMeshVirtualGeometry {

Ref<ImporterMesh> grid_mesh(int p_n, const String &p_name, const Ref<Material> &p_material) {
	PackedVector3Array vertices;
	PackedVector3Array normals;
	PackedInt32Array indices;
	for (int y = 0; y <= p_n; y++) {
		for (int x = 0; x <= p_n; x++) {
			vertices.push_back(Vector3(float(x), 0.0f, float(y)));
			normals.push_back(Vector3(0.0f, 1.0f, 0.0f));
		}
	}
	for (int y = 0; y < p_n; y++) {
		for (int x = 0; x < p_n; x++) {
			const int i = y * (p_n + 1) + x;
			indices.push_back(i);
			indices.push_back(i + p_n + 1);
			indices.push_back(i + 1);
			indices.push_back(i + 1);
			indices.push_back(i + p_n + 1);
			indices.push_back(i + p_n + 2);
		}
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_INDEX] = indices;
	Ref<ImporterMesh> mesh;
	mesh.instantiate();
	mesh->set_name(p_name);
	mesh->add_surface(Mesh::PRIMITIVE_TRIANGLES, arrays, TypedArray<Array>(), Dictionary(), p_material, "surf");
	return mesh;
}

Ref<StandardMaterial3D> standard_material() {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	return material;
}

TEST_CASE("[NextGeo][Import] Eligibility reasons match the fallback codes") {
	Ref<ImporterMesh> mesh = grid_mesh(1, "opaque", Ref<Material>());
	PackedInt32Array reasons = mesh->get_virtual_geometry_eligibility();
	CHECK(reasons.size() == 1);
	CHECK(reasons[0] == NEXTGEO_FALLBACK_NONE);

	Ref<StandardMaterial3D> alpha = standard_material();
	alpha->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR);
	mesh = grid_mesh(1, "alpha", alpha);
	CHECK(mesh->get_virtual_geometry_eligibility()[0] == NEXTGEO_FALLBACK_ALPHA);

	Ref<StandardMaterial3D> blended = standard_material();
	blended->set_blend_mode(BaseMaterial3D::BLEND_MODE_ADD);
	mesh = grid_mesh(1, "blend", blended);
	CHECK(mesh->get_virtual_geometry_eligibility()[0] == NEXTGEO_FALLBACK_ALPHA);

	Ref<StandardMaterial3D> refraction = standard_material();
	refraction->set_feature(BaseMaterial3D::FEATURE_REFRACTION, true);
	mesh = grid_mesh(1, "refract", refraction);
	CHECK(mesh->get_virtual_geometry_eligibility()[0] == NEXTGEO_FALLBACK_ALPHA);

	Ref<StandardMaterial3D> vertex = standard_material();
	vertex->set_shading_mode(BaseMaterial3D::SHADING_MODE_PER_VERTEX);
	mesh = grid_mesh(1, "vertex", vertex);
	CHECK(mesh->get_virtual_geometry_eligibility()[0] == NEXTGEO_FALLBACK_VERTEX_LIGHTING);

	Ref<StandardMaterial3D> next_pass = standard_material();
	next_pass->set_next_pass(standard_material());
	mesh = grid_mesh(1, "next", next_pass);
	CHECK(mesh->get_virtual_geometry_eligibility()[0] == NEXTGEO_FALLBACK_NEXT_PASS_OR_OVERLAY);

	Ref<StandardMaterial3D> stencil = standard_material();
	stencil->set_stencil_mode(BaseMaterial3D::STENCIL_MODE_OUTLINE);
	mesh = grid_mesh(1, "stencil", stencil);
	CHECK(mesh->get_virtual_geometry_eligibility()[0] == NEXTGEO_FALLBACK_NEXT_PASS_OR_OVERLAY);

	{
		Ref<ImporterMesh> shaped;
		shaped.instantiate();
		shaped->add_blend_shape("fat");
		Ref<ImporterMesh> donor = grid_mesh(1, "shape", Ref<Material>());
		Array shape_arrays;
		shape_arrays.resize(Mesh::ARRAY_MAX);
		shape_arrays[Mesh::ARRAY_VERTEX] = donor->get_surface_arrays(0)[Mesh::ARRAY_VERTEX];
		TypedArray<Array> shapes;
		shapes.push_back(shape_arrays);
		shaped->add_surface(Mesh::PRIMITIVE_TRIANGLES, donor->get_surface_arrays(0), shapes);
		// Blend shapes stay legacy with their own reason, skinned opt-in included (#159).
		CHECK(shaped->get_virtual_geometry_eligibility()[0] == NEXTGEO_FALLBACK_BLEND_SHAPES);
		CHECK(shaped->get_virtual_geometry_eligibility(true)[0] == NEXTGEO_FALLBACK_BLEND_SHAPES);
	}

	{
		PackedVector3Array vertices;
		vertices.push_back(Vector3(0, 0, 0));
		vertices.push_back(Vector3(1, 0, 0));
		vertices.push_back(Vector3(0, 1, 0));
		PackedInt32Array bones;
		PackedFloat32Array weights;
		for (int v = 0; v < 3; v++) {
			for (int b = 0; b < 4; b++) {
				bones.push_back(0);
				weights.push_back(b == 0 ? 1.0f : 0.0f);
			}
		}
		PackedInt32Array indices;
		indices.push_back(0);
		indices.push_back(1);
		indices.push_back(2);
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = vertices;
		arrays[Mesh::ARRAY_BONES] = bones;
		arrays[Mesh::ARRAY_WEIGHTS] = weights;
		arrays[Mesh::ARRAY_INDEX] = indices;
		Ref<ImporterMesh> skinned;
		skinned.instantiate();
		skinned->add_surface(Mesh::PRIMITIVE_TRIANGLES, arrays);
		CHECK(skinned->get_virtual_geometry_eligibility()[0] == NEXTGEO_FALLBACK_SKINNING);
		// The skinned opt-in (issue #159) makes the same surface eligible.
		CHECK(skinned->get_virtual_geometry_eligibility(true)[0] == NEXTGEO_FALLBACK_NONE);
	}

	{
		PackedVector3Array vertices;
		vertices.push_back(Vector3(0, 0, 0));
		vertices.push_back(Vector3(1, 0, 0));
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = vertices;
		Ref<ImporterMesh> lines;
		lines.instantiate();
		lines->add_surface(Mesh::PRIMITIVE_LINES, arrays);
		CHECK(lines->get_virtual_geometry_eligibility()[0] == NEXTGEO_FALLBACK_UNSUPPORTED_PRIMITIVE);
	}

	{
		Ref<Shader> shader;
		shader.instantiate();
		shader->set_code("shader_type canvas_item;\nvoid fragment() {\n\tCOLOR = vec4(1.0);\n}\n");
		Ref<ShaderMaterial> canvas;
		canvas.instantiate();
		canvas->set_shader(shader);
		mesh = grid_mesh(1, "canvas", canvas);
		CHECK(mesh->get_virtual_geometry_eligibility()[0] == NEXTGEO_FALLBACK_UNSUPPORTED_CUSTOM_SHADER);

		shader->set_code("shader_type spatial;\nvoid fragment() {\n\tALBEDO = vec3(1.0);\n}\n");
		mesh = grid_mesh(1, "spatial", canvas);
		CHECK(mesh->get_virtual_geometry_eligibility()[0] == NEXTGEO_FALLBACK_NONE);
	}
}

#ifdef MODULE_NEXTGEO_ENABLED

struct RestoreHook {
	ImporterMesh::BuildVirtualGeometryFunc saved = ImporterMesh::build_virtual_geometry_func;
	~RestoreHook() { ImporterMesh::build_virtual_geometry_func = saved; }
};

void expect_same_geometry(const Ref<VirtualGeometryData> &p_a, const Ref<VirtualGeometryData> &p_b) {
	REQUIRE(p_a.is_valid());
	REQUIRE(p_b.is_valid());
	CHECK(p_a->get_dag_header() == p_b->get_dag_header());
	CHECK(p_a->get_surface_eligibility() == p_b->get_surface_eligibility());
	CHECK(p_a->get_surface_fallback_reasons() == p_b->get_surface_fallback_reasons());
	CHECK(p_a->get_coarse_fallback() == p_b->get_coarse_fallback());
	CHECK(p_a->get_root_pages() == p_b->get_root_pages());
	CHECK(p_a->get_source_triangle_count() == p_b->get_source_triangle_count());
	CHECK(p_a->get_source_vertex_index_bytes() == p_b->get_source_vertex_index_bytes());
	CHECK(p_a->get_cluster_count() == p_b->get_cluster_count());
	CHECK(p_a->get_group_count() == p_b->get_group_count());
	CHECK(p_a->get_dag_depth() == p_b->get_dag_depth());
	CHECK(p_a->get_page_count() == p_b->get_page_count());
	CHECK(p_a->get_page_size() == p_b->get_page_size());
	CHECK(p_a->get_ngm_sha256() == p_b->get_ngm_sha256());
	CHECK(p_a->get_ngm_sha256().size() == 32);
	CHECK(p_a->get_rt_proxy_positions() == p_b->get_rt_proxy_positions());
	CHECK(p_a->get_rt_proxy_indices() == p_b->get_rt_proxy_indices());
	CHECK(p_a->get_rt_proxy_parts() == p_b->get_rt_proxy_parts());
	CHECK(p_a->get_rt_proxy_surface_ranges() == p_b->get_rt_proxy_surface_ranges());
	CHECK(p_a->get_rt_proxy_clusters() == p_b->get_rt_proxy_clusters());
	CHECK(p_a->get_rt_proxy_eps() == doctest::Approx(p_b->get_rt_proxy_eps()));
	CHECK(p_a->get_rt_proxy_error() == doctest::Approx(p_b->get_rt_proxy_error()));
}

// eps_rt = min(max_extent / 256, rendering/global_illumination/nextgi/rt_max_error).
float expected_proxy_eps(float p_max_extent) {
	const float rt_max_error = float(GLOBAL_GET("rendering/global_illumination/nextgi/rt_max_error"));
	return float(MIN(double(p_max_extent) / 256.0, double(rt_max_error)));
}

void check_filled_proxy(const Ref<VirtualGeometryData> &p_data, float p_max_extent, int p_surface_count) {
	REQUIRE(p_data.is_valid());
	CHECK_FALSE(p_data->get_rt_proxy_positions().is_empty());
	CHECK_FALSE(p_data->get_rt_proxy_indices().is_empty());
	CHECK_FALSE(p_data->get_rt_proxy_parts().is_empty());
	CHECK_FALSE(p_data->get_rt_proxy_surface_ranges().is_empty());
	CHECK_FALSE(p_data->get_rt_proxy_clusters().is_empty());
	CHECK(p_data->get_rt_proxy_positions().size() % 3 == 0);
	CHECK(p_data->get_rt_proxy_indices().size() % 4 == 0);
	CHECK(p_data->get_rt_proxy_eps() == doctest::Approx(expected_proxy_eps(p_max_extent)));
	CHECK(p_data->get_rt_proxy_error() <= p_data->get_rt_proxy_eps());

	const PackedInt32Array parts = p_data->get_rt_proxy_parts();
	CHECK(parts.size() % 4 == 0);
	const int part_count = parts.size() / 4;
	REQUIRE(part_count >= 1);
	CHECK(p_data->get_rt_proxy_surface_ranges().size() == part_count * p_surface_count * 2);
	for (int part = 0; part < part_count; part++) {
		const int index_count = parts[part * 4 + 3];
		CHECK(index_count > 0);
		CHECK(index_count % 3 == 0);
		CHECK(uint32_t(index_count / 3) <= NEXTGEO_RT_PROXY_MAX_PART_TRIANGLES);
	}
}

TEST_CASE("[NextGeo][Import] Virtualized surfaces have no LODs and no shadow mesh") {
	const String path = TestUtils::get_temp_path("ngm67_lods.ngm");
	Ref<ImporterMesh> mesh = grid_mesh(8, "grid", Ref<Material>());
	CHECK(mesh->build_virtual_geometry(path) == OK);
	REQUIRE(mesh->get_virtual_geometry().is_valid());
	CHECK(mesh->get_virtual_geometry()->get_surface_eligibility()[0] == 1);
	CHECK(mesh->get_virtual_geometry()->get_surface_fallback_reasons()[0] == NEXTGEO_FALLBACK_NONE);
	mesh->generate_lods(20.0f, Array());
	CHECK(mesh->get_surface_lod_count(0) == 0);
	mesh->create_shadow_mesh();
	CHECK(mesh->get_shadow_mesh().is_null());

	const PackedVector3Array authored = mesh->get_surface_arrays(0)[Mesh::ARRAY_VERTEX];
	Ref<ArrayMesh> kept = mesh->get_mesh();
	REQUIRE(kept.is_valid());
	CHECK(kept->get_virtual_geometry().is_valid());
	const PackedVector3Array saved = kept->surface_get_arrays(0)[Mesh::ARRAY_VERTEX];
	CHECK(saved.size() == authored.size());
	CHECK(saved.size() > 0);
}

TEST_CASE("[NextGeo][Import] A fully ineligible mesh keeps LODs and a shadow mesh") {
	Ref<StandardMaterial3D> alpha = standard_material();
	alpha->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	Ref<ImporterMesh> mesh = grid_mesh(8, "legacy", alpha);
	const String path = TestUtils::get_temp_path("ngm67_legacy.ngm");
	CHECK(mesh->build_virtual_geometry(path) == OK);
	CHECK(mesh->get_virtual_geometry().is_null());
	CHECK_FALSE(FileAccess::exists(path));
	mesh->generate_lods(20.0f, Array());
	CHECK(mesh->get_surface_lod_count(0) > 0);
	mesh->create_shadow_mesh();
	CHECK(mesh->get_shadow_mesh().is_valid());
}

TEST_CASE("[NextGeo][Import] A mixed mesh virtualizes only the eligible surface") {
	Ref<ImporterMesh> mesh = grid_mesh(8, "mixed", Ref<Material>());
	Ref<StandardMaterial3D> alpha = standard_material();
	alpha->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	Ref<ImporterMesh> extra = grid_mesh(8, "extra", alpha);
	mesh->add_surface(Mesh::PRIMITIVE_TRIANGLES, extra->get_surface_arrays(0), TypedArray<Array>(), Dictionary(), alpha, "alpha");
	const String path = TestUtils::get_temp_path("ngm67_mixed.ngm");
	CHECK(mesh->build_virtual_geometry(path) == OK);
	REQUIRE(mesh->get_virtual_geometry().is_valid());
	CHECK(mesh->get_virtual_geometry()->get_surface_eligibility()[0] == 1);
	CHECK(mesh->get_virtual_geometry()->get_surface_eligibility()[1] == 0);
	CHECK(mesh->get_virtual_geometry()->get_surface_fallback_reasons()[1] == NEXTGEO_FALLBACK_ALPHA);
	mesh->generate_lods(20.0f, Array());
	CHECK(mesh->get_surface_lod_count(0) == 0);
	CHECK(mesh->get_surface_lod_count(1) > 0);
	mesh->create_shadow_mesh();
	CHECK(mesh->get_shadow_mesh().is_null());
}

TEST_CASE("[NextGeo][Import] A virtualized import stores a filled RT proxy") {
	// 4 m extent: max_extent/256 is 1.5625 cm, under the 2 cm default, so the extent binds.
	Ref<ImporterMesh> small = grid_mesh(4, "extent", Ref<Material>());
	const String small_path = TestUtils::get_temp_path("ngm241_extent.ngm");
	CHECK(small->build_virtual_geometry(small_path) == OK);
	check_filled_proxy(small->get_virtual_geometry(), 4.0f, 1);

	// 8 m extent: max_extent/256 is 3.125 cm, so the rt_max_error setting binds.
	Ref<ImporterMesh> wide = grid_mesh(8, "setting", Ref<Material>());
	const String wide_path = TestUtils::get_temp_path("ngm241_setting.ngm");
	CHECK(wide->build_virtual_geometry(wide_path) == OK);
	check_filled_proxy(wide->get_virtual_geometry(), 8.0f, 1);
	REQUIRE(expected_proxy_eps(4.0f) != doctest::Approx(expected_proxy_eps(8.0f)));
}

TEST_CASE("[NextGeo][Import] Two imports write identical RT proxy bytes") {
	const String path_a = TestUtils::get_temp_path("ngm241_proxy_a.ngm");
	const String path_b = TestUtils::get_temp_path("ngm241_proxy_b.ngm");
	Ref<ImporterMesh> mesh_a = grid_mesh(4, "proxy", Ref<Material>());
	Ref<ImporterMesh> mesh_b = grid_mesh(4, "proxy", Ref<Material>());
	CHECK(mesh_a->build_virtual_geometry(path_a) == OK);
	CHECK(mesh_b->build_virtual_geometry(path_b) == OK);
	const Ref<VirtualGeometryData> a = mesh_a->get_virtual_geometry();
	const Ref<VirtualGeometryData> b = mesh_b->get_virtual_geometry();
	REQUIRE(a.is_valid());
	REQUIRE(b.is_valid());
	check_filled_proxy(a, 4.0f, 1);
	CHECK(a->get_rt_proxy_positions() == b->get_rt_proxy_positions());
	CHECK(a->get_rt_proxy_indices() == b->get_rt_proxy_indices());
	CHECK(a->get_rt_proxy_parts() == b->get_rt_proxy_parts());
	CHECK(a->get_rt_proxy_surface_ranges() == b->get_rt_proxy_surface_ranges());
	CHECK(a->get_rt_proxy_clusters() == b->get_rt_proxy_clusters());
	CHECK(a->get_rt_proxy_eps() == doctest::Approx(b->get_rt_proxy_eps()));
	CHECK(a->get_rt_proxy_error() == doctest::Approx(b->get_rt_proxy_error()));
}

TEST_CASE("[NextGeo][Import] An ineligible surface has an empty RT proxy range") {
	// The eligible grid spans 1 m. The legacy triangle spans x = 10..50, so the
	// mesh AABB is 50 m and eps_rt follows that, not the eligible surface alone.
	Ref<ImporterMesh> mesh = grid_mesh(1, "eligible", Ref<Material>());
	Ref<StandardMaterial3D> alpha = standard_material();
	alpha->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
	PackedVector3Array vertices;
	vertices.push_back(Vector3(10, 0, 0));
	vertices.push_back(Vector3(50, 0, 0));
	vertices.push_back(Vector3(10, 1, 0));
	PackedInt32Array indices;
	indices.push_back(0);
	indices.push_back(1);
	indices.push_back(2);
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_INDEX] = indices;
	mesh->add_surface(Mesh::PRIMITIVE_TRIANGLES, arrays, TypedArray<Array>(), Dictionary(), alpha, "legacy");

	const String path = TestUtils::get_temp_path("ngm241_legacy.ngm");
	CHECK(mesh->build_virtual_geometry(path) == OK);
	const Ref<VirtualGeometryData> data = mesh->get_virtual_geometry();
	check_filled_proxy(data, 50.0f, 2);
	REQUIRE(expected_proxy_eps(50.0f) != doctest::Approx(expected_proxy_eps(1.0f)));
	CHECK(data->get_surface_eligibility()[0] == 1);
	CHECK(data->get_surface_eligibility()[1] == 0);

	const PackedInt32Array ranges = data->get_rt_proxy_surface_ranges();
	const int part_count = data->get_rt_proxy_parts().size() / 4;
	for (int part = 0; part < part_count; part++) {
		const int legacy = (part * 2 + 1) * 2;
		CHECK(ranges[legacy] == 0);
		CHECK(ranges[legacy + 1] == 0);
		const int eligible = part * 4;
		CHECK(ranges[eligible] == 0);
		CHECK(ranges[eligible + 1] > 0);
	}
}

TEST_CASE("[NextGeo][Import] Two builds write the same bytes and the same geometry") {
	const String path_a = TestUtils::get_temp_path("ngm67_det_a.ngm");
	const String path_b = TestUtils::get_temp_path("ngm67_det_b.ngm");
	Ref<ImporterMesh> mesh_a = grid_mesh(4, "same", Ref<Material>());
	Ref<ImporterMesh> mesh_b = grid_mesh(4, "same", Ref<Material>());
	CHECK(mesh_a->build_virtual_geometry(path_a) == OK);
	CHECK(mesh_b->build_virtual_geometry(path_b) == OK);
	expect_same_geometry(mesh_a->get_virtual_geometry(), mesh_b->get_virtual_geometry());
	CHECK(FileAccess::get_sha256(path_a) == FileAccess::get_sha256(path_b));
	Ref<FileAccess> file_a = FileAccess::open(path_a, FileAccess::READ);
	Ref<FileAccess> file_b = FileAccess::open(path_b, FileAccess::READ);
	REQUIRE(file_a.is_valid());
	REQUIRE(file_b.is_valid());
	CHECK(file_a->get_buffer(file_a->get_length()) == file_b->get_buffer(file_b->get_length()));
}

TEST_CASE("[NextGeo][Import] strip_authored_to_coarse replaces virtualized surfaces") {
	// Large enough that the root cut is coarser than the authored grid.
	Ref<ImporterMesh> mesh = grid_mesh(32, "strip", Ref<Material>());
	const int authored = PackedVector3Array(mesh->get_surface_arrays(0)[Mesh::ARRAY_VERTEX]).size();
	const String path = TestUtils::get_temp_path("ngm67_strip.ngm");
	CHECK(mesh->build_virtual_geometry(path) == OK);
	mesh->strip_authored_surfaces_to_coarse();
	Ref<ArrayMesh> stripped = mesh->get_mesh();
	REQUIRE(stripped.is_valid());
	const int coarse = PackedVector3Array(stripped->surface_get_arrays(0)[Mesh::ARRAY_VERTEX]).size();
	CHECK(coarse > 0);
	CHECK(coarse < authored);
	CHECK(stripped->get_virtual_geometry().is_valid());
}

TEST_CASE("[NextGeo][Import] A null hook and an empty path do not attach geometry") {
	RestoreHook restore;
	Ref<ImporterMesh> mesh = grid_mesh(1, "hook", Ref<Material>());
	ImporterMesh::build_virtual_geometry_func = nullptr;
	const String path = TestUtils::get_temp_path("ngm67_null_hook.ngm");
	CHECK(mesh->build_virtual_geometry(path) == OK);
	CHECK(mesh->get_virtual_geometry().is_null());
	CHECK_FALSE(FileAccess::exists(path));

	ImporterMesh::build_virtual_geometry_func = restore.saved;
	CHECK(mesh->build_virtual_geometry(String()) == ERR_INVALID_PARAMETER);
	CHECK(mesh->get_virtual_geometry().is_null());
}

TEST_CASE("[NextGeo][Import] Merging two meshes builds one virtual geometry") {
	Ref<ImporterMesh> first = grid_mesh(2, "A", Ref<Material>());
	Ref<ImporterMesh> second = grid_mesh(2, "B", Ref<Material>());
	TypedArray<ImporterMesh> meshes;
	meshes.push_back(first);
	meshes.push_back(second);
	TypedArray<Transform3D> transforms;
	transforms.push_back(Transform3D());
	transforms.push_back(Transform3D(Basis(), Vector3(3, 0, 0)));
	Ref<ImporterMesh> merged = ImporterMesh::merge_importer_meshes(meshes, transforms, true);
	merged->set_name("Merged");
	const String path = TestUtils::get_temp_path("ngm67_merged.ngm");
	CHECK(merged->build_virtual_geometry(path) == OK);
	CHECK(merged->get_virtual_geometry().is_valid());
	CHECK(FileAccess::exists(path));
	CHECK(merged->get_mesh().is_valid());
	CHECK(merged->get_mesh()->get_virtual_geometry().is_valid());
}

#endif // MODULE_NEXTGEO_ENABLED

#ifdef TOOLS_ENABLED

bool option_is(const List<ResourceImporter::ImportOption> &p_options, const String &p_name, const Variant &p_default, const String &p_hint) {
	for (const ResourceImporter::ImportOption &option : p_options) {
		if (String(option.option.name) != p_name) {
			continue;
		}
		CHECK(option.default_value == p_default);
		if (!p_hint.is_empty()) {
			CHECK(option.option.hint_string == p_hint);
		}
		return true;
	}
	return false;
}

TEST_CASE("[NextGeo][Import] Importer option names and defaults match the plan") {
	Ref<ResourceImporterScene> scene;
	scene.instantiate();
	List<ResourceImporter::ImportOption> options;
	scene->get_import_options("", &options);
	CHECK(option_is(options, "meshes/generate_virtual_geometry", false, String()));
	List<ResourceImporter::ImportOption> mesh_options;
	scene->get_internal_import_options(ResourceImporterScene::INTERNAL_IMPORT_CATEGORY_MESH, &mesh_options);
	CHECK(option_is(mesh_options, "generate/virtual_geometry", 0, "Default,Enable,Disable"));

	Ref<ResourceImporterOBJ> obj;
	obj.instantiate();
	List<ResourceImporter::ImportOption> obj_options;
	obj->get_import_options("", &obj_options);
	CHECK(option_is(obj_options, "generate_virtual_geometry", false, String()));
	CHECK(option_is(obj_options, "virtual_geometry_fallback_mode", 0, "retain_authored,strip_authored_to_coarse"));
}

TEST_CASE("[NextGeo][Import] Embedded ngm names use the first 16 hex digits of the key") {
	const String key = "res://a.gltf::Box";
	const String path = ngm_embedded_path("/tmp/base", key);
	CHECK(path == "/tmp/base-" + key.md5_text().substr(0, 16) + ".ngm");
}

TEST_CASE("[NextGeo][Import] Reimport drops stale ngm files and keeps the ones still listed") {
	const String base = TestUtils::get_temp_path("ngm67_reimport");
	DirAccess::make_dir_recursive_absolute(base);
	const String path_a = base.path_join("a.ngm");
	const String path_b = base.path_join("b.ngm");
	Ref<FileAccess> file_a = FileAccess::open(path_a, FileAccess::WRITE);
	Ref<FileAccess> file_b = FileAccess::open(path_b, FileAccess::WRITE);
	REQUIRE(file_a.is_valid());
	REQUIRE(file_b.is_valid());
	file_a->store_string("a");
	file_b->store_string("b");
	file_a.unref();
	file_b.unref();

	Dictionary entry_a;
	entry_a["key"] = "source::A";
	entry_a["ngm"] = path_a;
	entry_a["sha256"] = "aa";
	entry_a["source_triangles"] = 1;
	entry_a["source_vertex_index_bytes"] = 2;
	Dictionary entry_b = entry_a.duplicate();
	entry_b["key"] = "source::B";
	entry_b["ngm"] = path_b;
	Array both;
	both.push_back(entry_a);
	both.push_back(entry_b);
	ngm_manifest_commit(base.path_join("mesh"), "res://source.gltf", both, nullptr, true);

	Array only_a;
	only_a.push_back(entry_a);
	List<String> generated;
	ngm_manifest_commit(base.path_join("mesh"), "res://source.gltf", only_a, &generated, true);
	CHECK(FileAccess::exists(path_a));
	CHECK_FALSE(FileAccess::exists(path_b));
	CHECK(FileAccess::exists(base.path_join("mesh.ngm_manifest.json")));
	CHECK(generated.size() == 2);
}

TEST_CASE("[NextGeo][Import] Deleting the source removes imported sidecars and leaves siblings") {
	const String root = TestUtils::get_temp_path("ngm67_delete");
	const String imported = root.path_join(".godot/imported");
	DirAccess::make_dir_recursive_absolute(imported);
	const String source = root.path_join("source.gltf");
	const String dest = imported.path_join("source.gltf-deadbeef.scn");
	const String imported_ngm = imported.path_join("source-aaa.ngm");
	const String sibling = root.path_join("sibling.ngm");
	Ref<FileAccess> source_file = FileAccess::open(source, FileAccess::WRITE);
	REQUIRE(source_file.is_valid());
	source_file->store_string("source");
	source_file.unref();
	Ref<FileAccess> import_file = FileAccess::open(source + ".import", FileAccess::WRITE);
	REQUIRE(import_file.is_valid());
	import_file->store_string("[remap]\n\npath=\"" + dest + "\"\n");
	import_file.unref();
	Ref<FileAccess> ngm_file = FileAccess::open(imported_ngm, FileAccess::WRITE);
	Ref<FileAccess> sibling_file = FileAccess::open(sibling, FileAccess::WRITE);
	REQUIRE(ngm_file.is_valid());
	REQUIRE(sibling_file.is_valid());
	ngm_file->store_string("ngm");
	sibling_file->store_string("sibling");
	ngm_file.unref();
	sibling_file.unref();

	Dictionary imported_entry;
	imported_entry["ngm"] = imported_ngm;
	Dictionary sibling_entry;
	sibling_entry["ngm"] = sibling;
	Array entries;
	entries.push_back(imported_entry);
	entries.push_back(sibling_entry);
	Dictionary manifest;
	manifest["version"] = 1;
	manifest["source"] = source;
	manifest["entries"] = entries;
	const String manifest_path = dest.get_basename() + ".ngm_manifest.json";
	Ref<FileAccess> manifest_file = FileAccess::open(manifest_path, FileAccess::WRITE);
	REQUIRE(manifest_file.is_valid());
	manifest_file->store_string(JSON::stringify(manifest, "", true));
	manifest_file.unref();

	ngm_delete_imported_sidecars(source);
	CHECK_FALSE(FileAccess::exists(imported_ngm));
	CHECK_FALSE(FileAccess::exists(manifest_path));
	CHECK(FileAccess::exists(sibling));
	CHECK(FileAccess::exists(source + ".import"));
}

#endif // TOOLS_ENABLED

} // namespace TestImporterMeshVirtualGeometry

#endif // _3D_DISABLED
