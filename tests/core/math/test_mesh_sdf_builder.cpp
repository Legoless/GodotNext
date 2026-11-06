/**************************************************************************/
/*  test_mesh_sdf_builder.cpp                                             */
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

TEST_FORCE_LINK(test_mesh_sdf_builder)

#ifndef _3D_DISABLED

#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/math/mesh_sdf_builder.h"
#include "core/object/worker_thread_pool.h"
#include "scene/resources/3d/mesh_distance_field.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/mesh.h"
#include "tests/test_utils.h"

namespace TestMeshSDFBuilder {

static MeshSDFData build_field(const Ref<Mesh> &p_mesh, float p_voxels_per_meter, bool p_two_sided = false, bool p_prefer_static_raycaster = true) {
	MeshSDFSettings settings;
	settings.voxels_per_meter = p_voxels_per_meter;
	settings.two_sided = p_two_sided;
	settings.prefer_static_raycaster = p_prefer_static_raycaster;
	MeshSDFData data;
	Error err = MeshSDFBuilder::build(p_mesh->get_faces(), settings, data);
	REQUIRE(err == OK);
	REQUIRE(data.is_valid());
	REQUIRE(data.get_mip_count() == 3);
	return data;
}

// Compares every stored mip 0 voxel against an analytic signed distance.
// Only voxels whose analytic distance fits inside the stored band can be
// represented exactly; farther voxels must saturate at the band.
template <typename F>
static void check_against_analytic(const MeshSDFData &p_data, F p_analytic) {
	const Vector3i dims = p_data.mip_dimensions[0];
	const float band = p_data.mip_bands[0];
	int checked = 0;
	for (int z = 0; z < dims.z; z++) {
		for (int y = 0; y < dims.y; y++) {
			for (int x = 0; x < dims.x; x++) {
				Vector3i voxel(x, y, z);
				Vector3 center = p_data.bounds.position + (Vector3(voxel) + Vector3(0.5f, 0.5f, 0.5f)) * p_data.voxel_size;
				float expected = p_analytic(center);
				float stored = p_data.get_voxel_distance(0, voxel);
				if (Math::abs(expected) <= band * 0.95f) {
					CHECK(Math::abs(stored - expected) <= 0.5f * p_data.voxel_size);
					checked++;
				} else if (expected > 0.0f) {
					CHECK(stored > 0.0f);
				} else {
					CHECK(stored < 0.0f);
				}
			}
		}
	}
	CHECK(checked > 100);
}

TEST_CASE("[MeshSDF][SceneTree] Sphere matches analytic distance") {
	Ref<SphereMesh> sphere;
	sphere.instantiate();
	sphere->set_radius(0.5f);
	sphere->set_height(1.0f);
	sphere->set_radial_segments(64);
	sphere->set_rings(32);
	MeshSDFData data = build_field(sphere, 16.0f);
	check_against_analytic(data, [](const Vector3 &p) { return p.length() - 0.5f; });
	// The center is inside, a point just outside the surface is not.
	CHECK(data.sample_distance(Vector3()) < 0.0f);
	CHECK(Math::abs(data.sample_distance(Vector3(0.55f, 0.0f, 0.0f)) - 0.05f) <= 0.5f * data.voxel_size);
}

TEST_CASE("[MeshSDF][SceneTree] Box matches analytic distance") {
	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size(Vector3(1.0f, 1.0f, 1.0f));
	MeshSDFData data = build_field(box, 16.0f);
	check_against_analytic(data, [](const Vector3 &p) {
		Vector3 d = p.abs() - Vector3(0.5f, 0.5f, 0.5f);
		float outside = d.maxf(0.0f).length();
		float inside = MIN(MAX(d.x, MAX(d.y, d.z)), 0.0f);
		return outside + inside;
	});
}

TEST_CASE("[MeshSDF][SceneTree] Torus matches analytic distance") {
	Ref<TorusMesh> torus;
	torus.instantiate();
	torus->set_inner_radius(0.25f);
	torus->set_outer_radius(0.75f);
	torus->set_rings(48);
	torus->set_ring_segments(24);
	MeshSDFData data = build_field(torus, 16.0f);
	// Godot's torus lies in the XZ plane: center radius 0.5, tube radius 0.25.
	check_against_analytic(data, [](const Vector3 &p) {
		Vector2 q(Vector2(p.x, p.z).length() - 0.5f, p.y);
		return q.length() - 0.25f;
	});
	// The hole is outside even though geometry surrounds it in the XZ plane.
	CHECK(data.sample_distance(Vector3(0.0f, 0.0f, 0.0f)) > 0.0f);
}

TEST_CASE("[MeshSDF][SceneTree] Open quad has no interior voxels") {
	Ref<PlaneMesh> plane;
	plane.instantiate();
	plane->set_size(Vector2(2.0f, 2.0f));
	MeshSDFData data = build_field(plane, 8.0f);
	const Vector3i dims = data.mip_dimensions[0];
	for (int z = 0; z < dims.z; z++) {
		for (int y = 0; y < dims.y; y++) {
			for (int x = 0; x < dims.x; x++) {
				CHECK(data.get_voxel_distance(0, Vector3i(x, y, z)) >= 0.0f);
			}
		}
	}
	CHECK((data.flags & MeshSDFData::FLAG_UNSIGNED) == 0);
}

TEST_CASE("[MeshSDF][SceneTree] Two-sided plane is an unsigned shell") {
	Ref<PlaneMesh> plane;
	plane.instantiate();
	plane->set_size(Vector2(2.0f, 2.0f));
	MeshSDFData data = build_field(plane, 8.0f, true);
	CHECK((data.flags & MeshSDFData::FLAG_TWO_SIDED) != 0);
	CHECK((data.flags & MeshSDFData::FLAG_UNSIGNED) != 0);
	const Vector3i dims = data.mip_dimensions[0];
	for (int z = 0; z < dims.z; z++) {
		for (int y = 0; y < dims.y; y++) {
			for (int x = 0; x < dims.x; x++) {
				CHECK(data.get_voxel_distance(0, Vector3i(x, y, z)) >= 0.0f);
			}
		}
	}
	CHECK(data.get_brick_count(0) > 0);
	CHECK(Math::abs(data.sample_distance(Vector3(0.0f, 0.0f, 0.0f))) <= 0.5f * data.voxel_size);
}

TEST_CASE("[MeshSDF][SceneTree] Three runs are bit-identical") {
	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size(Vector3(1.0f, 0.6f, 0.8f));
	MeshSDFData runs[3];
	for (int i = 0; i < 3; i++) {
		runs[i] = build_field(box, 12.0f);
	}
	for (int i = 1; i < 3; i++) {
		CHECK(runs[i].bounds == runs[0].bounds);
		CHECK(runs[i].voxel_size == runs[0].voxel_size);
		CHECK(runs[i].flags == runs[0].flags);
		for (int mip = 0; mip < 3; mip++) {
			CHECK(runs[i].mip_dimensions[mip] == runs[0].mip_dimensions[mip]);
			CHECK(runs[i].page_tables[mip] == runs[0].page_tables[mip]);
			CHECK(runs[i].brick_data[mip] == runs[0].brick_data[mip]);
		}
	}
}

TEST_CASE("[MeshSDF][SceneTree] Resource round trip and ArrayMesh attachment") {
	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size(Vector3(1.0f, 1.0f, 1.0f));
	MeshSDFData data = build_field(box, 12.0f);

	Ref<MeshDistanceField> field;
	field.instantiate();
	field->set_data(data);
	CHECK(field->get_mip_count() == 3);
	CHECK(field->get_data_size() == data.get_data_size());
	CHECK(field->get_data_size() > 0);
	CHECK(Math::is_equal_approx(field->sample_distance(Vector3(0.55f, 0.0f, 0.0f)), data.sample_distance(Vector3(0.55f, 0.0f, 0.0f))));

	Ref<ArrayMesh> array_mesh;
	array_mesh.instantiate();
	Array arrays = box->surface_get_arrays(0);
	array_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	CHECK(array_mesh->get_distance_field().is_null());
	array_mesh->set_distance_field(field);
	CHECK(array_mesh->get_distance_field() == field);
	// Mutating the surfaces detaches the now-stale field.
	array_mesh->clear_surfaces();
	CHECK(array_mesh->get_distance_field().is_null());
}

TEST_CASE("[MeshSDF][SceneTree] TriangleMesh sign fallback matches analytic box") {
	// Same analytic check as the default (StaticRaycaster when available)
	// path, forced through TriangleMesh::intersect_ray for the sign rays.
	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size(Vector3(1.0f, 1.0f, 1.0f));
	MeshSDFData data = build_field(box, 16.0f, false, false);
	check_against_analytic(data, [](const Vector3 &p) {
		Vector3 d = p.abs() - Vector3(0.5f, 0.5f, 0.5f);
		float outside = d.maxf(0.0f).length();
		float inside = MIN(MAX(d.x, MAX(d.y, d.z)), 0.0f);
		return outside + inside;
	});
}

static float box_distance(const Vector3 &p, const Vector3 &p_center) {
	Vector3 d = (p - p_center).abs() - Vector3(0.5f, 0.5f, 0.5f);
	return d.maxf(0.0f).length() + MIN(MAX(d.x, MAX(d.y, d.z)), 0.0f);
}

TEST_CASE("[MeshSDF][SceneTree] Intersecting kitbash boxes keep a union interior") {
	// Two unit boxes overlapped into one triangle soup. The magnitude near
	// the internal walls is the distance to those walls, so this checks the
	// sign: every voxel's sign must match the analytic union, away from the
	// surface itself.
	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size(Vector3(1.0f, 1.0f, 1.0f));
	Vector<Face3> faces = box->get_faces();
	const Vector3 offset(0.5f, 0.25f, 0.0f);
	int base_count = faces.size();
	faces.resize(base_count * 2);
	for (int i = 0; i < base_count; i++) {
		Face3 moved = faces[i];
		for (int j = 0; j < 3; j++) {
			moved.vertex[j] += offset;
		}
		faces.write[base_count + i] = moved;
	}
	MeshSDFSettings settings;
	settings.voxels_per_meter = 16.0f;
	MeshSDFData data;
	REQUIRE(MeshSDFBuilder::build(faces, settings, data) == OK);
	const Vector3i dims = data.mip_dimensions[0];
	int checked = 0;
	for (int z = 0; z < dims.z; z++) {
		for (int y = 0; y < dims.y; y++) {
			for (int x = 0; x < dims.x; x++) {
				Vector3i voxel(x, y, z);
				Vector3 center = data.bounds.position + (Vector3(voxel) + Vector3(0.5f, 0.5f, 0.5f)) * data.voxel_size;
				float expected = MIN(box_distance(center, Vector3()), box_distance(center, offset));
				if (Math::abs(expected) <= data.voxel_size) {
					continue; // On the surface; sign is not meaningful.
				}
				float stored = data.get_voxel_distance(0, voxel);
				CHECK((stored < 0.0f) == (expected < 0.0f));
				checked++;
			}
		}
	}
	CHECK(checked > 1000);
	// Overlap, each box only, and outside both.
	CHECK(data.sample_distance(Vector3(0.25f, 0.1f, 0.0f)) < 0.0f);
	CHECK(data.sample_distance(Vector3(-0.3f, -0.3f, 0.0f)) < 0.0f);
	CHECK(data.sample_distance(Vector3(0.8f, 0.5f, 0.0f)) < 0.0f);
	CHECK(data.sample_distance(Vector3(0.25f, -0.8f, 0.0f)) > 0.0f);
}

TEST_CASE("[MeshSDF][SceneTree] Distance field survives saving the mesh as an external file") {
	// The importer's save_to_file path relies on this: a mesh saved after the
	// field is attached (the importer re-saves in that case) loads back with
	// the field intact.
	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size(Vector3(1.0f, 1.0f, 1.0f));
	MeshSDFData data = build_field(box, 12.0f);
	Ref<MeshDistanceField> field;
	field.instantiate();
	field->set_data(data);

	Ref<ArrayMesh> array_mesh;
	array_mesh.instantiate();
	array_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, box->surface_get_arrays(0));
	array_mesh->set_distance_field(field);

	const String path = TestUtils::get_temp_path("mesh_sdf_save_to_file.tres");
	Error err = ResourceSaver::save(array_mesh, path);
	REQUIRE(err == OK);

	Ref<ArrayMesh> loaded = ResourceLoader::load(path, "ArrayMesh", ResourceFormatLoader::CACHE_MODE_IGNORE, &err);
	REQUIRE(err == OK);
	REQUIRE(loaded.is_valid());
	REQUIRE(loaded->get_distance_field().is_valid());
	CHECK(loaded->get_distance_field()->get_data_size() == data.get_data_size());
	CHECK(loaded->get_distance_field()->get_mip_count() == 3);
	CHECK(Math::is_equal_approx(loaded->get_distance_field()->sample_distance(Vector3(0.55f, 0.0f, 0.0f)), data.sample_distance(Vector3(0.55f, 0.0f, 0.0f))));
	CHECK(loaded->get_distance_field()->sample_distance(Vector3()) < 0.0f);
}

TEST_CASE("[MeshSDF][SceneTree] Empty input fails without a field") {
	MeshSDFSettings settings;
	MeshSDFData data;
	Error err = MeshSDFBuilder::build(Vector<Face3>(), settings, data);
	CHECK(err != OK);
	CHECK_FALSE(data.is_valid());
}

struct ConcurrentBuild {
	const Vector<Face3> *faces = nullptr;
	MeshSDFSettings settings;
	MeshSDFData data;
	Error error = FAILED;
};

static void run_concurrent_build(void *p_userdata) {
	ConcurrentBuild *build = static_cast<ConcurrentBuild *>(p_userdata);
	build->error = MeshSDFBuilder::build(*build->faces, build->settings, build->data);
}

TEST_CASE("[MeshSDF][SceneTree] Concurrent builds are independent") {
	Ref<BoxMesh> box;
	box.instantiate();
	const Vector<Face3> faces = box->get_faces();
	constexpr int BUILD_COUNT = 16;
	ConcurrentBuild builds[BUILD_COUNT];
	WorkerThreadPool::TaskID tasks[BUILD_COUNT];
	for (int i = 0; i < BUILD_COUNT; i++) {
		builds[i].faces = &faces;
		builds[i].settings.voxels_per_meter = 8.0f;
		tasks[i] = WorkerThreadPool::get_singleton()->add_native_task(&run_concurrent_build, &builds[i], false, "Concurrent mesh SDF test");
	}
	for (int i = 0; i < BUILD_COUNT; i++) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(tasks[i]);
		CHECK(builds[i].error == OK);
		CHECK(builds[i].data.is_valid());
		CHECK(builds[i].data.sample_distance(Vector3()) < 0.0f);
	}
}

} // namespace TestMeshSDFBuilder

#endif // _3D_DISABLED
