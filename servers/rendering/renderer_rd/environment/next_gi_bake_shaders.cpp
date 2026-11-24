/**************************************************************************/
/*  next_gi_bake_shaders.cpp                                              */
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

#include "next_gi_bake_shaders.h"

#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_gather.h"
#include "servers/rendering/renderer_rd/environment/next_gi_reference.h"
#include "servers/rendering/renderer_rd/environment/next_gi_relight.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_shadows.h"
#include "servers/rendering/renderer_rd/environment/next_gi_wrc.h"
// GodotNext: path_tracer (#167), path_tracer_denoise (#168), path_tracer_lights_camera (#169), rt_shadows (#141)
#include "servers/rendering/renderer_rd/effects/denoise.h"
#include "servers/rendering/renderer_rd/effects/path_tracer.h"
#include "servers/rendering/renderer_rd/shaders/effects/path_tracer_denoise.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/path_tracer_sky_cdf.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_denoise_shadow.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_gather.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_gather_swrt.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_reference.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_rt_shadow_alloc.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_rt_shadows.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_selftest.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_src_relight.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_src_relight_swrt.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_wrc_update.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_wrc_update_swrt.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/path_tracer.glsl.gen.h"

using namespace RendererRD;

struct NextGIBakeShaders::Instances {
	NextgiGatherShaderRD gather;
	NextgiSrcRelightShaderRD relight_packed;
	NextgiSrcRelightShaderRD relight_rgba16f;
	NextgiWrcUpdateShaderRD wrc_update;
	NextgiReferenceShaderRD reference;
	// GodotNext: nextgi_bake_swrt (#261). The software tier's siblings (no ray query, so every
	// container bakes them) and the startup self-test.
	NextgiGatherSwrtShaderRD gather_swrt;
	NextgiSrcRelightSwrtShaderRD relight_swrt_packed;
	NextgiSrcRelightSwrtShaderRD relight_swrt_rgba16f;
	NextgiWrcUpdateSwrtShaderRD wrc_update_swrt;
	NextgiSelftestShaderRD selftest;
	// GodotNext: path_tracer (#167). The product path tracer's inline backend (ray query).
	PathTracerShaderRD path_tracer;
	// GodotNext: path_tracer_denoise (#168). The screen-space JNLM denoiser (plain compute, no ray query).
	PathTracerDenoiseShaderRD path_tracer_denoise;
	// GodotNext: rt_shadows (#141). Standalone RT shadow allocation, ray-query trace, and per-light denoiser.
	NextgiRtShadowAllocShaderRD rt_shadow_alloc;
	NextgiRtShadowsShaderRD rt_shadows;
	NextgiDenoiseShadowShaderRD denoise_shadow;
	// GodotNext: path_tracer_lights_camera (#169). Sky 2D CDF importance sampling builder (plain compute, no ray query).
	PathTracerSkyCdfShaderRD path_tracer_sky_cdf;
};

void NextGIBakeShaders::_add(ShaderRD &p_shader, const Vector<String> &p_variant_defines) {
	p_shader.initialize(p_variant_defines);
	_add_initialized(p_shader);
}

void NextGIBakeShaders::_add_initialized(ShaderRD &p_shader) {
	const RID version = p_shader.version_create(false);
	ERR_FAIL_COND(!version.is_valid());
	versions.push_back({ &p_shader, version });
}

NextGIBakeShaders::NextGIBakeShaders() {
	instances = memnew(Instances);
	_add(instances->gather, NextGIGather::get_shader_variant_defines());
	_add(instances->relight_packed, NextGIRelight::get_shader_variant_defines(true));
	_add(instances->relight_rgba16f, NextGIRelight::get_shader_variant_defines(false));
	_add(instances->wrc_update, NextGIWRC::get_shader_variant_defines());
	_add(instances->reference, NextGIReference::get_shader_variant_defines());
	NextGIGather::initialize_swrt_shader(instances->gather_swrt);
	_add_initialized(instances->gather_swrt);
	_add(instances->relight_swrt_packed, NextGIRelight::get_shader_variant_defines(true));
	_add(instances->relight_swrt_rgba16f, NextGIRelight::get_shader_variant_defines(false));
	NextGIWRC::initialize_swrt_shader(instances->wrc_update_swrt);
	_add_initialized(instances->wrc_update_swrt);
	_add(instances->selftest, NextGI::get_selftest_variant_defines());
	_add(instances->path_tracer, PathTracer::get_shader_variant_defines()); // GodotNext: path_tracer (#167)
	_add(instances->path_tracer_denoise, PathTracer::get_denoise_shader_variant_defines()); // GodotNext: path_tracer_denoise (#168)
	_add(instances->rt_shadow_alloc, NextGIRTShadows::get_alloc_shader_variant_defines()); // GodotNext: rt_shadows (#141)
	_add(instances->rt_shadows, NextGIRTShadows::get_trace_shader_variant_defines()); // GodotNext: rt_shadows (#141)
	_add(instances->denoise_shadow, NextGIDenoise::get_shadow_shader_variant_defines()); // GodotNext: rt_shadows (#141)
	_add(instances->path_tracer_sky_cdf, PathTracer::get_sky_cdf_shader_variant_defines()); // GodotNext: path_tracer_lights_camera (#169)
}

NextGIBakeShaders::~NextGIBakeShaders() {
	for (const Pair<ShaderRD *, RID> &version : versions) {
		version.first->version_free(version.second);
	}
	versions.clear();
	memdelete(instances);
}
