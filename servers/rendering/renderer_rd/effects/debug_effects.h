/**************************************************************************/
/*  debug_effects.h                                                       */
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

#pragma once

#include "servers/rendering/renderer_rd/pipeline_cache_rd.h"
#include "servers/rendering/renderer_rd/shaders/effects/motion_vectors.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/nextgeo_rt_proxy.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/shadow_frustum.glsl.gen.h"
// GodotNext: vsm_settings_debug (#221).
#include "servers/rendering/renderer_rd/shaders/effects/vsm_debug.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/vsm_debug_pages.glsl.gen.h"

namespace RendererRD {

class DebugEffects {
private:
	struct {
		RD::VertexFormatID vertex_format;
		RID vertex_buffer;
		RID vertex_array;

		RID index_buffer;
		RID index_array;

		RID lines_buffer;
		RID lines_array;
	} frustum;

	struct ShadowFrustumPushConstant {
		float mvp[16];
		float color[4];
	};

	enum ShadowFrustumPipelines {
		SFP_TRANSPARENT,
		SFP_WIREFRAME,
		SFP_MAX
	};

	struct {
		ShadowFrustumShaderRD shader;
		RID shader_version;
		PipelineCacheRD pipelines[SFP_MAX];
	} shadow_frustum;

	struct MotionVectorsPushConstant {
		float reprojection_matrix[16];
		float resolution[2];
		uint32_t force_derive_from_depth;
		uint32_t pad;
	};

	struct {
		MotionVectorsShaderRD shader;
		RID shader_version;
		PipelineCacheRD pipeline;
		MotionVectorsPushConstant push_constant;
	} motion_vectors;

	struct NextGeoRTProxyPushConstant {
		float internal_size[2];
		float output_size[2];
		float z_near;
		float z_far;
		uint32_t is_orthogonal;
		uint32_t pad0;
	};

	struct {
		NextgeoRtProxyShaderRD shader;
		RID shader_version;
		PipelineCacheRD pipeline;
	} nextgeo_rt_proxy;

	// GodotNext: vsm_settings_debug (#221). Created on the first virtual shadow map debug draw.
	enum VSMDebugPagesMode {
		VSM_DEBUG_PAGES_CLEAR,
		VSM_DEBUG_PAGES_DIRTY,
		VSM_DEBUG_PAGES_PAIRS,
		VSM_DEBUG_PAGES_MAX,
	};
	struct VSMDebugPagesPushConstant {
		uint32_t pool_pages;
		uint32_t grid_width;
		uint32_t pair_capacity;
		uint32_t pair_stride; // MODE_PAIRS: the dispatched threads; each thread steps by this.
	};
	struct {
		VsmDebugShaderRD shader;
		RID shader_version;
		PipelineCacheRD pipeline;
		VsmDebugPagesShaderRD pages_shader;
		RID pages_version;
		RID pages_pipelines[VSM_DEBUG_PAGES_MAX];
		RID params_buffer;
		uint32_t params_lights = 0; // Lights the params buffer holds.
		RID page_info_buffer;
		uint32_t page_info_pages = 0;
		RID empty_buffer;
		bool ready = false;
		bool failed = false;
	} vsm_debug;
	bool _ensure_vsm_debug();

	void _create_frustum_arrays();

protected:
public:
	DebugEffects();
	~DebugEffects();

	void draw_shadow_frustum(RID p_light, const Projection &p_cam_projection, const Transform3D &p_cam_transform, RID p_dest_fb, const Rect2 p_rect);
	void draw_motion_vectors(RID p_velocity, RID p_depth, RID p_dest_fb, const Projection &p_current_projection, const Transform3D &p_current_transform, const Projection &p_previous_projection, const Transform3D &p_previous_transform, Size2i p_resolution);
	void draw_nextgeo_rt_proxy(RID p_raster_depth, RID p_proxy_depth, RID p_dest_fb, const Projection &p_projection, Size2i p_internal_size, Size2i p_output_size);

	// GodotNext: vsm_settings_debug (#221). One light of a VSM debug draw (DebugLight in vsm_debug.glsl).
	struct VSMDebugLight {
		uint32_t map_base = 0;
		uint32_t light_flags = 0;
		uint32_t kind = 0; // RendererVirtualShadowMapStorage::MapKind.
		float inv_reach = 0.0f; // Omni and spot: 1 / range; area: 1 / (range + half the diagonal).
		float position[3] = {};
		float cos_angle = 0.0f;
		float direction[3] = {};
		float pad = 0.0f;
	};
	static_assert(sizeof(VSMDebugLight) == 48, "VSMDebugLight matches DebugLight in vsm_debug.glsl.");
	static constexpr uint32_t VSM_DEBUG_MAX_LIGHTS = 256;
	struct VSMDebugInput {
		uint32_t mode = 0; // 0 mask, 1 mip, 2 pages, 3 cache, 4 overdraw, 5 clipmap (vsm_debug.glsl).
		RID depth_texture; // Resolved prepass depth (reversed Z) at the internal size.
		Size2i internal_size;
		Size2i output_size;
		RID dest_fb;
		Projection inv_projection; // Depth-texture clip space to view space.
		Transform3D cam_transform;
		float pixel_scale = 0.0f;
		float pixel_bias = 0.0f;
		int32_t lod_bias_directional = 0;
		int32_t lod_bias_local = 0;
		RID page_table;
		uint32_t table_maps = 0;
		RID map_data;
		RID pool;
		uint32_t pool_pages = 0;
		uint32_t grid_width = 0;
		// VSM_CACHE and VSM_OVERDRAW: the view's allocation state and dirty list, and its non-Nanite pairs.
		RID alloc_state;
		RID dirty;
		RID pairs;
		uint32_t pair_capacity = 0;
		RID cull_state;
		uint32_t max_dispatch_groups = 65535; // LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X.
		LocalVector<VSMDebugLight> lights;
	};
	void draw_vsm(const VSMDebugInput &p_input);
};

} // namespace RendererRD
