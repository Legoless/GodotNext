/**************************************************************************/
/*  test_nextgeo_routing.cpp                                              */
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

TEST_FORCE_LINK(test_nextgeo_routing)

#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_route.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/virtual_mesh_storage.h"

#include "modules/modules_enabled.gen.h"

#ifndef _3D_DISABLED
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/3d/mesh_library.h"
#include "scene/resources/3d/virtual_geometry_data.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#ifdef MODULE_GRIDMAP_ENABLED
#include "modules/gridmap/grid_map.h"
#endif
#endif

using namespace RendererRD;

namespace {

static NextGeoSurfaceFacts _candidate() {
	NextGeoSurfaceFacts facts;
	facts.instance_mesh = true;
	facts.mesh_has_virtual_geometry = true;
	facts.surface_in_mask = true;
	facts.draw_ready = true;
	return facts;
}

} // namespace

TEST_CASE("[NextGeo][Routing] Candidates and the draw-ready gate") {
	NextGeoSurfaceRoute route = nextgeo_resolve_surface_route(_candidate());
	CHECK(route.virtual_geometry);
	CHECK_EQ(route.reason, NEXTGEO_FALLBACK_NONE);

	// Not a candidate: legacy without a fallback reason.
	NextGeoSurfaceFacts not_mesh = _candidate();
	not_mesh.instance_mesh = false;
	CHECK_FALSE(nextgeo_resolve_surface_route(not_mesh).virtual_geometry);
	CHECK_EQ(nextgeo_resolve_surface_route(not_mesh).reason, NEXTGEO_FALLBACK_NONE);

	NextGeoSurfaceFacts no_vgd = _candidate();
	no_vgd.mesh_has_virtual_geometry = false;
	CHECK_FALSE(nextgeo_resolve_surface_route(no_vgd).virtual_geometry);
	CHECK_EQ(nextgeo_resolve_surface_route(no_vgd).reason, NEXTGEO_FALLBACK_NONE);

	NextGeoSurfaceFacts outside_mask = _candidate();
	outside_mask.surface_in_mask = false;
	CHECK_FALSE(nextgeo_resolve_surface_route(outside_mask).virtual_geometry);
	CHECK_EQ(nextgeo_resolve_surface_route(outside_mask).reason, NEXTGEO_FALLBACK_NONE);

	// #80 has not published the resolve yet.
	NextGeoSurfaceFacts not_ready = _candidate();
	not_ready.draw_ready = false;
	CHECK_FALSE(nextgeo_resolve_surface_route(not_ready).virtual_geometry);
	CHECK_EQ(nextgeo_resolve_surface_route(not_ready).reason, NEXTGEO_FALLBACK_NONE);
}

TEST_CASE("[NextGeo][Routing] Per-instance and chain fallback reasons") {
	NextGeoSurfaceFacts facts = _candidate();
	facts.skeleton = true;
	CHECK_EQ(nextgeo_resolve_surface_route(facts).reason, NEXTGEO_FALLBACK_SKINNING);

	facts = _candidate();
	facts.instance_uniforms = true;
	CHECK_EQ(nextgeo_resolve_surface_route(facts).reason, NEXTGEO_FALLBACK_INSTANCE_UNIFORMS);

	facts = _candidate();
	facts.lightmapped = true;
	CHECK_EQ(nextgeo_resolve_surface_route(facts).reason, NEXTGEO_FALLBACK_LIGHTMAPPED_INSTANCE);

	facts = _candidate();
	facts.chain_blocked = true;
	CHECK_EQ(nextgeo_resolve_surface_route(facts).reason, NEXTGEO_FALLBACK_NEXT_PASS_OR_OVERLAY);

	// A mutated mesh is not a candidate any more, but keeps its reason.
	facts = _candidate();
	facts.mutated = true;
	facts.mesh_has_virtual_geometry = false;
	facts.surface_in_mask = false;
	CHECK_FALSE(nextgeo_resolve_surface_route(facts).virtual_geometry);
	CHECK_EQ(nextgeo_resolve_surface_route(facts).reason, NEXTGEO_FALLBACK_MUTATED_MESH);

	// A baked GridMap mesh carries BAKED_GRIDMAP without VGD.
	facts = _candidate();
	facts.baked_gridmap = true;
	facts.mesh_has_virtual_geometry = false;
	facts.surface_in_mask = false;
	CHECK_FALSE(nextgeo_resolve_surface_route(facts).virtual_geometry);
	CHECK_EQ(nextgeo_resolve_surface_route(facts).reason, NEXTGEO_FALLBACK_BAKED_GRIDMAP);
	CHECK_EQ(String(nextgeo_fallback_name(NEXTGEO_FALLBACK_BAKED_GRIDMAP)), "BAKED_GRIDMAP");
}

TEST_CASE("[NextGeo][Routing] Material fallback reasons and mixed-surface alpha reporting") {
	NextGeoSurfaceFacts facts = _candidate();
	facts.material_fallback = NEXTGEO_FALLBACK_VERTEX_LIGHTING;
	CHECK_FALSE(nextgeo_resolve_surface_route(facts).virtual_geometry);
	CHECK_EQ(nextgeo_resolve_surface_route(facts).reason, NEXTGEO_FALLBACK_VERTEX_LIGHTING);
	CHECK_EQ(String(nextgeo_fallback_name(NEXTGEO_FALLBACK_VERTEX_LIGHTING)), "VERTEX_LIGHTING");

	facts = _candidate();
	facts.material_fallback = NEXTGEO_FALLBACK_UNSUPPORTED_CUSTOM_SHADER;
	CHECK_FALSE(nextgeo_resolve_surface_route(facts).virtual_geometry);
	CHECK_EQ(nextgeo_resolve_surface_route(facts).reason, NEXTGEO_FALLBACK_UNSUPPORTED_CUSTOM_SHADER);
	CHECK_EQ(String(nextgeo_fallback_name(NEXTGEO_FALLBACK_UNSUPPORTED_CUSTOM_SHADER)), "UNSUPPORTED_CUSTOM_SHADER");

	facts = _candidate();
	facts.material_fallback = NEXTGEO_FALLBACK_UNSUPPORTED_DERIVATIVE;
	CHECK_FALSE(nextgeo_resolve_surface_route(facts).virtual_geometry);
	CHECK_EQ(nextgeo_resolve_surface_route(facts).reason, NEXTGEO_FALLBACK_UNSUPPORTED_DERIVATIVE);
	CHECK_EQ(String(nextgeo_fallback_name(NEXTGEO_FALLBACK_UNSUPPORTED_DERIVATIVE)), "UNSUPPORTED_DERIVATIVE");

	// On a mixed opaque + glass mesh, surface 1 is excluded from the VGD mask AND
	// uses an alpha material; it must still report "alpha" rather than "NONE".
	facts = _candidate();
	facts.surface_in_mask = false;
	facts.material_fallback = NEXTGEO_FALLBACK_ALPHA;
	CHECK_FALSE(nextgeo_resolve_surface_route(facts).virtual_geometry);
	CHECK_EQ(nextgeo_resolve_surface_route(facts).reason, NEXTGEO_FALLBACK_ALPHA);
	CHECK_EQ(String(nextgeo_fallback_name(NEXTGEO_FALLBACK_ALPHA)), "alpha");
}

TEST_CASE("[NextGeo][Routing] View-level precedence") {
	CHECK_EQ(nextgeo_view_fallback(false, false, false), NEXTGEO_FALLBACK_NONE);
	CHECK_EQ(nextgeo_view_fallback(true, false, false), NEXTGEO_FALLBACK_MSAA);
	CHECK_EQ(nextgeo_view_fallback(true, true, true), NEXTGEO_FALLBACK_MSAA);
	CHECK_EQ(nextgeo_view_fallback(false, true, true), NEXTGEO_FALLBACK_MULTIVIEW);
	CHECK_EQ(nextgeo_view_fallback(false, false, true), NEXTGEO_FALLBACK_DEBUG_DRAW);

	NextGeoSurfaceFacts facts = _candidate();
	facts.msaa = true;
	CHECK_EQ(nextgeo_resolve_surface_route(facts).reason, NEXTGEO_FALLBACK_MSAA);

	facts = _candidate();
	facts.multiview = true;
	CHECK_EQ(nextgeo_resolve_surface_route(facts).reason, NEXTGEO_FALLBACK_MULTIVIEW);

	facts = _candidate();
	facts.unsupported_debug_draw = true;
	CHECK_EQ(nextgeo_resolve_surface_route(facts).reason, NEXTGEO_FALLBACK_DEBUG_DRAW);

	// View-level reasons win over the per-instance ones for the whole view.
	facts = _candidate();
	facts.msaa = true;
	facts.skeleton = true;
	CHECK_EQ(nextgeo_resolve_surface_route(facts).reason, NEXTGEO_FALLBACK_MSAA);
}

TEST_CASE("[NextGeo][Routing] Debug-draw classification and view gate (#79)") {
	const RSE::ViewportDebugDraw nextgeo_modes[] = {
		RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_CRACKS,
		RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_RT_PROXY,
		RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_CLUSTER_ERROR,
		RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_RESIDENCY,
		RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_VISIBILITY_ID,
		RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_FALLBACK,
		RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_TRIANGLE_SIZE,
	};
	for (RSE::ViewportDebugDraw mode : nextgeo_modes) {
		CHECK(nextgeo_is_debug_view(mode));
		CHECK_FALSE(nextgeo_is_unsupported_debug_draw(mode));
		CHECK_EQ(nextgeo_view_fallback(false, false, nextgeo_is_unsupported_debug_draw(mode)), NEXTGEO_FALLBACK_NONE);
	}

	CHECK_FALSE(nextgeo_is_debug_view(RSE::VIEWPORT_DEBUG_DRAW_DISABLED));
	CHECK_FALSE(nextgeo_is_unsupported_debug_draw(RSE::VIEWPORT_DEBUG_DRAW_DISABLED));

	// NextGI and Stage-A buffer debug views keep virtual geometry active.
	CHECK_FALSE(nextgeo_is_debug_view(RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_TIER));
	CHECK_FALSE(nextgeo_is_unsupported_debug_draw(RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_TIER));
	CHECK_FALSE(nextgeo_is_unsupported_debug_draw(RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_SRC_CARDS));
	CHECK_FALSE(nextgeo_is_unsupported_debug_draw(RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_WRC_PROBES));
	CHECK_FALSE(nextgeo_is_unsupported_debug_draw(RSE::VIEWPORT_DEBUG_DRAW_NORMAL_BUFFER));
	// GodotNext: megalights_inputs (#136). Buffer views keep virtual geometry active.
	CHECK_FALSE(nextgeo_is_debug_view(RSE::VIEWPORT_DEBUG_DRAW_ML_ALBEDO));
	CHECK_FALSE(nextgeo_is_debug_view(RSE::VIEWPORT_DEBUG_DRAW_ML_MATERIAL));
	CHECK_FALSE(nextgeo_is_unsupported_debug_draw(RSE::VIEWPORT_DEBUG_DRAW_ML_ALBEDO));
	CHECK_FALSE(nextgeo_is_unsupported_debug_draw(RSE::VIEWPORT_DEBUG_DRAW_ML_MATERIAL));

	// Legacy Forward+ debug views force the whole view to legacy with DEBUG_DRAW.
	const RSE::ViewportDebugDraw legacy_debug_modes[] = {
		RSE::VIEWPORT_DEBUG_DRAW_UNSHADED,
		RSE::VIEWPORT_DEBUG_DRAW_LIGHTING,
		RSE::VIEWPORT_DEBUG_DRAW_OVERDRAW,
		RSE::VIEWPORT_DEBUG_DRAW_WIREFRAME,
		RSE::VIEWPORT_DEBUG_DRAW_PSSM_SPLITS,
	};
	for (RSE::ViewportDebugDraw mode : legacy_debug_modes) {
		CHECK_FALSE(nextgeo_is_debug_view(mode));
		CHECK(nextgeo_is_unsupported_debug_draw(mode));
		CHECK_EQ(nextgeo_view_fallback(false, false, nextgeo_is_unsupported_debug_draw(mode)), NEXTGEO_FALLBACK_DEBUG_DRAW);
	}

#ifndef _3D_DISABLED
	CHECK_EQ(int(Viewport::DEBUG_DRAW_NEXTGEO_CRACKS), int(RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_CRACKS));
	CHECK_EQ(int(Viewport::DEBUG_DRAW_NEXTGEO_RT_PROXY), int(RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_RT_PROXY));
	CHECK_EQ(int(Viewport::DEBUG_DRAW_NEXTGEO_CLUSTER_ERROR), int(RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_CLUSTER_ERROR));
	CHECK_EQ(int(Viewport::DEBUG_DRAW_NEXTGEO_RESIDENCY), int(RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_RESIDENCY));
	CHECK_EQ(int(Viewport::DEBUG_DRAW_NEXTGEO_VISIBILITY_ID), int(RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_VISIBILITY_ID));
	CHECK_EQ(int(Viewport::DEBUG_DRAW_NEXTGEO_FALLBACK), int(RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_FALLBACK));
	CHECK_EQ(int(Viewport::DEBUG_DRAW_NEXTGEO_TRIANGLE_SIZE), int(RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_TRIANGLE_SIZE));
	CHECK_EQ(int(Viewport::DEBUG_DRAW_ML_ALBEDO), int(RSE::VIEWPORT_DEBUG_DRAW_ML_ALBEDO));
	CHECK_EQ(int(Viewport::DEBUG_DRAW_ML_MATERIAL), int(RSE::VIEWPORT_DEBUG_DRAW_ML_MATERIAL));
#endif
}

namespace {

class RoutingTestVirtualMeshStorage : public RendererVirtualMeshStorage {
protected:
	void _upload_records(uint32_t, uint32_t) override {}
	void _upload_mesh_records(uint32_t, uint32_t) override {}
	bool _resize_dag(uint32_t) override { return true; }
};

struct ScopedVirtualMeshStorage {
	RendererVirtualMeshStorage *prev = nullptr;
	RoutingTestVirtualMeshStorage test_storage;
	ScopedVirtualMeshStorage() {
		prev = RSG::virtual_mesh_storage;
		RSG::virtual_mesh_storage = &test_storage;
	}
	~ScopedVirtualMeshStorage() {
		RSG::virtual_mesh_storage = prev;
	}
};

#ifndef _3D_DISABLED
static Array _triangle_arrays() {
	PackedVector3Array vertices;
	vertices.push_back(Vector3(0.0f, 0.0f, 0.0f));
	vertices.push_back(Vector3(1.0f, 0.0f, 0.0f));
	vertices.push_back(Vector3(0.0f, 1.0f, 0.0f));
	PackedVector3Array normals;
	normals.push_back(Vector3(0.0f, 0.0f, 1.0f));
	normals.push_back(Vector3(0.0f, 0.0f, 1.0f));
	normals.push_back(Vector3(0.0f, 0.0f, 1.0f));
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

static Ref<ArrayMesh> _make_vgd_array_mesh() {
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, _triangle_arrays());

	Ref<VirtualGeometryData> vgd;
	vgd.instantiate();
	PackedByteArray elig;
	elig.push_back(1);
	vgd->set_surface_eligibility(elig);
	vgd->set_ngm_path("res://test_routing.ngm");
	PackedByteArray dag;
	dag.resize(8);
	vgd->set_dag_header(dag);
	TypedArray<PackedByteArray> roots;
	roots.push_back(PackedByteArray());
	vgd->set_root_pages(roots);
	TypedArray<Array> coarse;
	coarse.push_back(_triangle_arrays());
	vgd->set_coarse_fallback(coarse);
	mesh->set_virtual_geometry(vgd);
	return mesh;
}
#endif

} // namespace

#ifndef _3D_DISABLED
TEST_CASE("[SceneTree][NextGeo][Routing] ArrayMesh mutations detach VGD with MUTATED_MESH and GridMap bake sets BAKED_GRIDMAP (#79)") {
	ScopedVirtualMeshStorage scoped_vms;

	// 1. surface_set_material keeps VGD; coarse_surfaces are forwarded into VirtualGeometryDesc.
	{
		Ref<ArrayMesh> mesh = _make_vgd_array_mesh();
		CHECK(mesh->get_virtual_geometry().is_valid());
		CHECK(RSG::virtual_mesh_storage->mesh_has_virtual_geometry(mesh->get_rid()));
		const RenderingServerTypes::VirtualGeometryDesc *desc = RSG::virtual_mesh_storage->mesh_get_virtual_geometry_desc(mesh->get_rid());
		REQUIRE(desc != nullptr);
		CHECK_EQ(desc->coarse_surfaces.size(), 1);

		Ref<StandardMaterial3D> mat;
		mat.instantiate();
		mesh->surface_set_material(0, mat);
		CHECK(mesh->get_virtual_geometry().is_valid());
		CHECK(RSG::virtual_mesh_storage->mesh_has_virtual_geometry(mesh->get_rid()));
		CHECK_EQ(RSG::virtual_mesh_storage->mesh_clear_reason(mesh->get_rid()), uint32_t(RenderingServerTypes::VIRTUAL_GEOMETRY_CLEAR_NONE));

		// surface_update_vertex_region detaches VGD and marks MUTATED_MESH.
		Vector<uint8_t> patch;
		patch.resize(12);
		mesh->surface_update_vertex_region(0, 0, patch);
		CHECK(mesh->get_virtual_geometry().is_null());
		CHECK_FALSE(RSG::virtual_mesh_storage->mesh_has_virtual_geometry(mesh->get_rid()));
		CHECK_EQ(RSG::virtual_mesh_storage->mesh_clear_reason(mesh->get_rid()), uint32_t(RenderingServerTypes::VIRTUAL_GEOMETRY_CLEAR_MUTATED_MESH));
	}

	// 2. add_surface_from_arrays detaches VGD with MUTATED_MESH.
	{
		Ref<ArrayMesh> mesh = _make_vgd_array_mesh();
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, _triangle_arrays());
		CHECK(mesh->get_virtual_geometry().is_null());
		CHECK_EQ(RSG::virtual_mesh_storage->mesh_clear_reason(mesh->get_rid()), uint32_t(RenderingServerTypes::VIRTUAL_GEOMETRY_CLEAR_MUTATED_MESH));
	}

	// 3. surface_remove detaches VGD with MUTATED_MESH.
	{
		Ref<ArrayMesh> mesh = _make_vgd_array_mesh();
		mesh->surface_remove(0);
		CHECK(mesh->get_virtual_geometry().is_null());
		CHECK_EQ(RSG::virtual_mesh_storage->mesh_clear_reason(mesh->get_rid()), uint32_t(RenderingServerTypes::VIRTUAL_GEOMETRY_CLEAR_MUTATED_MESH));
	}

	// 4. clear_surfaces detaches VGD with MUTATED_MESH.
	{
		Ref<ArrayMesh> mesh = _make_vgd_array_mesh();
		const RID rid = mesh->get_rid();
		mesh->clear_surfaces();
		CHECK(mesh->get_virtual_geometry().is_null());
		CHECK_EQ(RSG::virtual_mesh_storage->mesh_clear_reason(rid), uint32_t(RenderingServerTypes::VIRTUAL_GEOMETRY_CLEAR_MUTATED_MESH));
	}

#ifdef MODULE_GRIDMAP_ENABLED
	// 5. GridMap::make_baked_meshes creates baked meshes without VGD and marks VIRTUAL_GEOMETRY_CLEAR_BAKED_GRIDMAP.
	{
		Ref<MeshLibrary> lib;
		lib.instantiate();
		lib->create_item(0);
		Ref<ArrayMesh> item_mesh = _make_vgd_array_mesh();
		lib->set_item_mesh(0, item_mesh);

		GridMap *gm = memnew(GridMap);
		gm->set_mesh_library(lib);
		SceneTree::get_singleton()->get_root()->add_child(gm);
		gm->set_cell_item(Vector3i(0, 0, 0), 0);
		gm->make_baked_meshes(false);
		Array baked = gm->get_bake_meshes();
		CHECK_EQ(baked.size(), 2);
		if (baked.size() == 2) {
			Ref<ArrayMesh> baked_mesh = baked[0];
			REQUIRE(baked_mesh.is_valid());
			CHECK(baked_mesh->get_virtual_geometry().is_null());
			CHECK_FALSE(RSG::virtual_mesh_storage->mesh_has_virtual_geometry(baked_mesh->get_rid()));
			CHECK_EQ(RSG::virtual_mesh_storage->mesh_clear_reason(baked_mesh->get_rid()), uint32_t(RenderingServerTypes::VIRTUAL_GEOMETRY_CLEAR_BAKED_GRIDMAP));
		}

		// Internal unattached MultiMesh (GridMap octant pattern) with VGD mesh reports VGD_IGNORED.
		RenderingServer *rs = RenderingServer::get_singleton();
		RID mm = rs->multimesh_create();
		rs->multimesh_allocate_data(mm, 1, RSE::MULTIMESH_TRANSFORM_3D);
		rs->multimesh_set_mesh(mm, item_mesh->get_rid());
		RID octant_inst = rs->instance_create2(mm, gm->get_world_3d()->get_scenario());
		Dictionary oct_st = rs->virtual_geometry_get_debug_stats(octant_inst);
		CHECK_EQ(String(oct_st["reason"]), "VGD_IGNORED");
		rs->free_rid(octant_inst);
		rs->free_rid(mm);

		gm->clear_baked_meshes();
		SceneTree::get_singleton()->get_root()->remove_child(gm);
		memdelete(gm);
	}
#endif

	// 6. RenderingServer::virtual_geometry_get_debug_stats exposes #79 routing/proxy/Q14 counters and viewport debug-draw routing.
	{
		RSG::virtual_mesh_storage->nextgeo_count_vgd_ignored_warning();
		RSG::virtual_mesh_storage->nextgeo_count_auxiliary_proxy_draw();
		RSG::virtual_mesh_storage->nextgeo_count_auxiliary_proxy_draw();
		RSG::virtual_mesh_storage->nextgeo_count_uv2_authored_draw();

		RID vp = RenderingServer::get_singleton()->viewport_create();
		RenderingServer::get_singleton()->viewport_set_debug_draw(vp, RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_FALLBACK);
		Dictionary st = RenderingServer::get_singleton()->virtual_geometry_get_debug_stats(vp);
		CHECK_EQ(int64_t(st["vgd_ignored_warnings"]), 1);
		CHECK_EQ(int64_t(st["auxiliary_proxy_draws"]), 2);
		CHECK_EQ(int64_t(st["auxiliary_authored_virtual_draws"]), 0);
		CHECK_EQ(int64_t(st["uv2_authored_draws"]), 1);
		CHECK_EQ(int64_t(st["uv2_proxy_draws"]), 0);
		CHECK(st.has("fallback_coverage"));
		CHECK(st.has("triangle_size_histogram"));
		CHECK_EQ(Array(st["triangle_size_histogram"]).size(), 16);
		CHECK_EQ(String(st["view_route"]), "virtual");
		CHECK_EQ(String(st["view_reason"]), "NONE");

		RenderingServer::get_singleton()->viewport_set_debug_draw(vp, RSE::VIEWPORT_DEBUG_DRAW_WIREFRAME);
		st = RenderingServer::get_singleton()->virtual_geometry_get_debug_stats(vp);
		CHECK_EQ(String(st["view_route"]), "legacy");
		CHECK_EQ(String(st["view_reason"]), "DEBUG_DRAW");

		RenderingServer::get_singleton()->free_rid(vp);
	}
}
#endif
