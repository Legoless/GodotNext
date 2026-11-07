/**************************************************************************/
/*  test_nextgeo_visibility.cpp                                           */
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

TEST_FORCE_LINK(test_nextgeo_visibility)

#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_visibility.h"
#include "servers/rendering/storage/virtual_mesh_storage.h"

#include <cstring>

using namespace RendererRD;

namespace {

class TestVisibilityVirtualMeshStorage : public RendererVirtualMeshStorage {
public:
	Vector<uint8_t> pages;
	Vector<uint8_t> clusters;

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
	bool _resize_clusters(uint32_t p_bytes) override {
		clusters.resize(int(p_bytes));
		return true;
	}
	void _upload_clusters(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) override {
		if (p_offset + p_size <= uint32_t(clusters.size())) {
			memcpy(clusters.ptrw() + p_offset, p_data, p_size);
		}
	}
	void _reset_clusters() override { clusters.clear(); }
};

static Vector<uint8_t> _make_single_cluster_page(uint8_t p_vertices, uint8_t p_triangles, uint16_t p_surface_id, uint32_t p_stride_words) {
	const uint32_t header[4] = { 1u, uint32_t(p_vertices), uint32_t(p_triangles), 0u };
	const uint32_t vert_bytes = uint32_t(p_vertices) * p_stride_words * 4u;
	const uint32_t tri_bytes = uint32_t(p_triangles) * 3u;
	Vector<uint8_t> page;
	page.resize(int(16u + 80u + vert_bytes + tri_bytes));
	memset(page.ptrw(), 0, page.size());
	memcpy(page.ptrw(), header, sizeof(header));

	uint8_t *rec = page.ptrw() + 16u;
	const uint32_t v_off = 0u;
	const uint32_t t_off = 0u;
	memcpy(rec + 68, &v_off, sizeof(uint32_t));
	memcpy(rec + 72, &t_off, sizeof(uint32_t));
	rec[76] = p_vertices;
	rec[77] = p_triangles;
	memcpy(rec + 78, &p_surface_id, sizeof(uint16_t));

	uint8_t *tri_ptr = page.ptrw() + 16u + 80u + vert_bytes;
	for (uint8_t t = 0; t < p_triangles; t++) {
		tri_ptr[t * 3u + 0u] = 0u;
		tri_ptr[t * 3u + 1u] = 1u;
		tri_ptr[t * 3u + 2u] = 2u;
	}
	return page;
}

} // namespace

TEST_CASE("[NextGeo][Visibility] 25|7 pack, unpack, background 0xFFFFFFFF and 128-triangle bound") {
	CHECK_EQ(NEXTGEO_VISIBILITY_TRIANGLE_BITS, uint32_t(7));
	CHECK_EQ(NEXTGEO_MAX_CLUSTER_TRIANGLES, uint32_t(128));
	CHECK_EQ(NEXTGEO_RASTER_VERTEX_COUNT, uint32_t(384));
	CHECK_EQ(NEXTGEO_VISIBILITY_BACKGROUND, uint32_t(0xFFFFFFFFu));
	CHECK_EQ(NEXTGEO_INSTANCE_ID_LEGACY, uint32_t(0xFFFFFFFFu));
	CHECK(nextgeo_visibility_is_background(NEXTGEO_VISIBILITY_BACKGROUND));
	CHECK_FALSE(nextgeo_visibility_is_background(0u));

	// Cluster 0, triangle 0 -> 0.
	const uint32_t first = nextgeo_pack_visibility_id(0u, 0u);
	CHECK_EQ(first, uint32_t(0));
	CHECK_FALSE(nextgeo_visibility_is_background(first));
	CHECK_EQ(nextgeo_unpack_visibility_cluster(first), uint32_t(0));
	CHECK_EQ(nextgeo_unpack_visibility_triangle(first), uint32_t(0));

	// Phase 1 and Phase 2 (base = 1 << 20) round-trip across [0, 127] triangles.
	const uint32_t sample_clusters[] = { 0u, 1u, 42u, (1u << 20u), (1u << 20u) + 7777u, NEXTGEO_VISIBILITY_MAX_CLUSTERS - 1u };
	const uint32_t sample_tris[] = { 0u, 1u, 63u, 64u, 126u, 127u };
	for (uint32_t c : sample_clusters) {
		for (uint32_t t : sample_tris) {
			const uint32_t packed = nextgeo_pack_visibility_id(c, t);
			CHECK_FALSE(nextgeo_visibility_is_background(packed));
			CHECK_EQ(nextgeo_unpack_visibility_cluster(packed), c);
			CHECK_EQ(nextgeo_unpack_visibility_triangle(packed), t);
		}
	}

	// Out-of-bounds triangle (>= 128) or reserved sentinel cluster maps to background.
	CHECK_EQ(nextgeo_pack_visibility_id(0u, 128u), NEXTGEO_VISIBILITY_BACKGROUND);
	CHECK_EQ(nextgeo_pack_visibility_id(5u, 255u), NEXTGEO_VISIBILITY_BACKGROUND);
	CHECK_EQ(nextgeo_pack_visibility_id(NEXTGEO_VISIBILITY_MAX_CLUSTERS, 0u), NEXTGEO_VISIBILITY_BACKGROUND);
	CHECK_EQ(nextgeo_pack_visibility_id(0x1FFFFFFu, 127u), NEXTGEO_VISIBILITY_BACKGROUND);
}

TEST_CASE("[NextGeo][Visibility] Draw path (c) indirect command layout") {
	const NextGeoDrawIndirectCommand zero = nextgeo_make_draw_indirect(0u);
	CHECK_EQ(zero.vertex_count, uint32_t(384));
	CHECK_EQ(zero.instance_count, uint32_t(0));
	CHECK_EQ(zero.first_vertex, uint32_t(0));
	CHECK_EQ(zero.first_instance, uint32_t(0));

	const NextGeoDrawIndirectCommand populated = nextgeo_make_draw_indirect(1536u);
	CHECK_EQ(populated.vertex_count, NEXTGEO_RASTER_VERTEX_COUNT);
	CHECK_EQ(populated.instance_count, uint32_t(1536));
	CHECK_EQ(populated.first_vertex, uint32_t(0));
	CHECK_EQ(populated.first_instance, uint32_t(0));
}

TEST_CASE("[NextGeo][Visibility] Per-upscaler velocity plan and PASS_MODE_COLOR load/clear flags") {
	// Neither NextGI nor NextGeo active -> today's behavior.
	const NextGIVelocityPlan idle_taa = nextgeo_velocity_plan(NEXTGI_VELOCITY_SCALE_NONE, NEXTGI_VELOCITY_TAA, true, false, false);
	CHECK_FALSE(idle_taa.active);
	CHECK_FALSE(idle_taa.prepass_writes);
	CHECK_FALSE(idle_taa.color_pass_loads);
	CHECK_FALSE(idle_taa.skip_clear_pass);
	CHECK_FALSE(idle_taa.mfx_preserve);

	// NextGeo without any temporal consumer -> velocity stays off.
	const NextGIVelocityPlan no_temporal = nextgeo_velocity_plan(NEXTGI_VELOCITY_SCALE_NONE, NEXTGI_VELOCITY_NONE, false, false, true);
	CHECK_FALSE(no_temporal.active);
	CHECK_FALSE(no_temporal.prepass_writes);

	// TAA with NextGeo only: pre-clear (0, 0), opaque color pass loads velocity.
	const NextGIVelocityPlan taa = nextgeo_velocity_plan(NEXTGI_VELOCITY_SCALE_NONE, NEXTGI_VELOCITY_TAA, true, false, true);
	CHECK(taa.active);
	CHECK(taa.prepass_writes);
	CHECK(taa.color_pass_loads);
	CHECK_FALSE(taa.skip_clear_pass);
	CHECK_FALSE(taa.mfx_preserve);
	CHECK_EQ(taa.pre_clear_r, 0.0f);
	CHECK_EQ(taa.pre_clear_g, 0.0f);

	// FSR2 with NextGeo only: pre-clear (-1, -1), skip clear-only velocity pass.
	const NextGIVelocityPlan fsr2 = nextgeo_velocity_plan(NEXTGI_VELOCITY_SCALE_FSR2, NEXTGI_VELOCITY_SENTINEL, false, false, true);
	CHECK(fsr2.active);
	CHECK(fsr2.prepass_writes);
	CHECK_FALSE(fsr2.color_pass_loads);
	CHECK(fsr2.skip_clear_pass);
	CHECK_FALSE(fsr2.mfx_preserve);
	CHECK_EQ(fsr2.pre_clear_r, -1.0f);
	CHECK_EQ(fsr2.pre_clear_g, -1.0f);

	// MetalFX (DENSE) and DLSS/external (DENSE) share the identical velocity_mode rule.
	const NextGIVelocityPlan mfx = nextgeo_velocity_plan(NEXTGI_VELOCITY_SCALE_MFX, NEXTGI_VELOCITY_DENSE, false, false, true);
	const NextGIVelocityPlan dlss = nextgeo_velocity_plan(NEXTGI_VELOCITY_SCALE_EXTERNAL, NEXTGI_VELOCITY_DENSE, true, false, true);
	const NextGIVelocityPlan dense_plans[] = { mfx, dlss };
	for (const NextGIVelocityPlan &dense : dense_plans) {
		CHECK(dense.active);
		CHECK(dense.prepass_writes);
		CHECK_FALSE(dense.color_pass_loads);
		CHECK_FALSE(dense.skip_clear_pass);
		CHECK(dense.mfx_preserve);
		CHECK_EQ(dense.pre_clear_r, -1.0f);
		CHECK_EQ(dense.pre_clear_g, -1.0f);
	}

	// Stage B load_color gate (§6.1): load_color turns on for NextGeo iff Stage B wrote.
	CHECK_FALSE(nextgeo_should_load_color(false, false, false));
	CHECK_FALSE(nextgeo_should_load_color(false, true, false));
	CHECK(nextgeo_should_load_color(false, true, true));
	CHECK(nextgeo_should_load_color(true, false, false));

	// Exhaustive check over all (load_color, color_pass_loads_velocity, separate_specular, depth_pre_pass)
	// combinations: DRAW_IGNORE_ALL (ATTACHMENT_LOAD_OP_DONT_CARE) is never emitted.
	for (int lc = 0; lc < 2; lc++) {
		for (int lv = 0; lv < 2; lv++) {
			for (int ss = 0; ss < 2; ss++) {
				for (int dp = 0; dp < 2; dp++) {
					const BitField<RenderingDevice::DrawFlags> flags = nextgeo_color_pass_draw_flags(lc != 0, lv != 0, ss != 0, dp != 0);
					CHECK_EQ(uint32_t(flags) & uint32_t(RenderingDevice::DRAW_IGNORE_ALL), uint32_t(0));
				}
			}
		}
	}

	// TAA color pass without separate specular: velocity is attachment 1, so only color 0 is cleared.
	const BitField<RenderingDevice::DrawFlags> taa_no_spec_flags = nextgeo_color_pass_draw_flags(false, true, false, true);
	CHECK_EQ(uint32_t(taa_no_spec_flags), uint32_t(RenderingDevice::DRAW_CLEAR_COLOR_0));

	// TAA color pass with separate specular: velocity is attachment 2, so color 0 and specular 1 are cleared.
	const BitField<RenderingDevice::DrawFlags> taa_spec_flags = nextgeo_color_pass_draw_flags(false, true, true, true);
	CHECK_EQ(uint32_t(taa_spec_flags), uint32_t(RenderingDevice::DRAW_CLEAR_COLOR_0 | RenderingDevice::DRAW_CLEAR_COLOR_1));

	// Stage B wrote -> load_color = true -> DRAW_DEFAULT_ALL with depth prepass.
	const BitField<RenderingDevice::DrawFlags> stage_b_flags = nextgeo_color_pass_draw_flags(true, true, false, true);
	CHECK_EQ(uint32_t(stage_b_flags), uint32_t(RenderingDevice::DRAW_DEFAULT_ALL));
}

TEST_CASE("[NextGeo][Visibility] VirtualMeshStorage page arena and cluster table packing") {
	TestVisibilityVirtualMeshStorage storage;
	const RID mesh = RID::from_uint64(9001);

	RenderingServerTypes::VirtualGeometryDesc vgd;
	vgd.eligible_surface_mask = 0x3u;
	vgd.root_pages.push_back(_make_single_cluster_page(24u, 32u, 1u, 3u));
	storage.mesh_set_virtual_geometry(mesh, vgd);

	const RendererVirtualMeshStorage::MeshRecord &mr = storage.get_mesh_record(mesh);
	CHECK_EQ(mr.page_count, uint32_t(1));
	CHECK_EQ(mr.cluster_count, uint32_t(1));
	REQUIRE(storage.clusters.size() >= int(sizeof(RendererVirtualMeshStorage::ClusterGPUDesc)));

	RendererVirtualMeshStorage::ClusterGPUDesc gpu_cluster;
	memcpy(&gpu_cluster, storage.clusters.ptr() + mr.page_table_offset * sizeof(gpu_cluster), sizeof(gpu_cluster));
	CHECK_EQ(gpu_cluster.pos_word_offset, uint32_t((16u + 80u) >> 2u));
	CHECK_EQ(gpu_cluster.tri_byte_offset, uint32_t(16u + 80u + 24u * 3u * 4u));
	CHECK_EQ(gpu_cluster.stride_and_counts & 0xFFu, uint32_t(3));
	CHECK_EQ((gpu_cluster.stride_and_counts >> 8u) & 0xFFu, uint32_t(24));
	CHECK_EQ((gpu_cluster.stride_and_counts >> 16u) & 0xFFu, uint32_t(32));
	CHECK_EQ(gpu_cluster.surface_and_page & 0xFFFFu, uint32_t(1));
	CHECK_EQ(gpu_cluster.surface_and_page >> 16u, uint32_t(0));

	storage.mesh_clear_virtual_geometry(mesh, 0);
	CHECK_FALSE(storage.mesh_has_virtual_geometry(mesh));
}
