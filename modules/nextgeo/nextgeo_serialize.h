/**************************************************************************/
/*  nextgeo_serialize.h                                                   */
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

#include "nextgeo_dag_builder.h"

#include "core/io/resource_format_ngm.h"
#include "core/templates/vector.h"

class VirtualGeometryData;

// Encodes one raw `.ngm` page payload per `NextGeoBuildOutput` page. Page-local cluster
// offsets replace the builder's global ones. The provisional codec is raw (no meshopt).
Error nextgeo_write_pages(const NextGeoBuildOutput &p_output, Vector<Vector<uint8_t>> &r_pages);

// Fills an `NGMHeader` from a build and the payloads `nextgeo_write_pages` produced.
// `source_sha256` is the SHA-256 of those payloads in page order. The codec stays raw;
// a skinned build sets NGM_FLAG_SKINNED and `skin_influences`.
Error nextgeo_fill_ngm_header(const NextGeoBuildOutput &p_output, const Vector<Vector<uint8_t>> &p_pages, NGMHeader &r_header);

// Fills `dag_header` (groups, then BVH8 nodes), `root_pages`, the per-surface root-cut
// `coarse_fallback`, eligibility (1 = virtualized) and the stats. Fallback reasons are
// cleared to 0; import records a non-zero reason afterwards. Does not touch the RT proxy,
// card descriptors, or the `.ngm` path and file hash.
Error nextgeo_fill_vgd(const NextGeoBuildInput &p_input, const NextGeoBuildOutput &p_output, const Vector<Vector<uint8_t>> &p_pages, const Ref<VirtualGeometryData> &p_data);
