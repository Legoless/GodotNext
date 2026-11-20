/**************************************************************************/
/*  test_displacement_material.cpp                                        */
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

TEST_FORCE_LINK(test_displacement_material)

// GodotNext: nextgeo_displacement (#150, plan §21.4 N11).
// Covers spatial displacement() parsing & restrictions, ShaderCompiler textureGrad lowering and silent
// compile-out on non-resolve backends, Material displacement_magnitude/center & RenderingServer API,
// BaseMaterial3D heightmap_mode (PARALLAX unchanged vs DISPLACEMENT), RASTER_FEATURE_DISPLACED and
// cull bound inflation, and Vulkan/D3D12/Metal shader container compilation.

#include "scene/resources/canvas_item_material.h"
#include "scene/resources/material.h"
#include "scene/resources/shader.h"
#include "servers/rendering/renderer_rd/forward_clustered/megalights_inputs.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_raster_bins.h"
#include "servers/rendering/renderer_rd/shaders/forward_clustered/scene_forward_clustered.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/forward_mobile/scene_forward_mobile.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/raster_material_hw.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/raster_material_sw.glsl.gen.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/rendering_shader_container.h"
#include "servers/rendering/shader_compiler.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"
#include "servers/rendering/storage/material_storage.h"
#include "servers/rendering/storage/virtual_mesh_storage.h"
#include "servers/rendering/storage/virtual_shadow_map_storage.h"

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

#include <windows.h>
#endif

#include <limits>

using namespace RendererRD;
using RDC = RenderingDeviceCommons;

namespace TestDisplacementMaterial {

class DummyVirtualMeshStorage : public RendererVirtualMeshStorage {
protected:
	virtual void _upload_records(uint32_t p_first, uint32_t p_count) override {}
	virtual void _upload_mesh_records(uint32_t p_first, uint32_t p_count) override {}
};

void init_spatial_compiler(ShaderCompiler &r_compiler) {
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
	actions.renames["INSTANCE_CUSTOM"] = "instance_custom";
	actions.renames["TIME"] = "global_time";
	actions.renames["PI"] = String::num(Math::PI);
	actions.renames["TAU"] = String::num(Math::TAU);
	actions.renames["E"] = String::num(Math::E);
	actions.renames["ALBEDO"] = "albedo_highp";
	actions.renames["ALPHA"] = "alpha_highp";
	actions.renames["METALLIC"] = "metallic_highp";
	actions.renames["SPECULAR"] = "specular";
	actions.renames["ROUGHNESS"] = "roughness_highp";
	actions.renames["NORMAL_MAP"] = "normal_map";
	actions.renames["NORMAL_MAP_DEPTH"] = "normal_map_depth";
	actions.renames["EYE_OFFSET"] = "eye_offset";
	actions.renames["FRAGCOORD"] = "gl_FragCoord";
	actions.renames["FRONT_FACING"] = "gl_FrontFacing";
	actions.renames["SCREEN_UV"] = "screen_uv";
	actions.renames["VIEW"] = "view_highp";

	actions.usage_defines["NORMAL"] = "#define NORMAL_USED\n";
	actions.usage_defines["TANGENT"] = "#define TANGENT_USED\n";
	actions.usage_defines["BINORMAL"] = "@TANGENT";
	actions.usage_defines["UV"] = "#define UV_USED\n";
	actions.usage_defines["UV2"] = "#define UV2_USED\n";
	actions.usage_defines["COLOR"] = "#define COLOR_USED\n";
	actions.usage_defines["INSTANCE_CUSTOM"] = "#define ENABLE_INSTANCE_CUSTOM\n";
	actions.usage_defines["NORMAL_MAP"] = "#define NORMAL_MAP_USED\n";
	actions.usage_defines["ALPHA_SCISSOR_THRESHOLD"] = "#define ALPHA_SCISSOR_USED\n";
	actions.usage_defines["DISCARD"] = "#define DISCARD_USED\n";
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
	actions.resolve_builtin_derivatives["UV2"] = Pair<String, String>("uv2_interp_ddx", "uv2_interp_ddy");
	actions.resolve_builtin_derivatives["COLOR"] = Pair<String, String>("color_interp_ddx", "color_interp_ddy");
	actions.resolve_builtin_derivatives["VIEW"] = Pair<String, String>("view_ddx", "view_ddy");
	actions.resolve_builtin_derivatives["FRAGCOORD"] = Pair<String, String>("_ng_fragcoord_ddx", "_ng_fragcoord_ddy");
	actions.resolve_mutable_builtin_derivatives["NORMAL"] = Pair<String, String>("normal_interp_ddx", "normal_interp_ddy");
	actions.resolve_mutable_builtin_derivatives["TANGENT"] = Pair<String, String>("tangent_interp_ddx", "tangent_interp_ddy");
	actions.resolve_mutable_builtin_derivatives["BINORMAL"] = Pair<String, String>("binormal_interp_ddx", "binormal_interp_ddy");
	actions.resolve_uniform_builtins.insert("TIME");
	actions.resolve_uniform_builtins.insert("PI");
	actions.resolve_uniform_builtins.insert("TAU");
	actions.resolve_uniform_builtins.insert("E");
	actions.resolve_renames["FRONT_FACING"] = "_ng_front_facing";
	actions.resolve_renames["FRAGCOORD"] = "_ng_fragcoord";
	actions.resolve_vertex_renames["VERTEX_ID"] = "_ng_vertex_id";
	actions.resolve_vertex_renames["INSTANCE_ID"] = "_ng_instance_id";

	actions.displacement_renames["VERTEX"] = "_ng_disp_vertex";
	actions.displacement_renames["NORMAL"] = "_ng_disp_normal";
	actions.displacement_renames["UV"] = "_ng_disp_uv";
	actions.displacement_renames["UV2"] = "_ng_disp_uv2";
	actions.displacement_renames["COLOR"] = "_ng_disp_color";
	actions.displacement_renames["INSTANCE_CUSTOM"] = "_ng_disp_instance_custom";
	actions.displacement_renames["DISPLACEMENT"] = "_ng_displacement";

	actions.displacement_builtin_derivatives["VERTEX"] = Pair<String, String>("_ng_disp_vertex_ddx", "_ng_disp_vertex_ddy");
	actions.displacement_builtin_derivatives["NORMAL"] = Pair<String, String>("_ng_disp_normal_ddx", "_ng_disp_normal_ddy");
	actions.displacement_builtin_derivatives["UV"] = Pair<String, String>("_ng_disp_uv_ddx", "_ng_disp_uv_ddy");
	actions.displacement_builtin_derivatives["UV2"] = Pair<String, String>("_ng_disp_uv2_ddx", "_ng_disp_uv2_ddy");
	actions.displacement_builtin_derivatives["COLOR"] = Pair<String, String>("_ng_disp_color_ddx", "_ng_disp_color_ddy");

	r_compiler.initialize(actions);
}

void init_mobile_compiler(ShaderCompiler &r_compiler) {
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
	actions.renames["TANGENT"] = "tangent_highp";
	actions.renames["BINORMAL"] = "binormal_highp";
	actions.renames["POSITION"] = "position";
	actions.renames["UV"] = "uv_interp";
	actions.renames["UV2"] = "uv2_interp";
	actions.renames["COLOR"] = "color_highp";
	actions.renames["INSTANCE_CUSTOM"] = "instance_custom";
	actions.renames["TIME"] = "scene_data_block.data.time";
	actions.renames["PI"] = String::num(Math::PI);
	actions.renames["TAU"] = String::num(Math::TAU);
	actions.renames["E"] = String::num(Math::E);
	actions.renames["ALBEDO"] = "albedo_highp";
	actions.renames["ALPHA"] = "alpha_highp";
	actions.renames["METALLIC"] = "metallic_highp";
	actions.renames["SPECULAR"] = "specular_highp";
	actions.renames["ROUGHNESS"] = "roughness_highp";
	actions.renames["NORMAL_MAP"] = "normal_map_highp";
	actions.renames["NORMAL_MAP_DEPTH"] = "normal_map_depth_highp";
	actions.renames["EYE_OFFSET"] = "eye_offset";
	actions.renames["FRAGCOORD"] = "gl_FragCoord";
	actions.renames["FRONT_FACING"] = "gl_FrontFacing";
	actions.renames["SCREEN_UV"] = "screen_uv";
	actions.renames["VIEW"] = "view_highp";

	actions.usage_defines["NORMAL"] = "#define NORMAL_USED\n";
	actions.usage_defines["TANGENT"] = "#define TANGENT_USED\n";
	actions.usage_defines["BINORMAL"] = "@TANGENT";
	actions.usage_defines["UV"] = "#define UV_USED\n";
	actions.usage_defines["UV2"] = "#define UV2_USED\n";
	actions.usage_defines["COLOR"] = "#define COLOR_USED\n";
	actions.usage_defines["INSTANCE_CUSTOM"] = "#define ENABLE_INSTANCE_CUSTOM\n";
	actions.usage_defines["NORMAL_MAP"] = "#define NORMAL_MAP_USED\n";
	actions.usage_defines["ALPHA_SCISSOR_THRESHOLD"] = "#define ALPHA_SCISSOR_USED\n";
	actions.usage_defines["DISCARD"] = "#define DISCARD_USED\n";
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
	actions.renames["TANGENT"] = "tangent";
	actions.renames["BINORMAL"] = "binormal";
	actions.renames["POSITION"] = "position";
	actions.renames["UV"] = "uv_interp";
	actions.renames["UV2"] = "uv2_interp";
	actions.renames["COLOR"] = "color_interp";
	actions.renames["INSTANCE_CUSTOM"] = "instance_custom";
	actions.renames["TIME"] = "scene_data_block.data.time";
	actions.renames["PI"] = String::num(Math::PI);
	actions.renames["TAU"] = String::num(Math::TAU);
	actions.renames["E"] = String::num(Math::E);
	actions.renames["ALBEDO"] = "albedo";
	actions.renames["ALPHA"] = "alpha";
	actions.renames["METALLIC"] = "metallic";
	actions.renames["SPECULAR"] = "specular";
	actions.renames["ROUGHNESS"] = "roughness";
	actions.renames["NORMAL_MAP"] = "normal_map";
	actions.renames["NORMAL_MAP_DEPTH"] = "normal_map_depth";
	actions.renames["EYE_OFFSET"] = "eye_offset";
	actions.renames["FRAGCOORD"] = "gl_FragCoord";
	actions.renames["FRONT_FACING"] = "gl_FrontFacing";
	actions.renames["SCREEN_UV"] = "screen_uv";
	actions.renames["VIEW"] = "view";

	actions.usage_defines["NORMAL"] = "#define NORMAL_USED\n";
	actions.usage_defines["TANGENT"] = "#define TANGENT_USED\n";
	actions.usage_defines["BINORMAL"] = "@TANGENT";
	actions.usage_defines["UV"] = "#define UV_USED\n";
	actions.usage_defines["UV2"] = "#define UV2_USED\n";
	actions.usage_defines["COLOR"] = "#define COLOR_USED\n";
	actions.usage_defines["INSTANCE_CUSTOM"] = "#define ENABLE_INSTANCE_CUSTOM\n";
	actions.usage_defines["NORMAL_MAP"] = "#define NORMAL_MAP_USED\n";
	actions.usage_defines["ALPHA_SCISSOR_THRESHOLD"] = "#define ALPHA_SCISSOR_USED\n";
	actions.usage_defines["DISCARD"] = "#define DISCARD_USED\n";
	actions.render_mode_defines["diffuse_burley"] = "#define DIFFUSE_BURLEY\n";
	actions.render_mode_defines["specular_schlick_ggx"] = "#define SPECULAR_SCHLICK_GGX\n";

	actions.default_filter = ShaderLanguage::FILTER_LINEAR_MIPMAP;
	actions.default_repeat = ShaderLanguage::REPEAT_ENABLE;
	actions.apply_luminance_multiplier = true;
	actions.global_buffer_array_variable = "global_shader_uniforms";
	actions.global_buffer_array_is_texture = true;
	actions.instance_uniform_index_variable = "instance_offset";

	r_compiler.initialize(actions);
}

Error parse_spatial(const String &p_code, String *r_error = nullptr) {
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
	bool uses_time = false;
	bool uses_normal = false;
	bool writes_vertex = false;
};

CompileOutput compile_spatial(const String &p_code, bool p_visibility_resolve, bool p_raster_target, bool p_nextgi_hit = false) {
	ShaderCompiler compiler;
	init_spatial_compiler(compiler);

	CompileOutput out;
	ShaderCompiler::IdentifierActions actions;
	actions.entry_point_stages["vertex"] = ShaderCompiler::STAGE_VERTEX;
	actions.entry_point_stages["fragment"] = ShaderCompiler::STAGE_FRAGMENT;
	actions.entry_point_stages["light"] = ShaderCompiler::STAGE_FRAGMENT;
	actions.uniforms = &out.uniforms;
	actions.usage_flag_pointers["TIME"] = &out.uses_time;
	actions.usage_flag_pointers["NORMAL"] = &out.uses_normal;
	actions.write_flag_pointers["VERTEX"] = &out.writes_vertex;
	actions.visibility_resolve = p_visibility_resolve;
	actions.raster_target = p_raster_target;
	actions.nextgi_hit = p_nextgi_hit;

	out.err = compiler.compile(RSE::SHADER_SPATIAL, p_code, &actions, String(), out.gen);
	return out;
}

TEST_CASE("[NextGeo][Displacement] ShaderTypes registers spatial displacement() built-ins and ShaderLanguage enforces restrictions") {
	const HashMap<StringName, ShaderLanguage::FunctionInfo> &funcs = ShaderTypes::get_singleton()->get_functions(RSE::SHADER_SPATIAL);
	REQUIRE(funcs.has(SNAME("displacement")));
	const ShaderLanguage::FunctionInfo &disp = funcs[SNAME("displacement")];
	CHECK(disp.main_function);
	CHECK_FALSE(disp.can_discard);
	REQUIRE(disp.built_ins.has(SNAME("VERTEX")));
	REQUIRE(disp.built_ins.has(SNAME("NORMAL")));
	REQUIRE(disp.built_ins.has(SNAME("UV")));
	REQUIRE(disp.built_ins.has(SNAME("UV2")));
	REQUIRE(disp.built_ins.has(SNAME("COLOR")));
	REQUIRE(disp.built_ins.has(SNAME("INSTANCE_CUSTOM")));
	REQUIRE(disp.built_ins.has(SNAME("DISPLACEMENT")));
	CHECK(disp.built_ins[SNAME("VERTEX")].constant);
	CHECK(disp.built_ins[SNAME("NORMAL")].constant);
	CHECK(disp.built_ins[SNAME("UV")].constant);
	CHECK(disp.built_ins[SNAME("UV2")].constant);
	CHECK(disp.built_ins[SNAME("COLOR")].constant);
	CHECK(disp.built_ins[SNAME("INSTANCE_CUSTOM")].constant);
	CHECK_FALSE(disp.built_ins[SNAME("DISPLACEMENT")].constant);
	CHECK_EQ(disp.built_ins[SNAME("DISPLACEMENT")].type, ShaderLanguage::TYPE_FLOAT);

	// Valid shader reading every allowed input, TIME, uniforms, textures, and calling a helper.
	String err_text;
	const Error valid_err = parse_spatial(R"(
shader_type spatial;
uniform sampler2D height_tex;
uniform vec2 tiling = vec2(2.0, 2.0);
uniform float wave_amp = 0.1;

float sample_height(vec2 uv) {
	return texture(height_tex, uv * tiling).r;
}

void displacement() {
	float h = sample_height(UV + UV2 * 0.1);
	float wave = sin(dot(VERTEX, NORMAL) + TIME) * wave_amp;
	DISPLACEMENT = clamp(h + wave + COLOR.r * 0.05 + INSTANCE_CUSTOM.x * 0.01, 0.0, 1.0);
}

void fragment() {
	ALBEDO = vec3(0.5);
}
)",
			&err_text);
	CHECK_MESSAGE(valid_err == OK, err_text);

	// Read-only inputs cannot be assigned.
	const char *const_inputs[] = {
		"VERTEX = vec3(0.0);",
		"NORMAL = vec3(0.0, 1.0, 0.0);",
		"UV = vec2(0.0);",
		"UV2 = vec2(0.0);",
		"COLOR = vec4(1.0);",
		"INSTANCE_CUSTOM = vec4(0.0);",
	};
	for (const char *assign : const_inputs) {
		ERR_PRINT_OFF;
		const Error err = parse_spatial(String("shader_type spatial;\nvoid displacement() {\n\t") + assign + "\n\tDISPLACEMENT = 0.5;\n}\n");
		ERR_PRINT_ON;
		CHECK_NE(err, OK);
	}

	// Fragment-only built-ins and discard are disallowed directly and via helpers.
	const char *disallowed_snippets[] = {
		"shader_type spatial;\nvoid displacement() {\n\tdiscard;\n}\n",
		"shader_type spatial;\nvoid h() { discard; }\nvoid displacement() {\n\th();\n\tDISPLACEMENT = 0.5;\n}\n",
		"shader_type spatial;\nvoid displacement() {\n\tDISPLACEMENT = dFdx(UV.x);\n}\n",
		"shader_type spatial;\nvoid displacement() {\n\tDISPLACEMENT = dFdy(UV.y);\n}\n",
		"shader_type spatial;\nvoid displacement() {\n\tDISPLACEMENT = fwidth(UV.x);\n}\n",
		"shader_type spatial;\nuniform sampler2D t;\nvoid displacement() {\n\tDISPLACEMENT = textureQueryLod(t, UV).x;\n}\n",
		"shader_type spatial;\nfloat h(vec2 uv) { return fwidth(uv.x); }\nvoid displacement() {\n\tDISPLACEMENT = h(UV);\n}\n",
		"shader_type spatial;\nvarying float v_vert;\nvoid vertex() { v_vert = 1.0; }\nvoid displacement() {\n\tDISPLACEMENT = v_vert;\n}\n",
		"shader_type spatial;\nvarying float v_vert;\nvoid vertex() { v_vert = 1.0; }\nfloat h() { return v_vert; }\nvoid displacement() {\n\tDISPLACEMENT = h();\n}\n",
		"shader_type spatial;\nvarying float v_vert;\nvoid displacement() {\n\tv_vert = 1.0;\n\tDISPLACEMENT = 0.5;\n}\n",
		"shader_type spatial;\nvarying float v_vert;\nfloat take_v(float x) { return x; }\nvoid vertex() { v_vert = 1.0; }\nvoid displacement() {\n\tDISPLACEMENT = take_v(v_vert);\n}\n",
	};
	for (const char *snippet : disallowed_snippets) {
		ERR_PRINT_OFF;
		const Error err = parse_spatial(snippet);
		ERR_PRINT_ON;
		const String msg = String("Expected parse error for: ") + String(snippet);
		CHECK_MESSAGE(err != OK, msg);
	}
}

TEST_CASE("[NextGeo][Displacement] ShaderCompiler lowers texture() to textureGrad and compiles out silently on non-resolve targets") {
	const String disp_shader = R"(
shader_type spatial;
uniform sampler2D height_tex;
uniform vec2 uv_scale = vec2(3.0, 3.0);
uniform vec2 uv_offset = vec2(0.25, 0.5);

float eval_height(vec2 uv) {
	return texture(height_tex, uv).r;
}

void displacement() {
	vec2 transformed_uv = UV * uv_scale + uv_offset;
	float h0 = eval_height(transformed_uv);
	float h1 = texture(height_tex, UV).g;
	float h2 = textureProj(height_tex, vec3(UV, 1.0)).b;
	DISPLACEMENT = clamp(h0 * 0.5 + h1 * 0.25 + h2 * 0.25 + sin(TIME) * 0.01 + INSTANCE_CUSTOM.x * 0.01, 0.0, 1.0);
}

void fragment() {
	ALBEDO = vec3(0.4, 0.5, 0.6);
}
)";
	const String no_disp_shader = R"(
shader_type spatial;
uniform sampler2D height_tex;
uniform vec2 uv_scale = vec2(3.0, 3.0);
uniform vec2 uv_offset = vec2(0.25, 0.5);

void fragment() {
	ALBEDO = vec3(0.4, 0.5, 0.6);
}
)";

	// 1. Visibility resolve + raster target compiles displacement() with textureGrad and edge-length derivatives.
	const CompileOutput res = compile_spatial(disp_shader, true, true, false);
	REQUIRE_EQ(res.err, OK);
	CHECK(res.gen.has_displacement);
	CHECK(res.gen.uses_displacement_time);
	// Forward+ flags must NOT be polluted by displacement().
	CHECK_FALSE(res.uses_time);
	CHECK_FALSE(res.gen.uses_vertex_time);
	CHECK_FALSE(res.gen.uses_fragment_time);
	CHECK_FALSE(res.writes_vertex);
	CHECK_FALSE(res.gen.defines.has("#define UV_USED\n"));
	CHECK_FALSE(res.gen.defines.has("#define ENABLE_INSTANCE_CUSTOM\n"));

	REQUIRE(res.gen.code.has("resolve_displacement"));
	REQUIRE(res.gen.code.has("displacement"));
	CHECK_EQ(res.gen.code["resolve_displacement"], res.gen.code["displacement"]);

	const String &disp_code = res.gen.code["resolve_displacement"];
	CHECK(disp_code.contains("_ng_displacement"));
	CHECK(disp_code.contains("_ng_disp_uv"));
	CHECK(disp_code.contains("_ng_disp_uv_ddx"));
	CHECK(disp_code.contains("_ng_disp_uv_ddy"));
	CHECK(disp_code.contains("_ng_disp_instance_custom"));
	CHECK(disp_code.contains("textureGrad("));
	CHECK(disp_code.contains("textureProjGrad("));
	CHECK(disp_code.contains("_ng_rd_m_eval_height("));
	CHECK_FALSE(disp_code.contains("texture("));
	CHECK_FALSE(disp_code.contains("textureProj("));

	const String &frag_globals = res.gen.stage_globals[ShaderCompiler::STAGE_FRAGMENT];
	const String &comp_globals = res.gen.stage_globals[ShaderCompiler::STAGE_COMPUTE];
	CHECK(frag_globals.contains("_ng_rd_m_eval_height("));
	CHECK(frag_globals.contains("textureGrad("));
	CHECK(comp_globals.contains("_ng_rd_m_eval_height("));
	CHECK(comp_globals.contains("textureGrad("));
	// Displacement-only helper must not be emitted into regular Forward+ vertex/fragment helpers.
	CHECK_FALSE(res.gen.stage_globals[ShaderCompiler::STAGE_VERTEX].contains("m_eval_height"));

	// 2. Non-resolve targets (Forward+ non-resolve / Mobile / Compatibility) compile displacement() out silently.
	const CompileOutput fwd_disp = compile_spatial(disp_shader, false, false, false);
	const CompileOutput fwd_plain = compile_spatial(no_disp_shader, false, false, false);
	REQUIRE_EQ(fwd_disp.err, OK);
	REQUIRE_EQ(fwd_plain.err, OK);
	CHECK(fwd_disp.gen.has_displacement);
	CHECK_FALSE(fwd_plain.gen.has_displacement);
	CHECK_FALSE(fwd_disp.gen.code.has("displacement"));
	CHECK_FALSE(fwd_disp.gen.code.has("resolve_displacement"));
	CHECK_EQ(fwd_disp.gen.defines, fwd_plain.gen.defines);
	CHECK_EQ(fwd_disp.gen.uniform_total_size, fwd_plain.gen.uniform_total_size);
	CHECK_EQ(fwd_disp.gen.texture_uniforms.size(), fwd_plain.gen.texture_uniforms.size());
	CHECK_EQ(fwd_disp.gen.code["fragment"], fwd_plain.gen.code["fragment"]);
	CHECK_FALSE(fwd_disp.uses_time);
	CHECK_FALSE(fwd_disp.gen.uses_vertex_time);
	CHECK_FALSE(fwd_disp.gen.uses_fragment_time);

	// 3. NextGI hit group mode also compiles displacement() out silently.
	const CompileOutput hit_disp = compile_spatial(disp_shader, false, false, true);
	REQUIRE_EQ(hit_disp.err, OK);
	CHECK_FALSE(hit_disp.gen.code.has("displacement"));
	CHECK_FALSE(hit_disp.gen.code.has("resolve_displacement"));
	CHECK_FALSE(hit_disp.gen.stage_globals[ShaderCompiler::STAGE_FRAGMENT].contains("m_eval_height"));

	// 4. Screen/depth/normal-roughness textures read in displacement() set raster_uses reject bits.
	const CompileOutput disp_screen = compile_spatial(R"(
shader_type spatial;
uniform sampler2D screen_tex : hint_screen_texture;
void displacement() {
	DISPLACEMENT = texture(screen_tex, UV).r;
}
void fragment() {
	ALBEDO = vec3(1.0);
}
)",
			true, true, false);
	REQUIRE_EQ(disp_screen.err, OK);
	CHECK((disp_screen.gen.raster_uses & ShaderCompiler::RASTER_USES_SCREEN_TEXTURE) != 0u);
	CHECK((nextgeo_raster_rejects(disp_screen.gen.raster_uses, RendererVirtualMeshStorage::RASTER_FEATURE_DISPLACED) & ShaderCompiler::RASTER_USES_SCREEN_TEXTURE) != 0u);

	// 5. Non-spatial shaders defining a callable helper named displacement() must not be misdetected as spatial displacement().
	{
		ShaderCompiler canvas_compiler;
		ShaderCompiler::DefaultIdentifierActions canvas_def;
		canvas_def.renames["COLOR"] = "color";
		canvas_def.renames["UV"] = "uv";
		canvas_def.renames["TIME"] = "time";
		canvas_def.base_uniform_string = "material.";
		canvas_def.default_filter = ShaderLanguage::FILTER_LINEAR;
		canvas_def.default_repeat = ShaderLanguage::REPEAT_DISABLE;
		canvas_compiler.initialize(canvas_def);

		ShaderCompiler::GeneratedCode canvas_gen;
		HashMap<StringName, ShaderLanguage::ShaderNode::Uniform> canvas_uniforms;
		bool canvas_uses_time = false;
		ShaderCompiler::IdentifierActions canvas_actions;
		canvas_actions.entry_point_stages["vertex"] = ShaderCompiler::STAGE_VERTEX;
		canvas_actions.entry_point_stages["fragment"] = ShaderCompiler::STAGE_FRAGMENT;
		canvas_actions.entry_point_stages["light"] = ShaderCompiler::STAGE_FRAGMENT;
		canvas_actions.uniforms = &canvas_uniforms;
		canvas_actions.usage_flag_pointers["TIME"] = &canvas_uses_time;

		const Error canvas_err = canvas_compiler.compile(RSE::SHADER_CANVAS_ITEM, R"(
shader_type canvas_item;
float displacement(float x) {
	return sin(x + TIME);
}
void fragment() {
	COLOR = vec4(displacement(UV.x), 0.5, 0.5, 1.0);
}
)",
				&canvas_actions, String(), canvas_gen);
		REQUIRE_EQ(canvas_err, OK);
		CHECK_FALSE(canvas_gen.has_displacement);
		CHECK_FALSE(canvas_gen.uses_displacement_time);
		CHECK(canvas_uses_time);
		CHECK(canvas_gen.stage_globals[ShaderCompiler::STAGE_FRAGMENT].contains("m_displacement"));
		CHECK(canvas_gen.code["fragment"].contains("m_displacement("));
	}
}

TEST_CASE("[NextGeo][Displacement] Material displacement properties and RenderingServer.material_set_displacement") {
	REQUIRE(RS::get_singleton() != nullptr);
	REQUIRE(RSG::material_storage != nullptr);

	// Defaults & clamping on StandardMaterial3D (including lazy RID forwarding).
	Ref<StandardMaterial3D> std_mat;
	std_mat.instantiate();
	CHECK_EQ(std_mat->get_displacement_magnitude(), doctest::Approx(0.0f));
	CHECK_EQ(std_mat->get_displacement_center(), doctest::Approx(0.5f));

	std_mat->set_displacement_magnitude(-2.0f);
	CHECK_EQ(std_mat->get_displacement_magnitude(), doctest::Approx(0.0f));
	std_mat->set_displacement_magnitude(std::numeric_limits<float>::quiet_NaN());
	CHECK_EQ(std_mat->get_displacement_magnitude(), doctest::Approx(0.0f));
	std_mat->set_displacement_magnitude(std::numeric_limits<float>::infinity());
	CHECK_EQ(std_mat->get_displacement_magnitude(), doctest::Approx(0.0f));

	std_mat->set_displacement_magnitude(0.25f);
	std_mat->set_displacement_center(1.5f);
	CHECK_EQ(std_mat->get_displacement_center(), doctest::Approx(1.0f));
	std_mat->set_displacement_center(-0.5f);
	CHECK_EQ(std_mat->get_displacement_center(), doctest::Approx(0.0f));
	std_mat->set_displacement_center(std::numeric_limits<float>::quiet_NaN());
	CHECK_EQ(std_mat->get_displacement_center(), doctest::Approx(0.5f));
	std_mat->set_displacement_center(0.3f);

	// Force lazy RID creation and verify values were forwarded to MaterialStorage.
	const RID std_rid = std_mat->get_rid();
	REQUIRE(std_rid.is_valid());
	CHECK_EQ(RSG::material_storage->material_get_displacement_magnitude(std_rid), doctest::Approx(0.25f));
	CHECK_EQ(RSG::material_storage->material_get_displacement_center(std_rid), doctest::Approx(0.3f));

	// Subsequent edits update MaterialStorage immediately.
	std_mat->set_displacement_magnitude(0.75f);
	std_mat->set_displacement_center(0.6f);
	CHECK_EQ(RSG::material_storage->material_get_displacement_magnitude(std_rid), doctest::Approx(0.75f));
	CHECK_EQ(RSG::material_storage->material_get_displacement_center(std_rid), doctest::Approx(0.6f));

	// Direct RenderingServer API round-trip and clamping.
	const RID raw_mat = RS::get_singleton()->material_create();
	REQUIRE(raw_mat.is_valid());
	CHECK_EQ(RSG::material_storage->material_get_displacement_magnitude(raw_mat), doctest::Approx(0.0f));
	CHECK_EQ(RSG::material_storage->material_get_displacement_center(raw_mat), doctest::Approx(0.5f));
	RS::get_singleton()->material_set_displacement(raw_mat, 0.4f, 0.2f);
	CHECK_EQ(RSG::material_storage->material_get_displacement_magnitude(raw_mat), doctest::Approx(0.4f));
	CHECK_EQ(RSG::material_storage->material_get_displacement_center(raw_mat), doctest::Approx(0.2f));
	RS::get_singleton()->material_set_displacement(raw_mat, -1.0f, 2.0f);
	CHECK_EQ(RSG::material_storage->material_get_displacement_magnitude(raw_mat), doctest::Approx(0.0f));
	CHECK_EQ(RSG::material_storage->material_get_displacement_center(raw_mat), doctest::Approx(1.0f));
	RS::get_singleton()->free_rid(raw_mat);

	// ShaderMaterial lazy RID propagation.
	Ref<ShaderMaterial> shader_mat;
	shader_mat.instantiate();
	shader_mat->set_displacement_magnitude(0.15f);
	shader_mat->set_displacement_center(0.8f);
	const RID sm_rid = shader_mat->get_rid();
	REQUIRE(sm_rid.is_valid());
	CHECK_EQ(RSG::material_storage->material_get_displacement_magnitude(sm_rid), doctest::Approx(0.15f));
	CHECK_EQ(RSG::material_storage->material_get_displacement_center(sm_rid), doctest::Approx(0.8f));

	// Non-spatial material hides displacement properties in property validation.
	Ref<CanvasItemMaterial> canvas_mat;
	canvas_mat.instantiate();
	List<PropertyInfo> props;
	canvas_mat->get_property_list(&props);
	bool found_disp_prop = false;
	for (const PropertyInfo &pi : props) {
		if ((pi.name == "displacement_magnitude" || pi.name == "displacement_center") && pi.usage != PROPERTY_USAGE_NONE) {
			found_disp_prop = true;
		}
	}
	CHECK_FALSE(found_disp_prop);
}

TEST_CASE("[NextGeo][Displacement] BaseMaterial3D heightmap_mode PARALLAX unchanged and DISPLACEMENT codegen") {
	REQUIRE(RS::get_singleton() != nullptr);

	// Default mode is PARALLAX and leaves generated shader code byte-for-byte unchanged.
	Ref<StandardMaterial3D> parallax_default;
	parallax_default.instantiate();
	CHECK_EQ(parallax_default->get_heightmap_mode(), BaseMaterial3D::HEIGHTMAP_MODE_PARALLAX);
	parallax_default->set_feature(BaseMaterial3D::FEATURE_HEIGHT_MAPPING, true);
	const String parallax_default_code = RS::get_singleton()->shader_get_code(parallax_default->get_shader_rid());
	REQUIRE_FALSE(parallax_default_code.is_empty());

	Ref<StandardMaterial3D> parallax_explicit;
	parallax_explicit.instantiate();
	parallax_explicit->set_feature(BaseMaterial3D::FEATURE_HEIGHT_MAPPING, true);
	parallax_explicit->set_heightmap_mode(BaseMaterial3D::HEIGHTMAP_MODE_PARALLAX);
	const String parallax_explicit_code = RS::get_singleton()->shader_get_code(parallax_explicit->get_shader_rid());

	CHECK_EQ(parallax_default_code, parallax_explicit_code);
	CHECK(parallax_default_code.contains("uniform sampler2D texture_heightmap"));
	CHECK(parallax_default_code.contains("uniform float heightmap_scale"));
	CHECK(parallax_default_code.contains("uniform int heightmap_min_layers"));
	CHECK(parallax_default_code.contains("uniform vec2 heightmap_flip"));
	CHECK(parallax_default_code.contains("// Height: Enabled"));
	CHECK_FALSE(parallax_default_code.contains("void displacement()"));

	// DISPLACEMENT mode generates displacement() from texture_heightmap and disables parallax.
	Ref<StandardMaterial3D> disp_mat;
	disp_mat.instantiate();
	disp_mat->set_feature(BaseMaterial3D::FEATURE_HEIGHT_MAPPING, true);
	disp_mat->set_heightmap_mode(BaseMaterial3D::HEIGHTMAP_MODE_DISPLACEMENT);
	const String disp_code = RS::get_singleton()->shader_get_code(disp_mat->get_shader_rid());
	REQUIRE_FALSE(disp_code.is_empty());

	CHECK(disp_code.contains("uniform sampler2D texture_heightmap"));
	CHECK(disp_code.contains("void displacement()"));
	CHECK(disp_code.contains("vec2 heightmap_uv = UV * uv1_scale.xy + uv1_offset.xy;"));
	CHECK(disp_code.contains("DISPLACEMENT = texture(texture_heightmap, heightmap_uv).r;"));
	CHECK_FALSE(disp_code.contains("1.0 - texture(texture_heightmap, heightmap_uv).r"));
	CHECK_FALSE(disp_code.contains("uniform float heightmap_scale"));
	CHECK_FALSE(disp_code.contains("uniform int heightmap_min_layers"));
	CHECK_FALSE(disp_code.contains("uniform int heightmap_max_layers"));
	CHECK_FALSE(disp_code.contains("uniform vec2 heightmap_flip"));
	CHECK_FALSE(disp_code.contains("// Height: Enabled"));

	// Inverted heightmap flips displacement polarity.
	Ref<StandardMaterial3D> disp_inv_mat;
	disp_inv_mat.instantiate();
	disp_inv_mat->set_feature(BaseMaterial3D::FEATURE_HEIGHT_MAPPING, true);
	disp_inv_mat->set_heightmap_mode(BaseMaterial3D::HEIGHTMAP_MODE_DISPLACEMENT);
	disp_inv_mat->set_flag(BaseMaterial3D::FLAG_INVERT_HEIGHTMAP, true);
	const String disp_inv_code = RS::get_singleton()->shader_get_code(disp_inv_mat->get_shader_rid());
	CHECK(disp_inv_code.contains("DISPLACEMENT = 1.0 - texture(texture_heightmap, heightmap_uv).r;"));

	// Generated BaseMaterial3D displacement shader compiles cleanly in both resolve+raster and Forward+ modes.
	const CompileOutput resolve_out = compile_spatial(disp_code, true, true, false);
	REQUIRE_EQ(resolve_out.err, OK);
	CHECK(resolve_out.gen.has_displacement);
	CHECK(resolve_out.gen.code["resolve_displacement"].contains("textureGrad("));
	CHECK_EQ(nextgeo_raster_rejects(resolve_out.gen.raster_uses, RendererVirtualMeshStorage::RASTER_FEATURE_DISPLACED), 0u);

	const CompileOutput fwd_out = compile_spatial(disp_code, false, false, false);
	REQUIRE_EQ(fwd_out.err, OK);
	CHECK_FALSE(fwd_out.gen.code.has("displacement"));
}

TEST_CASE("[NextGeo][Displacement] RASTER_FEATURE_DISPLACED classification and cull bound inflation") {
	using VMS = RendererVirtualMeshStorage;

	// Classification: magnitude > 0 sets DISPLACED, magnitude == 0 leaves it clear.
	NextGeoRasterFacts facts;
	CHECK_EQ(nextgeo_raster_features(facts), 0u);
	CHECK_FALSE(nextgeo_raster_has_program(nextgeo_raster_features(facts)));

	facts.displaced = true;
	CHECK_EQ(nextgeo_raster_features(facts), uint32_t(VMS::RASTER_FEATURE_DISPLACED));
	CHECK(nextgeo_raster_has_program(nextgeo_raster_features(facts)));
	CHECK_EQ(VMS::resolve_raster_bin(VMS::RASTER_FEATURE_DISPLACED, VMS::RASTER_PASS_VISIBILITY), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));
	CHECK_EQ(VMS::resolve_raster_bin(VMS::RASTER_FEATURE_DISPLACED, VMS::RASTER_PASS_VSM), int32_t(VMS::RASTER_BIN_PROGRAMMABLE));

	// Programmable raster bin exhaustion preserves the exclusive-dispatch invariant:
	// displaced-only materials (default fallback NONE) keep their virtual fixed-function bin (bin 0),
	// while masked / vertex-offset materials return to their legacy fallback reason.
	{
		NextGeoSurfaceRoute displaced_only_route;
		displaced_only_route.virtual_geometry = true;
		displaced_only_route.reason = NEXTGEO_FALLBACK_NONE;
		nextgeo_raster_bin_apply_exhausted(displaced_only_route, NEXTGEO_FALLBACK_NONE);
		CHECK(displaced_only_route.virtual_geometry);
		CHECK_EQ(displaced_only_route.reason, NEXTGEO_FALLBACK_NONE);

		NextGeoSurfaceRoute masked_route;
		masked_route.virtual_geometry = true;
		masked_route.reason = NEXTGEO_FALLBACK_NONE;
		nextgeo_raster_bin_apply_exhausted(masked_route, NEXTGEO_FALLBACK_ALPHA);
		CHECK_FALSE(masked_route.virtual_geometry);
		CHECK_EQ(masked_route.reason, NEXTGEO_FALLBACK_ALPHA);

		NextGeoSurfaceRoute vertex_offset_route;
		vertex_offset_route.virtual_geometry = true;
		vertex_offset_route.reason = NEXTGEO_FALLBACK_NONE;
		nextgeo_raster_bin_apply_exhausted(vertex_offset_route, NEXTGEO_FALLBACK_UNSUPPORTED_CUSTOM_SHADER);
		CHECK_FALSE(vertex_offset_route.virtual_geometry);
		CHECK_EQ(vertex_offset_route.reason, NEXTGEO_FALLBACK_UNSUPPORTED_CUSTOM_SHADER);
	}

	// Bound inflation helpers.
	const AABB base_aabb(Vector3(-1.0f, -2.0f, -3.0f), Vector3(2.0f, 4.0f, 6.0f));
	CHECK_EQ(VMS::inflate_aabb_for_displacement(base_aabb, 0.0f), base_aabb);
	CHECK_EQ(VMS::inflate_aabb_for_displacement(base_aabb, -0.5f), base_aabb);
	CHECK_EQ(VMS::inflate_aabb_for_displacement(base_aabb, std::numeric_limits<float>::quiet_NaN()), base_aabb);
	const AABB inflated = VMS::inflate_aabb_for_displacement(base_aabb, 0.5f);
	CHECK(inflated.position.is_equal_approx(Vector3(-1.5f, -2.5f, -3.5f)));
	CHECK(inflated.size.is_equal_approx(Vector3(3.0f, 5.0f, 7.0f)));

	CHECK_EQ(VMS::inflate_sphere_radius_for_displacement(1.25f, 0.0f), doctest::Approx(1.25f));
	CHECK_EQ(VMS::inflate_sphere_radius_for_displacement(1.25f, 0.35f), doctest::Approx(1.60f));

	// VirtualMeshStorage Record layout and displacement_bound persistence across transform updates and flush.
	DummyVirtualMeshStorage storage;
	const RID mesh = RID::from_uint64(101);
	const RID inst = RID::from_uint64(202);
	RenderingServerTypes::VirtualGeometryDesc desc;
	desc.eligible_surface_mask = 1u;
	desc.ngm_path = "res://test.ngm";
	desc.dag_header.resize(8);
	desc.root_pages.push_back(Vector<uint8_t>());
	storage.mesh_set_virtual_geometry(mesh, desc);

	const uint32_t slot = storage.instance_record_allocate(inst, mesh);
	REQUIRE_NE(slot, UINT32_MAX);
	CHECK_EQ(storage.get_record(slot).prev_world_aabb_max[3], doctest::Approx(0.0f));

	storage.instance_record_set_displacement_bound(slot, 0.45f);
	CHECK_EQ(storage.get_record(slot).prev_world_aabb_max[3], doctest::Approx(0.45f));

	// Transform updates and frame flushes must preserve prev_world_aabb_max[3].
	storage.instance_record_set_transform(slot, Transform3D(), inflated, true, false, 1.0f);
	CHECK_EQ(storage.get_record(slot).prev_world_aabb_max[3], doctest::Approx(0.45f));
	storage.flush();
	storage.begin_frame();
	storage.instance_record_set_transform(slot, Transform3D(Basis(), Vector3(1.0f, 0.0f, 0.0f)), inflated, true, false, 1.0f);
	CHECK_EQ(storage.get_record(slot).prev_world_aabb_max[3], doctest::Approx(0.45f));
	storage.flush();
	storage.begin_frame();
	storage.flush();
	CHECK_EQ(storage.get_record(slot).prev_world_aabb_max[3], doctest::Approx(0.45f));

	// Negative / NaN sanitize to 0.0f.
	storage.instance_record_set_displacement_bound(slot, -3.0f);
	CHECK_EQ(storage.get_record(slot).prev_world_aabb_max[3], doctest::Approx(0.0f));
	storage.instance_record_set_displacement_bound(slot, std::numeric_limits<float>::quiet_NaN());
	CHECK_EQ(storage.get_record(slot).prev_world_aabb_max[3], doctest::Approx(0.0f));

	storage.instance_record_free(slot);
}

TEST_CASE("[NextGeo][Displacement] Displacement shaders compile into raster target and shader containers") {
	const String shader_src = R"(
shader_type spatial;
uniform sampler2D height_tex;
uniform vec2 uv_scale = vec2(2.0, 2.0);
uniform float bias = 0.05;

float sample_h(vec2 uv) {
	return texture(height_tex, uv * uv_scale).r;
}

void displacement() {
	float h = sample_h(UV) + sin(dot(VERTEX, NORMAL) + TIME) * 0.02 + COLOR.r * 0.01 + INSTANCE_CUSTOM.x * 0.01 + bias;
	DISPLACEMENT = clamp(h, 0.0, 1.0);
}

void fragment() {
	ALBEDO = vec3(0.6, 0.7, 0.8);
}
)";
	const CompileOutput res = compile_spatial(shader_src, true, true, false);
	REQUIRE_EQ(res.err, OK);
	REQUIRE(res.gen.has_displacement);
	REQUIRE(res.gen.code.has("resolve_displacement"));
	REQUIRE(res.gen.code.has("displacement"));

	// Build SW and HW raster target templates (same path as ShaderBakerExportPlugin).
	RasterMaterialSwShaderRD sw_shader;
	RasterMaterialHwShaderRD hw_shader;
	Vector<ShaderRD::VariantDefine> raster_variants;
	raster_variants.push_back(ShaderRD::VariantDefine(0, "", false));
	raster_variants.push_back(ShaderRD::VariantDefine(1, "\n#define VIS64_BUFFER\n", false));
	sw_shader.initialize(raster_variants);
	hw_shader.initialize(raster_variants);

	HashMap<String, String> raster_code;
	raster_code["resolve_vertex"] = res.gen.code.has("resolve_vertex") ? res.gen.code["resolve_vertex"] : String();
	raster_code["resolve_vertex_store"] = res.gen.code.has("resolve_vertex_store") ? res.gen.code["resolve_vertex_store"] : String();
	raster_code["resolve_interpolate"] = res.gen.code.has("resolve_interpolate") ? res.gen.code["resolve_interpolate"] : String();
	raster_code["resolve_displacement"] = res.gen.code["resolve_displacement"];
	raster_code["raster_fragment"] = res.gen.code["raster_fragment"];

	const String &compute_globals = res.gen.stage_globals[ShaderCompiler::STAGE_COMPUTE];
	const RID sw_ver = sw_shader.version_create(false);
	const RID hw_ver = hw_shader.version_create(false);
	REQUIRE(sw_ver.is_valid());
	REQUIRE(hw_ver.is_valid());
	sw_shader.version_set_compute_code(sw_ver, raster_code, res.gen.uniforms, compute_globals, res.gen.defines);
	hw_shader.version_set_code(hw_ver, raster_code, res.gen.uniforms, compute_globals, compute_globals, res.gen.defines);

	for (int variant = 0; variant < 2; variant++) {
		const Vector<String> sw_sources = sw_shader.version_build_variant_stage_sources(sw_ver, variant);
		const Vector<String> hw_sources = hw_shader.version_build_variant_stage_sources(hw_ver, variant);
		REQUIRE_EQ(sw_sources.size(), int(RD::SHADER_STAGE_MAX));
		REQUIRE_EQ(hw_sources.size(), int(RD::SHADER_STAGE_MAX));
		CHECK(sw_sources[RD::SHADER_STAGE_COMPUTE].contains("_ng_eval_displacement"));
		CHECK(sw_sources[RD::SHADER_STAGE_COMPUTE].contains("_ng_rd_m_sample_h"));
		CHECK(hw_sources[RD::SHADER_STAGE_FRAGMENT].contains("_ng_eval_displacement"));

#if defined(MODULE_GLSLANG_ENABLED) && defined(VULKAN_ENABLED)
		RenderingShaderContainerFormatVulkan vk_format;
		String err;
		RDC::ShaderStageSPIRVData sw_spv;
		sw_spv.shader_stage = RDC::SHADER_STAGE_COMPUTE;
		sw_spv.spirv = compile_glslang_shader(RDC::SHADER_STAGE_COMPUTE, sw_sources[RD::SHADER_STAGE_COMPUTE], vk_format.get_shader_language_version(), vk_format.get_shader_spirv_version(RDC::SHADER_STAGE_COMPUTE), &err);
		CHECK_MESSAGE(!sw_spv.spirv.is_empty(), err);
		Ref<RenderingShaderContainer> sw_container = vk_format.create_container();
		Vector<RDC::ShaderStageSPIRVData> sw_stages = { sw_spv };
		CHECK(sw_container->set_code_from_spirv("RasterMaterialSwDisplacement", sw_stages));

		RDC::ShaderStageSPIRVData hw_vs;
		hw_vs.shader_stage = RDC::SHADER_STAGE_VERTEX;
		hw_vs.spirv = compile_glslang_shader(RDC::SHADER_STAGE_VERTEX, hw_sources[RD::SHADER_STAGE_VERTEX], vk_format.get_shader_language_version(), vk_format.get_shader_spirv_version(RDC::SHADER_STAGE_VERTEX), &err);
		CHECK_MESSAGE(!hw_vs.spirv.is_empty(), err);
		RDC::ShaderStageSPIRVData hw_fs;
		hw_fs.shader_stage = RDC::SHADER_STAGE_FRAGMENT;
		hw_fs.spirv = compile_glslang_shader(RDC::SHADER_STAGE_FRAGMENT, hw_sources[RD::SHADER_STAGE_FRAGMENT], vk_format.get_shader_language_version(), vk_format.get_shader_spirv_version(RDC::SHADER_STAGE_FRAGMENT), &err);
		CHECK_MESSAGE(!hw_fs.spirv.is_empty(), err);
		Ref<RenderingShaderContainer> hw_container = vk_format.create_container();
		Vector<RDC::ShaderStageSPIRVData> hw_stages = { hw_vs, hw_fs };
		CHECK(hw_container->set_code_from_spirv("RasterMaterialHwDisplacement", hw_stages));
#endif
	}

	CHECK(sw_shader.version_free(sw_ver));
	CHECK(hw_shader.version_free(hw_ver));

	// Live #163 dicing-compute harness: calls _ng_eval_displacement() from main() so textureGrad + _ng_rd_
	// helpers stay live through SPIR-V -> Vulkan, D3D12 (Mesa NIR -> DXIL), and Metal containers.
#ifdef MODULE_GLSLANG_ENABLED
	String dicing_cs = "#version 450\n#define MODE_VISIBILITY_RESOLVE\n";
	for (const String &def : res.gen.defines) {
		dicing_cs += def;
	}
	dicing_cs += R"(
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) buffer OutBuf { float values[]; } out_buf;
layout(set = 1, binding = 0, std140) uniform SceneDataBlock { float time; vec3 pad; } scene_data_block;
layout(set = 1, binding = 3) uniform sampler SAMPLER_NEAREST_CLAMP;
layout(set = 1, binding = 4) uniform sampler SAMPLER_LINEAR_CLAMP;
layout(set = 1, binding = 5) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_CLAMP;
layout(set = 1, binding = 6) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_CLAMP;
layout(set = 1, binding = 7) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_CLAMP;
layout(set = 1, binding = 8) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_CLAMP;
layout(set = 1, binding = 9) uniform sampler SAMPLER_NEAREST_REPEAT;
layout(set = 1, binding = 10) uniform sampler SAMPLER_LINEAR_REPEAT;
layout(set = 1, binding = 11) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_REPEAT;
layout(set = 1, binding = 12) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_REPEAT;
layout(set = 1, binding = 13) uniform sampler SAMPLER_NEAREST_WITH_MIPMAPS_ANISOTROPIC_REPEAT;
layout(set = 1, binding = 14) uniform sampler SAMPLER_LINEAR_WITH_MIPMAPS_ANISOTROPIC_REPEAT;
#define global_time scene_data_block.time
layout(set = 3, binding = 0, std140) uniform MaterialUniforms {
)";
	dicing_cs += res.gen.uniforms;
	dicing_cs += "} material;\n";
	dicing_cs += compute_globals;
	dicing_cs += R"(
float eval_disp(vec3 _ng_disp_vertex, vec3 _ng_disp_normal, vec2 _ng_disp_uv, vec2 _ng_disp_uv2,
		vec4 _ng_disp_color, vec4 _ng_disp_instance_custom,
		vec3 _ng_disp_vertex_ddx, vec3 _ng_disp_vertex_ddy,
		vec3 _ng_disp_normal_ddx, vec3 _ng_disp_normal_ddy,
		vec2 _ng_disp_uv_ddx, vec2 _ng_disp_uv_ddy,
		vec2 _ng_disp_uv2_ddx, vec2 _ng_disp_uv2_ddy,
		vec4 _ng_disp_color_ddx, vec4 _ng_disp_color_ddy) {
	float _ng_displacement = 0.0;
	{
)";
	dicing_cs += res.gen.code["displacement"];
	dicing_cs += R"(
	}
	return _ng_displacement;
}
void main() {
	uint idx = gl_GlobalInvocationID.x;
	vec2 uv = vec2(float(idx) * 0.01, 0.5);
	out_buf.values[idx] = eval_disp(
			vec3(uv, 0.0), vec3(0.0, 0.0, 1.0), uv, uv, vec4(1.0), vec4(0.0),
			vec3(0.01, 0.0, 0.0), vec3(0.0, 0.01, 0.0),
			vec3(0.0), vec3(0.0),
			vec2(0.01, 0.0), vec2(0.0, 0.01),
			vec2(0.01, 0.0), vec2(0.0, 0.01),
			vec4(0.0), vec4(0.0));
}
)";

#ifdef VULKAN_ENABLED
	{
		RenderingShaderContainerFormatVulkan vk_format;
		String err;
		RDC::ShaderStageSPIRVData spv;
		spv.shader_stage = RDC::SHADER_STAGE_COMPUTE;
		spv.spirv = compile_glslang_shader(RDC::SHADER_STAGE_COMPUTE, dicing_cs, vk_format.get_shader_language_version(), vk_format.get_shader_spirv_version(RDC::SHADER_STAGE_COMPUTE), &err);
		CHECK_MESSAGE(!spv.spirv.is_empty(), err);
		Ref<RenderingShaderContainer> container = vk_format.create_container();
		Vector<RDC::ShaderStageSPIRVData> stages = { spv };
		CHECK(container->set_code_from_spirv("DicingComputeDisplacementVulkan", stages));
	}
#endif
#ifdef D3D12_ENABLED
	{
		HMODULE lib_d3d12 = LoadLibraryW(L"D3D12.dll");
		REQUIRE(lib_d3d12 != nullptr);
		{
			RenderingShaderContainerFormatD3D12 d3d12_format;
			d3d12_format.set_lib_d3d12(lib_d3d12);
			String err;
			RDC::ShaderStageSPIRVData spv;
			spv.shader_stage = RDC::SHADER_STAGE_COMPUTE;
			spv.spirv = compile_glslang_shader(RDC::SHADER_STAGE_COMPUTE, dicing_cs, d3d12_format.get_shader_language_version(), d3d12_format.get_shader_spirv_version(RDC::SHADER_STAGE_COMPUTE), &err);
			CHECK_MESSAGE(!spv.spirv.is_empty(), err);
			Ref<RenderingShaderContainer> container = d3d12_format.create_container();
			Vector<RDC::ShaderStageSPIRVData> stages = { spv };
			CHECK(container->set_code_from_spirv("DicingComputeDisplacementD3D12", stages));
		}
		FreeLibrary(lib_d3d12);
	}
#endif
#ifdef METAL_ENABLED
	{
		const MetalDeviceProfile *profile = MetalDeviceProfile::get_profile(MetalDeviceProfile::Platform::macOS, MetalDeviceProfile::GPU::Apple9, os_version::MACOS_14_0);
		REQUIRE(profile != nullptr);
		RenderingShaderContainerFormatMetal mtl_format(profile, true);
		String err;
		RDC::ShaderStageSPIRVData spv;
		spv.shader_stage = RDC::SHADER_STAGE_COMPUTE;
		spv.spirv = compile_glslang_shader(RDC::SHADER_STAGE_COMPUTE, dicing_cs, mtl_format.get_shader_language_version(), mtl_format.get_shader_spirv_version(RDC::SHADER_STAGE_COMPUTE), &err);
		CHECK_MESSAGE(!spv.spirv.is_empty(), err);
		Ref<RenderingShaderContainer> container = mtl_format.create_container();
		Vector<RDC::ShaderStageSPIRVData> stages = { spv };
		CHECK(container->set_code_from_spirv("DicingComputeDisplacementMetal", stages));
	}
#endif
#endif // MODULE_GLSLANG_ENABLED
}

#ifdef MODULE_GLSLANG_ENABLED
static void check_graphics_containers(const String &p_name, const String &p_vs, const String &p_fs) {
#ifdef VULKAN_ENABLED
	{
		RenderingShaderContainerFormatVulkan vk_format;
		String err;
		RDC::ShaderStageSPIRVData vs;
		vs.shader_stage = RDC::SHADER_STAGE_VERTEX;
		vs.spirv = compile_glslang_shader(RDC::SHADER_STAGE_VERTEX, p_vs, vk_format.get_shader_language_version(), vk_format.get_shader_spirv_version(RDC::SHADER_STAGE_VERTEX), &err);
		CHECK_MESSAGE(!vs.spirv.is_empty(), err);
		RDC::ShaderStageSPIRVData fs;
		fs.shader_stage = RDC::SHADER_STAGE_FRAGMENT;
		fs.spirv = compile_glslang_shader(RDC::SHADER_STAGE_FRAGMENT, p_fs, vk_format.get_shader_language_version(), vk_format.get_shader_spirv_version(RDC::SHADER_STAGE_FRAGMENT), &err);
		CHECK_MESSAGE(!fs.spirv.is_empty(), err);
		Ref<RenderingShaderContainer> container = vk_format.create_container();
		Vector<RDC::ShaderStageSPIRVData> stages = { vs, fs };
		CHECK(container->set_code_from_spirv(p_name + "Vulkan", stages));
	}
#endif
#ifdef D3D12_ENABLED
	{
		HMODULE lib_d3d12 = LoadLibraryW(L"D3D12.dll");
		REQUIRE(lib_d3d12 != nullptr);
		{
			RenderingShaderContainerFormatD3D12 d3d12_format;
			d3d12_format.set_lib_d3d12(lib_d3d12);
			String err;
			RDC::ShaderStageSPIRVData vs;
			vs.shader_stage = RDC::SHADER_STAGE_VERTEX;
			vs.spirv = compile_glslang_shader(RDC::SHADER_STAGE_VERTEX, p_vs, d3d12_format.get_shader_language_version(), d3d12_format.get_shader_spirv_version(RDC::SHADER_STAGE_VERTEX), &err);
			CHECK_MESSAGE(!vs.spirv.is_empty(), err);
			RDC::ShaderStageSPIRVData fs;
			fs.shader_stage = RDC::SHADER_STAGE_FRAGMENT;
			fs.spirv = compile_glslang_shader(RDC::SHADER_STAGE_FRAGMENT, p_fs, d3d12_format.get_shader_language_version(), d3d12_format.get_shader_spirv_version(RDC::SHADER_STAGE_FRAGMENT), &err);
			CHECK_MESSAGE(!fs.spirv.is_empty(), err);
			Ref<RenderingShaderContainer> container = d3d12_format.create_container();
			Vector<RDC::ShaderStageSPIRVData> stages = { vs, fs };
			CHECK(container->set_code_from_spirv(p_name + "D3D12", stages));
		}
		FreeLibrary(lib_d3d12);
	}
#endif
#ifdef METAL_ENABLED
	{
		const MetalDeviceProfile *profile = MetalDeviceProfile::get_profile(MetalDeviceProfile::Platform::macOS, MetalDeviceProfile::GPU::Apple9, os_version::MACOS_14_0);
		REQUIRE(profile != nullptr);
		RenderingShaderContainerFormatMetal mtl_format(profile, true);
		String err;
		RDC::ShaderStageSPIRVData vs;
		vs.shader_stage = RDC::SHADER_STAGE_VERTEX;
		vs.spirv = compile_glslang_shader(RDC::SHADER_STAGE_VERTEX, p_vs, mtl_format.get_shader_language_version(), mtl_format.get_shader_spirv_version(RDC::SHADER_STAGE_VERTEX), &err);
		CHECK_MESSAGE(!vs.spirv.is_empty(), err);
		RDC::ShaderStageSPIRVData fs;
		fs.shader_stage = RDC::SHADER_STAGE_FRAGMENT;
		fs.spirv = compile_glslang_shader(RDC::SHADER_STAGE_FRAGMENT, p_fs, mtl_format.get_shader_language_version(), mtl_format.get_shader_spirv_version(RDC::SHADER_STAGE_FRAGMENT), &err);
		CHECK_MESSAGE(!fs.spirv.is_empty(), err);
		Ref<RenderingShaderContainer> container = mtl_format.create_container();
		Vector<RDC::ShaderStageSPIRVData> stages = { vs, fs };
		CHECK(container->set_code_from_spirv(p_name + "Metal", stages));
	}
#endif
}
#endif // MODULE_GLSLANG_ENABLED

TEST_CASE("[NextGeo][Displacement] Displacement shaders compile across Forward+, Forward Mobile, and Compatibility paths") {
	const String disp_shader = R"(
shader_type spatial;
uniform sampler2D height_tex;
uniform vec2 uv_scale = vec2(2.0, 2.0);
uniform float bias = 0.05;

float sample_h(vec2 uv) {
	return texture(height_tex, uv * uv_scale).r;
}

void displacement() {
	float h = sample_h(UV) + sin(dot(VERTEX, NORMAL) + TIME) * 0.02 + COLOR.r * 0.01 + INSTANCE_CUSTOM.x * 0.01 + bias;
	DISPLACEMENT = clamp(h, 0.0, 1.0);
}

void fragment() {
	ALBEDO = vec3(0.6, 0.7, 0.8);
	ROUGHNESS = 0.5;
}
)";
	const String plain_shader = R"(
shader_type spatial;
uniform sampler2D height_tex;
uniform vec2 uv_scale = vec2(2.0, 2.0);
uniform float bias = 0.05;

void fragment() {
	ALBEDO = vec3(0.6, 0.7, 0.8);
	ROUGHNESS = 0.5;
}
)";

	// Also exercise BaseMaterial3D HEIGHTMAP_MODE_DISPLACEMENT codegen across all three renderer paths.
	REQUIRE(RS::get_singleton() != nullptr);
	Ref<StandardMaterial3D> disp_std;
	disp_std.instantiate();
	disp_std->set_feature(BaseMaterial3D::FEATURE_HEIGHT_MAPPING, true);
	disp_std->set_heightmap_mode(BaseMaterial3D::HEIGHTMAP_MODE_DISPLACEMENT);
	const String std_disp_shader = RS::get_singleton()->shader_get_code(disp_std->get_shader_rid());
	REQUIRE_FALSE(std_disp_shader.is_empty());

	// 1. Forward+ (SceneForwardClusteredShaderRD): both regular color pass and NextGeo visibility-resolve pass.
	{
		String clustered_defines = "\n#define MAX_ROUGHNESS_LOD 7.0\n#define SDFGI_OCT_SIZE 8\n#define MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS 8\n#define MAX_LIGHTMAP_TEXTURES 8\n#define MAX_LIGHTMAPS 8\n#define MATERIAL_UNIFORM_SET 3\n";
		clustered_defines += RendererVirtualShadowMapStorage::get_glsl_defines();
		clustered_defines += String("\n") + RendererRD::MegaLightsInputs::glsl_pack_prelude();

		SceneForwardClusteredShaderRD clustered_shader;
		Vector<ShaderRD::VariantDefine> variants;
		variants.push_back(ShaderRD::VariantDefine(0, "", false));
		variants.push_back(ShaderRD::VariantDefine(1, "\n#define MODE_VISIBILITY_RESOLVE\n", false));
		clustered_shader.initialize(variants, clustered_defines);

		const String sources_to_check[2] = { disp_shader, std_disp_shader };
		const char *names[2] = { "ForwardClusteredCustomDisp", "ForwardClusteredStdDisp" };
		for (int s = 0; s < 2; s++) {
			CompileOutput res = compile_spatial(sources_to_check[s], true, true, false);
			REQUIRE_EQ(res.err, OK);
			CHECK(res.gen.has_displacement);
			res.gen.code.erase("raster_fragment");
			res.gen.code.erase("displacement");

			const RID ver = clustered_shader.version_create(false);
			REQUIRE(ver.is_valid());
			clustered_shader.version_set_code(ver, res.gen.code, res.gen.uniforms, res.gen.stage_globals[ShaderCompiler::STAGE_VERTEX], res.gen.stage_globals[ShaderCompiler::STAGE_FRAGMENT], res.gen.defines);

			for (int v = 0; v < 2; v++) {
				const Vector<String> stage_src = clustered_shader.version_build_variant_stage_sources(ver, v);
				REQUIRE_EQ(stage_src.size(), int(RD::SHADER_STAGE_MAX));
				if (v == 1) {
					CHECK(stage_src[RD::SHADER_STAGE_FRAGMENT].contains("_ng_eval_displacement"));
					CHECK(stage_src[RD::SHADER_STAGE_FRAGMENT].contains("textureGrad("));
				}
#ifdef MODULE_GLSLANG_ENABLED
				check_graphics_containers(String(names[s]) + (v == 0 ? "Regular" : "Resolve"), stage_src[RD::SHADER_STAGE_VERTEX], stage_src[RD::SHADER_STAGE_FRAGMENT]);
#endif
			}
			CHECK(clustered_shader.version_free(ver));
		}
	}

	// 2. Forward Mobile (SceneShaderForwardMobile compiler setup + SceneForwardMobileShaderRD).
	{
		ShaderCompiler mobile_compiler;
		init_mobile_compiler(mobile_compiler);

		auto compile_mobile = [&](const String &p_code) -> CompileOutput {
			CompileOutput out;
			ShaderCompiler::IdentifierActions actions;
			actions.entry_point_stages["vertex"] = ShaderCompiler::STAGE_VERTEX;
			actions.entry_point_stages["fragment"] = ShaderCompiler::STAGE_FRAGMENT;
			actions.entry_point_stages["light"] = ShaderCompiler::STAGE_FRAGMENT;
			actions.uniforms = &out.uniforms;
			actions.usage_flag_pointers["TIME"] = &out.uses_time;
			actions.usage_flag_pointers["NORMAL"] = &out.uses_normal;
			actions.write_flag_pointers["VERTEX"] = &out.writes_vertex;
			out.err = mobile_compiler.compile(RSE::SHADER_SPATIAL, p_code, &actions, String(), out.gen);
			return out;
		};

		const CompileOutput mob_disp = compile_mobile(disp_shader);
		const CompileOutput mob_plain = compile_mobile(plain_shader);
		REQUIRE_EQ(mob_disp.err, OK);
		REQUIRE_EQ(mob_plain.err, OK);
		CHECK(mob_disp.gen.has_displacement);
		CHECK_FALSE(mob_disp.uses_time);
		CHECK_FALSE(mob_disp.gen.uses_vertex_time);
		CHECK_FALSE(mob_disp.gen.uses_fragment_time);
		CHECK_FALSE(mob_disp.gen.code.has("displacement"));
		CHECK_FALSE(mob_disp.gen.code.has("resolve_displacement"));
		CHECK_EQ(mob_disp.gen.defines, mob_plain.gen.defines);
		CHECK_EQ(mob_disp.gen.uniforms, mob_plain.gen.uniforms);
		CHECK_EQ(mob_disp.gen.code["fragment"], mob_plain.gen.code["fragment"]);
		CHECK_EQ(mob_disp.gen.stage_globals[ShaderCompiler::STAGE_VERTEX], mob_plain.gen.stage_globals[ShaderCompiler::STAGE_VERTEX]);
		CHECK_EQ(mob_disp.gen.stage_globals[ShaderCompiler::STAGE_FRAGMENT], mob_plain.gen.stage_globals[ShaderCompiler::STAGE_FRAGMENT]);

		const CompileOutput mob_std = compile_mobile(std_disp_shader);
		REQUIRE_EQ(mob_std.err, OK);
		CHECK_FALSE(mob_std.gen.code.has("displacement"));
		CHECK_FALSE(mob_std.gen.code.has("resolve_displacement"));

		const String mobile_defines = "\n#define MAX_ROUGHNESS_LOD 7.0\n#define MAX_DIRECTIONAL_LIGHT_DATA_STRUCTS 8\n#define MAX_LIGHTMAP_TEXTURES 8\n#define MAX_LIGHTMAPS 8\n#define MATERIAL_UNIFORM_SET 3\n";
		SceneForwardMobileShaderRD mobile_shader;
		Vector<ShaderRD::VariantDefine> mob_variants;
		mob_variants.push_back(ShaderRD::VariantDefine(0, "", false));
		mobile_shader.initialize(mob_variants, mobile_defines);

		const CompileOutput *mob_outputs[2] = { &mob_disp, &mob_std };
		const char *mob_names[2] = { "ForwardMobileCustomDisp", "ForwardMobileStdDisp" };
		for (int s = 0; s < 2; s++) {
			const RID ver = mobile_shader.version_create(false);
			REQUIRE(ver.is_valid());
			mobile_shader.version_set_code(ver, mob_outputs[s]->gen.code, mob_outputs[s]->gen.uniforms, mob_outputs[s]->gen.stage_globals[ShaderCompiler::STAGE_VERTEX], mob_outputs[s]->gen.stage_globals[ShaderCompiler::STAGE_FRAGMENT], mob_outputs[s]->gen.defines);
			const Vector<String> stage_src = mobile_shader.version_build_variant_stage_sources(ver, 0);
			REQUIRE_EQ(stage_src.size(), int(RD::SHADER_STAGE_MAX));
			CHECK_FALSE(stage_src[RD::SHADER_STAGE_FRAGMENT].contains("_ng_eval_displacement"));
			CHECK_FALSE(stage_src[RD::SHADER_STAGE_FRAGMENT].contains("_ng_rd_"));
#ifdef MODULE_GLSLANG_ENABLED
			check_graphics_containers(mob_names[s], stage_src[RD::SHADER_STAGE_VERTEX], stage_src[RD::SHADER_STAGE_FRAGMENT]);
#endif
			CHECK(mobile_shader.version_free(ver));
		}
	}

	// 3. Compatibility (GLES3::MaterialStorage::shaders.compiler_scene setup).
	{
		ShaderCompiler gles3_compiler;
		init_gles3_compiler(gles3_compiler);

		auto compile_gles3 = [&](const String &p_code) -> CompileOutput {
			CompileOutput out;
			ShaderCompiler::IdentifierActions actions;
			actions.entry_point_stages["vertex"] = ShaderCompiler::STAGE_VERTEX;
			actions.entry_point_stages["fragment"] = ShaderCompiler::STAGE_FRAGMENT;
			actions.entry_point_stages["light"] = ShaderCompiler::STAGE_FRAGMENT;
			actions.uniforms = &out.uniforms;
			actions.usage_flag_pointers["TIME"] = &out.uses_time;
			actions.usage_flag_pointers["NORMAL"] = &out.uses_normal;
			actions.write_flag_pointers["VERTEX"] = &out.writes_vertex;
			out.err = gles3_compiler.compile(RSE::SHADER_SPATIAL, p_code, &actions, String(), out.gen);
			return out;
		};

		const CompileOutput gl_disp = compile_gles3(disp_shader);
		const CompileOutput gl_plain = compile_gles3(plain_shader);
		REQUIRE_EQ(gl_disp.err, OK);
		REQUIRE_EQ(gl_plain.err, OK);
		CHECK(gl_disp.gen.has_displacement);
		CHECK_FALSE(gl_disp.uses_time);
		CHECK_FALSE(gl_disp.gen.uses_vertex_time);
		CHECK_FALSE(gl_disp.gen.uses_fragment_time);
		CHECK_FALSE(gl_disp.gen.code.has("displacement"));
		CHECK_FALSE(gl_disp.gen.code.has("resolve_displacement"));
		CHECK_EQ(gl_disp.gen.defines, gl_plain.gen.defines);
		CHECK_EQ(gl_disp.gen.uniforms, gl_plain.gen.uniforms);
		CHECK_EQ(gl_disp.gen.code["fragment"], gl_plain.gen.code["fragment"]);
		CHECK_EQ(gl_disp.gen.stage_globals[ShaderCompiler::STAGE_VERTEX], gl_plain.gen.stage_globals[ShaderCompiler::STAGE_VERTEX]);
		CHECK_EQ(gl_disp.gen.stage_globals[ShaderCompiler::STAGE_FRAGMENT], gl_plain.gen.stage_globals[ShaderCompiler::STAGE_FRAGMENT]);
		CHECK_FALSE(gl_disp.gen.stage_globals[ShaderCompiler::STAGE_FRAGMENT].contains("m_sample_h"));

		const CompileOutput gl_std = compile_gles3(std_disp_shader);
		REQUIRE_EQ(gl_std.err, OK);
		CHECK(gl_std.gen.has_displacement);
		CHECK_FALSE(gl_std.uses_time);
		CHECK_FALSE(gl_std.gen.code.has("displacement"));
		CHECK_FALSE(gl_std.gen.code.has("resolve_displacement"));
		CHECK_FALSE(gl_std.gen.stage_globals[ShaderCompiler::STAGE_FRAGMENT].contains("_ng_rd_"));
	}
}

} // namespace TestDisplacementMaterial
