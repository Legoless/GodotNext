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
// GodotNext: path_tracer (#167), path_tracer_denoise (#168), path_tracer_lights_camera (#169), rt_shadows (#141), megalights_visibility (#146), megalights_denoiser (#135), megalights_fog_translucency (#137)
#include "servers/rendering/renderer_rd/effects/denoise.h"
#include "servers/rendering/renderer_rd/effects/mega_lights.h"
#include "servers/rendering/renderer_rd/effects/path_tracer.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_denoise_spatial.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_denoise_temporal.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_front_layer.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_froxel.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_froxel_compute.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_screen_trace.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_shade_samples.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_trace_rays.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_trace_rays_swrt.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_trace_vsm.glsl.gen.h"
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
#include "servers/rendering/storage/virtual_shadow_map_storage.h"

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
	// GodotNext: megalights_visibility (#146). MegaLights screen trace, ray query, SWRT continuation, and VSM trace.
	MlScreenTraceShaderRD ml_screen_trace;
	MlTraceRaysShaderRD ml_trace_rays;
	MlTraceRaysSwrtShaderRD ml_trace_rays_swrt;
	MlTraceVsmShaderRD ml_trace_vsm;
	// GodotNext: megalights_denoiser (#135). MegaLights demodulated shade, temporal, and spatial denoiser (plain compute).
	MlShadeSamplesShaderRD ml_shade_samples;
	MlDenoiseTemporalShaderRD ml_denoise_temporal;
	MlDenoiseSpatialShaderRD ml_denoise_spatial;
	// GodotNext: megalights_fog_translucency (#137). Shared froxel & D17 volume (HWRT + compute sibling) and front-layer guide prep.
	MlFroxelShaderRD ml_froxel;
	MlFroxelComputeShaderRD ml_froxel_compute;
	MlFrontLayerShaderRD ml_front_layer;
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
	// GodotNext: megalights_visibility (#146).
	_add(instances->ml_screen_trace, MegaLights::get_screen_trace_shader_variant_defines());
	_add(instances->ml_trace_rays, MegaLights::get_trace_rays_shader_variant_defines());
	_add(instances->ml_trace_rays_swrt, MegaLights::get_trace_rays_swrt_shader_variant_defines());
	instances->ml_trace_vsm.initialize(MegaLights::get_trace_vsm_shader_variant_defines(), RendererVirtualShadowMapStorage::get_glsl_defines());
	_add_initialized(instances->ml_trace_vsm);
	// GodotNext: megalights_denoiser (#135).
	_add(instances->ml_shade_samples, MegaLights::get_shade_samples_shader_variant_defines());
	_add(instances->ml_denoise_temporal, NextGIDenoise::get_megalights_temporal_shader_variant_defines());
	_add(instances->ml_denoise_spatial, NextGIDenoise::get_megalights_spatial_shader_variant_defines());
	// GodotNext: megalights_fog_translucency (#137).
	instances->ml_froxel.initialize(MegaLights::get_froxel_shader_variant_defines(), RendererVirtualShadowMapStorage::get_glsl_defines());
	_add_initialized(instances->ml_froxel);
	instances->ml_froxel_compute.initialize(MegaLights::get_froxel_compute_shader_variant_defines(), RendererVirtualShadowMapStorage::get_glsl_defines());
	_add_initialized(instances->ml_froxel_compute);
	_add(instances->ml_front_layer, MegaLights::get_front_layer_shader_variant_defines());
}

NextGIBakeShaders::~NextGIBakeShaders() {
	for (const Pair<ShaderRD *, RID> &version : versions) {
		version.first->version_free(version.second);
	}
	versions.clear();
	memdelete(instances);
}
