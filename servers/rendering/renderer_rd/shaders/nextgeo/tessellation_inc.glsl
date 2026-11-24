// GodotNext: nextgeo_tessellation (#163, plan §21.4 contract N11, GN-6, GN-12, N5).
// Shared GLSL layouts and crack-free triangulation matching nextgeo_tessellation.h bit-for-bit.

const uint NEXTGEO_TESS_MAX_EDGE_FACTOR = 64u;
const uint NEXTGEO_TESS_MAX_PATCH_TRIANGLES = 128u;
const float NEXTGEO_TESS_MIN_DICING_RATE = 0.5;
const float NEXTGEO_TESS_MAX_DICING_RATE = 16.0;

// 32-byte compact patch record (2 x uvec4, std430). Keep in sync with RendererRD::NextGeoDicedPatchRecord.
struct NextGeoDicedPatchRecord {
	uint cluster_slot;
	uint tri_and_sub; // bits 0..7 = src_tri, bits 8..15 = sub_patch_idx, bits 16..31 = micro_tri_count [1..128]
	uint packed_factors; // bits 0..7 = f0, bits 8..15 = f1, bits 16..23 = f2, bits 24..31 = flags/bin
	uint micro_tri_base; // sub_patch_idx * 128
	float disp_magnitude;
	float disp_center;
	uint pad0;
	uint pad1;
};

struct NextGeoTessVertexCoord {
	uint kind; // 0..2 = corner V0..V2, 3..5 = edge 0..2 (num/den), 6 = interior (u, v, w)
	uint num;
	uint den;
	vec3 bary;
};

uint nextgeo_tess_pack_tri_and_sub(uint p_src_tri, uint p_sub_patch, uint p_micro_count) {
	return (p_src_tri & 0xFFu) | ((p_sub_patch & 0xFFu) << 8u) | ((p_micro_count & 0xFFFFu) << 16u);
}

uint nextgeo_tess_pack_factors(uint p_f0, uint p_f1, uint p_f2, uint p_flags) {
	return (p_f0 & 0xFFu) | ((p_f1 & 0xFFu) << 8u) | ((p_f2 & 0xFFu) << 16u) | ((p_flags & 0xFFu) << 24u);
}

bool nextgeo_tess_vec3_less(vec3 p_a, vec3 p_b) {
	if (p_a.x != p_b.x) {
		return p_a.x < p_b.x;
	}
	if (p_a.y != p_b.y) {
		return p_a.y < p_b.y;
	}
	return p_a.z < p_b.z;
}

vec3 nextgeo_tess_xform_point(mat3x4 p_xform, vec3 p_local) {
	return vec3(
			dot(p_xform[0].xyz, p_local) + p_xform[0].w,
			dot(p_xform[1].xyz, p_local) + p_xform[1].w,
			dot(p_xform[2].xyz, p_local) + p_xform[2].w);
}

// Crack-free canonical edge factor computed strictly from sorted endpoints (a, b).
uint nextgeo_tess_edge_factor(vec3 p_a_local, vec3 p_b_local, mat3x4 p_xform, vec3 p_cam_pos, float p_px_scale, float p_z_near, bool p_ortho, float p_dicing_rate) {
	vec3 a = p_a_local;
	vec3 b = p_b_local;
	if (nextgeo_tess_vec3_less(b, a)) {
		vec3 tmp = a;
		a = b;
		b = tmp;
	}
	vec3 a_w = nextgeo_tess_xform_point(p_xform, a);
	vec3 b_w = nextgeo_tess_xform_point(p_xform, b);
	float world_len = length(b_w - a_w);
	float rate = clamp(p_dicing_rate, NEXTGEO_TESS_MIN_DICING_RATE, NEXTGEO_TESS_MAX_DICING_RATE);
	if (!(world_len > 0.0) || !(p_px_scale > 0.0)) {
		return 1u;
	}
	float proj_len = world_len * p_px_scale;
	if (!p_ortho) {
		vec3 mid_w = (a_w + b_w) * 0.5;
		float min_dist = max(length(mid_w - p_cam_pos) - 0.5 * world_len, max(p_z_near, 1.0e-4));
		proj_len /= min_dist;
	}
	float raw = ceil(proj_len / rate);
	if (isnan(raw) || isinf(raw)) {
		return NEXTGEO_TESS_MAX_EDGE_FACTOR;
	}
	if (!(raw > 1.0)) {
		return 1u;
	}
	return min(uint(raw), NEXTGEO_TESS_MAX_EDGE_FACTOR);
}

uint nextgeo_tess_vsm_edge_factor(vec3 p_a_local, vec3 p_b_local, mat3x4 p_xform, vec3 p_light_origin, float p_page_texel, float p_near_w, bool p_directional, float p_dicing_rate) {
	vec3 a = p_a_local;
	vec3 b = p_b_local;
	if (nextgeo_tess_vec3_less(b, a)) {
		vec3 tmp = a;
		a = b;
		b = tmp;
	}
	vec3 a_w = nextgeo_tess_xform_point(p_xform, a);
	vec3 b_w = nextgeo_tess_xform_point(p_xform, b);
	float world_len = length(b_w - a_w);
	float rate = clamp(p_dicing_rate, NEXTGEO_TESS_MIN_DICING_RATE, NEXTGEO_TESS_MAX_DICING_RATE);
	if (!(world_len > 0.0) || !(p_page_texel > 0.0)) {
		return 1u;
	}
	float proj_texels = world_len / p_page_texel;
	if (!p_directional) {
		vec3 mid_w = (a_w + b_w) * 0.5;
		float min_dist = max(length(mid_w - p_light_origin) - 0.5 * world_len, max(p_near_w, 1.0e-4));
		proj_texels /= min_dist;
	}
	float raw = ceil(proj_texels / rate);
	if (isnan(raw) || isinf(raw)) {
		return NEXTGEO_TESS_MAX_EDGE_FACTOR;
	}
	if (!(raw > 1.0)) {
		return 1u;
	}
	return min(uint(raw), NEXTGEO_TESS_MAX_EDGE_FACTOR);
}

uint nextgeo_tess_interior_factor(uint p_f0, uint p_f1, uint p_f2) {
	return max(p_f0, max(p_f1, p_f2));
}

uint nextgeo_tess_micro_tri_count(uint p_f0, uint p_f1, uint p_f2) {
	uint n = nextgeo_tess_interior_factor(p_f0, p_f1, p_f2);
	if (n <= 1u) {
		return 1u;
	}
	if (n <= 3u) {
		return p_f0 + p_f1 + p_f2;
	}
	uint m = n - 3u;
	return m * m + p_f0 + p_f1 + p_f2 + 3u * m;
}

uint nextgeo_tess_patch_count(uint p_f0, uint p_f1, uint p_f2) {
	uint total = nextgeo_tess_micro_tri_count(p_f0, p_f1, p_f2);
	return (total + NEXTGEO_TESS_MAX_PATCH_TRIANGLES - 1u) / NEXTGEO_TESS_MAX_PATCH_TRIANGLES;
}

bool nextgeo_tess_coarsen_for_budget(inout uint r_f0, inout uint r_f1, inout uint r_f2, uint p_avail_patches, out uint r_steps) {
	r_steps = 0u;
	if (p_avail_patches == 0u) {
		return false;
	}
	while (nextgeo_tess_patch_count(r_f0, r_f1, r_f2) > p_avail_patches && (r_f0 > 1u || r_f1 > 1u || r_f2 > 1u)) {
		r_f0 = max((r_f0 + 1u) >> 1u, 1u);
		r_f1 = max((r_f1 + 1u) >> 1u, 1u);
		r_f2 = max((r_f2 + 1u) >> 1u, 1u);
		r_steps++;
	}
	return nextgeo_tess_patch_count(r_f0, r_f1, r_f2) <= p_avail_patches;
}

NextGeoTessVertexCoord nextgeo_tess_make_corner(uint p_corner) {
	NextGeoTessVertexCoord c;
	c.kind = p_corner;
	c.num = 0u;
	c.den = 1u;
	c.bary = vec3(p_corner == 0u ? 1.0 : 0.0, p_corner == 1u ? 1.0 : 0.0, p_corner == 2u ? 1.0 : 0.0);
	return c;
}

NextGeoTessVertexCoord nextgeo_tess_make_edge(uint p_edge, uint p_step, uint p_factor) {
	if (p_step == 0u) {
		return nextgeo_tess_make_corner(p_edge);
	}
	if (p_step >= p_factor) {
		return nextgeo_tess_make_corner((p_edge + 1u) % 3u);
	}
	NextGeoTessVertexCoord c;
	c.kind = 3u + p_edge;
	c.num = p_step;
	c.den = p_factor;
	float t = float(p_step) / float(p_factor);
	float omt = 1.0 - t;
	if (p_edge == 0u) {
		c.bary = vec3(omt, t, 0.0);
	} else if (p_edge == 1u) {
		c.bary = vec3(0.0, omt, t);
	} else {
		c.bary = vec3(t, 0.0, omt);
	}
	return c;
}

NextGeoTessVertexCoord nextgeo_tess_make_inner(uint p_i, uint p_j, uint p_k, uint p_n) {
	NextGeoTessVertexCoord c;
	c.kind = 6u;
	c.num = 0u;
	c.den = 1u;
	float inv_n = 1.0 / float(p_n);
	c.bary = vec3(float(p_i), float(p_j), float(p_k)) * inv_n;
	return c;
}

NextGeoTessVertexCoord nextgeo_tess_make_inner_edge(uint p_edge, uint p_b, uint p_m, uint p_n) {
	if (p_edge == 0u) {
		return nextgeo_tess_make_inner(p_m - p_b + 1u, p_b + 1u, 1u, p_n);
	}
	if (p_edge == 1u) {
		return nextgeo_tess_make_inner(1u, p_m - p_b + 1u, p_b + 1u, p_n);
	}
	return nextgeo_tess_make_inner(p_b + 1u, 1u, p_m - p_b + 1u, p_n);
}

NextGeoTessVertexCoord nextgeo_tess_make_center() {
	NextGeoTessVertexCoord c;
	c.kind = 6u;
	c.num = 0u;
	c.den = 1u;
	c.bary = vec3(1.0 / 3.0);
	return c;
}

uint nextgeo_tess_isqrt(uint p_x) {
	uint s = uint(floor(sqrt(float(p_x))));
	while ((s + 1u) * (s + 1u) <= p_x) {
		s++;
	}
	while (s * s > p_x) {
		s--;
	}
	return s;
}

void nextgeo_tess_micro_tri_coords(uint p_f0, uint p_f1, uint p_f2, uint p_idx, out NextGeoTessVertexCoord r_c0, out NextGeoTessVertexCoord r_c1, out NextGeoTessVertexCoord r_c2) {
	uint n = nextgeo_tess_interior_factor(p_f0, p_f1, p_f2);
	if (n <= 1u) {
		r_c0 = nextgeo_tess_make_corner(0u);
		r_c1 = nextgeo_tess_make_corner(1u);
		r_c2 = nextgeo_tess_make_corner(2u);
		return;
	}
	if (n <= 3u) {
		NextGeoTessVertexCoord ctr = nextgeo_tess_make_center();
		if (p_idx < p_f0) {
			r_c0 = nextgeo_tess_make_edge(0u, p_idx, p_f0);
			r_c1 = nextgeo_tess_make_edge(0u, p_idx + 1u, p_f0);
			r_c2 = ctr;
		} else if (p_idx < p_f0 + p_f1) {
			uint s = p_idx - p_f0;
			r_c0 = nextgeo_tess_make_edge(1u, s, p_f1);
			r_c1 = nextgeo_tess_make_edge(1u, s + 1u, p_f1);
			r_c2 = ctr;
		} else {
			uint s = p_idx - p_f0 - p_f1;
			r_c0 = nextgeo_tess_make_edge(2u, s, p_f2);
			r_c1 = nextgeo_tess_make_edge(2u, s + 1u, p_f2);
			r_c2 = ctr;
		}
		return;
	}

	uint m = n - 3u;
	uint inner_tris = m * m;
	if (p_idx < inner_tris) {
		uint s = nextgeo_tess_isqrt(p_idx);
		uint col = p_idx - s * s;
		uint p = col >> 1u;
		if ((col & 1u) == 0u) {
			r_c0 = nextgeo_tess_make_inner(s + 2u - p, p + 1u, m - s, n);
			r_c1 = nextgeo_tess_make_inner(s + 1u - p, p + 2u, m - s, n);
			r_c2 = nextgeo_tess_make_inner(s + 1u - p, p + 1u, m - s + 1u, n);
		} else {
			r_c0 = nextgeo_tess_make_inner(s + 1u - p, p + 2u, m - s, n);
			r_c1 = nextgeo_tess_make_inner(s - p, p + 2u, m - s + 1u, n);
			r_c2 = nextgeo_tess_make_inner(s + 1u - p, p + 1u, m - s + 1u, n);
		}
		return;
	}

	uint rem = p_idx - inner_tris;
	uint s0 = p_f0 + m;
	uint s1 = p_f1 + m;
	uint edge = 0u;
	uint outer_segs = p_f0;
	if (rem < s0) {
		edge = 0u;
		outer_segs = p_f0;
	} else if (rem < s0 + s1) {
		edge = 1u;
		outer_segs = p_f1;
		rem -= s0;
	} else {
		edge = 2u;
		outer_segs = p_f2;
		rem -= (s0 + s1);
	}
	uint total_steps = outer_segs + m;
	uint a0 = (rem * outer_segs + (total_steps >> 1u)) / total_steps;
	uint a1 = ((rem + 1u) * outer_segs + (total_steps >> 1u)) / total_steps;
	uint b0 = rem - a0;
	if (a1 > a0) {
		r_c0 = nextgeo_tess_make_edge(edge, a0, outer_segs);
		r_c1 = nextgeo_tess_make_edge(edge, a0 + 1u, outer_segs);
		r_c2 = nextgeo_tess_make_inner_edge(edge, b0, m, n);
	} else {
		r_c0 = nextgeo_tess_make_edge(edge, a0, outer_segs);
		r_c1 = nextgeo_tess_make_inner_edge(edge, b0 + 1u, m, n);
		r_c2 = nextgeo_tess_make_inner_edge(edge, b0, m, n);
	}
}

// Crack-free canonical attribute interpolation & edge-length derivatives for displacement().
// Shared by tessellation_dice.glsl, tessellation_hw.glsl, and scene_forward_clustered.glsl resolve so
// every stage computes bit-identical displaced vertex positions along corners, shared edges, and interiors.
void nextgeo_tess_eval_vertex_attribs(
		in vec3 p_pos[3],
		in vec3 p_nrm[3],
		in vec3 p_tan[3],
		in vec3 p_bin[3],
		in vec2 p_uv[3],
		in vec2 p_uv2[3],
		in vec4 p_col[3],
		in NextGeoTessVertexCoord p_coord,
		in uint p_interior_factor,
		out vec3 r_pos,
		out vec3 r_nrm,
		out vec3 r_tan,
		out vec3 r_bin,
		out vec2 r_uv,
		out vec2 r_uv2,
		out vec4 r_col,
		out vec3 r_pos_ddx,
		out vec3 r_pos_ddy,
		out vec3 r_nrm_ddx,
		out vec3 r_nrm_ddy,
		out vec2 r_uv_ddx,
		out vec2 r_uv_ddy,
		out vec2 r_uv2_ddx,
		out vec2 r_uv2_ddy,
		out vec4 r_col_ddx,
		out vec4 r_col_ddy) {
	if (p_coord.kind < 3u) {
		uint idx = p_coord.kind;
		r_pos = p_pos[idx];
		r_nrm = p_nrm[idx];
		r_tan = p_tan[idx];
		r_bin = p_bin[idx];
		r_uv = p_uv[idx];
		r_uv2 = p_uv2[idx];
		r_col = p_col[idx];
		// Shared corners across arbitrary-valence fans must not depend on opposite vertices or
		// adjacent edges, ensuring bit-identical displacement() evaluation on all incident triangles.
		r_pos_ddx = vec3(0.0);
		r_pos_ddy = vec3(0.0);
		r_nrm_ddx = vec3(0.0);
		r_nrm_ddy = vec3(0.0);
		r_uv_ddx = vec2(0.0);
		r_uv_ddy = vec2(0.0);
		r_uv2_ddx = vec2(0.0);
		r_uv2_ddy = vec2(0.0);
		r_col_ddx = vec4(0.0);
		r_col_ddy = vec4(0.0);
		return;
	}
	if (p_coord.kind < 6u) {
		uint ia = p_coord.kind - 3u;
		uint ib = (ia + 1u) % 3u;
		uint num = p_coord.num;
		if (nextgeo_tess_vec3_less(p_pos[ib], p_pos[ia])) {
			uint tmp = ia;
			ia = ib;
			ib = tmp;
			num = p_coord.den - num;
		}
		float inv_den = 1.0 / float(max(p_coord.den, 1u));
		float t = float(num) * inv_den;
		r_pos = mix(p_pos[ia], p_pos[ib], t);
		r_nrm = mix(p_nrm[ia], p_nrm[ib], t);
		r_tan = mix(p_tan[ia], p_tan[ib], t);
		r_bin = mix(p_bin[ia], p_bin[ib], t);
		r_uv = mix(p_uv[ia], p_uv[ib], t);
		r_uv2 = mix(p_uv2[ia], p_uv2[ib], t);
		r_col = mix(p_col[ia], p_col[ib], t);
		// Edge-only derivatives computed strictly from canonicalized (ia, ib) and edge factor den.
		r_pos_ddx = (p_pos[ib] - p_pos[ia]) * inv_den;
		r_pos_ddy = vec3(0.0);
		r_nrm_ddx = (p_nrm[ib] - p_nrm[ia]) * inv_den;
		r_nrm_ddy = vec3(0.0);
		r_uv_ddx = (p_uv[ib] - p_uv[ia]) * inv_den;
		r_uv_ddy = vec2(0.0);
		r_uv2_ddx = (p_uv2[ib] - p_uv2[ia]) * inv_den;
		r_uv2_ddy = vec2(0.0);
		r_col_ddx = (p_col[ib] - p_col[ia]) * inv_den;
		r_col_ddy = vec4(0.0);
		return;
	}
	vec3 b = p_coord.bary;
	r_pos = p_pos[0] * b.x + p_pos[1] * b.y + p_pos[2] * b.z;
	r_nrm = p_nrm[0] * b.x + p_nrm[1] * b.y + p_nrm[2] * b.z;
	r_tan = p_tan[0] * b.x + p_tan[1] * b.y + p_tan[2] * b.z;
	r_bin = p_bin[0] * b.x + p_bin[1] * b.y + p_bin[2] * b.z;
	r_uv = p_uv[0] * b.x + p_uv[1] * b.y + p_uv[2] * b.z;
	r_uv2 = p_uv2[0] * b.x + p_uv2[1] * b.y + p_uv2[2] * b.z;
	r_col = p_col[0] * b.x + p_col[1] * b.y + p_col[2] * b.z;
	float inv_n = 1.0 / float(max(p_interior_factor, 1u));
	r_pos_ddx = (p_pos[1] - p_pos[0]) * inv_n;
	r_pos_ddy = (p_pos[2] - p_pos[0]) * inv_n;
	r_nrm_ddx = (p_nrm[1] - p_nrm[0]) * inv_n;
	r_nrm_ddy = (p_nrm[2] - p_nrm[0]) * inv_n;
	r_uv_ddx = (p_uv[1] - p_uv[0]) * inv_n;
	r_uv_ddy = (p_uv[2] - p_uv[0]) * inv_n;
	r_uv2_ddx = (p_uv2[1] - p_uv2[0]) * inv_n;
	r_uv2_ddy = (p_uv2[2] - p_uv2[0]) * inv_n;
	r_col_ddx = (p_col[1] - p_col[0]) * inv_n;
	r_col_ddy = (p_col[2] - p_col[0]) * inv_n;
}
