/**************************************************************************/
/*  test_shader_compiler_resolve.cpp                                      */
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

TEST_FORCE_LINK(test_shader_compiler_resolve)

#include "servers/rendering/shader_compiler.h"
#include "servers/rendering/shader_types.h"

namespace {

struct ResolveCompileResult {
	Error err = FAILED;
	ShaderCompiler::GeneratedCode gen;
	HashMap<StringName, ShaderLanguage::ShaderNode::Uniform> uniforms;
	bool writes_vertex = false;
	bool writes_position = false;
};

static void _init_spatial_resolve_compiler(ShaderCompiler &r_compiler) {
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
	actions.renames["VIEW"] = "view";
	actions.renames["SCREEN_UV"] = "screen_uv";
	actions.renames["POINT_COORD"] = "gl_PointCoord";
	actions.renames["FRAGCOORD"] = "gl_FragCoord";
	actions.renames["FRONT_FACING"] = "gl_FrontFacing";
	actions.renames["TIME"] = "global_time";
	actions.renames["ALBEDO"] = "albedo_highp";
	actions.renames["ROUGHNESS"] = "roughness";
	actions.renames["INSTANCE_ID"] = "INSTANCE_INDEX";
	actions.renames["VERTEX_ID"] = "VERTEX_INDEX";

	actions.base_texture_binding_index = 1;
	actions.texture_layout_set = 3;
	actions.base_uniform_string = "material.";
	actions.base_varying_index = 15;
	actions.default_filter = ShaderLanguage::FILTER_LINEAR_MIPMAP;
	actions.default_repeat = ShaderLanguage::REPEAT_ENABLE;

	actions.resolve_builtin_derivatives["VERTEX"] = Pair<String, String>("vertex_interp_ddx", "vertex_interp_ddy");
	actions.resolve_builtin_derivatives["UV"] = Pair<String, String>("uv_interp_ddx", "uv_interp_ddy");
	actions.resolve_builtin_derivatives["UV2"] = Pair<String, String>("uv2_interp_ddx", "uv2_interp_ddy");
	actions.resolve_builtin_derivatives["COLOR"] = Pair<String, String>("color_interp_ddx", "color_interp_ddy");
	actions.resolve_builtin_derivatives["VIEW"] = Pair<String, String>("view_ddx", "view_ddy");
	actions.resolve_builtin_derivatives["SCREEN_UV"] = Pair<String, String>("vec2(scene_data_block.data.screen_pixel_size.x, 0.0)", "vec2(0.0, scene_data_block.data.screen_pixel_size.y)");
	actions.resolve_builtin_derivatives["FRAGCOORD"] = Pair<String, String>("_ng_fragcoord_ddx", "_ng_fragcoord_ddy");
	actions.resolve_builtin_derivatives["POINT_COORD"] = Pair<String, String>("vec2(0.0)", "vec2(0.0)");

	actions.resolve_mutable_builtin_derivatives["NORMAL"] = Pair<String, String>("normal_interp_ddx", "normal_interp_ddy");
	actions.resolve_mutable_builtin_derivatives["TANGENT"] = Pair<String, String>("tangent_interp_ddx", "tangent_interp_ddy");
	actions.resolve_mutable_builtin_derivatives["BINORMAL"] = Pair<String, String>("binormal_interp_ddx", "binormal_interp_ddy");

	actions.resolve_uniform_builtins.insert("TIME");
	actions.resolve_uniform_builtins.insert("MODEL_MATRIX");
	actions.resolve_uniform_builtins.insert("VIEW_MATRIX");
	actions.resolve_uniform_builtins.insert("PROJECTION_MATRIX");

	actions.resolve_renames["FRONT_FACING"] = "_ng_front_facing";
	actions.resolve_renames["FRAGCOORD"] = "_ng_fragcoord";
	actions.resolve_vertex_renames["VERTEX_ID"] = "_ng_vertex_id";
	actions.resolve_vertex_renames["INSTANCE_ID"] = "_ng_instance_id";

	r_compiler.initialize(actions);
}

static ResolveCompileResult _compile_spatial_resolve(const String &p_code) {
	if (ShaderTypes::get_singleton() == nullptr) {
		memnew(ShaderTypes);
	}
	ShaderCompiler compiler;
	_init_spatial_resolve_compiler(compiler);

	ResolveCompileResult res;
	ShaderCompiler::IdentifierActions ia;
	ia.entry_point_stages["vertex"] = ShaderCompiler::STAGE_VERTEX;
	ia.entry_point_stages["fragment"] = ShaderCompiler::STAGE_FRAGMENT;
	ia.entry_point_stages["light"] = ShaderCompiler::STAGE_FRAGMENT;
	ia.write_flag_pointers["VERTEX"] = &res.writes_vertex;
	ia.write_flag_pointers["POSITION"] = &res.writes_position;
	ia.uniforms = &res.uniforms;
	ia.visibility_resolve = true;

	res.err = compiler.compile(RSE::SHADER_SPATIAL, p_code, &ia, "test_resolve.gdshader", res.gen);
	return res;
}

} // namespace

TEST_CASE("[NextGeo][ShaderCompilerResolve] Rewrites implicit-LOD texture calls to textureGrad with analytic derivatives") {
	const String code = R"(
shader_type spatial;
uniform sampler2D albedo_tex;
uniform sampler2D proj_tex;
uniform sampler2D lod_tex;

void fragment() {
	vec4 a = texture(albedo_tex, UV * 2.0 + vec2(0.25, 0.5));
	vec4 b = textureProj(proj_tex, vec3(UV, 1.0));
	vec4 c = textureLod(lod_tex, UV, 2.0);
	ALBEDO = a.rgb * b.rgb * c.rgb;
}
)";

	const ResolveCompileResult res = _compile_spatial_resolve(code);
	REQUIRE_EQ(res.err, OK);
	CHECK_FALSE(res.gen.uses_unsupported_derivative);
	CHECK_FALSE(res.writes_vertex);
	CHECK(res.gen.code.has("resolve_fragment"));

	const String &frag = res.gen.code["resolve_fragment"];
	CHECK(frag.contains("textureGrad("));
	CHECK(frag.contains("textureProjGrad("));
	CHECK(frag.contains("textureLod("));
	CHECK(frag.contains("uv_interp_ddx"));
	CHECK(frag.contains("uv_interp_ddy"));
	CHECK_FALSE(frag.contains("texture(sampler2D("));
}

TEST_CASE("[NextGeo][ShaderCompilerResolve] Runs non-position vertex() per corner and interpolates custom varyings") {
	const String code = R"(
shader_type spatial;
uniform sampler2D detail_tex;
varying vec2 v_detail_uv;
varying flat int v_corner_id;

void vertex() {
	UV = UV * 3.0;
	v_detail_uv = VERTEX.xz * 0.5 + UV;
	v_corner_id = VERTEX_ID;
}

void fragment() {
	vec2 grad_sum = dFdx(v_detail_uv) + dFdy(UV);
	ALBEDO = texture(detail_tex, v_detail_uv).rgb + vec3(grad_sum, float(v_corner_id));
}
)";

	const ResolveCompileResult res = _compile_spatial_resolve(code);
	REQUIRE_EQ(res.err, OK);
	CHECK_FALSE(res.gen.uses_unsupported_derivative);
	CHECK_FALSE(res.writes_vertex);
	CHECK_FALSE(res.writes_position);

	const String &frag_globals = res.gen.stage_globals[ShaderCompiler::STAGE_FRAGMENT];
	CHECK(frag_globals.contains("m_v_detail_uv"));
	CHECK(frag_globals.contains("_ng_dx_m_v_detail_uv"));
	CHECK(frag_globals.contains("_ng_dy_m_v_detail_uv"));
	REQUIRE(res.gen.code.has("resolve_vertex_store"));
	CHECK(res.gen.code["resolve_vertex_store"].contains("_ng_corner == 0u"));
	CHECK(res.gen.code["resolve_vertex_store"].contains("_ng_v0_m_v_detail_uv"));
	REQUIRE(res.gen.code.has("resolve_vertex"));
	CHECK(res.gen.code["resolve_vertex"].contains("_ng_vertex_id"));
	REQUIRE(res.gen.code.has("resolve_interpolate"));
	CHECK(res.gen.code["resolve_interpolate"].contains("_ng_bary"));
	CHECK(res.gen.code["resolve_interpolate"].contains("_ng_dbary_dx"));
	CHECK(res.gen.code["resolve_interpolate"].contains("_ng_dbary_dy"));
	REQUIRE(res.gen.code.has("resolve_fragment"));
	CHECK(res.gen.code["resolve_fragment"].contains("textureGrad("));
	CHECK_FALSE(res.gen.code["resolve_fragment"].contains("dFdx("));
	CHECK_FALSE(res.gen.code["resolve_fragment"].contains("dFdy("));
}

TEST_CASE("[NextGeo][ShaderCompilerResolve] Flags non-varying dFdx/dFdy/fwidth, textureQueryLod, and position-writing vertex()") {
	// Uniform derivative -> UNSUPPORTED_DERIVATIVE.
	{
		const ResolveCompileResult res = _compile_spatial_resolve(R"(
shader_type spatial;
uniform float factor = 0.5;
void fragment() {
	ALBEDO = vec3(abs(dFdx(factor)));
}
)");
		REQUIRE_EQ(res.err, OK);
		CHECK(res.gen.uses_unsupported_derivative);
	}

	// TIME derivative -> UNSUPPORTED_DERIVATIVE.
	{
		const ResolveCompileResult res = _compile_spatial_resolve(R"(
shader_type spatial;
void fragment() {
	ALBEDO = vec3(fwidth(sin(TIME)));
}
)");
		REQUIRE_EQ(res.err, OK);
		CHECK(res.gen.uses_unsupported_derivative);
	}

	// Derivative of a texture sample -> UNSUPPORTED_DERIVATIVE.
	{
		const ResolveCompileResult res = _compile_spatial_resolve(R"(
shader_type spatial;
uniform sampler2D tex;
void fragment() {
	ALBEDO = dFdx(texture(tex, UV).rgb);
}
)");
		REQUIRE_EQ(res.err, OK);
		CHECK(res.gen.uses_unsupported_derivative);
	}

	// textureQueryLod -> UNSUPPORTED_DERIVATIVE.
	{
		const ResolveCompileResult res = _compile_spatial_resolve(R"(
shader_type spatial;
uniform sampler2D tex;
void fragment() {
	ALBEDO = vec3(textureQueryLod(tex, UV), 0.0);
}
)");
		REQUIRE_EQ(res.err, OK);
		CHECK(res.gen.uses_unsupported_derivative);
	}

	// Position-altering vertex() -> writes_vertex.
	{
		const ResolveCompileResult res = _compile_spatial_resolve(R"(
shader_type spatial;
void vertex() {
	VERTEX.y += 0.1;
}
void fragment() {
	ALBEDO = vec3(1.0);
}
)");
		REQUIRE_EQ(res.err, OK);
		CHECK(res.writes_vertex);
	}
}
