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

#include "core/math/projection.h"
#include "core/math/vector3.h"
#include "core/variant/variant.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_denoise_diffuse.glsl.gen.h"
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

	NextGIDenoise();
	~NextGIDenoise();

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
};

} // namespace RendererRD
