/**************************************************************************/
/*  test_nextgi_swrt_trace.cpp                                            */
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

TEST_FORCE_LINK(test_nextgi_swrt_trace)

#include "core/math/mesh_sdf_builder.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "servers/rendering/renderer_rd/environment/next_gi_swrt_trace.h"

#ifdef RD_ENABLED
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_gather.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_gather_sdf.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_src_relight_sdf.glsl.gen.h"
#endif

namespace {

constexpr uint32_t LEVELS = 6;

MeshSDFData _unit_box_sdf(bool p_two_sided) {
	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size(Vector3(1.0f, 1.0f, 1.0f));
	MeshSDFSettings settings;
	settings.voxels_per_meter = 8.0f;
	settings.max_voxels_per_axis = 32;
	settings.prefer_static_raycaster = false;
	settings.two_sided = p_two_sided;
	MeshSDFData data;
	const Error err = MeshSDFBuilder::build(box->get_faces(), settings, data);
	CHECK(err == OK);
	CHECK(data.is_valid());
	return data;
}

void _fill_origins(const Vector3 &p_camera, Vector3i r_origins[LEVELS]) {
	for (uint32_t level = 0; level < LEVELS; level++) {
		r_origins[level] = NextGIGlobalSDF::scrolled_origin(p_camera, level);
	}
}

NextGIGlobalSDF::Instance _instance(const MeshSDFData *p_field, const Vector3 &p_origin, bool p_two_sided) {
	NextGIGlobalSDF::Instance instance;
	instance.transform = Transform3D(Basis(), p_origin);
	instance.field = p_field;
	instance.two_sided = p_two_sided;
	return instance;
}

} // namespace

#ifdef RD_ENABLED
TEST_CASE("[NextGI][SWRT] Software trace shaders do not require ray query") {
	CHECK_FALSE(NextgiGatherSdfShaderRD().requires_ray_query());
	CHECK_FALSE(NextgiSrcRelightSdfShaderRD().requires_ray_query());
	CHECK(NextgiGatherShaderRD().requires_ray_query());
}
#endif

TEST_CASE("[NextGI][SWRT] Global march hits a one-sided box and misses empty space") {
	const MeshSDFData box = _unit_box_sdf(false);
	Vector<NextGIGlobalSDF::Instance> instances;
	instances.push_back(_instance(&box, Vector3(), false));
	NextGIGlobalSDF field;
	Vector3i origins[LEVELS];
	_fill_origins(Vector3(), origins);
	const uint32_t bricks = field.update_region(0, origins[0], AABB(Vector3(-1.5f, -1.5f, -1.5f), Vector3(3.0f, 3.0f, 3.0f)), instances);
	CHECK(bricks > 0);

	const NextGISWRTTrace::Hit hit = NextGISWRTTrace::trace_global(field, origins, LEVELS, Vector3(), Vector3(0.0f, 0.0f, -2.0f), Vector3(0.0f, 0.0f, 1.0f), 4.0f, instances.ptr(), instances.size());
	CHECK(hit.hit);
	CHECK_FALSE(hit.detail);
	CHECK(hit.t > 0.5f);
	CHECK(hit.t < 2.0f);
	CHECK(hit.steps > 0);
	CHECK(hit.steps <= NextGISWRTTrace::MAX_STEPS);

	const NextGISWRTTrace::Hit miss = NextGISWRTTrace::trace_global(field, origins, LEVELS, Vector3(), Vector3(0.0f, 2.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f), 4.0f, instances.ptr(), instances.size());
	CHECK_FALSE(miss.hit);
	CHECK(miss.steps > 0);
	CHECK(miss.steps <= NextGISWRTTrace::MAX_STEPS);
}

TEST_CASE("[NextGI][SWRT] Two-sided coverage hits only a back-facing approach") {
	const MeshSDFData box = _unit_box_sdf(true);
	Vector<NextGIGlobalSDF::Instance> instances;
	instances.push_back(_instance(&box, Vector3(), true));
	NextGIGlobalSDF field;
	Vector3i origins[LEVELS];
	_fill_origins(Vector3(), origins);
	CHECK(field.update_region(0, origins[0], AABB(Vector3(-1.5f, -1.5f, -1.5f), Vector3(3.0f, 3.0f, 3.0f)), instances) > 0);

	// The front face is an outward approach. 1.7 m reaches that face and not the exit.
	const NextGISWRTTrace::Hit front = NextGISWRTTrace::trace_global(field, origins, LEVELS, Vector3(), Vector3(0.0f, 0.0f, -2.0f), Vector3(0.0f, 0.0f, 1.0f), 1.7f, instances.ptr(), instances.size());
	CHECK_FALSE(front.hit);

	const NextGISWRTTrace::Hit exit_face = NextGISWRTTrace::trace_global(field, origins, LEVELS, Vector3(), Vector3(), Vector3(0.0f, 0.0f, 1.0f), 2.0f, instances.ptr(), instances.size());
	CHECK(exit_face.hit);
	CHECK(exit_face.t > 0.2f);
	CHECK(exit_face.t < 1.2f);
}

TEST_CASE("[NextGI][SWRT] Detail mesh SDF hands off to the global field") {
	const MeshSDFData box = _unit_box_sdf(false);
	NextGIGlobalSDF::Instance near_box = _instance(&box, Vector3(), false);
	NextGISWRTTrace::DetailInstance detail;
	detail.instance = near_box;
	detail.id = 42;
	NextGIGlobalSDF empty;
	Vector3i origins[LEVELS];
	_fill_origins(Vector3(), origins);
	const NextGISWRTTrace::Hit near_hit = NextGISWRTTrace::trace_detail(empty, origins, LEVELS, Vector3(), Vector3(0.0f, 0.0f, -1.2f), Vector3(0.0f, 0.0f, 1.0f), 4.0f, &detail, 1, nullptr, nullptr, 0);
	CHECK(near_hit.hit);
	CHECK(near_hit.detail);
	CHECK_EQ(near_hit.instance_id, 42u);
	CHECK(near_hit.t < NextGISWRTTrace::DETAIL_METERS);

	Vector<NextGIGlobalSDF::Instance> far_instances;
	far_instances.push_back(_instance(&box, Vector3(0.0f, 0.0f, 3.0f), false));
	NextGIGlobalSDF far_field;
	CHECK(far_field.update_region(0, origins[0], AABB(Vector3(-1.0f, -1.0f, 2.0f), Vector3(2.0f, 2.0f, 2.0f)), far_instances) > 0);
	const NextGISWRTTrace::Hit far_hit = NextGISWRTTrace::trace_detail(far_field, origins, LEVELS, Vector3(), Vector3(), Vector3(0.0f, 0.0f, 1.0f), 8.0f, nullptr, 0, nullptr, far_instances.ptr(), far_instances.size());
	CHECK(far_hit.hit);
	CHECK_FALSE(far_hit.detail);
	CHECK(far_hit.t >= NextGISWRTTrace::DETAIL_METERS);
	CHECK(far_hit.t < 4.0f);
}

TEST_CASE("[NextGI][SWRT] Offset, handoff and shade follow the trace contract") {
	CHECK(NextGISWRTTrace::offset_origin(Vector3(), Vector3(0.0f, 1.0f, 0.0f), 0.1f).is_equal_approx(Vector3(0.0f, 0.15f, 0.0f)));
	CHECK(Math::is_equal_approx(NextGISWRTTrace::wrc_handoff(2.0f), 3.0f));
	CHECK(Math::is_equal_approx(NextGISWRTTrace::trace_limit(0.0f, 100.0f, false), 1.5f));
	CHECK(Math::is_equal_approx(NextGISWRTTrace::trace_limit(0.0f, 100.0f, true), 100.0f));
	const Vector3 cap_camera(0.0f, 2.0f, 8.0f);
	const Vector3 cap_forward(0.0f, 0.0f, -1.0f);
	CHECK(NextGISWRTTrace::instance_in_front(cap_camera, cap_forward, Vector3(0.0f, 1.0f, 0.0f)));
	CHECK_FALSE(NextGISWRTTrace::instance_in_front(cap_camera, cap_forward, Vector3(0.0f, 2.0f, 14.0f)));
	// A rock 8 m ahead stays ahead of a nearer rock behind the camera.
	CHECK(NextGISWRTTrace::instance_before(true, 64.0f, false, 1.0f));
	CHECK_FALSE(NextGISWRTTrace::instance_before(false, 1.0f, true, 64.0f));
	CHECK(NextGISWRTTrace::instance_before(true, 4.0f, true, 9.0f));
	CHECK_FALSE(NextGISWRTTrace::instance_before(true, 9.0f, true, 4.0f));

	const Color voxel(0.2f, 0.4f, 0.6f);
	const Color wrc(0.9f, 0.1f, 0.1f);
	const Color sky(0.05f, 0.07f, 0.1f);
	const NextGISWRTTrace::ShadeResult covered = NextGISWRTTrace::shade_global(0.5f, voxel, wrc, sky, true);
	CHECK(covered.used_voxel);
	CHECK(covered.color.is_equal_approx(voxel));
	const NextGISWRTTrace::ShadeResult from_wrc = NextGISWRTTrace::shade_global(0.49f, voxel, wrc, sky, true);
	CHECK(from_wrc.used_wrc);
	CHECK(from_wrc.color.is_equal_approx(wrc));
	const NextGISWRTTrace::ShadeResult from_sky = NextGISWRTTrace::shade_global(0.49f, voxel, wrc, sky, false);
	CHECK_FALSE(from_sky.used_voxel);
	CHECK_FALSE(from_sky.used_wrc);
	CHECK(from_sky.color.is_equal_approx(sky));
	const Color empty_wrc(0.9f, 0.1f, 0.1f, 0.0f);
	const NextGISWRTTrace::ShadeResult uncovered = NextGISWRTTrace::shade_global(0.49f, voxel, empty_wrc, sky, true);
	CHECK_FALSE(uncovered.used_wrc);
	CHECK(uncovered.color.is_equal_approx(sky));

	NextGIVoxelLighting::CardQuery card;
	card.origin = Vector3();
	card.axis_u = Vector3(1.0f, 0.0f, 0.0f);
	card.axis_v = Vector3(0.0f, 1.0f, 0.0f);
	card.axis_n = Vector3(0.0f, 0.0f, 1.0f);
	card.half_u = 0.6f;
	card.half_v = 0.6f;
	card.texel_size = 0.1f;
	const Color card_color(0.3f, 0.8f, 0.2f);
	const NextGISWRTTrace::ShadeResult from_card = NextGISWRTTrace::shade_detail(&card, &card_color, 1, Vector3(), Vector3(0.0f, 0.0f, 1.0f), 0.0f, voxel, wrc, sky, false);
	CHECK(from_card.used_card);
	CHECK_EQ(from_card.card, 0u);
	CHECK(from_card.color.is_equal_approx(card_color));
}

TEST_CASE("[NextGI][SWRT] Froxel grid keeps the box in the cell that contains it") {
	const MeshSDFData box = _unit_box_sdf(false);
	NextGISWRTTrace::DetailInstance detail;
	detail.instance = _instance(&box, Vector3(), false);
	detail.id = 7;
	NextGISWRTTrace::FroxelGrid grid;
	NextGISWRTTrace::build_froxel(&detail, 1, Vector3(-1.0f, -1.0f, -1.0f), grid);
	const uint32_t cell = NextGISWRTTrace::froxel_index(grid, Vector3());
	CHECK(cell < NextGISWRTTrace::FROXEL_CELLS);
	CHECK(grid.counts[cell] > 0);
	CHECK_EQ(grid.indices[cell * NextGISWRTTrace::FROXEL_CAP], 0u);
}
