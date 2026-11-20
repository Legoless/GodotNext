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
#include "core/math/rect2i.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/math/vector4.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_raster_bins.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_sw_raster.h"
// GodotNext: vsm_nanite (#217).
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_vsm.h"
#include "servers/rendering/renderer_rd/pipeline_cache_rd.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/cluster_cull.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/hiz_pyramid.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/instance_cull.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/raster_bin_compact.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/sw_raster.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/visibility_export.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/visibility_raster.glsl.gen.h"
// GodotNext: vsm_nanite (#217).
#include "servers/rendering/renderer_rd/shaders/vsm_raster_nextgeo.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/vsm_raster_sw.glsl.gen.h"

class RendererVirtualMeshStorage;

struct NextGeoShadowViewDesc {
	Projection projection;
	Transform3D transform;
	Vector4 frustum[6];
	Vector3 light_position;
	Size2i tile_size = Size2i(1, 1);
	float z_near = 0.001f;
	float z_far = 1.0f;
	float max_texel_error = 1.0f;
	float lod_bias = 1.0f;
	uint32_t cull_mask = 0xFFFFFFFFu;
	uint32_t cut_flags = 0u;
	int32_t light_type = 0;
	int32_t pass = 0;
	bool use_pancake = false;
	bool dual_paraboloid = false;
	bool dual_paraboloid_flip = false;
	bool flip_y = false;
	bool flip_cull = false;
};

// GodotNext: nextgeo_raster_bins (#155, plan §21.4 N3, N4). One programmable raster bin this frame: its
// material raster target's shaders (the software and hardware forms, per visibility-buffer storage:
// image, buffer) and the material's parameter uniforms for set 3.
struct NextGeoRasterBinProgram {
	uint32_t bin = 0;
	RID sw_shader[2];
	RID hw_shader[2];
	LocalVector<RD::Uniform> material_uniforms;
};

// GodotNext: vsm_nanite (#217, plan §21.3 contract S4, step 7). One programmable raster bin in the VSM
// page raster (group 2 depth-only shaders from SceneShaderForwardClustered, sharing #155's table).
struct NextGeoVSMBinProgram {
	uint32_t bin = 0;
	uint32_t features = 0;
	RID sw_shader;
	RID hw_shader;
	LocalVector<RD::Uniform> material_uniforms;
};

// GodotNext: vsm_nanite (#217). Inputs for one view's VSM x Nanite directional + local cull & raster.
struct NextGeoVSMInputs {
	RID pool_texture;
	RID page_table_buffer;
	RID map_data_buffer;
	RID dirty_buffer;
	RID alloc_state_buffer;
	RID indirect_buffer;
	uint32_t table_maps = 0;
	uint32_t pool_pages = 0;
	uint32_t grid_width = 0;
	uint64_t pool_reallocations = 0;
	Vector3 camera_position;
	float max_texel_error = RendererRD::NEXTGEO_VSM_DEFAULT_MAX_TEXEL_ERROR;
	float lod_bias = 1.0f;
	float sw_max_edge_texels = RendererRD::NEXTGEO_VSM_DEFAULT_SW_MAX_EDGE_TEXELS;
	bool hiz_enabled = true;
	bool dynamic_layer_hook = false;
	bool count_sw_writes = false;
	LocalVector<RendererRD::NextGeoVSMMapMeta> map_meta;
	const uint32_t *raster_bin_table = nullptr;
	uint32_t raster_bin_table_words = 0;
	LocalVector<NextGeoVSMBinProgram> raster_bin_programs;
	LocalVector<RD::Uniform> environment_uniforms;
};

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
	// GodotNext: nextgeo_sw_raster (#162). Set to run the hybrid raster: the 64-bit visibility buffer
	// (a texture for NEXTGEO_VIS64_IMAGE, a storage buffer for NEXTGEO_VIS64_BUFFER) and the
	// triangle-size switch. Without it the view keeps the v2.4 hardware raster.
	RID vis64;
	RendererRD::NextGeoVis64Storage vis64_storage = RendererRD::NEXTGEO_VIS64_NONE;
	Vector3 camera_forward = Vector3(0.0f, 0.0f, -1.0f);
	float z_near = 0.05f;
	bool orthographic = false;
	float sw_raster_max_edge_px = 0.0f;
	// GodotNext: nextgeo_raster_bins (#155). With the hybrid raster: the per-(instance, surface) raster-bin
	// table (nextgeo_raster_bins.h), the programmable bins its surfaces use, and the scene environment
	// their material code reads (raster_material_inc.glsl's set 1: the scene data, the per-record
	// instance data, the global shader uniforms and the 12 material samplers). No programs: no
	// programmable pass.
	const uint32_t *raster_bin_table = nullptr;
	uint32_t raster_bin_table_words = 0;
	LocalVector<NextGeoRasterBinProgram> raster_bin_programs;
	LocalVector<RD::Uniform> environment_uniforms;
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
	static constexpr uint32_t MAX_SHADOW_VIEWS = 80;
	static constexpr uint32_t SHADOW_NODES_PER_VIEW = 8192;
	static constexpr uint32_t SHADOW_GROUPS_PER_VIEW = 8192;
	static constexpr uint32_t SHADOW_CLUSTERS_PER_VIEW = 32768;

	// GodotNext: nextgeo_sw_raster (#162). True on the Nanite device tier (N1) once the hybrid raster's
	// shaders exist. The storage it picks is the image unless the device has only buffer 64-bit
	// atomics or p_prefer_buffer asks for the buffer form (rendering/nextgeo/debug_vis64_buffer).
	bool is_hybrid_raster_available();
	RendererRD::NextGeoVis64Storage get_vis64_storage(bool p_prefer_buffer) const;
	const RendererRD::NextGeoRasterTier &get_raster_tier() const { return raster_tier; }

	bool dispatch(RendererVirtualMeshStorage *p_storage, RID p_depth, const Projection &p_view_projection, const Projection &p_prev_view_projection, const Vector3 &p_camera, const Size2i &p_internal_size, uint32_t p_cull_mask, float p_max_pixel_error, float p_lod_bias, bool p_occlusion, bool p_debug_stats, const NextGeoRasterInputs *p_raster = nullptr, bool p_is_main_view = true);
	bool dispatch_shadows(RendererVirtualMeshStorage *p_storage, const NextGeoShadowViewDesc *p_views, uint32_t p_view_count, bool p_debug_stats);
	bool draw_shadow_view(uint32_t p_shadow_view, RID p_framebuffer, const Rect2i &p_rect, bool p_dual_paraboloid);
	// GodotNext: vsm_nanite (#217, plan §21.3 contract S4). Per-page multi-view cluster cull, hybrid
	// HW/SW rasterization (1 indirect chain for directional maps, 1 for local maps), and 1-level
	// per-page Hi-Z build.
	bool is_vsm_available();
	bool dispatch_vsm(RendererVirtualMeshStorage *p_storage, const NextGeoVSMInputs &p_inputs, Dictionary *r_stats = nullptr);
	void update_vsm_hiz(RID p_pool_texture, RID p_dirty_buffer, RID p_alloc_state_buffer, RID p_indirect_buffer, uint32_t p_table_maps, uint32_t p_pool_pages, uint32_t p_grid_width, uint64_t p_pool_reallocations);
	void invalidate_vsm_hiz();
	void mark_vsm_dynamic_test(RID p_page_table, uint32_t p_table_maps, uint32_t p_pool_pages);
	RID get_vsm_clusters_buffer(uint32_t p_chain) const { return p_chain < RendererRD::NEXTGEO_VSM_CHAIN_COUNT ? vsm_clusters[p_chain] : RID(); }
	RID get_vsm_counters_buffer(uint32_t p_chain) const { return p_chain < RendererRD::NEXTGEO_VSM_CHAIN_COUNT ? vsm_counters[p_chain] : RID(); }
	void begin_shadow_raster_timestamps();
	void end_shadow_raster_timestamps();
	void free_resources();

	RID get_clusters_buffer() const { return clusters; }
	static void set_resolve_stats(uint32_t p_active_buckets, uint32_t p_stage_a_draws, uint32_t p_stage_b_draws, bool p_stage_b_wrote);
	// GodotNext: nextgeo_shading_bins (#158). The shading path of the last main view's Stage A/B (keys
	// shading_*), merged into the stats.
	static void set_shading_stats(const Dictionary &p_stats);
	static Dictionary get_last_stats();

private:
	bool _ensure();
	void _ensure_hiz(const Size2i &p_internal_size);
	void _build_hiz(RID p_depth, const Size2i &p_internal_size);
	void _clear_visibility_targets(RID p_visibility, RID p_instance_id, const Size2i &p_internal_size);
	bool _draw_raster_phase(uint32_t p_phase, uint32_t p_list_base, const NextGeoRasterInputs &p_raster);
	// GodotNext: nextgeo_sw_raster (#162).
	bool _ensure_hybrid(RenderingDevice *p_rd);
	void _clear_vis64(const NextGeoRasterInputs &p_raster, const Size2i &p_internal_size);
	RID _get_vis64_framebuffer(const Size2i &p_size);
	bool _draw_hybrid_phase(uint32_t p_phase, uint32_t p_list_base, const NextGeoRasterInputs &p_raster, RID p_counters, const Size2i &p_internal_size, bool p_stats);
	// GodotNext: nextgeo_raster_bins (#155).
	bool _ensure_raster_bins(RenderingDevice *p_rd);
	bool _prepare_raster_bins(const NextGeoRasterInputs &p_raster);
	void _compact_raster_bins(uint32_t p_phase, uint32_t p_list_base);
	void _draw_raster_bins(uint32_t p_phase, uint32_t p_list_base, const NextGeoRasterInputs &p_raster, const Size2i &p_internal_size);
	RID _raster_bin_compute_pipeline(RID p_shader);
	RID _raster_bin_render_pipeline(RID p_shader, RD::FramebufferFormatID p_format);
	// GodotNext: vsm_nanite (#217).
	bool _ensure_vsm();
	bool _ensure_vsm_hiz(uint32_t p_pool_pages, uint32_t p_grid_width, uint64_t p_pool_reallocations);
	void _dispatch_vsm_hiz_build(RID p_pool_texture, RID p_dirty_buffer, RID p_alloc_state_buffer, RID p_indirect_buffer, uint32_t p_table_maps, uint32_t p_pool_pages, uint32_t p_grid_width);
	bool _ensure_vsm_bins(RenderingDevice *p_rd);
	void _compact_vsm_bins();
	void _draw_vsm_bins(uint32_t p_chain, const NextGeoVSMInputs &p_inputs);
	RID _vsm_bin_render_pipeline(RID p_shader, RD::FramebufferFormatID p_format);
	void _free_vsm_resources();
	void _request_vsm_stats(uint32_t p_ring_slot);
	void _publish_vsm_stats(const PackedByteArray &p_data, uint32_t p_ring_slot);
	static void _readback_vsm_stats_static(const PackedByteArray &p_data, uint64_t p_token, uint32_t p_ring_slot);
	void _swap_hiz();
	void _release_hiz();
	RID _storage(uint32_t p_bytes);
	RID _uniform_set(const LocalVector<RD::Uniform> &p_uniforms, int p_variant);
	void _set_common_uniforms(LocalVector<RD::Uniform> &r_uniforms, RID p_counters, RID p_hiz_tex, RID p_input, RID p_output, RID p_groups, RID p_clusters, RID p_bvh_args_out, RID p_request_buf);
	void _set_shadow_uniforms(LocalVector<RD::Uniform> &r_uniforms, RID p_counters, RID p_input, RID p_output, RID p_groups, RID p_clusters, RID p_bvh_args_out);
	void _set_raster_bin_uniforms(LocalVector<RD::Uniform> &r_uniforms);
	void _harvest_shadow_timestamps();
	void _request_stats(uint32_t p_ring_slot, RID p_counters_buf, uint32_t p_record_count, uint32_t p_max_levels, const void *p_cut, bool p_occlusion, bool p_have_prev, uint32_t p_raster_draws);
	void _publish_stats(const PackedByteArray &p_data, uint32_t p_ring_slot);
	void _request_shadow_stats(uint32_t p_ring_slot, RID p_counters_buf, uint32_t p_view_count, uint32_t p_record_count, uint32_t p_max_levels, const void *p_cuts, const NextGeoShadowViewDesc *p_views);
	void _publish_shadow_stats(const PackedByteArray &p_data, uint32_t p_ring_slot);
	static void _readback_cut_stats_static(const PackedByteArray &p_data, uint64_t p_token, uint32_t p_ring_slot);
	static void _readback_shadow_stats_static(const PackedByteArray &p_data, uint64_t p_token, uint32_t p_ring_slot);
	static void _readback_page_requests_static(const PackedByteArray &p_data, uint64_t p_token, uint32_t p_ring_slot);
	void _on_page_requests_readback(const PackedByteArray &p_data, uint32_t p_ring_slot);

	InstanceCullShaderRD instance_shader;
	RID instance_version;
	RID instance_pipeline;
	RID instance_phase2;
	RID instance_shadow_pipeline;
	ClusterCullShaderRD cluster_shader;
	RID cluster_version;
	RID cluster_bvh;
	RID cluster_leaf;
	RID cluster_hash;
	RID cluster_shadow_bvh;
	RID cluster_shadow_leaf;
	RID cluster_shadow_hash;
	HizPyramidShaderRD hiz_shader;
	RID hiz_version;
	RID hiz_pipeline;
	RID clear_vis_pipeline;
	VisibilityRasterShaderRD raster_shader;
	RID raster_version;
	PipelineCacheRD raster_pipelines[4];
	PipelineCacheRD shadow_raster_pipelines[2];

	// GodotNext: nextgeo_sw_raster (#162). Built only on the Nanite device tier.
	RendererRD::NextGeoRasterTier raster_tier;
	bool hybrid_ready = false;
	bool vis64_image_ready = false;
	bool vis64_buffer_ready = false;
	SwRasterShaderRD sw_shader;
	RID sw_version;
	RID sw_pipelines[2]; // Image, buffer.
	RID vis64_clear_pipelines[2];
	VisibilityExportShaderRD export_shader;
	RID export_version;
	PipelineCacheRD export_pipelines[4]; // (buffer ? 2 : 0) | (velocity ? 1 : 0).
	PipelineCacheRD vis64_raster_pipelines[4]; // (buffer ? 2 : 0) | (back-face debug ? 1 : 0).
	VisibilityRasterShaderRD vis64_raster_shader; // visibility_raster.glsl's MODE_VIS64 variants.
	RID vis64_raster_version;
	RID raster_args;
	RID raster_counts;
	RID raster_lists;
	// The attachment-less framebuffer of the hardware path, at the last internal size.
	RID vis64_framebuffer;
	Size2i vis64_framebuffer_size;
	// GodotNext: nextgeo_raster_bins (#155). Built the first time a programmable bin is used.
	bool raster_bins_ready = false;
	bool raster_bins_active = false; // This frame's main view runs programmable bins.
	RasterBinCompactShaderRD raster_bin_compact_shader;
	RID raster_bin_compact_version;
	RID raster_bin_prefix_pipeline;
	RID raster_bin_scatter_pipeline;
	RID raster_bin_state; // nextgeo_raster_bins.h's state words.
	RID raster_bin_args; // Dedicated indirect dispatch/draw arguments (written only in PREFIX).
	RID raster_bin_append;
	RID raster_bin_lists;
	RID raster_bin_table_buffer;
	uint32_t raster_bin_table_capacity = 0; // Words.
	HashMap<RID, RID> raster_bin_compute_pipelines; // Shader -> pipeline.
	HashMap<RID, RID> raster_bin_render_pipelines; // Shader -> pipeline, for the vis64 framebuffer format.
	RD::FramebufferFormatID raster_bin_render_format = RD::INVALID_ID;

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
	RID shadow_params;
	RID shadow_raster_params;
	RID shadow_nodes;
	RID shadow_level_a;
	RID shadow_level_b;
	RID shadow_groups;
	RID shadow_clusters;
	RID shadow_bvh_args[2];
	RID shadow_draw_args;
	RID shadow_counters;
	RID shadow_counters_ring[3];
	uint32_t shadow_counters_ring_cursor = 0;
	bool shadow_counters_ring_pending[3] = { false, false, false };
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

	struct ShadowStatsMeta {
		uint32_t view_count = 0;
		int64_t render_frame = 0;
		LocalVector<uint64_t> cpu_hashes;
		LocalVector<uint32_t> cpu_instances;
		LocalVector<uint32_t> cpu_groups;
		LocalVector<uint32_t> cpu_clusters;
		LocalVector<float> max_cut_errors;
		LocalVector<int32_t> light_types;
		LocalVector<int32_t> passes;
		LocalVector<uint32_t> cut_flags;
		LocalVector<Size2i> tile_sizes;
	};
	ShadowStatsMeta shadow_stats_ring_meta[3];
	uint32_t shadow_stats_tick = 0;
	uint32_t shadow_stats_counter = 0;
	uint32_t shadow_active_view_count = 0;
	bool shadow_capture_timestamps = false;
	uint64_t last_harvested_ts_frame = UINT64_MAX;

	// GodotNext: vsm_nanite (#217, plan §21.3 contract S4). Lazily initialized on first VSM x Nanite view.
	bool vsm_ready = false;
	bool vsm_failed = false;
	ClusterCullShaderRD vsm_cluster_shader;
	RID vsm_cluster_version;
	RID vsm_setup_pipeline;
	RID vsm_seed_pipeline;
	RID vsm_bvh_pipeline;
	RID vsm_leaf_pipeline;
	VsmRasterNextgeoShaderRD vsm_hw_shader;
	RID vsm_hw_version;
	PipelineCacheRD vsm_hw_pipeline;
	VsmRasterSwShaderRD vsm_sw_shader;
	RID vsm_sw_version;
	RID vsm_sw_pipeline;
	RID vsm_hiz_build_pipeline;
	RID vsm_mark_dynamic_pipeline;
	RID vsm_empty_fb;
	RID vsm_params_buffer;
	RID vsm_map_meta_buffer;
	uint32_t vsm_map_meta_capacity = 0;
	RID vsm_counters[RendererRD::NEXTGEO_VSM_CHAIN_COUNT];
	RID vsm_counters_ring[3]; // 2 * sizeof(NextGeoVSMCounters) per ring slot for async telemetry.
	uint32_t vsm_counters_ring_cursor = 0;
	bool vsm_counters_ring_pending[3] = { false, false, false };
	RID vsm_seed_args;
	RID vsm_bvh_args[2]; // Ping-pong across BVH levels (zero render-graph dual-usage hazards).
	RID vsm_group_args;
	RID vsm_raster_args;
	RID vsm_raster_counts;
	RID vsm_nodes_a;
	RID vsm_nodes_b;
	RID vsm_groups;
	RID vsm_clusters[RendererRD::NEXTGEO_VSM_CHAIN_COUNT];
	RID vsm_raster_lists;
	// Per-page 1-level 8x8 Hi-Z texture & owner table (resized with the VSM pool).
	RID vsm_hiz_texture;
	RID vsm_hiz_owner;
	uint32_t vsm_hiz_pool_pages = 0;
	uint32_t vsm_hiz_grid_width = 0;
	uint64_t vsm_hiz_pool_reallocations = UINT64_MAX;
	// VSM programmable raster bins (Step 7, reuses raster_bin_compact_shader with dedicated VSM buffers).
	bool vsm_bins_ready = false;
	RID vsm_bin_state;
	RID vsm_bin_args;
	RID vsm_bin_append;
	RID vsm_bin_lists;
	RID vsm_bin_table_buffer;
	uint32_t vsm_bin_table_capacity = 0;
	HashMap<RID, RID> vsm_bin_render_pipelines;
	RD::FramebufferFormatID vsm_bin_render_format = RD::INVALID_ID;
	// Last asynchronous GPU counters read back from vsm_counters_ring.
	RendererRD::NextGeoVSMCounters vsm_last_gpu_counters[RendererRD::NEXTGEO_VSM_CHAIN_COUNT];
	bool vsm_have_gpu_counters = false;
	uint64_t vsm_dispatched_views = 0;
};
