/**************************************************************************/
/*  nextgeo_debug.h                                                       */
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

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/renderer_rd/pipeline_cache_rd.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/nextgeo_debug.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/nextgeo_q14_reduce.glsl.gen.h"
#include "servers/rendering/rendering_server_enums.h"

namespace RendererRD {

struct NextGeoDebugSceneParams {
	Projection proj;
	Transform3D view;
	Projection view_proj;
	Vector3 cam_pos;
	Size2i internal_size = Size2i(1, 1);
	float max_pixel_error = 1.0f;
	float lod_bias = 1.0f;
	float cull_sign = 1.0f;
	bool using_nextgeo = false;
	RID visible_clusters_buf;
	uint32_t visible_capacity = 0;
	RID bucket_table_buf;
	uint32_t bucket_table_words = 0;
};

// GodotNext: nextgeo_exclusive_dispatch (#79, plan §9, §12, §13.4 Q5 & Q14).
// Manages the R8_UINT fallback classification target, RT-proxy scratch depth,
// Q14 fallback-coverage compute reduction, 16-bin triangle-size histogram,
// and fullscreen NextGeo debug views.
class NextGeoDebug {
public:
	RID prepare_fallback_fb(const Size2i &p_internal_size, RID p_depth_tex);
	RID get_fallback_texture() const { return fallback_class_tex; }
	void clear_fallback_texture();

	RID prepare_rt_proxy_depth_fb(const Size2i &p_internal_size);
	RID get_rt_proxy_depth() const { return rt_proxy_depth_tex; }

	void record_scene_params(const NextGeoDebugSceneParams &p_params);
	void dispatch_q14_reduce(RID p_visibility_tex);
	bool draw_debug_view(RSE::ViewportDebugDraw p_mode, RID p_visibility_tex, RID p_depth_tex, RID p_dest_fb, const Size2i &p_internal_size, const Size2i &p_output_size);

	void free_resources();

	static void fill_debug_stats(Dictionary &r_stats);
	static void reset_debug_stats();

private:
	bool _ensure();
	void _release_fallback_texture();
	void _release_rt_proxy_depth();
	static void _readback_q14_static(const PackedByteArray &p_data, uint64_t p_token, uint32_t p_ring_slot);
	static void _readback_histogram_static(const PackedByteArray &p_data, uint64_t p_token, uint32_t p_ring_slot);
	void _publish_q14(const PackedByteArray &p_data, uint32_t p_ring_slot);
	void _publish_histogram(const PackedByteArray &p_data, uint32_t p_ring_slot);

	NextgeoDebugShaderRD debug_shader;
	RID debug_version;
	PipelineCacheRD debug_pipeline;

	NextgeoQ14ReduceShaderRD q14_shader;
	RID q14_version;
	RID q14_pipeline;

	RID fallback_class_tex;
	Size2i fallback_size;

	RID rt_proxy_depth_tex;
	Size2i rt_proxy_size;

	RID debug_params_buffer;
	RID dummy_buffer;
	RID dummy_vis_tex;
	RID dummy_fallback_tex;
	RID nearest_sampler;

	RID q14_scratch;
	RID q14_ring[3];
	uint32_t q14_ring_cursor = 0;
	bool q14_ring_pending[3] = { false, false, false };

	RID histogram_scratch;
	RID histogram_ring[3];
	uint32_t histogram_ring_cursor = 0;
	bool histogram_ring_pending[3] = { false, false, false };

	LocalVector<uint64_t> pending_tokens;
	NextGeoDebugSceneParams last_scene_params;
	bool has_scene_params = false;
	bool initialized = false;
	bool failed = false;
};

} // namespace RendererRD
