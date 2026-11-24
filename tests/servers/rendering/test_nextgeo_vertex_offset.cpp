/**************************************************************************/
/*  test_nextgeo_vertex_offset.cpp                                        */
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

TEST_FORCE_LINK(test_nextgeo_vertex_offset)

// GodotNext: nextgeo_vertex_offset (#164, plan §21.4 N5, GN-3, GN-4, GN-12).
// CPU unit tests for Material.vertex_offset_max inheritance/sanitization, bounds inflation and
// offset clamping, VERTEX vs POSITION/MODELVIEW_MATRIX/PROJECTION_MATRIX routing,
// GeometryInstance3D.vertex_offset_disable_distance demotion, opt-in rt_evaluate_vertex_offset
// persistence, VirtualMeshStorage::Record packing invariants, and GLSL constant parity.

#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_raster_bins.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_vsm.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/nextgeo_records_inc.glsl.gen.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/shader_compiler.h"
#include "servers/rendering/shader_types.h"
#include "servers/rendering/storage/virtual_mesh_storage.h"
#ifndef _3D_DISABLED
#include "scene/3d/visual_instance_3d.h"
#include "scene/resources/material.h"
#endif

#include <limits>

using namespace RendererRD;

namespace TestNextGeoVertexOffset {

using VMS = RendererVirtualMeshStorage;

class DummyVirtualMeshStorage : public RendererVirtualMeshStorage {
protected:
	virtual void _upload_records(uint32_t p_first, uint32_t p_count) override {}
	virtual void _upload_mesh_records(uint32_t p_first, uint32_t p_count) override {}
};

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

} // namespace TestNextGeoVertexOffset

using namespace TestNextGeoVertexOffset;

TEST_CASE("[NextGeo][VertexOffset] Material.vertex_offset_max resolution and project default inheritance (#164)") {
	CHECK_EQ(NEXTGEO_VERTEX_OFFSET_MAX_DEFAULT, doctest::Approx(0.5f));
	CHECK_EQ(NEXTGEO_VERTEX_OFFSET_MAX_INHERIT, doctest::Approx(-1.0f));

	// Negative material values inherit the project default.
	CHECK_EQ(nextgeo_resolve_vertex_offset_max(-1.0f, 0.5f), doctest::Approx(0.5f));
	CHECK_EQ(nextgeo_resolve_vertex_offset_max(-10.0f, 1.75f), doctest::Approx(1.75f));
	CHECK_EQ(nextgeo_resolve_vertex_offset_max(std::numeric_limits<float>::quiet_NaN(), 0.5f), doctest::Approx(0.5f));
	CHECK_EQ(nextgeo_resolve_vertex_offset_max(std::numeric_limits<float>::infinity(), 0.5f), doctest::Approx(0.5f));

	// Non-negative material values override the project default, including explicit 0.0 (clamp-to-zero).
	CHECK_EQ(nextgeo_resolve_vertex_offset_max(0.0f, 0.5f), doctest::Approx(0.0f));
	CHECK_EQ(nextgeo_resolve_vertex_offset_max(0.25f, 0.5f), doctest::Approx(0.25f));
	CHECK_EQ(nextgeo_resolve_vertex_offset_max(2.5f, 0.5f), doctest::Approx(2.5f));

	// Invalid project defaults clamp to 0.0.
	CHECK_EQ(nextgeo_resolve_vertex_offset_max(-1.0f, -2.0f), doctest::Approx(0.0f));
	CHECK_EQ(nextgeo_resolve_vertex_offset_max(-1.0f, std::numeric_limits<float>::quiet_NaN()), doctest::Approx(0.0f));
	CHECK_EQ(nextgeo_resolve_vertex_offset_max(-1.0f, std::numeric_limits<float>::infinity()), doctest::Approx(0.0f));

	// Materials that do not write VERTEX always resolve to a zero bound.
	CHECK_EQ(nextgeo_effective_vertex_offset_bound(false, 2.0f, 0.5f), doctest::Approx(0.0f));
	CHECK_EQ(nextgeo_effective_vertex_offset_bound(false, -1.0f, 0.5f), doctest::Approx(0.0f));
	CHECK_EQ(nextgeo_effective_vertex_offset_bound(true, -1.0f, 0.5f), doctest::Approx(0.5f));
	CHECK_EQ(nextgeo_effective_vertex_offset_bound(true, 1.25f, 0.5f), doctest::Approx(1.25f));
	CHECK_EQ(nextgeo_effective_vertex_offset_bound(true, 0.0f, 0.5f), doctest::Approx(0.0f));

	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps != nullptr && ps->has_setting("rendering/nextgeo/vertex_offset_max_default")) {
		CHECK_EQ(float(ps->get_setting("rendering/nextgeo/vertex_offset_max_default")), doctest::Approx(0.5f));
	}
}

TEST_CASE("[NextGeo][VertexOffset] Bounds inflation and offset clamping (#164)") {
	// World margin = vertex_offset_max * abs(instance_max_scale).
	CHECK_EQ(nextgeo_inflate_vertex_offset_world_margin(0.5f, 1.0f), doctest::Approx(0.5f));
	CHECK_EQ(nextgeo_inflate_vertex_offset_world_margin(0.5f, 3.0f), doctest::Approx(1.5f));
	CHECK_EQ(nextgeo_inflate_vertex_offset_world_margin(0.5f, -2.5f), doctest::Approx(1.25f));
	CHECK_EQ(nextgeo_inflate_vertex_offset_world_margin(0.0f, 4.0f), doctest::Approx(0.0f));
	CHECK_EQ(nextgeo_inflate_vertex_offset_world_margin(-1.0f, 4.0f), doctest::Approx(0.0f));
	CHECK_EQ(nextgeo_inflate_vertex_offset_world_margin(std::numeric_limits<float>::quiet_NaN(), 2.0f), doctest::Approx(0.0f));
	CHECK_EQ(nextgeo_inflate_vertex_offset_world_margin(0.5f, std::numeric_limits<float>::infinity()), doctest::Approx(0.0f));

	// Clamping preserves direction and sets clamped flag only when exceeding limit.
	Vector3 zero_delta(0.0f, 0.0f, 0.0f);
	CHECK_FALSE(nextgeo_clamp_vertex_offset(zero_delta, 0.5f));
	CHECK_EQ(zero_delta, Vector3(0.0f, 0.0f, 0.0f));

	Vector3 inside_delta(0.2f, 0.1f, -0.1f);
	const Vector3 inside_orig = inside_delta;
	CHECK_FALSE(nextgeo_clamp_vertex_offset(inside_delta, 0.5f));
	CHECK_EQ(inside_delta, inside_orig);

	Vector3 exact_delta(0.3f, 0.4f, 0.0f);
	CHECK_FALSE(nextgeo_clamp_vertex_offset(exact_delta, 0.5f));
	CHECK_EQ(exact_delta.length(), doctest::Approx(0.5f));

	Vector3 over_delta(3.0f, 4.0f, 0.0f);
	CHECK(nextgeo_clamp_vertex_offset(over_delta, 0.5f));
	CHECK_EQ(over_delta.x, doctest::Approx(0.3f));
	CHECK_EQ(over_delta.y, doctest::Approx(0.4f));
	CHECK_EQ(over_delta.z, doctest::Approx(0.0f));
	CHECK_EQ(over_delta.length(), doctest::Approx(0.5f));

	// Explicit 0.0 limit clamps any non-zero offset to zero.
	Vector3 clamp_zero(0.1f, -0.2f, 0.3f);
	CHECK(nextgeo_clamp_vertex_offset(clamp_zero, 0.0f));
	CHECK_EQ(clamp_zero, Vector3(0.0f, 0.0f, 0.0f));

	Vector3 clamp_zero_at_zero(0.0f, 0.0f, 0.0f);
	CHECK_FALSE(nextgeo_clamp_vertex_offset(clamp_zero_at_zero, 0.0f));

	// Conservative half-float packing for NextGeo instance records never underestimates the bound.
	CHECK_EQ(VMS::pack_vertex_offset_bound_half(0.0f), 0u);
	CHECK_EQ(VMS::pack_vertex_offset_bound_half(-1.0f), 0u);
	CHECK_EQ(VMS::pack_vertex_offset_bound_half(std::numeric_limits<float>::quiet_NaN()), 0u);
	CHECK_EQ(VMS::pack_vertex_offset_bound_half(std::numeric_limits<float>::infinity()), 0u);
	const float test_bounds[] = { 0.001f, 0.01f, 0.1f, 0.25f, 0.3333f, 0.5f, 0.777f, 1.0f, 2.71828f, 15.3f, 100.25f };
	for (float b : test_bounds) {
		const uint16_t packed = VMS::pack_vertex_offset_bound_half(b);
		const float unpacked = VMS::unpack_vertex_offset_bound_half(uint32_t(packed) << 16u);
		CHECK(unpacked >= b);
		CHECK(unpacked <= b * 1.01f + 1e-4f);
	}
}

TEST_CASE("[NextGeo][VertexOffset] Default routing admits VERTEX writes and rejects POSITION/MODELVIEW/PROJECTION (#164)") {
	// Nanite tier defaults: admit_masked = true, admit_vertex_offset = true, admit_programmable = false.
	NextGeoMaterialFallbackFacts facts;
	facts.admit_masked = true;
	facts.admit_vertex_offset = true;
	facts.writes_vertex = true;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_NONE);

	// Combined masked + vertex offset is also admitted.
	facts.masked = true;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_NONE);

	// POSITION / MODELVIEW_MATRIX / PROJECTION_MATRIX writes stay on legacy raster.
	facts.other_custom_vertex = true;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_UNSUPPORTED_CUSTOM_SHADER);
	facts.other_custom_vertex = false;

	// Raster target rejects (FRAGCOORD, SCREEN_TEXTURE, DEPTH, etc.) still fall back.
	facts.raster_rejects = ShaderCompiler::RASTER_USES_FRAGCOORD;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE);
	facts.raster_rejects = 0u;

	// Alpha blending still falls back to ALPHA.
	facts.transparent = true;
	CHECK_EQ(nextgeo_material_fallback(facts), NEXTGEO_FALLBACK_ALPHA);

	// Raster bin table covers VERTEX_OFFSET on both main visibility and VSM passes.
	CHECK_EQ(VMS::resolve_raster_bin(VMS::RASTER_FEATURE_VERTEX_OFFSET, VMS::RASTER_PASS_VISIBILITY), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));
	CHECK_EQ(VMS::resolve_raster_bin(VMS::RASTER_FEATURE_VERTEX_OFFSET, VMS::RASTER_PASS_VSM), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));
	CHECK_EQ(VMS::resolve_raster_bin(VMS::RASTER_FEATURE_VERTEX_OFFSET | VMS::RASTER_FEATURE_MASKED, VMS::RASTER_PASS_VISIBILITY), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));
	CHECK_EQ(VMS::resolve_raster_bin(VMS::RASTER_FEATURE_VERTEX_OFFSET | VMS::RASTER_FEATURE_MASKED, VMS::RASTER_PASS_VSM), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));
	CHECK_EQ(VMS::resolve_raster_bin(VMS::RASTER_FEATURE_VERTEX_OFFSET | VMS::RASTER_FEATURE_TWO_SIDED, VMS::RASTER_PASS_VSM), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));
}

TEST_CASE("[NextGeo][VertexOffset] ShaderCompiler routes VERTEX vs POSITION/MODELVIEW_MATRIX/PROJECTION_MATRIX (#164)") {
	if (ShaderTypes::get_singleton() == nullptr) {
		memnew(ShaderTypes);
	}
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
	defaults.renames["ALBEDO"] = "albedo_highp";
	defaults.renames["TIME"] = "global_time";
	defaults.renames["FRONT_FACING"] = "gl_FrontFacing";
	defaults.renames["FRAGCOORD"] = "gl_FragCoord";
	defaults.base_texture_binding_index = 1;
	defaults.texture_layout_set = 3;
	defaults.base_uniform_string = "material.";
	defaults.base_varying_index = 15;
	defaults.default_filter = ShaderLanguage::FILTER_LINEAR_MIPMAP;
	defaults.default_repeat = ShaderLanguage::REPEAT_ENABLE;
	defaults.resolve_builtin_derivatives["VERTEX"] = Pair<String, String>("vertex_interp_ddx", "vertex_interp_ddy");
	defaults.resolve_builtin_derivatives["UV"] = Pair<String, String>("uv_interp_ddx", "uv_interp_ddy");
	defaults.resolve_builtin_derivatives["NORMAL"] = Pair<String, String>("normal_interp_ddx", "normal_interp_ddy");
	defaults.resolve_renames["FRONT_FACING"] = "_ng_front_facing";
	defaults.resolve_renames["FRAGCOORD"] = "_ng_fragcoord";
	compiler.initialize(defaults);

	auto compile_and_route = [&](const String &p_code) -> NextGeoFallback {
		bool writes_vertex = false;
		bool writes_position = false;
		bool writes_mv_or_proj = false;
		bool writes_depth = false;
		HashMap<StringName, ShaderLanguage::ShaderNode::Uniform> uniforms;
		ShaderCompiler::IdentifierActions ia;
		ia.entry_point_stages["vertex"] = ShaderCompiler::STAGE_VERTEX;
		ia.entry_point_stages["fragment"] = ShaderCompiler::STAGE_FRAGMENT;
		ia.entry_point_stages["light"] = ShaderCompiler::STAGE_FRAGMENT;
		ia.uniforms = &uniforms;
		ia.write_flag_pointers["VERTEX"] = &writes_vertex;
		ia.write_flag_pointers["POSITION"] = &writes_position;
		ia.write_flag_pointers["MODELVIEW_MATRIX"] = &writes_mv_or_proj;
		ia.write_flag_pointers["PROJECTION_MATRIX"] = &writes_mv_or_proj;
		ia.write_flag_pointers["DEPTH"] = &writes_depth;
		ia.visibility_resolve = true;
		ia.raster_target = true;

		ShaderCompiler::GeneratedCode gen;
		const Error err = compiler.compile(RSE::SHADER_SPATIAL, p_code, &ia, "test_wpo.gdshader", gen);
		REQUIRE_EQ(err, OK);

		NextGeoRasterFacts rfacts;
		rfacts.writes_vertex = writes_vertex;
		const uint32_t features = nextgeo_raster_features(rfacts);

		NextGeoMaterialFallbackFacts ffacts;
		ffacts.writes_vertex = writes_vertex;
		ffacts.other_custom_vertex = writes_position || writes_mv_or_proj || writes_depth;
		ffacts.raster_rejects = nextgeo_raster_rejects(gen.raster_uses, features);
		ffacts.admit_masked = true;
		ffacts.admit_vertex_offset = true;
		return nextgeo_material_fallback(ffacts);
	};

	CHECK_EQ(compile_and_route("shader_type spatial;\nvoid vertex() { VERTEX.y += sin(TIME + VERTEX.x) * 0.25; }\nvoid fragment() { ALBEDO = vec3(0.4, 0.7, 0.2); }\n"), NEXTGEO_FALLBACK_NONE);
	CHECK_EQ(compile_and_route("shader_type spatial;\nvoid vertex() { POSITION = vec4(VERTEX, 1.0); }\nvoid fragment() { ALBEDO = vec3(0.8); }\n"), NEXTGEO_FALLBACK_UNSUPPORTED_CUSTOM_SHADER);
	CHECK_EQ(compile_and_route("shader_type spatial;\nvoid vertex() { MODELVIEW_MATRIX = mat4(1.0); }\nvoid fragment() { ALBEDO = vec3(0.8); }\n"), NEXTGEO_FALLBACK_UNSUPPORTED_CUSTOM_SHADER);
	CHECK_EQ(compile_and_route("shader_type spatial;\nvoid vertex() { PROJECTION_MATRIX = mat4(1.0); }\nvoid fragment() { ALBEDO = vec3(0.8); }\n"), NEXTGEO_FALLBACK_UNSUPPORTED_CUSTOM_SHADER);
}

TEST_CASE("[NextGeo][VertexOffset] Disable distance cutoff and combined bin demotion (#164)") {
	CHECK(nextgeo_raster_bin_can_demote_vertex_offset(VMS::RASTER_FEATURE_VERTEX_OFFSET));
	CHECK(nextgeo_raster_bin_can_demote_vertex_offset(VMS::RASTER_FEATURE_VERTEX_OFFSET | VMS::RASTER_FEATURE_TWO_SIDED));
	CHECK(nextgeo_raster_bin_can_demote_vertex_offset(VMS::RASTER_FEATURE_VERTEX_OFFSET | VMS::RASTER_FEATURE_MASKED));
	CHECK_FALSE(nextgeo_raster_bin_can_demote_vertex_offset(0u));
	CHECK_FALSE(nextgeo_raster_bin_can_demote_vertex_offset(VMS::RASTER_FEATURE_MASKED));
	CHECK_FALSE(nextgeo_raster_bin_can_demote_vertex_offset(VMS::RASTER_FEATURE_DISPLACED));
	CHECK_FALSE(nextgeo_raster_bin_can_demote_vertex_offset(VMS::RASTER_FEATURE_VERTEX_OFFSET | VMS::RASTER_FEATURE_DISPLACED));

	const Vector3 aabb_min(-1.0f, -1.0f, -1.0f);
	const Vector3 aabb_max(1.0f, 1.0f, 1.0f);

	// 0, negative, NaN, and Inf never disable vertex offset.
	CHECK_FALSE(nextgeo_vertex_offset_disabled_at_distance(Vector3(100.0f, 0.0f, 0.0f), aabb_min, aabb_max, 0.0f));
	CHECK_FALSE(nextgeo_vertex_offset_disabled_at_distance(Vector3(100.0f, 0.0f, 0.0f), aabb_min, aabb_max, -2.0f));
	CHECK_FALSE(nextgeo_vertex_offset_disabled_at_distance(Vector3(100.0f, 0.0f, 0.0f), aabb_min, aabb_max, std::numeric_limits<float>::quiet_NaN()));
	CHECK_FALSE(nextgeo_vertex_offset_disabled_at_distance(Vector3(100.0f, 0.0f, 0.0f), aabb_min, aabb_max, std::numeric_limits<float>::infinity()));

	// Distance from (4, 0, 0) to [-1,1]^3 is exactly 3.0 m.
	CHECK_FALSE(nextgeo_vertex_offset_disabled_at_distance(Vector3(3.5f, 0.0f, 0.0f), aabb_min, aabb_max, 3.0f));
	CHECK_FALSE(nextgeo_vertex_offset_disabled_at_distance(Vector3(4.0f, 0.0f, 0.0f), aabb_min, aabb_max, 3.0f));
	CHECK(nextgeo_vertex_offset_disabled_at_distance(Vector3(4.01f, 0.0f, 0.0f), aabb_min, aabb_max, 3.0f));

	// Pure VERTEX_OFFSET demotes to FIXED past vertex_offset_disable_distance even when programmable_raster_distance == 0.
	const uint32_t pure_wpo = VMS::RASTER_FEATURE_VERTEX_OFFSET | VMS::RASTER_FEATURE_TWO_SIDED;
	CHECK_FALSE(nextgeo_raster_bin_demotes_to_fixed(Vector3(4.0f, 0.0f, 0.0f), aabb_min, aabb_max, 0.0f, 3.0f, pure_wpo));
	CHECK(nextgeo_raster_bin_demotes_to_fixed(Vector3(4.01f, 0.0f, 0.0f), aabb_min, aabb_max, 0.0f, 3.0f, pure_wpo));
	// programmable_raster_distance alone does not demote VERTEX_OFFSET.
	CHECK_FALSE(nextgeo_raster_bin_demotes_to_fixed(Vector3(20.0f, 0.0f, 0.0f), aabb_min, aabb_max, 3.0f, 0.0f, pure_wpo));

	// Combined MASKED | VERTEX_OFFSET demotes to FIXED only when BOTH cutoffs are exceeded.
	const uint32_t masked_wpo = VMS::RASTER_FEATURE_MASKED | VMS::RASTER_FEATURE_VERTEX_OFFSET;
	CHECK_FALSE(nextgeo_raster_bin_demotes_to_fixed(Vector3(10.0f, 0.0f, 0.0f), aabb_min, aabb_max, 0.0f, 3.0f, masked_wpo));
	CHECK_FALSE(nextgeo_raster_bin_demotes_to_fixed(Vector3(10.0f, 0.0f, 0.0f), aabb_min, aabb_max, 3.0f, 0.0f, masked_wpo));
	CHECK_FALSE(nextgeo_raster_bin_demotes_to_fixed(Vector3(6.0f, 0.0f, 0.0f), aabb_min, aabb_max, 6.0f, 3.0f, masked_wpo));
	CHECK(nextgeo_raster_bin_demotes_to_fixed(Vector3(8.0f, 0.0f, 0.0f), aabb_min, aabb_max, 6.0f, 3.0f, masked_wpo));

	// Displaced bins never demote to FIXED.
	CHECK_FALSE(nextgeo_raster_bin_demotes_to_fixed(Vector3(50.0f, 0.0f, 0.0f), aabb_min, aabb_max, 1.0f, 1.0f, VMS::RASTER_FEATURE_DISPLACED | VMS::RASTER_FEATURE_VERTEX_OFFSET));
}

TEST_CASE("[NextGeo][VertexOffset] VirtualMeshStorage record persistence and GLSL parity (#164)") {
	DummyVirtualMeshStorage storage;
	const RID mesh_rid = RID::from_uint64(16401);
	const RID inst_rid = RID::from_uint64(16402);
	RenderingServerTypes::VirtualGeometryDesc desc;
	desc.eligible_surface_mask = 1u;
	desc.ngm_path = "res://test_164.ngm";
	desc.dag_header.resize(8);
	desc.root_pages.push_back(Vector<uint8_t>());
	storage.mesh_set_virtual_geometry(mesh_rid, desc);

	const uint32_t slot = storage.instance_record_allocate(inst_rid, mesh_rid);
	REQUIRE_NE(slot, UINT32_MAX);

	// Defaults.
	CHECK_EQ(VMS::get_record_vertex_offset_bound(storage.get_record(slot)), doctest::Approx(0.0f));
	CHECK_EQ(VMS::get_record_vertex_offset_disable_distance(storage.get_record(slot)), doctest::Approx(0.0f));
	CHECK_EQ((storage.get_record(slot).flags & VMS::RECORD_RT_EVALUATE_VERTEX_OFFSET), 0u);

	// Set all three WPO record fields and verify they persist across state, gi_flags, transform, and frame flush.
	storage.instance_record_set_vertex_offset_bound(slot, 0.75f);
	storage.instance_record_set_vertex_offset_disable_distance(slot, 18.5f);
	storage.instance_record_set_rt_evaluate_vertex_offset(slot, true);

	CHECK(VMS::get_record_vertex_offset_bound(storage.get_record(slot)) >= 0.75f);
	CHECK_EQ(VMS::get_record_vertex_offset_disable_distance(storage.get_record(slot)), doctest::Approx(18.5f));
	CHECK_NE((storage.get_record(slot).flags & VMS::RECORD_RT_EVALUATE_VERTEX_OFFSET), 0u);

	// Updating state flags preserves RECORD_RT_EVALUATE_VERTEX_OFFSET; updating gi_flags preserves upper 16-bit WPO bound.
	storage.instance_record_set_state(slot, VMS::RECORD_VISIBLE | VMS::RECORD_CAST_SHADOW_ON);
	CHECK_NE((storage.get_record(slot).flags & VMS::RECORD_RT_EVALUATE_VERTEX_OFFSET), 0u);
	storage.instance_record_set_gi_flags(slot, 0x1234u);
	CHECK_EQ((storage.get_record(slot).gi_flags & 0xFFFFu), 0x1234u);
	CHECK(VMS::get_record_vertex_offset_bound(storage.get_record(slot)) >= 0.75f);

	// Updating transform and flushing preserves all WPO fields.
	const AABB unit_aabb(Vector3(-1, -1, -1), Vector3(2, 2, 2));
	storage.instance_record_set_transform(slot, Transform3D(Basis(), Vector3(3.0f, 0.0f, 0.0f)), unit_aabb, true, false, 1.0f);
	storage.flush();
	storage.begin_frame();
	CHECK(VMS::get_record_vertex_offset_bound(storage.get_record(slot)) >= 0.75f);
	CHECK_EQ(VMS::get_record_vertex_offset_disable_distance(storage.get_record(slot)), doctest::Approx(18.5f));
	CHECK_NE((storage.get_record(slot).flags & VMS::RECORD_RT_EVALUATE_VERTEX_OFFSET), 0u);

	// Disabling RT flag and sanitizing invalid inputs.
	storage.instance_record_set_rt_evaluate_vertex_offset(slot, false);
	CHECK_EQ((storage.get_record(slot).flags & VMS::RECORD_RT_EVALUATE_VERTEX_OFFSET), 0u);
	storage.instance_record_set_vertex_offset_disable_distance(slot, -5.0f);
	CHECK_EQ(VMS::get_record_vertex_offset_disable_distance(storage.get_record(slot)), doctest::Approx(0.0f));
	storage.instance_record_set_vertex_offset_disable_distance(slot, std::numeric_limits<float>::quiet_NaN());
	CHECK_EQ(VMS::get_record_vertex_offset_disable_distance(storage.get_record(slot)), doctest::Approx(0.0f));
	storage.instance_record_set_vertex_offset_bound(slot, -1.0f);
	CHECK_EQ(VMS::get_record_vertex_offset_bound(storage.get_record(slot)), doctest::Approx(0.0f));
	CHECK_EQ((storage.get_record(slot).gi_flags & 0xFFFFu), 0x1234u);
	storage.instance_record_free(slot);

	// GLSL constant & VSM counter layout parity.
	CHECK_EQ(glsl_constant(nextgeo_records_inc_shader_glsl, "NEXTGEO_RECORD_RT_EVALUATE_VERTEX_OFFSET"), String("1u << 15"));
	CHECK_EQ(uint32_t(VMS::RECORD_RT_EVALUATE_VERTEX_OFFSET), 1u << 15u);
	CHECK_EQ(sizeof(NextGeoVSMCounters), size_t(128));
	CHECK_EQ(offsetof(NextGeoVSMCounters, vertex_offset_distance_fixed), size_t(27 * sizeof(uint32_t)));
	CHECK_EQ(offsetof(NextGeoVSMCounters, vertex_offset_clamped), size_t(28 * sizeof(uint32_t)));
}

#ifndef _3D_DISABLED
TEST_CASE("[NextGeo][VertexOffset] Material and GeometryInstance3D properties round-trip (#164)") {
	Ref<ShaderMaterial> mat;
	mat.instantiate();
	CHECK_EQ(mat->get_vertex_offset_max(), doctest::Approx(-1.0f));
	mat->set_vertex_offset_max(1.25f);
	CHECK_EQ(mat->get_vertex_offset_max(), doctest::Approx(1.25f));
	mat->set_vertex_offset_max(0.0f);
	CHECK_EQ(mat->get_vertex_offset_max(), doctest::Approx(0.0f));
	mat->set_vertex_offset_max(-5.0f);
	CHECK_EQ(mat->get_vertex_offset_max(), doctest::Approx(-1.0f));
	mat->set_vertex_offset_max(std::numeric_limits<float>::quiet_NaN());
	CHECK_EQ(mat->get_vertex_offset_max(), doctest::Approx(-1.0f));
	mat->set_vertex_offset_max(std::numeric_limits<float>::infinity());
	CHECK_EQ(mat->get_vertex_offset_max(), doctest::Approx(-1.0f));

	GeometryInstance3D *inst = memnew(GeometryInstance3D);
	CHECK_EQ(inst->get_vertex_offset_disable_distance(), doctest::Approx(0.0f));
	inst->set_vertex_offset_disable_distance(42.5f);
	CHECK_EQ(inst->get_vertex_offset_disable_distance(), doctest::Approx(42.5f));
	inst->set_vertex_offset_disable_distance(0.0f);
	CHECK_EQ(inst->get_vertex_offset_disable_distance(), doctest::Approx(0.0f));
	ERR_PRINT_OFF;
	inst->set_vertex_offset_disable_distance(-3.0f);
	ERR_PRINT_ON;
	CHECK_EQ(inst->get_vertex_offset_disable_distance(), doctest::Approx(0.0f));

	CHECK_FALSE(inst->get_rt_evaluate_vertex_offset());
	inst->set_rt_evaluate_vertex_offset(true);
	CHECK(inst->get_rt_evaluate_vertex_offset());
	inst->set_rt_evaluate_vertex_offset(false);
	CHECK_FALSE(inst->get_rt_evaluate_vertex_offset());
	memdelete(inst);
}
#endif // _3D_DISABLED
