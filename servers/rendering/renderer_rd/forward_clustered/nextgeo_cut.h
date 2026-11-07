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
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/math/color.h"
#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/templates/rid.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/renderer_rd/pipeline_cache_rd.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/cluster_cull.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/hiz_pyramid.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/instance_cull.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/visibility_raster.glsl.gen.h"

class RendererVirtualMeshStorage;

struct NextGeoRasterInputs {
	RID visibility;
	RID instance_id;
	RID velocity;
	RID depth;
	Projection proj;
	Transform3D view;
	Projection prev_proj;
	Transform3D prev_view;
	Vector2 taa_jitter;
	Vector2 prev_taa_jitter;
	Color velocity_clear;
	bool clear_velocity = false;
	bool reset_velocity = false;
	bool reverse_cull = false;
	bool debug_backface = false;
	RID bucket_table;
	uint32_t bucket_table_words = 0;
};

// GodotNext: nextgeo_gpu_cull (#76), nextgeo_visibility_raster (#83)
// The GPU cut manager and draw path (c) visibility raster: instance cull,
// level-synchronous BVH8 traversal, cluster compaction and 1 indirect draw
// per phase per view.
class NextGeoCut {
public:
	static constexpr uint32_t NODE_CAPACITY = 1u << 17;
	static constexpr uint32_t GROUP_CAPACITY = 1u << 17;
	static constexpr uint32_t CLUSTER_CAPACITY = 1u << 21;
	static constexpr uint32_t PHASE2_CLUSTER_BASE = CLUSTER_CAPACITY / 2;
	static constexpr uint32_t HIZ_MAX_MIPS = 16;
	static constexpr uint32_t REQUEST_CAPACITY = 4096;
	static constexpr uint32_t REQUEST_USED_WORDS = 2048;

	bool dispatch(RendererVirtualMeshStorage *p_storage, RID p_depth, const Projection &p_view_projection, const Projection &p_prev_view_projection, const Vector3 &p_camera, const Size2i &p_internal_size, uint32_t p_cull_mask, float p_max_pixel_error, float p_lod_bias, bool p_occlusion, bool p_debug_stats, const NextGeoRasterInputs *p_raster = nullptr, bool p_is_main_view = true);
	void free_resources();

	RID get_clusters_buffer() const { return clusters; }
	static void set_resolve_stats(uint32_t p_active_buckets, uint32_t p_stage_a_draws, uint32_t p_stage_b_draws, bool p_stage_b_wrote);
	static Dictionary get_last_stats();

private:
	bool _ensure();
	void _ensure_hiz(const Size2i &p_internal_size);
	void _build_hiz(RID p_depth, const Size2i &p_internal_size);
	void _clear_visibility_targets(RID p_visibility, RID p_instance_id, const Size2i &p_internal_size);
	bool _draw_raster_phase(uint32_t p_phase, uint32_t p_list_base, const NextGeoRasterInputs &p_raster);
	void _swap_hiz();
	void _release_hiz();
	RID _storage(uint32_t p_bytes);
	RID _uniform_set(const LocalVector<RD::Uniform> &p_uniforms, int p_variant);
	void _set_common_uniforms(LocalVector<RD::Uniform> &r_uniforms, RID p_counters, RID p_hiz_tex, RID p_input, RID p_output, RID p_groups, RID p_clusters, RID p_bvh_args_out, RID p_request_buf);
	void _request_stats(uint32_t p_ring_slot, RID p_counters_buf, uint32_t p_record_count, uint32_t p_max_levels, const void *p_cut, bool p_occlusion, bool p_have_prev, uint32_t p_raster_draws);
	void _publish_stats(const PackedByteArray &p_data, uint32_t p_ring_slot);
	static void _readback_cut_stats_static(const PackedByteArray &p_data, uint64_t p_token, uint32_t p_ring_slot);
	static void _readback_page_requests_static(const PackedByteArray &p_data, uint64_t p_token, uint32_t p_ring_slot);
	void _on_page_requests_readback(const PackedByteArray &p_data, uint32_t p_ring_slot);

	InstanceCullShaderRD instance_shader;
	RID instance_version;
	RID instance_pipeline;
	RID instance_phase2;
	ClusterCullShaderRD cluster_shader;
	RID cluster_version;
	RID cluster_bvh;
	RID cluster_leaf;
	RID cluster_hash;
	HizPyramidShaderRD hiz_shader;
	RID hiz_version;
	RID hiz_pipeline;
	RID clear_vis_pipeline;
	VisibilityRasterShaderRD raster_shader;
	RID raster_version;
	PipelineCacheRD raster_pipelines[4];

	RID params;
	RID raster_params;
	RID nodes;
	RID phase2_nodes;
	RID groups;
	RID clusters;
	RID level_a;
	RID level_b;
	RID bvh_args[2];
	RID draw_args;
	RID counters;
	RID aux_counters;
	RID counters_ring[3];
	uint32_t counters_ring_cursor = 0;
	bool counters_ring_pending[3] = { false, false, false };
	RID request_ring[3];
	uint32_t request_ring_cursor = 0;
	bool request_ring_pending[3] = { false, false, false };
	LocalVector<uint64_t> pending_readback_tokens;
	RID dummy_page_buf;
	RID dummy_hiz;
	RID dummy_sampler;

	// Hi-Z chain: [0] is the previous frame's, [1] the one built from this
	// frame's depth; they swap after the cut.
	RID hiz_texture[2];
	RID hiz_views[2][HIZ_MAX_MIPS];
	uint32_t hiz_mips = 0;
	Size2i hiz_size;
	bool hiz_primed = false;

	bool initialized = false;
	bool failed = false;
	uint32_t stats_tick = 0;
	uint32_t stats_counter = 0;
	uint64_t stats_ring_cpu_hash[3] = { 0, 0, 0 };
	Dictionary stats_ring_info[3];
};
