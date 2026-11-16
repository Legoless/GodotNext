/**************************************************************************/
/*  test_mesh_instance_3d.cpp                                             */
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

TEST_FORCE_LINK(test_mesh_instance_3d)

#ifndef _3D_DISABLED

#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "servers/rendering/dummy/storage/material_storage.h"
#include "servers/rendering/rendering_server.h"
#include "tests/test_tools.h"

namespace TestMeshInstance3D {

// The doctest harness runs the dummy renderer, as headless runs do (#278).
static Ref<ArrayMesh> make_two_surface_mesh() {
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	for (int surface = 0; surface < 2; surface++) {
		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = PackedVector3Array({ Vector3(0, 0, surface), Vector3(1, 0, surface), Vector3(0, 1, surface) });
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	}
	return mesh;
}

// Gives every surface of p_node a material of its own: p_node holds the only reference to each.
static LocalVector<RID> set_own_override_materials(MeshInstance3D *p_node) {
	LocalVector<RID> rids;
	for (int surface = 0; surface < p_node->get_surface_override_material_count(); surface++) {
		Ref<StandardMaterial3D> material;
		material.instantiate();
		p_node->set_surface_override_material(surface, material);
		rids.push_back(material->get_rid());
	}
	return rids;
}

TEST_CASE("[SceneTree][MeshInstance3D] Freeing a mesh instance frees its own surface override materials without an error") {
	// The test keeps its own reference to the mesh: only the materials are freed with the node.
	Ref<ArrayMesh> mesh = make_two_surface_mesh();
	RendererDummy::MaterialStorage *storage = RendererDummy::MaterialStorage::get_singleton();
	REQUIRE(storage != nullptr);

	MeshInstance3D *node = memnew(MeshInstance3D);
	node->set_mesh(mesh);
	REQUIRE(node->get_surface_override_material_count() == 2);
	const LocalVector<RID> rids = set_own_override_materials(node);
	REQUIRE(rids.size() == 2);
	for (const RID &rid : rids) {
		CHECK(storage->owns_material(rid));
	}

	SUBCASE("Never in the tree") {
		// No frame has updated the node's rendering instance, so freeing it applies the pending update.
		ErrorDetector ed;
		memdelete(node);
		CHECK_FALSE(ed.has_error);
	}

	SUBCASE("Freed in the tree before a frame") {
		SceneTree::get_singleton()->get_root()->add_child(node);
		ErrorDetector ed;
		memdelete(node);
		CHECK_FALSE(ed.has_error);
	}

	SUBCASE("Mesh cleared first") {
		// Clearing the mesh keeps the override materials on the node, while the rendering instance
		// drops its surfaces.
		node->set_mesh(Ref<Mesh>());
		CHECK(node->get_surface_override_material_count() == 2);
		ErrorDetector ed;
		memdelete(node);
		CHECK_FALSE(ed.has_error);
	}

	// VisualInstance3D::set_base() is bound, so a script can change the base under the node, which
	// clears the rendering instance's materials while the node keeps its mesh and override materials.
	SUBCASE("Base cleared by set_base()") {
		ErrorDetector ed_set;
		node->set_base(RID());
		CHECK_FALSE(ed_set.has_error);
		ErrorDetector ed;
		memdelete(node);
		CHECK_FALSE(ed.has_error);
	}

	SUBCASE("Base changed to a multimesh by set_base()") {
		const RID multimesh = RS::get_singleton()->multimesh_create();
		ErrorDetector ed_set;
		node->set_base(multimesh);
		CHECK_FALSE(ed_set.has_error);
		ErrorDetector ed;
		memdelete(node);
		CHECK_FALSE(ed.has_error);
		RS::get_singleton()->free_rid(multimesh);
	}

	for (const RID &rid : rids) {
		CHECK_FALSE(storage->owns_material(rid));
	}
}

} // namespace TestMeshInstance3D

#endif // _3D_DISABLED
