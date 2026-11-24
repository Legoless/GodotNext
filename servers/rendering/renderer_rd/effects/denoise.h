/**************************************************************************/
/*  denoise.h                                                             */
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

#include "core/math/basis.h"
#include "core/math/projection.h"
#include "core/math/vector3.h"
#include "core/math/vector4.h"
#include "core/variant/variant.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_denoise_spatial.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/ml_denoise_temporal.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_denoise_diffuse.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_denoise_shadow.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_denoise_specular.glsl.gen.h"

namespace RendererRD {

// GodotNext: NextGI diffuse denoiser (#53, D8).
//
// The gather stores 16 rays per 16x16 probe tile, four texels per axis, and the probe image's alpha
// carries the distance from the camera. All three passes walk the probe grid and keep the ray index
// inside the tile, so a texel only mixes the same ray direction of neighboring probes.
class NextGIDenoise {
public:
	enum Mode {
		MODE_PROBE_SPATIAL = 0, // 3x3 probe filter with depth, normal and hit-distance weights.
		MODE_PROBE_TEMPORAL = 1, // Per-probe reprojection, history rejection, clamp and cap.
		MODE_STABILIZE = 2, // Per-pixel, non-accumulating stabilizer after the upsample.
		MODE_MAX,
	};

	// One spatial pass. #53 step 2 asks for two iterations: call this twice with probe_radiance and
	// probe_radiance_out swapped, which leaves the result in the first texture.
	struct DiffuseParams {
		RID probe_radiance; // Ray-texel radiance read by the filter.
		RID probe_radiance_out; // Ray-texel destination; must differ from probe_radiance.
		RID depth; // Full-resolution depth, reverse-Z.
		RID normal_roughness; // Full-resolution normal (xyz, encoded) and roughness (w) guide.
		Size2i probe_size; // Ray-texel grid, four texels per axis per probe.
		Size2i screen_size;
		Projection inv_projection; // GodotNext: nextgi_view_math. NextGIView::inv_projection(): depth texture to view space.
		float depth_threshold = 0.05f; // Plane-distance falloff as a fraction of probe depth.
		float normal_power = 8.0f;
		uint32_t probe_stride = 4;
		uint32_t probe_extent = 16;
	};

	// One temporal pass: reproject the previous history, reject, clamp to the neighbourhood and cap.
	// The caller writes the result to probe_radiance_out and swaps the history textures.
	struct TemporalParams {
		RID probe_radiance; // Current frame, already spatially filtered.
		RID probe_radiance_out; // Accumulated frame; must differ from probe_radiance.
		RID probe_history; // Previous frame's accumulated radiance.
		RID depth;
		RID normal_roughness;
		RID velocity; // Prepass motion vectors (#37); a dummy texture when velocity_enabled is false.
		RID history_length; // Probe-sized r8ui image, read and written in place.
		RID reset_mask; // Storage buffer of one uint per probe; non-zero resets the probe.
		Size2i probe_size;
		Size2i screen_size;
		// GodotNext: nextgi_view_math. Both are NextGIView matrices: the depth texture's clip
		// space (reverse Z, flip_y). inv_view_proj reconstructs world positions from depth.
		Projection prev_view_proj;
		Projection inv_view_proj;
		Vector3 camera_pos;
		Vector3 prev_camera_pos;
		float depth_tolerance = 0.1f; // Fraction of the stored distance.
		float normal_tolerance = 0.9f; // Minimum dot product with the reprojected guide.
		uint32_t history_cap = 16; // 0 makes the pass a pass-through (DLSS-RR bypass hook).
		bool velocity_enabled = false;
		uint32_t probe_stride = 4;
		uint32_t probe_extent = 16;
		// Optional debug views (#53). When p_debug_views is true the pass writes per-texel
		// variance, history length and rejection, and the three RIDs must be valid and sized
		// like the probe radiance texture.
		bool debug_views = false;
		RID debug_variance;
		RID debug_history;
		RID debug_rejected;
	};

	// One stabilizer pass. It never accumulates: the previous output is clamped to the current
	// 3x3 min/max and blended with a fixed weight of at most 0.5.
	struct StabilizeParams {
		RID current; // Upsampled ambient for this frame.
		RID previous; // The previous stabilizer output.
		RID output; // Destination; must differ from current.
		Size2i screen_size;
		float blend_weight = 0.25f;
		bool stabilizer_enabled = true;
	};

	explicit NextGIDenoise(bool p_init_gi_shaders = true);
	~NextGIDenoise();

	// GodotNext: rt_shadows (#141, plan §21.3 contract M9). Per-light shadow denoiser over the
	// allocated tile-sparse R8 shadow-mask atlas: temporal reprojection with velocity + twin
	// history atlas (light-ID invalidated), 2 à-trous spatial passes guided by depth/normal/penumbra,
	// and previous-frame depth/normal guide copy.
	struct ShadowDenoiseParams {
		RID raw_mask; // Traced raw R8 mask atlas (also reused as spatial scratch).
		RID history_mask_prev; // Previous frame's temporally accumulated R8 atlas (overwritten with final spatial output).
		RID history_mask_cur; // Current frame's temporally accumulated R8 atlas.
		RID history_len_prev; // Previous frame's R8UI history length atlas.
		RID history_len_cur; // Current frame's R8UI history length atlas.
		RID hit_dist; // R16F hit distance atlas for penumbra width estimation.
		RID depth; // Resolved prepass depth (reverse-Z).
		RID normal_roughness; // Prepass normal-roughness guide.
		RID velocity; // Prepass motion vectors (#37) or dummy texture.
		RID prev_depth_normal; // Previous frame's RGBA16F (view normal, linear depth) guide.
		RID params_buffer; // RTShadowParams SSBO.
		RID lights_buffer; // RTShadowLight[] SSBO.
		RID tile_list_buffer; // Active allocated tile list uvec4[].
		RID alloc_state_buffer; // Alloc state SSBO (active_tiles).
		RID cur_lookup_buffer; // Current frame tile lookup SSBO.
		RID prev_lookup_buffer; // Previous frame tile lookup SSBO.
		RID prev_owner_buffer; // Previous frame tile owner (stable_id, packed_st) SSBO.
		RID indirect_buffer; // 16-byte indirect dispatch buffer written by MODE_FINALIZE.
		Size2i screen_size;
	};
	void shadow_denoise(const ShadowDenoiseParams &p_params);
	static Vector<String> get_shadow_shader_variant_defines();

	// GodotNext: megalights_denoiser (#135, plan §21.3 contract M5, GM-1, GM-7, budget M-5).
	// Demodulated diffuse & specular temporal accumulation with luminance moments, firefly clamp
	// + GPU clamped-energy counter, 2-iteration variance-guided à-trous spatial filter (step 1 & 2),
	// optional on-demand debug visualization (ML_NOISY, ML_DENOISED, ML_VARIANCE), and rr_bypass
	// handoff for DLSS-RR (PU+).
	enum MegaLightsDenoiseFlags : uint32_t {
		ML_DENOISE_FLAG_HAS_HISTORY = 1u << 0u,
		ML_DENOISE_FLAG_VELOCITY_ENABLED = 1u << 1u,
		ML_DENOISE_FLAG_RESET_MASK_ENABLED = 1u << 2u,
		ML_DENOISE_FLAG_ORTHOGONAL = 1u << 3u,
		ML_DENOISE_FLAG_RR_BYPASS = 1u << 4u,
	};

	// std140, 336 bytes. Must match MLDenoiseParams in ml_denoise_{temporal,spatial}.glsl.
	struct MegaLightsDenoiseParamsGPU {
		float inv_projection[16] = {};
		float inv_view_proj[16] = {};
		float prev_view_proj[16] = {};
		float view_to_world[12] = {};
		float cur_depth_coeffs[4] = {};
		float prev_depth_coeffs[4] = {};
		float camera_pos[4] = {};
		float prev_camera_pos[4] = {};
		uint32_t screen_reset_size[4] = {};
		uint32_t config[4] = {};
	};
	static_assert(sizeof(MegaLightsDenoiseParamsGPU) == 336, "NextGIDenoise::MegaLightsDenoiseParamsGPU must match MLDenoiseParams (336 bytes).");
	static_assert(sizeof(MegaLightsDenoiseParamsGPU) % 16 == 0, "NextGIDenoise::MegaLightsDenoiseParamsGPU must stay 16-byte aligned.");

	struct MegaLightsDenoiseParams {
		bool rr_bypass = false;
		bool has_history = false;
		bool velocity_enabled = false;
		bool reset_mask_enabled = false;
		bool orthogonal = false;
		Size2i screen_size;
		Size2i reset_tile_dims;
		Projection inv_projection;
		Projection inv_view_proj;
		Projection prev_view_proj;
		Basis view_to_world;
		Vector4 cur_depth_coeffs;
		Vector4 prev_depth_coeffs;
		Vector3 camera_pos;
		Vector3 prev_camera_pos;
		uint32_t frame_index = 0;
		uint32_t max_history_diffuse = 16;
		uint32_t max_history_specular = 16;

		// Noisy demodulated render-resolution inputs from ml_shade_samples.glsl.
		RID noisy_diffuse;
		RID noisy_specular;

		// Guides (render resolution).
		RID depth;
		RID normal_roughness;
		RID ml_albedo;
		RID ml_material;
		RID dfg_lut;
		RID velocity;
		RID reset_mask;
		RID prev_depth_normal;

		// Temporal + spatial working targets (only required when rr_bypass == false).
		// Note: spatial pass 2 writes final denoised outputs into history_*_prev while history_*_cur
		// keeps clean pre-spatial temporal accumulation for next frame's history.
		RID history_diffuse_prev;
		RID history_diffuse_cur;
		RID history_specular_prev;
		RID history_specular_cur;
		RID spatial_scratch_diffuse;
		RID spatial_scratch_specular;
		RID moments_prev;
		RID moments_cur;
		RID history_len_prev;
		RID history_len_cur;
		RID clamp_stats_buffer;
		RID params_ubo;

		// Optional debug visualization (-1 = disabled, 0 = ML_NOISY, 1 = ML_DENOISED, 2 = ML_VARIANCE).
		int debug_vis_mode = -1;
		RID debug_vis_output;
	};

	struct MegaLightsDenoiseOutput {
		RID diffuse;
		RID specular;
		RID variance_diffuse;
		RID variance_specular;
		bool bypassed = false;
	};

	MegaLightsDenoiseOutput megalights_denoise(const MegaLightsDenoiseParams &p_params);
	static Vector<String> get_megalights_temporal_shader_variant_defines();
	static Vector<String> get_megalights_spatial_shader_variant_defines();

	// GodotNext: nextgi_denoise_specular (#54). Joint-bilateral upsample of the specular trace from
	// its trace resolution into RB_TEX_REFLECTION.
	struct SpecularUpsampleParams {
		RID raw; // Trace-resolution radiance + confidence.
		RID depth; // Full-resolution depth.
		RID normal_roughness; // Full-resolution normal-roughness guide.
		RID output; // RB_TEX_REFLECTION at the GI buffer resolution.
		Size2i trace_size;
		Size2i screen_size;
	};
	struct SpecularTemporalParams {
		RID raw;
		RID history;
		RID depth;
		RID normal_roughness;
		RID velocity;
		RID hit_distance;
		RID output;
		RID history_length;
		Size2i trace_size;
		Size2i screen_size;
		Projection prev_view_proj;
		Projection inv_view_proj;
		Vector3 camera_pos;
		Vector3 prev_camera_pos;
		bool velocity_enabled = false;
		uint32_t history_cap_min = 8;
		// #54 debug views: write the variance and rejected-history images.
		bool debug_views = false;
		RID debug_variance;
		RID debug_rejected;
	};
	struct SpecularSpatialParams {
		RID source;
		RID depth;
		RID normal_roughness;
		RID output;
		Size2i trace_size;
		Size2i screen_size;
		int32_t step = 1;
		float normal_power = 8.0f;
	};
	void process_specular_temporal(const SpecularTemporalParams &p_params);
	void process_specular_spatial(const SpecularSpatialParams &p_params);
	void process_specular_upsample(const SpecularUpsampleParams &p_params);

	void process_diffuse(const DiffuseParams &p_params);
	void process_temporal(const TemporalParams &p_params);
	void process_stabilize(const StabilizeParams &p_params);

	// GodotNext: denoise_reset_regions
	// One byte per probe, 1 when the probe falls inside any dirty AABB from the RT registry (#39).
	static PackedByteArray select_reset_probes(const Vector<Rect2i> &p_dirty_regions, const Size2i &p_probe_size);

	// GodotNext: nextgi_rr_hooks
	// Default-off RR hooks (#206). Parsed once from GODOT_NEXTGI_RR_HOOKS in debug builds, so the
	// DLSS-RR mode can bypass the stabilizer and cap the probe history without a project setting.
	struct RRHooks {
		bool stabilizer_enabled = true;
		uint32_t probe_temporal_max_history = 16;
		// GodotNext: nextgi_denoise_specular (#54). Default-off DLSS-RR hooks (#206).
		bool specular_filter_enabled = true;
		int sampling_mode = 0; // 0 = blue-noise tile, 1 = white hash.
	};
	static RRHooks parse_rr_hooks(const String &p_value);

	// Expanded source of one mode's compute stage. Used by the doctests; no GPU compile.
	static String build_shader_source(int p_variant = MODE_PROBE_SPATIAL);

private:
	struct DiffusePushConstant {
		float inv_projection[16];
		int32_t probe_size[2];
		int32_t screen_size[2];
		float depth_threshold;
		float normal_power;
		uint32_t probe_stride;
		uint32_t probe_extent;
	};

	// The matrices do not fit the 128-byte push-constant limit, so they travel in a storage buffer.
	struct TemporalParamsGPU {
		float prev_view_proj[16];
		float inv_view_proj[16];
		float camera_pos[4];
		float prev_camera_pos[4];
	};

	struct TemporalPushConstant {
		int32_t probe_size[2];
		int32_t screen_size[2];
		float depth_tolerance;
		float normal_tolerance;
		uint32_t history_cap;
		uint32_t velocity_enabled;
		uint32_t probe_stride;
		uint32_t probe_extent;
		uint32_t debug_views;
	};

	struct StabilizePushConstant {
		int32_t screen_size[2];
		float blend_weight;
		uint32_t stabilizer_enabled;
	};

	NextgiDenoiseDiffuseShaderRD shader;
	RID shader_version;
	RID pipelines[MODE_MAX];

	NextgiDenoiseSpecularShaderRD specular_shader;
	RID specular_version;
	RID specular_temporal;
	RID specular_spatial;
	RID specular_upsample;
	RID specular_params_buffer;
	RID temporal_params_buffer;
	bool gi_shaders_initialized = false;

	// GodotNext: rt_shadows (#141). Lazily initialized on first shadow_denoise() call.
	enum ShadowMode {
		SHADOW_MODE_TEMPORAL = 0,
		SHADOW_MODE_SPATIAL = 1,
		SHADOW_MODE_COPY_PREV_GUIDE = 2,
		SHADOW_MODE_MAX,
	};
	NextgiDenoiseShadowShaderRD shadow_shader;
	RID shadow_version;
	RID shadow_pipelines[SHADOW_MODE_MAX];
	bool shadow_shader_ready = false;
	bool shadow_shader_failed = false;
	bool _ensure_shadow_shader();

	// GodotNext: megalights_denoiser (#135). Lazily initialized on first megalights_denoise() call.
	enum MegaLightsTemporalMode {
		ML_TEMPORAL_MODE_ACCUM = 0,
		ML_TEMPORAL_MODE_COPY_PREV_GUIDE = 1,
		ML_TEMPORAL_MODE_MAX,
	};
	enum MegaLightsSpatialMode {
		ML_SPATIAL_MODE_ATROUS = 0,
		ML_SPATIAL_MODE_DEBUG_VIS = 1,
		ML_SPATIAL_MODE_MAX,
	};
	MlDenoiseTemporalShaderRD ml_temporal_shader;
	RID ml_temporal_version;
	RID ml_temporal_pipelines[ML_TEMPORAL_MODE_MAX];
	MlDenoiseSpatialShaderRD ml_spatial_shader;
	RID ml_spatial_version;
	RID ml_spatial_pipelines[ML_SPATIAL_MODE_MAX];
	bool ml_shaders_ready = false;
	bool ml_shaders_failed = false;
	bool _ensure_megalights_shaders();
};

} // namespace RendererRD
