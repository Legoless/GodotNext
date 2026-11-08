/**************************************************************************/
/*  test_megalights_inputs.cpp                                            */
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

TEST_FORCE_LINK(test_megalights_inputs)

#include "core/object/class_db.h"
#include "servers/rendering/renderer_rd/forward_clustered/megalights_inputs.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_forward_clustered.h"
#include "servers/rendering/rendering_server.h"

namespace {

void _restore_switch() {
	RendererRD::MegaLightsInputs::set_requested(false);
}

} // namespace

TEST_CASE("[Renderer][MegaLights] F0 pack and shading model (#136)") {
	using ML = RendererRD::MegaLightsInputs;
	float r, g, b;
	ML::f0_from_material(0.0f, 0.5f, 1.0f, 1.0f, 1.0f, r, g, b);
	CHECK(Math::is_equal_approx(r, 0.16f * 0.5f * 0.5f));
	CHECK(Math::is_equal_approx(g, r));
	CHECK(Math::is_equal_approx(b, r));

	uint8_t luma = 0;
	uint8_t tint = 0;
	ML::pack_f0(r, g, b, luma, tint);
	// Gray dielectric. Zero chroma lands on the exact center nibble.
	CHECK_EQ(luma, 10);
	CHECK_EQ(tint, uint8_t((ML::CHROMA_BIAS << 4) | ML::CHROMA_BIAS));
	CHECK_EQ(tint, 0x77);

	float ur, ug, ub;
	ML::unpack_f0(luma, tint, ur, ug, ub);
	CHECK(Math::abs(ur - ug) < (1.0f / 255.0f));
	CHECK(Math::abs(ug - ub) < (1.0f / 255.0f));
	CHECK(Math::abs(ur - r) < (1.0f / 255.0f));
	CHECK(Math::abs(ug - g) < (1.0f / 255.0f));
	CHECK(Math::abs(ub - b) < (1.0f / 255.0f));

	ML::f0_from_material(1.0f, 0.5f, 1.0f, 0.0f, 0.0f, r, g, b);
	CHECK(Math::is_equal_approx(r, 1.0f));
	CHECK(Math::is_equal_approx(g, 0.0f));
	CHECK(Math::is_equal_approx(b, 0.0f));
	ML::pack_f0(r, g, b, luma, tint);
	CHECK_EQ(luma, ML::quantize_unorm8(0.30f));
	CHECK_EQ(int(luma), 77);
	const int cb4 = ML::quantize_nibble((0.0f - 0.30f) / ML::CB_SCALE);
	const int cr4 = ML::quantize_nibble((1.0f - 0.30f) / ML::CR_SCALE);
	CHECK_EQ(tint, uint8_t((cb4 << 4) | cr4));
	CHECK_EQ(cr4, ML::CHROMA_LEVELS);

	CHECK_EQ(ML::pack_shading_model(false, false, false, false), ML::MODEL_STANDARD);
	CHECK_EQ(ML::pack_shading_model(false, true, false, false), uint8_t(ML::MODEL_SSS | ML::FLAG_SSS));
	CHECK_EQ(ML::pack_shading_model(false, true, true, false), uint8_t(ML::MODEL_SSS | ML::FLAG_SSS | ML::FLAG_TRANSMISSION));
	// Transmission without SSS is not LIGHT_TRANSMITTANCE_USED, so it stays standard plus clearcoat.
	CHECK_EQ(ML::pack_shading_model(false, false, true, true), uint8_t(ML::MODEL_CLEARCOAT | ML::FLAG_CLEARCOAT));
	CHECK_EQ(ML::pack_shading_model(false, false, false, true), uint8_t(ML::MODEL_CLEARCOAT | ML::FLAG_CLEARCOAT));
	CHECK_EQ(ML::pack_shading_model(true, true, true, true), ML::MODEL_FALLBACK);
}

TEST_CASE("[Renderer][MegaLights] Shared F0 pack rule (#136)") {
	using ML = RendererRD::MegaLightsInputs;
	CHECK_EQ((ML::CHROMA_LEVELS + 1) % 2, 1);
	CHECK_EQ(ML::CHROMA_BIAS * 2, ML::CHROMA_LEVELS);
	CHECK_EQ(ML::quantize_nibble(0.0f), ML::CHROMA_BIAS);
	CHECK_EQ(ML::quantize_nibble(0.01f), ML::CHROMA_BIAS);
	CHECK_EQ(ML::quantize_nibble(-0.01f), ML::CHROMA_BIAS);
	CHECK(Math::is_equal_approx(ML::LUMA_R, float(ML::LUMA_R_CENTI) / 100.0f));
	CHECK(Math::is_equal_approx(ML::LUMA_G, float(ML::LUMA_G_CENTI) / 100.0f));
	CHECK(Math::is_equal_approx(ML::LUMA_B, float(ML::LUMA_B_CENTI) / 100.0f));
	CHECK(Math::is_equal_approx(ML::CB_SCALE, float(ML::CB_SCALE_CENTI) / 100.0f));
	CHECK(Math::is_equal_approx(ML::CR_SCALE, float(ML::CR_SCALE_CENTI) / 100.0f));
	CHECK(Math::is_equal_approx(ML::DIELECTRIC_SCALE, float(ML::DIELECTRIC_CENTI) / 100.0f));

	const String prelude = ML::glsl_pack_prelude();
	CHECK(prelude.contains(vformat("#define ML_CHROMA_LEVELS %d\n", ML::CHROMA_LEVELS)));
	CHECK(prelude.contains(vformat("#define ML_CHROMA_BIAS %d\n", ML::CHROMA_BIAS)));
	CHECK(prelude.contains(vformat("#define ML_LUMA_R (%d.0 / 100.0)\n", ML::LUMA_R_CENTI)));
	CHECK(prelude.contains(vformat("#define ML_FLAG_CLEARCOAT %d\n", int(ML::FLAG_CLEARCOAT))));
	CHECK(prelude.contains(vformat("#define ML_FLAG_TRANSMISSION %d\n", int(ML::FLAG_TRANSMISSION))));
	CHECK(prelude.contains(vformat("#define ML_FLAG_SSS %d\n", int(ML::FLAG_SSS))));
	CHECK(prelude.contains(vformat("#define ML_MODEL_FALLBACK %d\n", int(ML::MODEL_FALLBACK))));
	CHECK(prelude.contains("#define ML_QUANTIZE_CHROMA(c) clamp(round((c) * float(ML_CHROMA_LEVELS)) + float(ML_CHROMA_BIAS), 0.0, float(ML_CHROMA_LEVELS))\n"));
}

TEST_CASE("[Renderer][MegaLights] Fallback classification (#136)") {
	using ML = RendererRD::MegaLightsInputs;
	ML::FallbackQuery query;
	CHECK_FALSE(ML::is_fallback(query));
	query.unshaded = true;
	CHECK(ML::is_fallback(query));
	query = ML::FallbackQuery();
	query.custom_light = true;
	CHECK(ML::is_fallback(query));
	query = ML::FallbackQuery();
	query.diffuse_toon = true;
	CHECK(ML::is_fallback(query));
	query = ML::FallbackQuery();
	query.diffuse_lambert_wrap = true;
	CHECK(ML::is_fallback(query));
	query = ML::FallbackQuery();
	query.specular_toon = true;
	CHECK(ML::is_fallback(query));
	query = ML::FallbackQuery();
	query.specular_disabled = true;
	CHECK(ML::is_fallback(query));
	query = ML::FallbackQuery();
	query.rim = true;
	CHECK(ML::is_fallback(query));
	query = ML::FallbackQuery();
	query.backlight = true;
	CHECK(ML::is_fallback(query));
	query = ML::FallbackQuery();
	query.anisotropy = true;
	CHECK(ML::is_fallback(query));
	query = ML::FallbackQuery();
	query.diffuse_lambert = true;
	CHECK(ML::is_fallback(query));
	query = ML::FallbackQuery();
	query.custom_light = true;
	query.diffuse_lambert_wrap = true;
	CHECK_EQ(ML::fallback_bits(query), ML::FALLBACK_CUSTOM_LIGHT | ML::FALLBACK_DIFFUSE_LAMBERT_WRAP);

	CHECK_FALSE(ML::is_diffuse_lambert(false, false, true, false, false));
	CHECK(ML::is_diffuse_lambert(false, true, false, false, false));
	CHECK(ML::is_diffuse_lambert(false, false, false, false, false));
	CHECK_FALSE(ML::is_diffuse_lambert(false, false, false, true, false));
	CHECK_FALSE(ML::is_diffuse_lambert(false, false, false, false, true));
	CHECK_FALSE(ML::is_diffuse_lambert(true, false, false, false, false));
}

TEST_CASE("[Renderer][MegaLights] Layer-group table (#136)") {
	using ML = RendererRD::MegaLightsInputs;
	ML::begin_layer_groups();
	CHECK_EQ(ML::get_layer_group_count(), 0u);
	CHECK_EQ(ML::assign_layer_group(0x1u), 0);
	CHECK_EQ(ML::assign_layer_group(0x1u), 0);
	CHECK_EQ(ML::assign_layer_group(0x2u), 1);
	for (uint32_t mask = 3; mask <= ML::LAYER_GROUP_LIMIT; mask++) {
		CHECK_EQ(ML::assign_layer_group(mask), uint8_t(mask - 1));
	}
	CHECK_EQ(ML::get_layer_group_count(), ML::LAYER_GROUP_LIMIT);
	CHECK_EQ(ML::assign_layer_group(0x00ffffffu), ML::LAYER_GROUP_OVERFLOW);
	CHECK_EQ(ML::get_layer_group_count(), ML::LAYER_GROUP_LIMIT);
	CHECK_EQ(ML::assign_layer_group(0x1u), 0);
	CHECK_EQ(ML::get_layer_group_masks().size(), ML::LAYER_GROUP_LIMIT);
	ML::begin_layer_groups();
	CHECK_EQ(ML::get_layer_group_count(), 0u);
}

TEST_CASE("[Renderer][MegaLights] Switch and MSAA gate (#136)") {
	using ML = RendererRD::MegaLightsInputs;
	ML::set_requested(false);
	CHECK_FALSE(ML::is_requested());
	CHECK_FALSE(ML::view_writes_inputs(false));
	CHECK_FALSE(ML::view_writes_inputs(true));

	ML::set_requested(true);
	CHECK(ML::is_requested());
	CHECK(ML::view_writes_inputs(false));
	CHECK_FALSE(ML::view_writes_inputs(true));
	_restore_switch();
	CHECK_FALSE(ML::view_writes_inputs(false));
}

TEST_CASE("[Renderer][MegaLights] Shader version layout (#136)") {
	using Shader = RendererSceneRenderImplementation::SceneShaderForwardClustered;
	using Version = Shader::ShaderVersion;
	CHECK_EQ(int(Version::SHADER_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS), int(Version::SHADER_VERSION_VISIBILITY_RESOLVE_BASE + Version::SHADER_VERSION_VISIBILITY_RESOLVE_COUNT));
	CHECK_EQ(int(Version::SHADER_VERSION_MEGALIGHTS_INPUTS_COUNT), 10);
	CHECK_EQ(int(Version::SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI_ML), int(Version::SHADER_VERSION_MEGALIGHTS_INPUTS_BASE + 9));
	const int mask_count = int(Version::SHADER_VERSION_COLOR_PASS) * 2 + int(Shader::SHADER_COLOR_PASS_FLAG_COUNT) + int(Version::SHADER_VERSION_VISIBILITY_RESOLVE_COUNT) + int(Version::SHADER_VERSION_MEGALIGHTS_INPUTS_COUNT) + int(Version::SHADER_VERSION_MEGALIGHTS_INPUTS_UBER_COUNT);
	CHECK_EQ(int(Version::SHADER_VERSION_MEGALIGHTS_INPUTS_UBER_COUNT), 8);
	CHECK_EQ(int(Shader::ShaderData::VERTEX_INPUT_MASKS_SIZE), mask_count);
	CHECK_EQ(mask_count, 86);
	CHECK_FALSE(Version::MEGALIGHTS_VARIANTS_DEFAULT_ENABLED);

	const int ml_count = int(Version::SHADER_VERSION_MEGALIGHTS_INPUTS_COUNT + Version::SHADER_VERSION_MEGALIGHTS_INPUTS_UBER_COUNT);
	CHECK_EQ(ml_count, 18);
	for (int i = 0; i < ml_count; i++) {
		const Shader::ShaderGroup group = Version::megalights_variant_group(uint16_t(i));
		CHECK(group != Shader::SHADER_GROUP_BASE);
		CHECK(group != Shader::SHADER_GROUP_ADVANCED);
		CHECK(group != Shader::SHADER_GROUP_MULTIVIEW);
		CHECK(group != Shader::SHADER_GROUP_ADVANCED_MULTIVIEW);
		CHECK(group != Shader::SHADER_GROUP_NEXTGEO);
		const String defines = Version::megalights_variant_defines(uint16_t(i));
		CHECK(defines.contains("MODE_MEGALIGHTS_INPUTS"));
	}
	CHECK(String(Version::megalights_variant_defines(0)).contains("MODE_RENDER_DEPTH"));
	CHECK_FALSE(String(Version::megalights_variant_defines(0)).contains("USE_MULTIVIEW"));
	CHECK_FALSE(String(Version::megalights_variant_defines(0)).contains("UBERSHADER"));
	CHECK(String(Version::megalights_variant_defines(4)).contains("USE_MULTIVIEW"));
	CHECK(String(Version::megalights_variant_defines(8)).contains("MODE_VISIBILITY_RESOLVE"));
	CHECK_FALSE(String(Version::megalights_variant_defines(8)).contains("UBERSHADER"));
	CHECK(String(Version::megalights_variant_defines(9)).contains("MODE_VISIBILITY_RESOLVE"));
	CHECK(String(Version::megalights_variant_defines(9)).contains("MODE_RENDER_VOXEL_GI"));
	CHECK_FALSE(String(Version::megalights_variant_defines(9)).contains("UBERSHADER"));
	CHECK(String(Version::megalights_variant_defines(10)).contains("UBERSHADER"));
	CHECK_FALSE(String(Version::megalights_variant_defines(10)).contains("USE_MULTIVIEW"));
	CHECK(String(Version::megalights_variant_defines(17)).contains("UBERSHADER"));
	CHECK(String(Version::megalights_variant_defines(17)).contains("USE_MULTIVIEW"));
	CHECK(String(Version::megalights_variant_defines(17)).contains("MODE_RENDER_VOXEL_GI"));
	CHECK(String(Version::megalights_variant_defines(17)).contains("MOTION_VECTORS"));
	CHECK_EQ(Version::megalights_variant_group(0), Shader::SHADER_GROUP_MEGALIGHTS);
	CHECK_EQ(Version::megalights_variant_group(4), Shader::SHADER_GROUP_MEGALIGHTS_MULTIVIEW);
	CHECK_EQ(Version::megalights_variant_group(8), Shader::SHADER_GROUP_NEXTGEO_MEGALIGHTS);
	CHECK_EQ(Version::megalights_variant_group(9), Shader::SHADER_GROUP_NEXTGEO_MEGALIGHTS);
	CHECK_EQ(Version::megalights_variant_group(10), Shader::SHADER_GROUP_MEGALIGHTS);
	CHECK_EQ(Version::megalights_variant_group(14), Shader::SHADER_GROUP_MEGALIGHTS_MULTIVIEW);
	CHECK_EQ(Version::megalights_variant_group(17), Shader::SHADER_GROUP_MEGALIGHTS_MULTIVIEW);

	// Headless doctest may not construct the RD scene shader. The table above is what init() pushes.
	// When the renderer exists and nothing has rendered with inputs on, the groups are still off.
	Shader *ml_shader = Shader::get_singleton();
	if (ml_shader != nullptr) {
		CHECK_FALSE(ml_shader->is_megalights_shader_group_enabled(false));
		CHECK_FALSE(ml_shader->is_megalights_shader_group_enabled(true));
		CHECK_FALSE(ml_shader->is_nextgeo_megalights_shader_group_enabled());
	}
}

TEST_CASE("[Renderer][MegaLights] Debug draw bindings (#136)") {
	CHECK_EQ(int(RSE::VIEWPORT_DEBUG_DRAW_ML_ALBEDO), int(RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_TRIANGLE_SIZE) + 1);
	CHECK_EQ(int(RSE::VIEWPORT_DEBUG_DRAW_ML_MATERIAL), int(RSE::VIEWPORT_DEBUG_DRAW_ML_ALBEDO) + 1);
	bool found = false;
	CHECK_EQ(ClassDB::get_integer_constant("RenderingServer", "VIEWPORT_DEBUG_DRAW_ML_ALBEDO", &found), int64_t(RSE::VIEWPORT_DEBUG_DRAW_ML_ALBEDO));
	CHECK(found);
	found = false;
	CHECK_EQ(ClassDB::get_integer_constant("RenderingServer", "VIEWPORT_DEBUG_DRAW_ML_MATERIAL", &found), int64_t(RSE::VIEWPORT_DEBUG_DRAW_ML_MATERIAL));
	CHECK(found);
#ifndef _3D_DISABLED
	found = false;
	CHECK_EQ(ClassDB::get_integer_constant("Viewport", "DEBUG_DRAW_ML_ALBEDO", &found), int64_t(RSE::VIEWPORT_DEBUG_DRAW_ML_ALBEDO));
	CHECK(found);
	found = false;
	CHECK_EQ(ClassDB::get_integer_constant("Viewport", "DEBUG_DRAW_ML_MATERIAL", &found), int64_t(RSE::VIEWPORT_DEBUG_DRAW_ML_MATERIAL));
	CHECK(found);
#endif
}
