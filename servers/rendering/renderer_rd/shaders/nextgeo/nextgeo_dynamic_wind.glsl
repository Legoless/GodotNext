#[compute]
#version 460
#VERSION_DEFINES

// GodotNext: nextgeo_dynamic_wind (#296, plan §21.4).
// Dedicated GPU per-bone spring + flutter wind simulation compute pass for skinned NextGeo foliage.
// Steps each active simulation slot's bones (shared pool + local-force-promoted dedicated slots),
// reads/writes ping-pong spring state, and writes current + previous 3x4 bone transforms directly
// into #160's skin palette arena so all NextGeo cull, SW/HW/mesh/programmable raster, resolve,
// velocity, and VSM passes consume simulated bones with zero extra shader bindings.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

// clang-format off
#include "nextgeo_records_inc.glsl"
// clang-format on

const uint SLOT_FLAG_ACTIVE = 1u << 0;
const uint SLOT_FLAG_PROMOTED = 1u << 1;
const uint SLOT_FLAG_RESET = 1u << 2;
const uint SLOT_FLAG_WARM_START = 1u << 3;

struct DynamicWindSlotDesc {
	vec4 transform[3]; // 48 B: instance world transform (identity for shared slots)
	uint mesh_index; // 48
	uint bone_count; // 52
	uint palette_bone_offset; // 56: bone index in skin_palette (cur = offset, prev = offset + bone_count)
	uint flags; // 60
	int warm_start_slot; // 64
	float phase_offset; // 68
	float pad0; // 72
	float pad1; // 76
}; // 80 B

struct DynamicWindForce {
	vec4 position_radius; // xyz = world pos, w = radius
	vec4 direction_strength; // xyz = normalized dir (or 0 for radial), w = strength
	vec4 falloff_flags; // x = falloff, y = is_radial (>0.5 radial), zw = pad
}; // 48 B

struct DynamicWindBoneState {
	vec4 sway_vel; // x = sway_u, y = vel_u, z = sway_v, w = vel_v
	vec4 prev_flutter_init; // x = prev_total_u, y = prev_total_v, z = flutter_phase, w = initialized
}; // 32 B

layout(set = 0, binding = 0, std430) buffer restrict readonly DynamicWindParams {
	vec4 wind_dir_strength; // xyz = normalized global wind dir, w = strength
	vec4 time_dt_gust_pad; // x = cur_time, y = dt, z = gust_frequency, w = pad
	uvec4 meta; // x = slot_count, y = wind_mesh_count, z = local_force_count, w = flags
}
params;

layout(set = 0, binding = 1, std430) buffer restrict readonly DynamicWindSlots {
	DynamicWindSlotDesc data[];
}
sim_slots;

layout(set = 0, binding = 2, std430) buffer restrict readonly NextGeoWindMeshes {
	NextGeoWindMesh data[];
}
wind_meshes;

layout(set = 0, binding = 3, std430) buffer restrict readonly DynamicWindForces {
	DynamicWindForce data[];
}
local_forces;

layout(set = 0, binding = 4, std430) buffer restrict readonly DynamicWindStateIn {
	DynamicWindBoneState data[];
}
state_in;

layout(set = 0, binding = 5, std430) buffer restrict writeonly DynamicWindStateOut {
	DynamicWindBoneState data[];
}
state_out;

layout(set = 0, binding = 6, std430) buffer restrict writeonly NextGeoSkinPalette {
	vec4 data[];
}
skin_palette;

vec3 safe_unit(vec3 p_v, vec3 p_fallback) {
	float len = length(p_v);
	return len > 1.0e-5 ? (p_v / len) : p_fallback;
}

void build_bone_matrix(vec3 p_pivot, vec3 p_baxis, vec3 p_tan_u, vec3 p_tan_v, float p_angle_u, float p_angle_v, out vec4 r_r0, out vec4 r_r1, out vec4 r_r2) {
	r_r0 = vec4(1.0, 0.0, 0.0, 0.0);
	r_r1 = vec4(0.0, 1.0, 0.0, 0.0);
	r_r2 = vec4(0.0, 0.0, 1.0, 0.0);

	float theta = length(vec2(p_angle_u, p_angle_v));
	if (!(theta > 1.0e-6)) {
		return;
	}
	vec3 bend_dir = (p_tan_u * p_angle_u + p_tan_v * p_angle_v) / theta;
	vec3 raxis = safe_unit(cross(p_baxis, bend_dir), p_tan_v);

	float c = cos(theta);
	float s = sin(theta);
	float omc = 1.0 - c;
	vec3 row0 = vec3(c + raxis.x * raxis.x * omc, raxis.x * raxis.y * omc - raxis.z * s, raxis.x * raxis.z * omc + raxis.y * s);
	vec3 row1 = vec3(raxis.y * raxis.x * omc + raxis.z * s, c + raxis.y * raxis.y * omc, raxis.y * raxis.z * omc - raxis.x * s);
	vec3 row2 = vec3(raxis.z * raxis.x * omc - raxis.y * s, raxis.z * raxis.y * omc + raxis.x * s, c + raxis.z * raxis.z * omc);

	r_r0 = vec4(row0, p_pivot.x - dot(row0, p_pivot));
	r_r1 = vec4(row1, p_pivot.y - dot(row1, p_pivot));
	r_r2 = vec4(row2, p_pivot.z - dot(row2, p_pivot));
}

void main() {
	uint tid = gl_GlobalInvocationID.x;
	uint total_threads = params.meta.x * NEXTGEO_WIND_MAX_BONES;
	if (tid >= total_threads) {
		return;
	}

	uint slot_id = tid / NEXTGEO_WIND_MAX_BONES;
	uint bone_idx = tid - slot_id * NEXTGEO_WIND_MAX_BONES;

	DynamicWindSlotDesc slot = sim_slots.data[slot_id];
	if ((slot.flags & SLOT_FLAG_ACTIVE) == 0u || bone_idx >= slot.bone_count || slot.mesh_index >= params.meta.y) {
		return;
	}

	NextGeoWindMesh wmesh = wind_meshes.data[slot.mesh_index];
	if (bone_idx >= wmesh.header.x) {
		return;
	}
	NextGeoWindBone bone = wmesh.bones[bone_idx];

	vec3 pivot = bone.rest_pos_lever.xyz;
	float lever = max(bone.rest_pos_lever.w, 0.25);
	vec3 baxis = safe_unit(bone.axis_stiffness.xyz, vec3(0.0, 1.0, 0.0));
	float stiff = clamp(bone.axis_stiffness.w, 0.0, 1.0);
	float max_sway = max(bone.sway_phase_branch.x, 0.0);
	float sway_phase = bone.sway_phase_branch.y;
	float branch_w = clamp(bone.sway_phase_branch.z, 0.0, 1.0);
	float mass = max(bone.physics.x, 0.05);
	float wind_catch = max(bone.physics.y, 0.01);

	vec3 ref_v = abs(baxis.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 tan_u = safe_unit(cross(baxis, ref_v), vec3(1.0, 0.0, 0.0));
	vec3 tan_v = safe_unit(cross(baxis, tan_u), vec3(0.0, 0.0, 1.0));

	uint dst_cur_base = (slot.palette_bone_offset + bone_idx) * 3u;
	uint dst_prev_base = (slot.palette_bone_offset + slot.bone_count + bone_idx) * 3u;

	float max_angle = clamp(max_sway / lever, 0.0, 1.04719755);
	if (!(max_angle > 1.0e-6)) {
		vec4 r0 = vec4(1.0, 0.0, 0.0, 0.0);
		vec4 r1 = vec4(0.0, 1.0, 0.0, 0.0);
		vec4 r2 = vec4(0.0, 0.0, 1.0, 0.0);
		skin_palette.data[dst_cur_base + 0u] = r0;
		skin_palette.data[dst_cur_base + 1u] = r1;
		skin_palette.data[dst_cur_base + 2u] = r2;
		skin_palette.data[dst_prev_base + 0u] = r0;
		skin_palette.data[dst_prev_base + 1u] = r1;
		skin_palette.data[dst_prev_base + 2u] = r2;
		DynamicWindBoneState zero_st;
		zero_st.sway_vel = vec4(0.0);
		zero_st.prev_flutter_init = vec4(0.0, 0.0, 0.0, 1.0);
		state_out.data[tid] = zero_st;
		return;
	}

	DynamicWindBoneState st;
	if ((slot.flags & SLOT_FLAG_RESET) != 0u) {
		st.sway_vel = vec4(0.0);
		st.prev_flutter_init = vec4(0.0);
	} else if ((slot.flags & SLOT_FLAG_WARM_START) != 0u && slot.warm_start_slot >= 0 && uint(slot.warm_start_slot) < params.meta.x) {
		uint src_tid = uint(slot.warm_start_slot) * NEXTGEO_WIND_MAX_BONES + bone_idx;
		st = state_in.data[src_tid];
	} else {
		st = state_in.data[tid];
	}

	float cur_time = params.time_dt_gust_pad.x;
	float dt = clamp(params.time_dt_gust_pad.y, 0.0, 0.05);
	float gfreq = max(params.time_dt_gust_pad.z, 0.05);
	vec3 gdir = safe_unit(params.wind_dir_strength.xyz, vec3(1.0, 0.0, 0.0));
	float gstr = max(params.wind_dir_strength.w, 0.0);
	float slot_phase = slot.phase_offset;

	vec3 g_ref = abs(gdir.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 g_side = safe_unit(cross(gdir, g_ref), vec3(0.0, 0.0, 1.0));
	float gust_wave = 0.65 + 0.35 * sin(cur_time * 6.28318530718 * gfreq * 0.18 + slot_phase + sway_phase * 0.5);
	float gust_cross = 0.18 * sin(cur_time * 6.28318530718 * gfreq * 0.31 + slot_phase * 1.37 + sway_phase);
	vec3 world_wind = (gdir * gust_wave + g_side * gust_cross) * gstr;

	if ((slot.flags & SLOT_FLAG_PROMOTED) != 0u && params.meta.z > 0u) {
		vec3 local_mid = pivot + baxis * (lever * 0.5);
		vec3 bone_world = vec3(
				dot(slot.transform[0].xyz, local_mid) + slot.transform[0].w,
				dot(slot.transform[1].xyz, local_mid) + slot.transform[1].w,
				dot(slot.transform[2].xyz, local_mid) + slot.transform[2].w);
		for (uint f = 0u; f < params.meta.z; f++) {
			DynamicWindForce wf = local_forces.data[f];
			float rad = wf.position_radius.w;
			if (!(rad > 1.0e-4) || !(wf.direction_strength.w > 0.0)) {
				continue;
			}
			vec3 delta = bone_world - wf.position_radius.xyz;
			float dist = length(delta);
			if (dist < rad) {
				float t = clamp(1.0 - dist / rad, 0.0, 1.0);
				float atten = pow(t, max(wf.falloff_flags.x, 0.05));
				vec3 fdir = (wf.falloff_flags.y > 0.5)
						? (dist > 1.0e-4 ? (delta / dist) : vec3(1.0, 0.0, 0.0))
						: wf.direction_strength.xyz;
				world_wind += fdir * (wf.direction_strength.w * atten);
			}
		}
	}

	vec3 c0 = vec3(slot.transform[0].x, slot.transform[1].x, slot.transform[2].x);
	vec3 c1 = vec3(slot.transform[0].y, slot.transform[1].y, slot.transform[2].y);
	vec3 c2 = vec3(slot.transform[0].z, slot.transform[1].z, slot.transform[2].z);
	vec3 local_wind = vec3(
			dot(c0, world_wind) / max(dot(c0, c0), 1.0e-8),
			dot(c1, world_wind) / max(dot(c1, c1), 1.0e-8),
			dot(c2, world_wind) / max(dot(c2, c2), 1.0e-8));
	float ww_len = length(world_wind);
	float lw_len = length(local_wind);
	if (lw_len > 1.0e-6) {
		local_wind *= (ww_len / lw_len);
	} else {
		local_wind = vec3(0.0);
	}

	vec2 wind_uv = vec2(dot(local_wind, tan_u), dot(local_wind, tan_v));
	float wind_speed = ww_len;

	// Slow sway: damped harmonic spring driven by wind_catch / mass and bone stiffness.
	float k_spring = mix(4.0, 22.0, stiff);
	float omega0 = clamp(sqrt(k_spring / mass), 0.75, 16.0);
	float zeta = mix(0.26, 0.52, stiff);
	float damping = 2.0 * zeta * omega0;

	vec2 raw_target = wind_uv * ((wind_catch * 2.5) / (max(lever * 0.5, 0.25) * k_spring));
	float raw_len = length(raw_target);
	float sway_cap = max_angle * 0.85;
	vec2 target_angle = (raw_len > 1.0e-6 && sway_cap > 1.0e-6)
			? (raw_target / raw_len) * (sway_cap * (raw_len / (sway_cap + raw_len)))
			: vec2(0.0);

	vec2 sway = vec2(st.sway_vel.x, st.sway_vel.z);
	vec2 vel = vec2(st.sway_vel.y, st.sway_vel.w);
	if (dt > 0.0) {
		float sub_dt = dt * 0.5;
		float omega2 = omega0 * omega0;
		for (int s = 0; s < 2; s++) {
			vec2 acc = (target_angle - sway) * omega2 - vel * damping;
			vel += acc * sub_dt;
			sway += vel * sub_dt;
		}
	}
	float sway_len = length(sway);
	if (sway_len > max_angle && sway_len > 1.0e-6) {
		sway *= (max_angle / sway_len);
	}

	// Fast flutter: high-frequency aerodynamic perturbation scaled by wind speed and wind_catch / mass.
	float flutter_freq = (2.8 + 1.8 * branch_w) * (1.0 + 0.35 * min(wind_speed, 10.0)) * (1.2 - 0.4 * stiff);
	float flutter_phase = st.prev_flutter_init.z + dt * 6.28318530718 * flutter_freq;
	const float PHASE_WRAP = 6.28318530718 * 1024.0;
	if (flutter_phase > PHASE_WRAP) {
		flutter_phase -= PHASE_WRAP * floor(flutter_phase / PHASE_WRAP);
	}
	float aero = wind_speed * wind_catch / mass;
	float flutter_norm = aero / (2.5 + aero);
	float flutter_amp = max_angle * (0.04 + 0.14 * branch_w) * flutter_norm;
	vec2 flutter_uv = vec2(
			sin(flutter_phase + sway_phase * 1.7 + slot_phase),
			cos(flutter_phase * 1.37 + sway_phase * 2.3 + slot_phase * 0.7)) * flutter_amp;

	vec2 total = sway + flutter_uv;
	float total_len = length(total);
	if (total_len > max_angle && total_len > 1.0e-6) {
		total *= (max_angle / total_len);
	}

	vec2 prev_total = (st.prev_flutter_init.w < 0.5) ? total : st.prev_flutter_init.xy;

	vec4 cur_r0, cur_r1, cur_r2;
	vec4 prev_r0, prev_r1, prev_r2;
	build_bone_matrix(pivot, baxis, tan_u, tan_v, total.x, total.y, cur_r0, cur_r1, cur_r2);
	build_bone_matrix(pivot, baxis, tan_u, tan_v, prev_total.x, prev_total.y, prev_r0, prev_r1, prev_r2);

	skin_palette.data[dst_cur_base + 0u] = cur_r0;
	skin_palette.data[dst_cur_base + 1u] = cur_r1;
	skin_palette.data[dst_cur_base + 2u] = cur_r2;
	skin_palette.data[dst_prev_base + 0u] = prev_r0;
	skin_palette.data[dst_prev_base + 1u] = prev_r1;
	skin_palette.data[dst_prev_base + 2u] = prev_r2;

	DynamicWindBoneState out_st;
	out_st.sway_vel = vec4(sway.x, vel.x, sway.y, vel.y);
	out_st.prev_flutter_init = vec4(total.x, total.y, flutter_phase, 1.0);
	state_out.data[tid] = out_st;
}
