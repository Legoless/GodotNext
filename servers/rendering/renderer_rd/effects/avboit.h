/**************************************************************************/
/*  avboit.h                                                              */
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
#include "core/math/vector2i.h"
#include "servers/rendering/avboit_policy.h"
#include "servers/rendering/renderer_rd/shaders/effects/avboit_resolve.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

#define RB_SCOPE_AVBOIT SNAME("avboit")
#define RB_TEX_AVBOIT_BOUNDS SNAME("bounds")
#define RB_TEX_AVBOIT_OCCLUSION SNAME("occlusion")
#define RB_TEX_AVBOIT_ACCUM SNAME("accum")

class RenderSceneBuffersRD;

namespace RendererRD {

// GodotNext: avboit (#295, plan §21.2.1). AVBOIT on Forward+: the view's alpha list, drawn so that
// the result does not depend on the order it is drawn in. GodotNext's own design from Epic's public
// description only (docs/godotnext/source-log/avboit.md):
// 1. Bounds pre-pass, at 1/TILE_SIZE resolution per axis: the occluding translucent surfaces write
//    their view depth to a camera-aligned grid of cells, one per TILE_SIZE x TILE_SIZE pixels,
//    which keeps the nearest (min blend, .rgb) and farthest (max blend, .a) one per cell.
// 2. Occlusion pre-pass, same resolution: each of those surfaces adds its optical depth
//    -ln(1 - alpha) to one of SLICES depth slices, spaced logarithmically between that cell's
//    nearest and farthest depth (the adaptive depth slices), in LAYERS RGBA16F layers.
//    Both pre-passes sample each surface once per cell, at the cell's center. The view depth they
//    record comes from the window depth, kept to [MIN_DEPTH, MAX_DEPTH] (view_depth()).
// 3. Composite, full resolution: every surface of the alpha list is shaded as in the sorted pass,
//    then weighted by the transmittance in front of it and added to an accumulation target whose
//    alpha collects the coverage 1 - prod(1 - alpha). A fragment is classified by its surface's depth
//    at the cell's center, where the pre-passes sampled: its window depth, extrapolated there along
//    its screen derivatives (exact for a flat triangle, under perspective and orthographic
//    projections alike), so a sloped surface is weighed where its own sample sits
//    (transmittance_front()). Additions and products give the same result in any order.
// 4. Resolve: the accumulation, a premultiplied layer, goes over the view's color.
// Blend modes that do not fit an order-free sum (mul, and blending disabled) are drawn after the
// resolve in the sorted order, as the remainder pass.
// Known limits, as the build stands (EXPERIMENTAL; FLIP and cost are measured in PQ with GL, #134):
// - The grid samples each surface once per cell, at the cell's center, so where translucent layers
//   overlap their weights step at the cell size along the edges of the nearer one, and a surface the
//   center misses (thinner than a cell, alpha-scissored holes, a triangle's edge) dims nothing behind
//   it in that cell. Where such a surface's depth falls in a slice that other surfaces fill, its own
//   share still comes off that slice, so it is dimmed too little by them.
// - Surfaces in the same slice of a cell dim each other: their order within the slice is lost. Depths
//   closer than the bounds' half-float precision (about 1/1000 of the depth, and GPUs may round the
//   bounds toward zero) share a slice, as do surfaces within SLICE_TOLERANCE of a slice boundary.
// - View depths beyond MAX_DEPTH, the largest half float, are recorded as MAX_DEPTH: such surfaces
//   share the far end of the slices.
// - The transmittance is one channel, as the sorted mix blend's alpha is; there is no tinted
//   transmittance.
// - A material that reads the screen or depth texture sees the opaque scene, as in the sorted pass;
//   its result is then weighted like any other surface.
// - The composite writes no depth, so a depth_draw_always surface does not hide the translucent
//   surfaces behind it (its depth from the prepass still does).
// - The remainder pass draws after the resolve and tests only the opaque depth, so a mul or
//   blend_disabled surface draws over every AVBOIT surface, the ones in front of it included.
// - A shadow_to_opacity surface takes its alpha from lighting, which the pre-passes do not run, so
//   it is composited (weighted by what is in front of it, and covering the background) but not
//   recorded: it does not dim the translucent surfaces behind it.
// - Reflection probes and the debug draw modes keep sorted translucency.
class AVBOIT {
public:
	static constexpr int32_t TILE_SIZE = 4;
	static constexpr uint32_t LAYERS = ::AVBOITPolicy::OCCLUSION_LAYERS;
	static constexpr uint32_t SLICES = LAYERS * 4;
	// The optical depth of one surface is capped at -ln(1 - MAX_ALPHA), about 6.9: transmittance 0.001.
	static constexpr float MAX_ALPHA = 0.999f;
	static constexpr float MIN_DEPTH = 1e-4f;
	// The largest view depth the bounds keep: the largest half float. The bounds target's clear is
	// MAX_DEPTH as the nearest depth and 0 as the farthest; a farthest depth of 0 marks a cell no
	// occluding surface reached.
	static constexpr float MAX_DEPTH = 65504.0f;
	static constexpr float BOUNDS_CLEAR_NEAR = MAX_DEPTH;
	// A surface counts as behind every occluder of a cell once its depth there passes the cell's
	// farthest one by this factor, which covers the half-float rounding of the stored farthest depth.
	static constexpr float FAR_MARGIN = 1.002f;
	// The composite's depth at a cell's center and the pre-passes' differ by rounding, so a surface on a
	// slice boundary can land in the next slice in one of them: a surface's own slice is every slice
	// within this relative distance of its depth.
	static constexpr float SLICE_TOLERANCE = 2.5e-4f;

	enum Pass {
		PASS_NONE,
		PASS_BOUNDS,
		PASS_OCCLUSION,
		PASS_COMPOSITE,
		PASS_REMAINDER,
	};

	// How a surface takes part. Occluding surfaces (mix, premultiplied alpha, alpha to coverage) are
	// recorded by the pre-passes and drawn by the composite. Unrecorded ones are drawn by the
	// composite only: additive ones (add, subtract), as they block nothing in the sorted pass either,
	// and shadow_to_opacity ones, whose alpha comes from lighting the pre-passes do not run. The rest
	// (mul, blending disabled) is the remainder pass.
	enum SurfaceClass {
		SURFACE_OCCLUDING,
		SURFACE_UNRECORDED,
		SURFACE_REMAINDER,
	};

	// The composite's weighting, the specialization constant avboit_composite (3 bits). 0 is off.
	// Modes 1 and 2 are recorded by the pre-passes, so their own optical depth comes off their slice;
	// modes 5 and 6 cover the background like 1 and 2 but are not recorded.
	enum CompositeMode : uint32_t {
		COMPOSITE_OFF = 0,
		COMPOSITE_MIX = 1, // rgb * alpha * T, alpha.
		COMPOSITE_PREMULTIPLIED = 2, // rgb * T, alpha.
		COMPOSITE_ADD = 3, // rgb * alpha * T, 0.
		COMPOSITE_SUBTRACT = 4, // -rgb * alpha * T, 0.
		COMPOSITE_MIX_UNRECORDED = 5, // As 1, with no optical depth of its own in the grid.
		COMPOSITE_PREMULTIPLIED_UNRECORDED = 6, // As 2, with no optical depth of its own in the grid.
	};

	// p_blend_mode is a MaterialStorage::ShaderData::BlendMode; p_shadow_to_opacity is the
	// shadow_to_opacity render mode.
	static SurfaceClass surface_class(int p_blend_mode, bool p_shadow_to_opacity = false);
	static bool pass_draws(Pass p_pass, int p_blend_mode, bool p_shadow_to_opacity = false);
	static CompositeMode composite_mode(int p_blend_mode, bool p_shadow_to_opacity = false);

	// CPU forms of the GLSL in scene_forward_clustered_inc.glsl, for the doctests.
	static Size2i grid_size(const Size2i &p_internal_size);
	static Vector2i cell_of_pixel(const Vector2 &p_frag_coord, const Size2i &p_internal_size, const Size2i &p_grid_size);
	// The window position (pixels) of a cell's center, where the pre-passes sample.
	static Vector2 cell_center(const Vector2i &p_cell, const Size2i &p_internal_size, const Size2i &p_grid_size);
	// The view depth of a window depth at p_ndc, through the view's inverse projection, kept to
	// [MIN_DEPTH, MAX_DEPTH].
	static float view_depth(const Projection &p_inv_projection, const Vector2 &p_ndc, float p_window_depth);
	static float grid_depth(float p_view_depth);
	static float optical_depth(float p_alpha);
	static uint32_t slice(float p_depth, float p_near, float p_far);
	// p_slices holds the optical depth of each slice of one cell. p_depth is the surface's depth at the
	// cell's center; p_self_optical_depth is its own optical depth when the pre-passes record it, else 0.
	static float transmittance_front(const float *p_slices, float p_near, float p_far, float p_depth, float p_self_optical_depth);

	static RD::PipelineColorBlendState bounds_blend_state();
	static RD::PipelineColorBlendState occlusion_blend_state();
	static RD::PipelineColorBlendState::Attachment composite_blend_attachment();
	static Color bounds_clear_color() { return Color(BOUNDS_CLEAR_NEAR, BOUNDS_CLEAR_NEAR, BOUNDS_CLEAR_NEAR, 0.0); }

	static ::AVBOITPolicy::DeviceFeatures query_device_features(RenderingDevice *p_device);

	// The view's AVBOIT targets, kept in its render buffers (freed with them): the bounds grid, the
	// LAYERS-layer occlusion grid and the full-resolution accumulation target, with the
	// framebuffers of the three passes. The composite tests against p_depth without writing it.
	struct Targets {
		Size2i grid;
		RID bounds;
		RID occlusion;
		RID accum;
		RID bounds_framebuffer;
		RID occlusion_framebuffer;
		RID composite_framebuffer;
	};
	static bool ensure_targets(RenderSceneBuffersRD *p_buffers, RID p_depth, Targets &r_targets);

	// Puts the accumulation over p_color (RGBA16F, storage) inside p_rect.
	void resolve(RID p_accum, RID p_color, const Rect2i &p_rect);

	AVBOIT() {}
	~AVBOIT();

private:
	AvboitResolveShaderRD shader;
	RID shader_version;
	RID pipeline;
	bool _ensure_shader();
};

} // namespace RendererRD
