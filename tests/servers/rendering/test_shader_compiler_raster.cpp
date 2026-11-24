/**************************************************************************/
/*  test_shader_compiler_raster.cpp                                       */
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

TEST_FORCE_LINK(test_shader_compiler_raster)

// GodotNext: nextgeo_raster_bins (#155, plan §21.4 N3, N4). The material raster target: one material per
// raster feature compiles to raster code, each built-in the target rejects gives the reason
// UNSUPPORTED_RASTER_FEATURE, and materials without a raster program stay in the fixed-function bin.

#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_raster_bins.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/raster_material_hw.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/raster_material_sw.glsl.gen.h"
#include "servers/rendering/rendering_shader_container.h"
#include "servers/rendering/shader_compiler.h"
#include "servers/rendering/shader_types.h"
#include "servers/rendering/storage/virtual_shadow_map_storage.h"

#include "modules/modules_enabled.gen.h"

#ifdef MODULE_GLSLANG_ENABLED
#include "modules/glslang/shader_compile.h"
#endif
#ifdef TOOLS_ENABLED
#include "editor/export/shader_baker/shader_baker_export_plugin.h"
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

namespace TestShaderCompilerRaster {

struct RasterCompileResult {
	Error err = FAILED;
	ShaderCompiler::GeneratedCode gen;
	HashMap<StringName, ShaderLanguage::ShaderNode::Uniform> uniforms;
	bool alpha_clip = false;
	bool alpha = false;
	bool discard = false;
	bool writes_vertex = false;
	bool writes_position = false;
	bool writes_depth = false;
	int cull_mode = RSE::CULL_MODE_BACK;

	// SceneShaderForwardClustered::ShaderData::set_code()'s classification, for an opaque material.
	uint32_t features() const {
		NextGeoRasterFacts facts;
		facts.alpha_scissor = alpha_clip;
		facts.discard = discard;
		facts.writes_vertex = writes_vertex;
		facts.cull_mode = RSE::CullMode(cull_mode);
		return nextgeo_raster_features(facts);
	}

	uint32_t rejects() const {
		const uint32_t uses = gen.raster_uses | (writes_depth ? uint32_t(ShaderCompiler::RASTER_WRITES_DEPTH) : 0u);
		return nextgeo_raster_rejects(uses, features());
	}

	NextGeoFallback fallback(bool p_admit_programmable) const {
		NextGeoMaterialFallbackFacts facts;
		facts.transparent = alpha && !alpha_clip;
		facts.screen_reads = gen.uses_screen_texture || gen.uses_depth_texture || gen.uses_normal_roughness_texture;
		facts.masked = (features() & RendererVirtualMeshStorage::RASTER_FEATURE_MASKED) != 0u;
		facts.writes_vertex = writes_vertex;
		facts.other_custom_vertex = writes_position || writes_depth;
		facts.raster_rejects = rejects();
		facts.admit_programmable = p_admit_programmable;
		return nextgeo_material_fallback(facts);
	}
};

// The scene shader's names for the built-ins the tests use (scene_shader_forward_clustered.cpp).
void init_spatial_raster_compiler(ShaderCompiler &r_compiler) {
	ShaderCompiler::DefaultIdentifierActions actions;

	actions.renames["MODEL_MATRIX"] = "read_model_matrix";
	actions.renames["VIEW_MATRIX"] = "read_view_matrix";
	actions.renames["INV_VIEW_MATRIX"] = "inv_view_matrix";
	actions.renames["PROJECTION_MATRIX"] = "projection_matrix";
	actions.renames["VERTEX"] = "vertex";
	actions.renames["NORMAL"] = "normal_highp";
	actions.renames["POSITION"] = "position";
	actions.renames["UV"] = "uv_interp";
	actions.renames["COLOR"] = "color_interp";
	actions.renames["SCREEN_UV"] = "screen_uv";
	actions.renames["FRAGCOORD"] = "gl_FragCoord";
	actions.renames["FRONT_FACING"] = "gl_FrontFacing";
	actions.renames["TIME"] = "global_time";
	actions.renames["ALBEDO"] = "albedo_highp";
	actions.renames["ALPHA"] = "alpha_highp";
	actions.renames["ALPHA_SCISSOR_THRESHOLD"] = "alpha_scissor_threshold";
	actions.renames["ALPHA_HASH_SCALE"] = "alpha_hash_scale";
	actions.renames["DEPTH"] = "gl_FragDepth";
	actions.renames["VERTEX_ID"] = "VERTEX_INDEX";
	actions.renames["LIGHT_COLOR"] = "light_color_highp";
	actions.renames["ATTENUATION"] = "attenuation_highp";
	actions.renames["DIFFUSE_LIGHT"] = "diffuse_light_highp";

	actions.usage_defines["ALPHA_SCISSOR_THRESHOLD"] = "#define ALPHA_SCISSOR_USED\n";
	actions.usage_defines["ALPHA_HASH_SCALE"] = "#define ALPHA_HASH_USED\n";
	actions.usage_defines["DISCARD"] = "#define DISCARD_USED\n";
	actions.usage_defines["UV"] = "#define UV_USED\n";
	actions.usage_defines["NORMAL"] = "#define NORMAL_USED\n";

	actions.base_texture_binding_index = 1;
	actions.texture_layout_set = 3;
	actions.base_uniform_string = "material.";
	actions.base_varying_index = 15;
	actions.default_filter = ShaderLanguage::FILTER_LINEAR_MIPMAP;
	actions.default_repeat = ShaderLanguage::REPEAT_ENABLE;

	actions.resolve_builtin_derivatives["VERTEX"] = Pair<String, String>("vertex_interp_ddx", "vertex_interp_ddy");
	actions.resolve_builtin_derivatives["UV"] = Pair<String, String>("uv_interp_ddx", "uv_interp_ddy");
	actions.resolve_builtin_derivatives["COLOR"] = Pair<String, String>("color_interp_ddx", "color_interp_ddy");
	actions.resolve_builtin_derivatives["SCREEN_UV"] = Pair<String, String>("vec2(scene_data_block.data.screen_pixel_size.x, 0.0)", "vec2(0.0, scene_data_block.data.screen_pixel_size.y)");
	actions.resolve_builtin_derivatives["FRAGCOORD"] = Pair<String, String>("_ng_fragcoord_ddx", "_ng_fragcoord_ddy");
	actions.resolve_mutable_builtin_derivatives["NORMAL"] = Pair<String, String>("normal_interp_ddx", "normal_interp_ddy");
	actions.resolve_uniform_builtins.insert("TIME");
	actions.resolve_uniform_builtins.insert("MODEL_MATRIX");
	actions.resolve_renames["FRONT_FACING"] = "_ng_front_facing";
	actions.resolve_renames["FRAGCOORD"] = "_ng_fragcoord";
	actions.resolve_vertex_renames["VERTEX_ID"] = "_ng_vertex_id";

	r_compiler.initialize(actions);
}

RasterCompileResult compile_spatial_raster(const String &p_code) {
	if (ShaderTypes::get_singleton() == nullptr) {
		memnew(ShaderTypes);
	}
	ShaderCompiler compiler;
	init_spatial_raster_compiler(compiler);

	RasterCompileResult res;
	ShaderCompiler::IdentifierActions ia;
	ia.entry_point_stages["vertex"] = ShaderCompiler::STAGE_VERTEX;
	ia.entry_point_stages["fragment"] = ShaderCompiler::STAGE_FRAGMENT;
	ia.entry_point_stages["light"] = ShaderCompiler::STAGE_FRAGMENT;
	ia.usage_flag_pointers["ALPHA"] = &res.alpha;
	ia.usage_flag_pointers["ALPHA_SCISSOR_THRESHOLD"] = &res.alpha_clip;
	ia.usage_flag_pointers["ALPHA_HASH_SCALE"] = &res.alpha_clip;
	ia.usage_flag_pointers["DISCARD"] = &res.discard;
	ia.write_flag_pointers["VERTEX"] = &res.writes_vertex;
	ia.write_flag_pointers["POSITION"] = &res.writes_position;
	ia.write_flag_pointers["DEPTH"] = &res.writes_depth;
	ia.render_mode_values["cull_disabled"] = Pair<int *, int>(&res.cull_mode, RSE::CULL_MODE_DISABLED);
	ia.render_mode_values["cull_front"] = Pair<int *, int>(&res.cull_mode, RSE::CULL_MODE_FRONT);
	ia.uniforms = &res.uniforms;
	ia.visibility_resolve = true;
	ia.raster_target = true;

	res.err = compiler.compile(RSE::SHADER_SPATIAL, p_code, &ia, "test_raster.gdshader", res.gen);
	return res;
}

} // namespace TestShaderCompilerRaster

using namespace TestShaderCompilerRaster;

TEST_CASE("[NextGeo][ShaderCompilerRaster] A fixed-function material has no raster program") {
	const RasterCompileResult res = compile_spatial_raster(R"(
shader_type spatial;
uniform sampler2D albedo_tex;
void fragment() {
	ALBEDO = texture(albedo_tex, UV).rgb;
}
)");
	REQUIRE_EQ(res.err, OK);
	CHECK_EQ(res.features(), 0u);
	CHECK_FALSE(nextgeo_raster_has_program(res.features()));
	CHECK_EQ(res.rejects(), 0u);
	CHECK_EQ(res.fallback(false), NEXTGEO_FALLBACK_NONE);
	CHECK_EQ(RendererVirtualMeshStorage::resolve_raster_bin(res.features(), RendererVirtualMeshStorage::RASTER_PASS_VISIBILITY), int32_t(RendererVirtualMeshStorage::RASTER_BIN_FIXED));
	// The raster target is emitted for every material; only materials with a raster program use it.
	CHECK(res.gen.code.has("raster_fragment"));
	CHECK_EQ(res.gen.raster_uses, 0u);
}

TEST_CASE("[NextGeo][ShaderCompilerRaster] Each raster feature compiles to raster code") {
	SUBCASE("MASKED: alpha scissor with a textured alpha") {
		const RasterCompileResult res = compile_spatial_raster(R"(
shader_type spatial;
uniform sampler2D mask_tex;
uniform float cutoff = 0.5;
void fragment() {
	ALBEDO = vec3(1.0);
	ALPHA = texture(mask_tex, UV).a;
	ALPHA_SCISSOR_THRESHOLD = cutoff;
}
)");
		REQUIRE_EQ(res.err, OK);
		CHECK_EQ(res.features(), uint32_t(RendererVirtualMeshStorage::RASTER_FEATURE_MASKED));
		CHECK(nextgeo_raster_has_program(res.features()));
		CHECK_EQ(res.rejects(), 0u);
		REQUIRE(res.gen.code.has("raster_fragment"));
		const String &frag = res.gen.code["raster_fragment"];
		CHECK(frag.contains("alpha_highp"));
		CHECK(frag.contains("alpha_scissor_threshold"));
		// #80's rewrite: implicit-LOD sampling takes analytic derivatives.
		CHECK(frag.contains("textureGrad("));
		CHECK(frag.contains("uv_interp_ddx"));
		CHECK(res.gen.defines.has("#define ALPHA_SCISSOR_USED\n"));
		// The compute and hardware templates take the compute globals: the texture and the resolve's
		// helpers.
		CHECK(res.gen.stage_globals[ShaderCompiler::STAGE_COMPUTE].contains("m_mask_tex"));
		CHECK_EQ(RendererVirtualMeshStorage::resolve_raster_bin(res.features(), RendererVirtualMeshStorage::RASTER_PASS_VISIBILITY), int32_t(RendererVirtualMeshStorage::RASTER_BIN_PROGRAMMABLE));
		CHECK_EQ(RendererVirtualMeshStorage::resolve_raster_bin(res.features(), RendererVirtualMeshStorage::RASTER_PASS_VSM), int32_t(RendererVirtualMeshStorage::RASTER_BIN_PROGRAMMABLE));
	}

	SUBCASE("MASKED: alpha hash") {
		const RasterCompileResult res = compile_spatial_raster(R"(
shader_type spatial;
void fragment() {
	ALPHA = UV.x;
	ALPHA_HASH_SCALE = 1.0;
}
)");
		REQUIRE_EQ(res.err, OK);
		CHECK_EQ(res.features(), uint32_t(RendererVirtualMeshStorage::RASTER_FEATURE_MASKED));
		CHECK(res.gen.defines.has("#define ALPHA_HASH_USED\n"));
		CHECK(res.gen.code["raster_fragment"].contains("alpha_hash_scale"));
	}

	SUBCASE("MASKED: discard becomes the raster target's discard") {
		const RasterCompileResult res = compile_spatial_raster(R"(
shader_type spatial;
void fragment() {
	if (fract(UV.x * 8.0) < 0.5) {
		discard;
	}
	ALBEDO = vec3(1.0);
}
)");
		REQUIRE_EQ(res.err, OK);
		CHECK_EQ(res.features(), uint32_t(RendererVirtualMeshStorage::RASTER_FEATURE_MASKED));
		const String &raster = res.gen.code["raster_fragment"];
		CHECK(raster.contains("_NG_RASTER_DISCARD;"));
		CHECK_FALSE(raster.contains("discard;"));
		// The resolve wraps fragment() discard in !_ng_skip_masked_discard (#153).
		CHECK(res.gen.code["resolve_fragment"].contains("if (!_ng_skip_masked_discard) { discard; }"));
		CHECK_FALSE(res.gen.code["resolve_fragment"].contains("_NG_RASTER_DISCARD"));
	}

	SUBCASE("VERTEX_OFFSET: vertex() moves VERTEX, run at each corner") {
		const RasterCompileResult res = compile_spatial_raster(R"(
shader_type spatial;
uniform float amount = 0.1;
void vertex() {
	VERTEX += NORMAL * amount * sin(TIME);
}
void fragment() {
	ALBEDO = vec3(1.0);
}
)");
		REQUIRE_EQ(res.err, OK);
		CHECK_EQ(res.features(), uint32_t(RendererVirtualMeshStorage::RASTER_FEATURE_VERTEX_OFFSET));
		CHECK(nextgeo_raster_has_program(res.features()));
		REQUIRE(res.gen.code.has("resolve_vertex"));
		CHECK(res.gen.code["resolve_vertex"].contains("vertex"));
		CHECK(res.gen.code["resolve_vertex"].contains("material.m_amount"));
		CHECK_EQ(res.fallback(false), NEXTGEO_FALLBACK_UNSUPPORTED_CUSTOM_SHADER);
		CHECK_EQ(res.fallback(true), NEXTGEO_FALLBACK_NONE);
	}

	SUBCASE("TWO_SIDED: a raster-bin bit with no raster program") {
		const RasterCompileResult res = compile_spatial_raster(R"(
shader_type spatial;
render_mode cull_disabled;
void fragment() {
	ALBEDO = vec3(1.0);
}
)");
		REQUIRE_EQ(res.err, OK);
		CHECK_EQ(res.features(), uint32_t(RendererVirtualMeshStorage::RASTER_FEATURE_TWO_SIDED));
		CHECK_FALSE(nextgeo_raster_has_program(res.features()));
		CHECK_EQ(res.fallback(false), NEXTGEO_FALLBACK_NONE);
		CHECK_EQ(RendererVirtualMeshStorage::resolve_raster_bin(res.features(), RendererVirtualMeshStorage::RASTER_PASS_VISIBILITY), int32_t(RendererVirtualMeshStorage::RASTER_BIN_FIXED));
	}

	SUBCASE("Custom UVs from vertex() feed masked alpha") {
		const RasterCompileResult res = compile_spatial_raster(R"(
shader_type spatial;
uniform sampler2D mask_tex;
varying vec2 mask_uv;
void vertex() {
	mask_uv = UV * 4.0;
}
void fragment() {
	ALPHA = texture(mask_tex, mask_uv).r;
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
)");
		REQUIRE_EQ(res.err, OK);
		CHECK_EQ(res.features(), uint32_t(RendererVirtualMeshStorage::RASTER_FEATURE_MASKED));
		REQUIRE(res.gen.code.has("resolve_vertex_store"));
		CHECK(res.gen.code["resolve_vertex_store"].contains("_ng_v0_m_mask_uv"));
		CHECK(res.gen.code["raster_fragment"].contains("m_mask_uv"));
		CHECK(res.gen.stage_globals[ShaderCompiler::STAGE_COMPUTE].contains("_ng_v0_m_mask_uv"));
	}
}

TEST_CASE("[NextGeo][ShaderCompilerRaster] Each built-in the raster target rejects gives UNSUPPORTED_RASTER_FEATURE") {
	struct Rejected {
		const char *name;
		const char *code;
		uint32_t reject;
	};
	const Rejected rejected[] = {
		{ "FRAGCOORD in a masked material", R"(
shader_type spatial;
void fragment() {
	ALPHA = fract(FRAGCOORD.x * 0.125);
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
)",
				ShaderCompiler::RASTER_USES_FRAGCOORD },
		{ "FRAGCOORD in a material masked by discard", R"(
shader_type spatial;
float pattern(float y) {
	return fract(y * 0.25);
}
void fragment() {
	if (pattern(FRAGCOORD.y) < 0.5) {
		discard;
	}
}
)",
				ShaderCompiler::RASTER_USES_FRAGCOORD },
		{ "the screen texture in a masked material", R"(
shader_type spatial;
uniform sampler2D screen_tex : hint_screen_texture;
void fragment() {
	ALBEDO = texture(screen_tex, SCREEN_UV).rgb;
	ALPHA = UV.x;
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
)",
				ShaderCompiler::RASTER_USES_SCREEN_TEXTURE },
		{ "the depth texture in a vertex-offset material", R"(
shader_type spatial;
uniform sampler2D depth_tex : hint_depth_texture;
void vertex() {
	VERTEX *= 1.1;
}
void fragment() {
	ALBEDO = vec3(texture(depth_tex, SCREEN_UV).r);
}
)",
				ShaderCompiler::RASTER_USES_DEPTH_TEXTURE },
		{ "the absolute DEPTH in a masked material", R"(
shader_type spatial;
void fragment() {
	if (UV.x < 0.25) {
		discard;
	}
	ALBEDO = vec3(1.0);
	DEPTH = 0.5;
}
)",
				ShaderCompiler::RASTER_WRITES_DEPTH },
		{ "the absolute DEPTH written through an out parameter", R"(
shader_type spatial;
void set_depth_out(out float d) {
	d = 0.5;
}
void fragment() {
	ALPHA = UV.x;
	ALPHA_SCISSOR_THRESHOLD = 0.5;
	set_depth_out(DEPTH);
}
)",
				ShaderCompiler::RASTER_WRITES_DEPTH },
		{ "the absolute DEPTH written through an inout parameter", R"(
shader_type spatial;
void adjust_depth(inout float d) {
	d *= 0.5;
}
void fragment() {
	ALPHA = UV.x;
	ALPHA_SCISSOR_THRESHOLD = 0.5;
	adjust_depth(DEPTH);
}
)",
				ShaderCompiler::RASTER_WRITES_DEPTH },
	};
	// hint_normal_roughness_texture parses only under the Forward+ renderer, which the doctests do not
	// run; ShaderCompiler reports it as GeneratedCode::uses_normal_roughness_texture, and the classifier's
	// doctest and tests/nextgeo_raster_bins cover its reject.
	for (const Rejected &entry : rejected) {
		const RasterCompileResult res = compile_spatial_raster(String(entry.code));
		REQUIRE_MESSAGE(res.err == OK, String(entry.name));
		CHECK_MESSAGE((res.rejects() & entry.reject) != 0u, String(entry.name));
		CHECK_MESSAGE(res.fallback(false) == NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE, String(entry.name));
		CHECK_MESSAGE(res.fallback(true) == NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE, String(entry.name));
		CHECK_MESSAGE(!nextgeo_raster_reject_names(res.rejects()).is_empty(), String(entry.name));
	}
	CHECK_EQ(String(nextgeo_fallback_name(NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE)), String("UNSUPPORTED_RASTER_FEATURE"));
	CHECK_EQ(nextgeo_raster_reject_names(ShaderCompiler::RASTER_USES_FRAGCOORD | ShaderCompiler::RASTER_WRITES_DEPTH), String("FRAGCOORD, DEPTH"));
}

TEST_CASE("[NextGeo][ShaderCompilerRaster] Without a raster program the rejected built-ins keep #80's reasons") {
	{
		const RasterCompileResult res = compile_spatial_raster(R"(
shader_type spatial;
uniform sampler2D screen_tex : hint_screen_texture;
void fragment() {
	ALBEDO = texture(screen_tex, SCREEN_UV).rgb;
}
)");
		REQUIRE_EQ(res.err, OK);
		CHECK((res.gen.raster_uses & ShaderCompiler::RASTER_USES_SCREEN_TEXTURE) != 0u);
		CHECK_EQ(res.rejects(), 0u);
		CHECK_EQ(res.fallback(false), NEXTGEO_FALLBACK_ALPHA);
	}
	{
		const RasterCompileResult res = compile_spatial_raster(R"(
shader_type spatial;
void fragment() {
	ALBEDO = vec3(1.0);
	DEPTH = 0.5;
}
)");
		REQUIRE_EQ(res.err, OK);
		CHECK((res.gen.raster_uses & ShaderCompiler::RASTER_WRITES_DEPTH) != 0u);
		CHECK_EQ(res.rejects(), 0u);
		CHECK_EQ(res.fallback(false), NEXTGEO_FALLBACK_UNSUPPORTED_CUSTOM_SHADER);
	}
}

TEST_CASE("[NextGeo][ShaderCompilerRaster] Helper and light() discards reject cleanly with UNSUPPORTED_RASTER_FEATURE") {
	// Helper containing discard called from fragment(): rejected with RASTER_USES_FUNCTION_DISCARD, no raster_fragment emitted.
	{
		const RasterCompileResult res = compile_spatial_raster(R"(
shader_type spatial;
void clip_stripes(float x) {
	if (fract(x * 8.0) < 0.5) {
		discard;
	}
}
void fragment() {
	clip_stripes(UV.x);
	ALBEDO = vec3(1.0);
}
)");
		REQUIRE_EQ(res.err, OK);
		CHECK_EQ(res.features(), uint32_t(RendererVirtualMeshStorage::RASTER_FEATURE_MASKED));
		CHECK((res.gen.raster_uses & ShaderCompiler::RASTER_USES_FUNCTION_DISCARD) != 0u);
		CHECK_EQ(res.rejects(), uint32_t(ShaderCompiler::RASTER_USES_FUNCTION_DISCARD));
		CHECK_EQ(res.fallback(false), NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE);
		CHECK_EQ(res.fallback(true), NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE);
		CHECK_FALSE(res.gen.code.has("raster_fragment"));
		CHECK_FALSE(res.gen.stage_globals[ShaderCompiler::STAGE_COMPUTE].contains("discard;"));
	}
	// discard directly in light(): rejected with RASTER_USES_LIGHT_DISCARD, never treated as fixed-coverage opaque.
	{
		const RasterCompileResult res = compile_spatial_raster(R"(
shader_type spatial;
void fragment() {
	ALBEDO = vec3(1.0);
}
void light() {
	if (ATTENUATION < 0.1) {
		discard;
	}
	DIFFUSE_LIGHT += ALBEDO * LIGHT_COLOR * ATTENUATION;
}
)");
		REQUIRE_EQ(res.err, OK);
		CHECK_EQ(res.features(), uint32_t(RendererVirtualMeshStorage::RASTER_FEATURE_MASKED));
		CHECK((res.gen.raster_uses & ShaderCompiler::RASTER_USES_LIGHT_DISCARD) != 0u);
		CHECK_EQ(res.rejects(), uint32_t(ShaderCompiler::RASTER_USES_LIGHT_DISCARD));
		CHECK_EQ(res.fallback(false), NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE);
		CHECK_EQ(res.fallback(true), NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE);
		CHECK_FALSE(res.gen.stage_globals[ShaderCompiler::STAGE_COMPUTE].contains("discard;"));
	}
	// discard in a helper called from light(): rejected with RASTER_USES_LIGHT_DISCARD, and light()'s helper stays out of STAGE_COMPUTE.
	{
		const RasterCompileResult res = compile_spatial_raster(R"(
shader_type spatial;
void clip_light(float a) {
	if (a < 0.1) {
		discard;
	}
}
void fragment() {
	ALPHA = UV.x;
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
void light() {
	clip_light(ATTENUATION);
	DIFFUSE_LIGHT += ALBEDO * LIGHT_COLOR * ATTENUATION;
}
)");
		REQUIRE_EQ(res.err, OK);
		CHECK((res.gen.raster_uses & ShaderCompiler::RASTER_USES_LIGHT_DISCARD) != 0u);
		CHECK_EQ(res.rejects(), uint32_t(ShaderCompiler::RASTER_USES_LIGHT_DISCARD));
		CHECK_EQ(res.fallback(false), NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE);
		CHECK_EQ(res.fallback(true), NEXTGEO_FALLBACK_UNSUPPORTED_RASTER_FEATURE);
		CHECK_FALSE(res.gen.stage_globals[ShaderCompiler::STAGE_COMPUTE].contains("m_clip_light"));
		CHECK_FALSE(res.gen.stage_globals[ShaderCompiler::STAGE_COMPUTE].contains("discard;"));
	}
	// A discard in fragment() itself is rewritten to _NG_RASTER_DISCARD and admitted.
	const RasterCompileResult own = compile_spatial_raster(R"(
shader_type spatial;
void fragment() {
	if (fract(UV.x * 8.0) < 0.5) {
		discard;
	}
}
)");
	REQUIRE_EQ(own.err, OK);
	CHECK_EQ(own.gen.raster_uses & (ShaderCompiler::RASTER_USES_FUNCTION_DISCARD | ShaderCompiler::RASTER_USES_LIGHT_DISCARD), 0u);
	CHECK_EQ(own.fallback(true), NEXTGEO_FALLBACK_NONE);
}

TEST_CASE("[NextGeo][ShaderCompilerRaster] Fragment-to-light varyings and raster templates compile cleanly") {
	const RasterCompileResult res = compile_spatial_raster(R"(
shader_type spatial;
render_mode cull_disabled;
uniform float scale = 1.1;
varying vec2 v_uv;
varying float v_f2l;
float mask_stripe(vec2 uv) {
	return step(0.5, fract(uv.x * 6.0));
}
float light_curve(float v) {
	return 0.25 + 0.75 * v;
}
void vertex() {
	VERTEX += NORMAL * (scale - 1.0);
	v_uv = UV * 2.0;
}
void fragment() {
	v_f2l = v_uv.y;
	ALBEDO = vec3(0.8);
	ALPHA = mask_stripe(v_uv);
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
void light() {
	DIFFUSE_LIGHT += ALBEDO * LIGHT_COLOR * ATTENUATION * light_curve(v_f2l);
}
)");
	REQUIRE_EQ(res.err, OK);
	CHECK_EQ(res.rejects(), 0u);
	CHECK_EQ(res.fallback(true), NEXTGEO_FALLBACK_NONE);
	const String &compute_globals = res.gen.stage_globals[ShaderCompiler::STAGE_COMPUTE];
	CHECK(compute_globals.contains("frag_to_light;"));
	CHECK(compute_globals.contains("_ng_dx_frag_to_light;"));
	CHECK(compute_globals.contains("_ng_rf_m_mask_stripe"));
	// light() helpers stay in STAGE_FRAGMENT only, never STAGE_COMPUTE.
	CHECK_FALSE(compute_globals.contains("m_light_curve"));
	CHECK(res.gen.stage_globals[ShaderCompiler::STAGE_FRAGMENT].contains("_ng_rf_m_light_curve"));

	// Initialize SW and HW ShaderRDs with separate VIS64_IMAGE (group 0) and VIS64_BUFFER (group 1) groups,
	// expand both variants, and verify GLSL -> SPIR-V -> container compilation.
	RasterMaterialSwShaderRD sw_shader;
	RasterMaterialHwShaderRD hw_shader;
	Vector<ShaderRD::VariantDefine> raster_variants;
	raster_variants.push_back(ShaderRD::VariantDefine(0, "", false));
	raster_variants.push_back(ShaderRD::VariantDefine(1, "\n#define VIS64_BUFFER\n", false));
	sw_shader.initialize(raster_variants);
	sw_shader.set_group_int64_atomics(0, true, false);
	sw_shader.set_group_int64_atomics(1, false, true);
	hw_shader.initialize(raster_variants);
	hw_shader.set_group_int64_atomics(0, true, false);
	hw_shader.set_group_int64_atomics(1, false, true);

	CHECK_EQ(sw_shader.get_group_count(), 2);
	CHECK_FALSE(sw_shader.is_group_enabled(0));
	CHECK_FALSE(sw_shader.is_group_enabled(1));
	CHECK(sw_shader.group_requires_image_int64_atomics(0));
	CHECK_FALSE(sw_shader.group_requires_buffer_int64_atomics(0));
	CHECK_FALSE(sw_shader.group_requires_image_int64_atomics(1));
	CHECK(sw_shader.group_requires_buffer_int64_atomics(1));

	HashMap<String, String> raster_code;
	raster_code["resolve_vertex"] = res.gen.code["resolve_vertex"];
	raster_code["resolve_vertex_store"] = res.gen.code["resolve_vertex_store"];
	raster_code["resolve_interpolate"] = res.gen.code["resolve_interpolate"];
	raster_code["raster_fragment"] = res.gen.code["raster_fragment"];

	Vector<String> defines = res.gen.defines;
	defines.push_back("#define DO_SIDE_CHECK\n");

	const RID sw_ver = sw_shader.version_create(false);
	const RID hw_ver = hw_shader.version_create(false);
	REQUIRE(sw_ver.is_valid());
	REQUIRE(hw_ver.is_valid());
	sw_shader.version_set_compute_code(sw_ver, raster_code, res.gen.uniforms, compute_globals, defines);
	hw_shader.version_set_code(hw_ver, raster_code, res.gen.uniforms, compute_globals, compute_globals, defines);

	CHECK_NE(sw_shader.version_get_cache_file_relative_path(sw_ver, 0, "vulkan"), sw_shader.version_get_cache_file_relative_path(sw_ver, 1, "vulkan"));
	CHECK_NE(hw_shader.version_get_cache_file_relative_path(hw_ver, 0, "vulkan"), hw_shader.version_get_cache_file_relative_path(hw_ver, 1, "vulkan"));

	for (int variant = 0; variant < 2; variant++) {
		const Vector<String> sw_sources = sw_shader.version_build_variant_stage_sources(sw_ver, variant);
		const Vector<String> hw_sources = hw_shader.version_build_variant_stage_sources(hw_ver, variant);
		REQUIRE_EQ(sw_sources.size(), int(RD::SHADER_STAGE_MAX));
		REQUIRE_EQ(hw_sources.size(), int(RD::SHADER_STAGE_MAX));
		CHECK_EQ(sw_sources[RD::SHADER_STAGE_COMPUTE].contains("#define VIS64_BUFFER"), variant == 1);
		CHECK_EQ(hw_sources[RD::SHADER_STAGE_FRAGMENT].contains("#define VIS64_BUFFER"), variant == 1);
#if defined(MODULE_GLSLANG_ENABLED) && defined(VULKAN_ENABLED)
		RenderingShaderContainerFormatVulkan vk_format;
		String err;
		RDC::ShaderStageSPIRVData sw_spv;
		sw_spv.shader_stage = RDC::SHADER_STAGE_COMPUTE;
		sw_spv.spirv = compile_glslang_shader(RDC::SHADER_STAGE_COMPUTE, sw_sources[RD::SHADER_STAGE_COMPUTE], vk_format.get_shader_language_version(), vk_format.get_shader_spirv_version(RDC::SHADER_STAGE_COMPUTE), &err);
		CHECK_MESSAGE(!sw_spv.spirv.is_empty(), err);
		Ref<RenderingShaderContainer> sw_container = vk_format.create_container();
		Vector<RDC::ShaderStageSPIRVData> sw_stages = { sw_spv };
		CHECK(sw_container->set_code_from_spirv("RasterMaterialSwShaderRD", sw_stages));

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
		CHECK(hw_container->set_code_from_spirv("RasterMaterialHwShaderRD", hw_stages));
#endif
	}

	CHECK(sw_shader.version_free(sw_ver));
	CHECK(hw_shader.version_free(hw_ver));
}

#ifdef TOOLS_ENABLED
TEST_CASE("[NextGeo][ShaderCompilerRaster] ShaderBakerExportPlugin gates 64-bit-atomic groups on container format capabilities") {
	CHECK(ShaderBakerExportPlugin::should_bake_int64_atomic_group(false, false, false, false));
	CHECK_FALSE(ShaderBakerExportPlugin::should_bake_int64_atomic_group(true, false, false, false));
	CHECK_FALSE(ShaderBakerExportPlugin::should_bake_int64_atomic_group(true, false, false, true));
	CHECK(ShaderBakerExportPlugin::should_bake_int64_atomic_group(true, false, true, false));
	CHECK_FALSE(ShaderBakerExportPlugin::should_bake_int64_atomic_group(false, true, false, false));
	CHECK_FALSE(ShaderBakerExportPlugin::should_bake_int64_atomic_group(false, true, true, false));
	CHECK(ShaderBakerExportPlugin::should_bake_int64_atomic_group(false, true, false, true));
	CHECK_FALSE(ShaderBakerExportPlugin::should_bake_int64_atomic_group(true, true, true, false));
	CHECK(ShaderBakerExportPlugin::should_bake_int64_atomic_group(true, true, true, true));

	// Serialized container versions stay untouched.
	CHECK_EQ(RenderingShaderContainer::CONTAINER_VERSION, 6u);
#ifdef VULKAN_ENABLED
	RenderingShaderContainerFormatVulkan vk_format;
	CHECK(vk_format.supports_image_int64_atomics());
	CHECK(vk_format.supports_buffer_int64_atomics());
#endif
#ifdef METAL_ENABLED
	MetalDeviceProfile apple9_mac;
	apple9_mac.platform = MetalDeviceProfile::Platform::macOS;
	apple9_mac.gpu = MetalDeviceProfile::GPU::Apple9;
	apple9_mac.features.msl_version = 30100;
	apple9_mac.features.use_argument_buffers = true;
	CHECK(RenderingShaderContainerFormatMetal(&apple9_mac, true).supports_image_int64_atomics());
	CHECK(RenderingShaderContainerFormatMetal(&apple9_mac, true).supports_buffer_int64_atomics());

	MetalDeviceProfile apple8_mac = apple9_mac;
	apple8_mac.gpu = MetalDeviceProfile::GPU::Apple8;
	CHECK(RenderingShaderContainerFormatMetal(&apple8_mac, true).supports_image_int64_atomics());

	MetalDeviceProfile apple8_ios = apple8_mac;
	apple8_ios.platform = MetalDeviceProfile::Platform::iOS;
	CHECK_FALSE(RenderingShaderContainerFormatMetal(&apple8_ios, true).supports_image_int64_atomics());

	MetalDeviceProfile apple7_mac = apple9_mac;
	apple7_mac.gpu = MetalDeviceProfile::GPU::Apple7;
	CHECK_FALSE(RenderingShaderContainerFormatMetal(&apple7_mac, true).supports_image_int64_atomics());
#endif
#ifdef D3D12_ENABLED
	RenderingShaderContainerFormatD3D12 d3d12_format;
	CHECK_FALSE(d3d12_format.supports_image_int64_atomics());
	CHECK_FALSE(d3d12_format.supports_buffer_int64_atomics());
	CHECK_EQ(RenderingShaderContainerD3D12::FORMAT_VERSION, 4u);
#endif
}
#endif // TOOLS_ENABLED

TEST_CASE("[NextGeo][ShaderCompilerRaster] FRAGCOORD outside a raster program and in light() is not rejected") {
	// The fixed-function bin runs no material code, and the resolve emulates FRAGCOORD.
	{
		const RasterCompileResult res = compile_spatial_raster(R"(
shader_type spatial;
void fragment() {
	ALBEDO = vec3(fract(FRAGCOORD.x * 0.125));
}
)");
		REQUIRE_EQ(res.err, OK);
		CHECK((res.gen.raster_uses & ShaderCompiler::RASTER_USES_FRAGCOORD) != 0u);
		CHECK_EQ(res.rejects(), 0u);
		CHECK_EQ(res.fallback(false), NEXTGEO_FALLBACK_NONE);
	}
	// light() does not run in a raster bin.
	{
		const RasterCompileResult res = compile_spatial_raster(R"(
shader_type spatial;
void fragment() {
	ALPHA = UV.x;
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
void light() {
	DIFFUSE_LIGHT += vec3(fract(FRAGCOORD.x));
}
)");
		REQUIRE_EQ(res.err, OK);
		CHECK_EQ(res.gen.raster_uses & ShaderCompiler::RASTER_USES_FRAGCOORD, 0u);
		CHECK_EQ(res.rejects(), 0u);
	}
}

TEST_CASE("[NextGeo][ShaderCompilerRaster] Alpha hash, custom UVs, two-sided FRONT_FACING, and VSM depth-only variants compile to SPIR-V (#153)") {
	const char *cases[] = {
		// Alpha hash + cull_disabled + custom varying UV + modified built-in UV.
		R"(
shader_type spatial;
render_mode cull_disabled;
uniform sampler2D mask_tex;
varying vec2 custom_uv;
void vertex() {
	UV = UV * 2.0 + vec2(0.1, 0.2);
	custom_uv = UV * 3.0;
}
void fragment() {
	ALBEDO = FRONT_FACING ? vec3(0.2, 0.8, 0.3) : vec3(0.8, 0.2, 0.3);
	ALPHA = texture(mask_tex, custom_uv).r * texture(mask_tex, UV).a;
	ALPHA_HASH_SCALE = 1.0;
}
)",
		// Alpha scissor + cull_front + FRONT_FACING.
		R"(
shader_type spatial;
render_mode cull_front;
uniform sampler2D mask_tex;
void fragment() {
	ALBEDO = FRONT_FACING ? vec3(1.0) : vec3(0.4);
	ALPHA = texture(mask_tex, UV).a;
	ALPHA_SCISSOR_THRESHOLD = 0.5;
}
)",
	};

	RasterMaterialSwShaderRD sw_shader;
	RasterMaterialHwShaderRD hw_shader;
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

	for (int c = 0; c < 2; c++) {
		const RasterCompileResult res = compile_spatial_raster(cases[c]);
		REQUIRE_EQ(res.err, OK);
		CHECK_EQ(res.features(), uint32_t(RendererVirtualMeshStorage::RASTER_FEATURE_MASKED | RendererVirtualMeshStorage::RASTER_FEATURE_TWO_SIDED));
		CHECK_EQ(res.rejects(), 0u);
		CHECK(res.gen.code["raster_fragment"].contains("_ng_front_facing"));
		if (c == 0) {
			CHECK(res.gen.defines.has("#define ALPHA_HASH_USED\n"));
			CHECK(res.gen.code["resolve_vertex"].contains("uv_interp"));
			CHECK(res.gen.code["resolve_vertex_store"].contains("_ng_v0_m_custom_uv"));
		}

		HashMap<String, String> raster_code;
		raster_code["resolve_vertex"] = res.gen.code.has("resolve_vertex") ? res.gen.code["resolve_vertex"] : String();
		raster_code["resolve_vertex_store"] = res.gen.code.has("resolve_vertex_store") ? res.gen.code["resolve_vertex_store"] : String();
		raster_code["resolve_interpolate"] = res.gen.code.has("resolve_interpolate") ? res.gen.code["resolve_interpolate"] : String();
		raster_code["raster_fragment"] = res.gen.code.has("raster_fragment") ? res.gen.code["raster_fragment"] : String();

		Vector<String> defines = res.gen.defines;
		defines.push_back("#define DO_SIDE_CHECK\n");

		const String &compute_globals = res.gen.stage_globals[ShaderCompiler::STAGE_COMPUTE];
		const RID sw_ver = sw_shader.version_create(false);
		const RID hw_ver = hw_shader.version_create(false);
		REQUIRE(sw_ver.is_valid());
		REQUIRE(hw_ver.is_valid());
		sw_shader.version_set_compute_code(sw_ver, raster_code, res.gen.uniforms, compute_globals, defines);
		hw_shader.version_set_code(hw_ver, raster_code, res.gen.uniforms, compute_globals, compute_globals, defines);

		for (int variant = 0; variant < 3; variant++) {
			const Vector<String> sw_sources = sw_shader.version_build_variant_stage_sources(sw_ver, variant);
			const Vector<String> hw_sources = hw_shader.version_build_variant_stage_sources(hw_ver, variant);
			REQUIRE_EQ(sw_sources.size(), int(RD::SHADER_STAGE_MAX));
			REQUIRE_EQ(hw_sources.size(), int(RD::SHADER_STAGE_MAX));
			if (c == 0) {
				CHECK(sw_sources[RD::SHADER_STAGE_COMPUTE].contains("compute_alpha_hash_threshold_grad"));
				CHECK(hw_sources[RD::SHADER_STAGE_FRAGMENT].contains("compute_alpha_hash_threshold_grad"));
			}
			CHECK_EQ(sw_sources[RD::SHADER_STAGE_COMPUTE].contains("#define NEXTGEO_RASTER_DEPTH_ONLY"), variant == 2);
			CHECK_EQ(hw_sources[RD::SHADER_STAGE_FRAGMENT].contains("#define NEXTGEO_RASTER_DEPTH_ONLY"), variant == 2);
#if defined(MODULE_GLSLANG_ENABLED) && defined(VULKAN_ENABLED)
			RenderingShaderContainerFormatVulkan vk_format;
			String err;
			RDC::ShaderStageSPIRVData sw_spv;
			sw_spv.shader_stage = RDC::SHADER_STAGE_COMPUTE;
			sw_spv.spirv = compile_glslang_shader(RDC::SHADER_STAGE_COMPUTE, sw_sources[RD::SHADER_STAGE_COMPUTE], vk_format.get_shader_language_version(), vk_format.get_shader_spirv_version(RDC::SHADER_STAGE_COMPUTE), &err);
			CHECK_MESSAGE(!sw_spv.spirv.is_empty(), err);
			Ref<RenderingShaderContainer> sw_container = vk_format.create_container();
			Vector<RDC::ShaderStageSPIRVData> sw_stages = { sw_spv };
			CHECK(sw_container->set_code_from_spirv("RasterMaterialSwShaderRD", sw_stages));

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
			CHECK(hw_container->set_code_from_spirv("RasterMaterialHwShaderRD", hw_stages));
#endif
		}
		CHECK(sw_shader.version_free(sw_ver));
		CHECK(hw_shader.version_free(hw_ver));
	}
}
