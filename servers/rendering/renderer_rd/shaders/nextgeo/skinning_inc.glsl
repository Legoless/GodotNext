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
