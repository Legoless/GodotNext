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
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,         */
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
