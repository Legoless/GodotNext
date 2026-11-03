/**************************************************************************/
/*  test_gpu_particles_collision_sdf.cpp                                  */
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

TEST_FORCE_LINK(test_gpu_particles_collision_sdf)

#ifndef _3D_DISABLED

#include "scene/3d/gpu_particles_collision_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/node_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/primitive_meshes.h"

namespace TestGPUParticlesCollisionSDF {

// FNV-1a over the baked image. The expected value pins the byte output of
// GPUParticlesCollisionSDF3D::bake() across the BVH extraction in #184
// (the node's BVH moved into core/math/mesh_sdf_builder); the only
// intended numeric change in that work is the face-centroid /3 fix, which
// is already included in this baseline.
static uint64_t bake_hash(const Ref<Image> &p_image) {
	uint64_t hash = 1469598103934665603ULL;
	auto mix_byte = [&](uint8_t p_byte) {
		hash ^= p_byte;
		hash *= 1099511628211ULL;
	};
	auto mix_int = [&](int32_t p_value) {
		for (int i = 0; i < 4; i++) {
			mix_byte(uint8_t((uint32_t(p_value) >> (i * 8)) & 0xff));
		}
	};
	mix_int(p_image->get_width());
	mix_int(p_image->get_height());
	mix_int(int32_t(p_image->get_format()));
	PackedByteArray data = p_image->get_data();
	for (int i = 0; i < data.size(); i++) {
		mix_byte(data[i]);
	}
	return hash;
}

TEST_CASE("[MeshSDF][SceneTree] GPUParticlesCollisionSDF3D bake output is byte-identical after the BVH extraction") {
	Node3D *root = memnew(Node3D);
	SceneTree::get_singleton()->get_root()->add_child(root);

	MeshInstance3D *mesh_instance = memnew(MeshInstance3D);
	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size(Vector3(1.0f, 0.8f, 0.6f));
	mesh_instance->set_mesh(box);
	mesh_instance->set_position(Vector3(0.2f, -0.1f, 0.05f));
	root->add_child(mesh_instance);

	GPUParticlesCollisionSDF3D *collision = memnew(GPUParticlesCollisionSDF3D);
	collision->set_size(Vector3(2.0f, 2.0f, 2.0f));
	collision->set_resolution(GPUParticlesCollisionSDF3D::RESOLUTION_32);
	collision->set_thickness(1.0f);
	root->add_child(collision);

	Ref<Image> baked = collision->bake();
	REQUIRE(baked.is_valid());
	const uint64_t hash = bake_hash(baked);
	// Baseline recorded before the extraction, with the centroid fix in place.
	CHECK(hash == 884518523704196648ULL);

	SceneTree::get_singleton()->get_root()->remove_child(root);
	memdelete(root);
}

} // namespace TestGPUParticlesCollisionSDF

#endif // _3D_DISABLED
