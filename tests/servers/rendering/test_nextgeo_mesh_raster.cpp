/**************************************************************************/
/*  test_nextgeo_mesh_raster.cpp                                          */
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

TEST_FORCE_LINK(test_nextgeo_mesh_raster)

// GodotNext: nextgeo_mesh_raster (#154, plan §21.4 contract N9, GN-10).
// Device-free checks of the NextGeo task/mesh HW rasterizer: task workgroup and payload sizing math,
// device limit checks, path selection across Vulkan/D3D12/Metal (including mocked Apple7/8 vs Apple9+
// and missing SUPPORTS_MESH_SHADER), multiview rejection, and GLSL constant agreement.

#ifdef RD_ENABLED

#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_cut.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_mesh_raster.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_vsm.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/raster_features_inc.glsl.gen.h"

namespace TestNextGeoMeshRaster {

using namespace RendererRD;

NextGeoHWRasterCapabilities make_full_vulkan_caps(bool p_gn10_passed = false) {
	NextGeoHWRasterCapabilities caps;
	caps.supports_mesh_shader = true;
	caps.supports_mesh_shader_indirect = true;
	caps.supports_draw_indirect_count = true;
	caps.limits = RenderingDeviceDriver::d3d12_mesh_shader_limits(true);
	caps.gn10_passed = p_gn10_passed;
	return caps;
}

String glsl_constant(const char *p_source, const char *p_name) {
	const String text = String::utf8(p_source);
	const String needle = String("const uint ") + p_name + " = ";
	const int start = text.find(needle);
	if (start < 0) {
		return String();
	}
	const int expr_start = start + needle.length();
	const int end = text.find(";", expr_start);
	return end > expr_start ? text.substr(expr_start, end - expr_start).strip_edges() : String();
}

int64_t glsl_integer(const String &p_expr) {
	return p_expr.trim_suffix("u").to_int();
}

} // namespace TestNextGeoMeshRaster

using namespace TestNextGeoMeshRaster;

TEST_CASE("[NextGeo][MeshRaster] Task workgroup count and payload sizing math") {
	CHECK_EQ(NEXTGEO_MESH_TASK_GROUP_SIZE, 32u);
	CHECK_EQ(NEXTGEO_MESH_TASK_PAYLOAD_BYTES, 128u);
	CHECK_EQ(NEXTGEO_MESH_WORKGROUP_SIZE, 128u);
	CHECK_EQ(NEXTGEO_MESH_MAX_VERTICES, 128u);
	CHECK_EQ(NEXTGEO_MESH_MAX_PRIMITIVES, 128u);

	CHECK_EQ(nextgeo_mesh_task_groups(0u), 0u);
	CHECK_EQ(nextgeo_mesh_task_groups(1u), 1u);
	CHECK_EQ(nextgeo_mesh_task_groups(31u), 1u);
	CHECK_EQ(nextgeo_mesh_task_groups(32u), 1u);
	CHECK_EQ(nextgeo_mesh_task_groups(33u), 2u);
	CHECK_EQ(nextgeo_mesh_task_groups(64u), 2u);
	CHECK_EQ(nextgeo_mesh_task_groups(65u), 3u);

	// Worst-case cluster capacities fit within the portable 65,535 X task-group limit (D3D12 / Vulkan min).
	CHECK_EQ(nextgeo_mesh_task_groups(NextGeoCut::PHASE2_CLUSTER_BASE), 32768u);
	CHECK_LE(nextgeo_mesh_task_groups(NextGeoCut::PHASE2_CLUSTER_BASE), 65535u);
	CHECK_EQ(nextgeo_mesh_task_groups(NEXTGEO_VSM_CLUSTER_CAPACITY), 32768u);
	CHECK_LE(nextgeo_mesh_task_groups(NEXTGEO_VSM_CLUSTER_CAPACITY), 65535u);

	// Per-workgroup batch counts sum exactly to the HW cluster count for partial and full waves.
	for (uint32_t total : { 0u, 1u, 17u, 32u, 33u, 63u, 64u, 97u }) {
		const uint32_t groups = nextgeo_mesh_task_groups(total);
		uint32_t sum = 0u;
		for (uint32_t g = 0u; g < groups; g++) {
			const uint32_t batch = nextgeo_mesh_task_batch_count(total, g);
			CHECK_GT(batch, 0u);
			CHECK_LE(batch, NEXTGEO_MESH_TASK_GROUP_SIZE);
			sum += batch;
		}
		CHECK_EQ(sum, total);
		CHECK_EQ(nextgeo_mesh_task_batch_count(total, groups), 0u);
	}
}

TEST_CASE("[NextGeo][MeshRaster] Variant parsing of rendering/nextgeo/hw_raster_path") {
	CHECK_EQ(nextgeo_hw_raster_path_from_variant(Variant(0)), NEXTGEO_HW_RASTER_PATH_AUTO);
	CHECK_EQ(nextgeo_hw_raster_path_from_variant(Variant(1)), NEXTGEO_HW_RASTER_PATH_VERTEX);
	CHECK_EQ(nextgeo_hw_raster_path_from_variant(Variant(2)), NEXTGEO_HW_RASTER_PATH_MESH);
	CHECK_EQ(nextgeo_hw_raster_path_from_variant(Variant(99)), NEXTGEO_HW_RASTER_PATH_AUTO);
	CHECK_EQ(nextgeo_hw_raster_path_from_variant(Variant("auto")), NEXTGEO_HW_RASTER_PATH_AUTO);
	CHECK_EQ(nextgeo_hw_raster_path_from_variant(Variant("vertex")), NEXTGEO_HW_RASTER_PATH_VERTEX);
	CHECK_EQ(nextgeo_hw_raster_path_from_variant(Variant("vs")), NEXTGEO_HW_RASTER_PATH_VERTEX);
	CHECK_EQ(nextgeo_hw_raster_path_from_variant(Variant("mesh")), NEXTGEO_HW_RASTER_PATH_MESH);
	CHECK_EQ(nextgeo_hw_raster_path_from_variant(Variant(" MESH ")), NEXTGEO_HW_RASTER_PATH_MESH);
	CHECK_EQ(nextgeo_hw_raster_path_from_variant(Variant("unknown")), NEXTGEO_HW_RASTER_PATH_AUTO);
	CHECK_EQ(String(nextgeo_hw_raster_path_name(false)), String("vertex"));
	CHECK_EQ(String(nextgeo_hw_raster_path_name(true)), String("mesh"));
}

TEST_CASE("[NextGeo][MeshRaster] Path selection across settings, capabilities, drivers and multiview") {
	SUBCASE("Vulkan-class full capabilities: auto keeps VS until GN-10 passes, mesh selects mesh, vertex keeps VS") {
		const NextGeoHWRasterCapabilities pre_pq = make_full_vulkan_caps(false);
		NextGeoHWRasterSelection sel = nextgeo_select_hw_raster_path(NEXTGEO_HW_RASTER_PATH_AUTO, pre_pq);
		CHECK_FALSE(sel.use_mesh);
		CHECK_EQ(sel.reason, NEXTGEO_HW_RASTER_REASON_AUTO_PENDING_PQ);

		sel = nextgeo_select_hw_raster_path(NEXTGEO_HW_RASTER_PATH_VERTEX, pre_pq);
		CHECK_FALSE(sel.use_mesh);
		CHECK_EQ(sel.reason, NEXTGEO_HW_RASTER_REASON_SETTING_VERTEX);

		sel = nextgeo_select_hw_raster_path(NEXTGEO_HW_RASTER_PATH_MESH, pre_pq);
		CHECK(sel.use_mesh);
		CHECK_EQ(sel.reason, NEXTGEO_HW_RASTER_REASON_NONE);

		// Once GN-10 passes on a driver in PQ, auto selects mesh while explicit vertex still stays VS.
		const NextGeoHWRasterCapabilities post_pq = make_full_vulkan_caps(true);
		sel = nextgeo_select_hw_raster_path(NEXTGEO_HW_RASTER_PATH_AUTO, post_pq);
		CHECK(sel.use_mesh);
		CHECK_EQ(sel.reason, NEXTGEO_HW_RASTER_REASON_NONE);

		sel = nextgeo_select_hw_raster_path(NEXTGEO_HW_RASTER_PATH_VERTEX, post_pq);
		CHECK_FALSE(sel.use_mesh);
		CHECK_EQ(sel.reason, NEXTGEO_HW_RASTER_REASON_SETTING_VERTEX);
	}

	SUBCASE("Metal: Apple9+ supports mesh path; Apple7/Apple8 (M2) stay on VS; Apple6 / old MSL stay on VS") {
		const uint32_t msl = RenderingDeviceDriver::METAL_MESH_SHADER_MIN_MSL;
		// Mocked Apple7 & Apple8 (M2): mesh shaders exist but indirect mesh draws require Apple9+.
		for (uint32_t family : { 1007u, 1008u }) {
			const NextGeoHWRasterCapabilities caps = nextgeo_hw_raster_capabilities_from_metal(family, 30200u, true, true, true);
			CHECK(caps.supports_mesh_shader);
			CHECK_FALSE(caps.supports_mesh_shader_indirect);
			const NextGeoHWRasterSelection sel = nextgeo_select_hw_raster_path(NEXTGEO_HW_RASTER_PATH_MESH, caps);
			CHECK_FALSE(sel.use_mesh);
			CHECK_EQ(sel.reason, NEXTGEO_HW_RASTER_REASON_NO_MESH_INDIRECT);
		}
		// Apple9, Apple10, Apple11: full mesh + indirect mesh support.
		for (uint32_t family : { 1009u, 1010u, 1011u }) {
			const NextGeoHWRasterCapabilities caps = nextgeo_hw_raster_capabilities_from_metal(family, msl, true, true, false);
			CHECK(caps.supports_mesh_shader);
			CHECK(caps.supports_mesh_shader_indirect);
			CHECK(nextgeo_mesh_shader_fits_limits(caps.limits));
			const NextGeoHWRasterSelection sel_mesh = nextgeo_select_hw_raster_path(NEXTGEO_HW_RASTER_PATH_MESH, caps);
			CHECK(sel_mesh.use_mesh);
			CHECK_EQ(sel_mesh.reason, NEXTGEO_HW_RASTER_REASON_NONE);
			const NextGeoHWRasterSelection sel_auto = nextgeo_select_hw_raster_path(NEXTGEO_HW_RASTER_PATH_AUTO, caps);
			CHECK_FALSE(sel_auto.use_mesh);
			CHECK_EQ(sel_auto.reason, NEXTGEO_HW_RASTER_REASON_AUTO_PENDING_PQ);
		}
		// Apple6 or MSL < 3.0 or no argument buffers: no mesh shaders at all.
		for (const NextGeoHWRasterCapabilities &caps : {
					 nextgeo_hw_raster_capabilities_from_metal(1006u, 30200u, true, true, true),
					 nextgeo_hw_raster_capabilities_from_metal(1010u, 20400u, true, true, true),
					 nextgeo_hw_raster_capabilities_from_metal(1010u, 30200u, false, true, true),
			 }) {
			const NextGeoHWRasterSelection sel = nextgeo_select_hw_raster_path(NEXTGEO_HW_RASTER_PATH_MESH, caps);
			CHECK_FALSE(sel.use_mesh);
			CHECK_EQ(sel.reason, NEXTGEO_HW_RASTER_REASON_NO_MESH_SHADER);
		}
	}

	SUBCASE("D3D12: requires tier 1 + DXIL mesh stages + draw_indirect_count") {
		const NextGeoHWRasterCapabilities ready = nextgeo_hw_raster_capabilities_from_d3d12(RenderingDeviceDriver::D3D12_MESH_SHADER_TIER_1, true, true, false);
		CHECK(nextgeo_mesh_shader_fits_limits(ready.limits));
		CHECK(nextgeo_select_hw_raster_path(NEXTGEO_HW_RASTER_PATH_MESH, ready).use_mesh);
		CHECK_FALSE(nextgeo_select_hw_raster_path(NEXTGEO_HW_RASTER_PATH_AUTO, ready).use_mesh);

		const NextGeoHWRasterCapabilities no_tier = nextgeo_hw_raster_capabilities_from_d3d12(0u, true, true, true);
		CHECK_EQ(nextgeo_select_hw_raster_path(NEXTGEO_HW_RASTER_PATH_MESH, no_tier).reason, NEXTGEO_HW_RASTER_REASON_NO_MESH_SHADER);

		const NextGeoHWRasterCapabilities old_nir = nextgeo_hw_raster_capabilities_from_d3d12(RenderingDeviceDriver::D3D12_MESH_SHADER_TIER_1, false, true, true);
		CHECK_EQ(nextgeo_select_hw_raster_path(NEXTGEO_HW_RASTER_PATH_MESH, old_nir).reason, NEXTGEO_HW_RASTER_REASON_NO_MESH_SHADER);

		const NextGeoHWRasterCapabilities no_count = nextgeo_hw_raster_capabilities_from_d3d12(RenderingDeviceDriver::D3D12_MESH_SHADER_TIER_1, true, false, true);
		CHECK_EQ(nextgeo_select_hw_raster_path(NEXTGEO_HW_RASTER_PATH_MESH, no_count).reason, NEXTGEO_HW_RASTER_REASON_NO_INDIRECT_COUNT);
	}

	SUBCASE("Missing capabilities, tight limits, and multiview always fall back cleanly to VS") {
		NextGeoHWRasterCapabilities caps = make_full_vulkan_caps(true);
		CHECK_EQ(nextgeo_select_hw_raster_path(NEXTGEO_HW_RASTER_PATH_MESH, caps, true).reason, NEXTGEO_HW_RASTER_REASON_MULTIVIEW);

		caps.supports_mesh_shader = false;
		CHECK_EQ(nextgeo_select_hw_raster_path(NEXTGEO_HW_RASTER_PATH_MESH, caps).reason, NEXTGEO_HW_RASTER_REASON_NO_MESH_SHADER);

		caps = make_full_vulkan_caps(true);
		caps.supports_mesh_shader_indirect = false;
		CHECK_EQ(nextgeo_select_hw_raster_path(NEXTGEO_HW_RASTER_PATH_MESH, caps).reason, NEXTGEO_HW_RASTER_REASON_NO_MESH_INDIRECT);

		caps = make_full_vulkan_caps(true);
		caps.supports_draw_indirect_count = false;
		CHECK_EQ(nextgeo_select_hw_raster_path(NEXTGEO_HW_RASTER_PATH_MESH, caps).reason, NEXTGEO_HW_RASTER_REASON_NO_INDIRECT_COUNT);

		// Tightening any single required limit below NextGeo's task/mesh requirements disables mesh raster.
		for (int which = 0; which < 6; which++) {
			caps = make_full_vulkan_caps(true);
			switch (which) {
				case 0:
					caps.limits.max_task_workgroup_invocations = NEXTGEO_MESH_TASK_GROUP_SIZE - 1u;
					break;
				case 1:
					caps.limits.max_mesh_workgroup_invocations = NEXTGEO_MESH_WORKGROUP_SIZE - 1u;
					break;
				case 2:
					caps.limits.max_mesh_output_vertices = NEXTGEO_MESH_MAX_VERTICES - 1u;
					break;
				case 3:
					caps.limits.max_mesh_output_primitives = NEXTGEO_MESH_MAX_PRIMITIVES - 1u;
					break;
				case 4:
					caps.limits.max_task_payload_size = NEXTGEO_MESH_TASK_PAYLOAD_BYTES - 1u;
					break;
				case 5:
					caps.limits.max_mesh_workgroup_count[0] = NEXTGEO_MESH_TASK_GROUP_SIZE - 1u;
					break;
			}
			CHECK_FALSE(nextgeo_mesh_shader_fits_limits(caps.limits));
			const NextGeoHWRasterSelection sel = nextgeo_select_hw_raster_path(NEXTGEO_HW_RASTER_PATH_MESH, caps);
			CHECK_FALSE(sel.use_mesh);
			CHECK_EQ(sel.reason, NEXTGEO_HW_RASTER_REASON_LIMITS_EXCEEDED);
		}
	}

	SUBCASE("RenderingDevice mock feeds nextgeo_hw_raster_capabilities_from_device") {
		RenderingDevice *rd = memnew(RenderingDevice);
		const uint64_t all_bits = (uint64_t(1) << RenderingDevice::SUPPORTS_MESH_SHADER) |
				(uint64_t(1) << RenderingDevice::SUPPORTS_MESH_SHADER_INDIRECT) |
				(uint64_t(1) << RenderingDevice::SUPPORTS_DRAW_INDIRECT_COUNT);
		rd->set_feature_mock(true, all_bits);
		rd->set_mesh_shader_limits_mock(true, RenderingDeviceDriver::metal_mesh_shader_limits(1009u, true));
		const NextGeoHWRasterCapabilities caps = nextgeo_hw_raster_capabilities_from_device(rd, false);
		CHECK(nextgeo_select_hw_raster_path(NEXTGEO_HW_RASTER_PATH_MESH, caps).use_mesh);
		CHECK_FALSE(nextgeo_select_hw_raster_path(NEXTGEO_HW_RASTER_PATH_AUTO, caps).use_mesh);

		rd->set_feature_mock(true, uint64_t(1) << RenderingDevice::SUPPORTS_DRAW_INDIRECT_COUNT);
		rd->set_mesh_shader_limits_mock(true, RenderingDeviceDriver::MeshShaderLimits());
		const NextGeoHWRasterCapabilities no_mesh = nextgeo_hw_raster_capabilities_from_device(rd, true);
		CHECK_EQ(nextgeo_select_hw_raster_path(NEXTGEO_HW_RASTER_PATH_MESH, no_mesh).reason, NEXTGEO_HW_RASTER_REASON_NO_MESH_SHADER);
		memdelete(rd);
	}
}

TEST_CASE("[NextGeo][MeshRaster] GLSL raster_args and counter layout constants agree with C++") {
	CHECK_EQ(glsl_integer(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_ARGS_SW")), 0);
	CHECK_EQ(glsl_integer(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_ARGS_HW")), 1);
	CHECK_EQ(glsl_integer(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_ARGS_HW_MESH")), 2);
	CHECK_EQ(glsl_integer(glsl_constant(raster_features_inc_shader_glsl, "NEXTGEO_RASTER_ARGS_KINDS")), 3);
	CHECK_EQ(offsetof(NextGeoVSMCounters, hw_clusters), 13u * sizeof(uint32_t));
	CHECK_EQ(offsetof(NextGeoVSMCounters, sink), 21u * sizeof(uint32_t));
}

#endif // RD_ENABLED
