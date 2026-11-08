/**************************************************************************/
/*  nextgeo_import.h                                                      */
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

#include "core/object/ref_counted.h"
#include "core/templates/vector.h"

class ImporterMesh;
class VirtualGeometryData;

// ImporterMesh::build_virtual_geometry_func. `p_eligible` is one byte per surface,
// 1 = eligible. Fills `r_vgd` (including the fixed RT proxy) and the raw page
// payloads. The caller writes the `.ngm`.
//
// The proxy is the DAG cut at eps_rt = min(max_extent / 256,
// `rendering/global_illumination/nextgi/rt_max_error`) (plan §7.1). The mesh
// AABB and the surface count cover every source surface, so an ineligible
// surface gets a [0, 0] range in every part. Stage-2 SRC card descriptors and
// per-triangle group IDs are populated from the proxy cut's cluster groups (#77).
Error nextgeo_import_build(const ImporterMesh *p_mesh, const PackedByteArray &p_eligible, Ref<VirtualGeometryData> &r_vgd, Vector<Vector<uint8_t>> &r_pages);
