/**************************************************************************/
/*  test_dummy_voxel_gi.cpp                                               */
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

TEST_FORCE_LINK(test_dummy_voxel_gi)

#ifndef _3D_DISABLED

#include "scene/3d/voxel_gi.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/rendering/dummy/environment/gi.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"
#include "tests/test_tools.h"

namespace TestDummyVoxelGI {

// The doctest harness runs the dummy renderer, as headless exports do.
struct BakedData {
	Transform3D to_cell_xform = Transform3D(Basis().scaled(Vector3(2, 2, 2)), Vector3(4, 2, 4));
	AABB bounds = AABB(Vector3(-2, -1, -2), Vector3(4, 2, 4));
	Vector3 octree_size = Vector3(4, 2, 4);
	Vector<uint8_t> octree_cells;
	Vector<uint8_t> data_cells;
	Vector<uint8_t> distance_field;
	Vector<int> level_counts;

	BakedData() {
		for (int i = 0; i < 64; i++) {
			octree_cells.push_back(uint8_t(i * 3));
			data_cells.push_back(uint8_t(255 - i));
		}
		// One Lum8 texel per cell: octree_size.x * octree_size.y by octree_size.z.
		for (int i = 0; i < 4 * 2 * 4; i++) {
			distance_field.push_back(uint8_t(i * 7));
		}
		level_counts.push_back(1);
		level_counts.push_back(3);
	}

	Ref<VoxelGIData> make() const {
		Ref<VoxelGIData> data;
		data.instantiate();
		data->allocate(to_cell_xform, bounds, octree_size, octree_cells, data_cells, distance_field, level_counts);
		return data;
	}

	void check(const Ref<VoxelGIData> &p_data) const {
		CHECK(p_data->get_to_cell_xform().is_equal_approx(to_cell_xform));
		CHECK(p_data->get_bounds().is_equal_approx(bounds));
		CHECK(p_data->get_octree_size() == octree_size);
		CHECK(p_data->get_octree_cells() == octree_cells);
		CHECK(p_data->get_data_cells() == data_cells);
		CHECK(p_data->get_distance_field() == distance_field);
		CHECK(p_data->get_level_counts() == level_counts);
	}
};

TEST_CASE("[SceneTree][VoxelGI] The dummy renderer keeps VoxelGIData's baked data") {
	const BakedData baked;
	Ref<VoxelGIData> data = baked.make();
	baked.check(data);

	const RID rid = data->get_rid();
	CHECK(RendererDummy::GI::get_singleton()->owns_voxel_gi(rid));
	CHECK(RSG::gi->voxel_gi_get_bounds(rid).is_equal_approx(baked.bounds));
	CHECK(RSG::gi->voxel_gi_get_octree_size(rid) == Vector3i(baked.octree_size));

	data.unref();
	CHECK_FALSE(RendererDummy::GI::get_singleton()->owns_voxel_gi(rid));
}

TEST_CASE("[SceneTree][VoxelGI] VoxelGIData saves and loads its data on the dummy renderer") {
	const BakedData baked;
	Ref<VoxelGIData> data = baked.make();

	// Saving reads the data back from the rendering server (VoxelGIData::_get_data).
	ErrorDetector ed;
	const Dictionary stored = data->get("_data");
	CHECK_FALSE(ed.has_error);
	REQUIRE(stored.has("octree_df_png"));
	CHECK_FALSE(Vector<uint8_t>(stored["octree_df_png"]).is_empty());
	CHECK(Vector<uint8_t>(stored["octree_cells"]) == baked.octree_cells);
	CHECK(Vector<uint8_t>(stored["octree_data"]) == baked.data_cells);

	Ref<VoxelGIData> loaded;
	loaded.instantiate();
	loaded->set("_data", stored);
	CHECK_FALSE(ed.has_error);
	baked.check(loaded);
}

TEST_CASE("[SceneTree][VoxelGI] A VoxelGI instance takes the dummy VoxelGIData as its base") {
	const BakedData baked;
	Ref<VoxelGIData> data = baked.make();
	CHECK(RSG::utilities->get_base_type(data->get_rid()) == RSE::INSTANCE_VOXEL_GI);

	Window *root = SceneTree::get_singleton()->get_root();
	VoxelGI *voxel_gi = memnew(VoxelGI);
	ErrorDetector ed;
	voxel_gi->set_probe_data(data);
	root->add_child(voxel_gi);
	SceneTree::get_singleton()->process(0.016);
	CHECK_FALSE(ed.has_error);
	memdelete(voxel_gi);

	// Freeing the data while an instance still uses it detaches the instance.
	const RID instance = RS::get_singleton()->instance_create2(data->get_rid(), root->get_world_3d()->get_scenario());
	data.unref();
	SceneTree::get_singleton()->process(0.016);
	RS::get_singleton()->free_rid(instance);
	CHECK_FALSE(ed.has_error);
}

} // namespace TestDummyVoxelGI

#endif // _3D_DISABLED
