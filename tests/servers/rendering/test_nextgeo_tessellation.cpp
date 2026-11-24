/**************************************************************************/
/*  test_nextgeo_tessellation.cpp                                         */
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

TEST_FORCE_LINK(test_nextgeo_tessellation)

// GodotNext: nextgeo_tessellation (#163, plan §21.4 contract N11, GN-6, GN-12, N5).
// Covers reserved 25-bit visibility ID partitioning (#163 vs #165), canonicalized crack-free edge
// factor and edge-vertex interpolation symmetry, zero-degenerate inner-grid + outer-zipper
// triangulation, <= 128 micro-triangle sub-patch splitting, overflow coarsening and stats,
// ProjectSettings defaults and VSM chain toggles, and #150 dicing-compute SPIR-V compilation.

#include "core/config/project_settings.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/templates/local_vector.h"
#include "scene/resources/material.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_tessellation.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_visibility.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/tessellation_dice.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/tessellation_hw.glsl.gen.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_shader_container.h"
#include "servers/rendering/shader_compiler.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/storage/virtual_mesh_storage.h"
#include "servers/rendering/storage/virtual_shadow_map_storage.h"

#include "modules/modules_enabled.gen.h"

#ifdef MODULE_GLSLANG_ENABLED
#include "modules/glslang/shader_compile.h"
#endif
#ifdef VULKAN_ENABLED
#include "drivers/vulkan/rendering_shader_container_vulkan.h"
#endif

#include <cmath>
#include <cstring>

using namespace RendererRD;
using RDC = RenderingDeviceCommons;

namespace TestNextGeoTessellation {

static bool bit_equal_vec3(const Vector3 &p_a, const Vector3 &p_b) {
	return memcmp(&p_a.x, &p_b.x, sizeof(real_t)) == 0 &&
			memcmp(&p_a.y, &p_b.y, sizeof(real_t)) == 0 &&
			memcmp(&p_a.z, &p_b.z, sizeof(real_t)) == 0;
}

static bool bit_equal_vec2(const Vector2 &p_a, const Vector2 &p_b) {
	return memcmp(&p_a.x, &p_b.x, sizeof(real_t)) == 0 &&
			memcmp(&p_a.y, &p_b.y, sizeof(real_t)) == 0;
}

static void store_xform_3x4(const Transform3D &p_xform, float r_rows[3][4]) {
	for (int r = 0; r < 3; r++) {
		r_rows[r][0] = float(p_xform.basis.rows[r][0]);
		r_rows[r][1] = float(p_xform.basis.rows[r][1]);
		r_rows[r][2] = float(p_xform.basis.rows[r][2]);
		r_rows[r][3] = float(p_xform.origin[r]);
	}
}

static float bary_signed_area_2x(const NextGeoTessVertexCoord &p_a, const NextGeoTessVertexCoord &p_b, const NextGeoTessVertexCoord &p_c) {
	// Project onto (u, v) plane where w = 1 - u - v; 2 * signed area = (b.u - a.u)*(c.v - a.v) - (b.v - a.v)*(c.u - a.u).
	// Note: V0=(1,0,0), V1=(0,1,0), V2=(0,0,1) has det = (0-1)*(0-0) - (1-0)*(0-1) = +1.0!
	return (p_b.u - p_a.u) * (p_c.v - p_a.v) - (p_b.v - p_a.v) * (p_c.u - p_a.u);
}

TEST_CASE("[NextGeo][Tessellation] Reserved 25-bit visibility cluster ID partition (#163 vs #165)") {
	// Verify shared constants match across virtual_mesh_storage.h and nextgeo_visibility.h.
	CHECK_EQ(RendererVirtualMeshStorage::VIS_NORMAL_MAX, NEXTGEO_VIS_NORMAL_MAX);
	CHECK_EQ(RendererVirtualMeshStorage::VIS_PATCH_BASE, NEXTGEO_VIS_PATCH_BASE);
	CHECK_EQ(RendererVirtualMeshStorage::VIS_PATCH_MASK, NEXTGEO_VIS_PATCH_MASK);
	CHECK_EQ(RendererVirtualMeshStorage::VIS_PATCH_INDEX_MASK, NEXTGEO_VIS_PATCH_INDEX_MASK);
	CHECK_EQ(RendererVirtualMeshStorage::VIS_VOXEL_BASE, NEXTGEO_VIS_VOXEL_BASE);
	CHECK_EQ(RendererVirtualMeshStorage::VIS_VOXEL_MASK, NEXTGEO_VIS_VOXEL_MASK);

	CHECK_EQ(NEXTGEO_VIS_NORMAL_MAX, 0x017FFFFFu);
	CHECK_EQ(NEXTGEO_VIS_PATCH_BASE, 0x01800000u);
	CHECK_EQ(NEXTGEO_VIS_PATCH_MASK, 0x01C00000u);
	CHECK_EQ(NEXTGEO_VIS_VOXEL_BASE, 0x01C00000u);
	CHECK_EQ(NEXTGEO_VIS_VOXEL_MASK, 0x01C00000u);

	// Boundary checks across the entire 25-bit space [0 .. 0x01FFFFFF].
	const uint32_t samples[] = {
		0u,
		1u,
		0x00FFFFFFu,
		0x017FFFFFu,
		0x01800000u,
		0x01800001u,
		0x01BFFFFFu,
		0x01C00000u,
		0x01C00001u,
		0x01FFFFFEu,
		0x01FFFFFFu,
	};
	for (uint32_t vc : samples) {
		const bool is_norm = nextgeo_visibility_cluster_is_normal(vc);
		const bool is_patch = nextgeo_visibility_cluster_is_patch(vc);
		const bool is_voxel = nextgeo_visibility_cluster_is_voxel(vc);
		// Every 25-bit cluster ID belongs to exactly one partition.
		CHECK_EQ((is_norm ? 1 : 0) + (is_patch ? 1 : 0) + (is_voxel ? 1 : 0), 1);
	}

	CHECK(nextgeo_visibility_cluster_is_normal(0u));
	CHECK(nextgeo_visibility_cluster_is_normal(0x017FFFFFu));
	CHECK(nextgeo_visibility_cluster_is_patch(0x01800000u));
	CHECK(nextgeo_visibility_cluster_is_patch(0x01BFFFFFu));
	CHECK(nextgeo_visibility_cluster_is_voxel(0x01C00000u));
	CHECK(nextgeo_visibility_cluster_is_voxel(0x01FFFFFFu));

	// Patch visibility ID packing / unpacking round-trip and disjointness from background sentinel.
	for (uint32_t patch_idx : { 0u, 1u, 17u, 65535u, NEXTGEO_TESS_PATCH_CAPACITY - 1u, NEXTGEO_VIS_PATCH_INDEX_MASK }) {
		for (uint32_t micro_tri : { 0u, 1u, 63u, 127u }) {
			const uint32_t vis_id = nextgeo_pack_patch_visibility_id(patch_idx, micro_tri);
			CHECK_NE(vis_id, NEXTGEO_VISIBILITY_BACKGROUND);
			const uint32_t vis_cluster = nextgeo_unpack_visibility_cluster(vis_id);
			const uint32_t tri = nextgeo_unpack_visibility_triangle(vis_id);
			CHECK(nextgeo_visibility_cluster_is_patch(vis_cluster));
			CHECK_FALSE(nextgeo_visibility_cluster_is_normal(vis_cluster));
			CHECK_FALSE(nextgeo_visibility_cluster_is_voxel(vis_cluster));
			CHECK_EQ(nextgeo_unpack_patch_index(vis_cluster), patch_idx);
			CHECK_EQ(tri, micro_tri);
		}
	}
	// Background sentinel 0xFFFFFFFF has cluster 0x01FFFFFF (voxel range), never collides with normal or patch.
	CHECK_FALSE(nextgeo_visibility_cluster_is_normal(nextgeo_unpack_visibility_cluster(NEXTGEO_VISIBILITY_BACKGROUND)));
	CHECK_FALSE(nextgeo_visibility_cluster_is_patch(nextgeo_unpack_visibility_cluster(NEXTGEO_VISIBILITY_BACKGROUND)));
}

TEST_CASE("[NextGeo][Tessellation] Canonicalized crack-free edge factors and edge vertex symmetry") {
	Transform3D xform;
	xform.basis = Basis::from_euler(Vector3(0.35f, -0.7f, 0.2f)).scaled(Vector3(1.5f, 2.25f, 0.8f));
	xform.origin = Vector3(3.2f, -1.7f, -4.5f);
	float xform_rows[3][4];
	store_xform_3x4(xform, xform_rows);
	const Vector3 cam_pos(0.4f, 1.8f, 5.2f);
	const float px_scale = 920.0f;
	const float z_near = 0.05f;

	const Vector3 pts[] = {
		Vector3(-1.25f, 0.0f, -0.75f),
		Vector3(0.83f, 0.42f, 0.19f),
		Vector3(0.83f, -0.61f, 1.37f),
		Vector3(-0.31f, 1.11f, -0.75f),
		Vector3(0.0f, 0.0f, 0.0f),
		Vector3(2.5f, 2.5f, 2.5f),
	};

	for (size_t i = 0; i < sizeof(pts) / sizeof(pts[0]); i++) {
		for (size_t j = 0; j < sizeof(pts) / sizeof(pts[0]); j++) {
			for (float rate : { 0.5f, 1.0f, 2.0f, 4.5f, 16.0f }) {
				// Perspective main view: flipping endpoint order must be bit-identical.
				const uint32_t f_ab = nextgeo_tess_edge_factor(pts[i], pts[j], xform_rows, cam_pos, px_scale, z_near, false, rate);
				const uint32_t f_ba = nextgeo_tess_edge_factor(pts[j], pts[i], xform_rows, cam_pos, px_scale, z_near, false, rate);
				CHECK_EQ(f_ab, f_ba);
				CHECK_GE(f_ab, 1u);
				CHECK_LE(f_ab, NEXTGEO_TESS_MAX_EDGE_FACTOR);

				// Orthographic main view: symmetric.
				const uint32_t fo_ab = nextgeo_tess_edge_factor(pts[i], pts[j], xform_rows, cam_pos, 180.0f, z_near, true, rate);
				const uint32_t fo_ba = nextgeo_tess_edge_factor(pts[j], pts[i], xform_rows, cam_pos, 180.0f, z_near, true, rate);
				CHECK_EQ(fo_ab, fo_ba);
				CHECK_GE(fo_ab, 1u);
				CHECK_LE(fo_ab, NEXTGEO_TESS_MAX_EDGE_FACTOR);

				// VSM directional (ortho) & local (perspective): symmetric.
				const uint32_t vd_ab = nextgeo_tess_vsm_edge_factor(pts[i], pts[j], xform_rows, cam_pos, 0.02f, 0.05f, true, rate);
				const uint32_t vd_ba = nextgeo_tess_vsm_edge_factor(pts[j], pts[i], xform_rows, cam_pos, 0.02f, 0.05f, true, rate);
				CHECK_EQ(vd_ab, vd_ba);

				const uint32_t vl_ab = nextgeo_tess_vsm_edge_factor(pts[i], pts[j], xform_rows, cam_pos, 0.004f, 0.1f, false, rate);
				const uint32_t vl_ba = nextgeo_tess_vsm_edge_factor(pts[j], pts[i], xform_rows, cam_pos, 0.004f, 0.1f, false, rate);
				CHECK_EQ(vl_ab, vl_ba);
			}
		}
	}

	// Edge-only canonical interpolation symmetry across two neighboring triangles sharing edge (A, B)
	// in opposite winding orientations (Tri1: A, B, C1 vs Tri2: B, A, C2 with unrelated C1 != C2).
	const Vector3 pos_a(-1.123456f, 0.333333f, 2.718281f);
	const Vector3 pos_b(1.618033f, -0.876543f, -0.444444f);
	const Vector3 nrm_a = Vector3(0.2f, 0.9f, 0.3f).normalized();
	const Vector3 nrm_b = Vector3(-0.3f, 0.85f, -0.4f).normalized();
	const Vector2 uv_a(0.125f, 0.375f);
	const Vector2 uv_b(0.875f, 0.625f);

	const Vector3 tri1_pos[3] = { pos_a, pos_b, Vector3(0.0f, 3.0f, 0.5f) };
	const Vector3 tri2_pos[3] = { pos_b, pos_a, Vector3(0.1f, -4.2f, -1.9f) };
	const Vector3 tri1_nrm[3] = { nrm_a, nrm_b, Vector3(0.0f, 1.0f, 0.0f) };
	const Vector3 tri2_nrm[3] = { nrm_b, nrm_a, Vector3(1.0f, 0.0f, 0.0f) };
	const Vector2 tri1_uv[3] = { uv_a, uv_b, Vector2(0.5f, 0.99f) };
	const Vector2 tri2_uv[3] = { uv_b, uv_a, Vector2(0.11f, 0.02f) };

	for (uint32_t f : { 2u, 3u, 5u, 7u, 13u, 32u, 64u }) {
		for (uint32_t k = 1u; k < f; k++) {
			// On Tri1, edge 0 is V0 -> V1 = A -> B at k / f.
			// On Tri2, edge 0 is V0 -> V1 = B -> A at (f - k) / f (the exact same point along the shared edge!).
			const NextGeoTessVertexCoord c1 = nextgeo_tess_make_edge(0u, k, f);
			const NextGeoTessVertexCoord c2 = nextgeo_tess_make_edge(0u, f - k, f);

			const Vector3 p1 = nextgeo_tess_interp_vec3(tri1_pos, tri1_pos, c1);
			const Vector3 p2 = nextgeo_tess_interp_vec3(tri2_pos, tri2_pos, c2);
			CHECK_MESSAGE(bit_equal_vec3(p1, p2), "Shared edge position must be bit-identical regardless of triangle orientation or opposite vertex.");

			const Vector3 n1 = nextgeo_tess_interp_vec3(tri1_pos, tri1_nrm, c1);
			const Vector3 n2 = nextgeo_tess_interp_vec3(tri2_pos, tri2_nrm, c2);
			CHECK_MESSAGE(bit_equal_vec3(n1, n2), "Shared edge normal must be bit-identical across neighbors.");

			const Vector2 u1 = nextgeo_tess_interp_vec2(tri1_pos, tri1_uv, c1);
			const Vector2 u2 = nextgeo_tess_interp_vec2(tri2_pos, tri2_uv, c2);
			CHECK_MESSAGE(bit_equal_vec2(u1, u2), "Shared edge UV must be bit-identical across neighbors.");
		}
	}
}

TEST_CASE("[NextGeo][Tessellation] Zero-degenerate triangulation covers domain with exact area and boundary steps") {
	struct Case {
		uint32_t f0;
		uint32_t f1;
		uint32_t f2;
	};
	const Case cases[] = {
		{ 1, 1, 1 },
		{ 2, 1, 1 },
		{ 1, 2, 1 },
		{ 1, 1, 2 },
		{ 2, 2, 2 },
		{ 3, 1, 1 },
		{ 3, 2, 1 },
		{ 3, 3, 3 },
		{ 4, 1, 1 },
		{ 4, 2, 3 },
		{ 4, 4, 4 },
		{ 5, 1, 4 },
		{ 6, 3, 2 },
		{ 7, 7, 7 },
		{ 8, 2, 5 },
		{ 12, 1, 12 },
		{ 16, 16, 16 },
	};

	for (const Case &tc : cases) {
		const uint32_t count = nextgeo_tess_micro_tri_count(tc.f0, tc.f1, tc.f2);
		CHECK_GT(count, 0u);
		if (tc.f0 == tc.f1 && tc.f1 == tc.f2) {
			if (tc.f0 == 1u) {
				CHECK_EQ(count, 1u);
			} else if (tc.f0 >= 4u) {
				CHECK_EQ(count, tc.f0 * tc.f0);
			}
		}
		double area_sum = 0.0;
		for (uint32_t m = 0; m < count; m++) {
			NextGeoTessVertexCoord c[3];
			nextgeo_tess_micro_tri_coords(tc.f0, tc.f1, tc.f2, m, c);
			// Barycentrics must be valid and sum to 1.
			for (int k = 0; k < 3; k++) {
				CHECK_GE(c[k].u, -1e-6f);
				CHECK_GE(c[k].v, -1e-6f);
				CHECK_GE(c[k].w, -1e-6f);
				CHECK_EQ(c[k].u + c[k].v + c[k].w, doctest::Approx(1.0f).epsilon(1e-5));
			}
			const float area2 = bary_signed_area_2x(c[0], c[1], c[2]);
			CHECK_MESSAGE(area2 > 1e-7f, "Micro-triangle must have strictly positive area and consistent winding.");
			area_sum += double(area2);
		}
		// Full source triangle has 2x barycentric area == 1.0.
		CHECK_EQ(area_sum, doctest::Approx(1.0).epsilon(1e-4));
	}
}

TEST_CASE("[NextGeo][Tessellation] Patch subdivision <= 128 micro-triangles per patch and overflow coarsening") {
	// High edge factor f = 64 -> 4096 micro-triangles -> 32 patches of exactly 128 micro-triangles each.
	CHECK_EQ(nextgeo_tess_micro_tri_count(64u, 64u, 64u), 4096u);
	CHECK_EQ(nextgeo_tess_patch_count(64u, 64u, 64u), 32u);

	LocalVector<NextGeoDicedPatchRecord> patches;
	NextGeoTessState state;
	const uint32_t emitted = nextgeo_tess_emit_triangle_patches(
			42u, 7u, 64u, 64u, 64u, 0.25f, 0.5f, 1024u, patches, state);
	CHECK_EQ(emitted, 32u);
	CHECK_EQ(patches.size(), 32u);
	CHECK_EQ(state.patch_count, 32u);
	CHECK_EQ(state.dicing_overflow, 0u);
	CHECK_EQ(state.coarsened_tris, 0u);

	uint32_t total_covered = 0u;
	for (uint32_t i = 0; i < patches.size(); i++) {
		const NextGeoDicedPatchRecord &rec = patches[i];
		const uint32_t src_tri = rec.tri_and_sub & 0xFFu;
		const uint32_t sub_patch = (rec.tri_and_sub >> 8u) & 0xFFu;
		const uint32_t micro_count = rec.tri_and_sub >> 16u;
		CHECK_EQ(rec.cluster_slot, 42u);
		CHECK_EQ(src_tri, 7u);
		CHECK_EQ(sub_patch, i);
		CHECK_GT(micro_count, 0u);
		CHECK_LE(micro_count, NEXTGEO_TESS_MAX_PATCH_TRIANGLES);
		CHECK_EQ(rec.micro_tri_base, i * NEXTGEO_TESS_MAX_PATCH_TRIANGLES);
		CHECK_EQ(rec.packed_factors & 0xFFu, 64u);
		CHECK_EQ((rec.packed_factors >> 8u) & 0xFFu, 64u);
		CHECK_EQ((rec.packed_factors >> 16u) & 0xFFu, 64u);
		CHECK_EQ(rec.disp_magnitude, doctest::Approx(0.25f));
		CHECK_EQ(rec.disp_center, doctest::Approx(0.5f));
		total_covered += micro_count;
	}
	CHECK_EQ(total_covered, 4096u);

	// Tight budget overflow: 4096 micro-tris needs 32 patches, only 4 remain -> coarsens cleanly into <= 4 patches.
	LocalVector<NextGeoDicedPatchRecord> tight_patches;
	NextGeoTessState tight_state;
	const uint32_t tight_emitted = nextgeo_tess_emit_triangle_patches(
			11u, 3u, 64u, 64u, 64u, 0.4f, 0.25f, 4u, tight_patches, tight_state);
	CHECK_GT(tight_emitted, 0u);
	CHECK_LE(tight_emitted, 4u);
	CHECK_EQ(tight_patches.size(), tight_emitted);
	CHECK_EQ(tight_state.dicing_overflow, 1u);
	CHECK_EQ(tight_state.coarsened_tris, 1u);
	for (const NextGeoDicedPatchRecord &rec : tight_patches) {
		CHECK_LE(rec.tri_and_sub >> 16u, NEXTGEO_TESS_MAX_PATCH_TRIANGLES);
		CHECK_LT(rec.packed_factors & 0xFFu, 64u);
	}

	// Zero remaining capacity -> 0 patches emitted, overflow incremented, no OOB writes.
	LocalVector<NextGeoDicedPatchRecord> full_patches;
	full_patches.resize(2);
	NextGeoTessState full_state;
	full_state.patch_count = 2u;
	const uint32_t full_emitted = nextgeo_tess_emit_triangle_patches(
			11u, 3u, 16u, 16u, 16u, 0.4f, 0.5f, 2u, full_patches, full_state);
	CHECK_EQ(full_emitted, 0u);
	CHECK_EQ(full_patches.size(), 2u);
	CHECK_EQ(full_state.dicing_overflow, 1u);
}

TEST_CASE("[NextGeo][Tessellation] ProjectSettings defaults, dicing rate clamping, and VSM toggles") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);

	CHECK(ps->has_setting("rendering/nextgeo/experimental/tessellation"));
	CHECK_EQ(bool(ps->get_setting("rendering/nextgeo/experimental/tessellation")), false);

	CHECK(ps->has_setting("rendering/nextgeo/tessellation/dicing_rate"));
	CHECK_EQ(float(ps->get_setting("rendering/nextgeo/tessellation/dicing_rate")), doctest::Approx(2.0f));

	CHECK(ps->has_setting("rendering/nextgeo/tessellation/debug_max_patches"));
	CHECK_EQ(int(ps->get_setting("rendering/nextgeo/tessellation/debug_max_patches")), 0);

	CHECK(ps->has_setting("rendering/lights_and_shadows/virtual_shadows/tessellation_directional"));
	CHECK_EQ(bool(ps->get_setting("rendering/lights_and_shadows/virtual_shadows/tessellation_directional")), true);

	CHECK(ps->has_setting("rendering/lights_and_shadows/virtual_shadows/tessellation_local"));
	CHECK_EQ(bool(ps->get_setting("rendering/lights_and_shadows/virtual_shadows/tessellation_local")), false);

	// Clamping rules.
	CHECK_EQ(nextgeo_tessellation_clamp_dicing_rate(0.1f), doctest::Approx(0.5f));
	CHECK_EQ(nextgeo_tessellation_clamp_dicing_rate(2.0f), doctest::Approx(2.0f));
	CHECK_EQ(nextgeo_tessellation_clamp_dicing_rate(64.0f), doctest::Approx(16.0f));
	CHECK_EQ(nextgeo_tessellation_clamp_dicing_rate(NAN), doctest::Approx(2.0f));

	// VSM chain toggle helper: master off -> both chains off; master on -> honours per-chain bools.
	CHECK_FALSE(nextgeo_tessellation_vsm_chain_enabled(false, NEXTGEO_VSM_CHAIN_DIRECTIONAL, true, true));
	CHECK_FALSE(nextgeo_tessellation_vsm_chain_enabled(false, NEXTGEO_VSM_CHAIN_LOCAL, true, true));
	CHECK(nextgeo_tessellation_vsm_chain_enabled(true, NEXTGEO_VSM_CHAIN_DIRECTIONAL, true, false));
	CHECK_FALSE(nextgeo_tessellation_vsm_chain_enabled(true, NEXTGEO_VSM_CHAIN_LOCAL, true, false));
	CHECK_FALSE(nextgeo_tessellation_vsm_chain_enabled(true, NEXTGEO_VSM_CHAIN_DIRECTIONAL, false, true));
	CHECK(nextgeo_tessellation_vsm_chain_enabled(true, NEXTGEO_VSM_CHAIN_LOCAL, false, true));

	// Conservative displacement bound accounts for center offset and scale.
	CHECK_EQ(nextgeo_tessellation_displacement_bound(1.0f, 0.5f), doctest::Approx(1.0f));
	CHECK_EQ(nextgeo_tessellation_displacement_bound(1.0f, 0.0f), doctest::Approx(1.0f));
	CHECK_EQ(nextgeo_tessellation_displacement_bound(1.0f, 1.5f), doctest::Approx(1.5f));
	CHECK_EQ(nextgeo_tessellation_displacement_bound(0.0f, 0.5f), doctest::Approx(0.0f));
}

TEST_CASE("[NextGeo][Tessellation] #150 dicing-compute and HW tessellation shader SPIR-V compilation") {
	ShaderCompiler compiler;
	ShaderCompiler::DefaultIdentifierActions defaults;
	defaults.renames["MODEL_MATRIX"] = "read_model_matrix";
	defaults.renames["MODEL_NORMAL_MATRIX"] = "model_normal_matrix";
	defaults.renames["VIEW_MATRIX"] = "read_view_matrix";
	defaults.renames["INV_VIEW_MATRIX"] = "inv_view_matrix";
	defaults.renames["PROJECTION_MATRIX"] = "projection_matrix";
	defaults.renames["INV_PROJECTION_MATRIX"] = "inv_projection_matrix";
	defaults.renames["MODELVIEW_MATRIX"] = "modelview";
	defaults.renames["MODELVIEW_NORMAL_MATRIX"] = "modelview_normal";
	defaults.renames["VERTEX"] = "vertex";
	defaults.renames["NORMAL"] = "normal_highp";
	defaults.renames["TANGENT"] = "tangent";
	defaults.renames["BINORMAL"] = "binormal";
	defaults.renames["POSITION"] = "position";
	defaults.renames["UV"] = "uv_interp";
	defaults.renames["UV2"] = "uv2_interp";
	defaults.renames["COLOR"] = "color_interp";
	defaults.renames["INSTANCE_CUSTOM"] = "instance_custom";
	defaults.renames["TIME"] = "global_time";
	defaults.renames["ALBEDO"] = "albedo_highp";
	defaults.renames["ALPHA"] = "alpha_highp";
	defaults.renames["ROUGHNESS"] = "roughness_highp";
	defaults.renames["FRAGCOORD"] = "gl_FragCoord";
	defaults.renames["FRONT_FACING"] = "gl_FrontFacing";
	defaults.renames["SCREEN_UV"] = "screen_uv";
	defaults.renames["VIEW"] = "view_highp";
	defaults.usage_defines["NORMAL"] = "#define NORMAL_USED\n";
	defaults.usage_defines["UV"] = "#define UV_USED\n";
	defaults.usage_defines["UV2"] = "#define UV2_USED\n";
	defaults.usage_defines["COLOR"] = "#define COLOR_USED\n";
	defaults.base_texture_binding_index = 1;
	defaults.texture_layout_set = 3;
	defaults.base_uniform_string = "material.";
	defaults.base_varying_index = 15;
	defaults.default_filter = ShaderLanguage::FILTER_LINEAR_MIPMAP;
	defaults.default_repeat = ShaderLanguage::REPEAT_ENABLE;
	defaults.global_buffer_array_variable = "global_shader_uniforms.data";
	defaults.instance_uniform_index_variable = "instances.data[instance_index_interp].instance_uniforms_ofs";
	defaults.resolve_builtin_derivatives["VERTEX"] = Pair<String, String>("vertex_interp_ddx", "vertex_interp_ddy");
	defaults.resolve_builtin_derivatives["UV"] = Pair<String, String>("uv_interp_ddx", "uv_interp_ddy");
	defaults.resolve_builtin_derivatives["UV2"] = Pair<String, String>("uv2_interp_ddx", "uv2_interp_ddy");
	defaults.resolve_builtin_derivatives["COLOR"] = Pair<String, String>("color_interp_ddx", "color_interp_ddy");
	defaults.resolve_builtin_derivatives["VIEW"] = Pair<String, String>("view_ddx", "view_ddy");
	defaults.resolve_builtin_derivatives["FRAGCOORD"] = Pair<String, String>("_ng_fragcoord_ddx", "_ng_fragcoord_ddy");
	defaults.resolve_mutable_builtin_derivatives["NORMAL"] = Pair<String, String>("normal_interp_ddx", "normal_interp_ddy");
	defaults.resolve_uniform_builtins.insert("TIME");
	defaults.resolve_renames["FRONT_FACING"] = "_ng_front_facing";
	defaults.resolve_renames["FRAGCOORD"] = "_ng_fragcoord";
	defaults.resolve_vertex_renames["VERTEX_ID"] = "_ng_vertex_id";
	defaults.resolve_vertex_renames["INSTANCE_ID"] = "_ng_instance_id";
	defaults.displacement_renames["VERTEX"] = "_ng_disp_vertex";
	defaults.displacement_renames["NORMAL"] = "_ng_disp_normal";
	defaults.displacement_renames["UV"] = "_ng_disp_uv";
	defaults.displacement_renames["UV2"] = "_ng_disp_uv2";
	defaults.displacement_renames["COLOR"] = "_ng_disp_color";
	defaults.displacement_renames["INSTANCE_CUSTOM"] = "_ng_disp_instance_custom";
	defaults.displacement_renames["DISPLACEMENT"] = "_ng_displacement";
	defaults.displacement_builtin_derivatives["VERTEX"] = Pair<String, String>("_ng_disp_vertex_ddx", "_ng_disp_vertex_ddy");
	defaults.displacement_builtin_derivatives["NORMAL"] = Pair<String, String>("_ng_disp_normal_ddx", "_ng_disp_normal_ddy");
	defaults.displacement_builtin_derivatives["UV"] = Pair<String, String>("_ng_disp_uv_ddx", "_ng_disp_uv_ddy");
	defaults.displacement_builtin_derivatives["UV2"] = Pair<String, String>("_ng_disp_uv2_ddx", "_ng_disp_uv2_ddy");
	defaults.displacement_builtin_derivatives["COLOR"] = Pair<String, String>("_ng_disp_color_ddx", "_ng_disp_color_ddy");
	compiler.initialize(defaults);

	const String disp_shader = R"(
shader_type spatial;
uniform sampler2D height_tex;
uniform float scale = 1.0;

void displacement() {
	float h = texture(height_tex, UV * scale).r;
	float ripple = sin(VERTEX.x * 3.0 + TIME) * 0.05;
	DISPLACEMENT = clamp(h + ripple, 0.0, 1.0);
}

void fragment() {
	ALBEDO = vec3(0.6, 0.5, 0.4);
	ROUGHNESS = 0.7;
}
)";

	ShaderCompiler::GeneratedCode gen;
	HashMap<StringName, ShaderLanguage::ShaderNode::Uniform> uniforms;
	ShaderCompiler::IdentifierActions actions;
	actions.entry_point_stages["vertex"] = ShaderCompiler::STAGE_VERTEX;
	actions.entry_point_stages["fragment"] = ShaderCompiler::STAGE_FRAGMENT;
	actions.entry_point_stages["light"] = ShaderCompiler::STAGE_FRAGMENT;
	actions.uniforms = &uniforms;
	actions.visibility_resolve = true;
	actions.raster_target = true;
	REQUIRE_EQ(compiler.compile(RSE::SHADER_SPATIAL, disp_shader, &actions, String(), gen), OK);
	REQUIRE(gen.has_displacement);
	REQUIRE(gen.code.has("resolve_displacement"));
	CHECK(gen.code["resolve_displacement"].contains("textureGrad("));

	HashMap<String, String> tess_code;
	tess_code["RESOLVE_VERTEX"] = gen.code.has("resolve_vertex") ? gen.code["resolve_vertex"] : String();
	tess_code["RESOLVE_VERTEX_STORE"] = gen.code.has("resolve_vertex_store") ? gen.code["resolve_vertex_store"] : String();
	tess_code["RESOLVE_VARYINGS"] = gen.code.has("resolve_varyings") ? gen.code["resolve_varyings"] : String();
	tess_code["RESOLVE_INTERPOLATE"] = gen.code.has("resolve_interpolate") ? gen.code["resolve_interpolate"] : String();
	tess_code["RESOLVE_DISPLACEMENT"] = gen.code["resolve_displacement"];
	tess_code["RASTER_FRAGMENT"] = gen.code.has("raster_fragment") ? gen.code["raster_fragment"] : String();

	const String vsm_defines = RendererVirtualShadowMapStorage::get_glsl_defines();
	Vector<ShaderRD::VariantDefine> variants;
	variants.push_back(ShaderRD::VariantDefine(0, "", false));
	variants.push_back(ShaderRD::VariantDefine(1, "\n#define VIS64_BUFFER\n", false));
	variants.push_back(ShaderRD::VariantDefine(2, "\n#define NEXTGEO_RASTER_DEPTH_ONLY\n" + vsm_defines, false));

	TessellationDiceShaderRD dice_shader;
	dice_shader.initialize(variants, "\n#define MATERIAL_UNIFORM_SET 3\n");
	const RID dice_ver = dice_shader.version_create(false);
	REQUIRE(dice_ver.is_valid());
	dice_shader.version_set_compute_code(dice_ver, tess_code, gen.uniforms, gen.stage_globals[ShaderCompiler::STAGE_FRAGMENT], gen.defines);

	TessellationHwShaderRD hw_shader;
	hw_shader.initialize(variants, "\n#define MATERIAL_UNIFORM_SET 3\n");
	const RID hw_ver = hw_shader.version_create(false);
	REQUIRE(hw_ver.is_valid());
	hw_shader.version_set_code(hw_ver, tess_code, gen.uniforms, gen.stage_globals[ShaderCompiler::STAGE_VERTEX], gen.stage_globals[ShaderCompiler::STAGE_FRAGMENT], gen.defines);

	for (int v = 0; v < 3; v++) {
		const Vector<String> dice_src = dice_shader.version_build_variant_stage_sources(dice_ver, v);
		REQUIRE_EQ(dice_src.size(), int(RD::SHADER_STAGE_MAX));
		CHECK(dice_src[RD::SHADER_STAGE_COMPUTE].contains("_ng_eval_displacement"));
		CHECK(dice_src[RD::SHADER_STAGE_COMPUTE].contains("nextgeo_tess_edge_factor"));

		const Vector<String> hw_src = hw_shader.version_build_variant_stage_sources(hw_ver, v);
		REQUIRE_EQ(hw_src.size(), int(RD::SHADER_STAGE_MAX));
		CHECK(hw_src[RD::SHADER_STAGE_VERTEX].contains("_ng_tess_eval_micro_triangle"));
		CHECK(hw_src[RD::SHADER_STAGE_FRAGMENT].contains("_ng_tess_eval_micro_triangle"));

#if defined(MODULE_GLSLANG_ENABLED) && defined(VULKAN_ENABLED)
		RenderingShaderContainerFormatVulkan vk_format;
		String err;
		RDC::ShaderStageSPIRVData cs;
		cs.shader_stage = RDC::SHADER_STAGE_COMPUTE;
		cs.spirv = compile_glslang_shader(RDC::SHADER_STAGE_COMPUTE, dice_src[RD::SHADER_STAGE_COMPUTE], vk_format.get_shader_language_version(), vk_format.get_shader_spirv_version(RDC::SHADER_STAGE_COMPUTE), &err);
		CHECK_MESSAGE(!cs.spirv.is_empty(), err);
		Ref<RenderingShaderContainer> cs_container = vk_format.create_container();
		CHECK(cs_container->set_code_from_spirv(vformat("TessDiceV%d", v), Vector<RDC::ShaderStageSPIRVData>({ cs })));

		RDC::ShaderStageSPIRVData vs;
		vs.shader_stage = RDC::SHADER_STAGE_VERTEX;
		vs.spirv = compile_glslang_shader(RDC::SHADER_STAGE_VERTEX, hw_src[RD::SHADER_STAGE_VERTEX], vk_format.get_shader_language_version(), vk_format.get_shader_spirv_version(RDC::SHADER_STAGE_VERTEX), &err);
		CHECK_MESSAGE(!vs.spirv.is_empty(), err);
		RDC::ShaderStageSPIRVData fs;
		fs.shader_stage = RDC::SHADER_STAGE_FRAGMENT;
		fs.spirv = compile_glslang_shader(RDC::SHADER_STAGE_FRAGMENT, hw_src[RD::SHADER_STAGE_FRAGMENT], vk_format.get_shader_language_version(), vk_format.get_shader_spirv_version(RDC::SHADER_STAGE_FRAGMENT), &err);
		CHECK_MESSAGE(!fs.spirv.is_empty(), err);
		Ref<RenderingShaderContainer> hw_container = vk_format.create_container();
		CHECK(hw_container->set_code_from_spirv(vformat("TessHwV%d", v), Vector<RDC::ShaderStageSPIRVData>({ vs, fs })));
#endif
	}

	CHECK(dice_shader.version_free(dice_ver));
	CHECK(hw_shader.version_free(hw_ver));
}

} // namespace TestNextGeoTessellation
