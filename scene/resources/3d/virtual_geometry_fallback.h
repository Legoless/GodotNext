/**************************************************************************/
/*  virtual_geometry_fallback.h                                           */
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

// Per-(instance, surface) fallback reasons from plan §6.3. Zero means the surface is
// eligible and was virtualized. Values are append-only: persisted `.ngm` data and
// VirtualGeometryData store the number, not the name.
//
// NEXTGEO_FALLBACK_UNSUPPORTED_PRIMITIVE is import-only. Plan §6.3 keeps wireframe and
// points on the forward path without naming a code; non-triangle surfaces need a reason
// so the builder is not handed lines or points.
enum NextGeoFallback {
	NEXTGEO_FALLBACK_NONE = 0,
	NEXTGEO_FALLBACK_NEXT_PASS_OR_OVERLAY = 1,
	NEXTGEO_FALLBACK_VERTEX_LIGHTING = 2,
	NEXTGEO_FALLBACK_BAKED_GRIDMAP = 3,
	NEXTGEO_FALLBACK_MUTATED_MESH = 4,
	NEXTGEO_FALLBACK_UNSUPPORTED_DERIVATIVE = 5,
	NEXTGEO_FALLBACK_ALPHA = 6,
	NEXTGEO_FALLBACK_SKINNING = 7,
	NEXTGEO_FALLBACK_UNSUPPORTED_CUSTOM_SHADER = 8,
	NEXTGEO_FALLBACK_INSTANCE_UNIFORMS = 9,
	NEXTGEO_FALLBACK_LIGHTMAPPED_INSTANCE = 10,
	NEXTGEO_FALLBACK_MULTIMESH_OR_PARTICLES = 11,
	NEXTGEO_FALLBACK_MISSING_PAGE = 12,
	NEXTGEO_FALLBACK_UNSUPPORTED_PRIMITIVE = 13,
	NEXTGEO_FALLBACK_NOT_IN_RT_SCENE = 14,
	NEXTGEO_FALLBACK_RT_INSTANCE_CAP = 15,
	NEXTGEO_FALLBACK_RT_DYNAMIC_EXCLUDED = 16,
	NEXTGEO_FALLBACK_ALPHA_TESTED_RT_EXCLUDED = 17,
	NEXTGEO_FALLBACK_SDF_UNSUPPORTED_DEFORMATION = 18,
	NEXTGEO_FALLBACK_MSAA = 19,
	NEXTGEO_FALLBACK_MULTIVIEW = 20,
	NEXTGEO_FALLBACK_DEBUG_DRAW = 21,
};
