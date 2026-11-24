/**************************************************************************/
/*  virtual_geometry_wind.cpp                                             */
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

#include "virtual_geometry_wind.h"

#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "core/templates/local_vector.h"

#include <cfloat>
#include <cmath>

static Vector3 _safe_unit(const Vector3 &p_v, const Vector3 &p_fallback) {
	const real_t len = p_v.length();
	if (len > real_t(1e-5)) {
		return p_v / len;
	}
	return p_fallback;
}

static void _build_basis_from_axis(const Vector3 &p_axis, Vector3 &r_u, Vector3 &r_v) {
	const Vector3 ref = Math::abs(p_axis.y) < real_t(0.9) ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
	r_u = _safe_unit(p_axis.cross(ref), Vector3(1, 0, 0));
	r_v = _safe_unit(p_axis.cross(r_u), Vector3(0, 0, 1));
}

static float _smoothstep(float p_e0, float p_e1, float p_x) {
	if (p_e1 <= p_e0) {
		return p_x >= p_e1 ? 1.0f : 0.0f;
	}
	const float t = CLAMP((p_x - p_e0) / (p_e1 - p_e0), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

void VirtualGeometryWind::_clamp_arrays() {
	if (bone_rest_positions.size() > MAX_BONES) {
		bone_rest_positions.resize(MAX_BONES);
	}
	if (bone_axes.size() > MAX_BONES) {
		bone_axes.resize(MAX_BONES);
	}
	if (bone_stiffness.size() > MAX_BONES) {
		bone_stiffness.resize(MAX_BONES);
	}
	if (bone_sway_phases.size() > MAX_BONES) {
		bone_sway_phases.resize(MAX_BONES);
	}
	if (bone_max_sway.size() > MAX_BONES) {
		bone_max_sway.resize(MAX_BONES);
	}
	if (bone_lever_lengths.size() > MAX_BONES) {
		bone_lever_lengths.resize(MAX_BONES);
	}
	if (bone_masses.size() > MAX_BONES) {
		bone_masses.resize(MAX_BONES);
	}
	if (bone_wind_catches.size() > MAX_BONES) {
		bone_wind_catches.resize(MAX_BONES);
	}
}

void VirtualGeometryWind::clear() {
	bone_rest_positions.clear();
	bone_axes.clear();
	bone_stiffness.clear();
	bone_sway_phases.clear();
	bone_max_sway.clear();
	bone_lever_lengths.clear();
	bone_masses.clear();
	bone_wind_catches.clear();
	max_sway = 0.0f;
	trunk_axis = Vector3(0.0f, 1.0f, 0.0f);
	base_height = 0.0f;
	top_height = 1.0f;
	emit_changed();
}

int VirtualGeometryWind::get_bone_count() const {
	return MIN(int(bone_rest_positions.size()), MAX_BONES);
}

Error VirtualGeometryWind::add_bone(const Vector3 &p_rest_position, const Vector3 &p_axis, float p_stiffness, float p_sway_phase, float p_max_sway, float p_lever_length, float p_mass, float p_wind_catch) {
	const int cur = get_bone_count();
	ERR_FAIL_COND_V_MSG(cur >= MAX_BONES, ERR_CANT_CREATE, vformat("VirtualGeometryWind supports at most %d bones (1 trunk + %d branches).", MAX_BONES, MAX_BRANCH_BONES));

	const Vector3 axis = _safe_unit(p_axis, trunk_axis);
	const float stiff = CLAMP(std::isfinite(p_stiffness) ? p_stiffness : 0.5f, 0.0f, 1.0f);
	const float phase = std::isfinite(p_sway_phase) ? p_sway_phase : 0.0f;
	const float sway = MAX(std::isfinite(p_max_sway) ? p_max_sway : 0.0f, 0.0f);
	const float lever = MAX(std::isfinite(p_lever_length) ? p_lever_length : 1.0f, 0.25f);
	const float mass = CLAMP((std::isfinite(p_mass) && p_mass > 0.0f) ? p_mass : 1.0f, 0.05f, 1000.0f);
	const float wind_catch = CLAMP((std::isfinite(p_wind_catch) && p_wind_catch > 0.0f) ? p_wind_catch : 1.0f, 0.01f, 1000.0f);

	const int prev_masses = bone_masses.size();
	const int prev_catches = bone_wind_catches.size();

	bone_rest_positions.resize(cur + 1);
	bone_axes.resize(cur + 1);
	bone_stiffness.resize(cur + 1);
	bone_sway_phases.resize(cur + 1);
	bone_max_sway.resize(cur + 1);
	bone_lever_lengths.resize(cur + 1);
	bone_masses.resize(cur + 1);
	bone_wind_catches.resize(cur + 1);

	for (int i = prev_masses; i < cur; i++) {
		bone_masses.write[i] = 1.0f;
	}
	for (int i = prev_catches; i < cur; i++) {
		bone_wind_catches.write[i] = 1.0f;
	}

	bone_rest_positions.write[cur] = p_rest_position;
	bone_axes.write[cur] = axis;
	bone_stiffness.write[cur] = stiff;
	bone_sway_phases.write[cur] = phase;
	bone_max_sway.write[cur] = sway;
	bone_lever_lengths.write[cur] = lever;
	bone_masses.write[cur] = mass;
	bone_wind_catches.write[cur] = wind_catch;
	if (sway > max_sway) {
		max_sway = sway;
	}
	emit_changed();
	return OK;
}

void VirtualGeometryWind::set_bone_rest_positions(const PackedVector3Array &p_positions) {
	bone_rest_positions = p_positions;
	_clamp_arrays();
	emit_changed();
}

PackedVector3Array VirtualGeometryWind::get_bone_rest_positions() const {
	return bone_rest_positions;
}

void VirtualGeometryWind::set_bone_axes(const PackedVector3Array &p_axes) {
	bone_axes = p_axes;
	_clamp_arrays();
	emit_changed();
}

PackedVector3Array VirtualGeometryWind::get_bone_axes() const {
	return bone_axes;
}

void VirtualGeometryWind::set_bone_stiffness(const PackedFloat32Array &p_stiffness) {
	bone_stiffness = p_stiffness;
	_clamp_arrays();
	emit_changed();
}

PackedFloat32Array VirtualGeometryWind::get_bone_stiffness() const {
	return bone_stiffness;
}

void VirtualGeometryWind::set_bone_sway_phases(const PackedFloat32Array &p_phases) {
	bone_sway_phases = p_phases;
	_clamp_arrays();
	emit_changed();
}

PackedFloat32Array VirtualGeometryWind::get_bone_sway_phases() const {
	return bone_sway_phases;
}

void VirtualGeometryWind::set_bone_max_sway(const PackedFloat32Array &p_max_sway) {
	bone_max_sway = p_max_sway;
	_clamp_arrays();
	emit_changed();
}

PackedFloat32Array VirtualGeometryWind::get_bone_max_sway() const {
	return bone_max_sway;
}

void VirtualGeometryWind::set_bone_lever_lengths(const PackedFloat32Array &p_lever_lengths) {
	bone_lever_lengths = p_lever_lengths;
	_clamp_arrays();
	emit_changed();
}

PackedFloat32Array VirtualGeometryWind::get_bone_lever_lengths() const {
	return bone_lever_lengths;
}

void VirtualGeometryWind::set_bone_masses(const PackedFloat32Array &p_masses) {
	bone_masses = p_masses;
	_clamp_arrays();
	emit_changed();
}

PackedFloat32Array VirtualGeometryWind::get_bone_masses() const {
	return bone_masses;
}

void VirtualGeometryWind::set_bone_wind_catches(const PackedFloat32Array &p_wind_catches) {
	bone_wind_catches = p_wind_catches;
	_clamp_arrays();
	emit_changed();
}

PackedFloat32Array VirtualGeometryWind::get_bone_wind_catches() const {
	return bone_wind_catches;
}

void VirtualGeometryWind::compute_bone_physics_from_geometry(const PackedVector3Array &p_positions, const PackedInt32Array &p_indices, const PackedInt32Array &p_bones, const PackedFloat32Array &p_weights, int p_influences) {
	const int bcount = get_bone_count();
	if (bcount <= 0) {
		bone_masses.clear();
		bone_wind_catches.clear();
		return;
	}

	bone_masses.resize(bcount);
	bone_wind_catches.resize(bcount);

	const int vert_count = p_positions.size();
	if (vert_count <= 0) {
		for (int b = 0; b < bcount; b++) {
			bone_masses.write[b] = (b == 0) ? 2.5f : 1.0f;
			bone_wind_catches.write[b] = 1.0f;
		}
		emit_changed();
		return;
	}

	const int inf = (p_influences == 8) ? 8 : SKIN_INFLUENCES;
	PackedInt32Array local_bones;
	PackedFloat32Array local_weights;
	const int32_t *b_ptr = nullptr;
	const float *w_ptr = nullptr;
	int eff_inf = inf;
	if (p_bones.size() == vert_count * inf && p_weights.size() == vert_count * inf) {
		b_ptr = p_bones.ptr();
		w_ptr = p_weights.ptr();
	} else {
		compute_vertex_skinning_arrays(p_positions, local_bones, local_weights);
		b_ptr = local_bones.ptr();
		w_ptr = local_weights.ptr();
		eff_inf = SKIN_INFLUENCES;
	}

	LocalVector<float> bone_area;
	LocalVector<float> bone_lateral_catch;
	LocalVector<float> bone_weight_sum;
	LocalVector<float> bone_radial_sum;
	LocalVector<Vector3> bone_bmin;
	LocalVector<Vector3> bone_bmax;
	LocalVector<uint8_t> bone_has_vert;
	bone_area.resize(bcount);
	bone_lateral_catch.resize(bcount);
	bone_weight_sum.resize(bcount);
	bone_radial_sum.resize(bcount);
	bone_bmin.resize(bcount);
	bone_bmax.resize(bcount);
	bone_has_vert.resize(bcount);
	for (int b = 0; b < bcount; b++) {
		bone_area[b] = 0.0f;
		bone_lateral_catch[b] = 0.0f;
		bone_weight_sum[b] = 0.0f;
		bone_radial_sum[b] = 0.0f;
		bone_bmin[b] = Vector3(FLT_MAX, FLT_MAX, FLT_MAX);
		bone_bmax[b] = Vector3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
		bone_has_vert[b] = 0;
	}

	Vector3 unit_axes[MAX_BONES];
	for (int b = 0; b < bcount; b++) {
		unit_axes[b] = b < bone_axes.size() ? _safe_unit(bone_axes[b], trunk_axis) : _safe_unit(trunk_axis, Vector3(0.0f, 1.0f, 0.0f));
	}

	// Accumulate vertex distribution and local extents per bone.
	for (int v = 0; v < vert_count; v++) {
		const Vector3 &p = p_positions[v];
		for (int k = 0; k < eff_inf; k++) {
			const int b = b_ptr[v * eff_inf + k];
			const float w = w_ptr[v * eff_inf + k];
			if (b < 0 || b >= bcount || !(w > 1.0e-4f)) {
				continue;
			}
			const Vector3 rel = p - bone_rest_positions[b];
			const Vector3 perp = rel - unit_axes[b] * rel.dot(unit_axes[b]);
			bone_weight_sum[b] += w;
			bone_radial_sum[b] += w * float(perp.length());
			bone_bmin[b] = bone_bmin[b].min(p);
			bone_bmax[b] = bone_bmax[b].max(p);
			bone_has_vert[b] = 1;
		}
	}

	// Accumulate exact triangle surface area and lateral aerodynamic wind-catch per bone when triangles are present.
	bool used_triangles = false;
	const int tri_count = p_indices.size() / 3;
	if (tri_count > 0) {
		for (int t = 0; t < tri_count; t++) {
			const int i0 = p_indices[t * 3 + 0];
			const int i1 = p_indices[t * 3 + 1];
			const int i2 = p_indices[t * 3 + 2];
			if (i0 < 0 || i0 >= vert_count || i1 < 0 || i1 >= vert_count || i2 < 0 || i2 >= vert_count) {
				continue;
			}
			const Vector3 &p0 = p_positions[i0];
			const Vector3 &p1 = p_positions[i1];
			const Vector3 &p2 = p_positions[i2];
			const Vector3 cr = (p1 - p0).cross(p2 - p0);
			const float cr_len = float(cr.length());
			if (!(cr_len > 1.0e-8f)) {
				continue;
			}
			const float tri_area = 0.5f * cr_len;
			const Vector3 tri_n = cr / real_t(cr_len);
			const float share = tri_area * (1.0f / 3.0f);
			const int verts[3] = { i0, i1, i2 };
			for (int c = 0; c < 3; c++) {
				const int vi = verts[c];
				for (int k = 0; k < eff_inf; k++) {
					const int b = b_ptr[vi * eff_inf + k];
					const float w = w_ptr[vi * eff_inf + k];
					if (b < 0 || b >= bcount || !(w > 1.0e-4f)) {
						continue;
					}
					const float nd = CLAMP(Math::abs(float(tri_n.dot(unit_axes[b]))), 0.0f, 1.0f);
					const float lateral = std::sqrt(MAX(1.0f - nd * nd, 0.0f));
					const float exposure = 0.35f + 0.65f * lateral;
					const float wa = share * w;
					bone_area[b] += wa;
					bone_lateral_catch[b] += wa * exposure;
				}
			}
			used_triangles = true;
		}
	}

	LocalVector<float> raw_areas;
	LocalVector<float> raw_catches;
	raw_areas.resize(bcount);
	raw_catches.resize(bcount);
	float total_area = 0.0f;
	for (int b = 0; b < bcount; b++) {
		const float lever = b < bone_lever_lengths.size() ? MAX(bone_lever_lengths[b], 0.25f) : 1.0f;
		float area = bone_area[b];
		float lateral_catch = bone_lateral_catch[b];
		if (used_triangles && area > 1.0e-5f) {
			// Already accumulated from bind-pose triangles.
		} else if (bone_has_vert[b] && bone_weight_sum[b] > 1.0e-4f) {
			const Vector3 ext = bone_bmax[b] - bone_bmin[b];
			const float box_surface = 2.0f * (float(ext.x * ext.y) + float(ext.y * ext.z) + float(ext.z * ext.x));
			const float mean_radius = MAX(bone_radial_sum[b] / bone_weight_sum[b], 0.08f);
			area = MAX(box_surface * 0.35f, float(Math::PI) * mean_radius * lever * 0.5f);
			lateral_catch = area * (b == 0 ? 0.55f : 0.78f);
		} else {
			area = 0.5f * lever;
			lateral_catch = 0.35f * lever;
		}
		raw_areas[b] = area;
		raw_catches[b] = lateral_catch;
		total_area += area;
	}

	const float ref_area = MAX(total_area / float(MAX(bcount, 1)), 0.25f);
	for (int b = 0; b < bcount; b++) {
		const float lever = b < bone_lever_lengths.size() ? MAX(bone_lever_lengths[b], 0.25f) : 1.0f;
		const float rel_area = raw_areas[b] / ref_area;
		const float rel_catch = raw_catches[b] / ref_area;
		// Sublinear scale inertia keeps taller trees heavier than small shrubs while keeping spring frequencies
		// in a physically responsive band (~1.5-5 rad/s). Trunk bone (0) is denser than leafy branch boughs.
		const float size_inertia = CLAMP(std::sqrt(lever / 2.0f), 0.5f, 2.2f);
		const float density_weight = (b == 0) ? 1.65f : 0.72f;
		const float mass = CLAMP(density_weight * size_inertia * (0.35f + 0.65f * rel_area), 0.2f, 6.0f);
		const float catch_weight = (b == 0) ? 0.65f : 1.25f;
		const float wind_catch = CLAMP(catch_weight * (0.20f + 0.95f * rel_catch), 0.1f, 6.0f);

		bone_masses.write[b] = mass;
		bone_wind_catches.write[b] = wind_catch;
	}
	emit_changed();
}

void VirtualGeometryWind::set_max_sway(float p_max_sway) {
	max_sway = MAX(std::isfinite(p_max_sway) ? p_max_sway : 0.0f, 0.0f);
	emit_changed();
}

float VirtualGeometryWind::get_max_sway() const {
	return max_sway;
}

void VirtualGeometryWind::set_trunk_axis(const Vector3 &p_axis) {
	trunk_axis = _safe_unit(p_axis, Vector3(0.0f, 1.0f, 0.0f));
	emit_changed();
}

Vector3 VirtualGeometryWind::get_trunk_axis() const {
	return trunk_axis;
}

void VirtualGeometryWind::set_base_height(float p_height) {
	base_height = std::isfinite(p_height) ? p_height : 0.0f;
	emit_changed();
}

float VirtualGeometryWind::get_base_height() const {
	return base_height;
}

void VirtualGeometryWind::set_top_height(float p_height) {
	top_height = std::isfinite(p_height) ? p_height : 1.0f;
	emit_changed();
}

float VirtualGeometryWind::get_top_height() const {
	return top_height;
}

Error VirtualGeometryWind::generate_from_geometry(const PackedVector3Array &p_positions, const Vector3 &p_trunk_axis, float p_max_sway) {
	ERR_FAIL_COND_V(p_positions.is_empty(), ERR_INVALID_PARAMETER);

	const Vector3 axis = _safe_unit(p_trunk_axis, Vector3(0.0f, 1.0f, 0.0f));
	Vector3 basis_u, basis_v;
	_build_basis_from_axis(axis, basis_u, basis_v);

	const int count = p_positions.size();
	float h_min = FLT_MAX;
	float h_max = -FLT_MAX;
	Vector3 all_sum;
	for (int i = 0; i < count; i++) {
		const Vector3 &p = p_positions[i];
		const float h = float(p.dot(axis));
		h_min = MIN(h_min, h);
		h_max = MAX(h_max, h);
		all_sum += p;
	}
	const float height = MAX(h_max - h_min, 1e-4f);

	// Estimate trunk horizontal center from the lower 25% of vertices (falls back to overall mean).
	Vector3 base_sum;
	int base_count = 0;
	const float base_cutoff = h_min + height * 0.25f;
	for (int i = 0; i < count; i++) {
		const Vector3 &p = p_positions[i];
		if (float(p.dot(axis)) <= base_cutoff) {
			base_sum += p;
			base_count++;
		}
	}
	const Vector3 ref_center = base_count > 0 ? (base_sum / real_t(base_count)) : (all_sum / real_t(count));
	const Vector3 trunk_perp = ref_center - axis * ref_center.dot(axis);
	const Vector3 trunk_base = trunk_perp + axis * real_t(h_min);

	float max_radius = 0.0f;
	for (int i = 0; i < count; i++) {
		const Vector3 &p = p_positions[i];
		const Vector3 radial = (p - trunk_perp) - axis * p.dot(axis);
		max_radius = MAX(max_radius, float(radial.length()));
	}

	const float asset_max_sway = (std::isfinite(p_max_sway) && p_max_sway > 0.0f)
			? p_max_sway
			: CLAMP(0.12f * height + 0.08f * max_radius, 0.05f, 2.0f);

	// Partition canopy vertices into up to MAX_BRANCH_BONES (8) angular sectors around the trunk.
	Vector3 sector_sum[MAX_BRANCH_BONES];
	int sector_count[MAX_BRANCH_BONES] = {};
	const float radial_thresh = max_radius * 0.05f;
	for (int i = 0; i < count; i++) {
		const Vector3 &p = p_positions[i];
		const float t_h = (float(p.dot(axis)) - h_min) / height;
		const Vector3 radial = (p - trunk_perp) - axis * p.dot(axis);
		const float r_len = float(radial.length());
		if (t_h < 0.15f && max_radius > 1e-4f) {
			continue;
		}
		if (r_len <= radial_thresh && max_radius > 1e-4f) {
			continue;
		}
		const float angle = std::atan2(float(radial.dot(basis_v)), float(radial.dot(basis_u)));
		const float norm_a = (angle < 0.0f ? angle + float(Math::TAU) : angle) / float(Math::TAU);
		const int sector = CLAMP(int(std::floor(norm_a * float(MAX_BRANCH_BONES))), 0, MAX_BRANCH_BONES - 1);
		sector_sum[sector] += p;
		sector_count[sector]++;
	}

	clear();
	trunk_axis = axis;
	base_height = h_min;
	top_height = h_max;
	max_sway = asset_max_sway;

	// Bone 0: Trunk bone rooted at base of trunk.
	bone_rest_positions.push_back(trunk_base);
	bone_axes.push_back(axis);
	bone_stiffness.push_back(0.70f);
	bone_sway_phases.push_back(0.0f);
	bone_max_sway.push_back(asset_max_sway * 0.65f);
	bone_lever_lengths.push_back(MAX(height, 0.25f));

	// Bones 1..N (<= 8): Branch bones for each populated canopy sector.
	for (int s = 0; s < MAX_BRANCH_BONES; s++) {
		if (sector_count[s] == 0) {
			continue;
		}
		const Vector3 centroid = sector_sum[s] / real_t(sector_count[s]);
		const float c_h = float(centroid.dot(axis));
		const float c_t_h = CLAMP((c_h - h_min) / height, 0.0f, 1.0f);
		const float attach_h = CLAMP(c_h - 0.15f * height, h_min + 0.15f * height, h_max);
		const Vector3 pivot = trunk_perp + axis * real_t(attach_h);
		const Vector3 b_axis = _safe_unit(centroid - pivot, axis);
		const float b_stiff = CLAMP(0.55f - 0.25f * c_t_h, 0.20f, 0.70f);
		const float b_phase = float(s) * (float(Math::TAU) / float(MAX_BRANCH_BONES)) + 0.7f * c_t_h;
		const float b_sway = asset_max_sway;

		bone_rest_positions.push_back(pivot);
		bone_axes.push_back(b_axis);
		bone_stiffness.push_back(b_stiff);
		bone_sway_phases.push_back(b_phase);
		bone_max_sway.push_back(b_sway);
		bone_lever_lengths.push_back(MAX(float(centroid.distance_to(pivot)), 0.25f));
	}

	// Compute skinning weights once and update exact conservative per-bone lever lengths so that
	// every influenced vertex displacement is strictly bounded by bone_max_sway <= max_sway.
	PackedInt32Array tmp_bones;
	PackedFloat32Array tmp_weights;
	compute_vertex_skinning_arrays(p_positions, tmp_bones, tmp_weights);
	const int bcount = get_bone_count();
	LocalVector<float> max_lever;
	max_lever.resize(bcount);
	for (int b = 0; b < bcount; b++) {
		max_lever[b] = 0.25f;
	}
	for (int i = 0; i < count; i++) {
		const Vector3 &p = p_positions[i];
		for (int k = 0; k < SKIN_INFLUENCES; k++) {
			const float w = tmp_weights[i * SKIN_INFLUENCES + k];
			const int b = tmp_bones[i * SKIN_INFLUENCES + k];
			if (w > 1e-4f && b >= 0 && b < bcount) {
				const float d = float(p.distance_to(bone_rest_positions[b]));
				max_lever[b] = MAX(max_lever[b], d);
			}
		}
	}
	for (int b = 0; b < bcount; b++) {
		bone_lever_lengths.write[b] = max_lever[b];
	}
	compute_bone_physics_from_geometry(p_positions, PackedInt32Array(), tmp_bones, tmp_weights, SKIN_INFLUENCES);

	emit_changed();
	return OK;
}

void VirtualGeometryWind::compute_vertex_skinning_arrays(const PackedVector3Array &p_positions, PackedInt32Array &r_bones, PackedFloat32Array &r_weights) const {
	const int count = p_positions.size();
	r_bones.resize(count * SKIN_INFLUENCES);
	r_weights.resize(count * SKIN_INFLUENCES);
	if (count == 0) {
		return;
	}

	const int bcount = get_bone_count();
	int32_t *bw = r_bones.ptrw();
	float *ww = r_weights.ptrw();

	if (bcount <= 1) {
		for (int i = 0; i < count; i++) {
			bw[i * 4 + 0] = 0;
			bw[i * 4 + 1] = 0;
			bw[i * 4 + 2] = 0;
			bw[i * 4 + 3] = 0;
			ww[i * 4 + 0] = 1.0f;
			ww[i * 4 + 1] = 0.0f;
			ww[i * 4 + 2] = 0.0f;
			ww[i * 4 + 3] = 0.0f;
		}
		return;
	}

	const Vector3 axis = _safe_unit(trunk_axis, Vector3(0.0f, 1.0f, 0.0f));
	const Vector3 trunk_base = bone_rest_positions[0];
	const Vector3 trunk_perp = trunk_base - axis * trunk_base.dot(axis);
	const float height = MAX(top_height - base_height, 1e-4f);

	// Estimate radial scale from vertex spread around trunk.
	float max_radius = 1e-4f;
	for (int i = 0; i < count; i++) {
		const Vector3 radial = (p_positions[i] - trunk_perp) - axis * p_positions[i].dot(axis);
		max_radius = MAX(max_radius, float(radial.length()));
	}

	// Precompute branch tip anchors for smooth inverse-distance branch blending.
	Vector3 branch_anchor[MAX_BONES];
	for (int b = 1; b < bcount; b++) {
		const Vector3 b_axis = b < bone_axes.size() ? _safe_unit(bone_axes[b], axis) : axis;
		const float lever = b < bone_lever_lengths.size() ? MAX(bone_lever_lengths[b], 0.25f) : 1.0f;
		branch_anchor[b] = bone_rest_positions[b] + b_axis * real_t(lever * 0.65f);
	}

	for (int i = 0; i < count; i++) {
		const Vector3 &p = p_positions[i];
		const float t_h = CLAMP((float(p.dot(axis)) - base_height) / height, 0.0f, 1.0f);
		const Vector3 radial = (p - trunk_perp) - axis * p.dot(axis);
		const float r_norm = CLAMP(float(radial.length()) / max_radius, 0.0f, 1.0f);

		// Low vertices and core trunk vertices lean heavily on trunk bone 0;
		// outer/upper foliage blends smoothly into up to 3 nearest branch bones.
		const float branch_blend = _smoothstep(0.10f, 0.65f, t_h) * (0.25f + 0.75f * _smoothstep(0.05f, 0.65f, r_norm));
		const float w_trunk = CLAMP(1.0f - 0.80f * branch_blend, 0.20f, 1.0f);

		// Pick top 3 closest branch bones by inverse-squared distance to branch anchor + alignment.
		int best_idx[3] = { 0, 0, 0 };
		float best_score[3] = { 0.0f, 0.0f, 0.0f };
		for (int b = 1; b < bcount; b++) {
			const float d2 = float(p.distance_squared_to(branch_anchor[b]));
			const Vector3 from_pivot = p - bone_rest_positions[b];
			const Vector3 b_axis = b < bone_axes.size() ? _safe_unit(bone_axes[b], axis) : axis;
			const float align = MAX(float(_safe_unit(from_pivot, b_axis).dot(b_axis)), 0.0f);
			const float score = (0.35f + 0.65f * align) / (d2 + 0.05f * height * height + 1e-4f);
			if (score > best_score[0]) {
				best_score[2] = best_score[1];
				best_idx[2] = best_idx[1];
				best_score[1] = best_score[0];
				best_idx[1] = best_idx[0];
				best_score[0] = score;
				best_idx[0] = b;
			} else if (score > best_score[1]) {
				best_score[2] = best_score[1];
				best_idx[2] = best_idx[1];
				best_score[1] = score;
				best_idx[1] = b;
			} else if (score > best_score[2]) {
				best_score[2] = score;
				best_idx[2] = b;
			}
		}

		const float score_sum = best_score[0] + best_score[1] + best_score[2];
		const float branch_budget = 1.0f - w_trunk;
		int out_b[4] = { 0, 0, 0, 0 };
		float out_w[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
		if (score_sum > 1e-8f && branch_budget > 1e-4f) {
			out_w[0] = w_trunk;
			for (int k = 0; k < 3; k++) {
				if (best_idx[k] > 0 && best_score[k] > 0.0f) {
					out_b[k + 1] = best_idx[k];
					out_w[k + 1] = branch_budget * (best_score[k] / score_sum);
				}
			}
		}

		// Sort descending by weight and renormalize to 1.0.
		for (int a = 0; a < 3; a++) {
			for (int b = a + 1; b < 4; b++) {
				if (out_w[b] > out_w[a]) {
					SWAP(out_w[a], out_w[b]);
					SWAP(out_b[a], out_b[b]);
				}
			}
		}
		float wsum = out_w[0] + out_w[1] + out_w[2] + out_w[3];
		if (wsum <= 1e-8f) {
			out_b[0] = 0;
			out_w[0] = 1.0f;
			out_w[1] = out_w[2] = out_w[3] = 0.0f;
			wsum = 1.0f;
		}
		for (int k = 0; k < 4; k++) {
			bw[i * 4 + k] = out_w[k] > 0.0f ? out_b[k] : 0;
			ww[i * 4 + k] = out_w[k] / wsum;
		}
	}
}

Dictionary VirtualGeometryWind::compute_vertex_skinning(const PackedVector3Array &p_positions) const {
	PackedInt32Array bones;
	PackedFloat32Array weights;
	compute_vertex_skinning_arrays(p_positions, bones, weights);
	Dictionary result;
	result["bones"] = bones;
	result["weights"] = weights;
	return result;
}

Error VirtualGeometryWind::generate_from_skeleton(const PackedVector3Array &p_bone_rest_positions, const PackedInt32Array &p_bone_parents, const PackedVector3Array &p_positions, const PackedInt32Array &p_src_bones, const PackedFloat32Array &p_src_weights, int p_src_influences, PackedInt32Array &r_remap_table, float p_max_sway) {
	r_remap_table.clear();
	const int src_bone_count = p_bone_rest_positions.size();
	ERR_FAIL_COND_V(src_bone_count <= 0 || p_bone_parents.size() != src_bone_count, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_positions.is_empty(), ERR_INVALID_PARAMETER);
	const int inf = (p_src_influences == 8) ? 8 : 4;
	const int vert_count = p_positions.size();
	const bool has_src_skin = p_src_bones.size() == vert_count * inf && p_src_weights.size() == vert_count * inf;

	const Vector3 axis = Vector3(0.0f, 1.0f, 0.0f);
	Vector3 basis_u, basis_v;
	_build_basis_from_axis(axis, basis_u, basis_v);

	float h_min = FLT_MAX;
	float h_max = -FLT_MAX;
	Vector3 mesh_centroid;
	for (int i = 0; i < vert_count; i++) {
		const float h = float(p_positions[i].dot(axis));
		h_min = MIN(h_min, h);
		h_max = MAX(h_max, h);
		mesh_centroid += p_positions[i];
	}
	mesh_centroid /= real_t(vert_count);
	const float height = MAX(h_max - h_min, 1e-4f);

	// Accumulate per-source-bone vertex influence weight and influenced vertex centroids.
	LocalVector<float> bone_weight_sum;
	LocalVector<Vector3> bone_vert_sum;
	bone_weight_sum.resize(src_bone_count);
	bone_vert_sum.resize(src_bone_count);
	for (int b = 0; b < src_bone_count; b++) {
		bone_weight_sum[b] = 0.0f;
		bone_vert_sum[b] = Vector3();
	}
	if (has_src_skin) {
		for (int v = 0; v < vert_count; v++) {
			for (int k = 0; k < inf; k++) {
				const int b = p_src_bones[v * inf + k];
				const float w = p_src_weights[v * inf + k];
				if (b >= 0 && b < src_bone_count && w > 0.0f) {
					bone_weight_sum[b] += w;
					bone_vert_sum[b] += p_positions[v] * real_t(w);
				}
			}
		}
	}

	// Pick trunk bone (wind bone 0): lowest-height root bone (parent < 0), or lowest-height bone overall.
	int trunk_src = 0;
	bool found_root = false;
	float best_root_h = FLT_MAX;
	for (int b = 0; b < src_bone_count; b++) {
		if (p_bone_parents[b] < 0) {
			const float bh = float(p_bone_rest_positions[b].dot(axis));
			if (!found_root || bh < best_root_h) {
				found_root = true;
				best_root_h = bh;
				trunk_src = b;
			}
		}
	}
	if (!found_root) {
		float lowest_h = FLT_MAX;
		for (int b = 0; b < src_bone_count; b++) {
			const float bh = float(p_bone_rest_positions[b].dot(axis));
			if (bh < lowest_h) {
				lowest_h = bh;
				trunk_src = b;
			}
		}
	}

	const Vector3 trunk_pos = p_bone_rest_positions[trunk_src];
	const Vector3 trunk_perp = trunk_pos - axis * trunk_pos.dot(axis);
	float max_radius = 1e-4f;
	for (int i = 0; i < vert_count; i++) {
		const Vector3 radial = (p_positions[i] - trunk_perp) - axis * p_positions[i].dot(axis);
		max_radius = MAX(max_radius, float(radial.length()));
	}
	const float asset_max_sway = (std::isfinite(p_max_sway) && p_max_sway > 0.0f)
			? p_max_sway
			: CLAMP(0.12f * height + 0.08f * max_radius, 0.05f, 2.0f);

	// Score candidate branch bones (all bones != trunk_src), preferring angularly spread
	// canopy/branch bones with significant vertex weight (<= MAX_BRANCH_BONES = 8).
	struct Candidate {
		int src_idx = -1;
		int sector = 0;
		float score = 0.0f;
	};
	LocalVector<Candidate> candidates;
	for (int b = 0; b < src_bone_count; b++) {
		if (b == trunk_src) {
			continue;
		}
		const Vector3 bp = p_bone_rest_positions[b];
		const Vector3 center = bone_weight_sum[b] > 1e-4f ? (bone_vert_sum[b] / real_t(bone_weight_sum[b])) : bp;
		const Vector3 radial = (center - trunk_perp) - axis * center.dot(axis);
		const float r_norm = CLAMP(float(radial.length()) / max_radius, 0.0f, 1.0f);
		const float t_h = CLAMP((float(center.dot(axis)) - h_min) / height, 0.0f, 1.0f);
		const float angle = std::atan2(float(radial.dot(basis_v)), float(radial.dot(basis_u)));
		const float norm_a = (angle < 0.0f ? angle + float(Math::TAU) : angle) / float(Math::TAU);
		const int sector = CLAMP(int(std::floor(norm_a * float(MAX_BRANCH_BONES))), 0, MAX_BRANCH_BONES - 1);
		const float weight_factor = has_src_skin ? (bone_weight_sum[b] + 1e-3f) : 1.0f;
		const float score = weight_factor * (0.25f + 0.5f * t_h + 0.5f * r_norm);
		candidates.push_back({ b, sector, score });
	}

	LocalVector<int> selected_src;
	selected_src.push_back(trunk_src);
	LocalVector<uint8_t> picked;
	picked.resize(src_bone_count);
	for (int b = 0; b < src_bone_count; b++) {
		picked[b] = 0;
	}
	picked[trunk_src] = 1;

	// First pass: pick highest-scoring candidate per azimuth sector (up to 8).
	for (int s = 0; s < MAX_BRANCH_BONES && int(selected_src.size()) < MAX_BONES; s++) {
		int best_b = -1;
		float best_score = -1.0f;
		for (const Candidate &c : candidates) {
			if (c.sector == s && !picked[c.src_idx] && c.score > best_score) {
				best_score = c.score;
				best_b = c.src_idx;
			}
		}
		if (best_b >= 0) {
			picked[best_b] = 1;
			selected_src.push_back(best_b);
		}
	}
	// Second pass: fill remaining branch slots up to MAX_BONES by highest overall score.
	while (int(selected_src.size()) < MAX_BONES) {
		int best_b = -1;
		float best_score = -1.0f;
		for (const Candidate &c : candidates) {
			if (!picked[c.src_idx] && c.score > best_score) {
				best_score = c.score;
				best_b = c.src_idx;
			}
		}
		if (best_b < 0) {
			break;
		}
		picked[best_b] = 1;
		selected_src.push_back(best_b);
	}

	// Build source-bone -> wind-bone remap table by walking up parent chains.
	r_remap_table.resize(src_bone_count);
	LocalVector<int> direct_map;
	direct_map.resize(src_bone_count);
	for (int b = 0; b < src_bone_count; b++) {
		direct_map[b] = -1;
	}
	for (uint32_t i = 0; i < selected_src.size(); i++) {
		direct_map[selected_src[i]] = int(i);
	}
	for (int b = 0; b < src_bone_count; b++) {
		int cur = b;
		int mapped = 0;
		for (int guard = 0; guard < src_bone_count && cur >= 0 && cur < src_bone_count; guard++) {
			if (direct_map[cur] >= 0) {
				mapped = direct_map[cur];
				break;
			}
			cur = p_bone_parents[cur];
		}
		r_remap_table.write[b] = mapped;
	}

	// Populate wind bones from selected source bones.
	clear();
	trunk_axis = axis;
	base_height = h_min;
	top_height = h_max;
	max_sway = asset_max_sway;

	const int wind_bone_count = int(selected_src.size());
	for (int i = 0; i < wind_bone_count; i++) {
		const int sb = selected_src[i];
		const Vector3 pivot = p_bone_rest_positions[sb];
		const Vector3 target = bone_weight_sum[sb] > 1e-4f ? (bone_vert_sum[sb] / real_t(bone_weight_sum[sb])) : (i == 0 ? mesh_centroid : pivot + axis);
		const Vector3 b_axis = (i == 0) ? axis : _safe_unit(target - pivot, axis);
		const float t_h = CLAMP((float(target.dot(axis)) - h_min) / height, 0.0f, 1.0f);
		const Vector3 radial = (target - trunk_perp) - axis * target.dot(axis);
		const float azimuth = std::atan2(float(radial.dot(basis_v)), float(radial.dot(basis_u)));
		const float stiff = (i == 0) ? 0.70f : CLAMP(0.55f - 0.25f * t_h, 0.20f, 0.70f);
		const float phase = (i == 0) ? 0.0f : (azimuth + 0.7f * t_h);
		const float b_sway = (i == 0) ? asset_max_sway * 0.65f : asset_max_sway;

		bone_rest_positions.push_back(pivot);
		bone_axes.push_back(b_axis);
		bone_stiffness.push_back(stiff);
		bone_sway_phases.push_back(phase);
		bone_max_sway.push_back(b_sway);
		bone_lever_lengths.push_back(MAX(height, 0.25f));
	}

	// Compute exact conservative lever lengths across vertices influencing each remapped wind bone.
	LocalVector<float> max_lever;
	max_lever.resize(wind_bone_count);
	for (int b = 0; b < wind_bone_count; b++) {
		max_lever[b] = 0.25f;
	}
	PackedInt32Array remapped_b;
	PackedFloat32Array remapped_w;
	if (has_src_skin) {
		remap_vertex_skinning(p_src_bones, p_src_weights, inf, r_remap_table, wind_bone_count, remapped_b, remapped_w);
		for (int v = 0; v < vert_count; v++) {
			const Vector3 &p = p_positions[v];
			for (int k = 0; k < SKIN_INFLUENCES; k++) {
				const int wb = remapped_b[v * SKIN_INFLUENCES + k];
				const float ww = remapped_w[v * SKIN_INFLUENCES + k];
				if (ww > 1e-4f && wb >= 0 && wb < wind_bone_count) {
					max_lever[wb] = MAX(max_lever[wb], float(p.distance_to(bone_rest_positions[wb])));
				}
			}
		}
	} else {
		for (int v = 0; v < vert_count; v++) {
			for (int b = 0; b < wind_bone_count; b++) {
				max_lever[b] = MAX(max_lever[b], float(p_positions[v].distance_to(bone_rest_positions[b])));
			}
		}
	}
	for (int b = 0; b < wind_bone_count; b++) {
		bone_lever_lengths.write[b] = max_lever[b];
	}
	compute_bone_physics_from_geometry(p_positions, PackedInt32Array(), remapped_b, remapped_w, SKIN_INFLUENCES);

	emit_changed();
	return OK;
}

PackedInt32Array VirtualGeometryWind::generate_from_skeleton_data(const PackedVector3Array &p_bone_rest_positions, const PackedInt32Array &p_bone_parents, const PackedVector3Array &p_positions, const PackedInt32Array &p_src_bones, const PackedFloat32Array &p_src_weights, int p_src_influences, float p_max_sway) {
	PackedInt32Array remap;
	if (generate_from_skeleton(p_bone_rest_positions, p_bone_parents, p_positions, p_src_bones, p_src_weights, p_src_influences, remap, p_max_sway) != OK) {
		return PackedInt32Array();
	}
	return remap;
}

void VirtualGeometryWind::remap_vertex_skinning(const PackedInt32Array &p_src_bones, const PackedFloat32Array &p_src_weights, int p_src_influences, const PackedInt32Array &p_remap_table, int p_wind_bone_count, PackedInt32Array &r_dst_bones, PackedFloat32Array &r_dst_weights) {
	const int inf = (p_src_influences == 8) ? 8 : 4;
	const int vert_count = MIN(p_src_bones.size(), p_src_weights.size()) / inf;
	r_dst_bones.resize(vert_count * SKIN_INFLUENCES);
	r_dst_weights.resize(vert_count * SKIN_INFLUENCES);
	if (vert_count <= 0) {
		return;
	}
	const int clamped_bones = CLAMP(p_wind_bone_count, 1, MAX_BONES);
	int32_t *bw = r_dst_bones.ptrw();
	float *ww = r_dst_weights.ptrw();

	for (int v = 0; v < vert_count; v++) {
		float accum[MAX_BONES] = {};
		for (int k = 0; k < inf; k++) {
			const int sb = p_src_bones[v * inf + k];
			const float sw = p_src_weights[v * inf + k];
			if (sw <= 0.0f) {
				continue;
			}
			const int wb = (sb >= 0 && sb < p_remap_table.size()) ? CLAMP(int(p_remap_table[sb]), 0, clamped_bones - 1) : 0;
			accum[wb] += sw;
		}
		int top_b[SKIN_INFLUENCES] = { 0, 0, 0, 0 };
		float top_w[SKIN_INFLUENCES] = { 0.0f, 0.0f, 0.0f, 0.0f };
		for (int b = 0; b < clamped_bones; b++) {
			const float w = accum[b];
			if (w <= 0.0f) {
				continue;
			}
			for (int s = 0; s < SKIN_INFLUENCES; s++) {
				if (w > top_w[s]) {
					for (int shift = SKIN_INFLUENCES - 1; shift > s; shift--) {
						top_w[shift] = top_w[shift - 1];
						top_b[shift] = top_b[shift - 1];
					}
					top_w[s] = w;
					top_b[s] = b;
					break;
				}
			}
		}
		float wsum = top_w[0] + top_w[1] + top_w[2] + top_w[3];
		if (wsum <= 1e-8f) {
			top_b[0] = 0;
			top_w[0] = 1.0f;
			top_w[1] = top_w[2] = top_w[3] = 0.0f;
			wsum = 1.0f;
		}
		for (int k = 0; k < SKIN_INFLUENCES; k++) {
			bw[v * SKIN_INFLUENCES + k] = top_w[k] > 0.0f ? top_b[k] : 0;
			ww[v * SKIN_INFLUENCES + k] = top_w[k] / wsum;
		}
	}
}

void VirtualGeometryWind::reset_state() {
	bone_rest_positions.clear();
	bone_axes.clear();
	bone_stiffness.clear();
	bone_sway_phases.clear();
	bone_max_sway.clear();
	bone_lever_lengths.clear();
	bone_masses.clear();
	bone_wind_catches.clear();
	max_sway = 0.0f;
	trunk_axis = Vector3(0.0f, 1.0f, 0.0f);
	base_height = 0.0f;
	top_height = 1.0f;
}

void VirtualGeometryWind::_bind_methods() {
	BIND_CONSTANT(MAX_BRANCH_BONES);
	BIND_CONSTANT(MAX_BONES);
	BIND_CONSTANT(SKIN_INFLUENCES);

	ClassDB::bind_method(D_METHOD("clear"), &VirtualGeometryWind::clear);
	ClassDB::bind_method(D_METHOD("get_bone_count"), &VirtualGeometryWind::get_bone_count);
	ClassDB::bind_method(D_METHOD("add_bone", "rest_position", "axis", "stiffness", "sway_phase", "max_sway", "lever_length", "mass", "wind_catch"), &VirtualGeometryWind::add_bone, DEFVAL(0.5f), DEFVAL(0.0f), DEFVAL(0.25f), DEFVAL(1.0f), DEFVAL(1.0f), DEFVAL(1.0f));

	ClassDB::bind_method(D_METHOD("set_bone_rest_positions", "positions"), &VirtualGeometryWind::set_bone_rest_positions);
	ClassDB::bind_method(D_METHOD("get_bone_rest_positions"), &VirtualGeometryWind::get_bone_rest_positions);
	ClassDB::bind_method(D_METHOD("set_bone_axes", "axes"), &VirtualGeometryWind::set_bone_axes);
	ClassDB::bind_method(D_METHOD("get_bone_axes"), &VirtualGeometryWind::get_bone_axes);
	ClassDB::bind_method(D_METHOD("set_bone_stiffness", "stiffness"), &VirtualGeometryWind::set_bone_stiffness);
	ClassDB::bind_method(D_METHOD("get_bone_stiffness"), &VirtualGeometryWind::get_bone_stiffness);
	ClassDB::bind_method(D_METHOD("set_bone_sway_phases", "phases"), &VirtualGeometryWind::set_bone_sway_phases);
	ClassDB::bind_method(D_METHOD("get_bone_sway_phases"), &VirtualGeometryWind::get_bone_sway_phases);
	ClassDB::bind_method(D_METHOD("set_bone_max_sway", "max_sway"), &VirtualGeometryWind::set_bone_max_sway);
	ClassDB::bind_method(D_METHOD("get_bone_max_sway"), &VirtualGeometryWind::get_bone_max_sway);
	ClassDB::bind_method(D_METHOD("set_bone_lever_lengths", "lever_lengths"), &VirtualGeometryWind::set_bone_lever_lengths);
	ClassDB::bind_method(D_METHOD("get_bone_lever_lengths"), &VirtualGeometryWind::get_bone_lever_lengths);
	ClassDB::bind_method(D_METHOD("set_bone_masses", "masses"), &VirtualGeometryWind::set_bone_masses);
	ClassDB::bind_method(D_METHOD("get_bone_masses"), &VirtualGeometryWind::get_bone_masses);
	ClassDB::bind_method(D_METHOD("set_bone_wind_catches", "wind_catches"), &VirtualGeometryWind::set_bone_wind_catches);
	ClassDB::bind_method(D_METHOD("get_bone_wind_catches"), &VirtualGeometryWind::get_bone_wind_catches);
	ClassDB::bind_method(D_METHOD("compute_bone_physics_from_geometry", "positions", "indices", "bones", "weights", "influences"), &VirtualGeometryWind::compute_bone_physics_from_geometry, DEFVAL(PackedInt32Array()), DEFVAL(PackedInt32Array()), DEFVAL(PackedFloat32Array()), DEFVAL(SKIN_INFLUENCES));

	ClassDB::bind_method(D_METHOD("set_max_sway", "max_sway"), &VirtualGeometryWind::set_max_sway);
	ClassDB::bind_method(D_METHOD("get_max_sway"), &VirtualGeometryWind::get_max_sway);
	ClassDB::bind_method(D_METHOD("set_trunk_axis", "axis"), &VirtualGeometryWind::set_trunk_axis);
	ClassDB::bind_method(D_METHOD("get_trunk_axis"), &VirtualGeometryWind::get_trunk_axis);
	ClassDB::bind_method(D_METHOD("set_base_height", "height"), &VirtualGeometryWind::set_base_height);
	ClassDB::bind_method(D_METHOD("get_base_height"), &VirtualGeometryWind::get_base_height);
	ClassDB::bind_method(D_METHOD("set_top_height", "height"), &VirtualGeometryWind::set_top_height);
	ClassDB::bind_method(D_METHOD("get_top_height"), &VirtualGeometryWind::get_top_height);

	ClassDB::bind_method(D_METHOD("generate_from_geometry", "positions", "trunk_axis", "max_sway"), &VirtualGeometryWind::generate_from_geometry, DEFVAL(Vector3(0.0f, 1.0f, 0.0f)), DEFVAL(-1.0f));
	ClassDB::bind_method(D_METHOD("compute_vertex_skinning", "positions"), &VirtualGeometryWind::compute_vertex_skinning);
	ClassDB::bind_method(D_METHOD("generate_from_skeleton_data", "bone_rest_positions", "bone_parents", "positions", "src_bones", "src_weights", "src_influences", "max_sway"), &VirtualGeometryWind::generate_from_skeleton_data, DEFVAL(4), DEFVAL(-1.0f));

	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR3_ARRAY, "bone_rest_positions"), "set_bone_rest_positions", "get_bone_rest_positions");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR3_ARRAY, "bone_axes"), "set_bone_axes", "get_bone_axes");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "bone_stiffness"), "set_bone_stiffness", "get_bone_stiffness");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "bone_sway_phases"), "set_bone_sway_phases", "get_bone_sway_phases");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "bone_max_sway"), "set_bone_max_sway", "get_bone_max_sway");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "bone_lever_lengths"), "set_bone_lever_lengths", "get_bone_lever_lengths");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "bone_masses"), "set_bone_masses", "get_bone_masses");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_FLOAT32_ARRAY, "bone_wind_catches"), "set_bone_wind_catches", "get_bone_wind_catches");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_sway", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater"), "set_max_sway", "get_max_sway");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "trunk_axis"), "set_trunk_axis", "get_trunk_axis");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "base_height"), "set_base_height", "get_base_height");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "top_height"), "set_top_height", "get_top_height");
}
