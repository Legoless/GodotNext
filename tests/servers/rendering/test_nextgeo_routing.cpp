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
