/**************************************************************************/
/*  nextgeo_route.h                                                       */
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

#include "core/typedefs.h"
#include "servers/rendering/rendering_server_enums.h"

namespace RendererRD {

// GodotNext: nextgeo_exclusive_dispatch (#79, plan §6.3, §7.3).
// Why a surface of a virtual-geometry mesh renders through the legacy path.
// The values are append-only: they feed the fallback debug view and Q14.
enum NextGeoFallback : uint32_t {
	NEXTGEO_FALLBACK_NONE = 0,
	NEXTGEO_FALLBACK_SKINNING,
	NEXTGEO_FALLBACK_INSTANCE_UNIFORMS,
	NEXTGEO_FALLBACK_LIGHTMAPPED_INSTANCE,
	NEXTGEO_FALLBACK_NEXT_PASS_OR_OVERLAY,
	NEXTGEO_FALLBACK_MSAA,
	NEXTGEO_FALLBACK_MULTIVIEW,
	NEXTGEO_FALLBACK_DEBUG_DRAW,
	NEXTGEO_FALLBACK_MUTATED_MESH,
	NEXTGEO_FALLBACK_BAKED_GRIDMAP,
	NEXTGEO_FALLBACK_VERTEX_LIGHTING,
	NEXTGEO_FALLBACK_UNSUPPORTED_CUSTOM_SHADER,
	NEXTGEO_FALLBACK_UNSUPPORTED_DERIVATIVE,
	NEXTGEO_FALLBACK_ALPHA,
	NEXTGEO_FALLBACK_MAX,
};

// Everything the route decides on, gathered once when the surface cache is
// built. View-level facts (MSAA, multiview, debug draw) are applied at draw
// time by nextgeo_view_fallback().
struct NextGeoSurfaceFacts {
	bool instance_mesh = false;
	bool mesh_has_virtual_geometry = false;
	bool surface_in_mask = false;
	bool draw_ready = false;
	bool skeleton = false;
	bool instance_uniforms = false;
	bool lightmapped = false;
	bool chain_blocked = false; // next pass, material overlay or stencil anywhere in the chain.
	bool mutated = false;
	bool baked_gridmap = false;
	NextGeoFallback material_fallback = NEXTGEO_FALLBACK_NONE;
	bool msaa = false;
	bool multiview = false;
	bool unsupported_debug_draw = false;
};

struct NextGeoSurfaceRoute {
	bool virtual_geometry = false;
	NextGeoFallback reason = NEXTGEO_FALLBACK_NONE;
};

// The first matching reason wins. A surface that is not a virtual-geometry
// candidate at all (not an INSTANCE_MESH surface, no VGD, outside the mask)
// is legacy with NEXTGEO_FALLBACK_NONE: it is not a fallback. The draw-ready
// gate keeps every candidate legacy until #80 publishes the resolve.
inline NextGeoFallback nextgeo_view_fallback(bool p_msaa, bool p_multiview, bool p_unsupported_debug_draw) {
	if (p_msaa) {
		return NEXTGEO_FALLBACK_MSAA;
	}
	if (p_multiview) {
		return NEXTGEO_FALLBACK_MULTIVIEW;
	}
	if (p_unsupported_debug_draw) {
		return NEXTGEO_FALLBACK_DEBUG_DRAW;
	}
	return NEXTGEO_FALLBACK_NONE;
}

inline NextGeoSurfaceRoute nextgeo_resolve_surface_route(const NextGeoSurfaceFacts &p_facts) {
	if (p_facts.mutated) {
		return { false, NEXTGEO_FALLBACK_MUTATED_MESH };
	}
	if (p_facts.baked_gridmap) {
		return { false, NEXTGEO_FALLBACK_BAKED_GRIDMAP };
	}
	if (!p_facts.instance_mesh || !p_facts.mesh_has_virtual_geometry) {
		return { false, NEXTGEO_FALLBACK_NONE };
	}
	const NextGeoFallback view = nextgeo_view_fallback(p_facts.msaa, p_facts.multiview, p_facts.unsupported_debug_draw);
	if (view != NEXTGEO_FALLBACK_NONE) {
		return { false, view };
	}
	if (p_facts.skeleton) {
		return { false, NEXTGEO_FALLBACK_SKINNING };
	}
	if (p_facts.instance_uniforms) {
		return { false, NEXTGEO_FALLBACK_INSTANCE_UNIFORMS };
	}
	if (p_facts.lightmapped) {
		return { false, NEXTGEO_FALLBACK_LIGHTMAPPED_INSTANCE };
	}
	if (p_facts.chain_blocked) {
		return { false, NEXTGEO_FALLBACK_NEXT_PASS_OR_OVERLAY };
	}
	if (p_facts.material_fallback != NEXTGEO_FALLBACK_NONE) {
		return { false, p_facts.material_fallback };
	}
	if (!p_facts.surface_in_mask) {
		return { false, NEXTGEO_FALLBACK_NONE };
	}
	if (!p_facts.draw_ready) {
		return { false, NEXTGEO_FALLBACK_NONE };
	}
	return { true, NEXTGEO_FALLBACK_NONE };
}

inline const char *nextgeo_fallback_name(NextGeoFallback p_reason) {
	switch (p_reason) {
		case NEXTGEO_FALLBACK_NONE:
			return "NONE";
		case NEXTGEO_FALLBACK_SKINNING:
			return "SKINNING";
		case NEXTGEO_FALLBACK_INSTANCE_UNIFORMS:
			return "INSTANCE_UNIFORMS";
		case NEXTGEO_FALLBACK_LIGHTMAPPED_INSTANCE:
			return "LIGHTMAPPED_INSTANCE";
		case NEXTGEO_FALLBACK_NEXT_PASS_OR_OVERLAY:
			return "NEXT_PASS_OR_OVERLAY";
		case NEXTGEO_FALLBACK_MSAA:
			return "MSAA";
		case NEXTGEO_FALLBACK_MULTIVIEW:
			return "MULTIVIEW";
		case NEXTGEO_FALLBACK_DEBUG_DRAW:
			return "DEBUG_DRAW";
		case NEXTGEO_FALLBACK_MUTATED_MESH:
			return "MUTATED_MESH";
		case NEXTGEO_FALLBACK_BAKED_GRIDMAP:
			return "BAKED_GRIDMAP";
		case NEXTGEO_FALLBACK_VERTEX_LIGHTING:
			return "VERTEX_LIGHTING";
		case NEXTGEO_FALLBACK_UNSUPPORTED_CUSTOM_SHADER:
			return "UNSUPPORTED_CUSTOM_SHADER";
		case NEXTGEO_FALLBACK_UNSUPPORTED_DERIVATIVE:
			return "UNSUPPORTED_DERIVATIVE";
		case NEXTGEO_FALLBACK_ALPHA:
			return "alpha";
		case NEXTGEO_FALLBACK_MAX:
			break;
	}
	return "NONE";
}

inline bool nextgeo_is_debug_view(RSE::ViewportDebugDraw p_mode) {
	switch (p_mode) {
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_CRACKS:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_RT_PROXY:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_CLUSTER_ERROR:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_RESIDENCY:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_VISIBILITY_ID:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_FALLBACK:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_TRIANGLE_SIZE:
			return true;
		default:
			return false;
	}
}

// Returns true when a viewport debug-draw mode relies on legacy scene-shader
// defines or legacy per-triangle draw lists and must route the whole view
// through the legacy path with NEXTGEO_FALLBACK_DEBUG_DRAW.
inline bool nextgeo_is_unsupported_debug_draw(RSE::ViewportDebugDraw p_mode) {
	switch (p_mode) {
		case RSE::VIEWPORT_DEBUG_DRAW_DISABLED:
		case RSE::VIEWPORT_DEBUG_DRAW_NORMAL_BUFFER:
		case RSE::VIEWPORT_DEBUG_DRAW_SHADOW_ATLAS:
		case RSE::VIEWPORT_DEBUG_DRAW_DIRECTIONAL_SHADOW_ATLAS:
		case RSE::VIEWPORT_DEBUG_DRAW_SCENE_LUMINANCE:
		case RSE::VIEWPORT_DEBUG_DRAW_SSAO:
		case RSE::VIEWPORT_DEBUG_DRAW_SSIL:
		case RSE::VIEWPORT_DEBUG_DRAW_DECAL_ATLAS:
		case RSE::VIEWPORT_DEBUG_DRAW_GI_BUFFER:
		case RSE::VIEWPORT_DEBUG_DRAW_MOTION_VECTORS:
		case RSE::VIEWPORT_DEBUG_DRAW_INTERNAL_BUFFER:
		case RSE::VIEWPORT_DEBUG_DRAW_AREA_LIGHT_ATLAS:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_TIER:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_TLAS_HEAT:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_BLAS_COST:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_HIT_INSTANCE:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_RT_PROXY:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_SRC_CARDS:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_WRC_PROBES:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_CARD_COVERAGE:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_SCREEN_PROBES:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_FALLBACK_REASONS:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_DENOISE_RAW:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_DENOISE_VARIANCE:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_DENOISE_HISTORY:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_DENOISE_REJECTED:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_DENOISE_DENOISED:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_REFLECTION:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_SPECULAR_HIT:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_SPECULAR_RAW:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_SPECULAR_VARIANCE:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_SPECULAR_HISTORY:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_SPECULAR_REJECTED:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGI_SPECULAR_DENOISED:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_CRACKS:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_RT_PROXY:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_CLUSTER_ERROR:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_RESIDENCY:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_VISIBILITY_ID:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_FALLBACK:
		case RSE::VIEWPORT_DEBUG_DRAW_NEXTGEO_TRIANGLE_SIZE:
			return false;
		default:
			return true;
	}
}

} // namespace RendererRD
