/**************************************************************************/
/*  test_spline_mesh.cpp                                                  */
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

TEST_FORCE_LINK(test_spline_mesh)

#include "scene/3d/path_3d.h"
#include "scene/3d/spline_mesh_instance_3d.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/curve.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_native.h"
#include "servers/rendering/storage/spline_deform.h"
#include "servers/rendering/storage/virtual_mesh_storage.h"

namespace TestSplineMesh {

class TestSplineVirtualMeshStorage : public RendererVirtualMeshStorage {
protected:
	void _upload_records(uint32_t p_first, uint32_t p_count) override {
		(void)p_first;
		(void)p_count;
	}
	void _upload_mesh_records(uint32_t p_first, uint32_t p_count) override {
		(void)p_first;
		(void)p_count;
	}
	bool _resize_dag(uint32_t p_bytes) override {
		(void)p_bytes;
		return true;
	}
	void _upload_dag(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) override {
		(void)p_offset;
		(void)p_data;
		(void)p_size;
	}
};

static SplineDeformPose make_pose(const Vector3 &p_p0, const Vector3 &p_t0, const Vector3 &p_p1, const Vector3 &p_t1, float p_r0, float p_r1, const Vector2 &p_s0, const Vector2 &p_s1, uint32_t p_axis, const Vector3 &p_up, const AABB &p_mesh_aabb) {
	Dictionary params;
	params["start_position"] = p_p0;
	params["start_tangent"] = p_t0;
	params["end_position"] = p_p1;
	params["end_tangent"] = p_t1;
	params["start_roll"] = p_r0;
	params["end_roll"] = p_r1;
	params["start_scale"] = p_s0;
	params["end_scale"] = p_s1;
	params["forward_axis"] = int(p_axis);
	params["up_vector"] = p_up;
	bool active = false;
	SplineDeformPose pose = spline_deform_pose_from_dictionary(params, active);
	spline_deform_apply_mesh_aabb(pose, p_mesh_aabb);
	return pose;
}

TEST_CASE("[SplineMesh] Hermite identity mapping across all forward axes") {
	const AABB unit_box(Vector3(-0.5, -0.5, -0.5), Vector3(1.0, 1.0, 1.0));
	const Vector3 sample_points[] = {
		Vector3(0.0, 0.0, 0.0),
		Vector3(0.3, -0.2, 0.4),
		Vector3(-0.4, 0.45, -0.35),
		Vector3(0.5, 0.5, 0.5),
		Vector3(-0.5, -0.5, -0.5),
	};

	// AXIS_Z: start=(0,0,-0.5), end=(0,0,0.5), tangent=(0,0,1), up=(0,1,0) -> exact identity.
	{
		const SplineDeformPose pose = make_pose(Vector3(0, 0, -0.5), Vector3(0, 0, 1), Vector3(0, 0, 0.5), Vector3(0, 0, 1), 0.0f, 0.0f, Vector2(1, 1), Vector2(1, 1), SPLINE_DEFORM_AXIS_Z, Vector3(0, 1, 0), unit_box);
		for (const Vector3 &p : sample_points) {
			Vector3 pos = p;
			Vector3 nrm = Vector3(0.3, 0.8, -0.5).normalized();
			Vector3 tan = Vector3(0.8, -0.3, 0.0).normalized();
			spline_deform_vertex(pose, pos, &nrm, &tan);
			CHECK(pos.is_equal_approx(p));
			CHECK(nrm.is_equal_approx(Vector3(0.3, 0.8, -0.5).normalized()));
			CHECK(tan.is_equal_approx(Vector3(0.8, -0.3, 0.0).normalized()));
		}
		CHECK(Math::is_equal_approx(pose.up_max_stretch[3], 1.0f, 1e-4f));
	}

	// AXIS_X: start=(-0.5,0,0), end=(0.5,0,0), tangent=(1,0,0), up=(0,1,0) -> exact identity.
	{
		const SplineDeformPose pose = make_pose(Vector3(-0.5, 0, 0), Vector3(1, 0, 0), Vector3(0.5, 0, 0), Vector3(1, 0, 0), 0.0f, 0.0f, Vector2(1, 1), Vector2(1, 1), SPLINE_DEFORM_AXIS_X, Vector3(0, 1, 0), unit_box);
		for (const Vector3 &p : sample_points) {
			const Vector3 deformed = spline_deform_position(pose, p);
			CHECK(deformed.is_equal_approx(p));
		}
	}

	// AXIS_Y: start=(0,-0.5,0), end=(0,0.5,0), tangent=(0,1,0), up=(0,1,0) -> exact identity.
	{
		const SplineDeformPose pose = make_pose(Vector3(0, -0.5, 0), Vector3(0, 1, 0), Vector3(0, 0.5, 0), Vector3(0, 1, 0), 0.0f, 0.0f, Vector2(1, 1), Vector2(1, 1), SPLINE_DEFORM_AXIS_Y, Vector3(0, 1, 0), unit_box);
		for (const Vector3 &p : sample_points) {
			const Vector3 deformed = spline_deform_position(pose, p);
			CHECK(deformed.is_equal_approx(p));
		}
	}
}

TEST_CASE("[SplineMesh] Analytic Hermite position, tangent, roll, and non-uniform scale frame") {
	const AABB box(Vector3(-0.25, -0.25, 0.0), Vector3(0.5, 0.5, 2.0));
	const Vector3 p0(0.0, 0.0, 0.0);
	const Vector3 t0(0.0, 4.0, 0.0);
	const Vector3 p1(4.0, 0.0, 0.0);
	const Vector3 t1(0.0, -4.0, 0.0);
	const SplineDeformPose pose = make_pose(p0, t0, p1, t1, 0.0f, float(Math::PI * 0.5), Vector2(1.0, 2.0), Vector2(3.0, 1.0), SPLINE_DEFORM_AXIS_Z, Vector3(0, 0, -1), box);

	// At t = 0.5 (z = 1.0):
	// H(0.5) = 0.5*(p0+p1) + 0.125*(t0-t1) = (2, 0, 0) + 0.125*(0, 8, 0) = (2, 1, 0).
	// H'(0.5) = 1.5*(p1-p0) - 0.25*(t0+t1) = (6, 0, 0) -> unit tangent = (1, 0, 0).
	Vector3 f_pos, f_right, f_up, f_forward;
	Vector2 f_scale;
	float f_deriv_len = 0.0f;
	spline_deform_eval_frame(pose, 0.5f, f_pos, f_right, f_up, f_forward, f_scale, f_deriv_len);
	CHECK(f_pos.is_equal_approx(Vector3(2.0, 1.0, 0.0)));
	CHECK(f_forward.is_equal_approx(Vector3(1.0, 0.0, 0.0)));
	CHECK(Math::is_equal_approx(f_deriv_len, 6.0f, 1e-4f));
	CHECK(Math::is_equal_approx(float(f_scale.x), 2.0f, 1e-5f));
	CHECK(Math::is_equal_approx(float(f_scale.y), 1.5f, 1e-5f));
	// Orthonormal right-handed frame check:
	CHECK(Math::is_equal_approx(float(f_right.length()), 1.0f, 1e-5f));
	CHECK(Math::is_equal_approx(float(f_up.length()), 1.0f, 1e-5f));
	CHECK(Math::abs(float(f_right.dot(f_up))) < 1e-5f);
	CHECK(Math::abs(float(f_right.dot(f_forward))) < 1e-5f);
	CHECK(Math::abs(float(f_up.dot(f_forward))) < 1e-5f);
	CHECK(f_right.cross(f_up).is_equal_approx(f_forward));

	// Endpoints match p0 and p1 for centerline points (x=0, y=0).
	CHECK(spline_deform_position(pose, Vector3(0, 0, 0)).is_equal_approx(p0));
	CHECK(spline_deform_position(pose, Vector3(0, 0, 2)).is_equal_approx(p1));
	CHECK(spline_deform_position(pose, Vector3(0, 0, 1)).is_equal_approx(Vector3(2.0, 1.0, 0.0)));

	// Roll check at t = 1 (roll = +90 deg) on straight Z spline with uniform scale.
	const SplineDeformPose roll_pose = make_pose(Vector3(0, 0, 0), Vector3(0, 0, 2), Vector3(0, 0, 2), Vector3(0, 0, 2), 0.0f, float(Math::PI * 0.5), Vector2(1, 1), Vector2(1, 1), SPLINE_DEFORM_AXIS_Z, Vector3(0, 1, 0), box);
	const Vector3 rolled_x = spline_deform_position(roll_pose, Vector3(1, 0, 2));
	// At t=1 with +90 deg roll around +Z: right = (0, 1, 0), up = (-1, 0, 0).
	// For AXIS_Z, local (x=1, y=0) -> u = +1 -> (+1) * right = (0, 1, 0).
	CHECK(rolled_x.is_equal_approx(Vector3(0, 1, 2)));

	// Non-uniform scale normal inverse-transpose orthogonality check on straight spline.
	const SplineDeformPose scale_pose = make_pose(Vector3(0, 0, 0), Vector3(0, 0, 2), Vector3(0, 0, 2), Vector3(0, 0, 2), 0.0f, 0.0f, Vector2(2.0, 0.5), Vector2(2.0, 0.5), SPLINE_DEFORM_AXIS_Z, Vector3(0, 1, 0), box);
	Vector3 v_pos(0.25, 0.25, 1.0);
	Vector3 v_nrm = Vector3(1.0, 1.0, 0.0).normalized();
	Vector3 v_tan = Vector3(-1.0, 1.0, 0.0).normalized();
	spline_deform_vertex(scale_pose, v_pos, &v_nrm, &v_tan);
	// Deformed normal must remain orthogonal to deformed tangent.
	CHECK(Math::abs(float(v_nrm.dot(v_tan))) < 1e-4f);
	CHECK(Math::is_equal_approx(float(v_nrm.length()), 1.0f, 1e-5f));
	CHECK(Math::is_equal_approx(float(v_tan.length()), 1.0f, 1e-5f));
}

TEST_CASE("[SplineMesh] Swept 16-sample AABB strictly contains densely sampled deformed mesh") {
	struct Scenario {
		Vector3 p0, t0, p1, t1;
		float r0, r1;
		Vector2 s0, s1;
		uint32_t axis;
		Vector3 up;
		AABB box;
	};
	const Scenario scenarios[] = {
		// S-bend along Z with roll and non-uniform taper.
		{ Vector3(0, 0, 0), Vector3(3.0, 0.5, 4.0), Vector3(2.0, 1.5, 6.0), Vector3(-4.0, -1.0, 4.0), 0.0f, 2.2f, Vector2(0.7, 1.6), Vector2(1.8, 0.6), SPLINE_DEFORM_AXIS_Z, Vector3(0, 1, 0), AABB(Vector3(-0.4, -0.3, -1.0), Vector3(0.8, 0.6, 2.0)) },
		// High arch along X.
		{ Vector3(-2, 0, 0), Vector3(1.0, 7.0, 0.0), Vector3(2, 0, 0), Vector3(1.0, -7.0, 0.0), -0.5f, 1.2f, Vector2(1.2, 1.2), Vector2(0.8, 1.5), SPLINE_DEFORM_AXIS_X, Vector3(0, 1, 0), AABB(Vector3(0.0, -0.3, -0.3), Vector3(1.0, 0.6, 0.6)) },
		// Corkscrew along Y with 180-degree roll.
		{ Vector3(0, 0, 0), Vector3(4.0, 2.0, -3.0), Vector3(-1.0, 5.0, 2.0), Vector3(-3.0, 2.0, 4.0), 0.0f, float(Math::PI), Vector2(1.0, 1.0), Vector2(1.5, 1.5), SPLINE_DEFORM_AXIS_Y, Vector3(0, 0, 1), AABB(Vector3(-0.35, -0.5, -0.35), Vector3(0.7, 1.0, 0.7)) },
	};

	for (const Scenario &sc : scenarios) {
		const SplineDeformPose pose = make_pose(sc.p0, sc.t0, sc.p1, sc.t1, sc.r0, sc.r1, sc.s0, sc.s1, sc.axis, sc.up, sc.box);
		const AABB swept = spline_deform_compute_swept_aabb(pose, sc.box);
		REQUIRE(swept.size.x > 0.0f);
		REQUIRE(swept.size.y > 0.0f);
		REQUIRE(swept.size.z > 0.0f);

		// Densely sample 200 slices x 4 cross-section corners and verify every point lies inside swept AABB.
		const Vector3 bmin = sc.box.position;
		const Vector3 bmax = sc.box.position + sc.box.size;
		const AABB padded(swept.position - Vector3(1e-3, 1e-3, 1e-3), swept.size + Vector3(2e-3, 2e-3, 2e-3));
		for (int i = 0; i <= 200; i++) {
			const float f = float(i) / 200.0f;
			for (int cx = 0; cx < 2; cx++) {
				for (int cy = 0; cy < 2; cy++) {
					Vector3 p;
					if (sc.axis == SPLINE_DEFORM_AXIS_X) {
						p = Vector3(Math::lerp(bmin.x, bmax.x, real_t(f)), cx ? bmax.y : bmin.y, cy ? bmax.z : bmin.z);
					} else if (sc.axis == SPLINE_DEFORM_AXIS_Y) {
						p = Vector3(cx ? bmax.x : bmin.x, Math::lerp(bmin.y, bmax.y, real_t(f)), cy ? bmax.z : bmin.z);
					} else {
						p = Vector3(cx ? bmax.x : bmin.x, cy ? bmax.y : bmin.y, Math::lerp(bmin.z, bmax.z, real_t(f)));
					}
					const Vector3 dp = spline_deform_position(pose, p);
					CHECK(padded.has_point(dp));
				}
			}
		}
	}
}

TEST_CASE("[SplineMesh] Cluster bounding sphere deform and max_stretch conservatively enclose deformed vertices") {
	const AABB mesh_box(Vector3(-0.5, -0.5, -1.0), Vector3(1.0, 1.0, 2.0));
	// S-bend with strong twist and stretch (> 2x along spline).
	const SplineDeformPose pose = make_pose(Vector3(0, 0, 0), Vector3(5.0, 2.0, 5.0), Vector3(1.0, 2.0, 6.0), Vector3(-5.0, -2.0, 5.0), 0.0f, 1.8f, Vector2(0.8, 1.5), Vector2(2.0, 0.9), SPLINE_DEFORM_AXIS_Z, Vector3(0, 1, 0), mesh_box);

	CHECK(pose.up_max_stretch[3] >= 2.0f);
	// Curvature error is positive and monotonically non-decreasing in undeformed radius on curved splines,
	// and zero on straight unrolled splines.
	const SplineDeformPose straight = make_pose(Vector3(0, 0, -1), Vector3(0, 0, 2), Vector3(0, 0, 1), Vector3(0, 0, 2), 0.0f, 0.0f, Vector2(1, 1), Vector2(1, 1), SPLINE_DEFORM_AXIS_Z, Vector3(0, 1, 0), mesh_box);
	CHECK(Math::is_equal_approx(spline_deform_curvature_error(straight, 0.5f), 0.0f, 1e-6f));
	CHECK(spline_deform_curvature_error(pose, 0.25f) > 0.0f);
	CHECK(spline_deform_curvature_error(pose, 0.5f) > spline_deform_curvature_error(pose, 0.25f));
	CHECK(spline_deform_curvature_error(pose, 1.0f) > spline_deform_curvature_error(pose, 0.5f));

	// Test 12 sub-cluster spheres distributed along the mesh and verify every point inside each
	// undeformed sphere maps inside the deformed sphere.
	for (int k = 0; k < 12; k++) {
		const float z = -0.85f + 1.7f * (float(k) / 11.0f);
		const Vector3 c0((k % 3 - 1) * 0.25f, ((k / 3) % 3 - 1) * 0.2f, z);
		const float r0 = 0.28f;
		Vector3 dc = c0;
		float dr = r0;
		spline_deform_bounds_sphere(pose, dc, dr);
		CHECK(dr >= r0 * pose.up_max_stretch[3]);

		// Check 26 directional points on the surface of the undeformed sphere.
		for (int dx = -1; dx <= 1; dx++) {
			for (int dy = -1; dy <= 1; dy++) {
				for (int dz = -1; dz <= 1; dz++) {
					if (dx == 0 && dy == 0 && dz == 0) {
						continue;
					}
					const Vector3 dir = Vector3(float(dx), float(dy), float(dz)).normalized();
					const Vector3 dp = spline_deform_position(pose, c0 + dir * r0);
					const float dist = float(dp.distance_to(dc));
					CHECK_MESSAGE(dist <= dr + 1e-3f, vformat("cluster %d vertex escaped deformed sphere: dist=%f > dr=%f", k, dist, dr));
				}
			}
		}
	}
}

TEST_CASE("[SplineMesh] Dictionary parsing and VirtualMeshStorage spline pool lifecycle") {
	Dictionary params;
	params["start_position"] = Vector3(1, 2, 3);
	params["start_tangent"] = Vector3(0, 0, 4);
	params["end_position"] = Vector3(5, 6, 7);
	params["end_tangent"] = Vector3(0, 2, 4);
	params["start_roll"] = 0.25;
	params["end_roll"] = -0.5;
	params["start_scale"] = Vector2(1.5, 0.75);
	params["end_scale"] = Vector2(0.5, 2.0);
	params["forward_axis"] = int(SPLINE_DEFORM_AXIS_X);
	params["up_vector"] = Vector3(0, 0, 1);

	bool active = false;
	const SplineDeformPose pose = spline_deform_pose_from_dictionary(params, active);
	CHECK(active);
	CHECK(Vector3(pose.start_pos_roll[0], pose.start_pos_roll[1], pose.start_pos_roll[2]).is_equal_approx(Vector3(1, 2, 3)));
	CHECK(Vector3(pose.end_pos_axis_min[0], pose.end_pos_axis_min[1], pose.end_pos_axis_min[2]).is_equal_approx(Vector3(5, 6, 7)));
	CHECK(Math::is_equal_approx(pose.start_pos_roll[3], 0.25f));
	CHECK(Math::is_equal_approx(pose.start_tangent_end_roll[3], -0.5f));
	CHECK_EQ(pose.meta[0], uint32_t(SPLINE_DEFORM_AXIS_X));

	bool cleared_active = true;
	spline_deform_pose_from_dictionary(Dictionary(), cleared_active);
	CHECK_FALSE(cleared_active);

	// NextGeo native RT exclusion includes spline instances.
	CHECK_EQ(RendererRD::nextgeo_rt_native_exclusion(false, false, false, true), RendererRD::NEXTGEO_RT_NATIVE_EXCLUDED_SPLINE);
	CHECK_EQ(RendererRD::nextgeo_rt_native_exclusion(true, false, false, true), RendererRD::NEXTGEO_RT_NATIVE_EXCLUDED_SKINNED);

	// VirtualMeshStorage spline slot allocation, history advance, and clear.
	TestSplineVirtualMeshStorage storage;
	const RID mesh_rid = RID::from_uint64(101);
	const RID inst_rid = RID::from_uint64(202);
	RenderingServerTypes::VirtualGeometryDesc desc;
	desc.eligible_surface_mask = 1u;
	desc.ngm_path = "res://test_spline.ngm";
	desc.dag_header.resize(8);
	desc.root_pages.push_back(Vector<uint8_t>());
	storage.mesh_set_virtual_geometry(mesh_rid, desc);
	const uint32_t rec = storage.instance_record_allocate(inst_rid, mesh_rid);
	REQUIRE(rec != UINT32_MAX);
	CHECK_EQ(storage.get_record(rec).flags & RendererVirtualMeshStorage::RECORD_HAS_SPLINE, 0u);

	const AABB box(Vector3(-0.5, -0.5, -0.5), Vector3(1, 1, 1));
	const SplineDeformPose p1 = make_pose(Vector3(0, 0, 0), Vector3(0, 0, 2), Vector3(0, 0, 2), Vector3(0, 0, 2), 0.0f, 0.0f, Vector2(1, 1), Vector2(1, 1), SPLINE_DEFORM_AXIS_Z, Vector3(0, 1, 0), box);
	const SplineDeformPose p2 = make_pose(Vector3(0, 0, 0), Vector3(0, 0, 2), Vector3(1, 0, 2), Vector3(1, 0, 2), 0.0f, 0.5f, Vector2(1, 1), Vector2(1, 1), SPLINE_DEFORM_AXIS_Z, Vector3(0, 1, 0), box);

	storage.instance_record_set_spline_deform(rec, true, p1);
	CHECK((storage.get_record(rec).flags & RendererVirtualMeshStorage::RECORD_HAS_SPLINE) != 0u);
	const uint32_t slot = storage.get_record(rec).flags >> RendererVirtualMeshStorage::RECORD_SPLINE_INDEX_SHIFT;
	// First activation initializes previous = current so there is no fake velocity spike.
	const SplineDeformEntry e1 = storage.get_spline_entry(slot);
	CHECK(Vector3(e1.current.end_pos_axis_min[0], e1.current.end_pos_axis_min[1], e1.current.end_pos_axis_min[2]).is_equal_approx(Vector3(0, 0, 2)));
	CHECK(Vector3(e1.previous.end_pos_axis_min[0], e1.previous.end_pos_axis_min[1], e1.previous.end_pos_axis_min[2]).is_equal_approx(Vector3(0, 0, 2)));

	// Flush advances history, then next update keeps previous == p1 and current == p2.
	storage.flush();
	storage.instance_record_set_spline_deform(rec, true, p2);
	const SplineDeformEntry e2 = storage.get_spline_entry(slot);
	CHECK(Vector3(e2.current.end_pos_axis_min[0], e2.current.end_pos_axis_min[1], e2.current.end_pos_axis_min[2]).is_equal_approx(Vector3(1, 0, 2)));
	CHECK(Vector3(e2.previous.end_pos_axis_min[0], e2.previous.end_pos_axis_min[1], e2.previous.end_pos_axis_min[2]).is_equal_approx(Vector3(0, 0, 2)));

	// Clearing removes the flag.
	storage.instance_record_set_spline_deform(rec, false, SplineDeformPose());
	CHECK_EQ(storage.get_record(rec).flags & RendererVirtualMeshStorage::RECORD_HAS_SPLINE, 0u);
	storage.instance_record_free(rec);
}

TEST_CASE("[SceneTree][SplineMesh] SplineMeshInstance3D properties, swept AABB, and set_from_path()") {
	SplineMeshInstance3D *spline = memnew(SplineMeshInstance3D);
	CHECK(spline->get_start_position().is_equal_approx(Vector3(0, 0, 0)));
	CHECK(spline->get_start_tangent().is_equal_approx(Vector3(0, 0, 1)));
	CHECK(spline->get_end_position().is_equal_approx(Vector3(0, 0, 1)));
	CHECK(spline->get_end_tangent().is_equal_approx(Vector3(0, 0, 1)));
	CHECK(Math::is_equal_approx(spline->get_start_roll(), 0.0f));
	CHECK(Math::is_equal_approx(spline->get_end_roll(), 0.0f));
	CHECK(spline->get_start_scale().is_equal_approx(Vector2(1, 1)));
	CHECK(spline->get_end_scale().is_equal_approx(Vector2(1, 1)));
	CHECK_EQ(spline->get_forward_axis(), SplineMeshInstance3D::AXIS_Z);
	CHECK(spline->get_up_vector().is_equal_approx(Vector3(0, 1, 0)));

	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size(Vector3(0.5, 0.5, 2.0));
	spline->set_mesh(box);

	spline->set_start_position(Vector3(0, 0, 0));
	spline->set_start_tangent(Vector3(0, 6, 0));
	spline->set_end_position(Vector3(4, 0, 0));
	spline->set_end_tangent(Vector3(0, -6, 0));
	spline->set_start_roll(0.2f);
	spline->set_end_roll(0.8f);
	spline->set_start_scale(Vector2(1.0, 1.5));
	spline->set_end_scale(Vector2(1.5, 1.0));
	spline->set_forward_axis(SplineMeshInstance3D::AXIS_Z);

	const AABB aabb = spline->get_aabb();
	// Arch crest at t=0.5 is at (2, 1.5, 0); swept AABB must contain both endpoints and the crest.
	CHECK(aabb.has_point(Vector3(0, 0, 0)));
	CHECK(aabb.has_point(Vector3(4, 0, 0)));
	CHECK(aabb.has_point(Vector3(2, 1.5, 0)));

	// set_from_path() with curved Bezier handles: Hermite segment must match Curve3D::sample(0, t).
	Path3D *path = memnew(Path3D);
	Ref<Curve3D> curve;
	curve.instantiate();
	curve->add_point(Vector3(0, 0, 0), Vector3(0, 0, 0), Vector3(1, 2, 0));
	curve->add_point(Vector3(4, 1, 3), Vector3(-1, 1, -1), Vector3(0, 0, 0));
	curve->set_point_tilt(0, 0.3f);
	curve->set_point_tilt(1, -0.4f);
	path->set_curve(curve);

	spline->set_from_path(path, 0);
	CHECK(spline->get_start_position().is_equal_approx(Vector3(0, 0, 0)));
	CHECK(spline->get_end_position().is_equal_approx(Vector3(4, 1, 3)));
	CHECK(spline->get_start_tangent().is_equal_approx(Vector3(3, 6, 0)));
	CHECK(spline->get_end_tangent().is_equal_approx(Vector3(3, -3, 3)));
	CHECK(Math::is_equal_approx(spline->get_start_roll(), 0.3f));
	CHECK(Math::is_equal_approx(spline->get_end_roll(), -0.4f));

	// Verify exact agreement between Hermite evaluation and Curve3D::sample() along segment 0.
	const SplineDeformPose path_pose = make_pose(spline->get_start_position(), spline->get_start_tangent(), spline->get_end_position(), spline->get_end_tangent(), spline->get_start_roll(), spline->get_end_roll(), Vector2(1, 1), Vector2(1, 1), SPLINE_DEFORM_AXIS_Z, Vector3(0, 1, 0), box->get_aabb());
	for (int i = 0; i <= 10; i++) {
		const float t = float(i) / 10.0f;
		const Vector3 curve_pt = curve->sample(0, t);
		const Vector3 h_pos = spline_deform_eval_hermite_pos(path_pose, t);
		CHECK(h_pos.is_equal_approx(curve_pt));
	}

	// Zero-handle straight segment falls back to chord vector (p1 - p0).
	curve->add_point(Vector3(4, 1, 8));
	spline->set_from_path(path, 1);
	CHECK(spline->get_start_position().is_equal_approx(Vector3(4, 1, 3)));
	CHECK(spline->get_end_position().is_equal_approx(Vector3(4, 1, 8)));
	CHECK(spline->get_start_tangent().is_equal_approx(Vector3(0, 0, 5)));
	CHECK(spline->get_end_tangent().is_equal_approx(Vector3(0, 0, 5)));

	memdelete(path);
	memdelete(spline);
}

} // namespace TestSplineMesh
