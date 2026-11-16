/**************************************************************************/
/*  test_nextgeo_sw_raster.cpp                                            */
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

TEST_FORCE_LINK(test_nextgeo_sw_raster)

// GodotNext: nextgeo_sw_raster (#162, plan §21.4 N1-N3). The hybrid HW/SW rasterizer's CPU side: the
// device tier, the 64-bit visibility packing, the triangle-size switch, the software rasterizer's
// setup and edge rules against an independent reference rasterizer, the raster-feature table and the
// `.ngm` v2 max_edge field.

#include "core/io/resource_format_ngm.h"
#include "core/math/random_pcg.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_sw_raster.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_visibility.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/raster_features_inc.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/sw_raster_setup_inc.glsl.gen.h"
#include "servers/rendering/storage/virtual_mesh_storage.h"

#include <cmath>
#include <cstring>

using namespace RendererRD;

namespace TestNextGeoSwRaster {

uint64_t feature_bit(RenderingDevice::Features p_feature) {
	return uint64_t(1) << p_feature;
}

// The reference rasterizer. It shares only the specification with nextgeo_sw_setup_triangle() and
// nextgeo_sw_sample(): vertices snap to 1 / 2^bits pixel (ties to even), pixel (x, y) samples its
// center, and an exactly-on-edge sample follows the top-left rule. It derives the rule differently:
// a sample on an edge counts when moving it by (e, e^2), for an infinitely small e > 0, moves it
// inside, which is the top-left rule for framebuffer y pointing down. It uses absolute 64-bit fixed
// point and double-precision barycentrics, with no bounding box or relative origin.
struct RefTriangle {
	bool valid = false;
	int bits = 0;
	int64_t x[3] = {};
	int64_t y[3] = {};
	double z[3] = {};
	int64_t area = 0;
};

int ref_subpixel_bits(double p_extent) {
	if (p_extent <= 64.0) {
		return 8;
	}
	return 8 - int(std::ceil(std::log2(p_extent / 64.0)));
}

RefTriangle ref_setup(const float p_px[3][2], const float p_z[3], int p_width, int p_height, NextGeoSwFacing p_facing) {
	RefTriangle tri;
	float min_x = p_px[0][0];
	float max_x = p_px[0][0];
	float min_y = p_px[0][1];
	float max_y = p_px[0][1];
	for (int i = 1; i < 3; i++) {
		min_x = MIN(min_x, p_px[i][0]);
		max_x = MAX(max_x, p_px[i][0]);
		min_y = MIN(min_y, p_px[i][1]);
		max_y = MAX(max_y, p_px[i][1]);
	}
	if (max_x < 0.0f || max_y < 0.0f || min_x > float(p_width) || min_y > float(p_height)) {
		return tri;
	}
	const float extent = MAX(max_x - min_x, max_y - min_y);
	if (double(extent) > double(NEXTGEO_SW_FULL_PRECISION_EXTENT_PX) * 128.0) {
		return tri;
	}
	tri.bits = ref_subpixel_bits(double(extent));
	for (int i = 0; i < 3; i++) {
		tri.x[i] = int64_t(std::nearbyint(double(p_px[i][0]) * double(int64_t(1) << tri.bits)));
		tri.y[i] = int64_t(std::nearbyint(double(p_px[i][1]) * double(int64_t(1) << tri.bits)));
		tri.z[i] = double(p_z[i]);
	}
	tri.area = (tri.x[1] - tri.x[0]) * (tri.y[2] - tri.y[0]) - (tri.y[1] - tri.y[0]) * (tri.x[2] - tri.x[0]);
	if (tri.area == 0 || (p_facing == NEXTGEO_SW_KEEP_POSITIVE && tri.area < 0) || (p_facing == NEXTGEO_SW_KEEP_NEGATIVE && tri.area > 0)) {
		return tri;
	}
	if (tri.area < 0) {
		SWAP(tri.x[1], tri.x[2]);
		SWAP(tri.y[1], tri.y[2]);
		SWAP(tri.z[1], tri.z[2]);
		tri.area = -tri.area;
	}
	tri.valid = true;
	return tri;
}

bool ref_inside_edge(int64_t p_ax, int64_t p_ay, int64_t p_bx, int64_t p_by, int64_t p_sx, int64_t p_sy) {
	const int64_t e = (p_bx - p_ax) * (p_sy - p_ay) - (p_by - p_ay) * (p_sx - p_ax);
	if (e != 0) {
		return e > 0;
	}
	// e(s + (d, d^2)) = e(s) + d * de/dx + d^2 * de/dy.
	const int64_t de_dx = -(p_by - p_ay);
	if (de_dx != 0) {
		return de_dx > 0;
	}
	return (p_bx - p_ax) > 0;
}

bool ref_sample(const RefTriangle &p_tri, int p_x, int p_y, double &r_depth) {
	if (!p_tri.valid) {
		return false;
	}
	const int64_t half = (int64_t(1) << p_tri.bits) / 2;
	const int64_t sx = (int64_t(p_x) << p_tri.bits) + half;
	const int64_t sy = (int64_t(p_y) << p_tri.bits) + half;
	for (int i = 0; i < 3; i++) {
		const int j = (i + 1) % 3;
		if (!ref_inside_edge(p_tri.x[i], p_tri.y[i], p_tri.x[j], p_tri.y[j], sx, sy)) {
			return false;
		}
	}
	double w[3];
	for (int i = 0; i < 3; i++) {
		const int j = (i + 1) % 3;
		const int k = (i + 2) % 3;
		w[i] = double((p_tri.x[k] - p_tri.x[j]) * (sy - p_tri.y[j]) - (p_tri.y[k] - p_tri.y[j]) * (sx - p_tri.x[j])) / double(p_tri.area);
	}
	r_depth = w[0] * p_tri.z[0] + w[1] * p_tri.z[1] + w[2] * p_tri.z[2];
	return true;
}

struct RasterCompare {
	int covered = 0;
	int mismatches = 0;
	double max_depth_error = 0.0;
};

// Rasterizes one triangle with the software rasterizer and the reference over the whole target.
void compare_triangle(const float p_px[3][2], const float p_z[3], int p_width, int p_height, NextGeoSwFacing p_facing, RasterCompare &r_result) {
	const NextGeoSwTriangle tri = nextgeo_sw_setup_triangle(p_px, p_z, p_width, p_height, p_facing);
	const RefTriangle ref = ref_setup(p_px, p_z, p_width, p_height, p_facing);
	for (int y = 0; y < p_height; y++) {
		for (int x = 0; x < p_width; x++) {
			float depth = 0.0f;
			double ref_depth = 0.0;
			const bool covered = nextgeo_sw_sample(tri, x, y, depth);
			const bool ref_covered = ref_sample(ref, x, y, ref_depth);
			if (covered != ref_covered) {
				r_result.mismatches++;
				continue;
			}
			if (covered) {
				r_result.covered++;
				r_result.max_depth_error = MAX(r_result.max_depth_error, std::abs(double(depth) - ref_depth));
			}
		}
	}
}

float random_coordinate(RandomPCG &p_rng, float p_min, float p_max) {
	const float value = p_rng.random(p_min, p_max);
	switch (p_rng.rand(4)) {
		case 0:
			// On pixel centers and pixel edges, where the edge rules decide.
			return Math::round(value * 2.0f) * 0.5f;
		case 1:
			// On the full-precision snapping grid.
			return Math::round(value * 256.0f) / 256.0f;
		default:
			return value;
	}
}

// The GLSL copies keep "const <type> NAME = <value>;" lines; returns the value text of NAME.
String glsl_constant(const char *p_source, const String &p_name) {
	const String source = String::utf8(p_source);
	const String key = " " + p_name + " = ";
	const int at = source.find(key);
	if (at < 0) {
		return String();
	}
	const int begin = at + key.length();
	const int end = source.find(";", begin);
	return end < 0 ? String() : source.substr(begin, end - begin).strip_edges();
}

// "1u << 3u" or "7u" or "8" as a number.
int64_t glsl_integer(const String &p_value) {
	if (p_value.contains("<<")) {
		const PackedStringArray parts = p_value.split("<<");
		return glsl_integer(parts[0].strip_edges()) << glsl_integer(parts[1].strip_edges());
	}
	return p_value.trim_suffix("u").to_int();
}

class TestSwRasterVirtualMeshStorage : public RendererVirtualMeshStorage {
public:
	Vector<uint8_t> pages;

protected:
	void _upload_records(uint32_t, uint32_t) override {}
	void _upload_mesh_records(uint32_t, uint32_t) override {}
	bool _resize_dag(uint32_t) override { return true; }
	void _upload_dag(uint32_t, const uint8_t *, uint32_t) override {}
	bool _resize_pages(uint32_t p_bytes) override {
		pages.resize(int(p_bytes));
		return true;
	}
	void _upload_pages(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) override {
		if (p_offset + p_size <= uint32_t(pages.size())) {
			memcpy(pages.ptrw() + p_offset, p_data, p_size);
		}
	}
	void _reset_pages() override { pages.clear(); }
	bool _resize_clusters(uint32_t) override { return true; }
	void _upload_clusters(uint32_t, const uint8_t *, uint32_t) override {}
	void _reset_clusters() override {}
};

void store_u32(uint8_t *p_dst, uint32_t p_value) {
	memcpy(p_dst, &p_value, sizeof(uint32_t));
}

uint32_t load_u32(const uint8_t *p_src) {
	uint32_t value = 0;
	memcpy(&value, p_src, sizeof(uint32_t));
	return value;
}

// A v1 page payload: two clusters over one vertex run (stride 3). Cluster 0 is the triangle
// (0,0,0) (3,0,0) (0,4,0) with a 5-unit hypotenuse; cluster 1 is (0,0,0) (1,0,0) (0,0,2), longest
// edge sqrt(5). Record bytes 64..67 hold the v1 page_id, 0.
Vector<uint8_t> make_v1_page() {
	const float vertices[] = { 0, 0, 0, 3, 0, 0, 0, 4, 0, 0, 0, 0, 1, 0, 0, 0, 0, 2 };
	const uint8_t triangles[] = { 0, 1, 2, 0, 1, 2 };
	Vector<uint8_t> page;
	page.resize(16 + 2 * 80 + int(sizeof(vertices)) + int(sizeof(triangles)));
	memset(page.ptrw(), 0, page.size());
	uint8_t *bytes = page.ptrw();
	store_u32(bytes + 0, 2);
	store_u32(bytes + 4, 6);
	store_u32(bytes + 8, 2);
	for (uint32_t c = 0; c < 2; c++) {
		uint8_t *record = bytes + 16 + c * 80;
		store_u32(record + 60, uint32_t(-1)); // refined_group: none.
		store_u32(record + 64, 0); // v1 page_id.
		store_u32(record + 68, c * 3); // First vertex.
		store_u32(record + 72, c); // First triangle.
		record[76] = 3;
		record[77] = 1;
	}
	memcpy(bytes + 16 + 160, vertices, sizeof(vertices));
	memcpy(bytes + 16 + 160 + sizeof(vertices), triangles, sizeof(triangles));
	return page;
}

TEST_CASE("[NextGeo][SwRaster] The Nanite device tier needs 64-bit atomics and indirect-count draws (N1)") {
	const NextGeoRasterTier none = nextgeo_raster_tier(false, false, true);
	CHECK_FALSE(none.hybrid);
	CHECK_EQ(none.storage, NEXTGEO_VIS64_NONE);
	CHECK_EQ(none.reason, NEXTGEO_FALLBACK_NO_INT64_ATOMICS);

	const NextGeoRasterTier no_count = nextgeo_raster_tier(true, true, false);
	CHECK_FALSE(no_count.hybrid);
	CHECK_EQ(no_count.reason, NEXTGEO_FALLBACK_NO_INT64_ATOMICS);

	const NextGeoRasterTier buffer_only = nextgeo_raster_tier(false, true, true);
	CHECK(buffer_only.hybrid);
	CHECK_EQ(buffer_only.storage, NEXTGEO_VIS64_BUFFER);
	CHECK_EQ(buffer_only.reason, NEXTGEO_FALLBACK_NONE);

	const NextGeoRasterTier image = nextgeo_raster_tier(true, true, true);
	CHECK(image.hybrid);
	CHECK_EQ(image.storage, NEXTGEO_VIS64_IMAGE);
	CHECK_EQ(image.reason, NEXTGEO_FALLBACK_NONE);

	// The reason is a §6.3 fallback value, appended after the existing ones.
	CHECK_EQ(uint32_t(NEXTGEO_FALLBACK_NO_INT64_ATOMICS), uint32_t(NEXTGEO_FALLBACK_ALPHA) + 1u);
	CHECK_EQ(String(nextgeo_fallback_name(NEXTGEO_FALLBACK_NO_INT64_ATOMICS)), String("NO_INT64_ATOMICS"));
	CHECK_EQ(String(nextgeo_raster_mode_name(NEXTGEO_RASTER_MODE_HYBRID)), String("hybrid"));
	CHECK_EQ(String(nextgeo_raster_mode_name(NEXTGEO_RASTER_MODE_HARDWARE)), String("hardware"));
}

TEST_CASE("[NextGeo][SwRaster] A device mocked without 64-bit atomics keeps the v2.4 hardware raster (GN-13)") {
	RenderingDevice *rd = memnew(RenderingDevice);

	rd->set_feature_mock(true, feature_bit(RenderingDevice::SUPPORTS_DRAW_INDIRECT_COUNT));
	const NextGeoRasterTier no_int64 = nextgeo_raster_tier(rd);
	CHECK_FALSE(no_int64.hybrid);
	CHECK_EQ(no_int64.storage, NEXTGEO_VIS64_NONE);
	CHECK_EQ(no_int64.reason, NEXTGEO_FALLBACK_NO_INT64_ATOMICS);

	// Metal without image atomics: the buffer form of the visibility buffer.
	rd->set_feature_mock(true, feature_bit(RenderingDevice::SUPPORTS_DRAW_INDIRECT_COUNT) | feature_bit(RenderingDevice::SUPPORTS_BUFFER_INT64_ATOMICS));
	const NextGeoRasterTier buffer = nextgeo_raster_tier(rd);
	CHECK(buffer.hybrid);
	CHECK_EQ(buffer.storage, NEXTGEO_VIS64_BUFFER);

	rd->set_feature_mock(true, feature_bit(RenderingDevice::SUPPORTS_DRAW_INDIRECT_COUNT) | feature_bit(RenderingDevice::SUPPORTS_BUFFER_INT64_ATOMICS) | feature_bit(RenderingDevice::SUPPORTS_IMAGE_INT64_ATOMICS));
	const NextGeoRasterTier image = nextgeo_raster_tier(rd);
	CHECK(image.hybrid);
	CHECK_EQ(image.storage, NEXTGEO_VIS64_IMAGE);
	CHECK_EQ(image.reason, NEXTGEO_FALLBACK_NONE);

	rd->set_feature_mock(true, feature_bit(RenderingDevice::SUPPORTS_BUFFER_INT64_ATOMICS) | feature_bit(RenderingDevice::SUPPORTS_IMAGE_INT64_ATOMICS));
	CHECK_FALSE(nextgeo_raster_tier(rd).hybrid);
	memdelete(rd);

	CHECK_FALSE(nextgeo_raster_tier(static_cast<const RenderingDevice *>(nullptr)).hybrid);
}

TEST_CASE("[NextGeo][SwRaster] The 64-bit visibility texel packs reversed-Z depth over the v2.4 ID (N2, MN1)") {
	const uint64_t texel = nextgeo_pack_vis64(0.5f, nextgeo_pack_visibility_id(42u, 7u));
	CHECK_EQ(uint32_t(texel >> 32u), nextgeo_depth_bits(0.5f));
	CHECK_EQ(nextgeo_vis64_visibility_id(texel), (42u << 7u) | 7u);
	CHECK_EQ(nextgeo_vis64_depth(texel), 0.5f);
	CHECK_FALSE(nextgeo_vis64_is_empty(texel));
	CHECK(nextgeo_vis64_is_empty(NEXTGEO_VIS64_EMPTY));
	// The far plane is depth 0, the clear value: it never wins a texel.
	CHECK(nextgeo_vis64_is_empty(nextgeo_pack_vis64(0.0f, 5u)));

	// atomicMax keeps the nearest fragment (larger reversed-Z depth) whatever the IDs, and the larger
	// ID at equal depth.
	RandomPCG rng(1621);
	for (int i = 0; i < 4096; i++) {
		const float depth_a = rng.random(1e-30f, 1.0f);
		const float depth_b = (i % 4 == 0) ? depth_a : rng.random(1e-30f, 1.0f);
		const uint32_t id_a = rng.rand();
		const uint32_t id_b = rng.rand();
		const uint64_t winner = MAX(nextgeo_pack_vis64(depth_a, id_a), nextgeo_pack_vis64(depth_b, id_b));
		if (depth_a != depth_b) {
			CHECK_EQ(nextgeo_vis64_depth(winner), MAX(depth_a, depth_b));
			CHECK_EQ(nextgeo_vis64_visibility_id(winner), depth_a > depth_b ? id_a : id_b);
		} else {
			CHECK_EQ(nextgeo_vis64_visibility_id(winner), MAX(id_a, id_b));
		}
	}

	// MN1: 8 B per internal pixel, 16.6 MB at 1080p and 66.4 MB at 2160p.
	CHECK_EQ(NEXTGEO_VIS64_TEXEL_BYTES, 8u);
	CHECK_EQ(uint64_t(NEXTGEO_VIS64_TEXEL_BYTES) * 1920u * 1080u, uint64_t(16588800u));
	CHECK_EQ(uint64_t(NEXTGEO_VIS64_TEXEL_BYTES) * 3840u * 2160u, uint64_t(66355200u));
}

TEST_CASE("[NextGeo][SwRaster] The triangle-size switch sends small clusters in front of the near plane to software (N2)") {
	const float width = 1920.0f;
	const float height = 1080.0f;
	const Projection perspective = Projection::create_perspective(90.0f, 16.0f / 9.0f, 0.05f, 500.0f);
	NextGeoSwRasterView view;
	view.max_edge_px = 8.0f;
	view.px_scale = nextgeo_sw_raster_px_scale(perspective, width, height, false);
	view.z_near = 0.05f;

	// tan(fovy / 2) = 1: 540 px per unit at unit depth, widened by sqrt(1 + (16/9)^2 + 1) off axis.
	CHECK(view.px_scale == doctest::Approx(540.0 * std::sqrt(2.0 + (16.0 / 9.0) * (16.0 / 9.0))).epsilon(1e-5));

	// A 1 cm edge 10 m away projects to about 1.2 px: software.
	CHECK(nextgeo_sw_raster_route(0.01f, 1.0f, 10.0f, 0.05f, view));
	// The same edge on an instance scaled by 100 projects to about 245 px: hardware.
	CHECK_FALSE(nextgeo_sw_raster_route(0.01f, 100.0f, 10.0f, 0.05f, view));
	// A cluster whose sphere crosses the near plane goes to hardware, however small its triangles are.
	CHECK_FALSE(nextgeo_sw_raster_route(0.0001f, 1.0f, 0.06f, 0.02f, view));
	// T = 0 sends every cluster to hardware.
	NextGeoSwRasterView hardware_only = view;
	hardware_only.max_edge_px = 0.0f;
	CHECK_FALSE(nextgeo_sw_raster_route(0.0001f, 1.0f, 10.0f, 0.05f, hardware_only));
	// An unknown or unbounded edge goes to hardware.
	CHECK_FALSE(nextgeo_sw_raster_route(INFINITY, 1.0f, 10.0f, 0.05f, view));
	CHECK_FALSE(nextgeo_sw_raster_route(NAN, 1.0f, 10.0f, 0.05f, view));
	CHECK_FALSE(nextgeo_sw_raster_route(-1.0f, 1.0f, 10.0f, 0.05f, view));
	// A scale that is not positive goes to hardware: a degenerate basis, or the record's signed
	// world_aabb_min.w of a mirrored instance passed by mistake.
	CHECK_FALSE(nextgeo_sw_raster_route(0.01f, -1.0f, 10.0f, 0.05f, view));
	CHECK_FALSE(nextgeo_sw_raster_route(0.01f, 0.0f, 10.0f, 0.05f, view));
	CHECK_FALSE(nextgeo_sw_raster_route(0.01f, NAN, 10.0f, 0.05f, view));
	// The test is strict: an edge that projects to exactly T px goes to hardware.
	const float depth = 4.0f;
	const float radius = 0.5f;
	const float exact_edge = view.max_edge_px * (depth - radius) / view.px_scale;
	const float projected = nextgeo_sw_raster_projected_edge(exact_edge, 1.0f, depth, radius, view);
	CHECK_EQ(nextgeo_sw_raster_route(exact_edge, 1.0f, depth, radius, view), projected < view.max_edge_px);
	CHECK(nextgeo_sw_raster_route(exact_edge * 0.99f, 1.0f, depth, radius, view));
	CHECK_FALSE(nextgeo_sw_raster_route(exact_edge * 1.01f, 1.0f, depth, radius, view));

	// Orthographic: a length projects the same at any depth.
	const Projection ortho = Projection::create_orthogonal(-10.0f, 10.0f, -5.0f, 5.0f, 0.05f, 100.0f);
	NextGeoSwRasterView ortho_view;
	ortho_view.max_edge_px = 8.0f;
	ortho_view.px_scale = nextgeo_sw_raster_px_scale(ortho, width, height, true);
	ortho_view.z_near = 0.05f;
	ortho_view.orthographic = true;
	CHECK(ortho_view.px_scale == doctest::Approx(108.0));
	CHECK(nextgeo_sw_raster_route(0.07f, 1.0f, 2.0f, 0.1f, ortho_view));
	CHECK(nextgeo_sw_raster_route(0.07f, 1.0f, 90.0f, 0.1f, ortho_view));
	CHECK_FALSE(nextgeo_sw_raster_route(0.08f, 1.0f, 90.0f, 0.1f, ortho_view));
}

// Projects random edges inside p_projection's frustum and checks each against the switch's estimate.
// Returns the largest ratio of a projected edge to its estimate.
double check_edge_bound(const Projection &p_projection, int p_width, int p_height, uint32_t p_seed) {
	NextGeoSwRasterView view;
	view.max_edge_px = 8.0f;
	view.px_scale = nextgeo_sw_raster_px_scale(p_projection, float(p_width), float(p_height), false);
	view.z_near = 0.1f;
	const double p00 = double(p_projection.columns[0][0]);
	const double p11 = double(p_projection.columns[1][1]);
	const double p20 = double(p_projection.columns[2][0]);
	const double p21 = double(p_projection.columns[2][1]);

	RandomPCG rng(p_seed);
	int checked = 0;
	double tightest = 0.0;
	for (int i = 0; i < 20000; i++) {
		// Two points inside the frustum, as a cluster's vertices, at most 2 m apart: a at a random
		// depth along -z and a random NDC position, so x / depth = (ndc_x + P[2][0]) / P[0][0].
		const double depth = rng.random(0.5f, 200.0f);
		Vector3 a;
		a.z = float(-depth);
		a.x = float((rng.random(-1.0f, 1.0f) + p20) / p00 * depth);
		a.y = float((rng.random(-1.0f, 1.0f) + p21) / p11 * depth);
		const Vector3 offset = Vector3(rng.random(-1.0f, 1.0f), rng.random(-1.0f, 1.0f), rng.random(-1.0f, 1.0f));
		const Vector3 b = a + offset;
		const Vector3 points[2] = { a, b };
		Vector2 pixels[2];
		bool inside = -b.z >= 0.2f;
		for (int p = 0; p < 2; p++) {
			const Vector4 clip = p_projection.xform(Vector4(points[p].x, points[p].y, points[p].z, 1.0f));
			const Vector2 ndc = Vector2(clip.x / clip.w, clip.y / clip.w);
			inside = inside && Math::abs(ndc.x) <= 1.0f && Math::abs(ndc.y) <= 1.0f;
			pixels[p] = Vector2((ndc.x * 0.5f + 0.5f) * float(p_width), (ndc.y * 0.5f + 0.5f) * float(p_height));
		}
		if (!inside) {
			continue;
		}
		const Vector3 center = (a + b) * 0.5f;
		const float radius = (b - a).length() * 0.5f;
		const float estimate = nextgeo_sw_raster_projected_edge((b - a).length(), 1.0f, -center.z, radius, view);
		const double exact = double((pixels[1] - pixels[0]).length());
		CHECK_MESSAGE(exact <= double(estimate) * (1.0 + 1e-4) + 1e-3, vformat("edge %s - %s projects to %f px, estimated %f px", a, b, exact, estimate));
		tightest = MAX(tightest, exact / MAX(double(estimate), 1e-9));
		checked++;
	}
	CHECK(checked > 10000);
	return tightest;
}

TEST_CASE("[NextGeo][SwRaster] The projected-edge estimate bounds every edge inside the frustum") {
	const int width = 1920;
	const int height = 1080;
	// The bound is not vacuous: some edges come close to it.
	CHECK(check_edge_bound(Projection::create_perspective(70.0f, float(width) / float(height), 0.1f, 1000.0f), width, height, 1622) > 0.5);
	// Off-axis frustums: P[2][0] and P[2][1] shift the screen, so one side reaches further than
	// 1 / P[0][0] and 1 / P[1][1]. The first also has pixels that are not square at 1920x1080.
	CHECK(check_edge_bound(Projection::create_frustum(-0.02f, 0.1f, -0.03f, 0.05f, 0.1f, 1000.0f), width, height, 1625) > 0.5);
	// A Camera3D frustum offset.
	CHECK(check_edge_bound(Projection::create_frustum_aspect(0.12f, float(width) / float(height), Vector2(0.05f, -0.03f), 0.1f, 1000.0f), width, height, 1626) > 0.5);
}

TEST_CASE("[NextGeo][SwRaster] The switch measures an instance by its largest stretch, mirrored or not (N2)") {
	// Basis::get_scale() carries the determinant's sign, so the largest component of a mirrored
	// instance's scale, the record's world_aabb_min.w, is negative, and for a non-uniform scale it is
	// the smallest axis.
	const Basis mirrored = Basis::from_scale(Vector3(-1.0f, 4.0f, 1.0f));
	const Vector3 signed_scale = mirrored.get_scale();
	CHECK(MAX(signed_scale.x, MAX(signed_scale.y, signed_scale.z)) == doctest::Approx(-1.0));
	CHECK(nextgeo_sw_raster_instance_scale(mirrored) == doctest::Approx(4.0).epsilon(1e-6));
	CHECK(nextgeo_sw_raster_instance_scale(Basis::from_scale(Vector3(-0.8f, 0.8f, 0.8f))) == doctest::Approx(0.8).epsilon(1e-6));
	// A flattened axis leaves the other two.
	CHECK(nextgeo_sw_raster_instance_scale(Basis::from_scale(Vector3(0.0f, 2.0f, 1.0f))) == doctest::Approx(2.0).epsilon(1e-6));
	// A rotation scaled non-uniformly afterwards, as under a non-uniformly scaled parent, is sheared:
	// its longest column (about 2.92) falls short of its stretch of 4 along the parent's x axis.
	const Basis sheared = Basis(Vector3(0.0f, 0.0f, 1.0f), Math::PI / 4.0f).scaled(Vector3(4.0f, 1.0f, 1.0f));
	CHECK(MAX(sheared.get_column(0).length(), MAX(sheared.get_column(1).length(), sheared.get_column(2).length())) < 3.0f);
	CHECK(nextgeo_sw_raster_instance_scale(sheared) == doctest::Approx(4.0).epsilon(1e-5));

	RandomPCG rng(1624);
	for (int i = 0; i < 2000; i++) {
		// A rotation times a scale, mirrored every other time: exact.
		const Basis rotation = Basis::from_euler(Vector3(rng.random(-3.2f, 3.2f), rng.random(-3.2f, 3.2f), rng.random(-3.2f, 3.2f)));
		Vector3 scale(rng.random(0.05f, 5.0f), rng.random(0.05f, 5.0f), rng.random(0.05f, 5.0f));
		if (i % 2 == 0) {
			scale.x = -scale.x;
		}
		const Basis child = rotation * Basis::from_scale(scale);
		const float largest = MAX(Math::abs(scale.x), MAX(Math::abs(scale.y), Math::abs(scale.z)));
		CHECK(nextgeo_sw_raster_instance_scale(child) == doctest::Approx(largest).epsilon(1e-4));

		// Under a non-uniformly scaled, rotated parent: never below the largest singular value
		// (power iteration on the Gram matrix), and within 3^(1/4) of it.
		const Basis parent = Basis::from_euler(Vector3(rng.random(-3.2f, 3.2f), rng.random(-3.2f, 3.2f), rng.random(-3.2f, 3.2f))) * Basis::from_scale(Vector3(rng.random(0.1f, 4.0f), rng.random(0.1f, 4.0f), rng.random(0.1f, 4.0f)));
		const Basis global = parent * child;
		double gram[3][3];
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 3; c++) {
				gram[r][c] = double(global.get_column(r).dot(global.get_column(c)));
			}
		}
		double v[3] = { 0.6, 0.5, 0.62 };
		double eigenvalue = 0.0;
		for (int iteration = 0; iteration < 500; iteration++) {
			double w[3];
			for (int r = 0; r < 3; r++) {
				w[r] = gram[r][0] * v[0] + gram[r][1] * v[1] + gram[r][2] * v[2];
			}
			eigenvalue = v[0] * w[0] + v[1] * w[1] + v[2] * w[2];
			const double length = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
			for (int r = 0; r < 3; r++) {
				v[r] = w[r] / length;
			}
		}
		const double singular = std::sqrt(eigenvalue);
		const double bound = double(nextgeo_sw_raster_instance_scale(global));
		CHECK(bound >= singular * (1.0 - 1e-5));
		CHECK(bound <= singular * 1.32);
	}

	// The switch with that stretch, perspective 90 degrees at 1920x1080 and T = 8 px.
	const Projection perspective = Projection::create_perspective(90.0f, 16.0f / 9.0f, 0.05f, 500.0f);
	NextGeoSwRasterView view;
	view.max_edge_px = 8.0f;
	view.px_scale = nextgeo_sw_raster_px_scale(perspective, 1920.0f, 1080.0f, false);
	view.z_near = 0.05f;
	const float unit_mirrored = nextgeo_sw_raster_instance_scale(Basis::from_scale(Vector3(-1.0f, 1.0f, 1.0f)));
	// A small, distant cluster of a mirrored instance still goes to software.
	CHECK(nextgeo_sw_raster_route(0.01f, unit_mirrored, 10.0f, 0.05f, view));
	// A 1 cm edge on an instance mirrored and scaled by 100, 10 m away, projects to about 245 px:
	// hardware.
	CHECK_FALSE(nextgeo_sw_raster_route(0.01f, nextgeo_sw_raster_instance_scale(Basis::from_scale(Vector3(-100.0f, 100.0f, 100.0f))), 10.0f, 0.05f, view));
	// A mirrored cluster whose sphere crosses the near plane goes to hardware.
	CHECK_FALSE(nextgeo_sw_raster_route(0.0001f, unit_mirrored, 0.3f, 0.5f, view));
	// A sphere of radius 0.1 at depth 0.3 crosses the near plane only through the 4x axis of a
	// mirrored (-1, 4, 1) instance.
	CHECK(nextgeo_sw_raster_route(0.0001f, 1.0f, 0.3f, 0.1f, view));
	CHECK_FALSE(nextgeo_sw_raster_route(0.0001f, nextgeo_sw_raster_instance_scale(mirrored), 0.3f, 0.1f, view));
}

TEST_CASE("[NextGeo][SwRaster] The switch threshold is clamped to the full-precision extent") {
	CHECK_EQ(nextgeo_sw_raster_switch_px(8.0f), 8.0f);
	CHECK_EQ(nextgeo_sw_raster_switch_px(64.0f), 64.0f);
	CHECK_EQ(nextgeo_sw_raster_switch_px(1000.0f), 64.0f);
	CHECK_EQ(nextgeo_sw_raster_switch_px(INFINITY), 64.0f);
	CHECK_EQ(nextgeo_sw_raster_switch_px(0.0f), 0.0f);
	CHECK_EQ(nextgeo_sw_raster_switch_px(-3.0f), 0.0f);
	CHECK_EQ(nextgeo_sw_raster_switch_px(NAN), 0.0f);

	// Why: two software triangles sharing an edge, one spanning 60 px and one 100 px, would snap that
	// edge to different grids.
	const float z[3] = { 0.5f, 0.5f, 0.5f };
	const float short_px[3][2] = { { 2.3f, 2.7f }, { 62.3f, 3.1f }, { 2.9f, 40.2f } };
	const float long_px[3][2] = { { 62.3f, 3.1f }, { 2.3f, 2.7f }, { 60.1f, 102.7f } };
	const NextGeoSwTriangle short_tri = nextgeo_sw_setup_triangle(short_px, z, 128, 128, NEXTGEO_SW_KEEP_BOTH);
	const NextGeoSwTriangle long_tri = nextgeo_sw_setup_triangle(long_px, z, 128, 128, NEXTGEO_SW_KEEP_BOTH);
	REQUIRE(short_tri.valid);
	REQUIRE(long_tri.valid);
	CHECK_EQ(short_tri.bits, NEXTGEO_SW_SUBPIXEL_BITS);
	CHECK_EQ(long_tri.bits, NEXTGEO_SW_SUBPIXEL_BITS - 1);

	// With the setting at 1000 px, a cluster whose longest edge projects to 100 px stays on hardware
	// and one at 60 px goes to software.
	const Projection perspective = Projection::create_perspective(70.0f, 16.0f / 9.0f, 0.1f, 1000.0f);
	NextGeoSwRasterView view;
	view.max_edge_px = nextgeo_sw_raster_switch_px(1000.0f);
	view.px_scale = nextgeo_sw_raster_px_scale(perspective, 1920.0f, 1080.0f, false);
	view.z_near = 0.1f;
	const float depth = 5.0f;
	const float radius = 0.1f;
	CHECK(nextgeo_sw_raster_route(60.0f * (depth - radius) / view.px_scale, 1.0f, depth, radius, view));
	CHECK_FALSE(nextgeo_sw_raster_route(100.0f * (depth - radius) / view.px_scale, 1.0f, depth, radius, view));
}

TEST_CASE("[NextGeo][SwRaster] Software setup and edge rules match the reference rasterizer") {
	const int width = 48;
	const int height = 40;
	RandomPCG rng(1623);
	RasterCompare total;
	int degenerate = 0;
	for (int i = 0; i < 3000; i++) {
		// Sizes from sub-pixel to most of the target, some partly outside it.
		const float size = (i % 3 == 0) ? rng.random(0.2f, 3.0f) : rng.random(1.0f, 60.0f);
		const float cx = rng.random(-4.0f, float(width) + 4.0f);
		const float cy = rng.random(-4.0f, float(height) + 4.0f);
		float px[3][2];
		float z[3];
		for (int v = 0; v < 3; v++) {
			px[v][0] = random_coordinate(rng, cx - size, cx + size);
			px[v][1] = random_coordinate(rng, cy - size, cy + size);
			z[v] = rng.random(0.001f, 1.0f);
		}
		if (i % 7 == 0) {
			// An exactly horizontal edge, top or bottom.
			px[1][1] = px[0][1];
		}
		const NextGeoSwFacing facing = (i % 5 == 0) ? NEXTGEO_SW_KEEP_BOTH : ((i % 5 == 1) ? NEXTGEO_SW_KEEP_NEGATIVE : NEXTGEO_SW_KEEP_POSITIVE);
		// Setup also rejects a triangle whose box holds no pixel center, so only coverage is compared
		// for those.
		const NextGeoSwTriangle tri = nextgeo_sw_setup_triangle(px, z, width, height, facing);
		const RefTriangle ref = ref_setup(px, z, width, height, facing);
		if (tri.valid && ref.valid) {
			CHECK_EQ(tri.bits, ref.bits);
		} else {
			degenerate++;
		}
		compare_triangle(px, z, width, height, facing, total);
	}
	CHECK_EQ(total.mismatches, 0);
	CHECK(total.covered > 100000);
	// Culled or empty triangles are part of the sample, not most of it.
	CHECK(degenerate < 2250);
	CHECK(total.max_depth_error < 1e-5);
}

TEST_CASE("[NextGeo][SwRaster] Larger triangles snap to a coarser grid and still match the reference") {
	RasterCompare total;
	// Extents of 100, 300 and 3000 px use 7, 5 and 2 subpixel bits.
	const float extents[] = { 100.0f, 300.0f, 3000.0f };
	const int expected_bits[] = { 7, 5, 2 };
	for (int e = 0; e < 3; e++) {
		const float extent = extents[e];
		const float px[3][2] = { { 3.3f, 2.7f }, { 3.3f + extent, 10.1f }, { 40.6f, 2.7f + extent * 0.5f } };
		const float z[3] = { 0.25f, 0.5f, 0.75f };
		const NextGeoSwTriangle tri = nextgeo_sw_setup_triangle(px, z, 96, 96, NEXTGEO_SW_KEEP_BOTH);
		REQUIRE(tri.valid);
		CHECK_EQ(tri.bits, expected_bits[e]);
		compare_triangle(px, z, 96, 96, NEXTGEO_SW_KEEP_BOTH, total);
	}
	CHECK_EQ(total.mismatches, 0);
	CHECK(total.covered > 1000);

	// Beyond 8192 px a triangle is not rasterized in software; the switch keeps such triangles away.
	const float huge[3][2] = { { 0.0f, 0.0f }, { 9000.0f, 1.0f }, { 1.0f, 30.0f } };
	const float largest[3][2] = { { 0.0f, 0.0f }, { 8192.0f, 1.0f }, { 1.0f, 30.0f } };
	const float z[3] = { 0.5f, 0.5f, 0.5f };
	CHECK_FALSE(nextgeo_sw_setup_triangle(huge, z, 64, 64, NEXTGEO_SW_KEEP_BOTH).valid);
	const NextGeoSwTriangle at_limit = nextgeo_sw_setup_triangle(largest, z, 64, 64, NEXTGEO_SW_KEEP_BOTH);
	CHECK(at_limit.valid);
	CHECK_EQ(at_limit.bits, 1);
	// NaN vertices are rejected before any integer conversion.
	const float nan_px[3][2] = { { NAN, 0.0f }, { 4.0f, 1.0f }, { 1.0f, 3.0f } };
	CHECK_FALSE(nextgeo_sw_setup_triangle(nan_px, z, 64, 64, NEXTGEO_SW_KEEP_BOTH).valid);
}

TEST_CASE("[NextGeo][SwRaster] A watertight mesh covers every pixel exactly once") {
	const int width = 40;
	const int height = 32;
	const int cells = 12;
	const float cell = 4.0f;
	RandomPCG rng(1624);
	// A jittered grid from -4 to past the target, on the half-pixel grid so many edges cross pixel
	// centers exactly, plus fans around shared vertices.
	Vector<Vector2> points;
	for (int j = 0; j <= cells; j++) {
		for (int i = 0; i <= cells; i++) {
			Vector2 p(-4.0f + float(i) * cell, -4.0f + float(j) * cell);
			if (i > 0 && j > 0 && i < cells && j < cells) {
				// At most a quarter cell, so every quad stays convex.
				p.x += Math::round(rng.random(-1.0f, 1.0f) * 2.0f) * 0.5f;
				p.y += Math::round(rng.random(-1.0f, 1.0f) * 2.0f) * 0.5f;
			}
			points.push_back(p);
		}
	}
	Vector<int> coverage;
	coverage.resize(width * height);
	coverage.fill(0);
	int triangles = 0;
	for (int j = 0; j < cells; j++) {
		for (int i = 0; i < cells; i++) {
			const int a = j * (cells + 1) + i;
			const int b = a + 1;
			const int c = a + cells + 1;
			const int d = c + 1;
			// Alternate the diagonal, and the winding, which KEEP_BOTH ignores.
			const int quads[2][3] = { { a, b, ((i + j) % 2 == 0) ? d : c }, { ((i + j) % 2 == 0) ? a : b, d, c } };
			for (int t = 0; t < 2; t++) {
				float px[3][2];
				const float z[3] = { 0.5f, 0.5f, 0.5f };
				for (int v = 0; v < 3; v++) {
					const int index = (t == 1 && (i + j) % 2 == 0) ? quads[t][2 - v] : quads[t][v];
					px[v][0] = points[index].x;
					px[v][1] = points[index].y;
				}
				const NextGeoSwTriangle tri = nextgeo_sw_setup_triangle(px, z, width, height, NEXTGEO_SW_KEEP_BOTH);
				triangles++;
				if (!tri.valid) {
					continue;
				}
				for (int y = tri.min_y; y <= tri.max_y; y++) {
					for (int x = tri.min_x; x <= tri.max_x; x++) {
						float depth = 0.0f;
						if (nextgeo_sw_sample(tri, x, y, depth)) {
							coverage.write[y * width + x]++;
						}
					}
				}
			}
		}
	}
	CHECK_EQ(triangles, cells * cells * 2);
	int holes = 0;
	int overlaps = 0;
	for (int value : coverage) {
		holes += value == 0 ? 1 : 0;
		overlaps += value > 1 ? 1 : 0;
	}
	CHECK_EQ(holes, 0);
	CHECK_EQ(overlaps, 0);

	// A fan of 16 triangles around a vertex on a pixel center.
	coverage.fill(0);
	const Vector2 center(20.5f, 16.5f);
	for (int k = 0; k < 16; k++) {
		const float angle_a = float(k) * Math::TAU / 16.0f;
		const float angle_b = float(k + 1) * Math::TAU / 16.0f;
		const float px[3][2] = {
			{ center.x, center.y },
			{ center.x + Math::round(Math::cos(angle_a) * 24.0f) * 0.5f, center.y + Math::round(Math::sin(angle_a) * 24.0f) * 0.5f },
			{ center.x + Math::round(Math::cos(angle_b) * 24.0f) * 0.5f, center.y + Math::round(Math::sin(angle_b) * 24.0f) * 0.5f },
		};
		const float z[3] = { 0.5f, 0.5f, 0.5f };
		const NextGeoSwTriangle tri = nextgeo_sw_setup_triangle(px, z, width, height, NEXTGEO_SW_KEEP_BOTH);
		if (!tri.valid) {
			continue;
		}
		for (int y = tri.min_y; y <= tri.max_y; y++) {
			for (int x = tri.min_x; x <= tri.max_x; x++) {
				float depth = 0.0f;
				if (nextgeo_sw_sample(tri, x, y, depth)) {
					coverage.write[y * width + x]++;
				}
			}
		}
	}
	int fan_pixels = 0;
	int fan_overlaps = 0;
	for (int value : coverage) {
		fan_pixels += value > 0 ? 1 : 0;
		fan_overlaps += value > 1 ? 1 : 0;
	}
	CHECK_EQ(fan_overlaps, 0);
	CHECK(fan_pixels > 300);
	// The fan's center pixel lies on every edge through it and is still covered exactly once.
	CHECK_EQ(coverage[16 * width + 20], 1);
}

TEST_CASE("[NextGeo][SwRaster] Facing follows the v2.4 winding and culling") {
	// Clockwise in framebuffer space (y down) is the front face of the v2.4 pipeline (cull back,
	// front face clockwise).
	const float front[3][2] = { { 2.0f, 2.0f }, { 12.0f, 2.0f }, { 2.0f, 12.0f } };
	const float back[3][2] = { { 2.0f, 2.0f }, { 2.0f, 12.0f }, { 12.0f, 2.0f } };
	const float z[3] = { 0.2f, 0.4f, 0.6f };
	const float z_back[3] = { 0.2f, 0.6f, 0.4f };
	CHECK(nextgeo_sw_setup_triangle(front, z, 16, 16, NEXTGEO_SW_KEEP_POSITIVE).valid);
	CHECK_FALSE(nextgeo_sw_setup_triangle(back, z_back, 16, 16, NEXTGEO_SW_KEEP_POSITIVE).valid);
	CHECK_FALSE(nextgeo_sw_setup_triangle(front, z, 16, 16, NEXTGEO_SW_KEEP_NEGATIVE).valid);
	CHECK(nextgeo_sw_setup_triangle(back, z_back, 16, 16, NEXTGEO_SW_KEEP_NEGATIVE).valid);

	// With culling off both windings cover the same pixels with the same depth.
	const NextGeoSwTriangle a = nextgeo_sw_setup_triangle(front, z, 16, 16, NEXTGEO_SW_KEEP_BOTH);
	const NextGeoSwTriangle b = nextgeo_sw_setup_triangle(back, z_back, 16, 16, NEXTGEO_SW_KEEP_BOTH);
	REQUIRE(a.valid);
	REQUIRE(b.valid);
	int covered = 0;
	for (int y = 0; y < 16; y++) {
		for (int x = 0; x < 16; x++) {
			float depth_a = 0.0f;
			float depth_b = 0.0f;
			const bool in_a = nextgeo_sw_sample(a, x, y, depth_a);
			const bool in_b = nextgeo_sw_sample(b, x, y, depth_b);
			CHECK_EQ(in_a, in_b);
			if (in_a && in_b) {
				CHECK(depth_a == doctest::Approx(depth_b).epsilon(1e-6));
				covered++;
			}
		}
	}
	// Pixel centers strictly inside the right triangle (2,2) (12,2) (2,12). The 10 centers on its
	// hypotenuse lie on a right edge and are left out.
	CHECK_EQ(covered, 45);

	// Depth is interpolated as the hardware does: linearly in screen space.
	float depth = 0.0f;
	REQUIRE(nextgeo_sw_sample(a, 2, 2, depth));
	CHECK(depth == doctest::Approx(0.2 + 0.2 * 0.05 + 0.4 * 0.05).epsilon(1e-6));

	// Zero area is never covered.
	const float line[3][2] = { { 1.0f, 1.0f }, { 5.0f, 5.0f }, { 9.0f, 9.0f } };
	CHECK_FALSE(nextgeo_sw_setup_triangle(line, z, 16, 16, NEXTGEO_SW_KEEP_BOTH).valid);
	// A triangle entirely outside the target is rejected in setup.
	const float outside[3][2] = { { 20.0f, 1.0f }, { 30.0f, 1.0f }, { 20.0f, 9.0f } };
	CHECK_FALSE(nextgeo_sw_setup_triangle(outside, z, 16, 16, NEXTGEO_SW_KEEP_BOTH).valid);
}

TEST_CASE("[NextGeo][SwRaster] Clip space maps to framebuffer pixels with y pointing down") {
	float px[2];
	float z = 0.0f;
	nextgeo_sw_clip_to_pixel(0.0f, 0.0f, 0.5f, 1.0f, 100, 50, px, z);
	CHECK_EQ(px[0], 50.0f);
	CHECK_EQ(px[1], 25.0f);
	CHECK_EQ(z, 0.5f);
	// NDC (-1, -1) is the first pixel row and column, as the viewport transform maps it.
	nextgeo_sw_clip_to_pixel(-2.0f, -2.0f, 0.4f, 2.0f, 100, 50, px, z);
	CHECK_EQ(px[0], 0.0f);
	CHECK_EQ(px[1], 0.0f);
	CHECK_EQ(z, 0.2f);
}

TEST_CASE("[NextGeo][SwRaster] The raster-feature table and its GLSL copies agree (N3)") {
	CHECK_EQ(uint32_t(RendererVirtualMeshStorage::RASTER_BIN_COUNT), 1u);
	CHECK_EQ(RendererVirtualMeshStorage::find_raster_bin_entry(0u, RendererVirtualMeshStorage::RASTER_PASS_VISIBILITY), 0);
	const int32_t vsm = RendererVirtualMeshStorage::find_raster_bin_entry(0u, RendererVirtualMeshStorage::RASTER_PASS_VSM);
	REQUIRE(vsm >= 0);
	CHECK_EQ(uint32_t(RendererVirtualMeshStorage::RASTER_BIN_TABLE[vsm].bin), uint32_t(RendererVirtualMeshStorage::RASTER_BIN_FIXED));
	CHECK_EQ(String(RendererVirtualMeshStorage::RASTER_BIN_TABLE[vsm].name), String("VSM_FIXED"));
	// No bin rasterizes a geometry feature yet (#155 adds them).
	CHECK_EQ(RendererVirtualMeshStorage::find_raster_bin_entry(RendererVirtualMeshStorage::RASTER_FEATURE_MASKED, RendererVirtualMeshStorage::RASTER_PASS_VISIBILITY), -1);

	struct Feature {
		const char *name;
		uint32_t bit;
	};
	const Feature features[] = {
		{ "MASKED", RendererVirtualMeshStorage::RASTER_FEATURE_MASKED },
		{ "VERTEX_OFFSET", RendererVirtualMeshStorage::RASTER_FEATURE_VERTEX_OFFSET },
		{ "DEPTH_OFFSET", RendererVirtualMeshStorage::RASTER_FEATURE_DEPTH_OFFSET },
		{ "TWO_SIDED", RendererVirtualMeshStorage::RASTER_FEATURE_TWO_SIDED },
		{ "SKINNED", RendererVirtualMeshStorage::RASTER_FEATURE_SKINNED },
		{ "DISPLACED", RendererVirtualMeshStorage::RASTER_FEATURE_DISPLACED },
		{ "SPLINE", RendererVirtualMeshStorage::RASTER_FEATURE_SPLINE },
		{ "VOXEL", RendererVirtualMeshStorage::RASTER_FEATURE_VOXEL },
		{ "ASSEMBLY", RendererVirtualMeshStorage::RASTER_FEATURE_ASSEMBLY },
	};
	uint32_t all = 0;
	for (const Feature &feature : features) {
		const String value = glsl_constant(raster_features_inc_shader_glsl, String("NEXTGEO_RASTER_FEATURE_") + feature.name);
		CHECK_MESSAGE(!value.is_empty(), feature.name);
		CHECK_EQ(glsl_integer(value), int64_t(feature.bit));
		CHECK_EQ(all & feature.bit, 0u);
		all |= feature.bit;
	}
	CHECK_EQ(all, uint32_t(RendererVirtualMeshStorage::RASTER_FEATURE_ALL));
	CHECK_EQ(glsl_integer(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_PASS_VISIBILITY")), int64_t(RendererVirtualMeshStorage::RASTER_PASS_VISIBILITY));
	CHECK_EQ(glsl_integer(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_PASS_VSM")), int64_t(RendererVirtualMeshStorage::RASTER_PASS_VSM));
	CHECK_EQ(glsl_integer(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_BIN_FIXED")), int64_t(RendererVirtualMeshStorage::RASTER_BIN_FIXED));
	CHECK_EQ(glsl_integer(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_BIN_COUNT")), int64_t(RendererVirtualMeshStorage::RASTER_BIN_COUNT));

	// The software setup's GLSL copy uses the same grid and limits.
	CHECK_EQ(glsl_integer(glsl_constant(sw_raster_setup_inc_shader_glsl, "NEXTGEO_SW_SUBPIXEL_BITS")), int64_t(NEXTGEO_SW_SUBPIXEL_BITS));
	CHECK_EQ(glsl_integer(glsl_constant(sw_raster_setup_inc_shader_glsl, "NEXTGEO_SW_FULL_PRECISION_EXTENT_PX")), int64_t(NEXTGEO_SW_FULL_PRECISION_EXTENT_PX));
	CHECK_EQ(glsl_integer(glsl_constant(sw_raster_setup_inc_shader_glsl, "NEXTGEO_SW_KEEP_POSITIVE")), int64_t(NEXTGEO_SW_KEEP_POSITIVE));
	CHECK_EQ(glsl_integer(glsl_constant(sw_raster_setup_inc_shader_glsl, "NEXTGEO_SW_KEEP_BOTH")), int64_t(NEXTGEO_SW_KEEP_BOTH));
	CHECK_EQ(glsl_constant(sw_raster_setup_inc_shader_glsl, "NEXTGEO_SW_KEEP_NEGATIVE"), String("-1"));
}

TEST_CASE("[NextGeo][SwRaster] max_edge is a binary16 rounded up") {
	CHECK_EQ(ngm_half_ceil(0.0), 0u);
	CHECK_EQ(ngm_half_ceil(-3.0), 0u);
	CHECK_EQ(ngm_half_ceil(1.0), 0x3C00u);
	CHECK_EQ(ngm_half_ceil(2.0), 0x4000u);
	// Just above 1 rounds up to the next binary16, 1 + 2^-10.
	CHECK_EQ(ngm_half_ceil(1.0000001), 0x3C01u);
	CHECK_EQ(ngm_half_ceil(1.9999), 0x4000u);
	CHECK_EQ(ngm_half_ceil(65504.0), 0x7BFFu);
	CHECK_EQ(ngm_half_ceil(65504.5), 0x7C00u);
	CHECK_EQ(ngm_half_ceil(NAN), 0x7C00u);
	CHECK_EQ(ngm_half_ceil(INFINITY), 0x7C00u);
	// Subnormals, and the smallest normal value.
	CHECK_EQ(ngm_half_ceil(std::ldexp(1.0, -24)), 0x0001u);
	CHECK_EQ(ngm_half_ceil(std::ldexp(1.0, -26)), 0x0001u);
	CHECK_EQ(ngm_half_ceil(std::ldexp(1.0, -14)), 0x0400u);
	CHECK_EQ(ngm_half_ceil(std::ldexp(1023.5, -24)), 0x0400u);

	// The stored value never understates an edge and is the closest binary16 that does not.
	RandomPCG rng(1625);
	for (int i = 0; i < 20000; i++) {
		const double value = std::ldexp(double(rng.random(1.0f, 2.0f)), int(rng.rand(40)) - 24);
		const uint16_t half = ngm_half_ceil(value);
		CHECK(double(ngm_half_to_float(half)) >= value);
		if (half > 0u && half < 0x7C00u) {
			CHECK(double(ngm_half_to_float(uint16_t(half - 1u))) < value);
		}
	}
	CHECK(std::isinf(ngm_half_to_float(0x7C00u)));
	CHECK_EQ(ngm_half_to_float(0x3C00u), 1.0f);
}

TEST_CASE("[NextGeo][SwRaster] A v1 page gets each cluster's tagged max_edge at install") {
	Vector<uint8_t> page = make_v1_page();
	CHECK_FALSE(ngm_page_has_max_edges(page.ptr(), uint64_t(page.size())));
	CHECK_EQ(ngm_cluster_max_edge(page.ptr() + 16 + 160, 3, 3, page.ptr() + 16 + 160 + 72, 1), 5.0);
	CHECK_EQ(ngm_cluster_max_edge(page.ptr() + 16 + 160, 3, 3, page.ptr() + 16 + 160 + 72, 0), 0.0);

	Vector<uint8_t> filled = page;
	REQUIRE(ngm_page_fill_max_edges(filled.ptrw(), uint64_t(filled.size()), 3));
	CHECK(ngm_page_has_max_edges(filled.ptr(), uint64_t(filled.size())));
	CHECK_EQ(load_u32(filled.ptr() + 16 + 64), ngm_max_edge_word(5.0));
	CHECK_EQ(load_u32(filled.ptr() + 16 + 80 + 64), ngm_max_edge_word(std::sqrt(5.0)));
	CHECK_EQ(load_u32(filled.ptr() + 16 + 64) >> 16, uint32_t(NGM_MAX_EDGE_TAG));
	CHECK_EQ(ngm_half_to_float(uint16_t(load_u32(filled.ptr() + 16 + 64) & 0xFFFFu)), 5.0f);
	// Every other byte is unchanged.
	for (int i = 0; i < page.size(); i++) {
		const bool max_edge_byte = (i >= 16 + 64 && i < 16 + 68) || (i >= 16 + 80 + 64 && i < 16 + 80 + 68);
		if (!max_edge_byte) {
			CHECK_EQ(filled[i], page[i]);
		}
	}

	// A cluster whose ranges leave the page is unbounded, so it stays on the hardware rasterizer.
	Vector<uint8_t> broken = page;
	store_u32(broken.ptrw() + 16 + 80 + 68, 5);
	REQUIRE(ngm_page_fill_max_edges(broken.ptrw(), uint64_t(broken.size()), 3));
	CHECK_EQ(load_u32(broken.ptr() + 16 + 80 + 64), ngm_max_edge_word(INFINITY));
	// A payload too short for its own header is left alone.
	Vector<uint8_t> truncated = page;
	truncated.resize(16 + 160 + 10);
	const Vector<uint8_t> truncated_before = truncated;
	CHECK_FALSE(ngm_page_fill_max_edges(truncated.ptrw(), uint64_t(truncated.size()), 3));
	CHECK(truncated == truncated_before);

	// The runtime: a v1 root page is uploaded with the field filled in; a v2 page byte for byte.
	TestSwRasterVirtualMeshStorage storage;
	const RID v1_mesh = RID::from_uint64(16201);
	RenderingServerTypes::VirtualGeometryDesc v1_desc;
	v1_desc.eligible_surface_mask = 0x1u;
	v1_desc.vertex_layout = 1u;
	v1_desc.root_pages.push_back(page);
	storage.mesh_set_virtual_geometry(v1_mesh, v1_desc);
	REQUIRE(storage.pages.size() >= filled.size());
	CHECK(memcmp(storage.pages.ptr(), filled.ptr(), filled.size()) == 0);
	storage.mesh_clear_virtual_geometry(v1_mesh, 0);

	TestSwRasterVirtualMeshStorage v2_storage;
	const RID v2_mesh = RID::from_uint64(16202);
	Vector<uint8_t> v2_page = filled;
	// A v2 page's stored value is used as is, even when it differs from what the vertices give.
	store_u32(v2_page.ptrw() + 16 + 64, ngm_max_edge_word(0.25));
	RenderingServerTypes::VirtualGeometryDesc v2_desc;
	v2_desc.eligible_surface_mask = 0x1u;
	v2_desc.vertex_layout = 1u;
	v2_desc.root_pages.push_back(v2_page);
	v2_storage.mesh_set_virtual_geometry(v2_mesh, v2_desc);
	REQUIRE(v2_storage.pages.size() >= v2_page.size());
	CHECK(memcmp(v2_storage.pages.ptr(), v2_page.ptr(), v2_page.size()) == 0);
	v2_storage.mesh_clear_virtual_geometry(v2_mesh, 0);
}

} // namespace TestNextGeoSwRaster
