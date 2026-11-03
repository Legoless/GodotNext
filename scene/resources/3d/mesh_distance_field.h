/**************************************************************************/
/*  mesh_distance_field.h                                                 */
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

#include "core/io/resource.h"
#include "core/math/mesh_sdf_builder.h"
#include "core/variant/typed_array.h"

// Per-mesh signed distance field generated at import (GodotNext #184).
// Sub-resource of ArrayMesh, beside the shadow mesh. Distances are stored
// as sparse 8x8x8 R8 bricks with one page table per mip; see
// core/math/mesh_sdf_builder.h for the encoding. The GPU atlas that
// consumes this at runtime is #185.
class MeshDistanceField : public Resource {
	GDCLASS(MeshDistanceField, Resource);

public:
	static constexpr int FORMAT_VERSION = 1;

	enum Flags {
		FLAG_TWO_SIDED = 1 << 0,
		FLAG_UNSIGNED = 1 << 1,
	};

private:
	int format_version = FORMAT_VERSION;
	AABB bounds;
	float voxel_size = 0.0f;
	int flags = 0;
	// Three int32 values (x, y, z) per mip.
	PackedInt32Array mip_dimensions;
	PackedInt32Array mip_brick_grid_dimensions;
	PackedFloat32Array mip_bands;
	TypedArray<PackedInt32Array> page_tables;
	TypedArray<PackedByteArray> brick_data;

protected:
	static void _bind_methods();
	void reset_state() override;

public:
	void set_format_version(int p_version);
	int get_format_version() const;

	void set_bounds(const AABB &p_bounds);
	AABB get_bounds() const;

	void set_voxel_size(float p_voxel_size);
	float get_voxel_size() const;

	void set_flags(int p_flags);
	int get_flags() const;
	bool is_two_sided() const;

	void set_mip_dimensions(const PackedInt32Array &p_dimensions);
	PackedInt32Array get_mip_dimensions() const;

	void set_mip_brick_grid_dimensions(const PackedInt32Array &p_dimensions);
	PackedInt32Array get_mip_brick_grid_dimensions() const;

	void set_mip_bands(const PackedFloat32Array &p_bands);
	PackedFloat32Array get_mip_bands() const;

	void set_page_tables(const TypedArray<PackedInt32Array> &p_tables);
	TypedArray<PackedInt32Array> get_page_tables() const;

	void set_brick_data(const TypedArray<PackedByteArray> &p_data);
	TypedArray<PackedByteArray> get_brick_data() const;

	// C++-side conversion to and from the builder's plain data.
	void set_data(const MeshSDFData &p_data);
	MeshSDFData get_data() const;

	int get_mip_count() const;
	int get_brick_count(int p_mip) const;
	int64_t get_data_size() const;
	float get_voxel_distance(int p_mip, const Vector3i &p_voxel) const;
	float sample_distance(const Vector3 &p_position, int p_mip = 0) const;
};

VARIANT_ENUM_CAST(MeshDistanceField::Flags);
