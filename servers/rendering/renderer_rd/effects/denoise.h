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
		Projection inv_projection;
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
		Projection prev_view_proj;
		Projection inv_projection;
		Vector3 camera_pos;
		Vector3 prev_camera_pos;
		float depth_tolerance = 0.1f; // Fraction of the stored distance.
		float normal_tolerance = 0.9f; // Minimum dot product with the reprojected guide.
		uint32_t history_cap = 16; // 0 makes the pass a pass-through (DLSS-RR bypass hook).
		bool velocity_enabled = false;
		uint32_t probe_stride = 4;
		uint32_t probe_extent = 16;
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

	void process_diffuse(const DiffuseParams &p_params);
	void process_temporal(const TemporalParams &p_params);
	void process_stabilize(const StabilizeParams &p_params);

	// GodotNext: denoise_reset_regions
	// One byte per probe, 1 when the probe falls inside any dirty AABB from the RT registry (#39).
	static PackedByteArray select_reset_probes(const Vector<Rect2i> &p_dirty_regions, const Size2i &p_probe_size);

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
		float inv_projection[16];
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
	};

	struct StabilizePushConstant {
		int32_t screen_size[2];
		float blend_weight;
		uint32_t stabilizer_enabled;
	};

	NextgiDenoiseDiffuseShaderRD shader;
	RID shader_version;
	RID pipelines[MODE_MAX];
	RID temporal_params_buffer;
};

} // namespace RendererRD
