/**************************************************************************/
/*  renderer_viewport.h                                                   */
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

#include "core/templates/rid_owner.h"
#include "servers/display/display_server_enums.h"
#include "servers/rendering/dynamic_resolution.h"
#include "servers/rendering/renderer_scene_render.h"
#include "servers/rendering/rendering_server_enums.h"
#include "servers/rendering/rendering_server_types.h"
#include "servers/rendering/storage/render_scene_buffers.h"

class RendererViewport {
public:
	struct CanvasBase {
	};

	struct Viewport {
		RID self;
		RID parent;

		// use xr interface to override camera positioning and projection matrices and control output
		bool use_xr = false;

		Size2i internal_size;
		Size2i size;
		uint32_t view_count;
		RID camera;
		RID scenario;

		RSE::ViewportScaling3DMode scaling_3d_mode = RSE::VIEWPORT_SCALING_3D_MODE_BILINEAR;
		// GodotNext: pu_dlss_scaling_mode (#103). Requested/active/reason/effective.
		RSE::ViewportScaling3DMode requested_scaling_3d_mode = RSE::VIEWPORT_SCALING_3D_MODE_BILINEAR;
		RSE::ViewportScaling3DReason scaling_3d_reason = RSE::VIEWPORT_SCALING_3D_REASON_NONE;
		float effective_scaling_3d_scale = 1.0f;
		float scaling_3d_scale = 1.0;
		bool dynres_active = false;
		float dynres_scale = 1.0f;
		float dynres_min_scale = 1.0f;
		float dynres_max_scale = 1.0f;
		float fsr_sharpness = 0.2f;
		RSE::ViewportTSRQuality tsr_quality = RSE::VIEWPORT_TSR_QUALITY_EPIC;
		// GodotNext: path_tracer (#167), path_tracer_denoise (#168). Viewport.path_tracing_*.
		bool path_tracing_enabled = false;
		int path_tracing_samples = RenderingServerTypes::PATH_TRACING_DEFAULT_SAMPLES;
		int path_tracing_max_bounces = RenderingServerTypes::PATH_TRACING_DEFAULT_MAX_BOUNCES;
		float path_tracing_firefly_clamp = 0.0f;
		RSE::ViewportPathTracingDenoiser path_tracing_denoiser = RenderingServerTypes::PATH_TRACING_DEFAULT_DENOISER;
		bool path_tracing_denoise_requested = false;
		float texture_mipmap_bias = 0.0f;
		RSE::ViewportAnisotropicFiltering anisotropic_filtering_level = RSE::VIEWPORT_ANISOTROPY_4X;
		bool fsr_enabled = false;
		uint32_t jitter_phase_count = 0;
		// GodotNext: pt_temporal_upscaler_extension (#192). The CUSTOM
		// registry state the render buffers were configured for.
		uint64_t custom_upscaler_revision = 0;
		uint64_t custom_upscaler_id = 0;
		RSE::ViewportUpdateMode update_mode = RSE::VIEWPORT_UPDATE_WHEN_VISIBLE;
		RID render_target;
		RID render_target_texture;
		Ref<RenderSceneBuffers> render_buffers;

		RSE::ViewportMSAA msaa_2d = RSE::VIEWPORT_MSAA_DISABLED;
		RSE::ViewportMSAA msaa_3d = RSE::VIEWPORT_MSAA_DISABLED;
		RSE::ViewportScreenSpaceAA screen_space_aa = RSE::VIEWPORT_SCREEN_SPACE_AA_DISABLED;
		bool use_taa = false;
		bool use_debanding = false;
		bool force_motion_vectors = false;

		RendererSceneRender::CameraData prev_camera_data;
		uint64_t prev_camera_data_frame = 0;

		bool use_occlusion_culling = false;
		bool occlusion_buffer_dirty = false;

		DisplayServerEnums::WindowID viewport_to_screen = DisplayServerEnums::INVALID_WINDOW_ID;
		Rect2 viewport_to_screen_rect;
		bool viewport_render_direct_to_screen = false;

		bool disable_2d = false;
		RSE::ViewportEnvironmentMode disable_environment = RSE::VIEWPORT_ENVIRONMENT_INHERIT;
		bool disable_3d = false;
		bool measure_render_time = false;

		bool snap_2d_transforms_to_pixel = false;
		bool snap_2d_vertices_to_pixel = false;

		uint64_t time_cpu_begin;
		uint64_t time_cpu_end;

		uint64_t time_gpu_begin;
		uint64_t time_gpu_end;

		RID shadow_atlas;
		int shadow_atlas_size = 2048;
		bool shadow_atlas_16_bits = true;

		bool sdf_active = false;
		// GodotNext: pu_fg_present_path (#203). The SDF settings, so the UI layer can match them.
		RSE::ViewportSDFOversize sdf_oversize = RSE::VIEWPORT_SDF_OVERSIZE_120_PERCENT;
		RSE::ViewportSDFScale sdf_scale = RSE::VIEWPORT_SDF_SCALE_50_PERCENT;

		// GodotNext: pu_fg_present_path (#203). The HUD-less split's textures: the UI layer's render
		// target and the copy of the 3D colour. Empty unless a frame-generation provider is active
		// and this is the main window's viewport.
		struct FrameGenerationSplit {
			RID ui_render_target;
			RID hudless_color;
			Size2i size;
			bool use_hdr = false;
			RSE::ViewportMSAA msaa = RSE::VIEWPORT_MSAA_DISABLED;
			RSE::ViewportSDFOversize sdf_oversize = RSE::VIEWPORT_SDF_OVERSIZE_120_PERCENT;
			RSE::ViewportSDFScale sdf_scale = RSE::VIEWPORT_SDF_SCALE_50_PERCENT;
			bool reset_pending = true;
			// The camera before this frame's 3D render, for clip_to_prev_clip.
			bool has_prev_camera = false;
			RendererSceneRender::CameraData prev_camera;
		} frame_generation;

		float mesh_lod_threshold = 1.0;

		uint64_t last_pass = 0;

		RSE::ViewportDebugDraw debug_draw = RSE::VIEWPORT_DEBUG_DRAW_DISABLED;

		RSE::ViewportClearMode clear_mode = RSE::VIEWPORT_CLEAR_ALWAYS;

		RSE::CanvasItemTextureFilter texture_filter = RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR;
		RSE::CanvasItemTextureRepeat texture_repeat = RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED;

		bool transparent_bg = false;
		bool use_hdr_2d = false;
		float window_output_max_value = 1.0;

		uint32_t canvas_cull_mask = 0xffffffff;

		struct CanvasKey {
			int64_t stacking;
			RID canvas;
			bool operator<(const CanvasKey &p_canvas) const {
				if (stacking == p_canvas.stacking) {
					return canvas < p_canvas.canvas;
				}
				return stacking < p_canvas.stacking;
			}
			CanvasKey() {
				stacking = 0;
			}
			CanvasKey(const RID &p_canvas, int p_layer, int p_sublayer) {
				canvas = p_canvas;
				int64_t sign = p_layer < 0 ? -1 : 1;
				stacking = sign * (((int64_t)Math::abs(p_layer)) << 32) + p_sublayer;
			}
			int get_layer() const { return stacking >> 32; }
		};

		struct CanvasData {
			CanvasBase *canvas = nullptr;
			Transform2D transform;
			int layer;
			int sublayer;
		};

		Transform2D global_transform;

		HashMap<RID, CanvasData> canvas_map;

		RenderingServerTypes::RenderInfo render_info;

		Viewport() {
			view_count = 1;
			update_mode = RSE::VIEWPORT_UPDATE_WHEN_VISIBLE;
			clear_mode = RSE::VIEWPORT_CLEAR_ALWAYS;
			transparent_bg = false;
			use_hdr_2d = false;
			window_output_max_value = 1.0;

			viewport_to_screen = DisplayServerEnums::INVALID_WINDOW_ID;
			shadow_atlas_size = 0;
			measure_render_time = false;

			debug_draw = RSE::VIEWPORT_DEBUG_DRAW_DISABLED;
			screen_space_aa = RSE::VIEWPORT_SCREEN_SPACE_AA_DISABLED;
			use_debanding = false;
			use_occlusion_culling = false;
			occlusion_buffer_dirty = true;

			snap_2d_transforms_to_pixel = false;
			snap_2d_vertices_to_pixel = false;

			use_xr = false;
			sdf_active = false;

			time_cpu_begin = 0;
			time_cpu_end = 0;

			time_gpu_begin = 0;
			time_gpu_end = 0;
		}
	};

	HashMap<String, RID> timestamp_vp_map;

	uint64_t draw_viewports_pass = 0;

	mutable RID_Owner<Viewport, true> viewport_owner;

	Vector<Viewport *> active_viewports;
	Vector<Viewport *> sorted_active_viewports;
	bool sorted_active_viewports_dirty = false;

	int total_objects_drawn = 0;
	int total_vertices_drawn = 0;
	int total_draw_calls_used = 0;

	int num_viewports_with_motion_vectors = 0;

private:
	RID main_3d_render_target;
	// GodotNext: pu_fg_present_path (#203). The one viewport that splits its frame: kept while it stays
	// eligible (frame_generation_owner), chosen again in each draw_viewports() pass.
	RID frame_generation_owner;
	Viewport *frame_generation_viewport = nullptr;
	void _frame_generation_select_viewport();
	DynamicResolutionController dynres_controller;
	uint64_t dynres_last_stamp_frame = UINT64_MAX;
	uint64_t dynres_min_valid_stamp_frame = 0;
	bool dynres_was_enabled = false;
	Vector2 _get_upscaler_scale_range(const Viewport *p_viewport) const;
	Vector<Viewport *> _sort_active_viewports();
	void _viewport_set_size(Viewport *p_viewport, int p_width, int p_height, uint32_t p_view_count);
	bool _viewport_requires_motion_vectors(Viewport *p_viewport);
	void _viewport_set_force_motion_vectors(Viewport *p_viewport, bool p_force_motion_vectors);
	void _configure_3d_render_buffers(Viewport *p_viewport);
	void _draw_3d(Viewport *p_viewport);
	void _draw_viewport(Viewport *p_viewport);
	// GodotNext: pu_fg_present_path (#203). The HUD-less split (plan §21.5 U5).
	void _frame_generation_update_status();
	// Before the frame draws: creates or updates the UI layer's render target.
	bool _frame_generation_prepare(Viewport *p_viewport);
	// Right after _draw_3d: copies the 3D colour to hudless_color and requests the UI layer's clear.
	bool _frame_generation_copy_hudless(Viewport *p_viewport);
	// After the canvas layers: composites the UI layer and hands the inputs to the provider.
	void _frame_generation_split_end(Viewport *p_viewport);
	void _frame_generation_release(Viewport *p_viewport);
	DisplayServerEnums::WindowID _get_containing_window(Viewport *p_viewport);

	int occlusion_rays_per_thread = 512;

	void _resize_occlusion_culling_buffer(const Size2i &p_size);

public:
	RID viewport_allocate();
	void viewport_initialize(RID p_rid);

#ifndef XR_DISABLED
	void viewport_set_use_xr(RID p_viewport, bool p_use_xr);
#endif // XR_DISABLED

	void viewport_set_size(RID p_viewport, int p_width, int p_height, int p_view_count = 1);

	void viewport_attach_to_screen(RID p_viewport, const Rect2 &p_rect = Rect2(), DisplayServerEnums::WindowID p_screen = DisplayServerEnums::MAIN_WINDOW_ID);
	void viewport_set_render_direct_to_screen(RID p_viewport, bool p_enable);

	void viewport_set_active(RID p_viewport, bool p_active);
	void viewport_set_parent_viewport(RID p_viewport, RID p_parent_viewport);

	void viewport_set_scaling_3d_mode(RID p_viewport, RSE::ViewportScaling3DMode p_mode);
	Dictionary viewport_get_scaling_3d_status(RID p_viewport) const;
	void viewport_reset_temporal_history(RID p_viewport);
	// GodotNext: pu_fg_present_path (#203). The registry's status, plus split_textures: how many
	// textures the split holds across all viewports (0 while no provider is active, 2 while one splits).
	Dictionary get_frame_generation_status();
	void viewport_set_scaling_3d_scale(RID p_viewport, float p_scaling_3d_scale);
	void viewport_set_fsr_sharpness(RID p_viewport, float p_sharpness);
	void viewport_set_tsr_quality(RID p_viewport, RSE::ViewportTSRQuality p_quality);
	// GodotNext: path_tracer (#167), path_tracer_denoise (#168)
	void viewport_set_path_tracing_enabled(RID p_viewport, bool p_enabled);
	void viewport_set_path_tracing_samples(RID p_viewport, int p_samples);
	void viewport_set_path_tracing_max_bounces(RID p_viewport, int p_max_bounces);
	void viewport_set_path_tracing_firefly_clamp(RID p_viewport, float p_firefly_clamp);
	void viewport_set_path_tracing_denoiser(RID p_viewport, RSE::ViewportPathTracingDenoiser p_denoiser);
	void viewport_request_path_tracing_denoise(RID p_viewport);
	Dictionary viewport_get_path_tracing_info(RID p_viewport) const;
	// Path traced: path_tracing_enabled, or the VIEWPORT_DEBUG_DRAW_PATH_TRACER debug draw.
	static bool viewport_path_tracing_active(bool p_enabled, RSE::ViewportDebugDraw p_debug_draw) { return p_enabled || p_debug_draw == RSE::VIEWPORT_DEBUG_DRAW_PATH_TRACER; }
	void viewport_set_texture_mipmap_bias(RID p_viewport, float p_mipmap_bias);
	void viewport_set_anisotropic_filtering_level(RID p_viewport, RSE::ViewportAnisotropicFiltering p_anisotropic_filtering_level);

	void viewport_set_update_mode(RID p_viewport, RSE::ViewportUpdateMode p_mode);
	RSE::ViewportUpdateMode viewport_get_update_mode(RID p_viewport) const;
	void viewport_set_vflip(RID p_viewport, bool p_enable);

	void viewport_set_clear_mode(RID p_viewport, RSE::ViewportClearMode p_clear_mode);

	RID viewport_get_render_target(RID p_viewport) const;
	RID viewport_get_texture(RID p_viewport) const;
	RID viewport_get_occluder_debug_texture(RID p_viewport) const;

	void viewport_set_prev_camera_data(RID p_viewport, const RendererSceneRender::CameraData *p_camera_data);
	const RendererSceneRender::CameraData *viewport_get_prev_camera_data(RID p_viewport);

	void viewport_set_disable_2d(RID p_viewport, bool p_disable);
	void viewport_set_environment_mode(RID p_viewport, RSE::ViewportEnvironmentMode p_mode);
	void viewport_set_disable_3d(RID p_viewport, bool p_disable);

	bool viewport_is_environment_disabled(Viewport *viewport);

	void viewport_attach_camera(RID p_viewport, RID p_camera);
	void viewport_set_scenario(RID p_viewport, RID p_scenario);
	void viewport_attach_canvas(RID p_viewport, RID p_canvas);
	void viewport_remove_canvas(RID p_viewport, RID p_canvas);
	void viewport_set_canvas_transform(RID p_viewport, RID p_canvas, const Transform2D &p_offset);
	void viewport_set_transparent_background(RID p_viewport, bool p_enabled);
	void viewport_set_use_hdr_2d(RID p_viewport, bool p_use_hdr_2d);

	bool viewport_is_using_hdr_2d(RID p_viewport) const;

	void viewport_set_global_canvas_transform(RID p_viewport, const Transform2D &p_transform);
	void viewport_set_canvas_stacking(RID p_viewport, RID p_canvas, int p_layer, int p_sublayer);

	void viewport_set_canvas_cull_mask(RID p_viewport, uint32_t p_canvas_cull_mask);

	void viewport_set_positional_shadow_atlas_size(RID p_viewport, int p_size, bool p_16_bits = true);
	void viewport_set_positional_shadow_atlas_quadrant_subdivision(RID p_viewport, int p_quadrant, int p_subdiv);

	void viewport_set_msaa_2d(RID p_viewport, RSE::ViewportMSAA p_msaa);
	void viewport_set_msaa_3d(RID p_viewport, RSE::ViewportMSAA p_msaa);
	void viewport_set_screen_space_aa(RID p_viewport, RSE::ViewportScreenSpaceAA p_mode);
	void viewport_set_use_taa(RID p_viewport, bool p_use_taa);
	void viewport_set_use_debanding(RID p_viewport, bool p_use_debanding);
	void viewport_set_force_motion_vectors(RID p_viewport, bool p_force_motion_vectors);
	void viewport_set_use_occlusion_culling(RID p_viewport, bool p_use_occlusion_culling);
	void viewport_set_occlusion_rays_per_thread(int p_rays_per_thread);
	void viewport_set_occlusion_culling_build_quality(RSE::ViewportOcclusionCullingBuildQuality p_quality);
	void viewport_set_mesh_lod_threshold(RID p_viewport, float p_pixels);

	virtual int viewport_get_render_info(RID p_viewport, RSE::ViewportRenderInfoType p_type, RSE::ViewportRenderInfo p_info);
	virtual void viewport_set_debug_draw(RID p_viewport, RSE::ViewportDebugDraw p_draw);

	void viewport_set_measure_render_time(RID p_viewport, bool p_enable);
	float viewport_get_measured_render_time_cpu(RID p_viewport) const;
	float viewport_get_measured_render_time_gpu(RID p_viewport) const;

	void viewport_set_snap_2d_transforms_to_pixel(RID p_viewport, bool p_enabled);
	void viewport_set_snap_2d_vertices_to_pixel(RID p_viewport, bool p_enabled);

	void viewport_set_default_canvas_item_texture_filter(RID p_viewport, RSE::CanvasItemTextureFilter p_filter);
	void viewport_set_default_canvas_item_texture_repeat(RID p_viewport, RSE::CanvasItemTextureRepeat p_repeat);

	void viewport_set_sdf_oversize_and_scale(RID p_viewport, RSE::ViewportSDFOversize p_over_size, RSE::ViewportSDFScale p_scale);

	virtual RID viewport_find_from_screen_attachment(DisplayServerEnums::WindowID p_id = DisplayServerEnums::MAIN_WINDOW_ID) const;

	void viewport_set_vrs_mode(RID p_viewport, RSE::ViewportVRSMode p_mode);
	void viewport_set_vrs_update_mode(RID p_viewport, RSE::ViewportVRSUpdateMode p_mode);
	void viewport_set_vrs_texture(RID p_viewport, RID p_texture);

	void handle_timestamp(String p_timestamp, uint64_t p_cpu_time, uint64_t p_gpu_time);

	void draw_viewports(bool p_swap_buffers);
	RID get_main_3d_render_target() const { return main_3d_render_target; }

	bool free(RID p_rid);

	int get_total_objects_drawn() const;
	int get_total_primitives_drawn() const;
	int get_total_draw_calls_used() const;
	int get_num_viewports_with_motion_vectors() const;

	// Workaround for setting this on thread.
	void call_set_vsync_mode(DisplayServerEnums::VSyncMode p_mode, DisplayServerEnums::WindowID p_window);

	RendererViewport();
	virtual ~RendererViewport() {}
};
