/**************************************************************************/
/*  test_nextgi_far_field.cpp                                             */
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

TEST_FORCE_LINK(test_nextgi_far_field)

#include "servers/rendering/renderer_rd/environment/next_gi_far_field.h"

namespace {
NextGIFarField::ProxySurface _grid() {
	NextGIFarField::ProxySurface surface;
	constexpr uint32_t SIDE = 8;
	for (uint32_t z = 0; z <= SIDE; z++) {
		for (uint32_t x = 0; x <= SIDE; x++) {
			const float u = float(x) / SIDE;
			const float v = float(z) / SIDE;
			for (const float position : { u, 0.0f, v }) {
				surface.positions.push_back(position);
			}
			for (const float normal : { 0.0f, 1.0f, 0.0f }) {
				surface.normals.push_back(normal);
			}
			surface.uvs.push_back(u);
			surface.uvs.push_back(v);
		}
	}
	for (uint32_t z = 0; z < SIDE; z++) {
		for (uint32_t x = 0; x < SIDE; x++) {
			const uint32_t a = z * (SIDE + 1) + x;
			const uint32_t b = a + SIDE + 1;
			for (const uint32_t index : { a, b, a + 1, a + 1, b, b + 1 }) {
				surface.indices.push_back(index);
			}
		}
	}
	return surface;
}
} // namespace

TEST_CASE("[NextGI][FarField] Cell partition and visibility") {
	using Far = NextGIFarField;
	CHECK_EQ(Far::cell_coordinate(Vector3(0.0f, 0.0f, 0.0f)), Vector3i());
	CHECK_EQ(Far::cell_coordinate(Vector3(300.0f, 600.0f, 950.0f)), Vector3i(4, 9, 14));
	CHECK_EQ(Far::cell_coordinate(Vector3(-0.01f, -64.01f, -65.0f)), Vector3i(-1, -2, -2));
	CHECK(Far::cell_transform(Vector3i(4, 9, 14)).origin.is_equal_approx(Vector3(288.0f, 608.0f, 928.0f)));

	Far::Source source;
	source.mesh = RID::from_uint64(1);
	CHECK(Far::include_source(source, true, true, true, false, false, 200.0f));
	source.visibility_end = 100.0f;
	CHECK_FALSE(Far::include_source(source, true, true, true, false, false, 200.0f));
	source.visibility_end = 250.0f;
	CHECK(Far::include_source(source, true, true, true, false, false, 200.0f));
	source.mode = RSE::FAR_FIELD_NEVER;
	CHECK_FALSE(Far::include_source(source, true, true, true, false, false, 200.0f));
	source.mode = RSE::FAR_FIELD_ALWAYS;
	CHECK(Far::include_source(source, true, true, true, false, false, 200.0f));
	CHECK_FALSE(Far::include_source(source, false, true, true, false, false, 200.0f));
	CHECK_FALSE(Far::include_source(source, true, false, true, false, false, 200.0f));
	CHECK_FALSE(Far::include_source(source, true, true, false, false, false, 200.0f));
	CHECK_FALSE(Far::include_source(source, true, true, true, true, false, 200.0f));
	CHECK_FALSE(Far::include_source(source, true, true, true, false, true, 200.0f));

	const AABB landmark(Vector3(299.0f, 0.0f, 0.0f), Vector3(2.0f, 4.0f, 2.0f));
	CHECK(Far::intersects_shell(landmark, Vector3(), 200.0f, 1000.0f));
	CHECK_FALSE(Far::intersects_shell(landmark, Vector3(), 400.0f, 1000.0f));
	CHECK(Far::intersects_shell(AABB(Vector3(399.0f, 0.0f, 0.0f), Vector3(2.0f, 4.0f, 2.0f)), Vector3(), 400.0f, 1000.0f));
	CHECK_FALSE(Far::intersects_shell(landmark, Vector3(), 200.0f, 250.0f));
	CHECK_FALSE(Far::intersects_shell(landmark, Vector3(), 100.0f, 200.0f));
	CHECK_FALSE(Far::intersects_shell(landmark, Vector3(300.0f, 1.0f, 0.0f), 200.0f, 1000.0f));
	CHECK(Far::occlusion_visibility(false) == 1.0f);
	CHECK(Far::occlusion_visibility(true) == 0.0f);
}

TEST_CASE("[NextGI][FarField] Proxy simplification is deterministic and preserves attributes") {
	NextGIFarField::ProxySurface a = _grid();
	NextGIFarField::ProxySurface b = a;
	const uint32_t vertices = a.positions.size() / 3;
	const uint64_t bytes_before = a.bytes();
	REQUIRE(NextGIFarField::simplify_surface(a, 18, 0.25f / NextGIFarField::CELL_SIZE));
	REQUIRE(NextGIFarField::simplify_surface(b, 18, 0.25f / NextGIFarField::CELL_SIZE));
	REQUIRE_EQ(a.indices.size(), b.indices.size());
	REQUIRE_EQ(a.positions.size(), b.positions.size());
	for (uint32_t i = 0; i < a.indices.size(); i++) {
		CHECK_EQ(a.indices[i], b.indices[i]);
	}
	for (uint32_t i = 0; i < a.positions.size(); i++) {
		CHECK_EQ(a.positions[i], b.positions[i]);
	}
	CHECK(a.indices.size() <= 18u);
	CHECK(a.indices.size() >= 3u);
	CHECK(a.indices.size() % 3 == 0u);
	CHECK(a.positions.size() <= vertices * 3);
	CHECK(a.normals.size() == a.positions.size());
	CHECK(a.uvs.size() == a.positions.size() / 3 * 2);
	CHECK(a.bytes() < bytes_before);
	for (const uint32_t index : a.indices) {
		CHECK(index < a.positions.size() / 3);
	}
	NextGIFarField::ProxySurface invalid;
	CHECK_FALSE(NextGIFarField::simplify_surface(invalid, 18, 0.1f));
}

TEST_CASE("[NextGI][FarField] Source fingerprint changes with geometry and transform") {
	NextGIFarField::Source a;
	a.mesh = RID::from_uint64(42);
	a.revision = 7;
	NextGIFarField::Source b = a;
	CHECK_EQ(NextGIFarField::fingerprint(a), NextGIFarField::fingerprint(b));
	b.revision++;
	CHECK(NextGIFarField::fingerprint(a) != NextGIFarField::fingerprint(b));
	b = a;
	b.transform.origin.x = 1.0f;
	CHECK(NextGIFarField::fingerprint(a) != NextGIFarField::fingerprint(b));
	CHECK(NextGIFarField::MAX_PROXY_BYTES == 128ull * 1024ull * 1024ull);
}
