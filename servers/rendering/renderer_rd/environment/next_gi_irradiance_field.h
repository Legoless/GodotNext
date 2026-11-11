/**************************************************************************/
/*  next_gi_irradiance_field.h                                            */
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
#include "servers/rendering/rendering_device.h"

class RenderDataRD;

namespace RendererRD {

// GodotNext: ps_lumen_lite (#183). Half-resolution irradiance field over WRC levels 0-1.
// The software tier writes the probe buffer from its SDF tracer. The hardware tier packs
// the WRC radiance image. Neither path dispatches screen-probe or reflection rays.
// Each probe holds L0 and L1 radiance, so the gather evaluates irradiance at the pixel normal
// and the rough specular at the reflection vector.
class NextGIIrradianceField {
public:
	static constexpr uint32_t PROBE_COUNT = 8192;
	static constexpr uint32_t PROBES_PER_FRAME = 512;
	static constexpr uint32_t PROBE_VEC4S = 4;

	// The std430 block nextgi_irradiance_field.glsl reads (LiteParams).
	struct Params {
		float inv_view_proj[16]; // Inverse of the projection the depth buffer was drawn with.
		float camera[4]; // xyz; w = 1 when the WRC field is bound (p_wrc_field).
		float screen[4]; // GI buffer width and height, half-resolution width and height.
		float depth_info[4]; // Internal width and height, depth valid, normal valid.
		float rules[4]; // Inside back-face fraction, push spacing, SSR roughness, mip-2 roughness.
		float level_origin[2][4]; // WRC level 0 and 1 grid origin (xyz) and spacing (w).
		// Camera basis as three vec4 columns. The prepass normals are in view space; the shader
		// rotates them into world space, where the probes and the WRC live.
		float view_to_world[12];
	};

	// Everything the shader needs from the camera and the buffers. p_cam_projection is the
	// uncorrected camera projection; the depth correction and TAA jitter are applied here,
	// as RenderSceneDataRD does for the scene UBO. p_wrc_field: the WRC textures are bound,
	// so the probes take the WRC's depth-moment visibility and its mips (camera.w = 1).
	static Params build_params(const Projection &p_cam_projection, const Transform3D &p_cam_transform, bool p_flip_y, const Vector2 &p_taa_jitter, const Size2i &p_gi_size, const Size2i &p_half_size, const Size2i &p_internal_size, bool p_depth_valid, bool p_normal_valid, bool p_wrc_field);

	NextGIIrradianceField();
	~NextGIIrradianceField();
	NextGIIrradianceField(const NextGIIrradianceField &) = delete;
	NextGIIrradianceField &operator=(const NextGIIrradianceField &) = delete;

	bool ensure_probe_buffer();
	RID get_probe_buffer() const { return probe_buffer; }
	RID get_debug_texture() const { return half_image; }

	uint32_t consume_swrt_probe_base();
	uint32_t consume_hwrt_probe_base();

	// p_hwrt packs the WRC before interpolation. Reflections-only skips the write.
	void integrate(RenderDataRD *p_render_data, bool p_hwrt, bool p_reflections_only);

private:
	struct GpuState;
	GpuState *gpu = nullptr;
	RID probe_buffer;
	RID half_image;
	Size2i half_size;
	uint32_t swrt_cursor = 0;
	uint32_t hwrt_cursor = 0;
	bool shader_failed = false;

	bool _ensure_gpu();
	uint32_t _consume(uint32_t &r_cursor);
};

} // namespace RendererRD
