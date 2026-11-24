/**************************************************************************/
/*  test_nextgeo_voxels.cpp                                               */
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

TEST_FORCE_LINK(test_nextgeo_voxels)

// GodotNext: nextgeo_voxels (#165, plan §21.4 contract N10, GN-7, GN-12, N6, MN3).
// Verifies the reserved 25-bit VIS_VOXEL_BASE ID partition, sparse 4^3 brick packing/unpacking,
// oct16 normal and fp16x2 UV encoding, frame-stable stochastic coverage hash, .ngm trailing voxel
// section validation and backward compatibility, ProjectSettings default, and VSM raster bin entries.

#include "core/config/project_settings.h"
#include "core/io/resource_format_ngm.h"
#include "core/math/math_defs.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_visibility.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_voxels.h"
#include "servers/rendering/storage/virtual_mesh_storage.h"

#include <cmath>
#include <cstring>

using namespace RendererRD;

namespace TestNextGeoVoxels {

static void write_u32_le(uint8_t *p_dst, uint32_t p_val) {
	p_dst[0] = uint8_t(p_val & 0xFFu);
	p_dst[1] = uint8_t((p_val >> 8u) & 0xFFu);
	p_dst[2] = uint8_t((p_val >> 16u) & 0xFFu);
	p_dst[3] = uint8_t((p_val >> 24u) & 0xFFu);
}

TEST_CASE("[NextGeo][Voxels] Reserved 25-bit VIS_VOXEL_BASE ID encoding and disjointness (#165)") {
	CHECK_EQ(RendererVirtualMeshStorage::VIS_VOXEL_BASE, NEXTGEO_VIS_VOXEL_BASE);
	CHECK_EQ(RendererVirtualMeshStorage::VIS_VOXEL_MASK, NEXTGEO_VIS_VOXEL_MASK);
	CHECK_EQ(NEXTGEO_VIS_VOXEL_BASE, 0x01C00000u);
	CHECK_EQ(NEXTGEO_VIS_VOXEL_INDEX_MASK, 0x003FFFFFu);
	CHECK_EQ(NEXTGEO_VIS_VOXEL_MAX_COUNT, 0x003FFFFFu);
	CHECK_LT(NEXTGEO_VOXEL_BRICK_CAPACITY, NEXTGEO_VIS_VOXEL_MAX_COUNT);

	const uint32_t brick_samples[] = {
		0u,
		1u,
		42u,
		65535u,
		NEXTGEO_VOXEL_BRICK_CAPACITY - 1u,
		NEXTGEO_VIS_VOXEL_MAX_COUNT - 1u,
	};
	const uint32_t voxel_samples[] = { 0u, 1u, 15u, 31u, 32u, 62u, 63u };
	for (uint32_t brick_idx : brick_samples) {
		for (uint32_t voxel_idx : voxel_samples) {
			const uint32_t vis_id = nextgeo_pack_voxel_visibility_id(brick_idx, voxel_idx);
			CHECK_NE(vis_id, NEXTGEO_VISIBILITY_BACKGROUND);
			const uint32_t vis_cluster = nextgeo_unpack_visibility_cluster(vis_id);
			const uint32_t tri_slot = nextgeo_unpack_visibility_triangle(vis_id);
			CHECK(nextgeo_visibility_cluster_is_voxel(vis_cluster));
			CHECK_FALSE(nextgeo_visibility_cluster_is_normal(vis_cluster));
			CHECK_FALSE(nextgeo_visibility_cluster_is_patch(vis_cluster));
			CHECK_EQ(nextgeo_unpack_voxel_brick_index(vis_cluster), brick_idx);
			CHECK_EQ(nextgeo_unpack_voxel_index(vis_id), voxel_idx);
			CHECK_EQ(tri_slot, voxel_idx);
		}
	}
}

TEST_CASE("[NextGeo][Voxels] Brick packing, oct16 normal, fp16 UV, occupancy rank, and coverage hash (#165)") {
	// Sparse occupancy mask rank, popcount, and is_occupied checks.
	const uint32_t occ_lo = 0x80010005u; // bits 0, 2, 16, 31 (4 bits)
	const uint32_t occ_hi = 0x80000003u; // bits 32, 33, 63 (3 bits)
	CHECK_EQ(nextgeo_voxel_popcount32(occ_lo) + nextgeo_voxel_popcount32(occ_hi), 7u);
	CHECK(nextgeo_voxel_is_occupied(occ_lo, occ_hi, 0u));
	CHECK_FALSE(nextgeo_voxel_is_occupied(occ_lo, occ_hi, 1u));
	CHECK(nextgeo_voxel_is_occupied(occ_lo, occ_hi, 63u));
	CHECK_FALSE(nextgeo_voxel_is_occupied(occ_lo, occ_hi, 64u));
	CHECK_EQ(nextgeo_voxel_rank(occ_lo, occ_hi, 0u), 0u);
	CHECK_EQ(nextgeo_voxel_rank(occ_lo, occ_hi, 2u), 1u);
	CHECK_EQ(nextgeo_voxel_rank(occ_lo, occ_hi, 16u), 2u);
	CHECK_EQ(nextgeo_voxel_rank(occ_lo, occ_hi, 31u), 3u);
	CHECK_EQ(nextgeo_voxel_rank(occ_lo, occ_hi, 32u), 4u);
	CHECK_EQ(nextgeo_voxel_rank(occ_lo, occ_hi, 33u), 5u);
	CHECK_EQ(nextgeo_voxel_rank(occ_lo, occ_hi, 63u), 6u);

	// Brick metadata and voxel word1 round-trip.
	const uint32_t meta = nextgeo_voxel_pack_brick_meta(37u, 5u, true);
	CHECK_EQ(meta & 0xFFu, 37u);
	CHECK_EQ((meta >> 8u) & 0xFFFFu, 5u);
	CHECK((meta & NEXTGEO_VOXEL_BRICK_FLAG_UNIFORM_SURFACE) != 0u);

	const uint16_t oct = nextgeo_voxel_encode_oct16(Vector3(0.0f, 1.0f, 0.0f));
	const uint32_t w1 = nextgeo_voxel_pack_word1(oct, 187u, 3u);
	CHECK_EQ(nextgeo_voxel_unpack_oct16(w1), oct);
	CHECK_EQ(nextgeo_voxel_unpack_coverage(w1), uint8_t(187u));
	CHECK_EQ(nextgeo_voxel_unpack_surface_id(w1), uint8_t(3u));

	// Oct16 normal encode/decode accuracy across cardinals, diagonals, and Fibonacci sphere.
	const Vector3 cardinal_normals[] = {
		Vector3(1, 0, 0),
		Vector3(-1, 0, 0),
		Vector3(0, 1, 0),
		Vector3(0, -1, 0),
		Vector3(0, 0, 1),
		Vector3(0, 0, -1),
		Vector3(1, 1, 1).normalized(),
		Vector3(-1, -1, -1).normalized(),
		Vector3(0.3f, -0.7f, 0.65f).normalized(),
	};
	for (const Vector3 &n : cardinal_normals) {
		const uint16_t packed = nextgeo_voxel_encode_oct16(n);
		const Vector3 decoded = nextgeo_voxel_decode_oct16(packed);
		CHECK_EQ(double(decoded.length()), doctest::Approx(1.0).epsilon(1e-4));
		CHECK_GT(decoded.dot(n), 0.995f);
	}
	const int fib_n = 128;
	const double golden = Math::PI * (3.0 - std::sqrt(5.0));
	for (int i = 0; i < fib_n; i++) {
		const double y = 1.0 - (2.0 * (i + 0.5)) / double(fib_n);
		const double r = std::sqrt(MAX(0.0, 1.0 - y * y));
		const double theta = golden * double(i);
		const Vector3 n(float(std::cos(theta) * r), float(y), float(std::sin(theta) * r));
		const Vector3 decoded = nextgeo_voxel_decode_oct16(nextgeo_voxel_encode_oct16(n));
		CHECK_GT(decoded.dot(n), 0.995f);
	}

	// Half2 UV packing round-trip.
	const Vector2 uv_samples[] = {
		Vector2(0.0f, 0.0f),
		Vector2(0.5f, 0.25f),
		Vector2(1.0f, 1.0f),
		Vector2(-0.375f, 2.75f),
		Vector2(0.1234f, 0.8765f),
	};
	for (const Vector2 &uv : uv_samples) {
		const uint32_t packed = nextgeo_voxel_pack_uv_half2(uv);
		const Vector2 decoded = nextgeo_voxel_unpack_uv_half2(packed);
		CHECK_EQ(double(decoded.x), doctest::Approx(double(uv.x)).epsilon(2e-3));
		CHECK_EQ(double(decoded.y), doctest::Approx(double(uv.y)).epsilon(2e-3));
	}

	// Frame-stable stochastic coverage hash: deterministic, exact 0/255 endpoints, calibrated mean.
	for (int32_t y = 0; y < 16; y++) {
		for (int32_t x = 0; x < 16; x++) {
			CHECK_FALSE(nextgeo_voxel_coverage_pass(x, y, 7u, 128u, 19u, 0u));
			CHECK(nextgeo_voxel_coverage_pass(x, y, 7u, 128u, 19u, 255u));
			const bool first = nextgeo_voxel_coverage_pass(x, y, 7u, 128u, 19u, 128u);
			const bool repeat = nextgeo_voxel_coverage_pass(x, y, 7u, 128u, 19u, 128u);
			CHECK_EQ(first, repeat);
		}
	}
	for (uint32_t cov : { 32u, 64u, 128u, 192u, 224u }) {
		uint32_t passed = 0u;
		const uint32_t total = 64u * 64u;
		for (int32_t y = 0; y < 64; y++) {
			for (int32_t x = 0; x < 64; x++) {
				if (nextgeo_voxel_coverage_pass(x, y, 3u, 512u, 11u, cov)) {
					passed++;
				}
			}
		}
		const double rate = double(passed) / double(total);
		const double expected = double(cov) / 255.0;
		CHECK_LE(std::abs(rate - expected), 0.025);
	}

	// Projected brick size helper (perspective & ortho).
	CHECK_EQ(double(nextgeo_voxel_projected_brick_px(0.25f, 1.0f, 0.0f, 50.0f, 0.05f, 100.0f, false)), doctest::Approx(2.0).epsilon(1e-5));
	CHECK_EQ(double(nextgeo_voxel_projected_brick_px(0.25f, 2.0f, 0.0f, 1.0f, 0.05f, 10.0f, true)), doctest::Approx(20.0).epsilon(1e-5));
}

TEST_CASE("[NextGeo][Voxels] .ngm trailing voxel section parsing and backward compatibility (#165)") {
	CHECK((NGM_KNOWN_FLAGS & NGM_FLAG_VOXELS) != 0u);
	CHECK_EQ(NGM_VOXEL_SECTION_MAGIC, NEXTGEO_VOXEL_SECTION_MAGIC);

	// Plain legacy page (no trailing voxel section): returns false and preserves full size.
	PackedByteArray plain_page;
	plain_page.resize(128);
	plain_page.fill(0);
	write_u32_le(plain_page.ptrw() + 0, 1u); // cluster_count
	write_u32_le(plain_page.ptrw() + 4, 16u); // vertex_bytes
	write_u32_le(plain_page.ptrw() + 8, 16u); // index_bytes
	uint32_t vox_off = 999u;
	uint32_t vox_bytes = 999u;
	CHECK_FALSE(ngm_page_voxel_section(plain_page.ptr(), plain_page.size(), vox_off, vox_bytes));
	CHECK_EQ(vox_off, 0u);
	CHECK_EQ(vox_bytes, 0u);

	// Page with a valid self-describing trailing voxel section (header + 1 group dir + 1 brick w/ 1 occupied voxel + trailer).
	PackedByteArray voxel_page = plain_page;
	const uint32_t sec_off = uint32_t(voxel_page.size());
	const uint32_t brick_bytes = NEXTGEO_VOXEL_BRICK_HEADER_WORDS * 4u + 1u * NEXTGEO_VOXEL_DATA_WORDS * 4u; // 40 bytes
	const uint32_t sec_bytes = 16u + 16u + brick_bytes + 8u;
	voxel_page.resize(sec_off + sec_bytes);
	memset(voxel_page.ptrw() + sec_off, 0, sec_bytes);
	// Header: magic, first_group=0, group_count=1, total_bricks=1.
	write_u32_le(voxel_page.ptrw() + sec_off + 0u, NGM_VOXEL_SECTION_MAGIC);
	write_u32_le(voxel_page.ptrw() + sec_off + 4u, 0u);
	write_u32_le(voxel_page.ptrw() + sec_off + 8u, 1u);
	write_u32_le(voxel_page.ptrw() + sec_off + 12u, 1u);
	// Group 0 directory: brick_rel_word = (16 + 16) / 4 = 8, brick_count = 1.
	write_u32_le(voxel_page.ptrw() + sec_off + 16u + 0u, 8u);
	write_u32_le(voxel_page.ptrw() + sec_off + 16u + 4u, 1u);
	// Brick 0 header: occ_lo = 1, meta = pack(1, 0, true).
	write_u32_le(voxel_page.ptrw() + sec_off + 32u + 16u, 1u);
	write_u32_le(voxel_page.ptrw() + sec_off + 32u + 24u, nextgeo_voxel_pack_brick_meta(1u, 0u, true));
	// Trailer: total_bytes, magic.
	write_u32_le(voxel_page.ptrw() + sec_off + sec_bytes - 8u, sec_bytes);
	write_u32_le(voxel_page.ptrw() + sec_off + sec_bytes - 4u, NGM_VOXEL_SECTION_MAGIC);

	CHECK(ngm_page_voxel_section(voxel_page.ptr(), voxel_page.size(), vox_off, vox_bytes));
	CHECK_EQ(vox_off, sec_off);
	CHECK_EQ(vox_bytes, sec_bytes);

	// Corrupted header magic must be rejected cleanly.
	PackedByteArray bad_magic = voxel_page;
	write_u32_le(bad_magic.ptrw() + sec_off + 0u, 0x12345678u);
	CHECK_FALSE(ngm_page_voxel_section(bad_magic.ptr(), bad_magic.size(), vox_off, vox_bytes));

	// Out-of-bounds section byte length must be rejected cleanly.
	PackedByteArray bad_len = voxel_page;
	write_u32_le(bad_len.ptrw() + sec_off + sec_bytes - 8u, uint32_t(bad_len.size()) + 16u);
	CHECK_FALSE(ngm_page_voxel_section(bad_len.ptr(), bad_len.size(), vox_off, vox_bytes));
}

TEST_CASE("[NextGeo][Voxels] Experimental setting default off and VSM raster bin table (#165)") {
	CHECK_FALSE(NEXTGEO_VOXEL_DEFAULT_ENABLED);
	if (ProjectSettings::get_singleton() != nullptr && ProjectSettings::get_singleton()->has_setting("rendering/nextgeo/experimental/voxels")) {
		CHECK_FALSE(bool(ProjectSettings::get_singleton()->get_setting("rendering/nextgeo/experimental/voxels")));
	}

	bool found_voxel = false;
	bool found_vsm_voxel = false;
	for (const RendererVirtualMeshStorage::RasterBinEntry &entry : RendererVirtualMeshStorage::RASTER_BIN_TABLE) {
		if (entry.features == RendererVirtualMeshStorage::RASTER_FEATURE_VOXEL) {
			if (entry.pass == RendererVirtualMeshStorage::RASTER_PASS_VISIBILITY && strcmp(entry.name, "VOXEL") == 0) {
				found_voxel = true;
				CHECK_EQ(entry.bin, RendererVirtualMeshStorage::RASTER_BIN_FIXED);
			} else if (entry.pass == RendererVirtualMeshStorage::RASTER_PASS_VSM && strcmp(entry.name, "VSM_VOXEL") == 0) {
				found_vsm_voxel = true;
				CHECK_EQ(entry.bin, RendererVirtualMeshStorage::RASTER_BIN_FIXED);
			}
		}
	}
	CHECK(found_voxel);
	CHECK(found_vsm_voxel);
}

} // namespace TestNextGeoVoxels
