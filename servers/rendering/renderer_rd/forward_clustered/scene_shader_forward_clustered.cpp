/**************************************************************************/
/*  scene_shader_forward_clustered.cpp                                    */
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

#include "scene_shader_forward_clustered.h"

#include "core/config/project_settings.h"
#include "core/math/math_defs.h"
// GodotNext: megalights_inputs (#136)
#include "core/config/engine.h"
#include "servers/rendering/renderer_rd/forward_clustered/megalights_inputs.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_mesh_raster.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_raster_bins.h"
// GodotNext: nextgeo_shading_bins (#158)
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_shading_bins.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_sw_raster.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/renderer_compositor_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
// GodotNext: nextgi_hit_groups (#130)
#include "core/templates/safe_refcount.h"
#include "servers/rendering/renderer_rd/environment/next_gi_hit_materials.h"
// GodotNext: avboit (#295)
#include "servers/rendering/renderer_rd/effects/avboit.h"
// GodotNext: vsm_nanite (#217)
#include "servers/rendering/storage/virtual_shadow_map_storage.h"

using namespace RendererSceneRenderImplementation;

void SceneShaderForwardClustered::ShaderData::set_code(const String &p_code) {
	//compile

	code = p_code;
	ubo_size = 0;
	uniforms.clear();
	_clear_vertex_input_mask_cache();

	if (code.is_empty()) {
		_free_raster_target();
		_free_resolve_compute(); // GodotNext: nextgeo_shading_bins (#158)
		raster_features = 0;
		raster_uses = 0;
		raster_rejects = 0;
		has_displacement = false;
		uses_displacement_time = false;
		displacement_code = String();
		nextgeo_programmable_fallback = RendererRD::NEXTGEO_FALLBACK_NONE;
		return; //just invalid, but no error
	}

	ShaderCompiler::GeneratedCode gen_code;

	blend_mode = BLEND_MODE_MIX;
	depth_test_disabledi = 0;
	depth_test_invertedi = 0;
	alpha_antialiasing_mode = ALPHA_ANTIALIASING_OFF;
	int cull_modei = RSE::CULL_MODE_BACK;

	uses_point_size = false;
	uses_alpha = false;
	uses_alpha_clip = false;
	uses_alpha_scissor = false;
	uses_alpha_hash = false;
	uses_alpha_antialiasing = false;
	uses_blend_alpha = false;
	uses_depth_prepass_alpha = false;
	// GodotNext: avboit (#295)
	uses_shadow_to_opacity = false;
	uses_discard = false;
	uses_roughness = false;
	uses_normal = false;
	uses_tangent = false;
	writes_tangent = false;
	uses_normal_map = false;
	uses_bent_normal_map = false;
	wireframe = false;

	unshaded = false;
	uses_vertex = false;
	uses_position = false;
	uses_sss = false;
	uses_transmittance = false;
	uses_time = false;
	writes_modelview_or_projection = false;
	uses_main_camera = false; // GodotNext: vsm_non_nanite (#218)
	uses_world_coordinates = false;
	uses_particle_trails = false;
	uses_z_clip_scale = false;
	uses_vertex_lighting = false;
	uses_skip_vertex_transform = false;
	writes_depth = false;
	uses_depth_offset = false;
	uses_unsupported_derivative = false;
	nextgeo_fallback = RendererRD::NEXTGEO_FALLBACK_NONE;
	raster_features = 0;
	raster_uses = 0;
	raster_rejects = 0;
	has_displacement = false;
	uses_displacement_time = false;
	displacement_code = String();
	nextgeo_masked_fallback = RendererRD::NEXTGEO_FALLBACK_NONE;
	nextgeo_programmable_fallback = RendererRD::NEXTGEO_FALLBACK_NONE;
	nextgeo_raster_reject_logged = false;

	int depth_drawi = DEPTH_DRAW_OPAQUE;

	int stencil_readi = 0;
	int stencil_writei = 0;
	int stencil_write_depth_faili = 0;
	int stencil_comparei = STENCIL_COMPARE_ALWAYS;
	int stencil_referencei = -1;

	ShaderCompiler::IdentifierActions actions;
	actions.entry_point_stages["vertex"] = ShaderCompiler::STAGE_VERTEX;
	actions.entry_point_stages["fragment"] = ShaderCompiler::STAGE_FRAGMENT;
	actions.entry_point_stages["light"] = ShaderCompiler::STAGE_FRAGMENT;

	actions.render_mode_values["blend_add"] = Pair<int *, int>(&blend_mode, BLEND_MODE_ADD);
	actions.render_mode_values["blend_mix"] = Pair<int *, int>(&blend_mode, BLEND_MODE_MIX);
	actions.render_mode_values["blend_sub"] = Pair<int *, int>(&blend_mode, BLEND_MODE_SUB);
	actions.render_mode_values["blend_mul"] = Pair<int *, int>(&blend_mode, BLEND_MODE_MUL);
	actions.render_mode_values["blend_premul_alpha"] = Pair<int *, int>(&blend_mode, BLEND_MODE_PREMULTIPLIED_ALPHA);

	actions.render_mode_values["alpha_to_coverage"] = Pair<int *, int>(&alpha_antialiasing_mode, ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE);
	actions.render_mode_values["alpha_to_coverage_and_one"] = Pair<int *, int>(&alpha_antialiasing_mode, ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE_AND_TO_ONE);

	actions.render_mode_values["depth_draw_never"] = Pair<int *, int>(&depth_drawi, DEPTH_DRAW_DISABLED);
	actions.render_mode_values["depth_draw_opaque"] = Pair<int *, int>(&depth_drawi, DEPTH_DRAW_OPAQUE);
	actions.render_mode_values["depth_draw_always"] = Pair<int *, int>(&depth_drawi, DEPTH_DRAW_ALWAYS);

	actions.render_mode_values["depth_test_disabled"] = Pair<int *, int>(&depth_test_disabledi, 1);
	actions.render_mode_values["depth_test_inverted"] = Pair<int *, int>(&depth_test_invertedi, 1);

	actions.render_mode_values["cull_disabled"] = Pair<int *, int>(&cull_modei, RSE::CULL_MODE_DISABLED);
	actions.render_mode_values["cull_front"] = Pair<int *, int>(&cull_modei, RSE::CULL_MODE_FRONT);
	actions.render_mode_values["cull_back"] = Pair<int *, int>(&cull_modei, RSE::CULL_MODE_BACK);

	actions.render_mode_flags["unshaded"] = &unshaded;
	actions.render_mode_flags["wireframe"] = &wireframe;
	actions.render_mode_flags["particle_trails"] = &uses_particle_trails;
	actions.render_mode_flags["world_vertex_coords"] = &uses_world_coordinates;
	actions.render_mode_flags["vertex_lighting"] = &uses_vertex_lighting;
	actions.render_mode_flags["skip_vertex_transform"] = &uses_skip_vertex_transform;

	actions.usage_flag_pointers["ALPHA"] = &uses_alpha;
	actions.usage_flag_pointers["ALPHA_SCISSOR_THRESHOLD"] = &uses_alpha_scissor;
	actions.usage_flag_pointers["ALPHA_HASH_SCALE"] = &uses_alpha_hash;
	actions.usage_flag_pointers["ALPHA_ANTIALIASING_EDGE"] = &uses_alpha_antialiasing;
	actions.usage_flag_pointers["ALPHA_TEXTURE_COORDINATE"] = &uses_alpha_antialiasing;
	actions.render_mode_flags["depth_prepass_alpha"] = &uses_depth_prepass_alpha;
	// GodotNext: nextgi_alpha_policy (#61).
	actions.render_mode_flags["nextgi_rt_opaque"] = &nextgi_rt_opaque;
	// GodotNext: avboit (#295). The render mode's define is set in init(); AVBOIT also needs to know.
	actions.render_mode_flags["shadow_to_opacity"] = &uses_shadow_to_opacity;

	actions.usage_flag_pointers["SSS_STRENGTH"] = &uses_sss;
	actions.usage_flag_pointers["SSS_TRANSMITTANCE_DEPTH"] = &uses_transmittance;

	actions.usage_flag_pointers["DISCARD"] = &uses_discard;
	actions.usage_flag_pointers["TIME"] = &uses_time;
	actions.usage_flag_pointers["ROUGHNESS"] = &uses_roughness;
	actions.usage_flag_pointers["NORMAL"] = &uses_normal;
	actions.usage_flag_pointers["NORMAL_MAP"] = &uses_normal_map;
	actions.usage_flag_pointers["BENT_NORMAL_MAP"] = &uses_bent_normal_map;

	actions.usage_flag_pointers["POINT_SIZE"] = &uses_point_size;
	actions.usage_flag_pointers["POINT_COORD"] = &uses_point_size;

	actions.usage_flag_pointers["TANGENT"] = &uses_tangent;
	actions.usage_flag_pointers["BINORMAL"] = &uses_tangent;
	actions.usage_flag_pointers["ANISOTROPY"] = &uses_tangent;
	actions.usage_flag_pointers["ANISOTROPY_FLOW"] = &uses_tangent;

	// GodotNext: vsm_non_nanite (#218). Camera-facing billboards follow the main camera in shadow passes.
	actions.usage_flag_pointers["MAIN_CAM_INV_VIEW_MATRIX"] = &uses_main_camera;

	actions.write_flag_pointers["MODELVIEW_MATRIX"] = &writes_modelview_or_projection;
	actions.write_flag_pointers["PROJECTION_MATRIX"] = &writes_modelview_or_projection;
	actions.write_flag_pointers["VERTEX"] = &uses_vertex;
	actions.write_flag_pointers["POSITION"] = &uses_position;
	actions.write_flag_pointers["TANGENT"] = &writes_tangent;
	actions.write_flag_pointers["BINORMAL"] = &writes_tangent;
	actions.write_flag_pointers["Z_CLIP_SCALE"] = &uses_z_clip_scale;
	actions.write_flag_pointers["DEPTH"] = &writes_depth;
	actions.write_flag_pointers["DEPTH_OFFSET"] = &uses_depth_offset;

	actions.visibility_resolve = true;
	// GodotNext: nextgeo_raster_bins (#155). The material raster target of the programmable raster bins.
	actions.raster_target = true;

	actions.stencil_mode_values["read"] = Pair<int *, int>(&stencil_readi, STENCIL_FLAG_READ);
	actions.stencil_mode_values["write"] = Pair<int *, int>(&stencil_writei, STENCIL_FLAG_WRITE);
	actions.stencil_mode_values["write_depth_fail"] = Pair<int *, int>(&stencil_write_depth_faili, STENCIL_FLAG_WRITE_DEPTH_FAIL);

	actions.stencil_mode_values["compare_less"] = Pair<int *, int>(&stencil_comparei, STENCIL_COMPARE_LESS);
	actions.stencil_mode_values["compare_equal"] = Pair<int *, int>(&stencil_comparei, STENCIL_COMPARE_EQUAL);
	actions.stencil_mode_values["compare_less_or_equal"] = Pair<int *, int>(&stencil_comparei, STENCIL_COMPARE_LESS_OR_EQUAL);
	actions.stencil_mode_values["compare_greater"] = Pair<int *, int>(&stencil_comparei, STENCIL_COMPARE_GREATER);
	actions.stencil_mode_values["compare_not_equal"] = Pair<int *, int>(&stencil_comparei, STENCIL_COMPARE_NOT_EQUAL);
	actions.stencil_mode_values["compare_greater_or_equal"] = Pair<int *, int>(&stencil_comparei, STENCIL_COMPARE_GREATER_OR_EQUAL);
	actions.stencil_mode_values["compare_always"] = Pair<int *, int>(&stencil_comparei, STENCIL_COMPARE_ALWAYS);

	actions.stencil_reference = &stencil_referencei;

	actions.uniforms = &uniforms;

	Error err = OK;
	{
		MutexLock lock(SceneShaderForwardClustered::singleton_mutex);
		err = SceneShaderForwardClustered::singleton->compiler.compile(RSE::SHADER_SPATIAL, code, &actions, path, gen_code);
	}

	if (err != OK) {
		_free_raster_target();
		_free_resolve_compute(); // GodotNext: nextgeo_shading_bins (#158)
		raster_features = 0;
		raster_rejects = 0;
		nextgeo_masked_fallback = RendererRD::NEXTGEO_FALLBACK_NONE;
		nextgeo_programmable_fallback = RendererRD::NEXTGEO_FALLBACK_NONE;
		if (version.is_valid()) {
			SceneShaderForwardClustered::singleton->shader.version_free(version);
			version = RID();
		}
		ERR_FAIL_MSG("Shader compilation failed.");
	}

	uses_alpha_clip = uses_alpha_scissor || uses_alpha_hash;

	if (version.is_null()) {
		version = SceneShaderForwardClustered::singleton->shader.version_create(false);
	}

	depth_draw = DepthDraw(depth_drawi);
	if (depth_test_disabledi) {
		depth_test = DEPTH_TEST_DISABLED;
	} else if (depth_test_invertedi) {
		depth_test = DEPTH_TEST_ENABLED_INVERTED;
	} else {
		depth_test = DEPTH_TEST_ENABLED;
	}
	cull_mode = RSE::CullMode(cull_modei);
	uses_screen_texture_mipmaps = gen_code.uses_screen_texture_mipmaps;
	uses_screen_texture = gen_code.uses_screen_texture;
	uses_depth_texture = gen_code.uses_depth_texture;
	uses_normal_texture = gen_code.uses_normal_roughness_texture;
	uses_vertex_time = gen_code.uses_vertex_time;
	uses_fragment_time = gen_code.uses_fragment_time;
	uses_normal |= uses_normal_map;
	uses_normal |= uses_bent_normal_map;
	uses_tangent |= uses_normal_map;
	uses_tangent |= uses_bent_normal_map;

	stencil_enabled = stencil_referencei != -1;
	stencil_flags = stencil_readi | stencil_writei | stencil_write_depth_faili;
	stencil_compare = StencilCompare(stencil_comparei);
	stencil_reference = stencil_referencei;

#if 0
	print_line("**compiling shader:");
	print_line("**defines:\n");
	for (int i = 0; i < gen_code.defines.size(); i++) {
		print_line(gen_code.defines[i]);
	}

	HashMap<String, String>::Iterator el = gen_code.code.begin();
	while (el) {
		print_line("\n**code " + el->key + ":\n" + el->value);
		++el;
	}

	print_line("\n**uniforms:\n" + gen_code.uniforms);
	print_line("\n**vertex_globals:\n" + gen_code.stage_globals[ShaderCompiler::STAGE_VERTEX]);
	print_line("\n**fragment_globals:\n" + gen_code.stage_globals[ShaderCompiler::STAGE_FRAGMENT]);
#endif
	// GodotNext: megalights_inputs (#136). Classify before version_set_code so every variant sees the defines.
	{
		RendererRD::MegaLightsInputs::FallbackQuery ml_query;
		ml_query.unshaded = unshaded;
		ml_query.custom_light = gen_code.code.has("light");
		ml_query.diffuse_toon = gen_code.defines.has("#define DIFFUSE_TOON\n");
		ml_query.diffuse_lambert_wrap = gen_code.defines.has("#define DIFFUSE_LAMBERT_WRAP\n");
		ml_query.specular_toon = gen_code.defines.has("#define SPECULAR_TOON\n");
		ml_query.specular_disabled = gen_code.defines.has("#define SPECULAR_DISABLED\n");
		ml_query.rim = gen_code.defines.has("#define LIGHT_RIM_USED\n");
		ml_query.backlight = gen_code.defines.has("#define LIGHT_BACKLIGHT_USED\n");
		ml_query.anisotropy = gen_code.defines.has("#define LIGHT_ANISOTROPY_USED\n");
		const bool ml_diffuse_lambert = RendererRD::MegaLightsInputs::is_diffuse_lambert(unshaded, gen_code.defines.has("#define DIFFUSE_LAMBERT\n"), gen_code.defines.has("#define DIFFUSE_BURLEY\n"), ml_query.diffuse_toon, ml_query.diffuse_lambert_wrap);
		// uses_alpha_pass() reads uses_blend_alpha, which upstream assigns after version_set_code
		// below. Assign the same value here; the later assignment leaves it unchanged.
		uses_blend_alpha = blend_mode_uses_blend_alpha(alpha_antialiasing_mode != ALPHA_ANTIALIASING_OFF ? BLEND_MODE_ALPHA_TO_COVERAGE : BlendMode(blend_mode));
		ml_query.alpha_pass = uses_alpha_pass() && uses_depth_in_alpha_pass();
		if (RendererRD::MegaLightsInputs::is_fallback(ml_query)) {
			gen_code.defines.push_back("#define MEGALIGHTS_FALLBACK\n");
			if (!megalights_fallback_warned && RendererRD::MegaLightsInputs::is_requested() && Engine::get_singleton() && Engine::get_singleton()->is_editor_hint()) {
				megalights_fallback_warned = true;
				const String ml_name = path.is_empty() ? vformat("<built-in>:%d", code.hash()) : path;
				WARN_PRINT_ED(vformat("MegaLights fallback material \"%s\": custom light(), unshaded, toon, wrap, rim, backlight, anisotropy, disabled specular, or an alpha-pass surface with a depth prepass keeps clustered shading.", ml_name));
			}
		} else if (ml_diffuse_lambert) {
			gen_code.defines.push_back("#define MEGALIGHTS_DIFFUSE_LAMBERT\n");
		}
	}
	// GodotNext: nextgeo_raster_bins (#155). The raster target's fragment code stays out of the scene shader.
	String raster_fragment;
	if (gen_code.code.has("raster_fragment")) {
		raster_fragment = gen_code.code["raster_fragment"];
		gen_code.code.erase("raster_fragment");
	}
	// GodotNext: nextgeo_displacement (#150). Save the #163 dicing compute hook out of the scene shader code map;
	// resolve_displacement stays in gen_code.code for scene_forward_clustered.glsl and resolve_compute.glsl.
	has_displacement = gen_code.has_displacement;
	uses_displacement_time = gen_code.uses_displacement_time;
	if (gen_code.code.has("displacement")) {
		displacement_code = gen_code.code["displacement"];
		gen_code.code.erase("displacement");
	}
	SceneShaderForwardClustered::singleton->shader.version_set_code(version, gen_code.code, gen_code.uniforms, gen_code.stage_globals[ShaderCompiler::STAGE_VERTEX], gen_code.stage_globals[ShaderCompiler::STAGE_FRAGMENT], gen_code.defines);
	// GodotNext: nextgi_hit_groups (#130). A new serial for each code, so a hit group is compiled once per code.
	{
		static SafeNumeric<uint64_t> nextgi_hit_serials;
		nextgi_hit_serial = nextgi_hit_serials.increment();
	}

	ubo_size = gen_code.uniform_total_size;
	ubo_offsets = gen_code.uniform_offsets;
	texture_uniforms = gen_code.texture_uniforms;

	pipeline_hash_map.clear_pipelines();

	// If any form of Alpha Antialiasing is enabled, set the blend mode to alpha to coverage.
	if (alpha_antialiasing_mode != ALPHA_ANTIALIASING_OFF) {
		blend_mode = BLEND_MODE_ALPHA_TO_COVERAGE;
	}

	uses_blend_alpha = blend_mode_uses_blend_alpha(BlendMode(blend_mode));
	uses_unsupported_derivative = gen_code.uses_unsupported_derivative;

	// GodotNext: nextgeo_resolve (#80, plan §7.4). Classify material-level NextGeo fallback reason.
	// GodotNext: nextgeo_raster_bins (#155, plan §21.4 N3, N4). The raster features and the raster
	// target's rejects join the classification: a material with a raster program (masked or vertex offset)
	// that reads FRAGCOORD, the screen, depth or normal-roughness texture, or writes the absolute DEPTH
	// stays on legacy raster with UNSUPPORTED_RASTER_FEATURE. Every other material keeps its #80 reason;
	// masked and vertex-offset materials keep theirs unless rendering/nextgeo/debug_programmable_raster
	// admits them (#153 and #164 turn them on).
	const bool force_vertex_shading = GLOBAL_GET_CACHED(bool, "rendering/shading/overrides/force_vertex_shading");
	RendererRD::NextGeoRasterFacts raster_facts;
	raster_facts.alpha_scissor = uses_alpha_scissor;
	raster_facts.alpha_hash = uses_alpha_hash;
	raster_facts.discard = uses_discard;
	raster_facts.writes_vertex = uses_vertex;
	raster_facts.depth_offset = uses_depth_offset;
	raster_facts.cull_mode = cull_mode;
	raster_features = RendererRD::nextgeo_raster_features(raster_facts);
	raster_uses = gen_code.raster_uses | (writes_depth ? uint32_t(ShaderCompiler::RASTER_WRITES_DEPTH) : 0u);
	raster_rejects = RendererRD::nextgeo_raster_rejects(raster_uses, raster_features);

	RendererRD::NextGeoMaterialFallbackFacts fallback_facts;
	fallback_facts.stencil = stencil_enabled;
	fallback_facts.vertex_lighting = uses_vertex_lighting || force_vertex_shading;
	// uses_alpha_pass() without its screen-reading term, which the raster target rejects instead.
	const bool has_base_alpha = uses_alpha && (!uses_alpha_clip || uses_alpha_antialiasing);
	fallback_facts.transparent = has_base_alpha || uses_blend_alpha || depth_draw == DEPTH_DRAW_DISABLED || depth_test != DEPTH_TEST_ENABLED || uses_alpha_antialiasing || alpha_antialiasing_mode != ALPHA_ANTIALIASING_OFF;
	fallback_facts.screen_reads = uses_screen_texture || uses_depth_texture || uses_normal_texture;
	fallback_facts.masked = (raster_features & RendererVirtualMeshStorage::RASTER_FEATURE_MASKED) != 0u;
	fallback_facts.depth_offset = uses_depth_offset;
	fallback_facts.writes_vertex = uses_vertex;
	fallback_facts.other_custom_vertex = uses_position || writes_modelview_or_projection || uses_skip_vertex_transform || uses_z_clip_scale || uses_point_size || uses_particle_trails || wireframe || writes_depth;
	fallback_facts.unsupported_derivative = uses_unsupported_derivative;
	fallback_facts.raster_rejects = raster_rejects;
	nextgeo_fallback = RendererRD::nextgeo_material_fallback(fallback_facts);
	// GodotNext: nextgeo_masked_two_sided (#153), depth_offset (#149), nextgeo_vertex_offset (#164).
	// Default routing on the Nanite tier admits masked, DEPTH_OFFSET, and vertex-offset materials.
	fallback_facts.admit_masked = true;
	fallback_facts.admit_depth_offset = true;
	fallback_facts.admit_vertex_offset = true;
	nextgeo_masked_fallback = RendererRD::nextgeo_material_fallback(fallback_facts);
	fallback_facts.admit_programmable = true;
	nextgeo_programmable_fallback = RendererRD::nextgeo_material_fallback(fallback_facts);

	// The raster target's code, for a material a programmable bin can take. Its versions are made only
	// when a bin or the shader baker first requests them (ensure_raster_target()), so materials that
	// never route through a programmable bin compile nothing more.
	const bool had_raster_versions = raster_sw_version.is_valid() || raster_hw_version.is_valid() || raster_hw_mesh_version.is_valid();
	_free_raster_target();
	// GodotNext: nextgeo_displacement (#150). Shaders defining displacement() also prepare the raster target
	// (including for shader baker) even when shader-level raster_features == 0, because displacement_magnitude
	// is a per-Material property.
	const uint32_t target_features = raster_features | (has_displacement ? uint32_t(RendererVirtualMeshStorage::RASTER_FEATURE_DISPLACED) : 0u);
	const uint32_t target_rejects = RendererRD::nextgeo_raster_rejects(raster_uses, target_features);
	const bool has_raster_frag_or_no_frag = !raster_fragment.is_empty() || !gen_code.code.has("fragment");
	raster_target_valid = RendererRD::nextgeo_raster_has_program(target_features) && nextgeo_programmable_fallback == RendererRD::NEXTGEO_FALLBACK_NONE && target_rejects == 0u && has_raster_frag_or_no_frag &&
			RendererVirtualMeshStorage::resolve_raster_bin(target_features, RendererVirtualMeshStorage::RASTER_PASS_VISIBILITY) == int32_t(RendererVirtualMeshStorage::RASTER_BIN_PROGRAMMABLE);
	if (raster_target_valid) {
		raster_code.clear();
		raster_code["resolve_vertex"] = gen_code.code.has("resolve_vertex") ? gen_code.code["resolve_vertex"] : String();
		raster_code["resolve_vertex_store"] = gen_code.code.has("resolve_vertex_store") ? gen_code.code["resolve_vertex_store"] : String();
		raster_code["resolve_interpolate"] = gen_code.code.has("resolve_interpolate") ? gen_code.code["resolve_interpolate"] : String();
		raster_code["resolve_displacement"] = gen_code.code.has("resolve_displacement") ? gen_code.code["resolve_displacement"] : String();
		raster_code["raster_fragment"] = raster_fragment;
		raster_globals = gen_code.stage_globals[ShaderCompiler::STAGE_COMPUTE];
		raster_uniforms = gen_code.uniforms;
		raster_defines = gen_code.defines;
		if (had_raster_versions) {
			ensure_raster_target();
		}
	}

	// GodotNext: nextgeo_shading_bins (#158, plan §21.4 N7). The compute form of the Stage A/B resolve, for a
	// material NextGeo resolves: the same code the tile quads compile (the Forward+ fragment stage with
	// MODE_VISIBILITY_RESOLVE), so the compute form needs no compiler mode of its own. Its version is made
	// from the scene shader version's code when a shading bin first takes the material, so nothing more is
	// kept or compiled with tile quads.
	_free_resolve_compute();
	RendererRD::NextGeoShadingFacts shading_facts;
	shading_facts.discard = uses_discard;
	shading_facts.alpha_test = uses_alpha_clip;
	shading_facts.alpha_antialiasing = uses_alpha_antialiasing;
	shading_facts.depth_prepass_alpha = uses_depth_prepass_alpha;
	shading_facts.writes_depth = writes_depth;
	resolve_compute_valid = (nextgeo_fallback == RendererRD::NEXTGEO_FALLBACK_NONE || nextgeo_programmable_fallback == RendererRD::NEXTGEO_FALLBACK_NONE) && !RendererRD::nextgeo_shading_compute_rejects(shading_facts);
}

// GodotNext: nextgeo_shading_bins (#158)
RID SceneShaderForwardClustered::ShaderData::get_resolve_compute_shader(ResolveComputeVariant p_variant) {
	ERR_FAIL_UNSIGNED_INDEX_V(uint32_t(p_variant), uint32_t(RESOLVE_COMPUTE_VARIANT_MAX), RID());
	if (!has_resolve_compute() || SceneShaderForwardClustered::singleton == nullptr || version.is_null()) {
		return RID();
	}
	ResolveComputeShaderRD &compute_shader = SceneShaderForwardClustered::singleton->resolve_compute_shader;
	if (resolve_compute_version.is_null()) {
		// The tile quads' code: what set_code() gave the scene shader's version.
		HashMap<String, String> compute_code;
		String compute_uniforms;
		String compute_globals;
		Vector<String> compute_defines;
		if (!SceneShaderForwardClustered::singleton->shader.version_get_fragment_code(version, compute_code, compute_uniforms, compute_globals, compute_defines)) {
			resolve_compute_failed = true;
			return RID();
		}
		MutexLock lock(SceneShaderForwardClustered::singleton_mutex);
		resolve_compute_version = compute_shader.version_create(false);
		compute_shader.version_set_compute_code(resolve_compute_version, compute_code, compute_uniforms, compute_globals, compute_defines);
	}
	if (!compute_shader.is_group_enabled(p_variant)) {
		compute_shader.enable_group(p_variant);
	}
	const RID shader_rid = compute_shader.version_get_shader(resolve_compute_version, p_variant);
	if (shader_rid.is_null()) {
		resolve_compute_failed = true;
	}
	return shader_rid;
}

RID SceneShaderForwardClustered::ShaderData::get_resolve_compute_pipeline(ResolveComputeVariant p_variant, ShaderSpecialization p_specialization) {
	if (!has_resolve_compute()) {
		return RID();
	}
	ResolveComputePipelineKey key;
	key.variant = uint32_t(p_variant);
	key.spec_packed_0 = p_specialization.packed_0;
	key.spec_packed_1 = p_specialization.packed_1;
	const RID *existing = resolve_compute_pipelines.getptr(key);
	if (existing != nullptr) {
		return *existing;
	}
	const RID shader_rid = get_resolve_compute_shader(p_variant);
	if (shader_rid.is_null()) {
		return RID();
	}
	// The specialization constants of the tile quads' pipelines (_create_pipeline()); the resolve never
	// emulates point size.
	Vector<RD::PipelineSpecializationConstant> specialization_constants;
	RD::PipelineSpecializationConstant sc;
	sc.constant_id = 0;
	sc.int_value = p_specialization.packed_0;
	sc.type = RD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_INT;
	specialization_constants.push_back(sc);
	sc.constant_id = 1;
	sc.int_value = p_specialization.packed_1;
	specialization_constants.push_back(sc);
	sc = {};
	sc.constant_id = 2;
	sc.bool_value = false;
	sc.type = RD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_BOOL;
	specialization_constants.push_back(sc);
	const RID pipeline = RD::get_singleton()->compute_pipeline_create(shader_rid, specialization_constants);
	if (pipeline.is_null()) {
		resolve_compute_failed = true;
		return RID();
	}
	resolve_compute_pipelines.insert(key, pipeline);
	return pipeline;
}

void SceneShaderForwardClustered::ShaderData::_free_resolve_compute() {
	resolve_compute_valid = false;
	resolve_compute_failed = false;
	RenderingDevice *rd = RD::get_singleton();
	for (const KeyValue<ResolveComputePipelineKey, RID> &E : resolve_compute_pipelines) {
		if (rd != nullptr && E.value.is_valid() && rd->compute_pipeline_is_valid(E.value)) {
			rd->free_rid(E.value);
		}
	}
	resolve_compute_pipelines.clear();
	if (resolve_compute_version.is_valid() && SceneShaderForwardClustered::singleton != nullptr) {
		SceneShaderForwardClustered::singleton->resolve_compute_shader.version_free(resolve_compute_version);
	}
	resolve_compute_version = RID();
}

bool SceneShaderForwardClustered::ShaderData::ensure_raster_target() {
	if (!raster_target_valid || SceneShaderForwardClustered::singleton == nullptr) {
		return false;
	}
	if (raster_sw_version.is_valid() && raster_hw_version.is_valid() && raster_hw_mesh_version.is_valid()) {
		return true;
	}
	MutexLock lock(SceneShaderForwardClustered::singleton_mutex);
	if (raster_sw_version.is_null()) {
		raster_sw_version = SceneShaderForwardClustered::singleton->raster_sw_shader.version_create(false);
		SceneShaderForwardClustered::singleton->raster_sw_shader.version_set_compute_code(raster_sw_version, raster_code, raster_uniforms, raster_globals, raster_defines);
	}
	if (raster_hw_version.is_null()) {
		raster_hw_version = SceneShaderForwardClustered::singleton->raster_hw_shader.version_create(false);
		SceneShaderForwardClustered::singleton->raster_hw_shader.version_set_code(raster_hw_version, raster_code, raster_uniforms, raster_globals, raster_globals, raster_defines);
	}
	if (raster_hw_mesh_version.is_null()) {
		raster_hw_mesh_version = SceneShaderForwardClustered::singleton->raster_hw_mesh_shader.version_create(false);
		SceneShaderForwardClustered::singleton->raster_hw_mesh_shader.version_set_code(raster_hw_mesh_version, raster_code, raster_uniforms, raster_globals, raster_globals, raster_defines);
	}
	return raster_sw_version.is_valid() && raster_hw_version.is_valid() && raster_hw_mesh_version.is_valid();
}

void SceneShaderForwardClustered::ShaderData::_free_raster_target() {
	raster_target_valid = false;
	raster_code.clear();
	raster_globals = String();
	raster_uniforms = String();
	raster_defines.clear();
	if (SceneShaderForwardClustered::singleton == nullptr) {
		return;
	}
	if (raster_sw_version.is_valid()) {
		SceneShaderForwardClustered::singleton->raster_sw_shader.version_free(raster_sw_version);
		raster_sw_version = RID();
	}
	if (raster_hw_version.is_valid()) {
		SceneShaderForwardClustered::singleton->raster_hw_shader.version_free(raster_hw_version);
		raster_hw_version = RID();
	}
	if (raster_hw_mesh_version.is_valid()) {
		SceneShaderForwardClustered::singleton->raster_hw_mesh_shader.version_free(raster_hw_mesh_version);
		raster_hw_mesh_version = RID();
	}
	// GodotNext: nextgeo_vertex_offset (#164).
	if (rt_vertex_offset_version.is_valid()) {
		SceneShaderForwardClustered::singleton->rt_vertex_offset_shader.version_free(rt_vertex_offset_version);
		rt_vertex_offset_version = RID();
	}
	// GodotNext: nextgeo_tessellation (#163).
	if (tessellation_dice_version.is_valid()) {
		SceneShaderForwardClustered::singleton->tessellation_dice_shader.version_free(tessellation_dice_version);
		tessellation_dice_version = RID();
	}
	if (tessellation_hw_version.is_valid()) {
		SceneShaderForwardClustered::singleton->tessellation_hw_shader.version_free(tessellation_hw_version);
		tessellation_hw_version = RID();
	}
}

// GodotNext: nextgeo_vertex_offset (#164, plan §21.4 N5, GN-12). Lazily compiles the RT proxy
// vertex-offset compute shader only when an instance opts in via rt_evaluate_vertex_offset.
RID SceneShaderForwardClustered::ShaderData::get_rt_vertex_offset_shader() {
	if (!uses_vertex || !raster_target_valid || SceneShaderForwardClustered::singleton == nullptr) {
		return RID();
	}
	MutexLock lock(SceneShaderForwardClustered::singleton_mutex);
	RtVertexOffsetShaderRD &rt_shader = SceneShaderForwardClustered::singleton->rt_vertex_offset_shader;
	if (rt_vertex_offset_version.is_null()) {
		HashMap<String, String> rt_code;
		rt_code["resolve_vertex"] = raster_code.has("resolve_vertex") ? raster_code["resolve_vertex"] : String();
		rt_vertex_offset_version = rt_shader.version_create(false);
		rt_shader.version_set_compute_code(rt_vertex_offset_version, rt_code, raster_uniforms, raster_globals, raster_defines);
	}
	if (!rt_shader.is_group_enabled(0)) {
		rt_shader.enable_group(0);
	}
	return rt_shader.version_get_shader(rt_vertex_offset_version, 0);
}

// GodotNext: nextgeo_tessellation (#163, plan §21.4 contract N11). Lazily compiles tessellation_dice.glsl
// and tessellation_hw.glsl using #150's displacement() compiler output only when tessellation is active.
bool SceneShaderForwardClustered::ShaderData::ensure_tessellation_target() {
	if (!raster_target_valid || !has_displacement || SceneShaderForwardClustered::singleton == nullptr) {
		return false;
	}
	if (tessellation_dice_version.is_valid() && tessellation_hw_version.is_valid()) {
		return true;
	}
	MutexLock lock(SceneShaderForwardClustered::singleton_mutex);
	if (tessellation_dice_version.is_null()) {
		tessellation_dice_version = SceneShaderForwardClustered::singleton->tessellation_dice_shader.version_create(false);
		SceneShaderForwardClustered::singleton->tessellation_dice_shader.version_set_compute_code(tessellation_dice_version, raster_code, raster_uniforms, raster_globals, raster_defines);
	}
	if (tessellation_hw_version.is_null()) {
		tessellation_hw_version = SceneShaderForwardClustered::singleton->tessellation_hw_shader.version_create(false);
		SceneShaderForwardClustered::singleton->tessellation_hw_shader.version_set_code(tessellation_hw_version, raster_code, raster_uniforms, raster_globals, raster_globals, raster_defines);
	}
	return tessellation_dice_version.is_valid() && tessellation_hw_version.is_valid();
}

RID SceneShaderForwardClustered::ShaderData::get_tessellation_dice_shader(bool p_buffer) const {
	if (tessellation_dice_version.is_null() || SceneShaderForwardClustered::singleton == nullptr || !SceneShaderForwardClustered::singleton->raster_target_ready) {
		return RID();
	}
	const bool supported = p_buffer ? SceneShaderForwardClustered::singleton->raster_buffer_int64_supported : SceneShaderForwardClustered::singleton->raster_image_int64_supported;
	if (!supported) {
		return RID();
	}
	const int group = p_buffer ? 1 : 0;
	if (!SceneShaderForwardClustered::singleton->tessellation_dice_shader.is_group_enabled(group)) {
		SceneShaderForwardClustered::singleton->tessellation_dice_shader.enable_group(group);
	}
	return SceneShaderForwardClustered::singleton->tessellation_dice_shader.version_get_shader(tessellation_dice_version, group);
}

RID SceneShaderForwardClustered::ShaderData::get_tessellation_hw_shader(bool p_buffer) const {
	if (tessellation_hw_version.is_null() || SceneShaderForwardClustered::singleton == nullptr || !SceneShaderForwardClustered::singleton->raster_target_ready) {
		return RID();
	}
	const bool supported = p_buffer ? SceneShaderForwardClustered::singleton->raster_buffer_int64_supported : SceneShaderForwardClustered::singleton->raster_image_int64_supported;
	if (!supported) {
		return RID();
	}
	const int group = p_buffer ? 1 : 0;
	if (!SceneShaderForwardClustered::singleton->tessellation_hw_shader.is_group_enabled(group)) {
		SceneShaderForwardClustered::singleton->tessellation_hw_shader.enable_group(group);
	}
	return SceneShaderForwardClustered::singleton->tessellation_hw_shader.version_get_shader(tessellation_hw_version, group);
}

RID SceneShaderForwardClustered::ShaderData::get_tessellation_vsm_dice_shader() const {
	if (tessellation_dice_version.is_null() || SceneShaderForwardClustered::singleton == nullptr || !SceneShaderForwardClustered::singleton->raster_vsm_supported) {
		return RID();
	}
	constexpr int group = 2;
	if (!SceneShaderForwardClustered::singleton->tessellation_dice_shader.is_group_enabled(group)) {
		SceneShaderForwardClustered::singleton->tessellation_dice_shader.enable_group(group);
	}
	return SceneShaderForwardClustered::singleton->tessellation_dice_shader.version_get_shader(tessellation_dice_version, group);
}

RID SceneShaderForwardClustered::ShaderData::get_tessellation_vsm_hw_shader() const {
	if (tessellation_hw_version.is_null() || SceneShaderForwardClustered::singleton == nullptr || !SceneShaderForwardClustered::singleton->raster_vsm_supported) {
		return RID();
	}
	constexpr int group = 2;
	if (!SceneShaderForwardClustered::singleton->tessellation_hw_shader.is_group_enabled(group)) {
		SceneShaderForwardClustered::singleton->tessellation_hw_shader.enable_group(group);
	}
	return SceneShaderForwardClustered::singleton->tessellation_hw_shader.version_get_shader(tessellation_hw_version, group);
}

RID SceneShaderForwardClustered::ShaderData::get_raster_target_shader(bool p_hardware, bool p_buffer) const {
	if (raster_sw_version.is_null() || raster_hw_version.is_null() || SceneShaderForwardClustered::singleton == nullptr || !SceneShaderForwardClustered::singleton->raster_target_ready) {
		return RID();
	}
	const bool supported = p_buffer ? SceneShaderForwardClustered::singleton->raster_buffer_int64_supported : SceneShaderForwardClustered::singleton->raster_image_int64_supported;
	if (!supported) {
		return RID();
	}
	const int group = p_buffer ? 1 : 0;
	if (p_hardware) {
		if (!SceneShaderForwardClustered::singleton->raster_hw_shader.is_group_enabled(group)) {
			SceneShaderForwardClustered::singleton->raster_hw_shader.enable_group(group);
		}
		return SceneShaderForwardClustered::singleton->raster_hw_shader.version_get_shader(raster_hw_version, group);
	}
	if (!SceneShaderForwardClustered::singleton->raster_sw_shader.is_group_enabled(group)) {
		SceneShaderForwardClustered::singleton->raster_sw_shader.enable_group(group);
	}
	return SceneShaderForwardClustered::singleton->raster_sw_shader.version_get_shader(raster_sw_version, group);
}

// GodotNext: vsm_nanite (#217, plan §21.3 contract S4, step 7). Depth-only VSM form (group 2): 32-bit
// imageAtomicMax into the physical page pool, sharing #155's raster-feature table without 64-bit atomics.
RID SceneShaderForwardClustered::ShaderData::get_raster_target_vsm_shader(bool p_hardware) const {
	if (raster_sw_version.is_null() || raster_hw_version.is_null() || SceneShaderForwardClustered::singleton == nullptr || !SceneShaderForwardClustered::singleton->raster_vsm_supported) {
		return RID();
	}
	constexpr int group = 2;
	if (p_hardware) {
		if (!SceneShaderForwardClustered::singleton->raster_hw_shader.is_group_enabled(group)) {
			SceneShaderForwardClustered::singleton->raster_hw_shader.enable_group(group);
		}
		return SceneShaderForwardClustered::singleton->raster_hw_shader.version_get_shader(raster_hw_version, group);
	}
	if (!SceneShaderForwardClustered::singleton->raster_sw_shader.is_group_enabled(group)) {
		SceneShaderForwardClustered::singleton->raster_sw_shader.enable_group(group);
	}
	return SceneShaderForwardClustered::singleton->raster_sw_shader.version_get_shader(raster_sw_version, group);
}

// GodotNext: nextgeo_mesh_raster (#154, plan §21.4 contract N9). Mesh-shader HW raster target
// (group 0 = VIS64 image, group 1 = VIS64 buffer); stays disabled on devices without mesh support.
RID SceneShaderForwardClustered::ShaderData::get_raster_target_mesh_shader(bool p_buffer) const {
	if (raster_hw_mesh_version.is_null() || SceneShaderForwardClustered::singleton == nullptr || !SceneShaderForwardClustered::singleton->raster_target_ready || !SceneShaderForwardClustered::singleton->raster_mesh_supported) {
		return RID();
	}
	const bool supported = p_buffer ? SceneShaderForwardClustered::singleton->raster_buffer_int64_supported : SceneShaderForwardClustered::singleton->raster_image_int64_supported;
	if (!supported) {
		return RID();
	}
	const int group = p_buffer ? 1 : 0;
	if (!SceneShaderForwardClustered::singleton->raster_hw_mesh_shader.is_group_enabled(group)) {
		SceneShaderForwardClustered::singleton->raster_hw_mesh_shader.enable_group(group);
	}
	return SceneShaderForwardClustered::singleton->raster_hw_mesh_shader.version_get_shader(raster_hw_mesh_version, group);
}

// GodotNext: nextgeo_mesh_raster (#154) x vsm_nanite (#217). Depth-only VSM mesh-shader HW raster target (group 2).
RID SceneShaderForwardClustered::ShaderData::get_raster_target_vsm_mesh_shader() const {
	if (raster_hw_mesh_version.is_null() || SceneShaderForwardClustered::singleton == nullptr || !SceneShaderForwardClustered::singleton->raster_vsm_supported || !SceneShaderForwardClustered::singleton->raster_mesh_supported) {
		return RID();
	}
	constexpr int group = 2;
	if (!SceneShaderForwardClustered::singleton->raster_hw_mesh_shader.is_group_enabled(group)) {
		SceneShaderForwardClustered::singleton->raster_hw_mesh_shader.enable_group(group);
	}
	return SceneShaderForwardClustered::singleton->raster_hw_mesh_shader.version_get_shader(raster_hw_mesh_version, group);
}

void SceneShaderForwardClustered::ShaderData::get_extra_native_shader_and_versions(LocalVector<Pair<ShaderRD *, RID>> &r_versions) {
	if (raster_target_valid && ensure_raster_target()) {
		r_versions.push_back({ &SceneShaderForwardClustered::singleton->raster_sw_shader, raster_sw_version });
		r_versions.push_back({ &SceneShaderForwardClustered::singleton->raster_hw_shader, raster_hw_version });
		r_versions.push_back({ &SceneShaderForwardClustered::singleton->raster_hw_mesh_shader, raster_hw_mesh_version });
	}
}

// GodotNext: nextgi_hit_groups (#130)
bool SceneShaderForwardClustered::ShaderData::build_nextgi_hit_source(const String &p_code, const String &p_path, RendererRD::NextGIHitShaderSource &r_source) {
	if (p_code.is_empty() || SceneShaderForwardClustered::singleton == nullptr) {
		return false;
	}
	ShaderCompiler::GeneratedCode gen_code;
	ShaderCompiler::IdentifierActions actions;
	actions.entry_point_stages["vertex"] = ShaderCompiler::STAGE_VERTEX;
	actions.entry_point_stages["fragment"] = ShaderCompiler::STAGE_FRAGMENT;
	// In hit mode light() is noted (code["light"] exists, empty) but neither it nor the functions only it uses
	// are generated.
	actions.entry_point_stages["light"] = ShaderCompiler::STAGE_FRAGMENT;
	bool alpha_scissor = false;
	bool alpha_hash = false;
	bool discard = false;
	bool depth = false;
	bool position = false;
	bool modelview_or_projection = false;
	actions.usage_flag_pointers["ALPHA_SCISSOR_THRESHOLD"] = &alpha_scissor;
	actions.usage_flag_pointers["ALPHA_HASH_SCALE"] = &alpha_hash;
	actions.usage_flag_pointers["DISCARD"] = &discard;
	actions.write_flag_pointers["DEPTH"] = &depth;
	actions.write_flag_pointers["POSITION"] = &position;
	actions.write_flag_pointers["MODELVIEW_MATRIX"] = &modelview_or_projection;
	actions.write_flag_pointers["PROJECTION_MATRIX"] = &modelview_or_projection;
	HashMap<StringName, ShaderLanguage::ShaderNode::Uniform> hit_uniforms;
	actions.uniforms = &hit_uniforms;
	actions.nextgi_hit = true;

	Error err = OK;
	{
		MutexLock lock(SceneShaderForwardClustered::singleton_mutex);
		err = SceneShaderForwardClustered::singleton->compiler.compile(RSE::SHADER_SPATIAL, p_code, &actions, p_path, gen_code);
	}
	if (err != OK) {
		return false;
	}

	r_source.defines = gen_code.defines;
	r_source.uniforms = gen_code.uniforms;
	r_source.globals = gen_code.stage_globals[ShaderCompiler::STAGE_FRAGMENT];
	r_source.vertex_code = gen_code.code.has("vertex") ? gen_code.code["vertex"] : String();
	r_source.fragment_code = gen_code.code.has("fragment") ? gen_code.code["fragment"] : String();
	r_source.textures = gen_code.texture_uniforms;
	r_source.ubo_size = gen_code.uniform_total_size;
	r_source.has_light = gen_code.code.has("light");
	r_source.alpha_scissor = alpha_scissor;
	r_source.alpha_hash = alpha_hash;
	r_source.screen_read = gen_code.uses_screen_texture || gen_code.uses_depth_texture || gen_code.uses_normal_roughness_texture;
	r_source.uses_discard = discard;
	r_source.writes_depth = depth;
	r_source.writes_position = position || modelview_or_projection;
	r_source.instance_uniforms = false;
	for (const KeyValue<StringName, ShaderLanguage::ShaderNode::Uniform> &E : hit_uniforms) {
		if (E.value.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_INSTANCE) {
			r_source.instance_uniforms = true;
		}
	}
	return true;
}

bool SceneShaderForwardClustered::ShaderData::is_animated() const {
	return (uses_fragment_time && uses_discard) || (uses_vertex_time && uses_vertex);
}

// GodotNext: vsm_caching (#212, plan §21.3 contract S5).
uint32_t SceneShaderForwardClustered::ShaderData::get_vsm_dynamic_flags() const {
	using VSM = RendererVirtualShadowMapStorage;
	uint32_t flags = 0u;
	if (uses_vertex || uses_position || writes_modelview_or_projection || uses_skip_vertex_transform) {
		flags |= VSM::CACHE_DYNAMIC_VERTEX;
	}
	if (uses_vertex_time) {
		flags |= VSM::CACHE_DYNAMIC_VERTEX_TIME;
	}
	if (writes_depth || (raster_features & RendererVirtualMeshStorage::RASTER_FEATURE_DEPTH_OFFSET) != 0u) {
		flags |= VSM::CACHE_DYNAMIC_DEPTH;
	}
	if (is_animated() || uses_displacement_time) {
		flags |= VSM::CACHE_DYNAMIC_ANIMATED;
	}
	if (uses_main_camera) {
		flags |= VSM::CACHE_DYNAMIC_MAIN_CAMERA;
	}
	return flags;
}

RendererRD::MaterialStorage::ShaderData::NextGIRTClass SceneShaderForwardClustered::ShaderData::get_nextgi_rt_class() const {
	if (uses_alpha_clip || uses_discard) {
		return nextgi_rt_opaque ? RendererRD::MaterialStorage::ShaderData::NEXTGI_RT_OPAQUE : RendererRD::MaterialStorage::ShaderData::NEXTGI_RT_ALPHA_TESTED;
	}
	const bool has_read_screen_alpha = uses_screen_texture || uses_depth_texture || uses_normal_texture;
	const bool has_base_alpha = (uses_alpha && (!uses_alpha_clip || uses_alpha_antialiasing)) || has_read_screen_alpha;
	const bool has_alpha = has_base_alpha || uses_blend_alpha;
	return has_alpha ? RendererRD::MaterialStorage::ShaderData::NEXTGI_RT_BLENDED : RendererRD::MaterialStorage::ShaderData::NEXTGI_RT_OPAQUE;
}

bool SceneShaderForwardClustered::ShaderData::casts_shadows() const {
	bool has_read_screen_alpha = uses_screen_texture || uses_depth_texture || uses_normal_texture;
	bool has_base_alpha = (uses_alpha && (!uses_alpha_clip || uses_alpha_antialiasing)) || has_read_screen_alpha;
	bool has_alpha = has_base_alpha || uses_blend_alpha;

	return !has_alpha || (uses_depth_prepass_alpha && !(depth_draw == DEPTH_DRAW_DISABLED || depth_test != DEPTH_TEST_ENABLED));
}

RenderingServerTypes::ShaderNativeSourceCode SceneShaderForwardClustered::ShaderData::get_native_source_code() const {
	if (version.is_valid()) {
		return SceneShaderForwardClustered::singleton->shader.version_get_native_source_code(version);
	} else {
		return RenderingServerTypes::ShaderNativeSourceCode();
	}
}

Pair<ShaderRD *, RID> SceneShaderForwardClustered::ShaderData::get_native_shader_and_version() const {
	if (version.is_valid()) {
		return { &SceneShaderForwardClustered::singleton->shader, version };
	} else {
		return {};
	}
}

uint16_t SceneShaderForwardClustered::ShaderData::_get_shader_version(PipelineVersion p_pipeline_version, uint32_t p_color_pass_flags, bool p_ubershader) const {
	uint32_t ubershader_base = p_ubershader ? ShaderVersion::SHADER_VERSION_COLOR_PASS : 0;
	switch (p_pipeline_version) {
		case PIPELINE_VERSION_DEPTH_PASS:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS + ubershader_base;
		case PIPELINE_VERSION_DEPTH_PASS_DP:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_DP + ubershader_base;
		case PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS + ubershader_base;
		case PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI + ubershader_base;
		case PIPELINE_VERSION_DEPTH_PASS_MULTIVIEW:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_MULTIVIEW + ubershader_base;
		case PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_MULTIVIEW:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_MULTIVIEW + ubershader_base;
		case PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_MULTIVIEW:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_MULTIVIEW + ubershader_base;
		case PIPELINE_VERSION_DEPTH_PASS_WITH_MATERIAL:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_WITH_MATERIAL + ubershader_base;
		case PIPELINE_VERSION_NEXTGI_TRANSLUCENCY_MARK:
			return ShaderVersion::SHADER_VERSION_NEXTGI_TRANSLUCENCY_MARK + ubershader_base;
		case PIPELINE_VERSION_NEXTGEO_FALLBACK:
			return ShaderVersion::SHADER_VERSION_NEXTGEO_FALLBACK + ubershader_base;
		case PIPELINE_VERSION_DEPTH_PASS_WITH_SDF:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_WITH_SDF + ubershader_base;
		case PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_MOTION:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_MOTION + ubershader_base;
		case PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_AND_MOTION:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_AND_MOTION + ubershader_base;
		case PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_MOTION_MULTIVIEW:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_MOTION_MULTIVIEW + ubershader_base;
		case PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_AND_MOTION_MULTIVIEW:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_AND_MOTION_MULTIVIEW + ubershader_base;
		case PIPELINE_VERSION_COLOR_PASS: {
			int shader_flags = 0;

			if (p_ubershader) {
				shader_flags |= SHADER_COLOR_PASS_FLAG_UBERSHADER;
			}

			if (p_color_pass_flags & PIPELINE_COLOR_PASS_FLAG_SEPARATE_SPECULAR) {
				shader_flags |= SHADER_COLOR_PASS_FLAG_SEPARATE_SPECULAR;
			}

			if (p_color_pass_flags & PIPELINE_COLOR_PASS_FLAG_MOTION_VECTORS) {
				shader_flags |= SHADER_COLOR_PASS_FLAG_MOTION_VECTORS;
			}

			if (p_color_pass_flags & PIPELINE_COLOR_PASS_FLAG_LIGHTMAP) {
				shader_flags |= SHADER_COLOR_PASS_FLAG_LIGHTMAP;
			}

			if (p_color_pass_flags & PIPELINE_COLOR_PASS_FLAG_MULTIVIEW) {
				shader_flags |= SHADER_COLOR_PASS_FLAG_MULTIVIEW;
			}

			// GodotNext: rr_guides (#205)
			if (p_color_pass_flags & PIPELINE_COLOR_PASS_FLAG_RR_GUIDES) {
				shader_flags |= SHADER_COLOR_PASS_FLAG_RR_GUIDES;
			}

			return ShaderVersion::SHADER_VERSION_COLOR_PASS * 2 + shader_flags;
		} break;
		case PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES:
			return ShaderVersion::SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES;
		case PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI:
			return ShaderVersion::SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI;
		case PIPELINE_VERSION_VISIBILITY_RESOLVE_COLOR: {
			const bool sep_spec = (p_color_pass_flags & PIPELINE_COLOR_PASS_FLAG_SEPARATE_SPECULAR) != 0;
			const bool motion = (p_color_pass_flags & PIPELINE_COLOR_PASS_FLAG_MOTION_VECTORS) != 0;
			// GodotNext: rr_guides (#205). Same order as the four variants without the guides.
			if (p_color_pass_flags & PIPELINE_COLOR_PASS_FLAG_RR_GUIDES) {
				return ShaderVersion::SHADER_VERSION_VISIBILITY_RESOLVE_RR_GUIDES_BASE + (sep_spec ? 1 : 0) + (motion ? 2 : 0);
			}
			if (sep_spec && motion) {
				return ShaderVersion::SHADER_VERSION_VISIBILITY_RESOLVE_COLOR_SEPARATE_SPECULAR_MOTION_VECTORS;
			} else if (motion) {
				return ShaderVersion::SHADER_VERSION_VISIBILITY_RESOLVE_COLOR_MOTION_VECTORS;
			} else if (sep_spec) {
				return ShaderVersion::SHADER_VERSION_VISIBILITY_RESOLVE_COLOR_SEPARATE_SPECULAR;
			}
			return ShaderVersion::SHADER_VERSION_VISIBILITY_RESOLVE_COLOR;
		} break;
		// GodotNext: megalights_inputs (#136). These variants are appended outside the ubershader loop.
		// The depth variants have ubershader twins so the 32-byte fallback push constant matches.
		case PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS + (p_ubershader ? ShaderVersion::SHADER_VERSION_MEGALIGHTS_INPUTS_COUNT : 0);
		case PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_VOXEL_GI:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_VOXEL_GI + (p_ubershader ? ShaderVersion::SHADER_VERSION_MEGALIGHTS_INPUTS_COUNT : 0);
		case PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_MOTION:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_MOTION + (p_ubershader ? ShaderVersion::SHADER_VERSION_MEGALIGHTS_INPUTS_COUNT : 0);
		case PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_VOXEL_GI_AND_MOTION:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_VOXEL_GI_AND_MOTION + (p_ubershader ? ShaderVersion::SHADER_VERSION_MEGALIGHTS_INPUTS_COUNT : 0);
		case PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_MULTIVIEW:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_MULTIVIEW + (p_ubershader ? ShaderVersion::SHADER_VERSION_MEGALIGHTS_INPUTS_COUNT : 0);
		case PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_VOXEL_GI_MULTIVIEW:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_VOXEL_GI_MULTIVIEW + (p_ubershader ? ShaderVersion::SHADER_VERSION_MEGALIGHTS_INPUTS_COUNT : 0);
		case PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_MOTION_MULTIVIEW:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_MOTION_MULTIVIEW + (p_ubershader ? ShaderVersion::SHADER_VERSION_MEGALIGHTS_INPUTS_COUNT : 0);
		case PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_VOXEL_GI_AND_MOTION_MULTIVIEW:
			return ShaderVersion::SHADER_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_VOXEL_GI_AND_MOTION_MULTIVIEW + (p_ubershader ? ShaderVersion::SHADER_VERSION_MEGALIGHTS_INPUTS_COUNT : 0);
		case PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_ML:
			return ShaderVersion::SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_ML;
		case PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI_ML:
			return ShaderVersion::SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI_ML;
		// GodotNext: vsm_non_nanite (#218).
		case PIPELINE_VERSION_VSM_DEPTH:
			return p_ubershader ? ShaderVersion::SHADER_VERSION_VSM_DEPTH_UBERSHADER : ShaderVersion::SHADER_VERSION_VSM_DEPTH;
		// GodotNext: avboit (#295). The ubershader twins follow the two pre-pass variants.
		case PIPELINE_VERSION_AVBOIT_BOUNDS:
			return ShaderVersion::SHADER_VERSION_AVBOIT_BOUNDS + (p_ubershader ? ShaderVersion::SHADER_VERSION_AVBOIT_PASS_COUNT : 0);
		case PIPELINE_VERSION_AVBOIT_OCCLUSION:
			return ShaderVersion::SHADER_VERSION_AVBOIT_OCCLUSION + (p_ubershader ? ShaderVersion::SHADER_VERSION_AVBOIT_PASS_COUNT : 0);
		// GodotNext: nextgeo_vertex_offset (#164). Stage A guides with motion vectors for WPO.
		case PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_MOTION:
			return ShaderVersion::SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_MOTION;
		case PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI_MOTION:
			return ShaderVersion::SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI_MOTION;
		case PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_ML_MOTION:
			return ShaderVersion::SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_ML_MOTION;
		case PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI_ML_MOTION:
			return ShaderVersion::SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI_ML_MOTION;
		default: {
			DEV_ASSERT(false && "Unknown pipeline version.");
			return 0;
		} break;
	}
}

void SceneShaderForwardClustered::ShaderData::_create_pipeline(PipelineKey p_pipeline_key) {
#if PRINT_PIPELINE_COMPILATION_KEYS
	print_line(
			"HASH:", p_pipeline_key.hash(),
			"VERSION:", version,
			"VERTEX:", p_pipeline_key.vertex_format_id,
			"FRAMEBUFFER:", p_pipeline_key.framebuffer_format_id,
			"CULL:", p_pipeline_key.cull_mode,
			"PRIMITIVE:", p_pipeline_key.primitive_type,
			"VERSION:", p_pipeline_key.version,
			"PASS FLAGS:", p_pipeline_key.color_pass_flags,
			"SPEC PACKED #0:", p_pipeline_key.shader_specialization.packed_0,
			"WIREFRAME:", p_pipeline_key.wireframe);
#endif

	// Color pass -> attachment 0: Color/Diffuse, attachment 1: Separate Specular, attachment 2: Motion Vectors
	// GodotNext: rr_guides (#205). With the RR guides -> attachment 3: Diffuse Albedo, attachment 4: Specular Albedo.
	const int color_pass_attachments = (p_pipeline_key.color_pass_flags & PIPELINE_COLOR_PASS_FLAG_RR_GUIDES) ? COLOR_PASS_ATTACHMENTS_RR_GUIDES : COLOR_PASS_ATTACHMENTS;
	RD::PipelineColorBlendState::Attachment blend_attachment = blend_mode_to_blend_attachment(BlendMode(blend_mode));
	RD::PipelineColorBlendState blend_state_color_blend;
	blend_state_color_blend.attachments = { blend_attachment, RD::PipelineColorBlendState::Attachment(), RD::PipelineColorBlendState::Attachment() };
	RD::PipelineColorBlendState blend_state_color_opaque = RD::PipelineColorBlendState::create_disabled(color_pass_attachments);
	RD::PipelineColorBlendState blend_state_depth_normal_roughness = RD::PipelineColorBlendState::create_disabled(1);
	RD::PipelineColorBlendState blend_state_depth_normal_roughness_giprobe = RD::PipelineColorBlendState::create_disabled(2);

	RD::PipelineDepthStencilState depth_stencil_state;

	if (depth_test != DEPTH_TEST_DISABLED) {
		depth_stencil_state.enable_depth_test = true;
		depth_stencil_state.enable_depth_write = depth_draw != DEPTH_DRAW_DISABLED ? true : false;
		depth_stencil_state.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;

		if (depth_test == DEPTH_TEST_ENABLED_INVERTED) {
			depth_stencil_state.depth_compare_operator = RD::COMPARE_OP_LESS;
		}
	}

	const bool is_visibility_resolve = p_pipeline_key.version == PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES ||
			p_pipeline_key.version == PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI ||
			p_pipeline_key.version == PIPELINE_VERSION_VISIBILITY_RESOLVE_COLOR ||
			// GodotNext: megalights_inputs (#136)
			p_pipeline_key.version == PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_ML ||
			p_pipeline_key.version == PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI_ML ||
			// GodotNext: nextgeo_vertex_offset (#164)
			p_pipeline_key.version == PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_MOTION ||
			p_pipeline_key.version == PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI_MOTION ||
			p_pipeline_key.version == PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_ML_MOTION ||
			p_pipeline_key.version == PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI_ML_MOTION;

	// GodotNext: vsm_non_nanite (#218). The VSM depth variant has no depth attachment.
	// GodotNext: avboit (#295). The AVBOIT pre-passes draw into depth-less low-resolution grids.
	const bool is_avboit_prepass = p_pipeline_key.version == PIPELINE_VERSION_AVBOIT_BOUNDS || p_pipeline_key.version == PIPELINE_VERSION_AVBOIT_OCCLUSION;
	if (p_pipeline_key.version == PIPELINE_VERSION_NEXTGI_TRANSLUCENCY_MARK || p_pipeline_key.version == PIPELINE_VERSION_VSM_DEPTH || is_visibility_resolve || is_avboit_prepass) {
		depth_stencil_state = RD::PipelineDepthStencilState();
	} else if (p_pipeline_key.version == PIPELINE_VERSION_NEXTGEO_FALLBACK) {
		depth_stencil_state = RD::PipelineDepthStencilState();
		depth_stencil_state.enable_depth_test = true;
		depth_stencil_state.enable_depth_write = false;
		depth_stencil_state.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;
	}
	bool use_stencil = stencil_enabled && p_pipeline_key.version == PIPELINE_VERSION_COLOR_PASS;
	depth_stencil_state.enable_stencil = use_stencil;
	if (use_stencil) {
		static const RD::CompareOperator stencil_compare_rd_table[STENCIL_COMPARE_MAX] = {
			RD::COMPARE_OP_LESS,
			RD::COMPARE_OP_EQUAL,
			RD::COMPARE_OP_LESS_OR_EQUAL,
			RD::COMPARE_OP_GREATER,
			RD::COMPARE_OP_NOT_EQUAL,
			RD::COMPARE_OP_GREATER_OR_EQUAL,
			RD::COMPARE_OP_ALWAYS,
		};

		uint32_t stencil_mask = 255;

		RD::PipelineDepthStencilState::StencilOperationState op;
		op.fail = RD::STENCIL_OP_KEEP;
		op.pass = RD::STENCIL_OP_KEEP;
		op.depth_fail = RD::STENCIL_OP_KEEP;
		op.compare = stencil_compare_rd_table[stencil_compare];
		op.compare_mask = 0;
		op.write_mask = 0;
		op.reference = stencil_reference;

		if (stencil_flags & STENCIL_FLAG_READ) {
			op.compare_mask = stencil_mask;
		}

		if (stencil_flags & STENCIL_FLAG_WRITE) {
			op.pass = RD::STENCIL_OP_REPLACE;
			op.write_mask = stencil_mask;
		}

		if (stencil_flags & STENCIL_FLAG_WRITE_DEPTH_FAIL) {
			op.depth_fail = RD::STENCIL_OP_REPLACE;
			op.write_mask = stencil_mask;
		}

		depth_stencil_state.front_op = op;
		depth_stencil_state.back_op = op;
	}

	const bool depth_pre_pass_enabled = SceneShaderForwardClustered::singleton->depth_prepass_enabled;

	RD::RenderPrimitive primitive_rd_table[RSE::PRIMITIVE_MAX] = {
		RD::RENDER_PRIMITIVE_POINTS,
		RD::RENDER_PRIMITIVE_LINES,
		RD::RENDER_PRIMITIVE_LINESTRIPS,
		RD::RENDER_PRIMITIVE_TRIANGLES,
		RD::RENDER_PRIMITIVE_TRIANGLE_STRIPS,
	};

	// GodotNext: vsm_non_nanite (#218). The VSM depth variant never emulates point sizes.
	bool emulate_point_size_flag = uses_point_size && !is_visibility_resolve && p_pipeline_key.version != PIPELINE_VERSION_VSM_DEPTH && SceneShaderForwardClustered::singleton->emulate_point_size;

	RD::RenderPrimitive primitive_rd;
	if (is_visibility_resolve) {
		primitive_rd = RD::RENDER_PRIMITIVE_TRIANGLES;
	} else if (uses_point_size) {
		primitive_rd = emulate_point_size_flag ? RD::RENDER_PRIMITIVE_TRIANGLES : RD::RENDER_PRIMITIVE_POINTS;
	} else {
		primitive_rd = primitive_rd_table[p_pipeline_key.primitive_type];
	}

	RD::PipelineRasterizationState raster_state;
	raster_state.cull_mode = is_visibility_resolve ? RD::POLYGON_CULL_DISABLED : p_pipeline_key.cull_mode;
	raster_state.wireframe = is_visibility_resolve ? false : (wireframe || p_pipeline_key.wireframe);

	RD::PipelineMultisampleState multisample_state;
	multisample_state.sample_count = RD::get_singleton()->framebuffer_format_get_texture_samples(p_pipeline_key.framebuffer_format_id, 0);

	RD::PipelineColorBlendState blend_state;
	if (p_pipeline_key.version == PIPELINE_VERSION_COLOR_PASS) {
		if (p_pipeline_key.color_pass_flags & PIPELINE_COLOR_PASS_FLAG_TRANSPARENT) {
			if (alpha_antialiasing_mode == ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE) {
				multisample_state.enable_alpha_to_coverage = true;
			} else if (alpha_antialiasing_mode == ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE_AND_TO_ONE) {
				multisample_state.enable_alpha_to_coverage = true;
				multisample_state.enable_alpha_to_one = true;
			}

			blend_state = blend_state_color_blend;

			if (depth_draw == DEPTH_DRAW_OPAQUE) {
				depth_stencil_state.enable_depth_write = false; //alpha does not draw depth
			}

			// GodotNext: avboit (#295). The AVBOIT composite adds into its accumulation target and only
			// tests depth: a depth write would make a surface hide the ones drawn after it.
			if (p_pipeline_key.avboit) {
				blend_state.attachments.write[0] = RendererRD::AVBOIT::composite_blend_attachment();
				depth_stencil_state.enable_depth_write = false;
				multisample_state.enable_alpha_to_coverage = false;
				multisample_state.enable_alpha_to_one = false;
			}
		} else {
			blend_state = blend_state_color_opaque;

			if (depth_pre_pass_enabled) {
				// We already have a depth from the depth pre-pass, there is no need to write it again.
				// In addition we can use COMPARE_OP_EQUAL instead of COMPARE_OP_LESS_OR_EQUAL.
				// This way we can use the early depth test to discard transparent fragments before the fragment shader even starts.
				depth_stencil_state.depth_compare_operator = RD::COMPARE_OP_EQUAL;
				depth_stencil_state.enable_depth_write = false;
			}
		}
	} else {
		switch (p_pipeline_key.version) {
			case PIPELINE_VERSION_NEXTGI_TRANSLUCENCY_MARK:
			case PIPELINE_VERSION_NEXTGEO_FALLBACK:
			case PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS:
			case PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_MULTIVIEW:
				blend_state = blend_state_depth_normal_roughness;
				break;
			case PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI:
			case PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_MULTIVIEW:
				blend_state = blend_state_depth_normal_roughness_giprobe;
				break;
			case PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_MOTION:
			case PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_MOTION_MULTIVIEW:
				blend_state = RD::PipelineColorBlendState::create_disabled(2);
				break;
			case PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_AND_MOTION:
			case PIPELINE_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_AND_MOTION_MULTIVIEW:
				blend_state = RD::PipelineColorBlendState::create_disabled(3);
				break;
			case PIPELINE_VERSION_DEPTH_PASS_WITH_MATERIAL:
				// Writes to normal and roughness in opaque way.
				blend_state = RD::PipelineColorBlendState::create_disabled(5);
				break;
			case PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES:
				blend_state = RD::PipelineColorBlendState::create_disabled(2);
				break;
			case PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI:
			case PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_MOTION: // GodotNext: nextgeo_vertex_offset (#164)
				blend_state = RD::PipelineColorBlendState::create_disabled(3);
				break;
			// GodotNext: megalights_inputs (#136). Velocity stays the last color attachment.
			case PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS:
			case PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_MULTIVIEW:
				blend_state = RD::PipelineColorBlendState::create_disabled(3);
				break;
			case PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_VOXEL_GI:
			case PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_VOXEL_GI_MULTIVIEW:
			case PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_MOTION:
			case PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_MOTION_MULTIVIEW:
			case PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_ML:
			case PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI_MOTION: // GodotNext: nextgeo_vertex_offset (#164)
				blend_state = RD::PipelineColorBlendState::create_disabled(4);
				break;
			case PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_VOXEL_GI_AND_MOTION:
			case PIPELINE_VERSION_DEPTH_PASS_WITH_MEGALIGHTS_INPUTS_AND_VOXEL_GI_AND_MOTION_MULTIVIEW:
			case PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI_ML:
			case PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_ML_MOTION: // GodotNext: nextgeo_vertex_offset (#164)
				blend_state = RD::PipelineColorBlendState::create_disabled(5);
				break;
			case PIPELINE_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI_ML_MOTION: // GodotNext: nextgeo_vertex_offset (#164)
				blend_state = RD::PipelineColorBlendState::create_disabled(6);
				break;
			case PIPELINE_VERSION_VISIBILITY_RESOLVE_COLOR:
				blend_state = blend_state_color_opaque;
				break;
			// GodotNext: avboit (#295)
			case PIPELINE_VERSION_AVBOIT_BOUNDS:
				blend_state = RendererRD::AVBOIT::bounds_blend_state();
				break;
			case PIPELINE_VERSION_AVBOIT_OCCLUSION:
				blend_state = RendererRD::AVBOIT::occlusion_blend_state();
				break;
			case PIPELINE_VERSION_DEPTH_PASS:
			case PIPELINE_VERSION_DEPTH_PASS_DP:
			case PIPELINE_VERSION_DEPTH_PASS_MULTIVIEW:
			case PIPELINE_VERSION_DEPTH_PASS_WITH_SDF:
			case PIPELINE_VERSION_VSM_DEPTH: // GodotNext: vsm_non_nanite (#218)
			default:
				break;
		}
	}

	if (p_pipeline_key.zero_velocity_write && !blend_state.attachments.is_empty()) {
		// GodotNext: rr_guides (#205). Velocity is the last attachment, except in a color pass
		// that writes the RR guides after it.
		const int velocity_index = (p_pipeline_key.color_pass_flags & PIPELINE_COLOR_PASS_FLAG_RR_GUIDES) ? int(COLOR_PASS_ATTACHMENT_VELOCITY) : blend_state.attachments.size() - 1;
		RD::PipelineColorBlendState::Attachment &velocity_attachment = blend_state.attachments.write[velocity_index];
		velocity_attachment.write_r = false;
		velocity_attachment.write_g = false;
		velocity_attachment.write_b = false;
		velocity_attachment.write_a = false;
	}

	// Convert the specialization from the key to pipeline specialization constants.
	Vector<RD::PipelineSpecializationConstant> specialization_constants;
	RD::PipelineSpecializationConstant sc;
	sc.constant_id = 0;
	sc.int_value = p_pipeline_key.shader_specialization.packed_0;
	sc.type = RD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_INT;
	specialization_constants.push_back(sc);

	sc.constant_id = 1;
	sc.int_value = p_pipeline_key.shader_specialization.packed_1;
	sc.type = RD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_INT;
	specialization_constants.push_back(sc);

	sc = {}; // Sanitize value bits. "bool_value" only assigns 8 bits and keeps the remaining bits intact.
	sc.constant_id = 2;
	sc.bool_value = emulate_point_size_flag;
	sc.type = RD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_BOOL;
	specialization_constants.push_back(sc);

	RID shader_rid = get_shader_variant(p_pipeline_key.version, p_pipeline_key.color_pass_flags, p_pipeline_key.ubershader);
	ERR_FAIL_COND(shader_rid.is_null());

	RID pipeline = RD::get_singleton()->render_pipeline_create(shader_rid, p_pipeline_key.framebuffer_format_id, p_pipeline_key.vertex_format_id, primitive_rd, raster_state, multisample_state, depth_stencil_state, blend_state, 0, 0, specialization_constants);
	ERR_FAIL_COND(pipeline.is_null());

	pipeline_hash_map.add_compiled_pipeline(p_pipeline_key.hash(), pipeline);
}

RD::PolygonCullMode SceneShaderForwardClustered::ShaderData::get_cull_mode_from_cull_variant(CullVariant p_cull_variant) {
	const RD::PolygonCullMode cull_mode_rd_table[CULL_VARIANT_MAX][3] = {
		{ RD::POLYGON_CULL_DISABLED, RD::POLYGON_CULL_FRONT, RD::POLYGON_CULL_BACK },
		{ RD::POLYGON_CULL_DISABLED, RD::POLYGON_CULL_BACK, RD::POLYGON_CULL_FRONT },
		{ RD::POLYGON_CULL_DISABLED, RD::POLYGON_CULL_DISABLED, RD::POLYGON_CULL_DISABLED }
	};

	return cull_mode_rd_table[p_cull_variant][cull_mode];
}

RID SceneShaderForwardClustered::ShaderData::_get_shader_variant(uint16_t p_shader_version) const {
	if (version.is_valid()) {
		ERR_FAIL_NULL_V(SceneShaderForwardClustered::singleton, RID());
		return SceneShaderForwardClustered::singleton->shader.version_get_shader(version, p_shader_version);
	} else {
		return RID();
	}
}

void SceneShaderForwardClustered::ShaderData::_clear_vertex_input_mask_cache() {
	for (uint32_t i = 0; i < VERTEX_INPUT_MASKS_SIZE; i++) {
		vertex_input_masks[i].store(0);
	}
}

RID SceneShaderForwardClustered::ShaderData::get_shader_variant(PipelineVersion p_pipeline_version, uint32_t p_color_pass_flags, bool p_ubershader) const {
	return _get_shader_variant(_get_shader_version(p_pipeline_version, p_color_pass_flags, p_ubershader));
}

uint64_t SceneShaderForwardClustered::ShaderData::get_vertex_input_mask(PipelineVersion p_pipeline_version, uint32_t p_color_pass_flags, bool p_ubershader) {
	// Vertex input masks require knowledge of the shader. Since querying the shader can be expensive due to high contention and the necessary mutex, we cache the result instead.
	uint16_t shader_version = _get_shader_version(p_pipeline_version, p_color_pass_flags, p_ubershader);
	uint64_t input_mask = vertex_input_masks[shader_version].load(std::memory_order_relaxed);
	if (input_mask == 0) {
		RID shader_rid = _get_shader_variant(shader_version);
		ERR_FAIL_COND_V(shader_rid.is_null(), 0);

		input_mask = RD::get_singleton()->shader_get_vertex_input_attribute_mask(shader_rid);
		vertex_input_masks[shader_version].store(input_mask, std::memory_order_relaxed);
	}

	return input_mask;
}

bool SceneShaderForwardClustered::ShaderData::is_valid() const {
	if (version.is_valid()) {
		ERR_FAIL_NULL_V(SceneShaderForwardClustered::singleton, false);
		return SceneShaderForwardClustered::singleton->shader.version_is_valid(version);
	} else {
		return false;
	}
}

SceneShaderForwardClustered::ShaderData::ShaderData() :
		shader_list_element(this) {
	pipeline_hash_map.set_creation_object_and_function(this, &ShaderData::_create_pipeline);
	pipeline_hash_map.set_compilations(SceneShaderForwardClustered::singleton->pipeline_compilations, &SceneShaderForwardClustered::singleton_mutex);
}

SceneShaderForwardClustered::ShaderData::~ShaderData() {
	pipeline_hash_map.clear_pipelines();
	// GodotNext: nextgeo_raster_bins (#155).
	_free_raster_target();
	// GodotNext: nextgeo_shading_bins (#158).
	_free_resolve_compute();

	if (version.is_valid()) {
		ERR_FAIL_NULL(SceneShaderForwardClustered::singleton);
		SceneShaderForwardClustered::singleton->shader.version_free(version);
	}
}

RendererRD::MaterialStorage::ShaderData *SceneShaderForwardClustered::_create_shader_func() {
	MutexLock lock(SceneShaderForwardClustered::singleton_mutex);
	ShaderData *shader_data = memnew(ShaderData);
	singleton->shader_list.add(&shader_data->shader_list_element);
	return shader_data;
}

void SceneShaderForwardClustered::MaterialData::set_render_priority(int p_priority) {
	priority = p_priority - RSE::MATERIAL_RENDER_PRIORITY_MIN; //8 bits
}

void SceneShaderForwardClustered::MaterialData::set_next_pass(RID p_pass) {
	next_pass = p_pass;
}

// GodotNext: nextgeo_displacement (#150, plan §21.4 N11).
void SceneShaderForwardClustered::MaterialData::set_displacement(float p_magnitude, float p_center) {
	displacement_magnitude = p_magnitude;
	displacement_center = p_center;
}

// GodotNext: nextgeo_vertex_offset (#164, plan §21.4 N5).
void SceneShaderForwardClustered::MaterialData::set_vertex_offset_max(float p_vertex_offset_max) {
	vertex_offset_max = p_vertex_offset_max;
}

float SceneShaderForwardClustered::MaterialData::get_effective_vertex_offset_max(float p_project_default) const {
	if (shader_data == nullptr || !shader_data->uses_vertex) {
		return 0.0f;
	}
	return RendererRD::nextgeo_resolve_vertex_offset_max(vertex_offset_max, p_project_default);
}

bool SceneShaderForwardClustered::MaterialData::get_rt_vertex_offset_uniforms(Vector<RD::Uniform> &r_uniforms) const {
	if (shader_data == nullptr) {
		return false;
	}
	return get_parameter_uniforms(r_uniforms, shader_data->texture_uniforms, shader_data->ubo_size, true);
}

uint32_t SceneShaderForwardClustered::MaterialData::get_raster_features() const {
	uint32_t features = shader_data != nullptr ? shader_data->get_raster_features() : 0u;
	if (shader_data != nullptr && shader_data->has_displacement && displacement_magnitude > 0.0f) {
		features |= RendererVirtualMeshStorage::RASTER_FEATURE_DISPLACED;
	}
	return features;
}

bool SceneShaderForwardClustered::MaterialData::update_parameters(const HashMap<StringName, Variant> &p_parameters, bool p_uniform_dirty, bool p_textures_dirty) {
	if (shader_data->version.is_valid()) {
		RID shader_rid = SceneShaderForwardClustered::singleton->shader.version_get_shader(shader_data->version, 0);

		MutexLock lock(SceneShaderForwardClustered::singleton_mutex);
		return update_parameters_uniform_set(p_parameters, p_uniform_dirty, p_textures_dirty, shader_data->uniforms, shader_data->ubo_offsets.ptr(), shader_data->texture_uniforms, shader_data->default_texture_params, shader_data->ubo_size, uniform_set, shader_rid, RenderForwardClustered::MATERIAL_UNIFORM_SET, true, true);
	} else {
		return false;
	}
}

SceneShaderForwardClustered::MaterialData::~MaterialData() {
	free_parameters_uniform_set(uniform_set);
}

RendererRD::MaterialStorage::MaterialData *SceneShaderForwardClustered::_create_material_func(ShaderData *p_shader) {
	MaterialData *material_data = memnew(MaterialData);
	material_data->shader_data = p_shader;
	//update will happen later anyway so do nothing.
	return material_data;
}

SceneShaderForwardClustered *SceneShaderForwardClustered::singleton = nullptr;
Mutex SceneShaderForwardClustered::singleton_mutex;

SceneShaderForwardClustered::SceneShaderForwardClustered() {
	// there should be only one of these, contained within our RenderFM singleton.
	singleton = this;
}

SceneShaderForwardClustered::~SceneShaderForwardClustered() {
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();

	RD::get_singleton()->free_rid(default_vec4_xform_buffer);
	RD::get_singleton()->free_rid(shadow_sampler);
	RD::get_singleton()->free_rid(default_material_feedback_buffer);

	material_storage->shader_free(overdraw_material_shader);
	material_storage->shader_free(default_shader);
	material_storage->shader_free(debug_shadow_splits_material_shader);

	material_storage->material_free(overdraw_material);
	material_storage->material_free(default_material);
	material_storage->material_free(debug_shadow_splits_material);
}

void SceneShaderForwardClustered::init(const String p_defines) {
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();

	emulate_point_size = !RD::get_singleton()->has_feature(RD::SUPPORTS_POINT_SIZE);

	depth_prepass_enabled = GLOBAL_GET("rendering/driver/depth_prepass/enable");

	{
		Vector<ShaderRD::VariantDefine> shader_versions;
		for (uint32_t ubershader = 0; ubershader < 2; ubershader++) {
			const String base_define = ubershader ? "\n#define UBERSHADER\n" : "";
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_BASE, base_define + "\n#define MODE_RENDER_DEPTH\n", true)); // SHADER_VERSION_DEPTH_PASS
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_BASE, base_define + "\n#define MODE_RENDER_DEPTH\n#define MODE_DUAL_PARABOLOID\n", true)); // SHADER_VERSION_DEPTH_PASS_DP
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_BASE, base_define + "\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n", true)); // SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_ADVANCED, base_define + "\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_RENDER_VOXEL_GI\n", false)); // SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_MULTIVIEW, base_define + "\n#define USE_MULTIVIEW\n#define MODE_RENDER_DEPTH\n", false)); // SHADER_VERSION_DEPTH_PASS_MULTIVIEW
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_MULTIVIEW, base_define + "\n#define USE_MULTIVIEW\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n", false)); // SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_MULTIVIEW
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_ADVANCED_MULTIVIEW, base_define + "\n#define USE_MULTIVIEW\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_RENDER_VOXEL_GI\n", false)); // SHADER_VERSION_DEPTH_PASS_WITH_NORMAL_AND_ROUGHNESS_AND_VOXEL_GI_MULTIVIEW
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_ADVANCED, base_define + "\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_MATERIAL\n", false)); // SHADER_VERSION_DEPTH_PASS_WITH_MATERIAL
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_ADVANCED, base_define + "\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_SDF\n", false)); // SHADER_VERSION_DEPTH_PASS_WITH_SDF
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_ADVANCED, base_define + "\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MOTION_VECTORS\n", false));
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_ADVANCED, base_define + "\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_RENDER_VOXEL_GI\n#define MOTION_VECTORS\n", false));
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_ADVANCED_MULTIVIEW, base_define + "\n#define USE_MULTIVIEW\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MOTION_VECTORS\n", false));
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_ADVANCED_MULTIVIEW, base_define + "\n#define USE_MULTIVIEW\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_RENDER_VOXEL_GI\n#define MOTION_VECTORS\n", false));
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_BASE, base_define + "\n#define MODE_RENDER_NEXTGI_TRANSLUCENCY_MARK\n", true));
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_BASE, base_define + "\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NEXTGEO_FALLBACK\n", true));
		}

		Vector<String> color_pass_flags = {
			"\n#define UBERSHADER\n", // SHADER_COLOR_PASS_FLAG_UBERSHADER
			"\n#define MODE_SEPARATE_SPECULAR\n", // SHADER_COLOR_PASS_FLAG_SEPARATE_SPECULAR
			"\n#define USE_LIGHTMAP\n", // SHADER_COLOR_PASS_FLAG_LIGHTMAP
			"\n#define USE_MULTIVIEW\n", // SHADER_COLOR_PASS_FLAG_MULTIVIEW
			"\n#define MOTION_VECTORS\n", // SHADER_COLOR_PASS_FLAG_MOTION_VECTORS
			"\n#define MODE_RR_GUIDES\n", // SHADER_COLOR_PASS_FLAG_RR_GUIDES
		};

		for (int i = 0; i < SHADER_COLOR_PASS_FLAG_COUNT; i++) {
			String version = "";
			for (int j = 0; (1 << j) < SHADER_COLOR_PASS_FLAG_COUNT; j += 1) {
				if ((1 << j) & i) {
					version += color_pass_flags[j];
				}
			}

			// Assign a group based on what features this pass contains.
			ShaderGroup group = SHADER_GROUP_BASE;
			bool advanced_group = (i & SHADER_COLOR_PASS_FLAG_SEPARATE_SPECULAR) || (i & SHADER_COLOR_PASS_FLAG_LIGHTMAP) || (i & SHADER_COLOR_PASS_FLAG_MOTION_VECTORS);
			bool multiview_group = i & SHADER_COLOR_PASS_FLAG_MULTIVIEW;
			if (advanced_group && multiview_group) {
				group = SHADER_GROUP_ADVANCED_MULTIVIEW;
			} else if (advanced_group) {
				group = SHADER_GROUP_ADVANCED;
			} else if (multiview_group) {
				group = SHADER_GROUP_MULTIVIEW;
			}
			// GodotNext: rr_guides (#205). Every variant with the guide outputs is compiled only
			// while a view writes the guides, whatever else it combines with.
			if (i & SHADER_COLOR_PASS_FLAG_RR_GUIDES) {
				group = color_pass_variant_rr_guides_group(uint32_t(i));
			}

			shader_versions.push_back(ShaderRD::VariantDefine(group, version, false));
		}

		// GodotNext: nextgeo_resolve (#80). Stage A guide variants and Stage B opaque color variants.
		shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_NEXTGEO, "\n#define MODE_VISIBILITY_RESOLVE\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n", false)); // SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES
		shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_NEXTGEO, "\n#define MODE_VISIBILITY_RESOLVE\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_RENDER_VOXEL_GI\n", false)); // SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI
		shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_NEXTGEO, "\n#define MODE_VISIBILITY_RESOLVE\n", false)); // SHADER_VERSION_VISIBILITY_RESOLVE_COLOR
		shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_NEXTGEO, "\n#define MODE_VISIBILITY_RESOLVE\n#define MODE_SEPARATE_SPECULAR\n", false)); // SHADER_VERSION_VISIBILITY_RESOLVE_COLOR_SEPARATE_SPECULAR
		shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_NEXTGEO, "\n#define MODE_VISIBILITY_RESOLVE\n#define MOTION_VECTORS\n", false)); // SHADER_VERSION_VISIBILITY_RESOLVE_COLOR_MOTION_VECTORS
		shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_NEXTGEO, "\n#define MODE_VISIBILITY_RESOLVE\n#define MODE_SEPARATE_SPECULAR\n#define MOTION_VECTORS\n", false)); // SHADER_VERSION_VISIBILITY_RESOLVE_COLOR_SEPARATE_SPECULAR_MOTION_VECTORS

		// GodotNext: megalights_inputs (#136). Dedicated groups, off until inputs are active.
		// Ubershader twins start at SHADER_VERSION_MEGALIGHTS_INPUTS_COUNT. Stage A has no twin.
		const uint16_t ml_variant_count = ShaderVersion::SHADER_VERSION_MEGALIGHTS_INPUTS_COUNT + ShaderVersion::SHADER_VERSION_MEGALIGHTS_INPUTS_UBER_COUNT;
		for (uint16_t ml_i = 0; ml_i < ml_variant_count; ml_i++) {
			shader_versions.push_back(ShaderRD::VariantDefine(ShaderVersion::megalights_variant_group(ml_i), ShaderVersion::megalights_variant_defines(ml_i), ShaderVersion::MEGALIGHTS_VARIANTS_DEFAULT_ENABLED));
		}

		// GodotNext: rr_guides (#205). Stage B with the RR guide outputs, in the order of the four
		// Stage B color variants above. Off until a NextGeo view writes the guides.
		shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_NEXTGEO_RR_GUIDES, "\n#define MODE_VISIBILITY_RESOLVE\n#define MODE_RR_GUIDES\n", false));
		shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_NEXTGEO_RR_GUIDES, "\n#define MODE_VISIBILITY_RESOLVE\n#define MODE_SEPARATE_SPECULAR\n#define MODE_RR_GUIDES\n", false));
		shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_NEXTGEO_RR_GUIDES, "\n#define MODE_VISIBILITY_RESOLVE\n#define MOTION_VECTORS\n#define MODE_RR_GUIDES\n", false));
		shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_NEXTGEO_RR_GUIDES, "\n#define MODE_VISIBILITY_RESOLVE\n#define MODE_SEPARATE_SPECULAR\n#define MOTION_VECTORS\n#define MODE_RR_GUIDES\n", false));

		// GodotNext: vsm_non_nanite (#218). Off until VSM raster runs. The VSM_* constants reach every variant
		// through the shader's defines (vsm_projection (#222): the light loop samples the maps).
		shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_VSM, "\n#define MODE_RENDER_DEPTH\n#define MODE_VSM\n", false));
		shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_VSM, "\n#define UBERSHADER\n#define MODE_RENDER_DEPTH\n#define MODE_VSM\n", false));

		// GodotNext: avboit (#295). The AVBOIT bounds and occlusion pre-passes, then their ubershader twins.
		// They run the material's fragment code for its alpha, so they are depth variants with outputs.
		for (uint32_t ubershader = 0; ubershader < 2; ubershader++) {
			const String base_define = ubershader ? "\n#define UBERSHADER\n" : "";
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_AVBOIT, base_define + "\n#define MODE_RENDER_DEPTH\n#define MODE_AVBOIT_PREPASS\n#define MODE_AVBOIT_BOUNDS\n", false)); // SHADER_VERSION_AVBOIT_BOUNDS
			shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_AVBOIT, base_define + "\n#define MODE_RENDER_DEPTH\n#define MODE_AVBOIT_PREPASS\n#define MODE_AVBOIT_OCCLUSION\n", false)); // SHADER_VERSION_AVBOIT_OCCLUSION
		}

		// GodotNext: nextgeo_vertex_offset (#164). Stage A guides with motion vectors so VERTEX_OFFSET
		// buckets write accurate velocity before NextGI and motion-pass upscalers.
		shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_NEXTGEO_MOTION, "\n#define MODE_VISIBILITY_RESOLVE\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MOTION_VECTORS\n", false)); // SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_MOTION
		shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_NEXTGEO_MOTION, "\n#define MODE_VISIBILITY_RESOLVE\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_RENDER_VOXEL_GI\n#define MOTION_VECTORS\n", false)); // SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI_MOTION
		shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_NEXTGEO_MEGALIGHTS_MOTION, "\n#define MODE_VISIBILITY_RESOLVE\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_MEGALIGHTS_INPUTS\n#define MOTION_VECTORS\n", false)); // SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_ML_MOTION
		shader_versions.push_back(ShaderRD::VariantDefine(SHADER_GROUP_NEXTGEO_MEGALIGHTS_MOTION, "\n#define MODE_VISIBILITY_RESOLVE\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_RENDER_VOXEL_GI\n#define MODE_MEGALIGHTS_INPUTS\n#define MOTION_VECTORS\n", false)); // SHADER_VERSION_VISIBILITY_RESOLVE_GUIDES_VOXEL_GI_ML_MOTION

		Vector<uint64_t> dynamic_buffers;
		dynamic_buffers.push_back(ShaderRD::DynamicBuffer::encode(RenderForwardClustered::RENDER_PASS_UNIFORM_SET, 2));
		// GodotNext: megalights_inputs (#136). The F0 pack constants and ML_QUANTIZE_CHROMA reach every
		// variant through the general defines; the MegaLights input variants use them.
		shader.initialize(shader_versions, p_defines + String("\n") + RendererRD::MegaLightsInputs::glsl_pack_prelude(), Vector<RD::PipelineImmutableSampler>(), dynamic_buffers);

		// GodotNext: nextgeo_shading_bins (#158, plan §21.4 N7). The compute form of the resolve variants above,
		// with the same general defines and dynamic buffers, so its sets 0, 1 and 3 take the tile quads' uniforms.
		Vector<ShaderRD::VariantDefine> compute_versions;
		compute_versions.push_back(ShaderRD::VariantDefine(RESOLVE_COMPUTE_GUIDES, "\n#define NEXTGEO_RESOLVE_COMPUTE\n#define MODE_VISIBILITY_RESOLVE\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n", false));
		compute_versions.push_back(ShaderRD::VariantDefine(RESOLVE_COMPUTE_GUIDES_VOXEL_GI, "\n#define NEXTGEO_RESOLVE_COMPUTE\n#define MODE_VISIBILITY_RESOLVE\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_RENDER_VOXEL_GI\n", false));
		compute_versions.push_back(ShaderRD::VariantDefine(RESOLVE_COMPUTE_GUIDES_ML, "\n#define NEXTGEO_RESOLVE_COMPUTE\n#define MODE_VISIBILITY_RESOLVE\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_MEGALIGHTS_INPUTS\n", false));
		compute_versions.push_back(ShaderRD::VariantDefine(RESOLVE_COMPUTE_GUIDES_VOXEL_GI_ML, "\n#define NEXTGEO_RESOLVE_COMPUTE\n#define MODE_VISIBILITY_RESOLVE\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_RENDER_VOXEL_GI\n#define MODE_MEGALIGHTS_INPUTS\n", false));
		compute_versions.push_back(ShaderRD::VariantDefine(RESOLVE_COMPUTE_COLOR, "\n#define NEXTGEO_RESOLVE_COMPUTE\n#define MODE_VISIBILITY_RESOLVE\n", false));
		compute_versions.push_back(ShaderRD::VariantDefine(RESOLVE_COMPUTE_COLOR_SEPARATE_SPECULAR, "\n#define NEXTGEO_RESOLVE_COMPUTE\n#define MODE_VISIBILITY_RESOLVE\n#define MODE_SEPARATE_SPECULAR\n", false));
		compute_versions.push_back(ShaderRD::VariantDefine(RESOLVE_COMPUTE_COLOR_MOTION_VECTORS, "\n#define NEXTGEO_RESOLVE_COMPUTE\n#define MODE_VISIBILITY_RESOLVE\n#define MOTION_VECTORS\n", false));
		compute_versions.push_back(ShaderRD::VariantDefine(RESOLVE_COMPUTE_COLOR_SEPARATE_SPECULAR_MOTION_VECTORS, "\n#define NEXTGEO_RESOLVE_COMPUTE\n#define MODE_VISIBILITY_RESOLVE\n#define MODE_SEPARATE_SPECULAR\n#define MOTION_VECTORS\n", false));
		// GodotNext: nextgeo_vertex_offset (#164). Stage A guides with velocity output.
		compute_versions.push_back(ShaderRD::VariantDefine(RESOLVE_COMPUTE_GUIDES_MOTION, "\n#define NEXTGEO_RESOLVE_COMPUTE\n#define MODE_VISIBILITY_RESOLVE\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MOTION_VECTORS\n", false));
		compute_versions.push_back(ShaderRD::VariantDefine(RESOLVE_COMPUTE_GUIDES_VOXEL_GI_MOTION, "\n#define NEXTGEO_RESOLVE_COMPUTE\n#define MODE_VISIBILITY_RESOLVE\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_RENDER_VOXEL_GI\n#define MOTION_VECTORS\n", false));
		compute_versions.push_back(ShaderRD::VariantDefine(RESOLVE_COMPUTE_GUIDES_ML_MOTION, "\n#define NEXTGEO_RESOLVE_COMPUTE\n#define MODE_VISIBILITY_RESOLVE\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_MEGALIGHTS_INPUTS\n#define MOTION_VECTORS\n", false));
		compute_versions.push_back(ShaderRD::VariantDefine(RESOLVE_COMPUTE_GUIDES_VOXEL_GI_ML_MOTION, "\n#define NEXTGEO_RESOLVE_COMPUTE\n#define MODE_VISIBILITY_RESOLVE\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_RENDER_VOXEL_GI\n#define MODE_MEGALIGHTS_INPUTS\n#define MOTION_VECTORS\n", false));
		resolve_compute_shader.initialize(compute_versions, p_defines + String("\n") + RendererRD::MegaLightsInputs::glsl_pack_prelude(), Vector<RD::PipelineImmutableSampler>(), dynamic_buffers);

		if (RendererCompositorRD::get_singleton()->is_xr_enabled()) {
			shader.enable_group(SHADER_GROUP_MULTIVIEW);
		}
	}

	material_storage->shader_set_data_request_function(RendererRD::MaterialStorage::SHADER_TYPE_3D, _create_shader_funcs);
	material_storage->material_set_data_request_function(RendererRD::MaterialStorage::SHADER_TYPE_3D, _create_material_funcs);

	{
		//shader compiler
		ShaderCompiler::DefaultIdentifierActions actions;

		actions.renames["MODEL_MATRIX"] = "read_model_matrix";
		actions.renames["MODEL_NORMAL_MATRIX"] = "model_normal_matrix";
		actions.renames["VIEW_MATRIX"] = "read_view_matrix";
		actions.renames["INV_VIEW_MATRIX"] = "inv_view_matrix";
		actions.renames["PROJECTION_MATRIX"] = "projection_matrix";
		actions.renames["INV_PROJECTION_MATRIX"] = "inv_projection_matrix";
		actions.renames["MODELVIEW_MATRIX"] = "modelview";
		actions.renames["MODELVIEW_NORMAL_MATRIX"] = "modelview_normal";
		actions.renames["MAIN_CAM_INV_VIEW_MATRIX"] = "scene_data.main_cam_inv_view_matrix";

		actions.renames["VERTEX"] = "vertex";
		actions.renames["NORMAL"] = "normal_highp";
		actions.renames["TANGENT"] = "tangent";
		actions.renames["BINORMAL"] = "binormal";
		actions.renames["POSITION"] = "position";
		actions.renames["UV"] = "uv_interp";
		actions.renames["STREAMING_LOD"] = "streaming_lod";
		actions.renames["UV2"] = "uv2_interp";
		actions.renames["COLOR"] = "color_interp";
		actions.renames["POINT_SIZE"] = "point_size";
		actions.renames["INSTANCE_ID"] = "INSTANCE_INDEX";
		actions.renames["VERTEX_ID"] = "VERTEX_INDEX";
		actions.renames["Z_CLIP_SCALE"] = "z_clip_scale";

		actions.renames["ALPHA_SCISSOR_THRESHOLD"] = "alpha_scissor_threshold";
		actions.renames["ALPHA_HASH_SCALE"] = "alpha_hash_scale";
		actions.renames["ALPHA_ANTIALIASING_EDGE"] = "alpha_antialiasing_edge";
		actions.renames["ALPHA_TEXTURE_COORDINATE"] = "alpha_texture_coordinate";

		//builtins

		actions.renames["TIME"] = "global_time";
		actions.renames["EXPOSURE"] = "(1.0 / scene_data_block.data.emissive_exposure_normalization)";
		actions.renames["PI"] = String::num(Math::PI);
		actions.renames["TAU"] = String::num(Math::TAU);
		actions.renames["E"] = String::num(Math::E);
		actions.renames["OUTPUT_IS_SRGB"] = "SHADER_IS_SRGB";
		actions.renames["CLIP_SPACE_FAR"] = "SHADER_SPACE_FAR";
		actions.renames["IN_SHADOW_PASS"] = "bool(scene_data_block.data.flags & SCENE_DATA_FLAGS_IN_SHADOW_PASS)";
		actions.renames["VIEWPORT_SIZE"] = "read_viewport_size";

		actions.renames["FRAGCOORD"] = "gl_FragCoord";
		actions.renames["FRONT_FACING"] = "gl_FrontFacing";
		actions.renames["NORMAL_MAP"] = "normal_map";
		actions.renames["NORMAL_MAP_DEPTH"] = "normal_map_depth";
		actions.renames["BENT_NORMAL_MAP"] = "bent_normal_map";
		actions.renames["ALBEDO"] = "albedo_highp";
		actions.renames["ALPHA"] = "alpha_highp";
		actions.renames["PREMUL_ALPHA_FACTOR"] = "premul_alpha";
		actions.renames["METALLIC"] = "metallic_highp";
		actions.renames["SPECULAR"] = "specular";
		actions.renames["ROUGHNESS"] = "roughness_highp";
		actions.renames["RIM"] = "rim";
		actions.renames["RIM_TINT"] = "rim_tint";
		actions.renames["CLEARCOAT"] = "clearcoat";
		actions.renames["CLEARCOAT_ROUGHNESS"] = "clearcoat_roughness";
		actions.renames["ANISOTROPY"] = "anisotropy";
		actions.renames["ANISOTROPY_FLOW"] = "anisotropy_flow";
		actions.renames["SSS_STRENGTH"] = "sss_strength";
		actions.renames["SSS_TRANSMITTANCE_COLOR"] = "transmittance_color";
		actions.renames["SSS_TRANSMITTANCE_DEPTH"] = "transmittance_depth";
		actions.renames["SSS_TRANSMITTANCE_BOOST"] = "transmittance_boost";
		actions.renames["BACKLIGHT"] = "backlight";
		actions.renames["AO"] = "ao";
		actions.renames["AO_LIGHT_AFFECT"] = "ao_light_affect";
		actions.renames["EMISSION"] = "emission";
		actions.renames["POINT_COORD"] = "point_coord";
		actions.renames["INSTANCE_CUSTOM"] = "instance_custom";
		actions.renames["SCREEN_UV"] = "screen_uv";
		actions.renames["DEPTH"] = "gl_FragDepth";
		actions.renames["DEPTH_OFFSET"] = "depth_offset";
		actions.renames["FOG"] = "fog";
		actions.renames["RADIANCE"] = "custom_radiance";
		actions.renames["IRRADIANCE"] = "custom_irradiance";
		actions.renames["BONE_INDICES"] = "bone_attrib";
		actions.renames["BONE_WEIGHTS"] = "weight_attrib";
		actions.renames["CUSTOM0"] = "custom0_attrib";
		actions.renames["CUSTOM1"] = "custom1_attrib";
		actions.renames["CUSTOM2"] = "custom2_attrib";
		actions.renames["CUSTOM3"] = "custom3_attrib";
		actions.renames["LIGHT_VERTEX"] = "light_vertex";

		actions.renames["NODE_POSITION_WORLD"] = "read_model_matrix[3].xyz";
		actions.renames["CAMERA_POSITION_WORLD"] = "inv_view_matrix[3].xyz";
		actions.renames["CAMERA_DIRECTION_WORLD"] = "inv_view_matrix[2].xyz";
		actions.renames["CAMERA_VISIBLE_LAYERS"] = "scene_data.camera_visible_layers";
		actions.renames["NODE_POSITION_VIEW"] = "(read_view_matrix * read_model_matrix)[3].xyz";

		actions.renames["IS_MULTIVIEW"] = "OUTPUT_IS_MULTIVIEW";
		actions.renames["VIEW_INDEX"] = "ViewIndex";
		actions.renames["VIEW_MONO_LEFT"] = "0";
		actions.renames["VIEW_RIGHT"] = "1";
		actions.renames["EYE_OFFSET"] = "eye_offset";

		//for light
		actions.renames["VIEW"] = "view_highp";
		actions.renames["SPECULAR_AMOUNT"] = "specular_amount_highp";
		actions.renames["LIGHT_COLOR"] = "light_color_highp";
		actions.renames["LIGHT_IS_DIRECTIONAL"] = "is_directional";
		actions.renames["LIGHT_IS_AREA"] = "is_area";
		actions.renames["LIGHT_AREA_DIFFUSE_MULTIPLIER"] = "area_diffuse";
		actions.renames["LIGHT_AREA_SPECULAR_MULTIPLIER"] = "area_specular";
		actions.renames["LIGHT"] = "light_highp";
		actions.renames["ATTENUATION"] = "attenuation_highp";
		actions.renames["DIFFUSE_LIGHT"] = "diffuse_light_highp";
		actions.renames["SPECULAR_LIGHT"] = "specular_light_highp";

		actions.usage_defines["NORMAL"] = "#define NORMAL_USED\n";
		actions.usage_defines["TANGENT"] = "#define TANGENT_USED\n";
		actions.usage_defines["BINORMAL"] = "@TANGENT";
		actions.usage_defines["RIM"] = "#define LIGHT_RIM_USED\n";
		actions.usage_defines["RIM_TINT"] = "@RIM";
		actions.usage_defines["CLEARCOAT"] = "#define LIGHT_CLEARCOAT_USED\n";
		actions.usage_defines["CLEARCOAT_ROUGHNESS"] = "@CLEARCOAT";
		actions.usage_defines["ANISOTROPY"] = "#define LIGHT_ANISOTROPY_USED\n";
		actions.usage_defines["ANISOTROPY_FLOW"] = "@ANISOTROPY";
		actions.usage_defines["AO"] = "#define AO_USED\n";
		actions.usage_defines["AO_LIGHT_AFFECT"] = "#define AO_USED\n";
		actions.usage_defines["UV"] = "#define UV_USED\n";
		actions.usage_defines["UV2"] = "#define UV2_USED\n";
		actions.usage_defines["BONE_INDICES"] = "#define BONES_USED\n";
		actions.usage_defines["BONE_WEIGHTS"] = "#define WEIGHTS_USED\n";
		actions.usage_defines["CUSTOM0"] = "#define CUSTOM0_USED\n";
		actions.usage_defines["CUSTOM1"] = "#define CUSTOM1_USED\n";
		actions.usage_defines["CUSTOM2"] = "#define CUSTOM2_USED\n";
		actions.usage_defines["CUSTOM3"] = "#define CUSTOM3_USED\n";
		actions.usage_defines["NORMAL_MAP"] = "#define NORMAL_MAP_USED\n";
		actions.usage_defines["NORMAL_MAP_DEPTH"] = "@NORMAL_MAP";
		actions.usage_defines["BENT_NORMAL_MAP"] = "#define BENT_NORMAL_MAP_USED\n";
		actions.usage_defines["COLOR"] = "#define COLOR_USED\n";
		actions.usage_defines["INSTANCE_CUSTOM"] = "#define ENABLE_INSTANCE_CUSTOM\n";
		actions.usage_defines["POSITION"] = "#define OVERRIDE_POSITION\n";
		actions.usage_defines["LIGHT_VERTEX"] = "#define LIGHT_VERTEX_USED\n";
		actions.usage_defines["Z_CLIP_SCALE"] = "#define Z_CLIP_SCALE_USED\n";
		actions.usage_defines["LIGHT_AREA_DIFFUSE_MULTIPLIER"] = "#define AREA_LIGHT_CODE_USED\n";
		actions.usage_defines["LIGHT_AREA_SPECULAR_MULTIPLIER"] = "@LIGHT_AREA_DIFFUSE_MULTIPLIER";
		actions.usage_defines["LIGHT_IS_AREA"] = "@LIGHT_AREA_DIFFUSE_MULTIPLIER";

		actions.usage_defines["ALPHA_SCISSOR_THRESHOLD"] = "#define ALPHA_SCISSOR_USED\n";
		actions.usage_defines["ALPHA_HASH_SCALE"] = "#define ALPHA_HASH_USED\n";
		actions.usage_defines["ALPHA_ANTIALIASING_EDGE"] = "#define ALPHA_ANTIALIASING_EDGE_USED\n";
		actions.usage_defines["ALPHA_TEXTURE_COORDINATE"] = "@ALPHA_ANTIALIASING_EDGE";
		actions.usage_defines["PREMUL_ALPHA_FACTOR"] = "#define PREMUL_ALPHA_USED\n";

		actions.usage_defines["SSS_STRENGTH"] = "#define ENABLE_SSS\n";
		actions.usage_defines["SSS_TRANSMITTANCE_DEPTH"] = "#define ENABLE_TRANSMITTANCE\n";
		actions.usage_defines["BACKLIGHT"] = "#define LIGHT_BACKLIGHT_USED\n";
		actions.usage_defines["SCREEN_UV"] = "#define SCREEN_UV_USED\n";

		actions.usage_defines["FOG"] = "#define CUSTOM_FOG_USED\n";
		actions.usage_defines["RADIANCE"] = "#define CUSTOM_RADIANCE_USED\n";
		actions.usage_defines["IRRADIANCE"] = "#define CUSTOM_IRRADIANCE_USED\n";

		actions.usage_defines["MODEL_MATRIX"] = "#define MODEL_MATRIX_USED\n";

		actions.usage_defines["POINT_SIZE"] = "#define POINT_SIZE_USED\n";
		actions.usage_defines["POINT_COORD"] = "#define POINT_COORD_USED\n";

		actions.usage_defines["DISCARD"] = "#define DISCARD_USED\n";
		actions.usage_defines["DEPTH"] = "#define DEPTH_USED\n";
		actions.usage_defines["DEPTH_OFFSET"] = "#define DEPTH_OFFSET_USED\n";

		actions.usage_defines["STREAMING_LOD"] = "#define STREAMING_LOD_USED\n";

		actions.render_mode_defines["skip_vertex_transform"] = "#define SKIP_TRANSFORM_USED\n";
		actions.render_mode_defines["world_vertex_coords"] = "#define VERTEX_WORLD_COORDS_USED\n";
		actions.render_mode_defines["ensure_correct_normals"] = "#define ENSURE_CORRECT_NORMALS\n";
		actions.render_mode_defines["cull_front"] = "#define DO_SIDE_CHECK\n";
		actions.render_mode_defines["cull_disabled"] = "#define DO_SIDE_CHECK\n";
		actions.render_mode_defines["particle_trails"] = "#define USE_PARTICLE_TRAILS\n";
		actions.render_mode_defines["depth_prepass_alpha"] = "#define USE_OPAQUE_PREPASS\n";

		actions.render_mode_defines["depth_draw_never"] = "#define DEPTH_DRAW_NEVER_USED\n";
		actions.render_mode_defines["depth_draw_always"] = "#define DEPTH_DRAW_ALWAYS_USED\n";
		actions.render_mode_defines["depth_test_disabled"] = "#define DEPTH_TEST_DISABLED_USED\n";
		actions.stencil_mode_defines["write"] = "#define STENCIL_WRITE_USED\n";
		actions.stencil_mode_defines["write_depth_fail"] = "#define STENCIL_WRITE_USED\n";

		bool force_lambert = GLOBAL_GET("rendering/shading/overrides/force_lambert_over_burley");

		if (!force_lambert) {
			actions.render_mode_defines["diffuse_burley"] = "#define DIFFUSE_BURLEY\n";
		}

		actions.render_mode_defines["diffuse_lambert_wrap"] = "#define DIFFUSE_LAMBERT_WRAP\n";
		// GodotNext: megalights_inputs (#136). Names the lights-inc Lambert #else for fallback classification.
		actions.render_mode_defines["diffuse_lambert"] = "#define DIFFUSE_LAMBERT\n";
		actions.render_mode_defines["diffuse_toon"] = "#define DIFFUSE_TOON\n";

		actions.render_mode_defines["sss_mode_skin"] = "#define SSS_MODE_SKIN\n";

		actions.render_mode_defines["specular_schlick_ggx"] = "#define SPECULAR_SCHLICK_GGX\n";

		actions.render_mode_defines["specular_toon"] = "#define SPECULAR_TOON\n";
		actions.render_mode_defines["specular_disabled"] = "#define SPECULAR_DISABLED\n";
		actions.render_mode_defines["shadows_disabled"] = "#define SHADOWS_DISABLED\n";
		actions.render_mode_defines["ambient_light_disabled"] = "#define AMBIENT_LIGHT_DISABLED\n";
		actions.render_mode_defines["shadow_to_opacity"] = "#define USE_SHADOW_TO_OPACITY\n";
		actions.render_mode_defines["unshaded"] = "#define MODE_UNSHADED\n";

		bool force_vertex_shading = GLOBAL_GET("rendering/shading/overrides/force_vertex_shading");
		if (!force_vertex_shading) {
			// If forcing vertex shading, this will be defined already.
			actions.render_mode_defines["vertex_lighting"] = "#define USE_VERTEX_LIGHTING\n";
		}

		actions.render_mode_defines["debug_shadow_splits"] = "#define DEBUG_DRAW_PSSM_SPLITS\n";
		actions.render_mode_defines["fog_disabled"] = "#define FOG_DISABLED\n";

		actions.render_mode_defines["specular_occlusion_disabled"] = "#define SPECULAR_OCCLUSION_DISABLED\n";

		actions.base_texture_binding_index = 1;
		actions.texture_layout_set = RenderForwardClustered::MATERIAL_UNIFORM_SET;
		actions.base_uniform_string = "material.";
		actions.base_varying_index = 15;

		actions.default_filter = ShaderLanguage::FILTER_LINEAR_MIPMAP;
		actions.default_repeat = ShaderLanguage::REPEAT_ENABLE;
		actions.global_buffer_array_variable = "global_shader_uniforms.data";
		actions.instance_uniform_index_variable = "instances.data[instance_index_interp].instance_uniforms_ofs";

		actions.check_multiview_samplers = true;

		// GodotNext: nextgeo_resolve (#80). Analytic derivative bindings for MODE_VISIBILITY_RESOLVE.
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
		actions.resolve_uniform_builtins.insert("EXPOSURE");
		actions.resolve_uniform_builtins.insert("PI");
		actions.resolve_uniform_builtins.insert("TAU");
		actions.resolve_uniform_builtins.insert("E");
		actions.resolve_uniform_builtins.insert("CLIP_SPACE_FAR");
		actions.resolve_uniform_builtins.insert("VIEWPORT_SIZE");
		actions.resolve_uniform_builtins.insert("NODE_POSITION_WORLD");
		actions.resolve_uniform_builtins.insert("CAMERA_POSITION_WORLD");
		actions.resolve_uniform_builtins.insert("CAMERA_DIRECTION_WORLD");
		actions.resolve_uniform_builtins.insert("NODE_POSITION_VIEW");
		actions.resolve_uniform_builtins.insert("EYE_OFFSET");
		actions.resolve_uniform_builtins.insert("MODEL_MATRIX");
		actions.resolve_uniform_builtins.insert("MODEL_NORMAL_MATRIX");
		actions.resolve_uniform_builtins.insert("VIEW_MATRIX");
		actions.resolve_uniform_builtins.insert("INV_VIEW_MATRIX");
		actions.resolve_uniform_builtins.insert("PROJECTION_MATRIX");
		actions.resolve_uniform_builtins.insert("INV_PROJECTION_MATRIX");
		actions.resolve_uniform_builtins.insert("MAIN_CAM_INV_VIEW_MATRIX");

		actions.resolve_renames["FRONT_FACING"] = "_ng_front_facing";
		actions.resolve_renames["FRAGCOORD"] = "_ng_fragcoord";
		actions.resolve_vertex_renames["VERTEX_ID"] = "_ng_vertex_id";
		actions.resolve_vertex_renames["INSTANCE_ID"] = "_ng_instance_id";

		// GodotNext: nextgeo_displacement (#150, plan §21.4 N11). Built-in bindings and edge-length
		// derivatives for displacement() in resolve, raster target, and #163 dicing compute.
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

		compiler.initialize(actions);
	}

	// GodotNext: nextgeo_raster_bins (#155, plan §21.4 N4). Initialized unconditionally so the shader baker
	// can bake per-bin software and hardware VIS64 variants regardless of the editor host's device tier.
	// Group 0 (image) and group 1 (buffer) stay disabled until first runtime use on a supporting device.
	// Before the default material, so every material classifies against the same state.
	{
		String raster_defines;
#ifdef REAL_T_IS_DOUBLE
		raster_defines += "\n#define USE_DOUBLE_PRECISION\n";
#endif
		Vector<ShaderRD::VariantDefine> raster_variants;
		raster_variants.push_back(ShaderRD::VariantDefine(0, "", false));
		raster_variants.push_back(ShaderRD::VariantDefine(1, "\n#define VIS64_BUFFER\n", false));
		// GodotNext: vsm_nanite (#217). Group 2: depth-only VSM page raster (32-bit imageAtomicMax, no 64-bit atomics).
		raster_variants.push_back(ShaderRD::VariantDefine(2, "\n#define NEXTGEO_RASTER_DEPTH_ONLY\n" + RendererVirtualShadowMapStorage::get_glsl_defines(), false));
		raster_sw_shader.initialize(raster_variants, raster_defines);
		raster_sw_shader.set_group_int64_atomics(0, true, false);
		raster_sw_shader.set_group_int64_atomics(1, false, true);
		raster_hw_shader.initialize(raster_variants, raster_defines);
		raster_hw_shader.set_group_int64_atomics(0, true, false);
		raster_hw_shader.set_group_int64_atomics(1, false, true);
		// GodotNext: nextgeo_mesh_raster (#154). Mesh-shader form of the HW material raster target.
		raster_hw_mesh_shader.initialize(raster_variants, raster_defines);
		raster_hw_mesh_shader.set_group_int64_atomics(0, true, false);
		raster_hw_mesh_shader.set_group_int64_atomics(1, false, true);
		// GodotNext: nextgeo_vertex_offset (#164). Opt-in RT proxy vertex-offset compute shader.
		Vector<ShaderRD::VariantDefine> rt_wpo_variants;
		rt_wpo_variants.push_back(ShaderRD::VariantDefine(0, "", false));
		rt_vertex_offset_shader.initialize(rt_wpo_variants, raster_defines);
		// GodotNext: nextgeo_tessellation (#163). Dicing compute + bounded HW stream shaders (groups stay disabled until active).
		tessellation_dice_shader.initialize(raster_variants, raster_defines);
		tessellation_dice_shader.set_group_int64_atomics(0, true, false);
		tessellation_dice_shader.set_group_int64_atomics(1, false, true);
		tessellation_hw_shader.initialize(raster_variants, raster_defines);
		tessellation_hw_shader.set_group_int64_atomics(0, true, false);
		tessellation_hw_shader.set_group_int64_atomics(1, false, true);

		const RendererRD::NextGeoRasterTier tier = RendererRD::nextgeo_raster_tier(RD::get_singleton());
		raster_target_ready = tier.hybrid;
		raster_image_int64_supported = tier.hybrid && RD::get_singleton()->has_feature(RD::SUPPORTS_IMAGE_INT64_ATOMICS);
		raster_buffer_int64_supported = tier.hybrid && RD::get_singleton()->has_feature(RD::SUPPORTS_BUFFER_INT64_ATOMICS);
		raster_vsm_supported = RD::get_singleton()->has_feature(RD::SUPPORTS_IMAGE_ATOMIC_32_BIT);
		const RendererRD::NextGeoHWRasterCapabilities mesh_caps = RendererRD::nextgeo_hw_raster_capabilities_from_device(RD::get_singleton());
		raster_mesh_supported = RendererRD::nextgeo_select_hw_raster_path(RendererRD::NEXTGEO_HW_RASTER_PATH_MESH, mesh_caps).use_mesh;
	}

	{
		//default material and shader
		default_shader = material_storage->shader_allocate();
		material_storage->shader_initialize(default_shader);
		material_storage->shader_set_code(default_shader, R"(
// Default 3D material shader (Forward+).

shader_type spatial;

void vertex() {
	ROUGHNESS = 0.8;
}

void fragment() {
	ALBEDO = vec3(0.6);
	ROUGHNESS = 0.8;
	METALLIC = 0.2;
}
)");
		default_material = material_storage->material_allocate();
		material_storage->material_initialize(default_material);
		material_storage->material_set_shader(default_material, default_shader);

		MaterialData *md = static_cast<MaterialData *>(material_storage->material_get_data(default_material, RendererRD::MaterialStorage::SHADER_TYPE_3D));
		default_shader_rd = md->shader_data->get_shader_variant(PIPELINE_VERSION_COLOR_PASS, 0, false);

		default_material_shader_ptr = md->shader_data;
		default_material_uniform_set = md->uniform_set;
	}

	{
		overdraw_material_shader = material_storage->shader_allocate();
		material_storage->shader_initialize(overdraw_material_shader);
		// Use relatively low opacity so that more "layers" of overlapping objects can be distinguished.
		material_storage->shader_set_code(overdraw_material_shader, R"(
// 3D editor Overdraw debug draw mode shader (Forward+).

shader_type spatial;

render_mode blend_add, unshaded, fog_disabled;

void fragment() {
	ALBEDO = vec3(0.4, 0.8, 0.8);
	ALPHA = 0.1;
}
)");
		overdraw_material = material_storage->material_allocate();
		material_storage->material_initialize(overdraw_material);
		material_storage->material_set_shader(overdraw_material, overdraw_material_shader);

		MaterialData *md = static_cast<MaterialData *>(material_storage->material_get_data(overdraw_material, RendererRD::MaterialStorage::SHADER_TYPE_3D));
		overdraw_material_shader_ptr = md->shader_data;
		overdraw_material_uniform_set = md->uniform_set;
	}

	{
		debug_shadow_splits_material_shader = material_storage->shader_allocate();
		material_storage->shader_initialize(debug_shadow_splits_material_shader);
		material_storage->shader_set_code(debug_shadow_splits_material_shader, R"(
// 3D debug shadow splits mode shader (Forward+).

shader_type spatial;

render_mode debug_shadow_splits, fog_disabled;

void fragment() {
	ALBEDO = vec3(1.0, 1.0, 1.0);
}
)");
		debug_shadow_splits_material = material_storage->material_allocate();
		material_storage->material_initialize(debug_shadow_splits_material);
		material_storage->material_set_shader(debug_shadow_splits_material, debug_shadow_splits_material_shader);

		MaterialData *md = static_cast<MaterialData *>(material_storage->material_get_data(debug_shadow_splits_material, RendererRD::MaterialStorage::SHADER_TYPE_3D));
		debug_shadow_splits_material_shader_ptr = md->shader_data;
		debug_shadow_splits_material_uniform_set = md->uniform_set;
	}

	{
		default_vec4_xform_buffer = RD::get_singleton()->storage_buffer_create(256);
		default_material_feedback_buffer = RD::get_singleton()->storage_buffer_create(256);
		Vector<RD::Uniform> uniforms;
		RD::Uniform u;
		u.uniform_type = RD::UNIFORM_TYPE_STORAGE_BUFFER;
		u.append_id(default_vec4_xform_buffer);
		u.binding = 0;
		uniforms.push_back(u);

		default_vec4_xform_uniform_set = RD::get_singleton()->uniform_set_create(uniforms, default_shader_rd, RenderForwardClustered::TRANSFORMS_UNIFORM_SET);
	}
	{
		RD::SamplerState sampler;
		sampler.mag_filter = RD::SAMPLER_FILTER_LINEAR;
		sampler.min_filter = RD::SAMPLER_FILTER_LINEAR;
		sampler.enable_compare = true;
		sampler.compare_op = RD::COMPARE_OP_GREATER;
		shadow_sampler = RD::get_singleton()->sampler_create(sampler);
	}
}

void SceneShaderForwardClustered::set_default_specialization(const ShaderSpecialization &p_specialization) {
	default_specialization = p_specialization;

	for (ShaderData &shader_data : shader_list) {
		shader_data.pipeline_hash_map.clear_pipelines();
	}
}

void SceneShaderForwardClustered::enable_multiview_shader_group() {
	shader.enable_group(SHADER_GROUP_MULTIVIEW);
}

void SceneShaderForwardClustered::enable_advanced_shader_group(bool p_needs_multiview) {
	if (p_needs_multiview || RendererCompositorRD::get_singleton()->is_xr_enabled()) {
		shader.enable_group(SHADER_GROUP_ADVANCED_MULTIVIEW);
	}
	shader.enable_group(SHADER_GROUP_ADVANCED);
}

void SceneShaderForwardClustered::enable_nextgeo_shader_group() {
	shader.enable_group(SHADER_GROUP_NEXTGEO);
}

bool SceneShaderForwardClustered::is_multiview_shader_group_enabled() const {
	return shader.is_group_enabled(SHADER_GROUP_MULTIVIEW);
}

bool SceneShaderForwardClustered::is_advanced_shader_group_enabled(bool p_multiview) const {
	if (p_multiview) {
		return shader.is_group_enabled(SHADER_GROUP_ADVANCED_MULTIVIEW);
	} else {
		return shader.is_group_enabled(SHADER_GROUP_ADVANCED);
	}
}

bool SceneShaderForwardClustered::is_nextgeo_shader_group_enabled() const {
	return shader.is_group_enabled(SHADER_GROUP_NEXTGEO);
}

// GodotNext: megalights_inputs (#136)
void SceneShaderForwardClustered::enable_megalights_shader_group(bool p_needs_multiview) {
	if (p_needs_multiview || RendererCompositorRD::get_singleton()->is_xr_enabled()) {
		shader.enable_group(SHADER_GROUP_MEGALIGHTS_MULTIVIEW);
	}
	shader.enable_group(SHADER_GROUP_MEGALIGHTS);
}

void SceneShaderForwardClustered::enable_nextgeo_megalights_shader_group() {
	shader.enable_group(SHADER_GROUP_NEXTGEO_MEGALIGHTS);
}

// GodotNext: rr_guides (#205)
void SceneShaderForwardClustered::enable_rr_guides_shader_group() {
	shader.enable_group(SHADER_GROUP_RR_GUIDES);
}

void SceneShaderForwardClustered::enable_nextgeo_rr_guides_shader_group() {
	shader.enable_group(SHADER_GROUP_NEXTGEO_RR_GUIDES);
}

// GodotNext: vsm_non_nanite (#218)
void SceneShaderForwardClustered::enable_vsm_shader_group() {
	shader.enable_group(SHADER_GROUP_VSM);
}

bool SceneShaderForwardClustered::is_vsm_shader_group_enabled() const {
	return shader.is_group_enabled(SHADER_GROUP_VSM);
}

// GodotNext: avboit (#295)
void SceneShaderForwardClustered::enable_avboit_shader_group() {
	shader.enable_group(SHADER_GROUP_AVBOIT);
}

// GodotNext: nextgeo_vertex_offset (#164)
void SceneShaderForwardClustered::enable_nextgeo_motion_shader_group(bool p_megalights_inputs) {
	shader.enable_group(p_megalights_inputs ? SHADER_GROUP_NEXTGEO_MEGALIGHTS_MOTION : SHADER_GROUP_NEXTGEO_MOTION);
}

const char *SceneShaderForwardClustered::ShaderVersion::megalights_variant_defines(uint16_t p_relative) {
	static const char *defines[SHADER_VERSION_MEGALIGHTS_INPUTS_COUNT + SHADER_VERSION_MEGALIGHTS_INPUTS_UBER_COUNT] = {
		"\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_MEGALIGHTS_INPUTS\n",
		"\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_RENDER_VOXEL_GI\n#define MODE_MEGALIGHTS_INPUTS\n",
		"\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MOTION_VECTORS\n#define MODE_MEGALIGHTS_INPUTS\n",
		"\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_RENDER_VOXEL_GI\n#define MOTION_VECTORS\n#define MODE_MEGALIGHTS_INPUTS\n",
		"\n#define USE_MULTIVIEW\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_MEGALIGHTS_INPUTS\n",
		"\n#define USE_MULTIVIEW\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_RENDER_VOXEL_GI\n#define MODE_MEGALIGHTS_INPUTS\n",
		"\n#define USE_MULTIVIEW\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MOTION_VECTORS\n#define MODE_MEGALIGHTS_INPUTS\n",
		"\n#define USE_MULTIVIEW\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_RENDER_VOXEL_GI\n#define MOTION_VECTORS\n#define MODE_MEGALIGHTS_INPUTS\n",
		"\n#define MODE_VISIBILITY_RESOLVE\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_MEGALIGHTS_INPUTS\n",
		"\n#define MODE_VISIBILITY_RESOLVE\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_RENDER_VOXEL_GI\n#define MODE_MEGALIGHTS_INPUTS\n",
		"\n#define UBERSHADER\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_MEGALIGHTS_INPUTS\n",
		"\n#define UBERSHADER\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_RENDER_VOXEL_GI\n#define MODE_MEGALIGHTS_INPUTS\n",
		"\n#define UBERSHADER\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MOTION_VECTORS\n#define MODE_MEGALIGHTS_INPUTS\n",
		"\n#define UBERSHADER\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_RENDER_VOXEL_GI\n#define MOTION_VECTORS\n#define MODE_MEGALIGHTS_INPUTS\n",
		"\n#define UBERSHADER\n#define USE_MULTIVIEW\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_MEGALIGHTS_INPUTS\n",
		"\n#define UBERSHADER\n#define USE_MULTIVIEW\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_RENDER_VOXEL_GI\n#define MODE_MEGALIGHTS_INPUTS\n",
		"\n#define UBERSHADER\n#define USE_MULTIVIEW\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MOTION_VECTORS\n#define MODE_MEGALIGHTS_INPUTS\n",
		"\n#define UBERSHADER\n#define USE_MULTIVIEW\n#define MODE_RENDER_DEPTH\n#define MODE_RENDER_NORMAL_ROUGHNESS\n#define MODE_RENDER_VOXEL_GI\n#define MOTION_VECTORS\n#define MODE_MEGALIGHTS_INPUTS\n",
	};
	if (p_relative >= SHADER_VERSION_MEGALIGHTS_INPUTS_COUNT + SHADER_VERSION_MEGALIGHTS_INPUTS_UBER_COUNT) {
		return "";
	}
	return defines[p_relative];
}

uint32_t SceneShaderForwardClustered::get_pipeline_compilations(RSE::PipelineSource p_source) {
	MutexLock lock(SceneShaderForwardClustered::singleton_mutex);
	return pipeline_compilations[p_source];
}

RID SceneShaderForwardClustered::get_default_shader_rd(bool p_is_multiview) {
	RID &shader_rd = p_is_multiview ? default_multiview_shader_rd : default_shader_rd;

	if (shader_rd.is_null()) {
		RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
		ERR_FAIL_NULL_V(material_storage, RID());
		ERR_FAIL_COND_V(!default_material.is_valid(), RID());

		uint32_t flags = 0;
		if (p_is_multiview) {
			flags |= SHADER_COLOR_PASS_FLAG_MULTIVIEW;
		}

		MaterialData *md = static_cast<MaterialData *>(material_storage->material_get_data(default_material, RendererRD::MaterialStorage::SHADER_TYPE_3D));
		shader_rd = md->shader_data->get_shader_variant(PIPELINE_VERSION_COLOR_PASS, flags, false);
	}

	return shader_rd;
}
