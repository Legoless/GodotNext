/**************************************************************************/
/*  nextgeo_rt_proxy.h                                                    */
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

#include "core/error/error_list.h"
#include "core/math/aabb.h"
#include "core/object/ref_counted.h"
#include "core/variant/variant.h"

#include <cstdint>

class VirtualGeometryData;

// Triangles per BLAS part of the fixed RT proxy (plan §7.5, D13). A cut larger
// than this is split into several parts; it is never coarsened to meet the cap.
static constexpr uint32_t NEXTGEO_RT_PROXY_MAX_PART_TRIANGLES = 131072;

// Stage-2 SRC card descriptor (plan §8.3 Stage 2, #77), 32 bytes little-endian.
// Axis indices match NextGI Stage-1 axis seeds:
//   0: +X, 1: -X, 2: +Y, 3: -Y, 4: +Z, 5: -Z.
struct NextGeoCardDescriptor {
	uint16_t group = 0;
	uint8_t axis = 0;
	uint8_t flags = 0;
	float center[3] = {};
	float half_extents[3] = {};
	uint16_t res_u = 0;
	uint16_t res_v = 0;
};
static_assert(sizeof(NextGeoCardDescriptor) == 32, "NextGeoCardDescriptor must stay 32 bytes (plan §8.3 Stage 2).");

// One fixed, view-independent RT proxy of a mesh, in the exact layout of the
// `VirtualGeometryData` RT proxy fields. Positions are float32 xyz (including
// in double-precision builds) and indices are little-endian uint32, part-local.
// Vertex and index offsets and counts are in elements, not bytes or floats:
// a part's vertices start at `positions[vertex_offset * 3]` and its indices at
// byte `index_offset * 4` of `indices`.
struct NextGeoRTProxy {
	PackedFloat32Array positions;
	PackedByteArray indices; // Little-endian uint32, 4 bytes per index.
	// Groups of 4 per BLAS part: vertex_offset, vertex_count, index_offset, index_count.
	PackedInt32Array parts;
	// Groups of 2 per (part, surface), part-major and relative to the part:
	// first_index, index_count. Surfaces with no triangles in the part (legacy
	// surfaces in every part) are [0, 0].
	PackedInt32Array surface_ranges;
	PackedInt32Array clusters; // The selected cluster IDs, in part-packing order.
	// Little-endian uint16 proxy-cut group ID per proxy triangle, in final part/surface order.
	PackedByteArray triangle_groups;
	// Packed 32-byte little-endian NextGeoCardDescriptor records, sorted by (group, axis).
	PackedByteArray card_descriptors;
	float eps = 0.0f; // The eps_rt the cut was taken at, in mesh-local units.
	float error = 0.0f; // Largest group_error of the selected clusters; never above eps.
	uint32_t surface_count = 0; // Source surfaces the ranges cover, legacy ones included.
	uint64_t triangle_count = 0;

	uint32_t part_count() const { return uint32_t(parts.size() / 4); }
	uint32_t card_count() const { return uint32_t(card_descriptors.size() / sizeof(NextGeoCardDescriptor)); }
};

void nextgeo_decode_card_descriptor(const uint8_t *p_bytes, NextGeoCardDescriptor &r_desc);

// eps_rt = min(max_extent / 256, rt_max_error), in mesh-local units at import
// scale. `/256` is half a card texel at 128-texel cards; the world-unit bound
// (project setting `rendering/global_illumination/nextgi/rt_max_error`, 2 cm
// by default) binds above 5.12 m of extent, so large meshes keep thin walls.
float nextgeo_rt_proxy_epsilon(const AABB &p_mesh_aabb, float p_rt_max_error);

// Builds the fixed RT proxy of one mesh (plan §7.5, D13): the error-only DAG
// cut at eps_rt — cluster `c` is selected iff `c.group_error <= eps_rt` and
// `c.parent_error > eps_rt` — split into BLAS parts of at most
// NEXTGEO_RT_PROXY_MAX_PART_TRIANGLES triangles without coarsening.
//
// The result depends only on the DAG, the AABB and `p_rt_max_error`, and two
// builds of the same inputs are byte-identical: clusters are packed in BVH8
// leaf order (then cluster index), triangles are sorted by surface within each
// part (stable, so BLAS geometry index = surface), and positions are welded by
// exact float32 bit equality in first-occurrence order.
//
// `p_mesh_aabb` covers every source surface, legacy ones included, and
// `p_surface_count` counts them too: the DAG alone cannot know about trailing
// ineligible surfaces, and the ranges must cover every ArrayMesh surface.
//
// A DAG with no clusters (no eligible surface) succeeds with an empty proxy:
// no parts, `error` 0 and `eps` still set.
//
// The importer calls this after `nextgeo_build_dag()`, passing
// `GLOBAL_GET("rendering/global_illumination/nextgi/rt_max_error")` so a
// changed setting applies on reimport, then `nextgeo_fill_vgd_rt_proxy()`.
Error nextgeo_build_rt_proxy(const NextGeoBuildOutput &p_dag, const AABB &p_mesh_aabb, float p_rt_max_error, uint32_t p_surface_count, NextGeoRTProxy &r_proxy);

// Copies a built proxy into the `VirtualGeometryData` RT proxy fields. Touches
// nothing else, so it composes with `nextgeo_fill_vgd()` in either order.
Error nextgeo_fill_vgd_rt_proxy(const NextGeoRTProxy &p_proxy, const Ref<VirtualGeometryData> &p_data);
