/**************************************************************************/
/*  virtual_geometry_wind.h                                               */
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
#include "core/math/vector3.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

// GodotNext: nextgeo_foliage_wind (#152, plan §21.4 contract N10).
// Procedural wind skeleton stored per foliage virtual-geometry asset:
// 1 trunk bone (index 0) + up to 8 branch bones (indices 1..8), total <= 9 bones.
// Vertex bone indices and weights live in #159's `.ngm` cluster skinning chunks;
// runtime bone transforms are evaluated procedurally in shader per instance with
// 0 bytes of per-instance skin palette arena memory.
class VirtualGeometryWind : public Resource {
	GDCLASS(VirtualGeometryWind, Resource);

public:
	static constexpr int MAX_BRANCH_BONES = 8;
	static constexpr int MAX_BONES = 1 + MAX_BRANCH_BONES; // 9 total: 1 trunk + <= 8 branches.
	static constexpr int SKIN_INFLUENCES = 4;

private:
	PackedVector3Array bone_rest_positions;
	PackedVector3Array bone_axes;
	PackedFloat32Array bone_stiffness;
	PackedFloat32Array bone_sway_phases;
	PackedFloat32Array bone_max_sway;
	PackedFloat32Array bone_lever_lengths;
	// GodotNext: nextgeo_dynamic_wind (#296). Per-bone part mass and aerodynamic wind catch
	// computed from bind-pose geometry (backward-compatible default 1.0 when empty/missing).
	PackedFloat32Array bone_masses;
	PackedFloat32Array bone_wind_catches;
	float max_sway = 0.0f;
	Vector3 trunk_axis = Vector3(0.0f, 1.0f, 0.0f);
	float base_height = 0.0f;
	float top_height = 1.0f;

	void _clamp_arrays();

protected:
	static void _bind_methods();
	void reset_state() override;

public:
	void clear();
	int get_bone_count() const;

	Error add_bone(const Vector3 &p_rest_position, const Vector3 &p_axis, float p_stiffness = 0.5f, float p_sway_phase = 0.0f, float p_max_sway = 0.25f, float p_lever_length = 1.0f, float p_mass = 1.0f, float p_wind_catch = 1.0f);

	void set_bone_rest_positions(const PackedVector3Array &p_positions);
	PackedVector3Array get_bone_rest_positions() const;

	void set_bone_axes(const PackedVector3Array &p_axes);
	PackedVector3Array get_bone_axes() const;

	void set_bone_stiffness(const PackedFloat32Array &p_stiffness);
	PackedFloat32Array get_bone_stiffness() const;

	void set_bone_sway_phases(const PackedFloat32Array &p_phases);
	PackedFloat32Array get_bone_sway_phases() const;

	void set_bone_max_sway(const PackedFloat32Array &p_max_sway);
	PackedFloat32Array get_bone_max_sway() const;

	void set_bone_lever_lengths(const PackedFloat32Array &p_lever_lengths);
	PackedFloat32Array get_bone_lever_lengths() const;

	void set_bone_masses(const PackedFloat32Array &p_masses);
	PackedFloat32Array get_bone_masses() const;

	void set_bone_wind_catches(const PackedFloat32Array &p_wind_catches);
	PackedFloat32Array get_bone_wind_catches() const;

	// GodotNext: nextgeo_dynamic_wind (#296). Computes per-bone mass and aerodynamic wind_catch
	// (drag/sail area factor) from bind-pose geometry (triangle surface area and vertex distribution).
	void compute_bone_physics_from_geometry(const PackedVector3Array &p_positions, const PackedInt32Array &p_indices = PackedInt32Array(), const PackedInt32Array &p_bones = PackedInt32Array(), const PackedFloat32Array &p_weights = PackedFloat32Array(), int p_influences = SKIN_INFLUENCES);

	void set_max_sway(float p_max_sway);
	float get_max_sway() const;

	void set_trunk_axis(const Vector3 &p_axis);
	Vector3 get_trunk_axis() const;

	void set_base_height(float p_height);
	float get_base_height() const;

	void set_top_height(float p_height);
	float get_top_height() const;

	// Generates 1 trunk bone + <= 8 branch bones from bind-pose geometry positions
	// using height along `p_trunk_axis` and radial distribution around the trunk.
	Error generate_from_geometry(const PackedVector3Array &p_positions, const Vector3 &p_trunk_axis = Vector3(0.0f, 1.0f, 0.0f), float p_max_sway = -1.0f);

	// Computes 4-influence `#159` vertex bone indices and normalized weights for `p_positions`.
	void compute_vertex_skinning_arrays(const PackedVector3Array &p_positions, PackedInt32Array &r_bones, PackedFloat32Array &r_weights) const;
	Dictionary compute_vertex_skinning(const PackedVector3Array &p_positions) const;

	// Derives 1 trunk bone + <= 8 branch bones from an imported skeleton's mesh-local rest
	// positions, parent indices, and existing skin weights, outputting a source->wind bone remap table.
	Error generate_from_skeleton(const PackedVector3Array &p_bone_rest_positions, const PackedInt32Array &p_bone_parents, const PackedVector3Array &p_positions, const PackedInt32Array &p_src_bones, const PackedFloat32Array &p_src_weights, int p_src_influences, PackedInt32Array &r_remap_table, float p_max_sway = -1.0f);
	PackedInt32Array generate_from_skeleton_data(const PackedVector3Array &p_bone_rest_positions, const PackedInt32Array &p_bone_parents, const PackedVector3Array &p_positions, const PackedInt32Array &p_src_bones, const PackedFloat32Array &p_src_weights, int p_src_influences = 4, float p_max_sway = -1.0f);

	// Remaps source vertex skinning (4 or 8 influences) through `p_remap_table` into 4 normalized
	// wind bone influences in `[0, p_wind_bone_count - 1]`.
	static void remap_vertex_skinning(const PackedInt32Array &p_src_bones, const PackedFloat32Array &p_src_weights, int p_src_influences, const PackedInt32Array &p_remap_table, int p_wind_bone_count, PackedInt32Array &r_dst_bones, PackedFloat32Array &r_dst_weights);
};
