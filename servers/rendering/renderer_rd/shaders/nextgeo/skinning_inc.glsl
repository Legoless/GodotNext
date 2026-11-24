// GodotNext: nextgeo_skinned_runtime (#160, plan §21.4 contract N8, GN-5, GN-12).
// Shared GPU linear-blend skinning and bone-bound union helpers for NextGeo culling,
// SW/HW/mesh rasterization, visibility resolve, velocity, and VSM rasterization.
//
// Callers declare the three readonly SSBOs before including this header:
//   nextgeo_skin_palette   (vec4 data[]; 3 vec4 rows per bone, matching skeleton.glsl)
//   nextgeo_skin_instances (uvec4 data[]; x=cur_vec4_off, y=prev_vec4_off, z=bone_count, w=max_scale_bits)
//   nextgeo_cluster_skin   (uint data[]; page-arena byte offset of cluster skin block, or 0xFFFFFFFFu)
// and may define NEXTGEO_SKIN_PAGES_DATA if their page-arena SSBO is not named `pages`.

#ifndef NEXTGEO_SKIN_PAGES_DATA
#define NEXTGEO_SKIN_PAGES_DATA pages.data
#endif

#ifndef NEXTGEO_SKIN_PALETTE_DATA
#define NEXTGEO_SKIN_PALETTE_DATA nextgeo_skin_palette.data
#endif

#ifndef NEXTGEO_SKIN_INSTANCES_DATA
#define NEXTGEO_SKIN_INSTANCES_DATA nextgeo_skin_instances.data
#endif

#ifndef NEXTGEO_CLUSTER_SKIN_DATA
#define NEXTGEO_CLUSTER_SKIN_DATA nextgeo_cluster_skin.data
#endif

uint nextgeo_skin_read_u8(uint p_byte_offset) {
	return (NEXTGEO_SKIN_PAGES_DATA[p_byte_offset >> 2u] >> ((p_byte_offset & 3u) * 8u)) & 0xFFu;
}

uint nextgeo_skin_read_u16(uint p_byte_offset) {
	uint word_idx = p_byte_offset >> 2u;
	uint shift = (p_byte_offset & 3u) * 8u;
	uint w0 = NEXTGEO_SKIN_PAGES_DATA[word_idx];
	if (shift <= 16u) {
		return (w0 >> shift) & 0xFFFFu;
	}
	uint w1 = NEXTGEO_SKIN_PAGES_DATA[word_idx + 1u];
	return ((w0 >> 24u) | ((w1 & 0xFFu) << 8u)) & 0xFFFFu;
}

uint nextgeo_skin_read_u32(uint p_byte_offset) {
	uint word_idx = p_byte_offset >> 2u;
	uint shift = (p_byte_offset & 3u) * 8u;
	uint w0 = NEXTGEO_SKIN_PAGES_DATA[word_idx];
	if (shift == 0u) {
		return w0;
	}
	uint w1 = NEXTGEO_SKIN_PAGES_DATA[word_idx + 1u];
	return (w0 >> shift) | (w1 << (32u - shift));
}

float nextgeo_skin_read_f32(uint p_byte_offset) {
	return uintBitsToFloat(nextgeo_skin_read_u32(p_byte_offset));
}

float nextgeo_skin_bone_scale(vec4 p_r0, vec4 p_r1, vec4 p_r2) {
	vec3 c0 = vec3(p_r0.x, p_r1.x, p_r2.x);
	vec3 c1 = vec3(p_r0.y, p_r1.y, p_r2.y);
	vec3 c2 = vec3(p_r0.z, p_r1.z, p_r2.z);
	float d01 = abs(dot(c0, c1));
	float d02 = abs(dot(c0, c2));
	float d12 = abs(dot(c1, c2));
	float s = sqrt(max(dot(c0, c0) + d01 + d02, max(dot(c1, c1) + d01 + d12, dot(c2, c2) + d02 + d12)));
	return s > 0.0 ? s : 1.0;
}

float nextgeo_skin_instance_max_scale(uint p_record_idx) {
	uvec4 sinfo = NEXTGEO_SKIN_INSTANCES_DATA[p_record_idx];
	if (sinfo.z == 0u) {
		return 1.0;
	}
	float s = uintBitsToFloat(sinfo.w);
	return s > 0.0 ? s : 1.0;
}

// Compute the union of each influencing bone's posed AABB in instance-local space,
// plus the maximum bone scale across the cluster's influencing bones.
bool nextgeo_skin_cluster_bounds(uint p_record_idx, uint p_cluster_table_idx, out vec3 r_min, out vec3 r_max, out float r_max_scale) {
	r_min = vec3(3.4e38);
	r_max = vec3(-3.4e38);
	r_max_scale = 1.0;

	uvec4 sinfo = NEXTGEO_SKIN_INSTANCES_DATA[p_record_idx];
	if (sinfo.z == 0u) {
		return false;
	}
	uint skin_off = NEXTGEO_CLUSTER_SKIN_DATA[p_cluster_table_idx];
	if (skin_off == 0xFFFFFFFFu) {
		return false;
	}
	uint bone_count = nextgeo_skin_read_u8(skin_off + 0u);
	if (bone_count == 0u) {
		return false;
	}

	uint pal_base = sinfo.x;
	uint skel_bones = sinfo.z;
	float max_s = 0.0;
	bool any_valid = false;
	for (uint b = 0u; b < bone_count; b++) {
		uint entry = skin_off + 4u + b * 26u;
		uint global_bone = nextgeo_skin_read_u16(entry);
		if (global_bone >= skel_bones) {
			continue;
		}
		vec3 bmin = vec3(
				nextgeo_skin_read_f32(entry + 2u),
				nextgeo_skin_read_f32(entry + 6u),
				nextgeo_skin_read_f32(entry + 10u));
		vec3 bmax = vec3(
				nextgeo_skin_read_f32(entry + 14u),
				nextgeo_skin_read_f32(entry + 18u),
				nextgeo_skin_read_f32(entry + 22u));

		uint p_off = pal_base + global_bone * 3u;
		vec4 r0 = NEXTGEO_SKIN_PALETTE_DATA[p_off + 0u];
		vec4 r1 = NEXTGEO_SKIN_PALETTE_DATA[p_off + 1u];
		vec4 r2 = NEXTGEO_SKIN_PALETTE_DATA[p_off + 2u];

		vec3 center = 0.5 * (bmin + bmax);
		vec3 extent = 0.5 * (bmax - bmin);
		vec3 tc = vec3(
				dot(r0.xyz, center) + r0.w,
				dot(r1.xyz, center) + r1.w,
				dot(r2.xyz, center) + r2.w);
		vec3 te = vec3(
				dot(abs(r0.xyz), extent),
				dot(abs(r1.xyz), extent),
				dot(abs(r2.xyz), extent));
		r_min = min(r_min, tc - te);
		r_max = max(r_max, tc + te);
		max_s = max(max_s, nextgeo_skin_bone_scale(r0, r1, r2));
		any_valid = true;
	}
	if (!any_valid) {
		return false;
	}
	r_max_scale = max_s > 0.0 ? max_s : 1.0;
	return true;
}

bool nextgeo_skin_blend_matrix(uint p_record_idx, uint p_cluster_table_idx, uint p_vert_idx, uint p_vert_count, bool p_use_prev, out vec4 r_r0, out vec4 r_r1, out vec4 r_r2) {
	r_r0 = vec4(1.0, 0.0, 0.0, 0.0);
	r_r1 = vec4(0.0, 1.0, 0.0, 0.0);
	r_r2 = vec4(0.0, 0.0, 1.0, 0.0);

	uvec4 sinfo = NEXTGEO_SKIN_INSTANCES_DATA[p_record_idx];
	if (sinfo.z == 0u || p_vert_idx >= p_vert_count) {
		return false;
	}
	uint skin_off = NEXTGEO_CLUSTER_SKIN_DATA[p_cluster_table_idx];
	if (skin_off == 0xFFFFFFFFu) {
		return false;
	}
	uint bone_count = nextgeo_skin_read_u8(skin_off + 0u);
	uint influences = nextgeo_skin_read_u8(skin_off + 1u);
	if (bone_count == 0u || (influences != 4u && influences != 8u)) {
		return false;
	}

	uint bone_table_base = skin_off + 4u;
	uint idx_base = bone_table_base + bone_count * 26u + p_vert_idx * influences;
	uint w_base = bone_table_base + bone_count * 26u + p_vert_count * influences + p_vert_idx * influences * 2u;
	uint pal_base = p_use_prev ? sinfo.y : sinfo.x;
	uint skel_bones = sinfo.z;

	vec4 acc0 = vec4(0.0);
	vec4 acc1 = vec4(0.0);
	vec4 acc2 = vec4(0.0);
	float w_sum = 0.0;
	for (uint k = 0u; k < influences; k++) {
		uint w_u16 = nextgeo_skin_read_u16(w_base + k * 2u);
		if (w_u16 == 0u) {
			continue;
		}
		uint local_bone = nextgeo_skin_read_u8(idx_base + k);
		if (local_bone >= bone_count) {
			continue;
		}
		uint global_bone = nextgeo_skin_read_u16(bone_table_base + local_bone * 26u);
		if (global_bone >= skel_bones) {
			continue;
		}
		float w = float(w_u16) * (1.0 / 65535.0);
		uint p_off = pal_base + global_bone * 3u;
		acc0 += NEXTGEO_SKIN_PALETTE_DATA[p_off + 0u] * w;
		acc1 += NEXTGEO_SKIN_PALETTE_DATA[p_off + 1u] * w;
		acc2 += NEXTGEO_SKIN_PALETTE_DATA[p_off + 2u] * w;
		w_sum += w;
	}
	if (!(w_sum > 0.0)) {
		return false;
	}
	float inv_w = 1.0 / w_sum;
	r_r0 = acc0 * inv_w;
	r_r1 = acc1 * inv_w;
	r_r2 = acc2 * inv_w;
	return true;
}

vec3 nextgeo_skin_position(vec3 p_pos, uint p_record_idx, uint p_cluster_table_idx, uint p_vert_idx, uint p_vert_count, bool p_use_prev) {
	vec4 r0;
	vec4 r1;
	vec4 r2;
	if (!nextgeo_skin_blend_matrix(p_record_idx, p_cluster_table_idx, p_vert_idx, p_vert_count, p_use_prev, r0, r1, r2)) {
		return p_pos;
	}
	return vec3(
			dot(r0.xyz, p_pos) + r0.w,
			dot(r1.xyz, p_pos) + r1.w,
			dot(r2.xyz, p_pos) + r2.w);
}

void nextgeo_skin_vertex(inout vec3 r_pos, inout vec3 r_normal, inout vec3 r_tangent, uint p_record_idx, uint p_cluster_table_idx, uint p_vert_idx, uint p_vert_count, bool p_use_prev) {
	vec4 r0;
	vec4 r1;
	vec4 r2;
	if (!nextgeo_skin_blend_matrix(p_record_idx, p_cluster_table_idx, p_vert_idx, p_vert_count, p_use_prev, r0, r1, r2)) {
		return;
	}
	r_pos = vec3(
			dot(r0.xyz, r_pos) + r0.w,
			dot(r1.xyz, r_pos) + r1.w,
			dot(r2.xyz, r_pos) + r2.w);

	// Inverse-transpose normal transform via cofactor matrix (handles non-uniform bone scale and mirrors).
	vec3 c0 = cross(r1.xyz, r2.xyz);
	vec3 c1 = cross(r2.xyz, r0.xyz);
	vec3 c2 = cross(r0.xyz, r1.xyz);
	float det = dot(r0.xyz, c0);
	vec3 n_sk;
	if (abs(det) > 1.0e-10) {
		float sgn = det < 0.0 ? -1.0 : 1.0;
		n_sk = vec3(dot(c0, r_normal), dot(c1, r_normal), dot(c2, r_normal)) * sgn;
	} else {
		n_sk = vec3(dot(r0.xyz, r_normal), dot(r1.xyz, r_normal), dot(r2.xyz, r_normal));
	}
	float n_len = length(n_sk);
	if (n_len > 1.0e-12) {
		r_normal = n_sk / n_len;
	}

	vec3 t_sk = vec3(dot(r0.xyz, r_tangent), dot(r1.xyz, r_tangent), dot(r2.xyz, r_tangent));
	float t_len = length(t_sk);
	if (t_len > 1.0e-12) {
		r_tangent = t_sk / t_len;
	}
}

// GodotNext: nextgeo_foliage_wind (#152, plan §21.4 contract N10).
// In-shader procedural bone evaluation per instance (0 bytes per-instance skin palette memory).
// Matches RendererRD::nextgeo_wind_eval_bone_matrix() in nextgeo_skinning.h bit-for-bit.
#ifdef NEXTGEO_HAS_WIND_BUFFERS
#ifndef NEXTGEO_WIND_HELPERS_DEFINED
#define NEXTGEO_WIND_HELPERS_DEFINED
bool nextgeo_wind_enabled() {
	return (floatBitsToUint(nextgeo_wind_params.time_gust_flags.w) & NEXTGEO_WIND_PARAM_FLAG_ENABLED) != 0u;
}

float nextgeo_wind_mesh_max_sway(uint p_mesh_idx) {
	if (!nextgeo_wind_enabled() || p_mesh_idx >= nextgeo_wind_params.meta.x) {
		return 0.0;
	}
	uvec4 hdr = nextgeo_wind_meshes.data[p_mesh_idx].header;
	return hdr.x > 0u ? uintBitsToFloat(hdr.y) : 0.0;
}

bool nextgeo_wind_mesh_active(uint p_mesh_idx) {
	if (!nextgeo_wind_enabled() || !(nextgeo_wind_params.direction_strength.w > 0.0) || p_mesh_idx >= nextgeo_wind_params.meta.x) {
		return false;
	}
	return nextgeo_wind_meshes.data[p_mesh_idx].header.x > 0u;
}
#endif

void nextgeo_wind_bone_matrix(uint p_mesh_idx, uint p_bone_idx, vec4 p_inst_transform[3], bool p_use_prev, out vec4 r_r0, out vec4 r_r1, out vec4 r_r2) {
	r_r0 = vec4(1.0, 0.0, 0.0, 0.0);
	r_r1 = vec4(0.0, 1.0, 0.0, 0.0);
	r_r2 = vec4(0.0, 0.0, 1.0, 0.0);

	if (!nextgeo_wind_mesh_active(p_mesh_idx)) {
		return;
	}
	uint bcount = min(nextgeo_wind_meshes.data[p_mesh_idx].header.x, NEXTGEO_WIND_MAX_BONES);
	if (p_bone_idx >= bcount) {
		return;
	}
	NextGeoWindBone bone = nextgeo_wind_meshes.data[p_mesh_idx].bones[p_bone_idx];
	float max_sway = bone.sway_phase_branch.x;
	if (!(max_sway > 0.0)) {
		return;
	}

	vec3 wdir = nextgeo_wind_params.direction_strength.xyz;
	float wd_len = length(wdir);
	wdir = wd_len > 1.0e-5 ? (wdir / wd_len) : vec3(1.0, 0.0, 0.0);

	// Transform world wind direction into instance-local space via transpose of scaled basis columns.
	vec3 c0 = vec3(p_inst_transform[0].x, p_inst_transform[1].x, p_inst_transform[2].x);
	vec3 c1 = vec3(p_inst_transform[0].y, p_inst_transform[1].y, p_inst_transform[2].y);
	vec3 c2 = vec3(p_inst_transform[0].z, p_inst_transform[1].z, p_inst_transform[2].z);
	vec3 ldir = vec3(
			dot(c0, wdir) / max(dot(c0, c0), 1.0e-8),
			dot(c1, wdir) / max(dot(c1, c1), 1.0e-8),
			dot(c2, wdir) / max(dot(c2, c2), 1.0e-8));
	float ld_len = length(ldir);
	ldir = ld_len > 1.0e-5 ? (ldir / ld_len) : vec3(1.0, 0.0, 0.0);

	// Deterministic per-instance phase seed from world origin (no per-instance buffer memory).
	vec3 inst_origin = vec3(p_inst_transform[0].w, p_inst_transform[1].w, p_inst_transform[2].w);
	float origin_along_wind = dot(inst_origin, wdir);
	float spatial_phase = origin_along_wind * 0.35 + dot(inst_origin, vec3(0.17, 0.07, 0.23));

	float t = p_use_prev ? nextgeo_wind_params.time_gust_flags.y : nextgeo_wind_params.time_gust_flags.x;
	float freq = max(nextgeo_wind_params.time_gust_flags.z, 0.05);
	float stiff = clamp(bone.axis_stiffness.w, 0.0, 1.0);
	float branch_w = bone.sway_phase_branch.z;
	float sway_phase = bone.sway_phase_branch.y;
	float w_freq = freq * (1.0 + 1.35 * branch_w) * (1.3 - 0.5 * stiff);
	float phase = t * (6.28318530718 * w_freq) * 0.25 + spatial_phase + sway_phase;
	float gust_phase = t * (6.28318530718 * freq * 0.12) - origin_along_wind * 0.15;
	float gust = 0.65 + 0.35 * sin(gust_phase);
	float primary = 0.45 + 0.55 * sin(phase);
	float flutter = 0.15 * branch_w * sin(phase * 2.17 + sway_phase * 1.7);
	float raw_amp = (primary * gust + flutter) * nextgeo_wind_params.direction_strength.w * (1.15 - 0.5 * stiff);
	float signed_norm = raw_amp / (1.0 + abs(raw_amp));
	float disp = signed_norm * max_sway;

	float lever = max(bone.rest_pos_lever.w, 0.25);
	float angle = clamp(disp / lever, -1.04719755, 1.04719755);
	if (abs(angle) <= 1.0e-7) {
		return;
	}

	vec3 baxis = bone.axis_stiffness.xyz;
	float a_len = length(baxis);
	baxis = a_len > 1.0e-5 ? (baxis / a_len) : vec3(0.0, 1.0, 0.0);

	vec3 raxis = cross(baxis, ldir);
	float r_len = length(raxis);
	if (r_len < 1.0e-4) {
		vec3 fallback = abs(baxis.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
		raxis = cross(baxis, fallback);
		r_len = length(raxis);
	}
	if (r_len <= 1.0e-6) {
		return;
	}
	raxis /= r_len;

	float c = cos(angle);
	float s = sin(angle);
	float omc = 1.0 - c;
	vec3 row0 = vec3(c + raxis.x * raxis.x * omc, raxis.x * raxis.y * omc - raxis.z * s, raxis.x * raxis.z * omc + raxis.y * s);
	vec3 row1 = vec3(raxis.y * raxis.x * omc + raxis.z * s, c + raxis.y * raxis.y * omc, raxis.y * raxis.z * omc - raxis.x * s);
	vec3 row2 = vec3(raxis.z * raxis.x * omc - raxis.y * s, raxis.z * raxis.y * omc + raxis.x * s, c + raxis.z * raxis.z * omc);

	vec3 pivot = bone.rest_pos_lever.xyz;
	r_r0 = vec4(row0, pivot.x - dot(row0, pivot));
	r_r1 = vec4(row1, pivot.y - dot(row1, pivot));
	r_r2 = vec4(row2, pivot.z - dot(row2, pivot));
}

bool nextgeo_wind_blend_matrix(uint p_mesh_idx, vec4 p_inst_transform[3], uint p_cluster_table_idx, uint p_vert_idx, uint p_vert_count, bool p_use_prev, out vec4 r_r0, out vec4 r_r1, out vec4 r_r2) {
	r_r0 = vec4(1.0, 0.0, 0.0, 0.0);
	r_r1 = vec4(0.0, 1.0, 0.0, 0.0);
	r_r2 = vec4(0.0, 0.0, 1.0, 0.0);

	if (!nextgeo_wind_mesh_active(p_mesh_idx) || p_vert_idx >= p_vert_count) {
		return false;
	}
	uint skin_off = NEXTGEO_CLUSTER_SKIN_DATA[p_cluster_table_idx];
	if (skin_off == 0xFFFFFFFFu) {
		return false;
	}
	uint bone_count = nextgeo_skin_read_u8(skin_off + 0u);
	uint influences = nextgeo_skin_read_u8(skin_off + 1u);
	if (bone_count == 0u || (influences != 4u && influences != 8u)) {
		return false;
	}

	uint wind_bones = min(nextgeo_wind_meshes.data[p_mesh_idx].header.x, NEXTGEO_WIND_MAX_BONES);
	uint bone_table_base = skin_off + 4u;
	uint idx_base = bone_table_base + bone_count * 26u + p_vert_idx * influences;
	uint w_base = bone_table_base + bone_count * 26u + p_vert_count * influences + p_vert_idx * influences * 2u;

	vec4 acc0 = vec4(0.0);
	vec4 acc1 = vec4(0.0);
	vec4 acc2 = vec4(0.0);
	float w_sum = 0.0;
	for (uint k = 0u; k < influences; k++) {
		uint w_u16 = nextgeo_skin_read_u16(w_base + k * 2u);
		if (w_u16 == 0u) {
			continue;
		}
		uint local_bone = nextgeo_skin_read_u8(idx_base + k);
		if (local_bone >= bone_count) {
			continue;
		}
		uint global_bone = nextgeo_skin_read_u16(bone_table_base + local_bone * 26u);
		if (global_bone >= wind_bones) {
			continue;
		}
		float w = float(w_u16) * (1.0 / 65535.0);
		vec4 br0;
		vec4 br1;
		vec4 br2;
		nextgeo_wind_bone_matrix(p_mesh_idx, global_bone, p_inst_transform, p_use_prev, br0, br1, br2);
		acc0 += br0 * w;
		acc1 += br1 * w;
		acc2 += br2 * w;
		w_sum += w;
	}
	if (!(w_sum > 0.0)) {
		return false;
	}
	float inv_w = 1.0 / w_sum;
	r_r0 = acc0 * inv_w;
	r_r1 = acc1 * inv_w;
	r_r2 = acc2 * inv_w;
	return true;
}

vec3 nextgeo_wind_position(vec3 p_pos, uint p_mesh_idx, vec4 p_inst_transform[3], uint p_cluster_table_idx, uint p_vert_idx, uint p_vert_count, bool p_use_prev) {
	vec4 r0;
	vec4 r1;
	vec4 r2;
	if (!nextgeo_wind_blend_matrix(p_mesh_idx, p_inst_transform, p_cluster_table_idx, p_vert_idx, p_vert_count, p_use_prev, r0, r1, r2)) {
		return p_pos;
	}
	return vec3(
			dot(r0.xyz, p_pos) + r0.w,
			dot(r1.xyz, p_pos) + r1.w,
			dot(r2.xyz, p_pos) + r2.w);
}

void nextgeo_wind_vertex(inout vec3 r_pos, inout vec3 r_normal, inout vec3 r_tangent, uint p_mesh_idx, vec4 p_inst_transform[3], uint p_cluster_table_idx, uint p_vert_idx, uint p_vert_count, bool p_use_prev) {
	vec4 r0;
	vec4 r1;
	vec4 r2;
	if (!nextgeo_wind_blend_matrix(p_mesh_idx, p_inst_transform, p_cluster_table_idx, p_vert_idx, p_vert_count, p_use_prev, r0, r1, r2)) {
		return;
	}
	r_pos = vec3(
			dot(r0.xyz, r_pos) + r0.w,
			dot(r1.xyz, r_pos) + r1.w,
			dot(r2.xyz, r_pos) + r2.w);

	vec3 c0 = cross(r1.xyz, r2.xyz);
	vec3 c1 = cross(r2.xyz, r0.xyz);
	vec3 c2 = cross(r0.xyz, r1.xyz);
	float det = dot(r0.xyz, c0);
	vec3 n_sk;
	if (abs(det) > 1.0e-10) {
		float sgn = det < 0.0 ? -1.0 : 1.0;
		n_sk = vec3(dot(c0, r_normal), dot(c1, r_normal), dot(c2, r_normal)) * sgn;
	} else {
		n_sk = vec3(dot(r0.xyz, r_normal), dot(r1.xyz, r_normal), dot(r2.xyz, r_normal));
	}
	float n_len = length(n_sk);
	if (n_len > 1.0e-12) {
		r_normal = n_sk / n_len;
	}

	vec3 t_sk = vec3(dot(r0.xyz, r_tangent), dot(r1.xyz, r_tangent), dot(r2.xyz, r_tangent));
	float t_len = length(t_sk);
	if (t_len > 1.0e-12) {
		r_tangent = t_sk / t_len;
	}
}
#endif
