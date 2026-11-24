/**************************************************************************/
/*  dynamic_wind_sim.h                                                    */
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

#include "core/math/math_funcs.h"
#include "core/math/vector3.h"
#include "core/string/print_string.h"
#include "core/typedefs.h"
#include "servers/rendering/storage/virtual_mesh_storage.h"

#include <cmath>

// GodotNext: nextgeo_dynamic_wind (#296, plan §21.4).
// Renderer-agnostic clean-room CPU reference, hardware/feature floor evaluation, one-time session
// logger, and per-bone spring + flutter simulation math mirroring shaders/nextgeo/nextgeo_dynamic_wind.glsl.
namespace NextGeoDynamicWindSim {

enum Reason : uint32_t {
	REASON_NONE = 0u,
	REASON_DISABLED = 1u,
	REASON_NEXTGEO_UNAVAILABLE = 2u,
	REASON_BELOW_TIER_FLOOR = 3u,
	REASON_SKINNED_UNAVAILABLE = 4u,
	REASON_FOLIAGE_UNAVAILABLE = 5u,
};

inline const char *reason_name(Reason p_reason) {
	switch (p_reason) {
		case REASON_NONE:
			return "NONE";
		case REASON_DISABLED:
			return "DISABLED";
		case REASON_NEXTGEO_UNAVAILABLE:
			return "NEXTGEO_UNAVAILABLE";
		case REASON_BELOW_TIER_FLOOR:
			return "BELOW_TIER_FLOOR";
		case REASON_SKINNED_UNAVAILABLE:
			return "SKINNED_UNAVAILABLE";
		case REASON_FOLIAGE_UNAVAILABLE:
			return "FOLIAGE_UNAVAILABLE";
	}
	return "UNKNOWN";
}

struct FloorResult {
	bool supported = false;
	Reason reason = REASON_DISABLED;
};

// Dynamic Wind requires the NextGeo device tier (SM6-class: 64-bit atomics + indirect-count),
// skinned NextGeo (#160), and the foliage path (#151, #152). Below this floor plants do not move.
inline FloorResult evaluate_floor(
		bool p_setting_enabled,
		bool p_nextgeo_available,
		bool p_hybrid_tier_supported,
		bool p_skinned_available,
		bool p_foliage_available) {
	FloorResult res;
	if (!p_setting_enabled) {
		res.reason = REASON_DISABLED;
		return res;
	}
	if (!p_nextgeo_available) {
		res.reason = REASON_NEXTGEO_UNAVAILABLE;
		return res;
	}
	if (!p_hybrid_tier_supported) {
		res.reason = REASON_BELOW_TIER_FLOOR;
		return res;
	}
	if (!p_skinned_available) {
		res.reason = REASON_SKINNED_UNAVAILABLE;
		return res;
	}
	if (!p_foliage_available) {
		res.reason = REASON_FOLIAGE_UNAVAILABLE;
		return res;
	}
	res.supported = true;
	res.reason = REASON_NONE;
	return res;
}

class FloorLogger {
	uint32_t logged_mask = 0u;
	uint32_t log_count = 0u;
	Reason last_reason = REASON_DISABLED;

public:
	// Logs a below-floor reason at most once per reason per session.
	// Returns true iff this invocation emitted a new one-time log entry.
	bool record_and_log_once(Reason p_reason, bool p_emit_print = true) {
		last_reason = p_reason;
		if (p_reason == REASON_NONE || p_reason == REASON_DISABLED) {
			return false;
		}
		const uint32_t bit = 1u << uint32_t(p_reason);
		if ((logged_mask & bit) != 0u) {
			return false;
		}
		logged_mask |= bit;
		log_count++;
		if (p_emit_print) {
			print_line(vformat("NextGeo Dynamic Wind inactive (%s): requires NextGeo device tier (64-bit atomics + indirect-count), skinned NextGeo, and foliage path; plants will remain static.", reason_name(p_reason)));
		}
		return true;
	}

	void reset() {
		logged_mask = 0u;
		log_count = 0u;
		last_reason = REASON_DISABLED;
	}

	uint32_t get_log_count() const { return log_count; }
	Reason get_last_reason() const { return last_reason; }
	bool has_logged(Reason p_reason) const {
		return (logged_mask & (1u << uint32_t(p_reason))) != 0u;
	}
};

// Shared std430 simulation parameters (48 B) matching nextgeo_dynamic_wind.glsl.
struct SimParamsGPU {
	float wind_dir_strength[4] = { 1.0f, 0.0f, 0.0f, 0.0f }; // xyz = normalized global wind dir, w = strength
	float time_dt_gust_pad[4] = { 0.0f, 0.0f, 1.0f, 0.0f }; // x = cur_time, y = dt, z = gust_frequency, w = pad
	uint32_t meta[4] = { 0u, 0u, 0u, 0u }; // x = slot_count, y = wind_mesh_count, z = local_force_count, w = flags
};
static_assert(sizeof(SimParamsGPU) == 48, "SimParamsGPU must match GLSL std430 layout (48 bytes).");

inline Vector3 safe_unit(const Vector3 &p_v, const Vector3 &p_fallback) {
	const float len = std::sqrt(float(p_v.x * p_v.x + p_v.y * p_v.y + p_v.z * p_v.z));
	if (len > 1.0e-5f) {
		return Vector3(float(p_v.x) / len, float(p_v.y) / len, float(p_v.z) / len);
	}
	return p_fallback;
}

inline void bone_basis(const Vector3 &p_baxis, Vector3 &r_tan_u, Vector3 &r_tan_v) {
	const Vector3 ref = std::abs(float(p_baxis.y)) < 0.9f ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
	r_tan_u = safe_unit(p_baxis.cross(ref), Vector3(1.0f, 0.0f, 0.0f));
	r_tan_v = safe_unit(p_baxis.cross(r_tan_u), Vector3(0.0f, 0.0f, 1.0f));
}

// Builds a 3x4 row-major Rodrigues rotation matrix pivoted at `p_pivot` that bends `p_baxis`
// along `(p_angle_u * tan_u + p_angle_v * tan_v)`. Mirrors nextgeo_dynamic_wind.glsl bit-for-bit.
inline void build_bone_matrix(
		const Vector3 &p_pivot,
		const Vector3 &p_baxis,
		const Vector3 &p_tan_u,
		const Vector3 &p_tan_v,
		float p_angle_u,
		float p_angle_v,
		float r_rows[12]) {
	r_rows[0] = 1.0f;
	r_rows[1] = 0.0f;
	r_rows[2] = 0.0f;
	r_rows[3] = 0.0f;
	r_rows[4] = 0.0f;
	r_rows[5] = 1.0f;
	r_rows[6] = 0.0f;
	r_rows[7] = 0.0f;
	r_rows[8] = 0.0f;
	r_rows[9] = 0.0f;
	r_rows[10] = 1.0f;
	r_rows[11] = 0.0f;

	const float theta = std::sqrt(p_angle_u * p_angle_u + p_angle_v * p_angle_v);
	if (!(theta > 1.0e-6f)) {
		return;
	}
	const Vector3 bend_dir = (p_tan_u * p_angle_u + p_tan_v * p_angle_v) * (1.0f / theta);
	const Vector3 raxis = safe_unit(p_baxis.cross(bend_dir), p_tan_v);

	const float rx = float(raxis.x);
	const float ry = float(raxis.y);
	const float rz = float(raxis.z);
	const float c = std::cos(theta);
	const float s = std::sin(theta);
	const float omc = 1.0f - c;

	const float r00 = c + rx * rx * omc;
	const float r01 = rx * ry * omc - rz * s;
	const float r02 = rx * rz * omc + ry * s;
	const float r10 = ry * rx * omc + rz * s;
	const float r11 = c + ry * ry * omc;
	const float r12 = ry * rz * omc - rx * s;
	const float r20 = rz * rx * omc - ry * s;
	const float r21 = rz * ry * omc + rx * s;
	const float r22 = c + rz * rz * omc;

	const float px = float(p_pivot.x);
	const float py = float(p_pivot.y);
	const float pz = float(p_pivot.z);

	r_rows[0] = r00;
	r_rows[1] = r01;
	r_rows[2] = r02;
	r_rows[3] = px - (r00 * px + r01 * py + r02 * pz);

	r_rows[4] = r10;
	r_rows[5] = r11;
	r_rows[6] = r12;
	r_rows[7] = py - (r10 * px + r11 * py + r12 * pz);

	r_rows[8] = r20;
	r_rows[9] = r21;
	r_rows[10] = r22;
	r_rows[11] = pz - (r20 * px + r21 * py + r22 * pz);
}

// CPU reference matching `nextgeo_dynamic_wind.glsl` per-bone spring + flutter simulation.
inline void step_bone(
		const RendererVirtualMeshStorage::WindBoneGPU &p_bone,
		const RendererVirtualMeshStorage::DynamicWindSlotDescGPU &p_slot,
		const Vector3 &p_global_wind_dir,
		float p_global_wind_strength,
		float p_gust_frequency,
		float p_time,
		float p_dt,
		const RendererVirtualMeshStorage::WindForceGPU *p_forces,
		uint32_t p_force_count,
		RendererVirtualMeshStorage::DynamicWindBoneStateGPU &r_state,
		float r_cur_rows[12],
		float r_prev_rows[12]) {
	const Vector3 pivot(p_bone.rest_pos[0], p_bone.rest_pos[1], p_bone.rest_pos[2]);
	const float lever = MAX(p_bone.lever_length, 0.25f);
	const Vector3 baxis = safe_unit(Vector3(p_bone.axis[0], p_bone.axis[1], p_bone.axis[2]), Vector3(0.0f, 1.0f, 0.0f));
	const float stiff = CLAMP(p_bone.stiffness, 0.0f, 1.0f);
	const float max_sway = MAX(p_bone.max_sway, 0.0f);
	const float sway_phase = p_bone.sway_phase;
	const float branch_w = CLAMP(p_bone.branch_weight, 0.0f, 1.0f);
	const float mass = MAX(p_bone.mass, 0.05f);
	const float wind_catch = MAX(p_bone.wind_catch, 0.01f);

	Vector3 tan_u;
	Vector3 tan_v;
	bone_basis(baxis, tan_u, tan_v);

	const float max_angle = CLAMP(max_sway / lever, 0.0f, 1.04719755f);
	if (!(max_angle > 1.0e-6f)) {
		build_bone_matrix(pivot, baxis, tan_u, tan_v, 0.0f, 0.0f, r_cur_rows);
		build_bone_matrix(pivot, baxis, tan_u, tan_v, 0.0f, 0.0f, r_prev_rows);
		r_state.sway_u = 0.0f;
		r_state.vel_u = 0.0f;
		r_state.sway_v = 0.0f;
		r_state.vel_v = 0.0f;
		r_state.prev_total_u = 0.0f;
		r_state.prev_total_v = 0.0f;
		r_state.initialized = 1.0f;
		return;
	}

	const float dt = CLAMP(Math::is_finite(p_dt) ? p_dt : 0.0f, 0.0f, 0.05f);
	const Vector3 gdir = safe_unit(p_global_wind_dir, Vector3(1.0f, 0.0f, 0.0f));
	const float gstr = MAX(p_global_wind_strength, 0.0f);
	const float gfreq = MAX(p_gust_frequency, 0.05f);
	const float slot_phase = p_slot.phase_offset;

	const Vector3 g_ref = std::abs(float(gdir.y)) < 0.9f ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
	const Vector3 g_side = safe_unit(gdir.cross(g_ref), Vector3(0.0f, 0.0f, 1.0f));
	const float gust_wave = 0.65f + 0.35f * std::sin(p_time * 6.28318530718f * gfreq * 0.18f + slot_phase + sway_phase * 0.5f);
	const float gust_cross = 0.18f * std::sin(p_time * 6.28318530718f * gfreq * 0.31f + slot_phase * 1.37f + sway_phase);
	Vector3 world_wind = (gdir * gust_wave + g_side * gust_cross) * gstr;

	if ((p_slot.flags & RendererVirtualMeshStorage::DYNAMIC_WIND_SLOT_FLAG_PROMOTED) != 0u && p_forces != nullptr && p_force_count > 0u) {
		const Vector3 local_mid = pivot + baxis * (lever * 0.5f);
		const Vector3 bone_world(
				p_slot.transform[0][0] * float(local_mid.x) + p_slot.transform[0][1] * float(local_mid.y) + p_slot.transform[0][2] * float(local_mid.z) + p_slot.transform[0][3],
				p_slot.transform[1][0] * float(local_mid.x) + p_slot.transform[1][1] * float(local_mid.y) + p_slot.transform[1][2] * float(local_mid.z) + p_slot.transform[1][3],
				p_slot.transform[2][0] * float(local_mid.x) + p_slot.transform[2][1] * float(local_mid.y) + p_slot.transform[2][2] * float(local_mid.z) + p_slot.transform[2][3]);
		for (uint32_t f = 0u; f < p_force_count; f++) {
			const RendererVirtualMeshStorage::WindForceGPU &wf = p_forces[f];
			const float rad = wf.position_radius[3];
			if (!(rad > 1.0e-4f) || !(wf.direction_strength[3] > 0.0f)) {
				continue;
			}
			const Vector3 delta(
					float(bone_world.x) - wf.position_radius[0],
					float(bone_world.y) - wf.position_radius[1],
					float(bone_world.z) - wf.position_radius[2]);
			const float dist = std::sqrt(float(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z));
			if (dist < rad) {
				const float t = CLAMP(1.0f - dist / rad, 0.0f, 1.0f);
				const float atten = std::pow(t, MAX(wf.falloff_flags[0], 0.05f));
				Vector3 fdir;
				if (wf.falloff_flags[1] > 0.5f) {
					fdir = dist > 1.0e-4f ? Vector3(float(delta.x) / dist, float(delta.y) / dist, float(delta.z) / dist) : Vector3(1.0f, 0.0f, 0.0f);
				} else {
					fdir = Vector3(wf.direction_strength[0], wf.direction_strength[1], wf.direction_strength[2]);
				}
				world_wind += fdir * (wf.direction_strength[3] * atten);
			}
		}
	}

	const Vector3 c0(p_slot.transform[0][0], p_slot.transform[1][0], p_slot.transform[2][0]);
	const Vector3 c1(p_slot.transform[0][1], p_slot.transform[1][1], p_slot.transform[2][1]);
	const Vector3 c2(p_slot.transform[0][2], p_slot.transform[1][2], p_slot.transform[2][2]);
	Vector3 local_wind(
			float(c0.dot(world_wind)) / MAX(float(c0.dot(c0)), 1.0e-8f),
			float(c1.dot(world_wind)) / MAX(float(c1.dot(c1)), 1.0e-8f),
			float(c2.dot(world_wind)) / MAX(float(c2.dot(c2)), 1.0e-8f));
	const float ww_len = std::sqrt(float(world_wind.x * world_wind.x + world_wind.y * world_wind.y + world_wind.z * world_wind.z));
	const float lw_len = std::sqrt(float(local_wind.x * local_wind.x + local_wind.y * local_wind.y + local_wind.z * local_wind.z));
	if (lw_len > 1.0e-6f) {
		local_wind *= (ww_len / lw_len);
	} else {
		local_wind = Vector3();
	}

	const float wind_u = float(local_wind.dot(tan_u));
	const float wind_v = float(local_wind.dot(tan_v));
	const float wind_speed = ww_len;

	const float k_spring = 4.0f + (22.0f - 4.0f) * stiff;
	const float omega0 = CLAMP(std::sqrt(k_spring / mass), 0.75f, 16.0f);
	const float zeta = 0.26f + (0.52f - 0.26f) * stiff;
	const float damping = 2.0f * zeta * omega0;

	const float force_scale = (wind_catch * 2.5f) / (MAX(lever * 0.5f, 0.25f) * k_spring);
	const float raw_u = wind_u * force_scale;
	const float raw_v = wind_v * force_scale;
	const float raw_len = std::sqrt(raw_u * raw_u + raw_v * raw_v);
	const float sway_cap = max_angle * 0.85f;
	float target_u = 0.0f;
	float target_v = 0.0f;
	if (raw_len > 1.0e-6f && sway_cap > 1.0e-6f) {
		const float sat = sway_cap * (raw_len / (sway_cap + raw_len));
		target_u = (raw_u / raw_len) * sat;
		target_v = (raw_v / raw_len) * sat;
	}

	float sway_u = r_state.sway_u;
	float vel_u = r_state.vel_u;
	float sway_v = r_state.sway_v;
	float vel_v = r_state.vel_v;
	if (dt > 0.0f) {
		const float sub_dt = dt * 0.5f;
		const float omega2 = omega0 * omega0;
		for (int s = 0; s < 2; s++) {
			const float acc_u = (target_u - sway_u) * omega2 - vel_u * damping;
			const float acc_v = (target_v - sway_v) * omega2 - vel_v * damping;
			vel_u += acc_u * sub_dt;
			vel_v += acc_v * sub_dt;
			sway_u += vel_u * sub_dt;
			sway_v += vel_v * sub_dt;
		}
	}
	const float sway_len = std::sqrt(sway_u * sway_u + sway_v * sway_v);
	if (sway_len > max_angle && sway_len > 1.0e-6f) {
		const float sc = max_angle / sway_len;
		sway_u *= sc;
		sway_v *= sc;
	}

	const float flutter_freq = (2.8f + 1.8f * branch_w) * (1.0f + 0.35f * MIN(wind_speed, 10.0f)) * (1.2f - 0.4f * stiff);
	float flutter_phase = r_state.flutter_phase + dt * 6.28318530718f * flutter_freq;
	constexpr float PHASE_WRAP = 6.28318530718f * 1024.0f;
	if (flutter_phase > PHASE_WRAP) {
		flutter_phase -= PHASE_WRAP * std::floor(flutter_phase / PHASE_WRAP);
	}
	const float aero = wind_speed * wind_catch / mass;
	const float flutter_norm = aero / (2.5f + aero);
	const float flutter_amp = max_angle * (0.04f + 0.14f * branch_w) * flutter_norm;
	const float fl_u = std::sin(flutter_phase + sway_phase * 1.7f + slot_phase) * flutter_amp;
	const float fl_v = std::cos(flutter_phase * 1.37f + sway_phase * 2.3f + slot_phase * 0.7f) * flutter_amp;

	float total_u = sway_u + fl_u;
	float total_v = sway_v + fl_v;
	const float total_len = std::sqrt(total_u * total_u + total_v * total_v);
	if (total_len > max_angle && total_len > 1.0e-6f) {
		const float sc = max_angle / total_len;
		total_u *= sc;
		total_v *= sc;
	}

	const float prev_u = (r_state.initialized < 0.5f) ? total_u : r_state.prev_total_u;
	const float prev_v = (r_state.initialized < 0.5f) ? total_v : r_state.prev_total_v;

	build_bone_matrix(pivot, baxis, tan_u, tan_v, total_u, total_v, r_cur_rows);
	build_bone_matrix(pivot, baxis, tan_u, tan_v, prev_u, prev_v, r_prev_rows);

	r_state.sway_u = sway_u;
	r_state.vel_u = vel_u;
	r_state.sway_v = sway_v;
	r_state.vel_v = vel_v;
	r_state.prev_total_u = total_u;
	r_state.prev_total_v = total_v;
	r_state.flutter_phase = flutter_phase;
	r_state.initialized = 1.0f;
}

inline bool force_overlaps_aabb(
		const RendererVirtualMeshStorage::WindForceGPU &p_force,
		const Vector3 &p_aabb_min,
		const Vector3 &p_aabb_max,
		float p_margin = 0.0f) {
	const float rad = p_force.position_radius[3];
	if (!(rad > 1.0e-4f) || !(p_force.direction_strength[3] > 0.0f)) {
		return false;
	}
	const float m = MAX(p_margin, 0.0f);
	const float cx = CLAMP(p_force.position_radius[0], float(p_aabb_min.x) - m, float(p_aabb_max.x) + m);
	const float cy = CLAMP(p_force.position_radius[1], float(p_aabb_min.y) - m, float(p_aabb_max.y) + m);
	const float cz = CLAMP(p_force.position_radius[2], float(p_aabb_min.z) - m, float(p_aabb_max.z) + m);
	const float dx = p_force.position_radius[0] - cx;
	const float dy = p_force.position_radius[1] - cy;
	const float dz = p_force.position_radius[2] - cz;
	return (dx * dx + dy * dy + dz * dz) <= rad * rad;
}

} // namespace NextGeoDynamicWindSim
