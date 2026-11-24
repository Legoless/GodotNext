/**************************************************************************/
/*  test_light_function.cpp                                               */
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

TEST_FORCE_LINK(test_light_function)

// GodotNext: light_function_atlas (#138, plan §21.3 contract M8, GM-11, M-9).
// Unit tests for LightFunctionAtlasPacker (1,024+ light shelf packing, static vs TIME dirty tracking),
// ShaderCompiler validation of `shader_type light_function;`, CPU projection math, and Light3D API.

#include "core/config/project_settings.h"
#include "scene/3d/light_3d.h"
#include "scene/resources/material.h"
#include "scene/resources/shader.h"
#include "servers/rendering/renderer_rd/light_function_atlas_packer.h"
#include "servers/rendering/shader_compiler.h"
#include "servers/rendering/shader_types.h"

namespace TestLightFunction {

using Packer = RendererRD::LightFunctionAtlasPacker;

static void verify_no_overlap(const Packer &p_packer, uint32_t p_expected_count) {
	LocalVector<uint64_t> ids;
	p_packer.get_registered_light_ids(ids);
	CHECK_EQ(ids.size(), p_expected_count);

	const uint32_t atlas_size = p_packer.get_atlas_size();
	const uint32_t tile_size = p_packer.get_current_tile_size();
	REQUIRE_GT(tile_size, 0u);
	const uint32_t cells_per_axis = atlas_size / tile_size;
	LocalVector<uint8_t> occupied;
	occupied.resize_initialized(cells_per_axis * cells_per_axis);

	for (uint32_t i = 0; i < ids.size(); i++) {
		const Packer::TileRect rect = p_packer.get_tile_rect(ids[i]);
		REQUIRE(rect.valid);
		CHECK_EQ(rect.size, tile_size);
		CHECK_EQ(rect.x % tile_size, 0u);
		CHECK_EQ(rect.y % tile_size, 0u);
		CHECK_LE(rect.x + rect.size, atlas_size);
		CHECK_LE(rect.y + rect.size, atlas_size);

		const uint32_t cx = rect.x / tile_size;
		const uint32_t cy = rect.y / tile_size;
		const uint32_t cell = cy * cells_per_axis + cx;
		REQUIRE_LT(cell, occupied.size());
		CHECK_EQ(occupied[cell], 0u);
		occupied[cell] = 1u;
	}
}

static ShaderCompiler make_light_function_compiler() {
	ShaderCompiler compiler;
	ShaderCompiler::DefaultIdentifierActions actions;
	actions.renames["LIGHT_UV"] = "light_uv";
	actions.renames["TIME"] = "params.time";
	actions.renames["LIGHT_MASK"] = "light_mask";
	actions.renames["PI"] = "3.14159265359";
	actions.renames["TAU"] = "6.28318530718";
	actions.renames["E"] = "2.71828182846";

	actions.base_texture_binding_index = 1;
	actions.texture_layout_set = 2;
	actions.base_uniform_string = "material.";
	actions.base_varying_index = 0;
	actions.default_filter = ShaderLanguage::FILTER_LINEAR;
	actions.default_repeat = ShaderLanguage::REPEAT_ENABLE;
	actions.global_buffer_array_variable = "global_shader_uniforms.data";
	compiler.initialize(actions);
	return compiler;
}

TEST_CASE("[LightFunction] Shelf packer handles 1,024+ lights with free-list reuse and repack") {
	Packer packer(2048);
	CHECK_EQ(packer.get_atlas_size(), 2048u);
	CHECK_EQ(packer.get_current_tile_size(), 256u);

	// Register 1,024 lights: 2048/256=8 (64 slots) -> halves to 128 (256) -> halves to 64 (1,024).
	for (uint64_t i = 1; i <= 1024; i++) {
		REQUIRE(packer.register_or_update_light(i, 10000 + i, (i % 4) == 0, 1));
	}
	CHECK_EQ(packer.get_active_count(), 1024u);
	CHECK_EQ(packer.get_current_tile_size(), 64u);
	CHECK_EQ(packer.get_stats().active_tiles, 1024u);
	CHECK_EQ(packer.get_stats().animated_tiles, 256u);
	CHECK_EQ(packer.get_stats().static_tiles, 768u);
	verify_no_overlap(packer, 1024u);

	const uint64_t repacks_at_1024 = packer.get_stats().repacks;
	CHECK_EQ(repacks_at_1024, 2u);

	// Free 256 lights and allocate 256 replacements: must reuse freed slots without repacking.
	for (uint64_t i = 1; i <= 256; i++) {
		CHECK(packer.unregister_light(i));
	}
	CHECK_EQ(packer.get_active_count(), 768u);
	for (uint64_t i = 2001; i <= 2256; i++) {
		REQUIRE(packer.register_or_update_light(i, 20000 + i, false, 1));
	}
	CHECK_EQ(packer.get_active_count(), 1024u);
	CHECK_EQ(packer.get_stats().repacks, repacks_at_1024);
	verify_no_overlap(packer, 1024u);

	// Grow beyond 1,024 to 2,048 lights: must automatically halve to 32 px tiles.
	for (uint64_t i = 3001; i <= 4024; i++) {
		REQUIRE(packer.register_or_update_light(i, 30000 + i, false, 1));
	}
	CHECK_EQ(packer.get_active_count(), 2048u);
	CHECK_EQ(packer.get_current_tile_size(), 32u);
	CHECK_EQ(packer.get_stats().repacks, repacks_at_1024 + 1u);
	verify_no_overlap(packer, 2048u);

	// Unregister everything: tile size resets to default preferred size and no jobs run.
	LocalVector<uint64_t> remaining;
	packer.get_registered_light_ids(remaining);
	for (uint32_t i = 0; i < remaining.size(); i++) {
		CHECK(packer.unregister_light(remaining[i]));
	}
	CHECK_EQ(packer.get_active_count(), 0u);
	CHECK_EQ(packer.get_current_tile_size(), 256u);
	LocalVector<Packer::BakeJob> jobs;
	CHECK_EQ(packer.collect_bake_jobs(1, jobs), 0u);
	CHECK(jobs.is_empty());
}

TEST_CASE("[LightFunction] Static functions bake once; animated TIME functions bake every frame") {
	Packer packer(2048);
	constexpr uint64_t STATIC_LIGHT = 10;
	constexpr uint64_t ANIM_LIGHT = 20;

	REQUIRE(packer.register_or_update_light(STATIC_LIGHT, 101, false, 1));
	REQUIRE(packer.register_or_update_light(ANIM_LIGHT, 202, true, 1));

	LocalVector<Packer::BakeJob> jobs;

	// Frame 1: both lights bake once.
	CHECK_EQ(packer.collect_bake_jobs(1, jobs), 2u);
	CHECK_EQ(packer.get_stats().static_bakes, 1u);
	CHECK_EQ(packer.get_stats().animated_bakes, 1u);
	CHECK_EQ(packer.get_stats().total_bakes, 2u);

	// Second view in the same frame (frame 1): zero bakes.
	CHECK_EQ(packer.collect_bake_jobs(1, jobs), 0u);
	CHECK_EQ(packer.get_stats().last_frame_bakes, 0u);

	// Frames 2 and 3: only the animated light bakes.
	CHECK_EQ(packer.collect_bake_jobs(2, jobs), 1u);
	CHECK_EQ(jobs[0].light_id, ANIM_LIGHT);
	CHECK_EQ(packer.collect_bake_jobs(3, jobs), 1u);
	CHECK_EQ(jobs[0].light_id, ANIM_LIGHT);
	CHECK_EQ(packer.get_stats().static_bakes, 1u);
	CHECK_EQ(packer.get_stats().animated_bakes, 3u);

	// Unchanged serial update does NOT re-dirty the static light.
	REQUIRE(packer.register_or_update_light(STATIC_LIGHT, 101, false, 1));
	CHECK_EQ(packer.collect_bake_jobs(4, jobs), 1u);
	CHECK_EQ(jobs[0].light_id, ANIM_LIGHT);
	CHECK_EQ(packer.get_stats().static_bakes, 1u);

	// Parameter/shader change bumps material serial -> static light re-bakes once.
	REQUIRE(packer.register_or_update_light(STATIC_LIGHT, 101, false, 2));
	CHECK_EQ(packer.collect_bake_jobs(5, jobs), 2u);
	CHECK_EQ(packer.get_stats().static_bakes, 2u);
	CHECK_EQ(packer.get_stats().animated_bakes, 5u);

	// Frame 6: static light is clean again.
	CHECK_EQ(packer.collect_bake_jobs(6, jobs), 1u);
	CHECK_EQ(jobs[0].light_id, ANIM_LIGHT);
	CHECK_EQ(packer.get_stats().static_bakes, 2u);

	// Atlas resize forces repack and re-bakes both lights.
	packer.set_atlas_size(1024);
	CHECK_EQ(packer.collect_bake_jobs(7, jobs), 2u);
	CHECK_EQ(packer.get_stats().static_bakes, 3u);
	CHECK_EQ(packer.get_stats().animated_bakes, 7u);
}

TEST_CASE("[LightFunction] ShaderCompiler validates shader_type light_function built-ins and TIME tracking") {
	REQUIRE(ShaderTypes::get_singleton() != nullptr);
	CHECK(ShaderTypes::get_singleton()->get_types_list().find("light_function") != nullptr);

	ShaderCompiler compiler = make_light_function_compiler();

	// 1. Static light_function shader with LIGHT_UV, LIGHT_MASK, uniform float, and uniform sampler2D.
	{
		const String static_code =
				"shader_type light_function;\n"
				"uniform float gain = 0.75;\n"
				"uniform sampler2D cookie_tex;\n"
				"void light_function() {\n"
				"\tvec3 sample_rgb = texture(cookie_tex, LIGHT_UV).rgb;\n"
				"\tLIGHT_MASK = sample_rgb * gain;\n"
				"}\n";
		bool uses_time = false;
		HashMap<StringName, ShaderLanguage::ShaderNode::Uniform> uniforms;
		ShaderCompiler::IdentifierActions actions;
		actions.entry_point_stages["light_function"] = ShaderCompiler::STAGE_COMPUTE;
		actions.uniforms = &uniforms;
		actions.usage_flag_pointers["TIME"] = &uses_time;
		ShaderCompiler::GeneratedCode gen;
		const Error err = compiler.compile(RSE::SHADER_LIGHT_FUNCTION, static_code, &actions, "test_static_lf.gdshader", gen);
		CHECK_EQ(err, OK);
		CHECK_FALSE(uses_time);
		CHECK(gen.code.has("light_function"));
		CHECK_EQ(uniforms.size(), 2u);
		CHECK_GT(gen.uniform_total_size, 0u);
		CHECK_EQ(gen.texture_uniforms.size(), 1);
	}

	// 2. Animated light_function shader referencing TIME and math constants.
	{
		const String anim_code =
				"shader_type light_function;\n"
				"void light_function() {\n"
				"\tfloat wave = 0.5 + 0.5 * sin(TIME * TAU + LIGHT_UV.x * PI);\n"
				"\tLIGHT_MASK = vec3(wave);\n"
				"}\n";
		bool uses_time = false;
		HashMap<StringName, ShaderLanguage::ShaderNode::Uniform> uniforms;
		ShaderCompiler::IdentifierActions actions;
		actions.entry_point_stages["light_function"] = ShaderCompiler::STAGE_COMPUTE;
		actions.uniforms = &uniforms;
		actions.usage_flag_pointers["TIME"] = &uses_time;
		ShaderCompiler::GeneratedCode gen;
		const Error err = compiler.compile(RSE::SHADER_LIGHT_FUNCTION, anim_code, &actions, "test_anim_lf.gdshader", gen);
		CHECK_EQ(err, OK);
		CHECK(uses_time);
		CHECK(gen.code.has("light_function"));
	}

	// 3. Invalid shader (writing to read-only built-in LIGHT_UV) must fail compilation.
	{
		const String bad_code =
				"shader_type light_function;\n"
				"void light_function() {\n"
				"\tLIGHT_UV = vec2(0.0);\n"
				"\tLIGHT_MASK = vec3(1.0);\n"
				"}\n";
		HashMap<StringName, ShaderLanguage::ShaderNode::Uniform> uniforms;
		ShaderCompiler::IdentifierActions actions;
		actions.entry_point_stages["light_function"] = ShaderCompiler::STAGE_COMPUTE;
		actions.uniforms = &uniforms;
		ShaderCompiler::GeneratedCode gen;
		ERR_PRINT_OFF;
		const Error err = compiler.compile(RSE::SHADER_LIGHT_FUNCTION, bad_code, &actions, "test_bad_lf.gdshader", gen);
		ERR_PRINT_ON;
		CHECK_NE(err, OK);
	}
}

TEST_CASE("[LightFunction] Projection math and half-texel inset UV mapping") {
	Packer::TileRect rect;
	rect.x = 256;
	rect.y = 512;
	rect.size = 128;
	rect.valid = true;

	// Half-texel inset keeps [0, 1] strictly inside [x+0.5, x+size-0.5] / atlas_size.
	const Vector4 inset = rect.to_inset_uv_rect(2048);
	CHECK(Math::is_equal_approx(inset.x, 256.5f / 2048.0f));
	CHECK(Math::is_equal_approx(inset.y, 512.5f / 2048.0f));
	CHECK(Math::is_equal_approx(inset.x + inset.z, 383.5f / 2048.0f));
	CHECK(Math::is_equal_approx(inset.y + inset.w, 639.5f / 2048.0f));

	// Directional: planar wrap with light_function_scale.
	{
		const Transform3D xform = Transform3D();
		const Packer::EntryGPU entry = Packer::build_entry_gpu(rect, 2048, Packer::MODE_DIRECTIONAL, xform, Vector3(4.0f, 2.0f, 1.0f), 45.0f, false);
		Vector2 uv;
		REQUIRE(Packer::project_local_uv(entry, Vector3(0.0f, 0.0f, -5.0f), uv));
		CHECK(Math::is_equal_approx(uv.x, 0.5f));
		CHECK(Math::is_equal_approx(uv.y, 0.5f));

		REQUIRE(Packer::project_local_uv(entry, Vector3(1.0f, 0.5f, 0.0f), uv));
		CHECK(Math::is_equal_approx(uv.x, 0.75f));
		CHECK(Math::is_equal_approx(uv.y, 0.75f));

		// Wraps cleanly at tile boundary.
		REQUIRE(Packer::project_local_uv(entry, Vector3(5.0f, 0.5f, 0.0f), uv));
		CHECK(Math::is_equal_approx(uv.x, 0.75f));
	}

	// Spot: perspective projection along -Z, rejecting points behind the light.
	{
		const Transform3D xform(Basis(), Vector3(0.0f, 0.0f, 3.0f));
		const Packer::EntryGPU entry = Packer::build_entry_gpu(rect, 2048, Packer::MODE_SPOT, xform, Vector3(1.0f, 1.0f, 1.0f), 45.0f, true);
		CHECK_NE(entry.flags & Packer::FLAG_RGBA, 0u);
		Vector2 uv;
		REQUIRE(Packer::project_local_uv(entry, Vector3(0.0f, 0.0f, 0.0f), uv));
		CHECK(Math::is_equal_approx(uv.x, 0.5f));
		CHECK(Math::is_equal_approx(uv.y, 0.5f));

		// Right edge of 45-degree cone at distance 3 -> x = +3 -> uv.x = 1.0.
		REQUIRE(Packer::project_local_uv(entry, Vector3(3.0f, 0.0f, 0.0f), uv));
		CHECK(Math::is_equal_approx(uv.x, 1.0f));
		CHECK(Math::is_equal_approx(uv.y, 0.5f));

		// Behind the spotlight (z > 3.0) -> rejected.
		CHECK_FALSE(Packer::project_local_uv(entry, Vector3(0.0f, 0.0f, 4.0f), uv));
	}

	// Omni: octahedral direction mapping keeps all 6 cardinal directions inside [0,1]^2 and preserves X sign.
	{
		const Transform3D xform = Transform3D();
		const Packer::EntryGPU entry = Packer::build_entry_gpu(rect, 2048, Packer::MODE_OMNI, xform, Vector3(1.0f, 1.0f, 1.0f), 45.0f, false);
		const Vector3 dirs[6] = {
			Vector3(1, 0, 0),
			Vector3(-1, 0, 0),
			Vector3(0, 1, 0),
			Vector3(0, -1, 0),
			Vector3(0, 0, 1),
			Vector3(0, 0, -1),
		};
		for (int i = 0; i < 6; i++) {
			Vector2 uv;
			REQUIRE(Packer::project_local_uv(entry, dirs[i] * 2.0f, uv));
			CHECK_GE(uv.x, 0.0f);
			CHECK_LE(uv.x, 1.0f);
			CHECK_GE(uv.y, 0.0f);
			CHECK_LE(uv.y, 1.0f);
		}
		Vector2 uv_left;
		Vector2 uv_right;
		REQUIRE(Packer::project_local_uv(entry, Vector3(-1.5f, 0.0f, -2.0f), uv_left));
		REQUIRE(Packer::project_local_uv(entry, Vector3(1.5f, 0.0f, -2.0f), uv_right));
		CHECK_LT(uv_left.x, 0.5f);
		CHECK_GT(uv_right.x, 0.5f);
	}
}

TEST_CASE("[LightFunction] Shader mode, Light3D properties, and ProjectSettings defaults") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	REQUIRE(ps != nullptr);
	CHECK_EQ(int(ps->get_setting("rendering/lights_and_shadows/light_function_atlas/size")), 2048);
	CHECK_EQ(int(ps->get_setting("rendering/lights_and_shadows/light_function_atlas/format")), 0);
	CHECK_EQ(bool(ps->get_setting("rendering/lights_and_shadows/light_function_atlas/translucent")), false);

	Ref<Shader> lf_shader;
	lf_shader.instantiate();
	lf_shader->set_code("shader_type light_function;\nvoid light_function() {\n\tLIGHT_MASK = vec3(LIGHT_UV.x);\n}\n");
	CHECK_EQ(lf_shader->get_mode(), Shader::MODE_LIGHT_FUNCTION);

	Ref<ShaderMaterial> lf_mat;
	lf_mat.instantiate();
	lf_mat->set_shader(lf_shader);
	CHECK_EQ(lf_mat->get_shader_mode(), Shader::MODE_LIGHT_FUNCTION);

	SpotLight3D *spot = memnew(SpotLight3D);
	CHECK(spot->get_light_function().is_null());
	CHECK_EQ(spot->get_light_function_scale(), Vector3(1.0f, 1.0f, 1.0f));

	spot->set_light_function(lf_mat);
	spot->set_light_function_scale(Vector3(2.0f, 3.0f, 4.0f));
	CHECK_EQ(spot->get_light_function(), lf_mat);
	CHECK_EQ(spot->get_light_function_scale(), Vector3(2.0f, 3.0f, 4.0f));

	// Assigning a non-light_function material emits a configuration warning on Light3D.
	Ref<StandardMaterial3D> wrong_mat;
	wrong_mat.instantiate();
	spot->set_light_function(wrong_mat);
	const PackedStringArray warnings = spot->get_configuration_warnings();
	bool found_lf_warning = false;
	for (int i = 0; i < warnings.size(); i++) {
		if (warnings[i].contains("shader_type light_function")) {
			found_lf_warning = true;
			break;
		}
	}
	CHECK(found_lf_warning);

	spot->set_light_function(Ref<Material>());
	CHECK(spot->get_light_function().is_null());
	memdelete(spot);
}

} // namespace TestLightFunction
