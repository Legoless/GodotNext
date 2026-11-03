/**************************************************************************/
/*  test_virtual_geometry_data.cpp                                        */
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

TEST_FORCE_LINK(test_virtual_geometry_data)

#ifndef _3D_DISABLED

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_format_ngm.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "scene/resources/3d/virtual_geometry_data.h"
#include "scene/resources/mesh.h"
#include "tests/test_utils.h"

#include "modules/modules_enabled.gen.h"

#ifdef MODULE_NEXTGEO_ENABLED
#include "core/crypto/crypto_core.h"

#include "modules/nextgeo/nextgeo_dag_builder.h"
#include "modules/nextgeo/nextgeo_serialize.h"

#include <cstring>
#endif

namespace TestVirtualGeometryData {

Array triangle_arrays() {
	PackedVector3Array vertices;
	vertices.push_back(Vector3(0, 0, 0));
	vertices.push_back(Vector3(1, 0, 0));
	vertices.push_back(Vector3(0, 1, 0));
	PackedVector3Array normals;
	normals.push_back(Vector3(0, 0, 1));
	normals.push_back(Vector3(0, 0, 1));
	normals.push_back(Vector3(0, 0, 1));
	PackedVector2Array uvs;
	uvs.push_back(Vector2(0, 0));
	uvs.push_back(Vector2(1, 0));
	uvs.push_back(Vector2(0, 1));
	PackedInt32Array indices;
	indices.push_back(0);
	indices.push_back(1);
	indices.push_back(2);

	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_INDEX] = indices;
	return arrays;
}

Ref<ArrayMesh> triangle_mesh() {
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, triangle_arrays());
	return mesh;
}

Ref<VirtualGeometryData> filled_data() {
	Ref<VirtualGeometryData> data;
	data.instantiate();
	PackedByteArray header;
	header.push_back(1);
	header.push_back(2);
	header.push_back(3);
	header.push_back(4);
	data->set_dag_header(header);
	data->set_surface_eligibility(PackedByteArray({ 1, 0 }));
	data->set_surface_fallback_reasons(PackedInt32Array({ 0, 0 }));
	data->set_rt_proxy_positions(PackedFloat32Array({ 1.0f, 2.0f, 3.0f }));
	data->set_rt_proxy_indices(PackedByteArray({ 4, 0, 0, 0 }));
	data->set_rt_proxy_eps(0.25f);
	data->set_rt_proxy_error(1.5f);
	data->set_ngm_path("res://mesh.ngm");
	PackedByteArray sha;
	sha.resize(32);
	sha.fill(0xAB);
	data->set_ngm_sha256(sha);
	data->set_source_triangle_count(1);
	data->set_page_size(NGM_PROVISIONAL_PAGE_SIZE);
	return data;
}

TEST_CASE("[NextGeo][VGD] Surface mutation detaches the data") {
	Ref<ArrayMesh> mesh = triangle_mesh();
	Ref<VirtualGeometryData> data;
	data.instantiate();
	mesh->set_virtual_geometry(data);
	REQUIRE(mesh->get_virtual_geometry().is_valid());

	SUBCASE("add_surface_from_arrays") {
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, triangle_arrays());
		CHECK(mesh->get_virtual_geometry().is_null());
	}
	SUBCASE("surface_update_vertex_region") {
		PackedByteArray region;
		region.resize(12);
		mesh->surface_update_vertex_region(0, 0, region);
		CHECK(mesh->get_virtual_geometry().is_null());
	}
	SUBCASE("surface_update_attribute_region") {
		PackedByteArray region;
		region.resize(8);
		mesh->surface_update_attribute_region(0, 0, region);
		CHECK(mesh->get_virtual_geometry().is_null());
	}
	SUBCASE("surface_update_skin_region") {
		PackedByteArray region;
		region.resize(4);
		mesh->surface_update_skin_region(0, 0, region);
		CHECK(mesh->get_virtual_geometry().is_null());
	}
	SUBCASE("clear_surfaces") {
		mesh->clear_surfaces();
		CHECK(mesh->get_virtual_geometry().is_null());
	}
	SUBCASE("surface_remove") {
		mesh->surface_remove(0);
		CHECK(mesh->get_virtual_geometry().is_null());
	}
	SUBCASE("regen_normal_maps") {
		ERR_PRINT_OFF;
		mesh->regen_normal_maps();
		ERR_PRINT_ON;
		CHECK(mesh->get_virtual_geometry().is_null());
	}
	SUBCASE("lightmap_unwrap") {
		ERR_PRINT_OFF;
		mesh->lightmap_unwrap();
		ERR_PRINT_ON;
		CHECK(mesh->get_virtual_geometry().is_null());
	}
	SUBCASE("lightmap_unwrap_cached") {
		Vector<uint8_t> cache;
		ERR_PRINT_OFF;
		mesh->lightmap_unwrap_cached(Transform3D(), 0.05f, cache, cache, false);
		ERR_PRINT_ON;
		CHECK(mesh->get_virtual_geometry().is_null());
	}
}

TEST_CASE("[NextGeo][VGD] Loading surfaces keeps the data, and a saved mesh reloads") {
	Ref<ArrayMesh> mesh = triangle_mesh();
	Ref<VirtualGeometryData> data = filled_data();
	mesh->set_virtual_geometry(data);

	mesh->set("_surfaces", mesh->get("_surfaces"));
	CHECK(mesh->get_virtual_geometry() == data);

	const String extensions[2] = { "res", "tres" };
	for (const String &extension : extensions) {
		const String path = TestUtils::get_temp_path("ngm72_mesh." + extension);
		REQUIRE(ResourceSaver::save(mesh, path) == OK);
		Ref<ArrayMesh> loaded = ResourceLoader::load(path, "", ResourceFormatLoader::CACHE_MODE_IGNORE);
		REQUIRE(loaded.is_valid());
		Ref<VirtualGeometryData> loaded_data = loaded->get_virtual_geometry();
		REQUIRE(loaded_data.is_valid());
		CHECK(loaded_data->get_format_version() == VirtualGeometryData::FORMAT_VERSION);
		CHECK(loaded_data->get_dag_header() == data->get_dag_header());
		CHECK(loaded_data->get_surface_eligibility() == data->get_surface_eligibility());
		CHECK(loaded_data->get_surface_fallback_reasons() == data->get_surface_fallback_reasons());
		CHECK(loaded_data->get_surface_fallback_reasons()[0] == 0);
		CHECK(loaded_data->get_rt_proxy_positions() == data->get_rt_proxy_positions());
		CHECK(loaded_data->get_rt_proxy_indices() == data->get_rt_proxy_indices());
		CHECK(loaded_data->get_rt_proxy_eps() == doctest::Approx(0.25f));
		CHECK(loaded_data->get_rt_proxy_error() == doctest::Approx(1.5f));
		CHECK(loaded_data->get_ngm_path() == data->get_ngm_path());
		CHECK(loaded_data->get_ngm_sha256() == data->get_ngm_sha256());
		CHECK(loaded_data->get_source_triangle_count() == 1);
		CHECK(loaded_data->get_page_size() == NGM_PROVISIONAL_PAGE_SIZE);
		loaded->clear_surfaces();
		CHECK(loaded->get_virtual_geometry().is_null());
		DirAccess::remove_absolute(path);
	}
}

#ifdef MODULE_NEXTGEO_ENABLED

void add_triangle(NextGeoSurfaceInput &r_surface, const Vector3 &p_origin) {
	r_surface.positions.push_back(p_origin);
	r_surface.positions.push_back(p_origin + Vector3(1, 0, 0));
	r_surface.positions.push_back(p_origin + Vector3(0, 1, 0));
	for (int i = 0; i < 3; i++) {
		r_surface.normals.push_back(Vector3(0, 0, 1));
		r_surface.uv0.push_back(Vector2(float(i), 0));
	}
	const int base = 0;
	r_surface.indices.push_back(base);
	r_surface.indices.push_back(base + 1);
	r_surface.indices.push_back(base + 2);
}

TEST_CASE("[NextGeo][VGD] Build output fills the resource and a deterministic .ngm") {
	NGMHandlePool::get_singleton().clear();

	NextGeoBuildInput input;
	NextGeoSurfaceInput eligible;
	add_triangle(eligible, Vector3());
	NextGeoSurfaceInput ineligible;
	add_triangle(ineligible, Vector3(3, 0, 0));
	ineligible.eligible = false;
	input.surfaces.push_back(eligible);
	input.surfaces.push_back(ineligible);
	input.page_size = NGM_PROVISIONAL_PAGE_SIZE;
	input.use_threads = false;

	NextGeoBuildOutput output;
	REQUIRE(nextgeo_build_dag(input, output) == OK);

	Vector<Vector<uint8_t>> payloads;
	REQUIRE(nextgeo_write_pages(output, payloads) == OK);
	REQUIRE(payloads.size() == int(output.pages.size()));

	Ref<VirtualGeometryData> data;
	data.instantiate();
	REQUIRE(nextgeo_fill_vgd(input, output, payloads, data) == OK);
	CHECK(data->get_format_version() == 1);
	CHECK(data->get_page_size() == NGM_PROVISIONAL_PAGE_SIZE);
	CHECK(data->get_page_count() == int64_t(output.pages.size()));
	CHECK(data->get_cluster_count() == int64_t(output.clusters.size()));
	CHECK(data->get_group_count() == int64_t(output.groups.size()));
	CHECK(data->get_dag_depth() == int64_t(output.dag_depth));
	CHECK(data->get_source_triangle_count() == int64_t(output.source_triangle_count));
	CHECK(data->get_source_vertex_index_bytes() == 96);
	CHECK(data->get_surface_eligibility() == PackedByteArray({ 1, 0 }));
	CHECK(data->get_surface_fallback_reasons() == PackedInt32Array({ 0, 0 }));
	CHECK(data->get_root_pages().size() == int(output.root_page_count));
	if (output.root_page_count > 0) {
		CHECK(PackedByteArray(data->get_root_pages()[0]) == payloads[0]);
	}

	TypedArray<Array> fallback = data->get_coarse_fallback();
	REQUIRE(fallback.size() == 2);
	Array eligible_arrays = fallback[0];
	PackedVector3Array fallback_vertices = eligible_arrays[Mesh::ARRAY_VERTEX];
	CHECK(fallback_vertices.size() > 0);
	Array ineligible_arrays = fallback[1];
	CHECK(ineligible_arrays[Mesh::ARRAY_VERTEX].get_type() == Variant::NIL);

	NGMHeader header;
	REQUIRE(nextgeo_fill_ngm_header(output, payloads, header) == OK);
	CHECK(header.flags == 0);
	CHECK(header.page_size == NGM_PROVISIONAL_PAGE_SIZE);

	Vector<NGMPage> pages;
	pages.resize(payloads.size());
	for (int i = 0; i < payloads.size(); i++) {
		pages.write[i].bytes = payloads[i];
		pages.write[i].first_group = output.pages[i].group_offset;
		pages.write[i].group_count = output.pages[i].group_count;
	}
	const String path_a = TestUtils::get_temp_path("ngm72_dag_a.ngm");
	const String path_b = TestUtils::get_temp_path("ngm72_dag_b.ngm");
	REQUIRE(NGMWriter::write(path_a, header, pages) == OK);
	REQUIRE(NGMWriter::write(path_b, header, pages) == OK);
	const Vector<uint8_t> file_a = FileAccess::get_file_as_bytes(path_a);
	const Vector<uint8_t> file_b = FileAccess::get_file_as_bytes(path_b);
	CHECK(file_a == file_b);
	const String sha = FileAccess::get_sha256(path_a);
	CHECK(sha == FileAccess::get_sha256(path_b));
	CHECK(sha.length() == 64);
	REQUIRE(file_a.size() >= 88);
	CHECK(uint32_t(file_a[12]) == 0);
	CHECK((uint32_t(file_a[16]) | (uint32_t(file_a[17]) << 8) | (uint32_t(file_a[18]) << 16) | (uint32_t(file_a[19]) << 24)) == NGM_PROVISIONAL_PAGE_SIZE);

	CryptoCore::SHA256Context sha_ctx;
	REQUIRE(sha_ctx.start() == OK);
	for (int i = 0; i < payloads.size(); i++) {
		if (!payloads[i].is_empty()) {
			REQUIRE(sha_ctx.update(payloads[i].ptr(), payloads[i].size()) == OK);
		}
	}
	uint8_t digest[32];
	REQUIRE(sha_ctx.finish(digest) == OK);
	CHECK(memcmp(digest, header.source_sha256, 32) == 0);
	CHECK(memcmp(digest, file_a.ptr() + 56, 32) == 0);

	Ref<NGMFile> file;
	file.instantiate();
	REQUIRE(file->open(path_a) == OK);
	CHECK(file->get_page_count() == uint32_t(payloads.size()));
	for (int i = 0; i < payloads.size(); i++) {
		Vector<uint8_t> read_back;
		REQUIRE(file->read_page(i, read_back) == OK);
		CHECK(read_back == payloads[i]);
	}

	DirAccess::remove_absolute(path_a);
	DirAccess::remove_absolute(path_b);
	NGMHandlePool::get_singleton().clear();
}

uint32_t read_u32(const Vector<uint8_t> &p_bytes, int p_offset) {
	return uint32_t(p_bytes[p_offset]) | (uint32_t(p_bytes[p_offset + 1]) << 8) | (uint32_t(p_bytes[p_offset + 2]) << 16) | (uint32_t(p_bytes[p_offset + 3]) << 24);
}

uint64_t read_u64(const Vector<uint8_t> &p_bytes, int p_offset) {
	uint64_t value = 0;
	for (int i = 0; i < 8; i++) {
		value |= uint64_t(p_bytes[p_offset + i]) << (i * 8);
	}
	return value;
}

NextGeoSurfaceInput make_grid(int p_cells) {
	NextGeoSurfaceInput surface;
	for (int z = 0; z <= p_cells; z++) {
		for (int x = 0; x <= p_cells; x++) {
			surface.positions.push_back(Vector3(x, 0, z));
			surface.normals.push_back(Vector3(0, 1, 0));
			surface.uv0.push_back(Vector2(float(x), float(z)));
		}
	}
	for (int z = 0; z < p_cells; z++) {
		for (int x = 0; x < p_cells; x++) {
			const int row = z * (p_cells + 1) + x;
			const int next_row = row + p_cells + 1;
			surface.indices.push_back(row);
			surface.indices.push_back(row + 1);
			surface.indices.push_back(next_row);
			surface.indices.push_back(row + 1);
			surface.indices.push_back(next_row + 1);
			surface.indices.push_back(next_row);
		}
	}
	return surface;
}

void check_written_pages_fit(uint32_t p_page_size, const String &p_path, bool p_expect_split) {
	NextGeoBuildInput input;
	input.surfaces.push_back(make_grid(16));
	input.page_size = p_page_size;
	input.use_threads = false;

	NextGeoBuildOutput output;
	REQUIRE(nextgeo_build_dag(input, output) == OK);
	CHECK(output.vertex_stride == 12);
	if (p_expect_split) {
		CHECK(output.pages.size() > 1);
	}

	Vector<Vector<uint8_t>> payloads;
	REQUIRE(nextgeo_write_pages(output, payloads) == OK);
	REQUIRE(payloads.size() == int(output.pages.size()));

	NGMHeader header;
	REQUIRE(nextgeo_fill_ngm_header(output, payloads, header) == OK);
	CHECK(header.page_size == p_page_size);

	Vector<NGMPage> pages;
	pages.resize(payloads.size());
	for (int i = 0; i < payloads.size(); i++) {
		const uint32_t raw = uint32_t(payloads[i].size());
		CHECK(raw <= header.page_size);
		const uint32_t remainder = raw % NGM_PAGE_ALIGN;
		const uint32_t pad = remainder == 0 ? 0 : NGM_PAGE_ALIGN - remainder;
		CHECK(raw + pad == output.pages[i].byte_size);
		CHECK(output.pages[i].byte_size <= header.page_size);
		pages.write[i].bytes = payloads[i];
		pages.write[i].first_group = output.pages[i].group_offset;
		pages.write[i].group_count = output.pages[i].group_count;
	}

	REQUIRE(NGMWriter::write(p_path, header, pages) == OK);
	const Vector<uint8_t> file = FileAccess::get_file_as_bytes(p_path);
	REQUIRE(file.size() >= int(NGM_HEADER_SIZE + 8));
	const uint64_t table = read_u64(file, 48);
	REQUIRE(table + uint64_t(payloads.size()) * NGM_PAGE_TABLE_ENTRY_SIZE <= uint64_t(file.size()));
	for (int i = 0; i < payloads.size(); i++) {
		const int entry = int(table) + i * int(NGM_PAGE_TABLE_ENTRY_SIZE);
		const uint32_t raw_size = read_u32(file, entry + 12);
		CHECK(raw_size <= header.page_size);
		CHECK(raw_size == uint32_t(payloads[i].size()));
	}
	DirAccess::remove_absolute(p_path);
	NGMHandlePool::get_singleton().clear();
}

TEST_CASE("[NextGeo][VGD] Every stored page fits in page_size") {
	NGMHandlePool::get_singleton().clear();
	check_written_pages_fit(NGM_PROVISIONAL_PAGE_SIZE, TestUtils::get_temp_path("ngm72_pages_64k.ngm"), false);
	const uint32_t one_cluster_page = nextgeo_page_bytes(nextgeo_cluster_payload_bytes(NEXTGEO_MAX_CLUSTER_VERTICES, NEXTGEO_MAX_CLUSTER_TRIANGLES, 12));
	check_written_pages_fit(one_cluster_page, TestUtils::get_temp_path("ngm72_pages_small.ngm"), true);
}

#endif // MODULE_NEXTGEO_ENABLED

} // namespace TestVirtualGeometryData

#endif // _3D_DISABLED
