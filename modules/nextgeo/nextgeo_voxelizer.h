/**************************************************************************/
/*  nextgeo_voxelizer.h                                                   */
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

#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/templates/local_vector.h"

#include <cstdint>

// GodotNext: nextgeo_voxels (#165, plan §21.4 contract N10).
// Offline clean-room sparse 4^3 brick voxelizer for NextGeo cluster groups.
// Bricks are packed into each group's `.ngm` page payload so #78 page residency
// and streaming rules remain unchanged.

struct NextGeoVoxelTriInput {
	Vector3 p[3];
	Vector3 n[3];
	Vector2 uv[3];
	uint16_t surface_id = 0;
};

// Packed per-occupied-voxel payload (8 bytes = 2 uint32 words, matches nextgeo_voxels.h & voxel_inc.glsl).
struct NextGeoVoxelData {
	uint32_t uv_half2 = 0u; // packHalf2x16(mean_uv)
	uint32_t word1 = 0u; // oct16 (bits 0..15) | coverage_u8 (bits 16..23) | surface_id_u8 (bits 24..31)
};

// Sparse 4^3 (64-voxel) brick descriptor (32-byte header on disk + occupied_count * 8 bytes).
struct NextGeoVoxelBrick {
	float origin[3] = { 0.0f, 0.0f, 0.0f };
	float voxel_size = 0.0f;
	uint32_t occ_lo = 0u;
	uint32_t occ_hi = 0u;
	uint32_t meta = 0u; // occupied_count (0..7) | dominant_surface (8..23) | uniform_flag (bit 24)
	float uv_footprint = 0.0f; // Mean |dUV/dP| * voxel_size for resolve analytic UV derivatives.
	uint32_t voxel_offset = 0u; // First occupied voxel index in the owning voxel_data array.
};

// Per-group voxel brick span stored in NextGeoBuildOutput (parallel to `groups`).
struct NextGeoGroupVoxels {
	uint32_t brick_offset = 0u;
	uint32_t brick_count = 0u;
	float voxel_size = 0.0f;
	float group_error = 0.0f;
};

// Serialized byte count of a brick set (each brick: 32-byte header + occupied_count * 8 bytes).
uint32_t nextgeo_voxel_bricks_bytes(const NextGeoVoxelBrick *p_bricks, uint32_t p_brick_count);

// Serialized byte count of a page's trailing voxel section (16-byte header + group directory +
// brick payloads + 8-byte trailer + up to 3 bytes alignment padding). Returns 0 if p_brick_count == 0.
uint32_t nextgeo_voxel_page_section_max_bytes(uint32_t p_page_group_count, uint32_t p_bricks_bytes);

// Deterministic clean-room voxelizer: clips each triangle against overlapped cubic cells of edge
// `p_voxel_size`, accumulates area-weighted normals, UVs, surface IDs, and axis-projected coverage,
// and packs occupied cells into sparse 4^3 bricks sorted lexicographically by brick coordinate.
void nextgeo_voxelize_triangles(const LocalVector<NextGeoVoxelTriInput> &p_tris, float p_voxel_size, LocalVector<NextGeoVoxelBrick> &r_bricks, LocalVector<NextGeoVoxelData> &r_voxels);
