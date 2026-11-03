/**************************************************************************/
/*  test_dummy_lightmap.cpp                                               */
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

TEST_FORCE_LINK(test_dummy_lightmap)

#ifndef _3D_DISABLED

#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "scene/3d/lightmap_gi.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/packed_scene.h"
#include "servers/rendering/dummy/storage/light_storage.h"
#include "servers/rendering/rendering_server_globals.h"
#include "tests/test_tools.h"
#include "tests/test_utils.h"

namespace TestDummyLightmap {

// The doctest harness runs the dummy renderer, as headless exports do.
struct CaptureData {
	AABB bounds = AABB(Vector3(-2, -1, -2), Vector3(4, 2, 4));
	bool interior = true;
	PackedVector3Array points;
	PackedColorArray point_sh;
	PackedInt32Array tetrahedra;
	PackedInt32Array bsp_tree;
	float baked_exposure = 2.5f;
	uint32_t lightprobe_hash = 0x12345678;

	CaptureData() {
		// One tetrahedron of probes.
		points.push_back(Vector3(0, 0, 0));
		points.push_back(Vector3(1, 0, 0));
		points.push_back(Vector3(0, 1, 0));
		points.push_back(Vector3(0, 0, 1));
		// Nine SH colors per probe, each one distinct.
		for (int i = 0; i < points.size() * 9; i++) {
			point_sh.push_back(Color(i / 36.0f, (i % 9) / 9.0f, 0.25f, 1.0f));
		}
		tetrahedra.push_back(0);
		tetrahedra.push_back(1);
		tetrahedra.push_back(2);
		tetrahedra.push_back(3);
		// One BSP node: a plane (1.0f as bits, 0, 0, 0), an empty leaf over, leaf 0 under.
		bsp_tree.push_back(1065353216);
		bsp_tree.push_back(0);
		bsp_tree.push_back(0);
		bsp_tree.push_back(0);
		bsp_tree.push_back(INT32_MIN);
		bsp_tree.push_back(-1);
	}

	Ref<LightmapGIData> make() const {
		Ref<LightmapGIData> data;
		data.instantiate();
		data->set_capture_data(bounds, interior, points, point_sh, tetrahedra, bsp_tree, baked_exposure, lightprobe_hash);
		return data;
	}

	void check(const Ref<LightmapGIData> &p_data) const {
		REQUIRE(p_data.is_valid());
		CHECK(p_data->get_capture_bounds().is_equal_approx(bounds));
		CHECK(p_data->is_interior() == interior);
		CHECK(p_data->get_capture_points() == points);
		CHECK(p_data->get_capture_sh() == point_sh);
		CHECK(p_data->get_capture_tetrahedra() == tetrahedra);
		CHECK(p_data->get_capture_bsp_tree() == bsp_tree);
		CHECK(Math::is_equal_approx(p_data->get_baked_exposure(), baked_exposure));
		CHECK(p_data->get_lightprobe_hash() == lightprobe_hash);
	}
};

TEST_CASE("[SceneTree][LightmapGI] The dummy renderer keeps LightmapGIData's probe data") {
	const CaptureData capture;
	Ref<LightmapGIData> data = capture.make();
	capture.check(data);

	const RID rid = data->get_rid();
	CHECK(RendererDummy::LightStorage::get_singleton()->owns_lightmap(rid));
	CHECK(RSG::light_storage->lightmap_get_probe_capture_points(rid) == capture.points);
	CHECK(RSG::light_storage->lightmap_get_probe_capture_sh(rid) == capture.point_sh);
	CHECK(RSG::light_storage->lightmap_get_probe_capture_tetrahedra(rid) == capture.tetrahedra);
	CHECK(RSG::light_storage->lightmap_get_probe_capture_bsp_tree(rid) == capture.bsp_tree);
	CHECK(RSG::light_storage->lightmap_get_aabb(rid).is_equal_approx(capture.bounds));
	CHECK(RSG::light_storage->lightmap_is_interior(rid) == capture.interior);

	data.unref();
	CHECK_FALSE(RendererDummy::LightStorage::get_singleton()->owns_lightmap(rid));
}

TEST_CASE("[SceneTree][LightmapGI] LightmapGIData saves and loads its probe data on the dummy renderer") {
	const CaptureData capture;
	Ref<LightmapGIData> data = capture.make();

	// Saving reads the probe data back from the rendering server
	// (LightmapGIData::_get_probe_data), as a headless export re-save does.
	ErrorDetector ed;
	const Dictionary stored = data->get("probe_data");
	CHECK_FALSE(ed.has_error);
	CHECK(PackedVector3Array(stored["points"]) == capture.points);
	CHECK(PackedColorArray(stored["sh"]) == capture.point_sh);
	CHECK(PackedInt32Array(stored["tetrahedra"]) == capture.tetrahedra);
	CHECK(PackedInt32Array(stored["bsp"]) == capture.bsp_tree);

	Ref<LightmapGIData> loaded;
	loaded.instantiate();
	loaded->set("probe_data", stored);
	CHECK_FALSE(ed.has_error);
	capture.check(loaded);

	// Clearing the capture data clears what the dummy renderer kept.
	loaded->set_capture_data(AABB(), false, PackedVector3Array(), PackedColorArray(), PackedInt32Array(), PackedInt32Array(), 1.0f, 0);
	CHECK(loaded->get_capture_points().is_empty());
	CHECK(loaded->get_capture_sh().is_empty());
	CHECK(loaded->get_capture_tetrahedra().is_empty());
	CHECK(loaded->get_capture_bsp_tree().is_empty());
	CHECK_FALSE(loaded->is_interior());
}

TEST_CASE("[SceneTree][LightmapGI] An embedded LightmapGIData keeps its probe data when its scene is re-saved") {
	const CaptureData capture;
	Ref<LightmapGIData> data = capture.make();

	Node3D *root = memnew(Node3D);
	root->set_name("Root");
	LightmapGI *lightmap_gi = memnew(LightmapGI);
	root->add_child(lightmap_gi);
	lightmap_gi->set_owner(root);
	// The data has no path of its own, so the scene embeds it as a subresource.
	lightmap_gi->set_light_data(data);

	Ref<PackedScene> packed;
	packed.instantiate();
	REQUIRE(packed->pack(root) == OK);
	memdelete(root);
	data.unref();

	for (const String &extension : { String("tscn"), String("scn") }) {
		// A headless export loads the scene on the dummy renderer and saves
		// the converted copy; .tscn is the text form, .scn the binary one.
		const String path = TestUtils::get_temp_path("dummy_lightmap_embedded." + extension);
		ErrorDetector ed;
		REQUIRE(ResourceSaver::save(packed, path) == OK);

		Ref<PackedScene> loaded_scene = ResourceLoader::load(path, "PackedScene", ResourceLoader::CACHE_MODE_IGNORE);
		REQUIRE(loaded_scene.is_valid());
		Node *instance = loaded_scene->instantiate();
		REQUIRE(instance != nullptr);
		LightmapGI *loaded_gi = Object::cast_to<LightmapGI>(instance->get_child(0));
		REQUIRE(loaded_gi != nullptr);
		capture.check(loaded_gi->get_light_data());
		memdelete(instance);
		CHECK_FALSE(ed.has_error);
	}
}

} // namespace TestDummyLightmap

#endif // _3D_DISABLED
