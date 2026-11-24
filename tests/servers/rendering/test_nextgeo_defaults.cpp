/**************************************************************************/
/*  test_nextgeo_defaults.cpp                                             */
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

TEST_FORCE_LINK(test_nextgeo_defaults)

// GodotNext: nextgeo_product_surface (#157, plan §21.4, §21.4.1, N1, N3, GN-14).
// Verifies UE-matched §21.4.1 defaults, EXPERIMENTAL XML labels, §6.3 PN fallback
// reasons, all 8 Nanite visualization ViewportDebugDraw modes, and per-frame NextGeo
// debug stat keys.

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/io/xml_parser.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_raster_bins.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_route.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_sw_raster.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/virtual_mesh_storage.h"

#ifndef _3D_DISABLED
#include "scene/3d/visual_instance_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/virtual_geometry_fallback.h"
#include "scene/resources/material.h"
#endif

namespace TestNextGeoDefaults {

class DefaultsTestVirtualMeshStorage : public RendererVirtualMeshStorage {
protected:
	void _upload_records(uint32_t, uint32_t) override {}
	void _upload_mesh_records(uint32_t, uint32_t) override {}
	bool _resize_dag(uint32_t) override { return true; }
};

struct ScopedVirtualMeshStorage {
	RendererVirtualMeshStorage *prev = nullptr;
	DefaultsTestVirtualMeshStorage storage;
	ScopedVirtualMeshStorage() {
		prev = RSG::virtual_mesh_storage;
		RSG::virtual_mesh_storage = &storage;
	}
	~ScopedVirtualMeshStorage() {
		RSG::virtual_mesh_storage = prev;
	}
};

static String resolve_doc_xml_path(const String &p_filename) {
	const String rel = String("doc/classes/").path_join(p_filename);
	if (FileAccess::exists(rel)) {
		return rel;
	}
	const String exe_dir = OS::get_singleton()->get_executable_path().get_base_dir();
	const String from_bin = exe_dir.path_join("../doc/classes").path_join(p_filename);
	if (FileAccess::exists(from_bin)) {
		return from_bin;
	}
	return rel;
}

static bool xml_class_has_experimental(const String &p_filename, String *r_value = nullptr) {
	Ref<XMLParser> parser;
	parser.instantiate();
	if (parser->open(resolve_doc_xml_path(p_filename)) != OK) {
		return false;
	}
	while (parser->read() == OK) {
		if (parser->get_node_type() == XMLParser::NODE_ELEMENT && parser->get_node_name() == "class") {
			if (!parser->has_attribute("experimental")) {
				return false;
			}
			const String val = parser->get_named_attribute_value_safe("experimental").strip_edges();
			if (r_value != nullptr) {
				*r_value = val;
			}
			return !val.is_empty();
		}
	}
	return false;
}

static bool xml_member_has_experimental(const String &p_filename, const String &p_member_name, String *r_value = nullptr) {
	Ref<XMLParser> parser;
	parser.instantiate();
	if (parser->open(resolve_doc_xml_path(p_filename)) != OK) {
		return false;
	}
	while (parser->read() == OK) {
		if (parser->get_node_type() == XMLParser::NODE_ELEMENT && parser->get_node_name() == "member") {
			if (parser->get_named_attribute_value_safe("name") == p_member_name) {
				if (!parser->has_attribute("experimental")) {
					return false;
				}
				const String val = parser->get_named_attribute_value_safe("experimental").strip_edges();
				if (r_value != nullptr) {
					*r_value = val;
				}
				return !val.is_empty();
			}
		}
	}
	return false;
}

static bool xml_member_exists(const String &p_filename, const String &p_member_name) {
	Ref<XMLParser> parser;
	parser.instantiate();
	if (parser->open(resolve_doc_xml_path(p_filename)) != OK) {
		return false;
	}
	while (parser->read() == OK) {
		if (parser->get_node_type() == XMLParser::NODE_ELEMENT && parser->get_node_name() == "member") {
			if (parser->get_named_attribute_value_safe("name") == p_member_name) {
				return true;
			}
		}
	}
	return false;
}

static bool xml_constant_has_value(const String &p_filename, const String &p_const_name, int p_expected_value) {
	Ref<XMLParser> parser;
	parser.instantiate();
	if (parser->open(resolve_doc_xml_path(p_filename)) != OK) {
		return false;
	}
	while (parser->read() == OK) {
		if (parser->get_node_type() == XMLParser::NODE_ELEMENT && parser->get_node_name() == "constant") {
			if (parser->get_named_attribute_value_safe("name") == p_const_name) {
				return parser->get_named_attribute_value_safe("value") == itos(p_expected_value);
			}
		}
	}
	return false;
}

static RendererRD::NextGeoSurfaceFacts make_virtual_candidate() {
	RendererRD::NextGeoSurfaceFacts facts;
	facts.instance_mesh = true;
	facts.mesh_has_virtual_geometry = true;
	facts.surface_in_mask = true;
	facts.draw_ready = true;
	return facts;
}

} // namespace TestNextGeoDefaults

using namespace TestNextGeoDefaults;

TEST_CASE("[NextGeo][Defaults] All §21.4.1 project settings and defaults (#157)") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);

	// Production PN settings & §21.4.1 UE-matched defaults.
	REQUIRE(ps->has_setting("rendering/nextgeo/raster_mode"));
	CHECK_EQ(int(ps->get_setting("rendering/nextgeo/raster_mode")), int(RendererRD::NEXTGEO_RASTER_MODE_HYBRID));

	REQUIRE(ps->has_setting("rendering/nextgeo/sw_raster_max_edge_px"));
	CHECK_EQ(float(ps->get_setting("rendering/nextgeo/sw_raster_max_edge_px")), doctest::Approx(8.0f));

	REQUIRE(ps->has_setting("rendering/nextgeo/hw_raster_path"));
	CHECK_EQ(int(ps->get_setting("rendering/nextgeo/hw_raster_path")), 0); // Auto

	REQUIRE(ps->has_setting("rendering/nextgeo/shading_path"));
	CHECK_EQ(int(ps->get_setting("rendering/nextgeo/shading_path")), 0); // Auto

	REQUIRE(ps->has_setting("rendering/nextgeo/ray_tracing_mode"));
	CHECK_EQ(int(ps->get_setting("rendering/nextgeo/ray_tracing_mode")), 0); // Fallback (Proxy); Native is Experimental

	REQUIRE(ps->has_setting("rendering/nextgeo/vertex_offset_max_default"));
	CHECK_EQ(float(ps->get_setting("rendering/nextgeo/vertex_offset_max_default")), doctest::Approx(0.5f));

	REQUIRE(ps->has_setting("rendering/nextgeo/tessellation/dicing_rate"));
	CHECK_EQ(float(ps->get_setting("rendering/nextgeo/tessellation/dicing_rate")), doctest::Approx(2.0f));

	// Experimental PN toggles: every single one MUST default to false.
	REQUIRE(ps->has_setting("rendering/nextgeo/experimental/assemblies"));
	CHECK_FALSE(bool(ps->get_setting("rendering/nextgeo/experimental/assemblies")));

	REQUIRE(ps->has_setting("rendering/nextgeo/experimental/voxels"));
	CHECK_FALSE(bool(ps->get_setting("rendering/nextgeo/experimental/voxels")));

	REQUIRE(ps->has_setting("rendering/nextgeo/experimental/foliage_wind"));
	CHECK_FALSE(bool(ps->get_setting("rendering/nextgeo/experimental/foliage_wind")));

	REQUIRE(ps->has_setting("rendering/nextgeo/experimental/tessellation"));
	CHECK_FALSE(bool(ps->get_setting("rendering/nextgeo/experimental/tessellation")));

	REQUIRE(ps->has_setting("rendering/nextgeo/experimental/dynamic_wind"));
	CHECK_FALSE(bool(ps->get_setting("rendering/nextgeo/experimental/dynamic_wind")));

	REQUIRE(ps->has_setting("rendering/nextgeo/experimental/dynamic_wind_mode"));
	CHECK_EQ(int(ps->get_setting("rendering/nextgeo/experimental/dynamic_wind_mode")), 0); // Force Based

	// Core & diagnostic NextGeo settings registered alongside PN defaults.
	REQUIRE(ps->has_setting("rendering/nextgeo/max_pixel_error"));
	CHECK_EQ(float(ps->get_setting("rendering/nextgeo/max_pixel_error")), doctest::Approx(1.0f));

	REQUIRE(ps->has_setting("rendering/nextgeo/occlusion"));
	CHECK(bool(ps->get_setting("rendering/nextgeo/occlusion")));

	REQUIRE(ps->has_setting("rendering/nextgeo/visibility_only"));
	CHECK_FALSE(bool(ps->get_setting("rendering/nextgeo/visibility_only")));

	REQUIRE(ps->has_setting("rendering/nextgeo/debug_programmable_raster"));
	CHECK_FALSE(bool(ps->get_setting("rendering/nextgeo/debug_programmable_raster")));

	REQUIRE(ps->has_setting("rendering/nextgeo/debug_no_int64_atomics"));
	CHECK_FALSE(bool(ps->get_setting("rendering/nextgeo/debug_no_int64_atomics")));

#ifndef _3D_DISABLED
	// Per-material and per-instance §21.4.1 defaults.
	{
		Ref<StandardMaterial3D> std_mat;
		std_mat.instantiate();
		CHECK_EQ(std_mat->get_vertex_offset_max(), doctest::Approx(-1.0f));
		CHECK_EQ(std_mat->get_displacement_magnitude(), doctest::Approx(0.0f));
		CHECK_EQ(std_mat->get_displacement_center(), doctest::Approx(0.5f));

		Ref<ShaderMaterial> shader_mat;
		shader_mat.instantiate();
		CHECK_EQ(shader_mat->get_vertex_offset_max(), doctest::Approx(-1.0f));
		CHECK_EQ(shader_mat->get_displacement_magnitude(), doctest::Approx(0.0f));
		CHECK_EQ(shader_mat->get_displacement_center(), doctest::Approx(0.5f));
	}

	{
		GeometryInstance3D *gi = memnew(GeometryInstance3D);
		CHECK_EQ(gi->get_programmable_raster_distance(), doctest::Approx(0.0f));
		CHECK_EQ(gi->get_vertex_offset_disable_distance(), doctest::Approx(0.0f));
		CHECK_FALSE(gi->get_rt_evaluate_vertex_offset());
		memdelete(gi);
	}
#endif
}

TEST_CASE("[NextGeo][Defaults] Experimental XML labels for §21.4.1 features (#157)") {
	// Every PN setting documented in ProjectSettings.xml.
	const char *documented_settings[] = {
		"rendering/nextgeo/raster_mode",
		"rendering/nextgeo/sw_raster_max_edge_px",
		"rendering/nextgeo/hw_raster_path",
		"rendering/nextgeo/shading_path",
		"rendering/nextgeo/ray_tracing_mode",
		"rendering/nextgeo/vertex_offset_max_default",
		"rendering/nextgeo/tessellation/dicing_rate",
		"rendering/nextgeo/experimental/assemblies",
		"rendering/nextgeo/experimental/voxels",
		"rendering/nextgeo/experimental/foliage_wind",
		"rendering/nextgeo/experimental/tessellation",
		"rendering/nextgeo/experimental/dynamic_wind",
		"rendering/nextgeo/experimental/dynamic_wind_mode",
		"rendering/nextgeo/debug_no_int64_atomics",
	};
	for (const char *setting : documented_settings) {
		CHECK_MESSAGE(xml_member_exists("ProjectSettings.xml", setting), "Missing ProjectSettings.xml entry: " << setting);
	}

	// Experimental ProjectSettings MUST carry the experimental="..." attribute so the editor renders EXPERIMENTAL.
	const char *experimental_project_settings[] = {
		"rendering/nextgeo/experimental/assemblies",
		"rendering/nextgeo/experimental/voxels",
		"rendering/nextgeo/experimental/foliage_wind",
		"rendering/nextgeo/experimental/tessellation",
		"rendering/nextgeo/experimental/dynamic_wind",
		"rendering/nextgeo/experimental/dynamic_wind_mode",
		"rendering/nextgeo/tessellation/dicing_rate",
	};
	for (const char *setting : experimental_project_settings) {
		CHECK_MESSAGE(xml_member_has_experimental("ProjectSettings.xml", setting), "Missing experimental attribute in ProjectSettings.xml for " << setting);
	}

	// Experimental PN resource classes.
	CHECK_MESSAGE(xml_class_has_experimental("VirtualGeometryAssembly.xml"), "VirtualGeometryAssembly.xml must carry class-level experimental attribute");
	CHECK_MESSAGE(xml_class_has_experimental("VirtualGeometryWind.xml"), "VirtualGeometryWind.xml must carry class-level experimental attribute");

	// Experimental VirtualGeometryData members.
	const char *experimental_vgd_members[] = {
		"assembly",
		"skin_influences",
		"voxel_brick_count",
		"wind",
	};
	for (const char *member : experimental_vgd_members) {
		CHECK_MESSAGE(xml_member_has_experimental("VirtualGeometryData.xml", member), "Missing experimental attribute in VirtualGeometryData.xml for " << member);
	}

	// Experimental scene/OBJ import options.
	const char *experimental_scene_import[] = {
		"meshes/virtual_geometry_assembly",
		"meshes/virtual_geometry_skinned",
		"virtual_geometry/voxels",
		"virtual_geometry/wind",
	};
	for (const char *member : experimental_scene_import) {
		CHECK_MESSAGE(xml_member_has_experimental("ResourceImporterScene.xml", member), "Missing experimental attribute in ResourceImporterScene.xml for " << member);
	}

	const char *experimental_obj_import[] = {
		"virtual_geometry/voxels",
		"virtual_geometry/wind",
	};
	for (const char *member : experimental_obj_import) {
		CHECK_MESSAGE(xml_member_has_experimental("ResourceImporterOBJ.xml", member), "Missing experimental attribute in ResourceImporterOBJ.xml for " << member);
	}
}

TEST_CASE("[NextGeo][Defaults][Fallback] All four §6.3 PN fallback reasons and routing (#157)") {
	// String table entries for all four PN fallback reasons.
	CHECK_EQ(String(RendererRD::nextgeo_fallback_name(RendererRD::NEXTGEO_FALLBACK_NO_INT64_ATOMICS)), "NO_INT64_ATOMICS");
	CHECK_EQ(String(RendererRD::nextgeo_fallback_name(RendererRD::NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE)), "UNSUPPORTED_RASTER_FEATURE");
	CHECK_EQ(String(RendererRD::nextgeo_fallback_name(RendererRD::NEXTGEO_FALLBACK_SW_RASTER_UNAVAILABLE)), "SW_RASTER_UNAVAILABLE");
	CHECK_EQ(String(RendererRD::nextgeo_fallback_name(RendererRD::NEXTGEO_FALLBACK_BLEND_SHAPES)), "BLEND_SHAPES");

	// Verify all four fit within the 5-bit fallback_class_tex reason field (< 31, reserving 31 for MULTIMESH_OR_PARTICLES).
	CHECK_LT(uint32_t(RendererRD::NEXTGEO_FALLBACK_MAX), 31u);

#ifndef _3D_DISABLED
	// Mirror enum in scene/resources/3d/virtual_geometry_fallback.h.
	CHECK_EQ(int(::NEXTGEO_FALLBACK_BLEND_SHAPES), 22);
	CHECK_EQ(int(::NEXTGEO_FALLBACK_NO_INT64_ATOMICS), 23);
	CHECK_EQ(int(::NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE), 24);
	CHECK_EQ(int(::NEXTGEO_FALLBACK_SW_RASTER_UNAVAILABLE), 25);
#endif

	// 1. NO_INT64_ATOMICS: below-tier device keeps HW raster with NO_INT64_ATOMICS.
	const RendererRD::NextGeoRasterTier no_atomics_tier = RendererRD::nextgeo_raster_tier(false, false, true);
	CHECK_FALSE(no_atomics_tier.hybrid);
	CHECK_EQ(no_atomics_tier.storage, RendererRD::NEXTGEO_VIS64_NONE);
	CHECK_EQ(no_atomics_tier.reason, RendererRD::NEXTGEO_FALLBACK_NO_INT64_ATOMICS);

	// 2. UNSUPPORTED_RASTER_FEATURE: material with raster program + FRAGCOORD / DEPTH / screen read.
	RendererRD::NextGeoMaterialFallbackFacts mat_facts;
	mat_facts.masked = true;
	mat_facts.admit_masked = true;
	mat_facts.raster_rejects = ShaderCompiler::RASTER_USES_FRAGCOORD;
	CHECK_EQ(RendererRD::nextgeo_material_fallback(mat_facts), RendererRD::NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE);

	RendererRD::NextGeoSurfaceFacts surf_facts = make_virtual_candidate();
	surf_facts.material_fallback = RendererRD::NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE;
	RendererRD::NextGeoSurfaceRoute route = RendererRD::nextgeo_resolve_surface_route(surf_facts);
	CHECK_FALSE(route.virtual_geometry);
	CHECK_EQ(route.reason, RendererRD::NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE);

	// 3. SW_RASTER_UNAVAILABLE: PN feature needing SW raster (tessellation / voxels) when SW raster is unavailable.
	surf_facts = make_virtual_candidate();
	surf_facts.sw_raster_required_unavailable = true;
	route = RendererRD::nextgeo_resolve_surface_route(surf_facts);
	CHECK_FALSE(route.virtual_geometry);
	CHECK_EQ(route.reason, RendererRD::NEXTGEO_FALLBACK_SW_RASTER_UNAVAILABLE);

	// 4. BLEND_SHAPES: meshes with blend shapes stay legacy with BLEND_SHAPES (even when skinned VGD is present).
	surf_facts = make_virtual_candidate();
	surf_facts.blend_shapes = true;
	surf_facts.skeleton = true;
	surf_facts.mesh_skinned_virtual_geometry = true;
	route = RendererRD::nextgeo_resolve_surface_route(surf_facts);
	CHECK_FALSE(route.virtual_geometry);
	CHECK_EQ(route.reason, RendererRD::NEXTGEO_FALLBACK_BLEND_SHAPES);
}

TEST_CASE("[NextGeo][Defaults] All 8 Nanite visualization ViewportDebugDraw modes (#157)") {
	// Existing enum values 0..72 MUST remain unchanged (VIEWPORT_DEBUG_DRAW_ML_VARIANCE == 72).
	CHECK_EQ(int(RSE::VIEWPORT_DEBUG_DRAW_ML_VARIANCE), 72);

	struct ModeSpec {
		RSE::ViewportDebugDraw rs_mode;
		int expected_value;
		const char *rs_name;
		const char *vp_name;
#ifndef _3D_DISABLED
		Viewport::DebugDraw vp_mode;
#endif
	};

	const ModeSpec specs[] = {
		{ RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_RASTER_BINS, 73, "VIEWPORT_DEBUG_DRAW_NEXTGEO_RASTER_BINS", "DEBUG_DRAW_NEXTGEO_RASTER_BINS"
#ifndef _3D_DISABLED
				,
				Viewport::DEBUG_DRAW_NEXTGEO_RASTER_BINS
#endif
		},
		{ RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_HW_SW, 74, "VIEWPORT_DEBUG_DRAW_NEXTGEO_HW_SW", "DEBUG_DRAW_NEXTGEO_HW_SW"
#ifndef _3D_DISABLED
				,
				Viewport::DEBUG_DRAW_NEXTGEO_HW_SW
#endif
		},
		{ RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_SHADING_BINS, 75, "VIEWPORT_DEBUG_DRAW_NEXTGEO_SHADING_BINS", "DEBUG_DRAW_NEXTGEO_SHADING_BINS"
#ifndef _3D_DISABLED
				,
				Viewport::DEBUG_DRAW_NEXTGEO_SHADING_BINS
#endif
		},
		{ RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_PROGRAMMABLE, 76, "VIEWPORT_DEBUG_DRAW_NEXTGEO_PROGRAMMABLE", "DEBUG_DRAW_NEXTGEO_PROGRAMMABLE"
#ifndef _3D_DISABLED
				,
				Viewport::DEBUG_DRAW_NEXTGEO_PROGRAMMABLE
#endif
		},
		{ RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_VERTEX_OFFSET, 77, "VIEWPORT_DEBUG_DRAW_NEXTGEO_VERTEX_OFFSET", "DEBUG_DRAW_NEXTGEO_VERTEX_OFFSET"
#ifndef _3D_DISABLED
				,
				Viewport::DEBUG_DRAW_NEXTGEO_VERTEX_OFFSET
#endif
		},
		{ RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_SKINNED, 78, "VIEWPORT_DEBUG_DRAW_NEXTGEO_SKINNED", "DEBUG_DRAW_NEXTGEO_SKINNED"
#ifndef _3D_DISABLED
				,
				Viewport::DEBUG_DRAW_NEXTGEO_SKINNED
#endif
		},
		{ RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_TESSELLATION, 79, "VIEWPORT_DEBUG_DRAW_NEXTGEO_TESSELLATION", "DEBUG_DRAW_NEXTGEO_TESSELLATION"
#ifndef _3D_DISABLED
				,
				Viewport::DEBUG_DRAW_NEXTGEO_TESSELLATION
#endif
		},
		{ RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_VOXELS, 80, "VIEWPORT_DEBUG_DRAW_NEXTGEO_VOXELS", "DEBUG_DRAW_NEXTGEO_VOXELS"
#ifndef _3D_DISABLED
				,
				Viewport::DEBUG_DRAW_NEXTGEO_VOXELS
#endif
		},
	};

	for (const ModeSpec &spec : specs) {
		CHECK_EQ(int(spec.rs_mode), spec.expected_value);
		CHECK(RendererRD::nextgeo_is_debug_view(spec.rs_mode));
		CHECK_FALSE(RendererRD::nextgeo_is_unsupported_debug_draw(spec.rs_mode));
		CHECK_EQ(RendererRD::nextgeo_view_fallback(false, false, RendererRD::nextgeo_is_unsupported_debug_draw(spec.rs_mode)), RendererRD::NEXTGEO_FALLBACK_NONE);

		bool valid_rs = false;
		const int64_t bound_rs = ClassDB::get_integer_constant("RenderingServer", spec.rs_name, &valid_rs);
		CHECK_MESSAGE(valid_rs, "RenderingServer constant not bound: " << spec.rs_name);
		CHECK_EQ(bound_rs, spec.expected_value);
		CHECK_MESSAGE(xml_constant_has_value("RenderingServer.xml", spec.rs_name, spec.expected_value), "RenderingServer.xml constant mismatch: " << spec.rs_name);

#ifndef _3D_DISABLED
		CHECK_EQ(int(spec.vp_mode), spec.expected_value);
		bool valid_vp = false;
		const int64_t bound_vp = ClassDB::get_integer_constant("Viewport", spec.vp_name, &valid_vp);
		CHECK_MESSAGE(valid_vp, "Viewport constant not bound: " << spec.vp_name);
		CHECK_EQ(bound_vp, spec.expected_value);
		CHECK_MESSAGE(xml_constant_has_value("Viewport.xml", spec.vp_name, spec.expected_value), "Viewport.xml constant mismatch: " << spec.vp_name);
#endif
	}
}

TEST_CASE("[NextGeo][Defaults] Per-frame NextGeo debug stats surface (#157)") {
	ScopedVirtualMeshStorage scoped_vms;

	// Verify native RT rebuild counter propagation into VirtualMeshStorage and virtual_geometry_get_debug_stats().
	RSG::virtual_mesh_storage->nextgeo_record_native_rt_rebuilds(3u);
	CHECK_EQ(RSG::virtual_mesh_storage->nextgeo_get_native_rt_rebuilds(), 3u);
	CHECK_EQ(RSG::virtual_mesh_storage->nextgeo_get_native_rt_rebuilds_total(), 3u);

	RSG::virtual_mesh_storage->nextgeo_record_native_rt_rebuilds(2u);
	CHECK_EQ(RSG::virtual_mesh_storage->nextgeo_get_native_rt_rebuilds(), 2u);
	CHECK_EQ(RSG::virtual_mesh_storage->nextgeo_get_native_rt_rebuilds_total(), 5u);

	RenderingServer *rs = RenderingServer::get_singleton();
	REQUIRE(rs != nullptr);
	RID vp = rs->viewport_create();
	rs->viewport_set_debug_draw(vp, RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_RASTER_BINS);

	Dictionary stats = rs->virtual_geometry_get_debug_stats(vp);
	CHECK_EQ(String(stats["view_route"]), "virtual");
	CHECK_EQ(String(stats["view_reason"]), "NONE");
	CHECK_EQ(int64_t(stats["native_rt_rebuilds"]), 2);
	CHECK_EQ(int64_t(stats["native_rt_rebuilds_total"]), 5);
	CHECK_EQ(String(stats["ray_tracing_mode"]), "fallback");

	const char *required_viewport_keys[] = {
		"view_route",
		"view_reason",
		"native_rt_rebuilds",
		"native_rt_rebuilds_total",
		"ray_tracing_mode",
		"fallback_coverage",
		"triangle_size_histogram",
	};
	for (const char *key : required_viewport_keys) {
		CHECK_MESSAGE(stats.has(key), "Missing viewport debug stats key: " << key);
	}

	rs->free_rid(vp);
}
