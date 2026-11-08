/**************************************************************************/
/*  test_nextgeo_cull.cpp                                                 */
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

TEST_FORCE_LINK(test_nextgeo_cull)

#include "servers/rendering/renderer_rd/environment/nextgeo_cull.h"

using namespace RendererRD;

TEST_CASE("[NextGeo][Cull] Projected error") {
	const Projection perspective = Projection::create_perspective(70.0f, 16.0f / 9.0f, 0.1f, 100.0f);
	const float height = 1080.0f;
	const float expected = 0.5f * 0.5f * height * perspective.columns[1][1] / 9.5f;
	const float px = nextgeo_projected_error(Vector3(0.0f, 0.0f, -10.0f), 0.5f, Transform3D(), Vector3(), 1.0f, height, perspective);
	CHECK(Math::is_equal_approx(px, expected));

	// Distance and LOD bias scale the error linearly.
	const float far_px = nextgeo_projected_error(Vector3(0.0f, 0.0f, -20.0f), 0.5f, Transform3D(), Vector3(), 1.0f, height, perspective);
	CHECK(far_px < px);
	const float biased_px = nextgeo_projected_error(Vector3(0.0f, 0.0f, -10.0f), 0.5f, Transform3D(), Vector3(), 2.0f, height, perspective);
	CHECK(Math::is_equal_approx(biased_px, px * 2.0f));

	// Instance scale multiplies the radius.
	const Transform3D scaled(Basis().scaled(Vector3(4.0f, 4.0f, 4.0f)), Vector3());
	const float scaled_px = nextgeo_projected_error(Vector3(0.0f, 0.0f, -2.5f), 0.5f, scaled, Vector3(), 1.0f, height, perspective);
	const float scaled_expected = 0.5f * 2.0f * height * perspective.columns[1][1] / 8.0f;
	CHECK(Math::is_equal_approx(scaled_px, scaled_expected));

	// Orthographic views drop the distance division.
	const Projection ortho = Projection::create_orthogonal(-50.0f, 50.0f, -25.0f, 25.0f, 0.1f, 1000.0f);
	const float ortho_px = nextgeo_projected_error(Vector3(0.0f, 0.0f, -10.0f), 0.5f, Transform3D(), Vector3(), 1.0f, height, ortho, true);
	const float ortho_expected = 0.5f * 0.5f * height * ortho.columns[1][1];
	CHECK(Math::is_equal_approx(ortho_px, ortho_expected));

	// The cut accepts at most the pixel budget.
	CHECK_FALSE(nextgeo_cut_passes(px, 1.0f));
	CHECK(nextgeo_cut_passes(0.5f, 1.0f));
}

TEST_CASE("[NextGeo][Cull] Indirect dispatch group counts") {
	CHECK_EQ(nextgeo_dispatch_group_count(0), 0u);
	CHECK_EQ(nextgeo_dispatch_group_count(1), 1u);
	CHECK_EQ(nextgeo_dispatch_group_count(64), 1u);
	CHECK_EQ(nextgeo_dispatch_group_count(65), 2u);
	CHECK_EQ(nextgeo_dispatch_group_count(4096), 64u);
}

TEST_CASE("[NextGeo][Cull] Instance predicates") {
	using V = RendererVirtualMeshStorage;
	uint32_t flags = V::RECORD_VALID | V::RECORD_VISIBLE;
	CHECK(nextgeo_instance_cullable(flags, 1, 1));
	CHECK_FALSE(nextgeo_instance_cullable(flags, 2, 1));
	CHECK_FALSE(nextgeo_instance_cullable(flags | V::RECORD_HAS_SKELETON, 1, 1));
	CHECK_FALSE(nextgeo_instance_cullable(flags | V::RECORD_HAS_INSTANCE_UNIFORMS, 1, 1));
	CHECK_FALSE(nextgeo_instance_cullable(V::RECORD_VISIBLE, 1, 1));
	CHECK_FALSE(nextgeo_instance_cullable(V::RECORD_VALID, 1, 1));
	CHECK(nextgeo_instance_cullable(flags | V::RECORD_MOVED | V::RECORD_CAST_SHADOW_ON | V::RECORD_RT_STATIC, 1, 1));
	// A layer outside the cull mask is excluded even when everything else passes.
	CHECK_FALSE(nextgeo_instance_cullable(flags, 0x80000000u, 0x00000001u));
}

TEST_CASE("[NextGeo][Cull] Order-independent cut hash") {
	LocalVector<uint64_t> a;
	a.push_back((uint64_t(1) << 32) | 2);
	a.push_back((uint64_t(3) << 32) | 4);
	a.push_back((uint64_t(5) << 32) | 6);
	LocalVector<uint64_t> b;
	b.push_back(a[2]);
	b.push_back(a[0]);
	b.push_back(a[1]);
	CHECK_EQ(nextgeo_cut_hash(a), nextgeo_cut_hash(b));

	LocalVector<uint64_t> c;
	for (uint32_t index = 0; index < a.size(); index++) {
		c.push_back(a[index]);
	}
	c.push_back((uint64_t(7) << 32) | 8);
	CHECK_NE(nextgeo_cut_hash(a), nextgeo_cut_hash(c));
	LocalVector<uint64_t> empty;
	CHECK_EQ(nextgeo_cut_hash(empty), uint64_t(0));
	// A duplicate pair changes the multiset, so the hash differs.
	LocalVector<uint64_t> twice;
	for (uint32_t index = 0; index < a.size(); index++) {
		twice.push_back(a[index]);
	}
	twice.push_back(a[0]);
	CHECK_NE(nextgeo_cut_hash(twice), nextgeo_cut_hash(a));
}

TEST_CASE("[NextGeo][Cull][Shadow] Main vs shadow cast-shadow predicates") {
	using V = RendererVirtualMeshStorage;
	const uint32_t base = V::RECORD_VALID | V::RECORD_VISIBLE;

	// CAST_SHADOW_OFF: drawn in main view, skipped in shadow views.
	CHECK(nextgeo_instance_cullable(base | V::RECORD_CAST_SHADOW_OFF, 1u, 1u));
	CHECK_FALSE(nextgeo_shadow_instance_cullable(base | V::RECORD_CAST_SHADOW_OFF, 1u, 1u));

	// CAST_SHADOW_ON and DOUBLE_SIDED: drawn in both main and shadow views.
	CHECK(nextgeo_instance_cullable(base | V::RECORD_CAST_SHADOW_ON, 1u, 1u));
	CHECK(nextgeo_shadow_instance_cullable(base | V::RECORD_CAST_SHADOW_ON, 1u, 1u));
	CHECK(nextgeo_instance_cullable(base | V::RECORD_CAST_SHADOW_DOUBLE_SIDED, 1u, 1u));
	CHECK(nextgeo_shadow_instance_cullable(base | V::RECORD_CAST_SHADOW_DOUBLE_SIDED, 1u, 1u));

	// CAST_SHADOW_SHADOWS_ONLY: excluded from main view, included in shadow views.
	CHECK_FALSE(nextgeo_instance_cullable(base | V::RECORD_CAST_SHADOW_SHADOWS_ONLY, 1u, 1u));
	CHECK(nextgeo_shadow_instance_cullable(base | V::RECORD_CAST_SHADOW_SHADOWS_ONLY, 1u, 1u));

	// Disqualifying flags and layer masks still exclude shadow instances.
	CHECK_FALSE(nextgeo_shadow_instance_cullable(base | V::RECORD_CAST_SHADOW_ON | V::RECORD_HAS_SKELETON, 1u, 1u));
	CHECK_FALSE(nextgeo_shadow_instance_cullable(base | V::RECORD_CAST_SHADOW_ON | V::RECORD_HAS_INSTANCE_UNIFORMS, 1u, 1u));
	CHECK_FALSE(nextgeo_shadow_instance_cullable(V::RECORD_VALID | V::RECORD_CAST_SHADOW_ON, 1u, 1u));
	CHECK_FALSE(nextgeo_shadow_instance_cullable(V::RECORD_VISIBLE | V::RECORD_CAST_SHADOW_ON, 1u, 1u));
	CHECK_FALSE(nextgeo_shadow_instance_cullable(base | V::RECORD_CAST_SHADOW_ON, 0x2u, 0x1u));
}

TEST_CASE("[NextGeo][Cull][Shadow] Plane conversion and orthographic texel size") {
	// Outward plane x <= 10 (normal=(2,0,0), d=20 before normalization).
	const Plane outward(Vector3(2.0f, 0.0f, 0.0f), 20.0f);
	const Vector4 fp = nextgeo_plane_to_frustum(outward);
	CHECK(Math::is_equal_approx(Vector3(fp.x, fp.y, fp.z).length(), 1.0f));
	const auto eval = [&](const Vector3 &p) {
		return fp.x * p.x + fp.y * p.y + fp.z * p.z + fp.w;
	};
	CHECK_GT(eval(Vector3(9.5f, 3.0f, -4.0f)), 0.0f); // inside
	CHECK(Math::is_zero_approx(eval(Vector3(10.0f, 0.0f, 0.0f)))); // on plane
	CHECK_LT(eval(Vector3(10.5f, 0.0f, 0.0f)), 0.0f); // outside

	// Square 4-split directional tile: 2048x2048 over 100m x 100m -> 1 texel = 100/2048 m.
	const Projection ortho_sq = Projection::create_orthogonal(-50.0f, 50.0f, -50.0f, 50.0f, 0.0f, 500.0f);
	CHECK(Math::is_equal_approx(nextgeo_ortho_inv_texel_size(Size2i(2048, 2048), ortho_sq), 2048.0f / 100.0f));

	// Non-square 2-split directional tile: 4096x2048 over 100m x 100m -> conservative MIN(inv_tx, inv_ty).
	CHECK(Math::is_equal_approx(nextgeo_ortho_inv_texel_size(Size2i(4096, 2048), ortho_sq), 2048.0f / 100.0f));
}

TEST_CASE("[NextGeo][Cull][Shadow] Shadow projected error across light types at 1.0 texel") {
	// 1. Orthographic directional split: distance-invariant.
	const Projection ortho = Projection::create_orthogonal(-50.0f, 50.0f, -50.0f, 50.0f, 0.0f, 500.0f);
	const uint32_t ortho_flags = NEXTGEO_CUT_FLAG_SHADOW_VIEW | NEXTGEO_CUT_FLAG_ORTHOGRAPHIC;
	const float one_texel_world = 100.0f / 2048.0f;
	const float err_near = nextgeo_shadow_projected_error(Vector3(0.0f, 0.0f, -5.0f), 1.0f, one_texel_world, Transform3D(), Vector3(), 1.0f, Size2i(2048, 2048), ortho, ortho_flags);
	const float err_far = nextgeo_shadow_projected_error(Vector3(0.0f, 0.0f, -350.0f), 1.0f, one_texel_world, Transform3D(), Vector3(), 1.0f, Size2i(2048, 2048), ortho, ortho_flags);
	CHECK(Math::is_equal_approx(err_near, 1.0f));
	CHECK(Math::is_equal_approx(err_far, 1.0f));
	CHECK(nextgeo_cut_passes(err_near, 1.0f));
	const Transform3D scaled2(Basis().scaled(Vector3(2.0f, 2.0f, 2.0f)), Vector3());
	const float err_scaled = nextgeo_shadow_projected_error(Vector3(0.0f, 0.0f, -5.0f), 1.0f, one_texel_world, scaled2, Vector3(), 1.0f, Size2i(2048, 2048), ortho, ortho_flags);
	CHECK(Math::is_equal_approx(err_scaled, 2.0f));
	CHECK_FALSE(nextgeo_cut_passes(err_scaled, 1.0f));

	// 2. Perspective spot / cubemap face (90 deg FOV -> abs(P[1][1]) == 1.0), including negative Y-flipped P[1][1].
	Projection cube_proj = Projection::create_perspective(90.0f, 1.0f, 0.025f, 50.0f);
	cube_proj.columns[1][1] = -cube_proj.columns[1][1];
	const uint32_t persp_flags = NEXTGEO_CUT_FLAG_SHADOW_VIEW;
	// At center z = -10.5 with sphere radius 0.5 -> d = 10.0, tile = 512 -> inv_texel = 256 / 10 = 25.6.
	const float persp_err = nextgeo_shadow_projected_error(Vector3(0.0f, 0.0f, -10.5f), 0.5f, 10.0f / 256.0f, Transform3D(), Vector3(), 1.0f, Size2i(512, 512), cube_proj, persp_flags, 0.025f);
	CHECK(Math::is_equal_approx(persp_err, 1.0f));
	CHECK(nextgeo_cut_passes(persp_err, 1.0f));

	// 3. Dual-paraboloid omni / area hemisphere (identity Projection(), conservative 0.5 * H / d).
	const uint32_t dp_flags = NEXTGEO_CUT_FLAG_SHADOW_VIEW | NEXTGEO_CUT_FLAG_DUAL_PARABOLOID;
	const float dp_err = nextgeo_shadow_projected_error(Vector3(10.5f, 0.0f, 0.0f), 0.5f, 10.0f / 255.0f, Transform3D(), Vector3(), 1.0f, Size2i(510, 510), Projection(), dp_flags, 0.025f);
	CHECK(Math::is_equal_approx(dp_err, 1.0f));
	CHECK(nextgeo_cut_passes(dp_err, 1.0f));

	// 4. Sentinel parent error (>= 3.4e38) never passes the 1.0-texel cut budget.
	const float sentinel_err = nextgeo_shadow_projected_error(Vector3(0.0f, 0.0f, -10.0f), 1.0f, 3.4e38f, Transform3D(), Vector3(), 1.0f, Size2i(512, 512), cube_proj, persp_flags, 0.025f);
	CHECK_FALSE(nextgeo_cut_passes(sentinel_err, 1.0f));
}
