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
#include "core/templates/safe_refcount.h"
#include "servers/rendering/renderer_rd/storage_rd/render_buffer_custom_data_rd.h"
#include "servers/rendering/rendering_device.h"

class RenderDataRD;
class RenderSceneBuffersRD;

namespace RendererRD {

// GodotNext: ps_lumen_lite (#183). Half-resolution irradiance field over WRC levels 0-1.
// Both tiers pack the WRC radiance image into the probe buffer (the software tier's WRC
// traces the SDF). Neither dispatches screen-probe or reflection rays.
// Each probe holds L0 and L1 radiance, so the gather evaluates irradiance at the pixel normal
// and the rough specular at the reflection vector. The shader and pipeline are shared; every
// view keeps its own field (ViewField) on its render buffers.
class NextGIIrradianceField {
public:
	static constexpr uint32_t PROBE_COUNT = 8192;
	static constexpr uint32_t PROBES_PER_FRAME = 512;
	static constexpr uint32_t PROBE_VEC4S = 4;

	// One view's field: the probes packed from the WRC, the half-resolution irradiance, the
	// params and the pack cursor. It lives on the view's render buffers (custom data
	// "nextgi_lite_field") and is freed with them. A reconfigure (a resize) drops only the
	// half-resolution image; the probes keep their packed radiance.
	class ViewField : public RenderBufferCustomDataRD {
		GDCLASS(ViewField, RenderBufferCustomDataRD);

		static inline SafeNumeric<uint32_t> alive;

	public:
		RID probe_buffer;
		RID half_image;
		RID params_buffer;
		Size2i half_size;
		uint32_t cursor = 0;

		// Every probe starts not traced (validity 1, above the inside threshold).
		bool ensure_probe_buffer();
		// The first probe of this frame's pack; PROBES_PER_FRAME per call, wrapping at PROBE_COUNT.
		uint32_t consume_probe_base();
		// Fields alive across all views (tier info "lite_view_fields").
		static uint32_t get_alive_count() { return alive.get(); }

		virtual void configure(RenderSceneBuffersRD *p_render_buffers) override {}
		virtual void free_data() override;

		ViewField();
		~ViewField();
	};

	// The std430 block nextgi_irradiance_field.glsl reads (LiteParams).
	struct Params {
		float inv_view_proj[16]; // Inverse of the projection the depth buffer was drawn with.
		float camera[4]; // xyz; w = 1 when the WRC field is bound (p_wrc_field).
		float screen[4]; // GI buffer width and height, half-resolution width and height.
		float depth_info[4]; // Internal width and height, depth valid, normal valid.
		float rules[4]; // Inside back-face fraction, push spacing, SSR roughness, mip-2 roughness.
		float level_origin[2][4]; // WRC level 0 and 1 grid origin (xyz) and spacing (w), snapped to the grid camera.
		// Camera basis as three vec4 columns. The prepass normals are in view space; the shader
		// rotates them into world space, where the probes and the WRC live.
		float view_to_world[12];
	};

	// Everything the shader needs from the camera and the buffers. p_cam_projection is the
	// uncorrected camera projection; the depth correction and TAA jitter are applied here,
	// as RenderSceneDataRD does for the scene UBO. p_wrc_field: the WRC textures are bound,
	// so the probes take the WRC's depth-moment visibility and its mips (camera.w = 1).
	// p_grid_camera is the camera the probe grid snaps to: the WRC's grid camera
	// (NextGIWRC::get_grid_camera()) while the field is bound, since the WRC updates once per
	// frame from the frame's first view and the pack copies its probes by grid cell. The
	// camera position (camera.xyz) stays this view's.
	static Params build_params(const Projection &p_cam_projection, const Transform3D &p_cam_transform, bool p_flip_y, const Vector2 &p_taa_jitter, const Size2i &p_gi_size, const Size2i &p_half_size, const Size2i &p_internal_size, bool p_depth_valid, bool p_normal_valid, bool p_wrc_field, const Vector3 &p_grid_camera);

	NextGIIrradianceField();
	~NextGIIrradianceField();
	NextGIIrradianceField(const NextGIIrradianceField &) = delete;
	NextGIIrradianceField &operator=(const NextGIIrradianceField &) = delete;

	// The view's field, or null before Lite first integrated it.
	static Ref<ViewField> get_view_field(RenderSceneBuffersRD *p_render_buffers);
	// The view's half-resolution irradiance (DEBUG_DRAW_NEXTGI_LITE_PROBES), or an invalid RID.
	static RID get_debug_texture(RenderSceneBuffersRD *p_render_buffers);

	// Packs the WRC (when it is bound) into the view's field and writes its GI buffers. The frame
	// block calls it per view, outside reflections-only mode, where LightmapGI or VoxelGI writes
	// the ambient.
	void integrate(RenderDataRD *p_render_data);

private:
	struct GpuState;
	GpuState *gpu = nullptr;
	bool shader_failed = false;

	bool _ensure_gpu();
	static Ref<ViewField> _ensure_view_field(RenderSceneBuffersRD *p_render_buffers);
};

} // namespace RendererRD
