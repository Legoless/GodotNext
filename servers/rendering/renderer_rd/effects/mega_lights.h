/**************************************************************************/
/*  mega_lights.h                                                         */
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

// GodotNext: megalights_sampling (#142, plan §21.3 contract M3, GM-1, GM-2, budget M-3),
// megalights_visibility (#146, plan §21.3 contract M4, GM-1, GM-8, GM-12, budget M-4), and
// megalights_denoiser (#135, plan §21.3 contract M5, GM-1, GM-7, budget M-5).
// MegaLights sampling core + visibility chain + demodulated spatio-temporal denoiser: per-pixel
// weighted reservoir sampling over the #139 packed froxel light grid, Hi-Z screen-space shadow
// trace, 2x2 block ray deduplication & stream compaction, near-field ray query via #145
// trace_shadow() (with alpha cutout + #130 material retrace), optional #126 far-field TLAS
// continuation, optional #182 SWRT global-SDF continuation on the non-RT tier, per-light
// VSM-method sampling via #222 vsm_shadow() (SMRT 2 rays), packed RGBA8 transmittance + R16F
// hit-distance outputs, 8x8 tile guiding, demodulated diffuse/specular shading (+ 2x2 bilateral
// half-res fill), temporal accumulation with firefly clamped-energy counter and 2-pass à-trous
// spatial filter (or rr_bypass handoff for DLSS-RR), and ML_{NOISY,DENOISED,VARIANCE} debug views.

#include "core/math/color.h"
#include "core/math/math_funcs.h"
#include "core/math/projection.h"
#include "core/math/rect2i.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "core/math/vector3i.h"
#include "core/math/vector4.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/renderer_rd/storage_rd/render_buffer_custom_data_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_enums.h"

#include <cstdint>

class RenderDataRD;
class RenderSceneBuffersRD;
class NextGISWRTTrace;
class NextGIVoxelLighting;

namespace RendererRD {

class LightFunctionAtlas;
class NextGI;
class NextGIDenoise;

// Per-view GPU resources owned by RenderSceneBuffersRD under RB_SCOPE_MEGALIGHTS.
class MegaLightsViewData : public RenderBufferCustomDataRD {
	GDCLASS(MegaLightsViewData, RenderBufferCustomDataRD);

	Size2i internal_size;
	Size2i sample_grid_size;
	Size2i guiding_tile_dims;
	uint32_t samples_per_pixel = 0;
	uint32_t downsample_mode = 1; // 0 = full, 1 = half

	RID sample_buffer;
	RID visibility_buffer;
	// GodotNext: megalights_visibility (#146). Per-sample R16F hit distance (2DArray, layer = k, miss = 65504.0).
	RID hit_distance_texture;
	RID guiding_buffers[2];
	uint32_t guiding_write_index = 0;
	bool guiding_has_history = false;

	RID reference_accum_texture;
	Size2i reference_accum_size;
	uint32_t reference_frame_count = 0;
	uint64_t reference_camera_hash = 0;

	RID verify_accum_texture;
	RID verify_counts_buffer;
	Size2i verify_size;

	RID params_ubo;
	RID layer_group_buffer;
	uint32_t layer_group_capacity = 0;
	RID light_shadow_policy_buffer;
	uint32_t light_shadow_policy_capacity = 0;

	// GodotNext: megalights_visibility (#146). Lazy per-view visibility working buffers (0 allocated when off).
	RID vis_params_ubo;
	RID vis_counters_buffer;
	RID vis_indirect_buffer;
	RID vis_vsm_params_buffer;
	RID vis_ray_list_buffer;
	RID vis_vsm_list_buffer;
	RID vis_sample_ray_map_buffer;
	uint32_t vis_list_capacity = 0;
	RID vis_retrace_buffer;

	// GodotNext: megalights_denoiser (#135, plan §21.3 contract M5). Lazy per-view shade & denoise targets (0 when off).
	Size2i shade_sample_size;
	Size2i shade_full_size;
	RID shade_sample_diffuse;
	RID shade_sample_specular;
	RID noisy_diffuse;
	RID noisy_specular;
	RID denoise_params_ubo;

	Size2i denoise_size;
	Size2i denoise_reset_tile_dims;
	RID denoise_diffuse[2];
	RID denoise_specular[2];
	RID denoise_scratch_diffuse;
	RID denoise_scratch_specular;
	RID denoise_moments[2];
	RID denoise_history_len[2];
	RID denoise_prev_depth_normal;
	RID denoise_clamp_stats_buffer;
	RID denoise_reset_mask_buffer;
	uint32_t denoise_history_index = 0;
	bool denoise_has_history = false;
	uint64_t denoise_last_frame = 0;
	Transform3D denoise_last_cam_transform;
	bool denoise_has_cam_transform = false;

	Size2i denoise_debug_size;
	RID denoise_debug_texture;

	RID denoise_output_diffuse;
	RID denoise_output_specular;
	bool denoise_last_rr_bypass = false;

	uint32_t frame_index = 0;

	// GodotNext: megalights_fog_translucency (#137, plan §21.3 contract M7, GM-6).
	// Shared volumetric-fog froxel light & SH-L1 buffers (allocated only when fog+MegaLights active)
	// and front-layer M1 neutral guide textures (allocated only when front_layer_translucency is on).
	bool is_front_layer_view = false;
	Vector3i froxel_fog_size;
	RID froxel_fog_tex[2];
	RID froxel_sh_r[2];
	RID froxel_sh_g[2];
	RID froxel_sh_b[2];
	RID froxel_params_ubo;
	uint32_t froxel_slot = 0;
	bool froxel_history_valid = false;
	uint32_t froxel_history_frames = 0;
	Transform3D froxel_prev_cam;
	Projection froxel_prev_view_proj;
	bool froxel_has_prev_cam = false;

	Size2i front_layer_guide_size;
	RID front_layer_albedo_tex;
	RID front_layer_material_tex;

	// GodotNext: megalights_frame (#143, GM-9) & #137 (M7). Exact per-view allocated GPU byte accounting.
	uint64_t sampling_bytes = 0;
	uint64_t visibility_bytes = 0;
	uint64_t retrace_bytes = 0;
	uint64_t shade_bytes = 0;
	uint64_t denoise_bytes = 0;
	uint64_t denoise_debug_bytes = 0;
	uint64_t reference_bytes = 0;
	uint64_t verify_bytes = 0;
	uint64_t transient_bytes = 0;
	uint64_t froxel_bytes = 0;
	uint64_t front_layer_guide_bytes = 0;
	uint64_t tracked_total_bytes = 0;
	uint64_t tracked_froxel_bytes = 0;
	uint64_t tracked_front_layer_bytes = 0;

	void _update_memory_accounting();

	void _free_sampling_buffers();
	void _free_visibility_buffers();
	void _free_shade_buffers();
	void _free_denoise_buffers();
	void _free_denoise_debug_texture();
	void _free_reference_buffers();
	void _free_verify_buffers();
	void _free_transient_buffers();
	void _free_froxel_buffers();
	void _free_front_layer_guide_textures();

public:
	struct DenoiseStats {
		uint64_t clamped_energy_removed_fp = 0;
		uint64_t total_direct_energy_fp = 0;
		double clamped_energy_ratio = 0.0;
		uint32_t clamped_pixels = 0;
		uint32_t valid_pixels = 0;
		bool rr_bypass = false;
	};

	struct DenoiseReadbackState {
		SafeRefCount refs;
		SafeFlag alive;
		SafeFlag pending;
		uint32_t removed_fp = 0;
		uint32_t total_fp = 0;
		uint32_t clamped_pixels = 0;
		uint32_t valid_pixels = 0;
		bool has_result = false;
	};

private:
	DenoiseReadbackState *denoise_readback = nullptr;
	DenoiseStats last_denoise_stats;

public:
	virtual void configure(RenderSceneBuffersRD *p_render_buffers) override;
	virtual void free_data() override;

	bool ensure_sampling_buffers(const Size2i &p_internal_size, uint32_t p_samples_per_pixel, uint32_t p_downsample_mode);
	bool ensure_visibility_buffers(uint32_t p_total_samples);
	bool ensure_retrace_buffer();
	bool ensure_shade_buffers(const Size2i &p_internal_size, const Size2i &p_sample_size, uint32_t p_downsample_mode);
	bool ensure_denoise_buffers(const Size2i &p_internal_size, bool p_rr_bypass);
	bool ensure_denoise_debug_texture(const Size2i &p_internal_size);
	bool ensure_reference_buffers(const Size2i &p_internal_size);
	bool ensure_verify_buffers(const Size2i &p_internal_size);
	bool ensure_layer_group_buffer(uint32_t p_count);
	bool ensure_light_shadow_policy_buffer(uint32_t p_count);
	// GodotNext: megalights_fog_translucency (#137, plan §21.3 contract M7).
	void set_is_front_layer_view(bool p_is_front_layer);
	bool ensure_froxel_buffers(bool p_fog_active, const Vector3i &p_fog_size, bool p_need_fog_sh);
	void free_froxel_buffers();
	bool ensure_front_layer_guide_textures(const Size2i &p_size);
	void free_front_layer_guide_textures();

	RID get_froxel_params_ubo() const { return froxel_params_ubo; }
	RID get_froxel_fog_texture() const { return froxel_fog_tex[froxel_slot]; }
	RID get_froxel_fog_write_texture() const { return froxel_fog_tex[1u - froxel_slot]; }
	RID get_froxel_sh_r() const { return froxel_sh_r[froxel_slot]; }
	RID get_froxel_sh_g() const { return froxel_sh_g[froxel_slot]; }
	RID get_froxel_sh_b() const { return froxel_sh_b[froxel_slot]; }
	RID get_froxel_sh_write_r() const { return froxel_sh_r[1u - froxel_slot]; }
	RID get_froxel_sh_write_g() const { return froxel_sh_g[1u - froxel_slot]; }
	RID get_froxel_sh_write_b() const { return froxel_sh_b[1u - froxel_slot]; }
	Vector3i get_froxel_fog_size() const { return froxel_fog_size; }
	bool is_froxel_history_valid() const { return froxel_history_valid; }
	uint32_t get_froxel_history_frames() const { return froxel_history_frames; }
	bool has_froxel_prev_cam() const { return froxel_has_prev_cam; }
	const Transform3D &get_froxel_prev_cam() const { return froxel_prev_cam; }
	const Projection &get_froxel_prev_view_proj() const { return froxel_prev_view_proj; }
	void advance_froxel_history(const Transform3D &p_cam, const Projection &p_view_proj, bool p_reset_frames) {
		froxel_slot = 1u - froxel_slot;
		froxel_prev_cam = p_cam;
		froxel_prev_view_proj = p_view_proj;
		froxel_has_prev_cam = true;
		froxel_history_valid = true;
		froxel_history_frames = p_reset_frames ? 1u : MIN(froxel_history_frames + 1u, 16u);
	}
	void invalidate_froxel_history() {
		froxel_history_valid = false;
		froxel_history_frames = 0;
		froxel_has_prev_cam = false;
	}

	RID get_front_layer_albedo_texture() const { return front_layer_albedo_tex; }
	RID get_front_layer_material_texture() const { return front_layer_material_tex; }
	uint64_t get_froxel_allocated_bytes() const { return froxel_bytes; }

	void reset_guiding();
	void reset_denoise_history();
	void reset_reference();

	Size2i get_internal_size() const { return internal_size; }
	Size2i get_sample_grid_size() const { return sample_grid_size; }
	Size2i get_guiding_tile_dims() const { return guiding_tile_dims; }
	uint32_t get_samples_per_pixel() const { return samples_per_pixel; }
	uint32_t get_downsample_mode() const { return downsample_mode; }

	// Clean buffer contracts for #146 (visibility chain), #135 (denoiser), #143 (composite), and #206 (DLSS-RR).
	RID get_sample_buffer() const { return sample_buffer; }
	RID get_visibility_buffer() const { return visibility_buffer; }
	RID get_hit_distance_texture() const { return hit_distance_texture; }
	RID get_current_guiding_buffer() const { return guiding_buffers[guiding_write_index]; }
	RID get_previous_guiding_buffer() const { return guiding_buffers[1u - guiding_write_index]; }
	bool has_guiding_history() const { return guiding_has_history; }
	void advance_guiding_history();

	RID get_shade_sample_diffuse() const { return shade_sample_diffuse; }
	RID get_shade_sample_specular() const { return shade_sample_specular; }
	RID get_noisy_diffuse_texture() const { return noisy_diffuse; }
	RID get_noisy_specular_texture() const { return noisy_specular; }
	RID get_denoised_diffuse_texture() const { return denoise_output_diffuse; }
	RID get_denoised_specular_texture() const { return denoise_output_specular; }
	RID get_denoise_debug_texture() const { return denoise_debug_texture; }
	RID get_denoise_params_ubo() const { return denoise_params_ubo; }
	RID get_denoise_diffuse_slot(uint32_t p_slot) const { return denoise_diffuse[p_slot & 1u]; }
	RID get_denoise_specular_slot(uint32_t p_slot) const { return denoise_specular[p_slot & 1u]; }
	RID get_denoise_scratch_diffuse() const { return denoise_scratch_diffuse; }
	RID get_denoise_scratch_specular() const { return denoise_scratch_specular; }
	RID get_denoise_moments_slot(uint32_t p_slot) const { return denoise_moments[p_slot & 1u]; }
	RID get_denoise_history_len_slot(uint32_t p_slot) const { return denoise_history_len[p_slot & 1u]; }
	RID get_denoise_prev_depth_normal() const { return denoise_prev_depth_normal; }
	RID get_denoise_clamp_stats_buffer() const { return denoise_clamp_stats_buffer; }
	RID get_denoise_reset_mask_buffer() const { return denoise_reset_mask_buffer; }
	Size2i get_denoise_reset_tile_dims() const { return denoise_reset_tile_dims; }
	uint32_t get_denoise_history_index() const { return denoise_history_index; }
	bool has_denoise_history() const { return denoise_has_history; }
	uint64_t get_denoise_last_frame() const { return denoise_last_frame; }
	bool has_denoise_cam_transform() const { return denoise_has_cam_transform; }
	const Transform3D &get_denoise_last_cam_transform() const { return denoise_last_cam_transform; }
	bool is_denoise_rr_bypass() const { return denoise_last_rr_bypass; }
	void set_denoise_output(RID p_diffuse, RID p_specular, bool p_rr_bypass) {
		denoise_output_diffuse = p_diffuse;
		denoise_output_specular = p_specular;
		denoise_last_rr_bypass = p_rr_bypass;
		last_denoise_stats.rr_bypass = p_rr_bypass;
	}
	void advance_denoise_history(uint64_t p_frame, const Transform3D &p_cam_transform, bool p_bypassed);
	void poll_denoise_stats();
	void submit_denoise_stats_readback();
	void record_denoise_stats(uint32_t p_removed_fp, uint32_t p_total_fp, uint32_t p_clamped_pixels, uint32_t p_valid_pixels, bool p_rr_bypass);
	const DenoiseStats &get_denoise_stats() const { return last_denoise_stats; }

	RID get_reference_accum_texture() const { return reference_accum_texture; }
	uint32_t get_reference_frame_count() const { return reference_frame_count; }
	uint64_t get_reference_camera_hash() const { return reference_camera_hash; }
	void set_reference_state(uint32_t p_frame_count, uint64_t p_camera_hash) {
		reference_frame_count = p_frame_count;
		reference_camera_hash = p_camera_hash;
	}

	RID get_verify_accum_texture() const { return verify_accum_texture; }
	RID get_verify_counts_buffer() const { return verify_counts_buffer; }

	RID get_params_ubo() const { return params_ubo; }
	RID get_layer_group_buffer() const { return layer_group_buffer; }
	RID get_light_shadow_policy_buffer() const { return light_shadow_policy_buffer; }

	RID get_vis_params_ubo() const { return vis_params_ubo; }
	RID get_vis_counters_buffer() const { return vis_counters_buffer; }
	RID get_vis_indirect_buffer() const { return vis_indirect_buffer; }
	RID get_vis_vsm_params_buffer() const { return vis_vsm_params_buffer; }
	RID get_vis_ray_list_buffer() const { return vis_ray_list_buffer; }
	RID get_vis_vsm_list_buffer() const { return vis_vsm_list_buffer; }
	RID get_vis_sample_ray_map_buffer() const { return vis_sample_ray_map_buffer; }
	uint32_t get_vis_list_capacity() const { return vis_list_capacity; }
	RID get_vis_retrace_buffer() const { return vis_retrace_buffer; }

	uint32_t get_frame_index() const { return frame_index; }
	void set_frame_index(uint32_t p_frame) { frame_index = p_frame; }
	void advance_frame_index() { frame_index++; }
	uint64_t get_allocated_bytes() const { return tracked_total_bytes; }

	MegaLightsViewData();
	virtual ~MegaLightsViewData();
};

class MegaLights {
public:
	// Project settings keys (registered in RenderingServer, documented in ProjectSettings.xml).
	// GodotNext: megalights_frame (#143, plan §21.3 contract M1, M6, GM-9).
	static constexpr const char *SETTING_ENABLED = "rendering/megalights/enabled";
	static constexpr const char *SETTING_SAMPLES_PER_PIXEL = "rendering/megalights/samples_per_pixel";
	static constexpr const char *SETTING_DOWNSAMPLE = "rendering/megalights/downsample";
	static constexpr const char *SETTING_DIRECTIONAL_LIGHTS = "rendering/megalights/directional_lights";
	static constexpr const char *SETTING_FALLOFF_CULL_THRESHOLD = "rendering/megalights/falloff_cull_threshold";
	static constexpr const char *SETTING_REFERENCE_MODE = "rendering/megalights/reference_mode";
	// GodotNext: megalights_visibility (#146, plan §21.3 contract M4).
	static constexpr const char *SETTING_SCREEN_TRACES = "rendering/megalights/screen_traces";
	static constexpr const char *SETTING_DISTANT_SCREEN_TRACES_LENGTH = "rendering/megalights/distant_screen_traces_length";
	static constexpr const char *SETTING_FAR_FIELD = "rendering/megalights/far_field";
	static constexpr const char *SETTING_SWRT_CONTINUATION = "rendering/megalights/swrt_continuation";
	// GodotNext: megalights_denoiser (#135, plan §21.3 contract M5).
	static constexpr const char *SETTING_DENOISER = "rendering/megalights/denoiser";
	// GodotNext: megalights_fog_translucency (#137, plan §21.3 contract M7, GM-6, budget M-8).
	static constexpr const char *SETTING_FRONT_LAYER_TRANSLUCENCY = "rendering/megalights/front_layer_translucency";

	enum DownsampleMode : uint32_t {
		DOWNSAMPLE_FULL = 0,
		DOWNSAMPLE_HALF = 1,
		DOWNSAMPLE_AUTO = 2,
	};

	enum DenoiserMode : uint32_t {
		DENOISER_BUILTIN = 0,
		DENOISER_RR_BYPASS = 1,
	};

	enum LightType : uint32_t {
		LIGHT_TYPE_OMNI = 0,
		LIGHT_TYPE_SPOT = 1,
		LIGHT_TYPE_AREA = 2,
		LIGHT_TYPE_DIRECTIONAL = 3,
	};

	// GodotNext: megalights_visibility (#146, plan §21.3 contract M4, GM-8).
	enum VisibilityTier : uint32_t {
		VISIBILITY_TIER_DISABLED = 0,
		VISIBILITY_TIER_HWRT = 1,
		VISIBILITY_TIER_HWRT_INLINE_ONLY = 2, // Mac Apple M2+ Experimental (#171)
		VISIBILITY_TIER_SWRT_EXTENSION = 3, // Non-RT tier global SDF continuation extension
		VISIBILITY_TIER_SCREEN_ONLY = 4,
	};

	static constexpr bool DEFAULT_ENABLED = false;
	static constexpr uint32_t DEFAULT_SAMPLES_PER_PIXEL = 4u;
	static constexpr uint32_t MIN_SAMPLES_PER_PIXEL = 1u;
	static constexpr uint32_t MAX_SAMPLES_PER_PIXEL = 16u;
	static constexpr DownsampleMode DEFAULT_DOWNSAMPLE = DOWNSAMPLE_HALF;
	static constexpr bool DEFAULT_DIRECTIONAL_LIGHTS = false;
	static constexpr float DEFAULT_FALLOFF_CULL_THRESHOLD = 0.001f;
	static constexpr bool DEFAULT_REFERENCE_MODE = false;
	static constexpr bool DEFAULT_SCREEN_TRACES = true;
	static constexpr float DEFAULT_DISTANT_SCREEN_TRACES_LENGTH = 4.0f;
	static constexpr float DEFAULT_NEAR_SCREEN_TRACE_LENGTH = 1.0f;
	static constexpr float DEFAULT_DISTANT_SCREEN_TRACE_START_DEPTH = 16.0f;
	static constexpr bool DEFAULT_FAR_FIELD = false;
	static constexpr bool DEFAULT_SWRT_CONTINUATION = false;
	static constexpr DenoiserMode DEFAULT_DENOISER = DENOISER_BUILTIN;
	static constexpr bool DEFAULT_FRONT_LAYER_TRANSLUCENCY = false;
	static constexpr float SSS_TRANSMIT_BOOST = 0.2f;
	static constexpr float SSS_EXTINCTION_DEFAULT = 4.0f;
	static constexpr float D17_VOLUME_NEAR = 0.05f;
	static constexpr float D17_VOLUME_FAR = 64.0f;
	static constexpr uint32_t D17_VOLUME_SLICES = 32u;
	static constexpr float DEMOD_ALBEDO_FLOOR = 0.01f;
	static constexpr float DEMOD_SPECULAR_FLOOR = 0.01f;
	static constexpr uint32_t DENOISE_RESET_TILE_SIZE = 16u;
	static constexpr uint32_t VSM_SMRT_RAYS = 2u;
	static constexpr float HIT_DISTANCE_MISS = 65504.0f;

	static constexpr uint32_t SAMPLE_INDEX_MASK = 0xFFFFu;
	static constexpr uint32_t SAMPLE_TYPE_SHIFT = 16u;
	static constexpr uint32_t SAMPLE_TYPE_MASK = 0x3u;
	static constexpr uint32_t SAMPLE_INVALID = 0xFFFFFFFFu;

	static constexpr uint32_t GUIDING_TILE_SIZE = 8u;
	static constexpr uint32_t GUIDING_MAX_LIGHTS = 32u;
	static constexpr uint32_t GUIDING_HEADER_WORDS = 4u;
	static constexpr uint32_t GUIDING_TILE_WORDS = 36u;
	static constexpr uint32_t GUIDING_LIGHT_MASK = 0x3FFFFu;
	static constexpr uint32_t GUIDING_COUNT_SHIFT = 18u;
	static constexpr uint32_t GUIDING_COUNT_MAX = 16383u;
	// Strictly positive minimum weight floor for occluded/unlisted lights so WRS remains unbiased.
	static constexpr float DEFAULT_GUIDING_FLOOR = 0.25f;

	static constexpr uint32_t STBN_WIDTH = 64u;
	static constexpr uint32_t STBN_HEIGHT = 64u;
	static constexpr uint32_t STBN_DEPTH = 32u;
	static constexpr uint32_t STBN_CHANNELS = 2u;

	enum ParamFlags : uint32_t {
		PARAM_FLAG_DIRECTIONAL_LIGHTS = 1u << 0u,
		PARAM_FLAG_ORTHOGONAL = 1u << 1u,
		PARAM_FLAG_HAS_GUIDING_HISTORY = 1u << 2u,
		PARAM_FLAG_USE_LIGHT_FUNCTION = 1u << 3u,
		PARAM_FLAG_FORCE_VISIBLE_ONE = 1u << 4u,
		PARAM_FLAG_USE_SOFT_SHADOWS = 1u << 5u,
	};

	enum GuidingFlags : uint32_t {
		GUIDING_FLAG_PHASE_MASK = 3u,
		GUIDING_FLAG_DOWNSAMPLE_HALF = 1u << 2u,
		GUIDING_FLAG_FORCE_VISIBLE_ONE = 1u << 3u,
	};

	// GodotNext: megalights_visibility (#146). Policy and visibility flags matching ml_screen_trace.glsl.
	enum PolicyFlags : uint32_t {
		POLICY_FLAG_CASTS_SHADOW = 1u << 0u,
		POLICY_FLAG_USE_VSM = 1u << 1u,
		POLICY_FLAG_FULL_CASTER_MASK = 1u << 2u,
	};

	enum VisFlags : uint32_t {
		VIS_FLAG_SCREEN_TRACES = 1u << 0u,
		VIS_FLAG_DEDUPE = 1u << 1u,
		VIS_FLAG_ORTHOGONAL = 1u << 2u,
		VIS_FLAG_FAR_FIELD = 1u << 3u,
		VIS_FLAG_SWRT = 1u << 4u,
		VIS_FLAG_VSM_ENABLED = 1u << 5u,
		VIS_FLAG_NO_RT_FALLBACK = 1u << 6u,
		VIS_FLAG_USE_HIZ = 1u << 7u,
	};

	struct QualityPreset {
		uint32_t samples_per_pixel = DEFAULT_SAMPLES_PER_PIXEL;
		DownsampleMode downsample = DEFAULT_DOWNSAMPLE;
		uint32_t screen_trace_steps = 16u;
	};

	struct Settings {
		bool enabled = DEFAULT_ENABLED;
		uint32_t samples_per_pixel = DEFAULT_SAMPLES_PER_PIXEL;
		DownsampleMode downsample = DEFAULT_DOWNSAMPLE;
		uint32_t screen_trace_steps = 16u;
		bool directional_lights = DEFAULT_DIRECTIONAL_LIGHTS;
		float falloff_cull_threshold = DEFAULT_FALLOFF_CULL_THRESHOLD;
		bool reference_mode = DEFAULT_REFERENCE_MODE;
		bool screen_traces = DEFAULT_SCREEN_TRACES;
		float distant_screen_traces_length = DEFAULT_DISTANT_SCREEN_TRACES_LENGTH;
		bool far_field = DEFAULT_FAR_FIELD;
		bool swrt_continuation = DEFAULT_SWRT_CONTINUATION;
		DenoiserMode denoiser = DEFAULT_DENOISER;
		bool front_layer_translucency = DEFAULT_FRONT_LAYER_TRANSLUCENCY;
	};

	// GodotNext: megalights_fog_translucency (#137, plan §21.3 contract M7).
	enum FroxelFlags : uint32_t {
		FROXEL_FLAG_FOG_ACTIVE = 1u << 0u,
		FROXEL_FLAG_D17_DIRECT_WRITE = 1u << 1u,
		FROXEL_FLAG_WRITE_SH = 1u << 2u,
		FROXEL_FLAG_HISTORY_VALID = 1u << 3u,
		FROXEL_FLAG_USE_LIGHT_FUNCTION = 1u << 4u,
		FROXEL_FLAG_USE_VSM = 1u << 5u,
		FROXEL_FLAG_DIRECTIONAL_LIGHTS = 1u << 6u,
		FROXEL_FLAG_ORTHOGONAL = 1u << 7u,
		FROXEL_FLAG_FORCE_VISIBLE_ONE = 1u << 8u,
	};

	// std140, 400 bytes. Must match MLFroxelParams in ml_froxel_inc.glsl.
	struct FroxelParamsUBO {
		float inv_view_rows[12] = {};
		float to_prev_view_rows[12] = {};
		float inv_view_proj[16] = {};
		float prev_view_proj[16] = {};
		float depth_coeffs[4] = {};
		float fog_frustum_size[4] = {};
		float fog_z_params[4] = {};
		float d17_params[4] = {};
		uint32_t grid_dims[4] = {};
		uint32_t d17_dims[4] = {};
		uint32_t fog_dims_pad[4] = {};
		uint32_t light_counts[4] = {};
		uint32_t light_grid_dims[4] = {};
		float light_grid_z_params[4] = {};
		uint32_t screen_frame_vsm[4] = {};
	};
	static_assert(sizeof(FroxelParamsUBO) == 400, "MegaLights::FroxelParamsUBO must match MLFroxelParams (400 bytes).");
	static_assert(sizeof(FroxelParamsUBO) % 16 == 0, "MegaLights::FroxelParamsUBO must stay 16-byte aligned.");

	struct FrontLayerPushConstant {
		uint32_t size[2] = {};
		uint32_t pad0 = 0;
		uint32_t pad1 = 0;
	};
	static_assert(sizeof(FrontLayerPushConstant) == 16, "MegaLights::FrontLayerPushConstant must match ml_front_layer.glsl.");

	struct FroxelInputs {
		RenderDataRD *render_data = nullptr;
		NextGI *next_gi = nullptr;
		LightFunctionAtlas *light_function_atlas = nullptr;
		bool fog_active = false;
		bool translucency_active = false;
		Vector3i fog_size;
		float fog_end = 64.0f;
		float fog_detail_spread = 2.0f;
		float fog_phase_g = 0.0f;
		bool flip_y = true;
		bool vsm_sample_view = false;
		RID vsm_default_pool;
		RID vsm_default_buffer;
		RID vsm_default_miss_buffer;
		int quality_level = 2;
		bool reset_temporal_history = false;
	};

	struct FroxelResult {
		RID fog_light_texture;
		RID d17_direct_r;
		RID d17_direct_g;
		RID d17_direct_b;
		bool fog_active = false;
		bool d17_active = false;
	};

	struct FrontLayerResult {
		RID diffuse_texture;
		RID specular_texture;
		bool active = false;
	};

	// std140, 272 bytes. Must match MLVisParams in ml_screen_trace.glsl / ml_trace_*.glsl.
	struct VisParamsUBO {
		float inv_projection[16] = {};
		float depth_projection[16] = {};
		float inv_view_rows[12] = {};
		uint32_t screen_sample_size[4] = {};
		uint32_t sample_config[4] = {};
		uint32_t light_counts[4] = {};
		float screen_trace_params[4] = {};
		uint32_t trace_config[4] = {};
		float rt_params[4] = {};
	};
	static_assert(sizeof(VisParamsUBO) == 272, "MegaLights::VisParamsUBO must match MLVisParams (272 bytes).");
	static_assert(sizeof(VisParamsUBO) % 16 == 0, "MegaLights::VisParamsUBO must stay 16-byte aligned.");

	// std430, 64 bytes. Must match VSMProjectionParams in ml_trace_vsm.glsl.
	struct VisVSMProjectionParams {
		float footprint[4] = {};
		int32_t bias[4] = {};
		uint32_t counts[4] = {};
		uint32_t misc[4] = {};
	};
	static_assert(sizeof(VisVSMProjectionParams) == 64, "MegaLights::VisVSMProjectionParams must match VSMProjectionParams.");

	struct VisibilityOptions {
		bool override_screen_traces = false;
		bool screen_traces = DEFAULT_SCREEN_TRACES;
		bool dedupe_enabled = true;
		bool override_far_field = false;
		bool far_field = DEFAULT_FAR_FIELD;
		bool override_swrt = false;
		bool swrt = DEFAULT_SWRT_CONTINUATION;
		bool force_no_vsm = false;
		bool use_explicit_frame_index = false;
		uint32_t frame_index = 0;
	};

	struct TierInputs {
		bool forward_plus = false;
		bool supports_ray_query = false;
		bool supports_ray_tracing_pipeline = false;
		bool is_metal = false;
		bool is_apple_m2_or_newer = false;
		bool nextgi_hwrt_available = false;
		bool df_sdf_available = false;
		bool swrt_continuation_requested = false;
		bool vsm_available = false;
		bool far_field_requested = false;
		bool screen_traces_enabled = DEFAULT_SCREEN_TRACES;
	};

	struct TierStatus {
		VisibilityTier tier = VISIBILITY_TIER_DISABLED;
		bool experimental = false;
		bool inline_ray_query_only = false;
		bool retrace_supported = false;
		bool far_field_supported = false;
		bool swrt_extension = false;
		bool vsm_supported = false;
		const char *tier_name = "UNAVAILABLE";
		const char *backend = "none";
		const char *status_label = "Unavailable";
		const char *status_warning = "";
		const char *experimental_label = "";
		const char *reason = "";
	};

	// CPU reference structures for 2x2 block ray deduplication & stream compaction doctests.
	struct DedupeBlockSample {
		uint32_t light_packed = SAMPLE_INVALID;
		float linear_depth = -1.0f;
		float depth_tol = 0.0f;
		Vector3 world_normal = Vector3(0.0f, 1.0f, 0.0f);
		Vector3 ray_dir = Vector3(0.0f, 1.0f, 0.0f);
		float t_resume = 0.0f;
		bool casts_shadow = true;
		bool use_vsm = false;
		bool screen_hit = false;
	};

	struct DedupeBlockResult {
		uint32_t ray_count = 0;
		uint32_t deduped_count = 0;
		uint32_t vsm_count = 0;
		uint32_t screen_hit_count = 0;
		uint32_t unshadowed_count = 0;
		LocalVector<uint32_t> sample_ray_map;
		LocalVector<float> ray_min_t_resume;
	};

	// Packed 16-byte sample matching MLSample (uvec4) in ml_generate_samples.glsl.
	struct SampleGPU {
		uint32_t light_packed = SAMPLE_INVALID;
		float inv_pdf = 0.0f;
		uint32_t ray_dir_oct = 0u;
		float max_distance = 0.0f;
	};
	static_assert(sizeof(SampleGPU) == 16, "MegaLights::SampleGPU must be 16 bytes (uvec4).");
	static_assert(sizeof(SampleGPU) % 16 == 0, "MegaLights::SampleGPU must stay 16-byte aligned.");

	// std140, 256 bytes. Must match MLSceneParams in ml_light_eval_inc.glsl.
	struct ParamsUBO {
		float inv_projection[16] = {};
		float inv_view_rows[12] = {};
		uint32_t screen_sample_size[4] = {};
		uint32_t grid_dims_flags[4] = {};
		float grid_z_params[4] = {};
		uint32_t light_counts[4] = {};
		uint32_t sample_config[4] = {};
		uint32_t guiding_tile_info[4] = {};
		float thresholds[4] = {};
		uint32_t pad_extra[4] = {};
		uint32_t pad_extra2[4] = {};
	};
	static_assert(sizeof(ParamsUBO) == 256, "MegaLights::ParamsUBO must match MLSceneParams (256 bytes).");
	static_assert(sizeof(ParamsUBO) % 16 == 0, "MegaLights::ParamsUBO must stay 16-byte aligned.");

	// Push constant for ml_guiding.glsl (32 bytes).
	struct GuidingPushConstant {
		uint32_t full_size[2] = {};
		uint32_t sample_size[2] = {};
		uint32_t tile_dims[2] = {};
		uint32_t samples_per_pixel = 0;
		uint32_t flags = 0;
	};
	static_assert(sizeof(GuidingPushConstant) == 32, "MegaLights::GuidingPushConstant must match ml_guiding.glsl.");

	// Inputs supplied per view by RenderForwardClustered (or #143/#146 orchestrator).
	struct FrameInputs {
		RenderDataRD *render_data = nullptr;
		RID depth_texture;
		RID normal_roughness_texture;
		RID ml_albedo_texture;
		RID ml_material_texture;
		RID dfg_lut_texture;
		RID ltc_lut1_texture;
		RID ltc_lut2_texture;
		LightFunctionAtlas *light_function_atlas = nullptr;
		NextGI *next_gi = nullptr;
		bool flip_y = true;
		bool use_soft_shadows = true;
		// GodotNext: megalights_visibility (#146, plan §21.3 contract M4).
		RID hiz_texture;
		uint32_t hiz_mip_count = 1;
		NextGISWRTTrace *swrt_trace = nullptr;
		NextGIVoxelLighting *voxel_lighting = nullptr;
		RID vsm_blue_noise_texture;
		bool vsm_sample_view = false;
		float surface_thickness = 0.01f;
		int quality_level = 2;
		// GodotNext: megalights_denoiser (#135, plan §21.3 contract M5).
		RID velocity_texture;
		bool velocity_enabled = false;
		bool reset_temporal_history = false;
		RSE::ViewportDebugDraw debug_draw = RSE::VIEWPORT_DEBUG_DRAW_DISABLED;
		Vector<Rect2i> dirty_reset_regions;
	};

	// --- Pure, device-free helpers (tested by CPU doctests) ---

	static bool is_enabled();
	static bool is_front_layer_enabled();
	static bool effective_directional_lights();
	static int read_nextgi_quality_level();
	static QualityPreset quality_preset_for_level(int p_quality_level);
	static DownsampleMode downsample_for_quality_preset(int p_quality_level);
	static bool light_uses_megalights(bool p_megalights_active, RSE::LightMegaLightsMode p_mode, RSE::LightType p_type, bool p_directional_lights_enabled);
	static uint64_t get_total_allocated_bytes();
	static uint64_t get_froxel_allocated_bytes();
	static uint64_t get_front_layer_allocated_bytes();
	static uint64_t get_d17_direct_allocated_bytes();
	static Settings read_settings();
	static Settings validate_settings(const Settings &p_settings);
	// Provisional sample count per quality preset (Low=2, Medium=2, High=4, Epic=8; tuned in PQ).
	static uint32_t samples_for_quality_preset(int p_quality_level);
	// GodotNext: megalights_fog_translucency (#137, plan §21.3 contract M7). Shared froxel samples per cell (Low/Med/High=1, Epic=2).
	static uint32_t froxel_samples_for_quality_preset(int p_quality_level);
	static void sh_l1_project_directional(const Vector3 &p_dir_ws, const Color &p_radiance, Vector4 &r_sh_r, Vector4 &r_sh_g, Vector4 &r_sh_b);
	static Color sh_l1_evaluate(const Vector4 &p_sh_r, const Vector4 &p_sh_g, const Vector4 &p_sh_b, const Vector3 &p_normal_ws);
	static float sss_transmission_lobe(float p_ndotl, float p_boost = SSS_TRANSMIT_BOOST);
	static float sss_transmission_attenuation(float p_hit_dist, float p_visibility, float p_extinction = SSS_EXTINCTION_DEFAULT);
	static uint64_t estimate_d17_direct_bytes(bool p_megalights_enabled, const Size2i &p_screen_size);
	static uint64_t estimate_front_layer_bytes(bool p_megalights_enabled, bool p_front_layer_enabled, const Size2i &p_screen_size, uint32_t p_spp, DownsampleMode p_downsample);
	static DownsampleMode parse_downsample_variant(const Variant &p_value);
	static DenoiserMode parse_denoiser_variant(const Variant &p_value);
	static int debug_vis_mode_from_viewport(RSE::ViewportDebugDraw p_mode);

	static Vector2i downsample_offset(uint32_t p_frame_index);
	static Size2i compute_sample_grid_size(const Size2i &p_internal_size, DownsampleMode p_downsample);
	static Vector2i sample_to_full_pixel(const Vector2i &p_sample_px, const Size2i &p_full_size, DownsampleMode p_downsample, uint32_t p_frame_index);
	static Size2i compute_guiding_tile_dims(const Size2i &p_full_size);

	static uint32_t pack_sample_light(uint32_t p_type, uint32_t p_index);
	static uint32_t unpack_sample_light_type(uint32_t p_packed);
	static uint32_t unpack_sample_light_index(uint32_t p_packed);

	static uint32_t pack_guiding_entry(uint32_t p_light_packed, uint32_t p_count);
	static uint32_t unpack_guiding_light(uint32_t p_entry);
	static uint32_t unpack_guiding_count(uint32_t p_entry);

	static uint32_t pack_oct_snorm16(const Vector3 &p_dir);
	static Vector3 unpack_oct_snorm16(uint32_t p_word);

	static float omni_attenuation(float p_distance, float p_inv_range, float p_decay);
	static bool passes_falloff_cull(float p_attenuation, float p_threshold);
	static float guiding_weight(uint32_t p_visible_count, uint32_t p_max_tile_count, bool p_has_tile_history, float p_floor = DEFAULT_GUIDING_FLOOR);

	// CPU reference guiding aggregator for one 8x8 tile (matching ml_guiding.glsl top-32 sort).
	struct GuidingEntry {
		uint32_t light_packed = SAMPLE_INVALID;
		uint32_t count = 0;
	};
	static void aggregate_guiding_tile_cpu(const uint32_t *p_visible_lights, uint32_t p_count, LocalVector<GuidingEntry> &r_top32);

	// CPU reference 1-pass K-reservoir weighted sampler (matching ml_generate_samples.glsl).
	struct ReservoirSample {
		uint32_t light_index = SAMPLE_INVALID;
		float inv_pdf = 0.0f;
	};
	static void sample_reservoirs_cpu(const float *p_weights, uint32_t p_light_count, uint32_t p_k, uint64_t &r_rng_state, LocalVector<ReservoirSample> &r_out);

	// Welford relative standard error helper matching ml_reference.glsl.
	static float welford_relative_std_error(float p_mean_luma, float p_m2, uint32_t p_sample_count);

	// GodotNext: megalights_visibility (#146). Pure device-free visibility chain helpers.
	static uint32_t screen_trace_steps_for_quality(int p_quality_level);
	static float screen_trace_max_distance(float p_linear_depth, float p_near_length, float p_distant_length, float p_distant_start_depth = DEFAULT_DISTANT_SCREEN_TRACE_START_DEPTH);
	static float screen_trace_surface_thickness(float p_linear_depth, float p_surface_thickness_setting);
	static float dedupe_depth_tolerance(float p_linear_depth, float p_normal_vs_z, float p_proj_y, uint32_t p_full_height, bool p_downsample_half, bool p_orthogonal);
	static bool can_dedupe_ray(uint32_t p_light_a, uint32_t p_light_b, float p_depth_a, float p_depth_b, float p_tol_a, float p_tol_b, const Vector3 &p_normal_a, const Vector3 &p_normal_b, const Vector3 &p_dir_a, const Vector3 &p_dir_b);
	static DedupeBlockResult compact_and_dedupe_2x2_cpu(const DedupeBlockSample *p_samples, uint32_t p_spp, bool p_dedupe_enabled);
	static bool light_uses_vsm(RSE::LightMegaLightsShadowMethod p_method, bool p_vsm_available);
	static TierStatus resolve_visibility_tier(const TierInputs &p_inputs);
	static Dictionary get_tier_info();
	static uint32_t pack_visibility_rgba8(float p_r, float p_g, float p_b, float p_a = 1.0f);
	static Color unpack_visibility_rgba8(uint32_t p_packed);

	// GodotNext: megalights_denoiser (#135). Pure device-free demodulation, firefly clamp, and cut helpers.
	static Vector3 diffuse_demod_factor(const Vector3 &p_diffuse_albedo);
	static Vector3 env_brdf_split_sum(const Vector3 &p_f0, float p_dfg_x, float p_dfg_y);
	static Vector3 env_brdf_approx(const Vector3 &p_f0, float p_roughness, float p_nov);
	static Vector3 specular_demod_factor(const Vector3 &p_spec_env_brdf);
	static Vector3 demodulate_radiance(const Vector3 &p_radiance, const Vector3 &p_factor);
	static Vector3 remodulate_radiance(const Vector3 &p_demod, const Vector3 &p_factor);
	static double compute_clamped_energy_ratio(uint64_t p_removed_fp, uint64_t p_total_fp);
	static float firefly_clamp_factor(float p_luma, float p_limit, float &r_removed);
	static bool is_camera_cut(const Transform3D &p_prev, const Transform3D &p_cur);

	// Shader variant lists for NextGIBakeShaders and runtime compilation.
	static Vector<String> get_screen_trace_shader_variant_defines();
	static Vector<String> get_trace_rays_shader_variant_defines();
	static Vector<String> get_trace_rays_swrt_shader_variant_defines();
	static Vector<String> get_trace_vsm_shader_variant_defines();
	static Vector<String> get_shade_samples_shader_variant_defines();
	static Vector<String> get_froxel_shader_variant_defines();
	static Vector<String> get_froxel_compute_shader_variant_defines();
	static Vector<String> get_front_layer_shader_variant_defines();

	// Spatio-temporal blue-noise table accessors (64x64x32 RG8 committed data).
	static const uint8_t *get_stbn_data();
	static uint32_t get_stbn_byte_size();

	// Per-view state accessors (owned by RenderSceneBuffersRD under RB_SCOPE_MEGALIGHTS / RB_SCOPE_MEGALIGHTS_FRONT_LAYER).
	static Ref<MegaLightsViewData> get_view_data(RenderSceneBuffersRD *p_rb);
	static Ref<MegaLightsViewData> ensure_view_data(RenderSceneBuffersRD *p_rb);
	static Ref<MegaLightsViewData> get_front_layer_view_data(RenderSceneBuffersRD *p_rb);
	static Ref<MegaLightsViewData> ensure_front_layer_view_data(RenderSceneBuffersRD *p_rb);
	static void free_front_layer_view_data(RenderSceneBuffersRD *p_rb);

	// GPU verification check hook (matching LightGrid / VSM check pattern).
	static void request_check(const Dictionary &p_options = Dictionary());
	static bool has_pending_check();
	static bool check_wants_far_field();
	static bool check_wants_swrt();
	static bool check_wants_directional_lights();
	static Dictionary get_check_result();

private:
	struct GPUState;
	GPUState *gpu = nullptr;

	bool _ensure_common_shaders();
	bool _ensure_rt_reference_shader(NextGI *p_next_gi, bool p_use_alpha_textures);
	bool _ensure_screen_trace_shaders();
	bool _ensure_trace_rays_shaders(NextGI *p_next_gi);
	bool _ensure_trace_rays_swrt_shader();
	bool _ensure_trace_vsm_shader();
	bool _ensure_shade_shaders();
	bool _ensure_froxel_rt_shader(NextGI *p_next_gi);
	bool _ensure_froxel_compute_shaders();
	bool _ensure_front_layer_shader();
	NextGIDenoise *_ensure_denoiser();
	void _ensure_stbn_texture();
	void _fill_params_ubo(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, const Settings &p_settings, uint32_t p_frame_index, uint32_t p_ref_frame_index, bool p_has_guiding, bool p_force_visible_one, float p_falloff_override, ParamsUBO &r_ubo) const;
	void _upload_layer_group_masks(const Ref<MegaLightsViewData> &p_view) const;
	void _upload_light_shadow_policies(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, bool p_include_directional, bool p_allow_vsm = true) const;
	void _build_common_uniforms(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, Vector<RD::Uniform> &r_uniforms) const;
	void _dispatch_generate(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, const Settings &p_settings, uint32_t p_frame_index, bool p_has_guiding, float p_falloff_override);
	void _dispatch_visibility_chain(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, const Settings &p_settings, const VisibilityOptions &p_opts, bool *r_retrace_ran = nullptr);
	void _dispatch_guiding(const Ref<MegaLightsViewData> &p_view, RID p_visibility_buffer, uint32_t p_frame_index, bool p_force_visible_one);
	void _dispatch_shade(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, const Settings &p_settings, uint32_t p_frame_index, bool p_force_visible_one, float p_falloff_override);
	void _dispatch_denoise(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, uint32_t p_frame_index, bool p_rr_bypass, int p_debug_vis_mode, bool p_force_reset, const Vector<Rect2i> &p_dirty_regions, bool p_submit_async_readback, bool p_in_check_loop = false);
	void _dispatch_reference_unshadowed(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, const Settings &p_settings, uint32_t p_frame_index, uint32_t p_ref_n, float p_falloff_override);
	void _dispatch_reference_rt(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, const Settings &p_settings, uint32_t p_frame_index, uint32_t p_ref_n, bool p_force_visible_one, float p_falloff_override);
	void _dispatch_verify_samples(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, const Settings &p_settings, uint32_t p_frame_index, uint32_t p_step_n, float p_falloff_override);
	Dictionary _run_check(const FrameInputs &p_inputs, const Ref<MegaLightsViewData> &p_view, const Dictionary &p_options);

public:
	// Stage 1 (M3): generate K weighted light samples per sampling pixel into view->get_sample_buffer().
	void generate_samples(const FrameInputs &p_inputs, Ref<RenderSceneBuffersRD> p_rb);

	// Stage 2 (M4): visibility chain (screen trace -> 2x2 dedupe & compaction -> HWRT/SWRT + VSM -> scatter).
	void trace_visibility(const FrameInputs &p_inputs, Ref<RenderSceneBuffersRD> p_rb);

	// Stage 3 (M3, called after #146 visibility): aggregate visible lights per 8x8 tile for next frame.
	void update_guiding(Ref<RenderSceneBuffersRD> p_rb, RID p_visibility_buffer = RID(), bool p_force_visible_one = false);

	// Stage 4 (M5): shade (sample, visibility) into demodulated diffuse/specular (+ 2x2 bilateral fill if half-res).
	void shade_samples(const FrameInputs &p_inputs, Ref<RenderSceneBuffersRD> p_rb);

	// Stage 5 (M5): temporal accumulation + 2-pass variance-guided spatial denoise (or rr_bypass handoff).
	void denoise_samples(const FrameInputs &p_inputs, Ref<RenderSceneBuffersRD> p_rb);

	// Reference mode (M3, GM-1): exhaustive per-light shading with trace_shadow() accumulated across frames.
	void render_reference(const FrameInputs &p_inputs, Ref<RenderSceneBuffersRD> p_rb, bool p_force_visible_one = false);

	// Per-frame entry point invoked by RenderForwardClustered after prepass & packed light grid build.
	void render(const FrameInputs &p_inputs, Ref<RenderSceneBuffersRD> p_rb);
	void render_sampling(const FrameInputs &p_inputs, Ref<RenderSceneBuffersRD> p_rb);

	// GodotNext: megalights_fog_translucency (#137, plan §21.3 contract M7, GM-6, budget M-8).
	// Shared stochastic froxel lighting volume feeding BOTH volumetric fog and D17 translucency direct SH-L1.
	FroxelResult render_froxel(const FroxelInputs &p_inputs, Ref<RenderSceneBuffersRD> p_rb);
	// High-quality per-pixel MegaLights pass for the frontmost transparent layer with separate temporal history.
	FrontLayerResult render_front_layer(const FrameInputs &p_inputs, RID p_front_layer_depth, RID p_front_layer_normal_roughness, Ref<RenderSceneBuffersRD> p_rb);

	void free_resources();

	MegaLights();
	~MegaLights();
};

} // namespace RendererRD
