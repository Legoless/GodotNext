/**************************************************************************/
/*  test_depth_offset.cpp                                                 */
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

TEST_FORCE_LINK(test_depth_offset)

// GodotNext: depth_offset (#149, plan §21.4 contract N6, GN-3, GN-12).
// Covers DEPTH_OFFSET spatial fragment built-in parsing, mutual-exclusion compile error with DEPTH,
// uses_depth_offset & RASTER_FEATURE_DEPTH_OFFSET admission/fallback & VSM dynamic flags,
// Forward+ / Forward Mobile conservative depth SPIR-V execution modes, Compatibility (GLES3)
// plain gl_FragDepth compilation, and NextGeo SW / HW VS / HW Mesh / VSM raster target SPIR-V.

#include "servers/rendering/renderer_rd/forward_clustered/megalights_inputs.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_raster_bins.h"
#include "servers/rendering/renderer_rd/shaders/forward_clustered/scene_forward_clustered.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/forward_mobile/scene_forward_mobile.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/raster_material_hw.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/raster_material_hw_mesh.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/raster_material_sw.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/resolve_compute.glsl.gen.h"
#include "servers/rendering/rendering_shader_container.h"
#include "servers/rendering/shader_compiler.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"
#include "servers/rendering/storage/virtual_mesh_storage.h"
#include "servers/rendering/storage/virtual_shadow_map_storage.h"

#ifdef GLES3_ENABLED
#include "drivers/gles3/shaders/scene.glsl.gen.h"
#endif

#include "modules/modules_enabled.gen.h"

#ifdef MODULE_GLSLANG_ENABLED
#include "modules/glslang/shader_compile.h"
#endif
#ifdef VULKAN_ENABLED
#include "drivers/vulkan/rendering_shader_container_vulkan.h"
#endif
#ifdef METAL_ENABLED
#include "drivers/metal/rendering_shader_container_metal.h"
#endif
#ifdef D3D12_ENABLED
#include "drivers/d3d12/rendering_shader_container_d3d12.h"
#endif

using namespace RendererRD;
using RDC = RenderingDeviceCommons;

namespace TestDepthOffset {

constexpr uint32_t SPIRV_MAGIC = 0x07230203u;
constexpr uint32_t SPIRV_OP_EXECUTION_MODE = 16u;
constexpr uint32_t SPIRV_EXEC_MODE_EARLY_FRAGMENT_TESTS = 9u;
constexpr uint32_t SPIRV_EXEC_MODE_DEPTH_REPLACING = 12u;
constexpr uint32_t SPIRV_EXEC_MODE_DEPTH_GREATER = 14u;
constexpr uint32_t SPIRV_EXEC_MODE_DEPTH_LESS = 15u;

bool spirv_has_execution_mode(const Vector<uint8_t> &p_spirv, uint32_t p_mode) {
	if (p_spirv.size() < 20 || (p_spirv.size() % 4) != 0) {
		return false;
	}
	const uint32_t *words = reinterpret_cast<const uint32_t *>(p_spirv.ptr());
	const size_t word_count = size_t(p_spirv.size()) / 4u;
	if (words[0] != SPIRV_MAGIC) {
		return false;
	}
	size_t i = 5u;
	while (i < word_count) {
		const uint32_t wc = words[i] >> 16u;
		const uint32_t op = words[i] & 0xFFFFu;
		if (wc == 0u || i + wc > word_count) {
			break;
		}
		if (op == SPIRV_OP_EXECUTION_MODE && wc >= 3u && words[i + 2u] == p_mode) {
			return true;
		}
		i += wc;
	}
	return false;
}

Error parse_spatial(const String &p_code, String *r_error = nullptr) {
	if (ShaderTypes::get_singleton() == nullptr) {
		memnew(ShaderTypes);
	}
	ShaderLanguage sl;
	ShaderLanguage::ShaderCompileInfo info;
	info.functions = ShaderTypes::get_singleton()->get_functions(RSE::SHADER_SPATIAL);
	info.render_modes = ShaderTypes::get_singleton()->get_modes(RSE::SHADER_SPATIAL);
	info.stencil_modes = ShaderTypes::get_singleton()->get_stencil_modes(RSE::SHADER_SPATIAL);
	info.shader_types = ShaderTypes::get_singleton()->get_types();
	const Error err = sl.compile(p_code, info);
	if (r_error != nullptr) {
		*r_error = sl.get_error_text();
	}
	return err;
}

struct CompileOutput {
	Error err = FAILED;
	ShaderCompiler::GeneratedCode gen;
	HashMap<StringName, ShaderLanguage::ShaderNode::Uniform> uniforms;
	bool uses_depth_offset = false;
	bool writes_depth = false;
	bool alpha = false;
	bool alpha_scissor = false;
	bool discard = false;
	bool writes_vertex = false;
	int cull_mode = RSE::CULL_MODE_BACK;
};

void init_forward_clustered_compiler(ShaderCompiler &r_compiler) {
	ShaderCompiler::DefaultIdentifierActions actions;

	actions.renames["MODEL_MATRIX"] = "read_model_matrix";
	actions.renames["MODEL_NORMAL_MATRIX"] = "model_normal_matrix";
	actions.renames["VIEW_MATRIX"] = "read_view_matrix";
	actions.renames["INV_VIEW_MATRIX"] = "inv_view_matrix";
	actions.renames["PROJECTION_MATRIX"] = "projection_matrix";
	actions.renames["INV_PROJECTION_MATRIX"] = "inv_projection_matrix";
	actions.renames["MODELVIEW_MATRIX"] = "modelview";
	actions.renames["MODELVIEW_NORMAL_MATRIX"] = "modelview_normal";
	actions.renames["VERTEX"] = "vertex";
	actions.renames["NORMAL"] = "normal_highp";
	actions.renames["TANGENT"] = "tangent";
	actions.renames["BINORMAL"] = "binormal";
	actions.renames["POSITION"] = "position";
	actions.renames["UV"] = "uv_interp";
	actions.renames["UV2"] = "uv2_interp";
	actions.renames["COLOR"] = "color_interp";
	actions.renames["TIME"] = "global_time";
	actions.renames["ALBEDO"] = "albedo_highp";
	actions.renames["ALPHA"] = "alpha_highp";
	actions.renames["METALLIC"] = "metallic_highp";
	actions.renames["SPECULAR"] = "specular";
	actions.renames["ROUGHNESS"] = "roughness_highp";
	actions.renames["ALPHA_SCISSOR_THRESHOLD"] = "alpha_scissor_threshold";
	actions.renames["ALPHA_HASH_SCALE"] = "alpha_hash_scale";
	actions.renames["DEPTH"] = "gl_FragDepth";
	actions.renames["DEPTH_OFFSET"] = "depth_offset";
	actions.renames["FRAGCOORD"] = "gl_FragCoord";
	actions.renames["FRONT_FACING"] = "gl_FrontFacing";
	actions.renames["SCREEN_UV"] = "screen_uv";
	actions.renames["VIEW"] = "view_highp";

	actions.usage_defines["NORMAL"] = "#define NORMAL_USED\n";
	actions.usage_defines["UV"] = "#define UV_USED\n";
	actions.usage_defines["COLOR"] = "#define COLOR_USED\n";
	actions.usage_defines["ALPHA_SCISSOR_THRESHOLD"] = "#define ALPHA_SCISSOR_USED\n";
	actions.usage_defines["ALPHA_HASH_SCALE"] = "#define ALPHA_HASH_USED\n";
	actions.usage_defines["DISCARD"] = "#define DISCARD_USED\n";
	actions.usage_defines["DEPTH"] = "#define DEPTH_USED\n";
	actions.usage_defines["DEPTH_OFFSET"] = "#define DEPTH_OFFSET_USED\n";
	actions.render_mode_defines["diffuse_burley"] = "#define DIFFUSE_BURLEY\n";
	actions.render_mode_defines["specular_schlick_ggx"] = "#define SPECULAR_SCHLICK_GGX\n";

	actions.base_texture_binding_index = 1;
	actions.texture_layout_set = 3;
	actions.base_uniform_string = "material.";
	actions.base_varying_index = 15;
	actions.default_filter = ShaderLanguage::FILTER_LINEAR_MIPMAP;
	actions.default_repeat = ShaderLanguage::REPEAT_ENABLE;
	actions.global_buffer_array_variable = "global_shader_uniforms.data";
	actions.instance_uniform_index_variable = "instances.data[instance_index_interp].instance_uniforms_ofs";

	actions.resolve_builtin_derivatives["VERTEX"] = Pair<String, String>("vertex_interp_ddx", "vertex_interp_ddy");
	actions.resolve_builtin_derivatives["UV"] = Pair<String, String>("uv_interp_ddx", "uv_interp_ddy");
	actions.resolve_builtin_derivatives["COLOR"] = Pair<String, String>("color_interp_ddx", "color_interp_ddy");
	actions.resolve_builtin_derivatives["VIEW"] = Pair<String, String>("view_ddx", "view_ddy");
	actions.resolve_builtin_derivatives["FRAGCOORD"] = Pair<String, String>("_ng_fragcoord_ddx", "_ng_fragcoord_ddy");
	actions.resolve_mutable_builtin_derivatives["NORMAL"] = Pair<String, String>("normal_interp_ddx", "normal_interp_ddy");
	actions.resolve_uniform_builtins.insert("TIME");
	actions.resolve_renames["FRONT_FACING"] = "_ng_front_facing";
	actions.resolve_renames["FRAGCOORD"] = "_ng_fragcoord";
	actions.resolve_vertex_renames["VERTEX_ID"] = "_ng_vertex_id";

	r_compiler.initialize(actions);
}

void init_forward_mobile_compiler(ShaderCompiler &r_compiler) {
	ShaderCompiler::DefaultIdentifierActions actions;

	actions.renames["MODEL_MATRIX"] = "read_model_matrix";
	actions.renames["MODEL_NORMAL_MATRIX"] = "model_normal_matrix";
	actions.renames["VIEW_MATRIX"] = "read_view_matrix";
	actions.renames["INV_VIEW_MATRIX"] = "inv_view_matrix";
	actions.renames["PROJECTION_MATRIX"] = "projection_matrix";
	actions.renames["INV_PROJECTION_MATRIX"] = "inv_projection_matrix";
	actions.renames["MODELVIEW_MATRIX"] = "modelview";
	actions.renames["MODELVIEW_NORMAL_MATRIX"] = "modelview_normal";
	actions.renames["VERTEX"] = "vertex";
	actions.renames["NORMAL"] = "normal_highp";
	actions.renames["UV"] = "uv_interp";
	actions.renames["COLOR"] = "color_highp";
	actions.renames["TIME"] = "scene_data_block.data.time";
	actions.renames["ALBEDO"] = "albedo_highp";
	actions.renames["ALPHA"] = "alpha_highp";
	actions.renames["ROUGHNESS"] = "roughness_highp";
	actions.renames["ALPHA_SCISSOR_THRESHOLD"] = "alpha_scissor_threshold";
	actions.renames["DEPTH"] = "gl_FragDepth";
	actions.renames["DEPTH_OFFSET"] = "depth_offset";
	actions.renames["FRAGCOORD"] = "gl_FragCoord";
	actions.renames["FRONT_FACING"] = "gl_FrontFacing";
	actions.renames["SCREEN_UV"] = "screen_uv";
	actions.renames["VIEW"] = "view_highp";

	actions.usage_defines["NORMAL"] = "#define NORMAL_USED\n";
	actions.usage_defines["UV"] = "#define UV_USED\n";
	actions.usage_defines["ALPHA_SCISSOR_THRESHOLD"] = "#define ALPHA_SCISSOR_USED\n";
	actions.usage_defines["DISCARD"] = "#define DISCARD_USED\n";
	actions.usage_defines["DEPTH"] = "#define DEPTH_USED\n";
	actions.usage_defines["DEPTH_OFFSET"] = "#define DEPTH_OFFSET_USED\n";
	actions.render_mode_defines["diffuse_burley"] = "#define DIFFUSE_BURLEY\n";
	actions.render_mode_defines["specular_schlick_ggx"] = "#define SPECULAR_SCHLICK_GGX\n";

	actions.base_texture_binding_index = 1;
	actions.texture_layout_set = 3;
	actions.base_uniform_string = "material.";
	actions.base_varying_index = 15;
	actions.default_filter = ShaderLanguage::FILTER_LINEAR_MIPMAP;
	actions.default_repeat = ShaderLanguage::REPEAT_ENABLE;
	actions.global_buffer_array_variable = "global_shader_uniforms.data";
	actions.instance_uniform_index_variable = "instances.data[draw_call.instance_index].instance_uniforms_ofs";
	actions.apply_luminance_multiplier = true;
	actions.check_multiview_samplers = true;

	r_compiler.initialize(actions);
}

void init_gles3_compiler(ShaderCompiler &r_compiler) {
	ShaderCompiler::DefaultIdentifierActions actions;

	actions.renames["MODEL_MATRIX"] = "model_matrix";
	actions.renames["MODEL_NORMAL_MATRIX"] = "model_normal_matrix";
	actions.renames["VIEW_MATRIX"] = "scene_data_block.data.view_matrix";
	actions.renames["INV_VIEW_MATRIX"] = "scene_data_block.data.inv_view_matrix";
	actions.renames["PROJECTION_MATRIX"] = "projection_matrix";
	actions.renames["INV_PROJECTION_MATRIX"] = "inv_projection_matrix";
	actions.renames["MODELVIEW_MATRIX"] = "modelview";
	actions.renames["MODELVIEW_NORMAL_MATRIX"] = "modelview_normal";
	actions.renames["VERTEX"] = "vertex";
	actions.renames["NORMAL"] = "normal";
	actions.renames["UV"] = "uv_interp";
	actions.renames["COLOR"] = "color_interp";
	actions.renames["TIME"] = "scene_data_block.data.time";
	actions.renames["ALBEDO"] = "albedo";
	actions.renames["ALPHA"] = "alpha";
	actions.renames["ROUGHNESS"] = "roughness";
	actions.renames["ALPHA_SCISSOR_THRESHOLD"] = "alpha_scissor_threshold";
	actions.renames["DEPTH"] = "gl_FragDepth";
	actions.renames["DEPTH_OFFSET"] = "depth_offset";
	actions.renames["FRAGCOORD"] = "gl_FragCoord";
	actions.renames["FRONT_FACING"] = "gl_FrontFacing";
	actions.renames["SCREEN_UV"] = "screen_uv";
	actions.renames["VIEW"] = "view";

	actions.usage_defines["NORMAL"] = "#define NORMAL_USED\n";
	actions.usage_defines["UV"] = "#define UV_USED\n";
	actions.usage_defines["ALPHA_SCISSOR_THRESHOLD"] = "#define ALPHA_SCISSOR_USED\n";
	actions.usage_defines["DISCARD"] = "#define DISCARD_USED\n";
	actions.usage_defines["DEPTH"] = "#define DEPTH_USED\n";
	actions.usage_defines["DEPTH_OFFSET"] = "#define DEPTH_OFFSET_USED\n";

	actions.default_filter = ShaderLanguage::FILTER_LINEAR_MIPMAP;
	actions.default_repeat = ShaderLanguage::REPEAT_ENABLE;
	actions.apply_luminance_multiplier = true;
	actions.global_buffer_array_variable = "global_shader_uniforms";
	actions.global_buffer_array_is_texture = true;
	actions.instance_uniform_index_variable = "instance_offset";

	r_compiler.initialize(actions);
}

CompileOutput compile_with(ShaderCompiler &p_compiler, const String &p_code, bool p_visibility_resolve, bool p_raster_target) {
	if (ShaderTypes::get_singleton() == nullptr) {
		memnew(ShaderTypes);
	}
	CompileOutput out;
	ShaderCompiler::IdentifierActions ia;
	ia.entry_point_stages["vertex"] = ShaderCompiler::STAGE_VERTEX;
	ia.entry_point_stages["fragment"] = ShaderCompiler::STAGE_FRAGMENT;
	ia.entry_point_stages["light"] = ShaderCompiler::STAGE_FRAGMENT;
	ia.uniforms = &out.uniforms;
	ia.usage_flag_pointers["ALPHA"] = &out.alpha;
	ia.usage_flag_pointers["ALPHA_SCISSOR_THRESHOLD"] = &out.alpha_scissor;
	ia.usage_flag_pointers["DISCARD"] = &out.discard;
	ia.write_flag_pointers["VERTEX"] = &out.writes_vertex;
	ia.write_flag_pointers["DEPTH"] = &out.writes_depth;
	ia.write_flag_pointers["DEPTH_OFFSET"] = &out.uses_depth_offset;
	ia.render_mode_values["cull_disabled"] = Pair<int *, int>(&out.cull_mode, RSE::CULL_MODE_DISABLED);
	ia.render_mode_values["cull_front"] = Pair<int *, int>(&out.cull_mode, RSE::CULL_MODE_FRONT);
	ia.visibility_resolve = p_visibility_resolve;
	ia.raster_target = p_raster_target;
	out.err = p_compiler.compile(RSE::SHADER_SPATIAL, p_code, &ia, "test_depth_offset.gdshader", out.gen);
	return out;
}

TEST_CASE("[DepthOffset] DEPTH_OFFSET built-in registration and mutual exclusion with DEPTH") {
	if (ShaderTypes::get_singleton() == nullptr) {
		memnew(ShaderTypes);
	}
	const HashMap<StringName, ShaderLanguage::FunctionInfo> &spatial_funcs = ShaderTypes::get_singleton()->get_functions(RSE::SHADER_SPATIAL);
	REQUIRE(spatial_funcs.has("fragment"));
	REQUIRE(spatial_funcs["fragment"].built_ins.has("DEPTH_OFFSET"));
	CHECK_EQ(spatial_funcs["fragment"].built_ins["DEPTH_OFFSET"].type, ShaderLanguage::TYPE_FLOAT);
	CHECK_FALSE(spatial_funcs["vertex"].built_ins.has("DEPTH_OFFSET"));
	CHECK_FALSE(spatial_funcs["light"].built_ins.has("DEPTH_OFFSET"));

	// Writing DEPTH_OFFSET alone succeeds.
	String err_text;
	CHECK_EQ(parse_spatial(R"(
shader_type spatial;
void fragment() {
	ALBEDO = vec3(1.0);
	DEPTH_OFFSET = 0.25;
}
)",
					 &err_text),
			OK);

	// Reading DEPTH while writing DEPTH_OFFSET succeeds (not a conflicting write).
	CHECK_EQ(parse_spatial(R"(
shader_type spatial;
void fragment() {
	float d = DEPTH;
	DEPTH_OFFSET = max(0.0, d * 0.1);
}
)",
					 &err_text),
			OK);

	// Writing DEPTH alone while reading DEPTH_OFFSET succeeds (not a conflicting write).
	CHECK_EQ(parse_spatial(R"(
shader_type spatial;
void fragment() {
	float d = DEPTH_OFFSET;
	DEPTH = clamp(d, 0.0, 1.0);
}
)",
					 &err_text),
			OK);

	// Writing BOTH DEPTH and DEPTH_OFFSET in any form must fail with a clear diagnostic.
	const char *conflict_cases[] = {
		// Direct assignment.
		R"(
shader_type spatial;
void fragment() {
	DEPTH = 0.5;
	DEPTH_OFFSET = 0.1;
}
)",
		// Reversed order + compound assignment.
		R"(
shader_type spatial;
void fragment() {
	DEPTH_OFFSET = 0.1;
	DEPTH += 0.2;
}
)",
		// Compound assignment on DEPTH_OFFSET.
		R"(
shader_type spatial;
void fragment() {
	DEPTH = 0.5;
	DEPTH_OFFSET += 0.1;
}
)",
		// Increment operator on DEPTH_OFFSET.
		R"(
shader_type spatial;
void fragment() {
	DEPTH = 0.5;
	DEPTH_OFFSET++;
}
)",
		// Prefix decrement on DEPTH.
		R"(
shader_type spatial;
void fragment() {
	--DEPTH;
	DEPTH_OFFSET = 0.2;
}
)",
		// User helper out / inout parameters.
		R"(
shader_type spatial;
void set_val(out float x) { x = 0.2; }
void adj_val(inout float x) { x += 0.1; }
void fragment() {
	set_val(DEPTH);
	adj_val(DEPTH_OFFSET);
}
)",
		// Built-in out parameter (modf).
		R"(
shader_type spatial;
void fragment() {
	float f = modf(1.5, DEPTH_OFFSET);
	DEPTH = f;
}
)",
	};

	for (const char *code : conflict_cases) {
		err_text.clear();
		const Error err = parse_spatial(code, &err_text);
		CHECK_EQ(err, ERR_PARSE_ERROR);
		CHECK_MESSAGE(err_text.contains("DEPTH"), err_text);
		CHECK_MESSAGE(err_text.contains("DEPTH_OFFSET"), err_text);
	}
}

TEST_CASE("[DepthOffset] uses_depth_offset write flag, RASTER_FEATURE_DEPTH_OFFSET, fallback admission, and VSM dynamic flag") {
	ShaderCompiler compiler;
	init_forward_clustered_compiler(compiler);

	// Reading DEPTH_OFFSET without writing does NOT set uses_depth_offset.
	{
		const CompileOutput out = compile_with(compiler, R"(
shader_type spatial;
void fragment() {
	ALBEDO = vec3(DEPTH_OFFSET);
}
)",
				true, true);
		REQUIRE_EQ(out.err, OK);
		CHECK_FALSE(out.uses_depth_offset);
	}

	// Direct write, compound write, increment, and out-parameter write all set uses_depth_offset.
	const char *write_cases[] = {
		"shader_type spatial; void fragment() { DEPTH_OFFSET = 0.15; }",
		"shader_type spatial; void fragment() { DEPTH_OFFSET += 0.15; }",
		"shader_type spatial; void fragment() { ++DEPTH_OFFSET; }",
		"shader_type spatial; void fragment() { DEPTH_OFFSET++; }",
		"shader_type spatial; void fill_pdo(out float d) { d = 0.2; } void fragment() { fill_pdo(DEPTH_OFFSET); }",
	};
	for (const char *code : write_cases) {
		const CompileOutput out = compile_with(compiler, code, true, true);
		REQUIRE_EQ(out.err, OK);
		CHECK_MESSAGE(out.uses_depth_offset, String(code));
		CHECK(out.gen.defines.has("#define DEPTH_OFFSET_USED\n"));
		CHECK(out.gen.code["fragment"].contains("depth_offset"));
		CHECK(out.gen.code["raster_fragment"].contains("depth_offset"));
	}

	// Classification, default Nanite admission, VSM bin resolution, and VSM dynamic flag.
	using VMS = RendererVirtualMeshStorage;
	using VSM = RendererVirtualShadowMapStorage;

	NextGeoRasterFacts rf;
	rf.depth_offset = true;
	const uint32_t feats = nextgeo_raster_features(rf);
	CHECK_EQ(feats, uint32_t(VMS::RASTER_FEATURE_DEPTH_OFFSET));
	CHECK(nextgeo_raster_has_program(feats));
	CHECK_EQ(VMS::resolve_raster_bin(feats, VMS::RASTER_PASS_VISIBILITY), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));
	CHECK_EQ(VMS::resolve_raster_bin(feats, VMS::RASTER_PASS_VSM), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));
	CHECK_EQ(VMS::resolve_raster_bin(feats | VMS::RASTER_FEATURE_MASKED | VMS::RASTER_FEATURE_TWO_SIDED, VMS::RASTER_PASS_VISIBILITY), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));
	CHECK_EQ(VMS::resolve_raster_bin(feats | VMS::RASTER_FEATURE_MASKED | VMS::RASTER_FEATURE_TWO_SIDED, VMS::RASTER_PASS_VSM), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));
	CHECK(nextgeo_raster_bin_can_demote_by_distance(feats));
	CHECK(nextgeo_raster_bin_can_demote_by_distance(feats | VMS::RASTER_FEATURE_MASKED | VMS::RASTER_FEATURE_TWO_SIDED));
	CHECK_FALSE(nextgeo_raster_bin_can_demote_by_distance(feats | VMS::RASTER_FEATURE_VERTEX_OFFSET));

	// Fallback admission: non-hybrid falls back to UNSUPPORTED_CUSTOM_SHADER; Nanite default (admit_depth_offset=true) admits.
	NextGeoMaterialFallbackFacts ff;
	ff.depth_offset = true;
	CHECK_EQ(nextgeo_material_fallback(ff), NEXTGEO_FALLBACK_UNSUPPORTED_CUSTOM_SHADER);
	ff.admit_depth_offset = true;
	CHECK_EQ(nextgeo_material_fallback(ff), NEXTGEO_FALLBACK_NONE);
	// Rejects (FRAGCOORD / screen reads) still reject to UNSUPPORTED_RASTER_FEATURE.
	ff.raster_rejects = ShaderCompiler::RASTER_USES_FRAGCOORD;
	CHECK_EQ(nextgeo_material_fallback(ff), NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE);
	ff.raster_rejects = 0u;
	// Does not silently admit vertex offset without admit_programmable.
	ff.writes_vertex = true;
	CHECK_EQ(nextgeo_material_fallback(ff), NEXTGEO_FALLBACK_UNSUPPORTED_CUSTOM_SHADER);
	ff.admit_programmable = true;
	CHECK_EQ(nextgeo_material_fallback(ff), NEXTGEO_FALLBACK_NONE);
	// Transparent PDO stays ALPHA fallback.
	ff.transparent = true;
	CHECK_EQ(nextgeo_material_fallback(ff), NEXTGEO_FALLBACK_ALPHA);

	// RASTER_FEATURE_DEPTH_OFFSET maps to VSM CACHE_DYNAMIC_DEPTH (matching SceneShaderForwardClustered::get_vsm_dynamic_flags).
	const uint32_t dynamic_flag = ((feats & VMS::RASTER_FEATURE_DEPTH_OFFSET) != 0u) ? uint32_t(VSM::CACHE_DYNAMIC_DEPTH) : 0u;
	CHECK_EQ(dynamic_flag, uint32_t(VSM::CACHE_DYNAMIC_DEPTH));
}

TEST_CASE("[DepthOffset] Forward+, Forward Mobile, and Compatibility (GLES3) compile cleanly with conservative depth on RD") {
	const String pdo_shader = R"(
shader_type spatial;
uniform sampler2D depth_tex;
uniform float scale = 0.35;
void fragment() {
	ALBEDO = vec3(0.7, 0.5, 0.3);
	ROUGHNESS = 0.6;
	DEPTH_OFFSET = texture(depth_tex, UV).r * scale;
}
)";

	// 1. Forward+ (SceneForwardClusteredShaderRD + ResolveComputeShaderRD).
	{
		ShaderCompiler clustered_compiler;
		init_forward_clustered_compiler(clustered_compiler);
		CompileOutput res = compile_with(clustered_compiler, pdo_shader, true, true);
		REQUIRE_EQ(res.err, OK);
		CHECK(res.uses_depth_offset);
		CHECK(res.gen.defines.has("#define DEPTH_OFFSET_USED\n"));

		String clustered_defines = "\n#define MAX_ROUGHNESS_LOD 7.0\n#define SDFGI_OCT_SIZE 8\n#define MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS 8\n#define MAX_LIGHTMAP_TEXTURES 8\n#define MAX_LIGHTMAPS 8\n#define MATERIAL_UNIFORM_SET 3\n";
		clustered_defines += RendererVirtualShadowMapStorage::get_glsl_defines();
		clustered_defines += String("\n") + RendererRD::MegaLightsInputs::glsl_pack_prelude();

		SceneForwardClusteredShaderRD clustered_shader;
		Vector<ShaderRD::VariantDefine> variants;
		variants.push_back(ShaderRD::VariantDefine(0, "", false)); // 0: regular color pass
		variants.push_back(ShaderRD::VariantDefine(1, "\n#define MODE_RENDER_DEPTH\n", false)); // 1: depth prepass / shadow
		variants.push_back(ShaderRD::VariantDefine(2, "\n#define MODE_RENDER_DEPTH\n#define MODE_DUAL_PARABOLOID\n", false)); // 2: dual paraboloid shadow
		variants.push_back(ShaderRD::VariantDefine(3, "\n#define MODE_RENDER_DEPTH\n#define MODE_VSM\n", false)); // 3: Forward+ non-Nanite VSM
		variants.push_back(ShaderRD::VariantDefine(4, "\n#define MODE_VISIBILITY_RESOLVE\n", false)); // 4: NextGeo tile-quad resolve
		clustered_shader.initialize(variants, clustered_defines);

		res.gen.code.erase("raster_fragment");
		const RID ver = clustered_shader.version_create(false);
		REQUIRE(ver.is_valid());
		clustered_shader.version_set_code(ver, res.gen.code, res.gen.uniforms, res.gen.stage_globals[ShaderCompiler::STAGE_VERTEX], res.gen.stage_globals[ShaderCompiler::STAGE_FRAGMENT], res.gen.defines);

		for (int v = 0; v < variants.size(); v++) {
			const Vector<String> stage_src = clustered_shader.version_build_variant_stage_sources(ver, v);
			REQUIRE_EQ(stage_src.size(), int(RD::SHADER_STAGE_MAX));
			const String &fs_src = stage_src[RD::SHADER_STAGE_FRAGMENT];
			CHECK(fs_src.contains("depth_offset_clamped"));
			if (v == 3) {
				CHECK(fs_src.contains("vsm_depth += depth_offset_clamped"));
			}
#if defined(MODULE_GLSLANG_ENABLED) && defined(VULKAN_ENABLED)
			RenderingShaderContainerFormatVulkan vk_format;
			String err;
			RDC::ShaderStageSPIRVData vs;
			vs.shader_stage = RDC::SHADER_STAGE_VERTEX;
			vs.spirv = compile_glslang_shader(RDC::SHADER_STAGE_VERTEX, stage_src[RD::SHADER_STAGE_VERTEX], vk_format.get_shader_language_version(), vk_format.get_shader_spirv_version(RDC::SHADER_STAGE_VERTEX), &err);
			CHECK_MESSAGE(!vs.spirv.is_empty(), err);

			RDC::ShaderStageSPIRVData fs;
			fs.shader_stage = RDC::SHADER_STAGE_FRAGMENT;
			fs.spirv = compile_glslang_shader(RDC::SHADER_STAGE_FRAGMENT, fs_src, vk_format.get_shader_language_version(), vk_format.get_shader_spirv_version(RDC::SHADER_STAGE_FRAGMENT), &err);
			CHECK_MESSAGE(!fs.spirv.is_empty(), err);

			const bool expect_depth_less = (v == 0 || v == 1 || v == 2);
			CHECK_EQ(spirv_has_execution_mode(fs.spirv, SPIRV_EXEC_MODE_DEPTH_LESS), expect_depth_less);
			CHECK_EQ(spirv_has_execution_mode(fs.spirv, SPIRV_EXEC_MODE_DEPTH_REPLACING), expect_depth_less);
			CHECK_FALSE(spirv_has_execution_mode(fs.spirv, SPIRV_EXEC_MODE_DEPTH_GREATER));
			if (expect_depth_less) {
				CHECK_FALSE(spirv_has_execution_mode(fs.spirv, SPIRV_EXEC_MODE_EARLY_FRAGMENT_TESTS));
			}

			Ref<RenderingShaderContainer> container = vk_format.create_container();
			Vector<RDC::ShaderStageSPIRVData> stages = { vs, fs };
			CHECK(container->set_code_from_spirv("ForwardClusteredPDO_" + itos(v), stages));
#endif
#if defined(MODULE_GLSLANG_ENABLED) && defined(METAL_ENABLED)
			if (v == 0) {
				MetalDeviceProfile profile;
				profile.platform = MetalDeviceProfile::Platform::macOS;
				profile.gpu = MetalDeviceProfile::GPU::Apple9;
				profile.features.msl_version = 30100;
				profile.features.use_argument_buffers = true;
				RenderingShaderContainerFormatMetal mtl_format(&profile, true);
				String merr;
				RDC::ShaderStageSPIRVData mvs;
				mvs.shader_stage = RDC::SHADER_STAGE_VERTEX;
				mvs.spirv = compile_glslang_shader(RDC::SHADER_STAGE_VERTEX, stage_src[RD::SHADER_STAGE_VERTEX], mtl_format.get_shader_language_version(), mtl_format.get_shader_spirv_version(RDC::SHADER_STAGE_VERTEX), &merr);
				CHECK_MESSAGE(!mvs.spirv.is_empty(), merr);
				RDC::ShaderStageSPIRVData mfs;
				mfs.shader_stage = RDC::SHADER_STAGE_FRAGMENT;
				mfs.spirv = compile_glslang_shader(RDC::SHADER_STAGE_FRAGMENT, fs_src, mtl_format.get_shader_language_version(), mtl_format.get_shader_spirv_version(RDC::SHADER_STAGE_FRAGMENT), &merr);
				CHECK_MESSAGE(!mfs.spirv.is_empty(), merr);
				Ref<RenderingShaderContainer> mcontainer = mtl_format.create_container();
				Vector<RDC::ShaderStageSPIRVData> mstages = { mvs, mfs };
				CHECK(mcontainer->set_code_from_spirv("ForwardClusteredPDOMetal", mstages));
			}
#endif
#if defined(MODULE_GLSLANG_ENABLED) && defined(D3D12_ENABLED)
			if (v == 0) {
				RenderingShaderContainerFormatD3D12 d3d_format;
				String derr;
				RDC::ShaderStageSPIRVData dvs;
				dvs.shader_stage = RDC::SHADER_STAGE_VERTEX;
				dvs.spirv = compile_glslang_shader(RDC::SHADER_STAGE_VERTEX, stage_src[RD::SHADER_STAGE_VERTEX], d3d_format.get_shader_language_version(), d3d_format.get_shader_spirv_version(RDC::SHADER_STAGE_VERTEX), &derr);
				CHECK_MESSAGE(!dvs.spirv.is_empty(), derr);
				RDC::ShaderStageSPIRVData dfs;
				dfs.shader_stage = RDC::SHADER_STAGE_FRAGMENT;
				dfs.spirv = compile_glslang_shader(RDC::SHADER_STAGE_FRAGMENT, fs_src, d3d_format.get_shader_language_version(), d3d_format.get_shader_spirv_version(RDC::SHADER_STAGE_FRAGMENT), &derr);
				CHECK_MESSAGE(!dfs.spirv.is_empty(), derr);
				Ref<RenderingShaderContainer> dcontainer = d3d_format.create_container();
				Vector<RDC::ShaderStageSPIRVData> dstages = { dvs, dfs };
				CHECK(dcontainer->set_code_from_spirv("ForwardClusteredPDOD3D12", dstages));
			}
#endif
		}
		CHECK(clustered_shader.version_free(ver));

		// NextGeo compute shading bin (ResolveComputeShaderRD) must also compile cleanly with DEPTH_OFFSET_USED.
		ResolveComputeShaderRD resolve_compute;
		Vector<ShaderRD::VariantDefine> compute_variants;
		compute_variants.push_back(ShaderRD::VariantDefine(0, "\n#define NEXTGEO_RESOLVE_COMPUTE\n#define MODE_VISIBILITY_RESOLVE\n", false));
		resolve_compute.initialize(compute_variants, clustered_defines);
		const RID cver = resolve_compute.version_create(false);
		REQUIRE(cver.is_valid());
		resolve_compute.version_set_compute_code(cver, res.gen.code, res.gen.uniforms, res.gen.stage_globals[ShaderCompiler::STAGE_FRAGMENT], res.gen.defines);
		const Vector<String> csrc = resolve_compute.version_build_variant_stage_sources(cver, 0);
		REQUIRE_EQ(csrc.size(), int(RD::SHADER_STAGE_MAX));
#if defined(MODULE_GLSLANG_ENABLED) && defined(VULKAN_ENABLED)
		{
			RenderingShaderContainerFormatVulkan vk_format;
			String err;
			RDC::ShaderStageSPIRVData cs;
			cs.shader_stage = RDC::SHADER_STAGE_COMPUTE;
			cs.spirv = compile_glslang_shader(RDC::SHADER_STAGE_COMPUTE, csrc[RD::SHADER_STAGE_COMPUTE], vk_format.get_shader_language_version(), vk_format.get_shader_spirv_version(RDC::SHADER_STAGE_COMPUTE), &err);
			CHECK_MESSAGE(!cs.spirv.is_empty(), err);
			Ref<RenderingShaderContainer> container = vk_format.create_container();
			Vector<RDC::ShaderStageSPIRVData> stages = { cs };
			CHECK(container->set_code_from_spirv("ResolveComputePDO", stages));
		}
#endif
		CHECK(resolve_compute.version_free(cver));
	}

	// 2. Forward Mobile (SceneForwardMobileShaderRD).
	{
		ShaderCompiler mobile_compiler;
		init_forward_mobile_compiler(mobile_compiler);
		const CompileOutput res = compile_with(mobile_compiler, pdo_shader, false, false);
		REQUIRE_EQ(res.err, OK);
		CHECK(res.uses_depth_offset);
		CHECK(res.gen.defines.has("#define DEPTH_OFFSET_USED\n"));

		const String mobile_defines = "\n#define MAX_ROUGHNESS_LOD 7.0\n#define MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS 8\n#define MAX_LIGHTMAP_TEXTURES 8\n#define MAX_LIGHTMAPS 8\n#define MATERIAL_UNIFORM_SET 3\n";
		SceneForwardMobileShaderRD mobile_shader;
		Vector<ShaderRD::VariantDefine> mob_variants;
		mob_variants.push_back(ShaderRD::VariantDefine(0, "", false));
		mob_variants.push_back(ShaderRD::VariantDefine(1, "\n#define MODE_RENDER_DEPTH\n", false));
		mob_variants.push_back(ShaderRD::VariantDefine(2, "\n#define MODE_RENDER_DEPTH\n#define MODE_DUAL_PARABOLOID\n", false));
		mobile_shader.initialize(mob_variants, mobile_defines);

		const RID ver = mobile_shader.version_create(false);
		REQUIRE(ver.is_valid());
		mobile_shader.version_set_code(ver, res.gen.code, res.gen.uniforms, res.gen.stage_globals[ShaderCompiler::STAGE_VERTEX], res.gen.stage_globals[ShaderCompiler::STAGE_FRAGMENT], res.gen.defines);
		for (int v = 0; v < mob_variants.size(); v++) {
			const Vector<String> stage_src = mobile_shader.version_build_variant_stage_sources(ver, v);
			REQUIRE_EQ(stage_src.size(), int(RD::SHADER_STAGE_MAX));
#if defined(MODULE_GLSLANG_ENABLED) && defined(VULKAN_ENABLED)
			RenderingShaderContainerFormatVulkan vk_format;
			String err;
			RDC::ShaderStageSPIRVData vs;
			vs.shader_stage = RDC::SHADER_STAGE_VERTEX;
			vs.spirv = compile_glslang_shader(RDC::SHADER_STAGE_VERTEX, stage_src[RD::SHADER_STAGE_VERTEX], vk_format.get_shader_language_version(), vk_format.get_shader_spirv_version(RDC::SHADER_STAGE_VERTEX), &err);
			CHECK_MESSAGE(!vs.spirv.is_empty(), err);

			RDC::ShaderStageSPIRVData fs;
			fs.shader_stage = RDC::SHADER_STAGE_FRAGMENT;
			fs.spirv = compile_glslang_shader(RDC::SHADER_STAGE_FRAGMENT, stage_src[RD::SHADER_STAGE_FRAGMENT], vk_format.get_shader_language_version(), vk_format.get_shader_spirv_version(RDC::SHADER_STAGE_FRAGMENT), &err);
			CHECK_MESSAGE(!fs.spirv.is_empty(), err);
			CHECK(spirv_has_execution_mode(fs.spirv, SPIRV_EXEC_MODE_DEPTH_LESS));
			CHECK(spirv_has_execution_mode(fs.spirv, SPIRV_EXEC_MODE_DEPTH_REPLACING));
			CHECK_FALSE(spirv_has_execution_mode(fs.spirv, SPIRV_EXEC_MODE_EARLY_FRAGMENT_TESTS));

			Ref<RenderingShaderContainer> container = vk_format.create_container();
			Vector<RDC::ShaderStageSPIRVData> stages = { vs, fs };
			CHECK(container->set_code_from_spirv("ForwardMobilePDO_" + itos(v), stages));
#endif
		}
		CHECK(mobile_shader.version_free(ver));
	}

	// 3. Compatibility (GLES3) compiler actions & SceneShaderGLES3 template expansion check.
	{
		ShaderCompiler gles3_compiler;
		init_gles3_compiler(gles3_compiler);
		const CompileOutput res = compile_with(gles3_compiler, pdo_shader, false, false);
		REQUIRE_EQ(res.err, OK);
		CHECK(res.uses_depth_offset);
		CHECK(res.gen.defines.has("#define DEPTH_OFFSET_USED\n"));
		CHECK_FALSE(res.gen.defines.has("#define DEPTH_USED\n"));
		CHECK(res.gen.code["fragment"].contains("depth_offset"));
#ifdef GLES3_ENABLED
		struct SceneShaderGLES3Test : public SceneShaderGLES3 {
			void init_templates_only() { _init(); }
		};
		SceneShaderGLES3Test gles3_shader;
		gles3_shader.init_templates_only();
		const RID gver = gles3_shader.version_create();
		REQUIRE(gver.is_valid());
		LocalVector<ShaderGLES3::TextureUniformData> tex_uniforms;
		gles3_shader.version_set_code(gver, res.gen.code, res.gen.uniforms, res.gen.stage_globals[ShaderCompiler::STAGE_VERTEX], res.gen.stage_globals[ShaderCompiler::STAGE_FRAGMENT], res.gen.defines, tex_uniforms, false);
		const RenderingServerTypes::ShaderNativeSourceCode native = gles3_shader.version_get_native_source_code(gver);
		REQUIRE_GT(native.versions.size(), 0);
		for (int v = 0; v < native.versions.size(); v++) {
			REQUIRE_EQ(native.versions[v].stages.size(), 2);
			const String &gles3_frag = native.versions[v].stages[1].code;
			CHECK(gles3_frag.contains("#define DEPTH_OFFSET_USED"));
			CHECK(gles3_frag.contains("depth_offset_clamped"));
			CHECK(gles3_frag.contains("max(pdo_depth, gl_FragCoord.z)"));
			CHECK(gles3_frag.contains("length(vertex_interp) + depth_offset_clamped"));
			CHECK_FALSE(gles3_frag.contains("depth_less"));
		}
		CHECK(gles3_shader.version_free(gver));
#endif
	}
}

TEST_CASE("[DepthOffset] NextGeo SW, HW VS, HW Mesh, and VSM raster targets compile to SPIR-V") {
	ShaderCompiler compiler;
	init_forward_clustered_compiler(compiler);

	const char *cases[] = {
		// Plain texture-driven PDO (exercises textureGrad lowering in raster target).
		R"(
shader_type spatial;
uniform sampler2D depth_tex;
uniform float scale = 0.4;
void fragment() {
	ALBEDO = vec3(0.8, 0.3, 0.2);
	DEPTH_OFFSET = texture(depth_tex, UV).r * scale;
}
)",
		// Combined PDO + MASKED alpha scissor + TWO_SIDED cull_disabled.
		R"(
shader_type spatial;
render_mode cull_disabled;
uniform sampler2D mask_depth_tex;
void fragment() {
	vec4 s = texture(mask_depth_tex, UV);
	ALBEDO = FRONT_FACING ? vec3(0.2, 0.8, 0.4) : vec3(0.8, 0.2, 0.4);
	ALPHA = s.a;
	ALPHA_SCISSOR_THRESHOLD = 0.5;
	DEPTH_OFFSET = s.r * 0.5;
}
)",
	};

	RasterMaterialSwShaderRD sw_shader;
	RasterMaterialHwShaderRD hw_shader;
	RasterMaterialHwMeshShaderRD mesh_shader;
	Vector<ShaderRD::VariantDefine> raster_variants;
	raster_variants.push_back(ShaderRD::VariantDefine(0, "", false));
	raster_variants.push_back(ShaderRD::VariantDefine(1, "\n#define VIS64_BUFFER\n", false));
	raster_variants.push_back(ShaderRD::VariantDefine(2, "\n#define NEXTGEO_RASTER_DEPTH_ONLY\n" + RendererVirtualShadowMapStorage::get_glsl_defines(), false));
	sw_shader.initialize(raster_variants);
	sw_shader.set_group_int64_atomics(0, true, false);
	sw_shader.set_group_int64_atomics(1, false, true);
	hw_shader.initialize(raster_variants);
	hw_shader.set_group_int64_atomics(0, true, false);
	hw_shader.set_group_int64_atomics(1, false, true);
	mesh_shader.initialize(raster_variants);
	mesh_shader.set_group_int64_atomics(0, true, false);
	mesh_shader.set_group_int64_atomics(1, false, true);

	for (int c = 0; c < 2; c++) {
		const CompileOutput res = compile_with(compiler, cases[c], true, true);
		REQUIRE_EQ(res.err, OK);
		CHECK(res.uses_depth_offset);
		REQUIRE(res.gen.code.has("raster_fragment"));
		CHECK(res.gen.code["raster_fragment"].contains("depth_offset"));
		CHECK(res.gen.code["raster_fragment"].contains("textureGrad("));

		HashMap<String, String> raster_code;
		raster_code["resolve_vertex"] = res.gen.code.has("resolve_vertex") ? res.gen.code["resolve_vertex"] : String();
		raster_code["resolve_vertex_store"] = res.gen.code.has("resolve_vertex_store") ? res.gen.code["resolve_vertex_store"] : String();
		raster_code["resolve_interpolate"] = res.gen.code.has("resolve_interpolate") ? res.gen.code["resolve_interpolate"] : String();
		raster_code["raster_fragment"] = res.gen.code["raster_fragment"];

		Vector<String> defines = res.gen.defines;
		defines.push_back("#define DO_SIDE_CHECK\n");

		const String &compute_globals = res.gen.stage_globals[ShaderCompiler::STAGE_COMPUTE];
		const RID sw_ver = sw_shader.version_create(false);
		const RID hw_ver = hw_shader.version_create(false);
		const RID mesh_ver = mesh_shader.version_create(false);
		REQUIRE(sw_ver.is_valid());
		REQUIRE(hw_ver.is_valid());
		REQUIRE(mesh_ver.is_valid());
		sw_shader.version_set_compute_code(sw_ver, raster_code, res.gen.uniforms, compute_globals, defines);
		hw_shader.version_set_code(hw_ver, raster_code, res.gen.uniforms, compute_globals, compute_globals, defines);
		mesh_shader.version_set_code(mesh_ver, raster_code, res.gen.uniforms, compute_globals, compute_globals, defines);

		for (int v = 0; v < 3; v++) {
			const Vector<String> sw_src = sw_shader.version_build_variant_stage_sources(sw_ver, v);
			const Vector<String> hw_src = hw_shader.version_build_variant_stage_sources(hw_ver, v);
			const Vector<String> mesh_src = mesh_shader.version_build_variant_stage_sources(mesh_ver, v);
			REQUIRE_EQ(sw_src.size(), int(RD::SHADER_STAGE_MAX));
			REQUIRE_EQ(hw_src.size(), int(RD::SHADER_STAGE_MAX));
			REQUIRE_EQ(mesh_src.size(), int(RD::SHADER_STAGE_MAX));
			CHECK(sw_src[RD::SHADER_STAGE_COMPUTE].contains("_ng_apply_depth_offset"));
			CHECK(hw_src[RD::SHADER_STAGE_FRAGMENT].contains("_ng_apply_depth_offset"));
			CHECK(mesh_src[RD::SHADER_STAGE_FRAGMENT].contains("_ng_apply_depth_offset"));
#if defined(MODULE_GLSLANG_ENABLED) && defined(VULKAN_ENABLED)
			RenderingShaderContainerFormatVulkan vk_format;
			String err;

			// SW compute.
			RDC::ShaderStageSPIRVData sw_spv;
			sw_spv.shader_stage = RDC::SHADER_STAGE_COMPUTE;
			sw_spv.spirv = compile_glslang_shader(RDC::SHADER_STAGE_COMPUTE, sw_src[RD::SHADER_STAGE_COMPUTE], vk_format.get_shader_language_version(), vk_format.get_shader_spirv_version(RDC::SHADER_STAGE_COMPUTE), &err);
			CHECK_MESSAGE(!sw_spv.spirv.is_empty(), err);
			Ref<RenderingShaderContainer> sw_container = vk_format.create_container();
			CHECK(sw_container->set_code_from_spirv("RasterMaterialSwPDO", Vector<RDC::ShaderStageSPIRVData>({ sw_spv })));

			// HW VS + FS.
			RDC::ShaderStageSPIRVData hw_vs;
			hw_vs.shader_stage = RDC::SHADER_STAGE_VERTEX;
			hw_vs.spirv = compile_glslang_shader(RDC::SHADER_STAGE_VERTEX, hw_src[RD::SHADER_STAGE_VERTEX], vk_format.get_shader_language_version(), vk_format.get_shader_spirv_version(RDC::SHADER_STAGE_VERTEX), &err);
			CHECK_MESSAGE(!hw_vs.spirv.is_empty(), err);
			RDC::ShaderStageSPIRVData hw_fs;
			hw_fs.shader_stage = RDC::SHADER_STAGE_FRAGMENT;
			hw_fs.spirv = compile_glslang_shader(RDC::SHADER_STAGE_FRAGMENT, hw_src[RD::SHADER_STAGE_FRAGMENT], vk_format.get_shader_language_version(), vk_format.get_shader_spirv_version(RDC::SHADER_STAGE_FRAGMENT), &err);
			CHECK_MESSAGE(!hw_fs.spirv.is_empty(), err);
			Ref<RenderingShaderContainer> hw_container = vk_format.create_container();
			CHECK(hw_container->set_code_from_spirv("RasterMaterialHwPDO", Vector<RDC::ShaderStageSPIRVData>({ hw_vs, hw_fs })));

			// HW Task + Mesh + FS (SPIR-V 1.4).
			RDC::ShaderStageSPIRVData mesh_ts;
			mesh_ts.shader_stage = RDC::SHADER_STAGE_TASK;
			mesh_ts.spirv = compile_glslang_shader(RDC::SHADER_STAGE_TASK, mesh_src[RD::SHADER_STAGE_TASK], RDC::SHADER_LANGUAGE_VULKAN_VERSION_1_1, RDC::SHADER_SPIRV_VERSION_1_4, &err);
			CHECK_MESSAGE(!mesh_ts.spirv.is_empty(), err);
			RDC::ShaderStageSPIRVData mesh_ms;
			mesh_ms.shader_stage = RDC::SHADER_STAGE_MESH;
			mesh_ms.spirv = compile_glslang_shader(RDC::SHADER_STAGE_MESH, mesh_src[RD::SHADER_STAGE_MESH], RDC::SHADER_LANGUAGE_VULKAN_VERSION_1_1, RDC::SHADER_SPIRV_VERSION_1_4, &err);
			CHECK_MESSAGE(!mesh_ms.spirv.is_empty(), err);
			RDC::ShaderStageSPIRVData mesh_fs;
			mesh_fs.shader_stage = RDC::SHADER_STAGE_FRAGMENT;
			mesh_fs.spirv = compile_glslang_shader(RDC::SHADER_STAGE_FRAGMENT, mesh_src[RD::SHADER_STAGE_FRAGMENT], RDC::SHADER_LANGUAGE_VULKAN_VERSION_1_1, RDC::SHADER_SPIRV_VERSION_1_4, &err);
			CHECK_MESSAGE(!mesh_fs.spirv.is_empty(), err);
			Ref<RenderingShaderContainer> mesh_container = vk_format.create_container();
			CHECK(mesh_container->set_code_from_spirv("RasterMaterialHwMeshPDO", Vector<RDC::ShaderStageSPIRVData>({ mesh_ts, mesh_ms, mesh_fs })));
#endif
		}

		CHECK(sw_shader.version_free(sw_ver));
		CHECK(hw_shader.version_free(hw_ver));
		CHECK(mesh_shader.version_free(mesh_ver));
	}
}

} // namespace TestDepthOffset
