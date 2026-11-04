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
#include "core/variant/variant.h"
#include "servers/rendering/renderer_rd/shaders/environment/nextgi_denoise_diffuse.glsl.gen.h"

namespace RendererRD {

// GodotNext: NextGI diffuse denoiser (#53, D8).
//
// One spatial pass today. The temporal and stabilizer passes of #53 land on top of this class;
// the two default-off RR hooks (probe_temporal_max_history, stabilizer_enabled) travel here so the
// call sites do not change when they arrive.
class NextGIDenoise {
public:
	// Inputs for one diffuse denoise step. The probe grid matches the screen, one probe per texel group.
	struct DiffuseParams {
		RID probe_radiance; // Probe-grid radiance read by the filter.
		RID probe_radiance_out; // Probe-grid destination; must differ from probe_radiance.
		RID depth; // Full-resolution depth, reverse-Z.
		RID normal_roughness; // Full-resolution normal (xyz, encoded) and roughness (w) guide.
		RID probe_hit_distance; // Probe-grid hit distance for the hit-distance weight.
		Size2i probe_size;
		Size2i screen_size;
		Projection inv_projection;
		float depth_threshold = 0.05f; // Plane-distance falloff as a fraction of probe depth.
		float normal_power = 8.0f;
		uint32_t probe_temporal_max_history = 16; // Read by the temporal pass (#53 step 8).
		bool stabilizer_enabled = true; // Read by the stabilizer (#53 step 8).
	};

	NextGIDenoise();
	~NextGIDenoise();

	// One 3x3 probe-space spatial pass. #53 step 2 asks for two iterations: call this twice with
	// probe_radiance and probe_radiance_out swapped, which leaves the result in the first texture.
	void process_diffuse(const DiffuseParams &p_params);

	// GodotNext: denoise_reset_regions
	// One byte per probe, 1 when the probe falls inside any dirty AABB from the RT registry (#39).
	// The temporal pass rejects history there instead of reprojecting it.
	static PackedByteArray select_reset_probes(const Vector<Rect2i> &p_dirty_regions, const Size2i &p_probe_size);

private:
	struct DiffusePushConstant {
		float inv_projection[16];
		int32_t probe_size[2];
		int32_t screen_size[2];
		float depth_threshold;
		float normal_power;
		uint32_t probe_temporal_max_history;
		uint32_t stabilizer_enabled;
	};

	NextgiDenoiseDiffuseShaderRD shader;
	RID shader_version;
	RID pipeline;
};

} // namespace RendererRD
