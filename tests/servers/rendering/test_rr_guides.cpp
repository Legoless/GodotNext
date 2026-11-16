/**************************************************************************/
/*  test_rr_guides.cpp                                                    */
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

TEST_FORCE_LINK(test_rr_guides)

#include "core/math/math_funcs.h"
#include "servers/rendering/renderer_rd/effects/rr_guides.h"
#include "servers/rendering/renderer_rd/forward_clustered/scene_shader_forward_clustered.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

namespace {

// Stands in for SceneShaderForwardClustered in RRGuides::enable_view_shader_groups().
struct RRGroupRecorder {
	int rr_calls = 0;
	int nextgeo_calls = 0;

	void enable_rr_guides_shader_group() {
		rr_calls++;
	}

	void enable_nextgeo_rr_guides_shader_group() {
		nextgeo_calls++;
	}
};

// What an RGBA16F texel keeps of a value.
float _to_half_and_back(float p_value) {
	return Math::half_to_float(Math::make_half_float(p_value));
}

// What an RGBA8 texel keeps of a value.
float _to_unorm8_and_back(float p_value) {
	return Math::round(CLAMP(p_value, 0.0f, 1.0f) * 255.0f) / 255.0f;
}

} // namespace

TEST_CASE("[Upscalers][RRGuides] Color pass flag counts and shader version layout (#205)") {
	using Shader = RendererSceneRenderImplementation::SceneShaderForwardClustered;
	using Version = Shader::ShaderVersion;

	// The flag is appended: the five earlier flags keep their bits.
	CHECK_EQ(int(Shader::SHADER_COLOR_PASS_FLAG_UBERSHADER), 1 << 0);
	CHECK_EQ(int(Shader::SHADER_COLOR_PASS_FLAG_SEPARATE_SPECULAR), 1 << 1);
	CHECK_EQ(int(Shader::SHADER_COLOR_PASS_FLAG_LIGHTMAP), 1 << 2);
	CHECK_EQ(int(Shader::SHADER_COLOR_PASS_FLAG_MULTIVIEW), 1 << 3);
	CHECK_EQ(int(Shader::SHADER_COLOR_PASS_FLAG_MOTION_VECTORS), 1 << 4);
	CHECK_EQ(int(Shader::SHADER_COLOR_PASS_FLAG_RR_GUIDES), 1 << 5);
	CHECK_EQ(int(Shader::SHADER_COLOR_PASS_FLAG_COUNT), 1 << 6);

	CHECK_EQ(int(Shader::PIPELINE_COLOR_PASS_FLAG_TRANSPARENT), 1 << 0);
	CHECK_EQ(int(Shader::PIPELINE_COLOR_PASS_FLAG_MOTION_VECTORS), 1 << 4);
	CHECK_EQ(int(Shader::PIPELINE_COLOR_PASS_FLAG_RR_GUIDES), 1 << 5);
	CHECK_EQ(int(Shader::PIPELINE_COLOR_PASS_FLAG_OPTIONS), 6);
	CHECK_EQ(int(Shader::PIPELINE_COLOR_PASS_FLAG_COMBINATIONS), 64);

	// The color pass block holds one variant per flag combination; the blocks after it start past it.
	CHECK_EQ(int(Version::SHADER_VERSION_VISIBILITY_RESOLVE_BASE), int(Version::SHADER_VERSION_COLOR_PASS) * 2 + int(Shader::SHADER_COLOR_PASS_FLAG_COUNT));
	CHECK_EQ(int(Version::SHADER_VERSION_MEGALIGHTS_INPUTS_BASE), int(Version::SHADER_VERSION_VISIBILITY_RESOLVE_BASE) + int(Version::SHADER_VERSION_VISIBILITY_RESOLVE_COUNT));
	CHECK_EQ(int(Version::SHADER_VERSION_VISIBILITY_RESOLVE_RR_GUIDES_BASE), int(Version::SHADER_VERSION_MEGALIGHTS_INPUTS_BASE) + int(Version::SHADER_VERSION_MEGALIGHTS_INPUTS_COUNT) + int(Version::SHADER_VERSION_MEGALIGHTS_INPUTS_UBER_COUNT));
	CHECK_EQ(int(Version::SHADER_VERSION_VISIBILITY_RESOLVE_RR_GUIDES_COUNT), 4);
	// One vertex input mask per variant, the last Stage B RR variant included.
	CHECK_EQ(int(Shader::ShaderData::VERTEX_INPUT_MASKS_SIZE), int(Version::SHADER_VERSION_VISIBILITY_RESOLVE_RR_GUIDES_BASE) + int(Version::SHADER_VERSION_VISIBILITY_RESOLVE_RR_GUIDES_COUNT));

	// Every variant with the guide outputs is in an RR group, so none compiles with RR off.
	for (uint32_t flags = 0; flags < uint32_t(Shader::SHADER_COLOR_PASS_FLAG_COUNT); flags++) {
		if (!(flags & Shader::SHADER_COLOR_PASS_FLAG_RR_GUIDES)) {
			continue;
		}
		const Shader::ShaderGroup group = Shader::color_pass_variant_rr_guides_group(flags);
		if (flags & Shader::SHADER_COLOR_PASS_FLAG_MULTIVIEW) {
			CHECK_EQ(group, Shader::SHADER_GROUP_RR_GUIDES_MULTIVIEW);
		} else {
			CHECK_EQ(group, Shader::SHADER_GROUP_RR_GUIDES);
		}
	}

	// The guides follow color, separate specular and velocity.
	CHECK_EQ(int(Shader::COLOR_PASS_ATTACHMENT_VELOCITY), 2);
	CHECK_EQ(int(Shader::COLOR_PASS_ATTACHMENTS), 3);
	CHECK_EQ(int(Shader::COLOR_PASS_ATTACHMENTS_RR_GUIDES), 5);
	CHECK_EQ(int(RendererRD::RRGuides::ATTACHMENT_DIFFUSE_ALBEDO), int(Shader::COLOR_PASS_ATTACHMENTS));
	CHECK_EQ(int(RendererRD::RRGuides::ATTACHMENT_SPECULAR_ALBEDO), int(Shader::COLOR_PASS_ATTACHMENTS_RR_GUIDES) - 1);
}

TEST_CASE("[Upscalers][RRGuides] Shader groups stay off unless a view writes the guides (#205)") {
	using RR = RendererRD::RRGuides;

	// The DLSS-RR mode's request adds to the DEV switch; it never turns the switch off.
	RR::set_requested(true);
	CHECK(RR::is_requested());
	RR::set_requested(false);
	CHECK_EQ(RR::is_requested(), RR::is_dev_switch_on());

	// Not requested: no group is enabled for any view shape.
	for (int shape = 0; shape < 16; shape++) {
		RRGroupRecorder recorder;
		CHECK_FALSE(RR::enable_view_shader_groups(recorder, false, (shape & 1) != 0, (shape & 2) ? 2 : 1, (shape & 4) != 0, (shape & 8) != 0));
		CHECK_EQ(recorder.rr_calls, 0);
		CHECK_EQ(recorder.nextgeo_calls, 0);
	}

	// Requested, but reflection probes, multiview and MSAA views write no guides.
	CHECK_FALSE(RR::view_writes_guides(true, true, 1, false));
	CHECK_FALSE(RR::view_writes_guides(true, false, 2, false));
	ERR_PRINT_OFF;
	CHECK_FALSE(RR::view_writes_guides(true, false, 1, true));
	ERR_PRINT_ON;
	CHECK_FALSE(RR::view_writes_guides(false, false, 1, false));
	CHECK(RR::view_writes_guides(true, false, 1, false));
	{
		RRGroupRecorder recorder;
		CHECK_FALSE(RR::enable_view_shader_groups(recorder, true, true, 1, false, true));
		CHECK_FALSE(RR::enable_view_shader_groups(recorder, true, false, 2, false, true));
		ERR_PRINT_OFF;
		CHECK_FALSE(RR::enable_view_shader_groups(recorder, true, false, 1, true, true));
		ERR_PRINT_ON;
		CHECK_EQ(recorder.rr_calls, 0);
		CHECK_EQ(recorder.nextgeo_calls, 0);
	}

	// Active: the forward group always, the Stage B group only with virtual meshes.
	{
		RRGroupRecorder recorder;
		CHECK(RR::enable_view_shader_groups(recorder, true, false, 1, false, false));
		CHECK_EQ(recorder.rr_calls, 1);
		CHECK_EQ(recorder.nextgeo_calls, 0);
	}
	{
		RRGroupRecorder recorder;
		CHECK(RR::enable_view_shader_groups(recorder, true, false, 1, false, true));
		CHECK_EQ(recorder.rr_calls, 1);
		CHECK_EQ(recorder.nextgeo_calls, 1);
	}
}

TEST_CASE("[Upscalers][RRGuides] The first opaque pass clears the guides at their color index (#205, #275)") {
	using RR = RendererRD::RRGuides;

	// Where no opaque surface is: diffuse albedo 0.5, specular albedo 0, alpha 0 (plan §8.6).
	CHECK_EQ(RR::diffuse_albedo_clear_color(), Color(0.5, 0.5, 0.5, 0.0));
	CHECK_EQ(RR::specular_albedo_clear_color(), Color(0.0, 0.0, 0.0, 0.0));

	// draw_list_begin() numbers its clear flags and clear colors by the color attachments that
	// exist: color, then separate specular and velocity when the framebuffer has them, then the
	// two guides.
	CHECK_EQ(RR::first_guide_color_index(false, false), 1u);
	CHECK_EQ(RR::first_guide_color_index(true, false), 2u);
	CHECK_EQ(RR::first_guide_color_index(false, true), 2u);
	CHECK_EQ(RR::first_guide_color_index(true, true), 3u);

	// The color pass flags nextgeo_color_pass_draw_flags() returns, and every color flag.
	const BitField<RD::DrawFlags> inputs[] = {
		BitField<RD::DrawFlags>(RD::DRAW_CLEAR_COLOR_ALL | RD::DRAW_CLEAR_DEPTH),
		BitField<RD::DrawFlags>(RD::DRAW_CLEAR_COLOR_ALL),
		BitField<RD::DrawFlags>(RD::DRAW_DEFAULT_ALL),
		BitField<RD::DrawFlags>(RD::DRAW_CLEAR_COLOR_0 | RD::DRAW_CLEAR_COLOR_1),
		BitField<RD::DrawFlags>(RD::DRAW_IGNORE_COLOR_ALL | RD::DRAW_CLEAR_STENCIL),
	};
	// The colors the opaque passes give: color, separate specular and velocity when they clear
	// color (all three even where the framebuffer lacks the last two), none when they load it.
	const Color color_clear(0.1, 0.2, 0.3, 0.0);
	Vector<Color> clearing_colors;
	clearing_colors.push_back(color_clear);
	clearing_colors.push_back(Color(0, 0, 0, 0));
	clearing_colors.push_back(Color(0, 0, 0, 0));
	const Vector<Color> color_inputs[] = { clearing_colors, Vector<Color>() };

	int refused = 0;
	for (int shape = 0; shape < 4; shape++) {
		const bool has_specular = (shape & 1) != 0;
		const bool has_velocity = (shape & 2) != 0;
		const uint32_t first = RR::first_guide_color_index(has_specular, has_velocity);
		for (const BitField<RD::DrawFlags> &input : inputs) {
			for (const Vector<Color> &input_colors : color_inputs) {
				// draw_list_begin() fails a flag that clears a color attachment without a clear
				// color. Before the guides, clear_guides() fails it too and changes nothing, so
				// draw_list_begin() still reports it (the opaque passes never do this).
				bool has_colors = true;
				for (uint32_t i = uint32_t(input_colors.size()); i < first; i++) {
					has_colors = has_colors && !input.has_flag(RD::DrawFlags(RD::DRAW_CLEAR_COLOR_0 << i));
				}
				BitField<RD::DrawFlags> output = input;
				Vector<Color> colors = input_colors;
				if (!has_colors) {
					ERR_PRINT_OFF;
					RR::clear_guides(output, colors, has_specular, has_velocity);
					ERR_PRINT_ON;
					CHECK_EQ(uint64_t(output), uint64_t(input));
					CHECK(colors == input_colors);
					refused++;
					continue;
				}
				RR::clear_guides(output, colors, has_specular, has_velocity);
				for (uint32_t i = 0; i < 8; i++) {
					const bool guide = i == first || i == first + 1;
					const RD::DrawFlags clear = RD::DrawFlags(RD::DRAW_CLEAR_COLOR_0 << i);
					const RD::DrawFlags ignore = RD::DrawFlags(RD::DRAW_IGNORE_COLOR_0 << i);
					// The guides clear; every other attachment keeps its flags.
					CHECK_EQ(output.has_flag(clear), guide ? true : input.has_flag(clear));
					CHECK_EQ(output.has_flag(ignore), guide ? false : input.has_flag(ignore));
				}
				CHECK_EQ(output.has_flag(RD::DRAW_CLEAR_DEPTH), input.has_flag(RD::DRAW_CLEAR_DEPTH));
				CHECK_EQ(output.has_flag(RD::DRAW_CLEAR_STENCIL), input.has_flag(RD::DRAW_CLEAR_STENCIL));
				REQUIRE(uint32_t(colors.size()) >= first + 2);
				CHECK_EQ(colors[first], RR::diffuse_albedo_clear_color());
				CHECK_EQ(colors[first + 1], RR::specular_albedo_clear_color());
				// The colors before the guides are the pass's own.
				for (uint32_t i = 0; i < first && i < uint32_t(input_colors.size()); i++) {
					CHECK_EQ(colors[i], input_colors[i]);
				}
			}
		}
	}
	// Without colors, the three inputs that clear color 0 are refused in every shape.
	CHECK_EQ(refused, 12);
}

TEST_CASE("[Upscalers][RRGuides] Pack math: world-space normals of 64 sample normals (#205)") {
	using RR = RendererRD::RRGuides;

	// A camera that is neither axis-aligned nor a pure yaw.
	const Basis view_to_world = Basis::from_euler(Vector3(0.3, -1.1, 0.2));

	real_t worst_float = 0.0;
	real_t worst_stored = 0.0;
	for (int i = 0; i < 64; i++) {
		// A Fibonacci sphere: 64 view-space unit normals over the whole sphere.
		const real_t y = 1.0 - (real_t(i) + 0.5) * 2.0 / 64.0;
		const real_t radius = Math::sqrt(1.0 - y * y);
		const real_t phi = real_t(i) * 2.39996322972865332;
		const Vector3 view_normal = Vector3(Math::cos(phi) * radius, y, Math::sin(phi) * radius);
		const Vector3 expected = view_to_world.xform(view_normal);

		// The prepass stores encode24(normal) * 0.5 + 0.5, where encode24() only scales the normal.
		const real_t scales[3] = { 1.0, 0.75, 0.31 };
		for (const real_t scale : scales) {
			const Vector3 encoded = view_normal * scale * 0.5 + Vector3(0.5, 0.5, 0.5);
			const Vector3 world_normal = RR::unpack_world_normal(view_to_world, encoded);
			CHECK(world_normal.is_normalized());
			for (int axis = 0; axis < 3; axis++) {
				worst_float = MAX(worst_float, Math::abs(world_normal[axis] - expected[axis]));
			}
		}

		// The same through the textures: RGBA8 in, RGBA16F out. The pack must not add more
		// than the RGBA16F rounding to the normal the RGBA8 texel holds.
		const real_t max_axis = MAX(Math::abs(view_normal.x), MAX(Math::abs(view_normal.y), Math::abs(view_normal.z)));
		const Vector3 on_cube = view_normal / max_axis;
		const Vector3 texel = Vector3(_to_unorm8_and_back(on_cube.x * 0.5 + 0.5), _to_unorm8_and_back(on_cube.y * 0.5 + 0.5), _to_unorm8_and_back(on_cube.z * 0.5 + 0.5));
		const Vector3 texel_normal = view_to_world.xform((texel * 2.0 - Vector3(1.0, 1.0, 1.0)).normalized());
		const Vector3 packed = RR::unpack_world_normal(view_to_world, texel);
		for (int axis = 0; axis < 3; axis++) {
			worst_stored = MAX(worst_stored, Math::abs(real_t(_to_half_and_back(packed[axis])) - texel_normal[axis]));
		}
	}
	CHECK(worst_float < 1e-5);
	CHECK(worst_stored < 1e-3);

	// A texel that decodes to no direction packs to the zero vector, not to NaN.
	CHECK_EQ(RR::unpack_world_normal(view_to_world, Vector3(0.5, 0.5, 0.5)), Vector3());
}

TEST_CASE("[Upscalers][RRGuides] Pack math: roughness in both dynamic-flag states (#205)") {
	using RR = RendererRD::RRGuides;

	// The prepass stores roughness * 127 / 255 for a static instance and one minus that for a
	// dynamic one, in an RGBA8 texel: bytes 0..127 are static, 128..255 dynamic.
	for (int step = 0; step <= 127; step++) {
		const float roughness = float(step) / 127.0f;
		const float stored_static = _to_unorm8_and_back(roughness * (127.0f / 255.0f));
		const float stored_dynamic = _to_unorm8_and_back(1.0f - roughness * (127.0f / 255.0f));
		CHECK(stored_static < 0.5f);
		CHECK(stored_dynamic > 0.5f);
		CHECK(Math::abs(RR::unpack_roughness(stored_static) - roughness) < 1e-5f);
		CHECK(Math::abs(RR::unpack_roughness(stored_dynamic) - roughness) < 1e-5f);
		CHECK(Math::abs(_to_half_and_back(RR::unpack_roughness(stored_dynamic)) - roughness) < 1e-3f);
	}
	// The two states of one roughness unpack to the same value, at both ends of the range.
	CHECK_EQ(RR::unpack_roughness(0.0f), 0.0f);
	CHECK(Math::is_equal_approx(RR::unpack_roughness(1.0f), 0.0f));
	CHECK(Math::is_equal_approx(RR::unpack_roughness(127.0f / 255.0f), 1.0f));
	CHECK(Math::is_equal_approx(RR::unpack_roughness(128.0f / 255.0f), 1.0f));
}

TEST_CASE("[Upscalers][RRGuides] The pack shader takes its decode from glsl_pack_prelude() (#205)") {
	using RR = RendererRD::RRGuides;

	// rr_guide_pack.glsl holds none of the decode's numbers: it calls these macros, and
	// unpack_world_normal() and unpack_roughness() (checked above) are the same math.
	CHECK_EQ(RR::ROUGHNESS_STORED_MAX, 127);
	CHECK_EQ(RR::DYNAMIC_FLAG_THRESHOLD, 0.5f);
	const String prelude = RR::glsl_pack_prelude();
	CHECK(prelude.contains(vformat("#define RR_ROUGHNESS_STORED_MAX %d\n", RR::ROUGHNESS_STORED_MAX)));
	CHECK(prelude.contains("#define RR_DYNAMIC_FLAG_THRESHOLD 0.5\n"));
	CHECK(prelude.contains("#define RR_DECODE_NORMAL(e) ((e) * 2.0 - 1.0)\n"));
	CHECK(prelude.contains("#define RR_DECODE_ROUGHNESS(w) (((w) > RR_DYNAMIC_FLAG_THRESHOLD ? 1.0 - (w) : (w)) * (255.0 / float(RR_ROUGHNESS_STORED_MAX)))\n"));
}

TEST_CASE("[Upscalers][RRGuides] The guides are not on the output-sized list (#205)") {
	// The dynamic resolution fast path (#191) frees every named texture that is not output
	// sized and lets its owner recreate it. This is a guard only: it fails if the guides are
	// added to that list. The guides' internal size itself is checked by the testbed check
	// tests/rr_guides, which needs internal width x height x 16 guide bytes, at scale 0.67
	// under FSR 2.2 and TSR too.
	CHECK_FALSE(RenderSceneBuffersRD::is_output_sized_named_texture(RB_SCOPE_RR_GUIDES, RB_TEX_RR_DIFFUSE_ALBEDO));
	CHECK_FALSE(RenderSceneBuffersRD::is_output_sized_named_texture(RB_SCOPE_RR_GUIDES, RB_TEX_RR_SPECULAR_ALBEDO));
	CHECK_FALSE(RenderSceneBuffersRD::is_output_sized_named_texture(RB_SCOPE_RR_GUIDES, RB_TEX_RR_NORMAL_ROUGHNESS));
}
