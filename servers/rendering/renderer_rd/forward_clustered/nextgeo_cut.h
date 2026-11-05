/**************************************************************************/
/*  nextgeo_cut.h                                                         */
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
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,         */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/math/projection.h"
#include "core/templates/rid.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/cluster_cull.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/hiz_pyramid.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/instance_cull.glsl.gen.h"

class RendererVirtualMeshStorage;

// GodotNext: nextgeo_gpu_cull (#76)
// The GPU cut manager: instance cull (level 0), one BVH8 dispatch per level,
// then the leaf-group test and compaction into (record, cluster).
class NextGeoCut {
public:
	static constexpr uint32_t NODE_CAPACITY = 1u << 17;
	static constexpr uint32_t GROUP_CAPACITY = 1u << 17;
	static constexpr uint32_t CLUSTER_CAPACITY = 1u << 21;

	void dispatch(RendererVirtualMeshStorage *p_storage, RID p_depth, const Projection &p_view_projection, const Projection &p_prev_view_projection, const Vector3 &p_camera, const Size2i &p_internal_size, uint32_t p_cull_mask, float p_max_pixel_error, float p_lod_bias, bool p_occlusion, bool p_debug_stats);
	void free_resources();

	static Dictionary get_last_stats();

private:
	bool _ensure();
	RID _storage(uint32_t p_bytes);
	RID _uniform_set(const Vector<RD::Uniform> &p_uniforms, int p_variant);
	void _set_common_uniforms(Vector<RD::Uniform> &r_uniforms, RID p_input, RID p_output, RID p_groups, RID p_clusters);

	InstanceCullShaderRD instance_shader;
	RID instance_version;
	RID instance_pipeline;
	RID instance_phase2;
	ClusterCullShaderRD cluster_shader;
	RID cluster_version;
	RID cluster_bvh;
	RID cluster_leaf;
	HizPyramidShaderRD hiz_shader;
	RID hiz_version;
	RID hiz_pipeline;

	RID params;
	RID nodes;
	RID phase2_nodes;
	RID groups;
	RID clusters;
	RID level_a;
	RID level_b;
	RID counters;
	RID dummy_hiz;
	RID dummy_sampler;
	RID hiz_prev;
	Size2i hiz_size;
	RID dummy_hiz_set;
	bool initialized = false;
	bool failed = false;
};
