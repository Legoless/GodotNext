/**************************************************************************/
/*  nextgeo_voxelizer.cpp                                                 */
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

#include "nextgeo_voxelizer.h"

#include "core/math/math_funcs.h"
#include "core/templates/hash_map.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_voxels.h"

#include <cmath>

namespace {

struct ClipVertex {
	double p[3] = { 0.0, 0.0, 0.0 };
	double w0 = 0.0;
	double w1 = 0.0;
};

struct ClipPolygon {
	static constexpr uint32_t MAX_VERTS = 16;
	ClipVertex v[MAX_VERTS];
	uint32_t count = 0;
};

// Exact Sutherland-Hodgman clip against half-space:
//   p_keep_greater ? (p[p_axis] >= p_plane) : (p[p_axis] <= p_plane).
static void _clip_polygon_plane(const ClipPolygon &p_in, int p_axis, double p_plane, bool p_keep_greater, ClipPolygon &r_out) {
	r_out.count = 0;
	if (p_in.count < 3) {
		return;
	}
	auto inside = [&](const ClipVertex &p_v) -> bool {
		return p_keep_greater ? (p_v.p[p_axis] >= p_plane) : (p_v.p[p_axis] <= p_plane);
	};
	auto emit = [&](const ClipVertex &p_v) {
		if (r_out.count < ClipPolygon::MAX_VERTS) {
			r_out.v[r_out.count++] = p_v;
		}
	};

	ClipVertex prev = p_in.v[p_in.count - 1];
	bool prev_in = inside(prev);
	for (uint32_t i = 0; i < p_in.count; i++) {
		const ClipVertex &curr = p_in.v[i];
		const bool curr_in = inside(curr);
		if (curr_in != prev_in) {
			const double d0 = prev.p[p_axis] - p_plane;
			const double d1 = curr.p[p_axis] - p_plane;
			const double denom = d0 - d1;
			const double t = std::abs(denom) > 1.0e-18 ? CLAMP(d0 / denom, 0.0, 1.0) : 0.5;
			ClipVertex mid;
			for (int k = 0; k < 3; k++) {
				mid.p[k] = prev.p[k] + (curr.p[k] - prev.p[k]) * t;
			}
			mid.p[p_axis] = p_plane;
			mid.w0 = prev.w0 + (curr.w0 - prev.w0) * t;
			mid.w1 = prev.w1 + (curr.w1 - prev.w1) * t;
			emit(mid);
		}
		if (curr_in) {
			emit(curr);
		}
		prev = curr;
		prev_in = curr_in;
	}
}

static bool _clip_triangle_to_aabb(const Vector3 p_tri[3], const double p_min[3], const double p_max[3], double p_eps, double &r_area, double &r_w0, double &r_w1) {
	ClipPolygon a;
	ClipPolygon b;
	a.count = 3;
	for (int i = 0; i < 3; i++) {
		a.v[i].p[0] = double(p_tri[i].x);
		a.v[i].p[1] = double(p_tri[i].y);
		a.v[i].p[2] = double(p_tri[i].z);
		a.v[i].w0 = (i == 0) ? 1.0 : 0.0;
		a.v[i].w1 = (i == 1) ? 1.0 : 0.0;
	}

	for (int axis = 0; axis < 3; axis++) {
		_clip_polygon_plane(a, axis, p_min[axis] - p_eps, true, b);
		if (b.count < 3) {
			return false;
		}
		_clip_polygon_plane(b, axis, p_max[axis] + p_eps, false, a);
		if (a.count < 3) {
			return false;
		}
	}

	double total_area = 0.0;
	double sum_w0 = 0.0;
	double sum_w1 = 0.0;
	const ClipVertex &v0 = a.v[0];
	for (uint32_t i = 1; i + 1 < a.count; i++) {
		const ClipVertex &v1 = a.v[i];
		const ClipVertex &v2 = a.v[i + 1];
		const double e1x = v1.p[0] - v0.p[0];
		const double e1y = v1.p[1] - v0.p[1];
		const double e1z = v1.p[2] - v0.p[2];
		const double e2x = v2.p[0] - v0.p[0];
		const double e2y = v2.p[1] - v0.p[1];
		const double e2z = v2.p[2] - v0.p[2];
		const double cx = e1y * e2z - e1z * e2y;
		const double cy = e1z * e2x - e1x * e2z;
		const double cz = e1x * e2y - e1y * e2x;
		const double tri_area = 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
		if (!(tri_area > 1.0e-14)) {
			continue;
		}
		total_area += tri_area;
		sum_w0 += tri_area * ((v0.w0 + v1.w0 + v2.w0) / 3.0);
		sum_w1 += tri_area * ((v0.w1 + v1.w1 + v2.w1) / 3.0);
	}
	if (!(total_area > 1.0e-14)) {
		return false;
	}
	r_area = total_area;
	r_w0 = sum_w0 / total_area;
	r_w1 = sum_w1 / total_area;
	return true;
}

struct VoxelCoordKey {
	int32_t x = 0;
	int32_t y = 0;
	int32_t z = 0;

	bool operator==(const VoxelCoordKey &p_other) const {
		return x == p_other.x && y == p_other.y && z == p_other.z;
	}
};

struct VoxelCoordKeyHasher {
	static _FORCE_INLINE_ uint32_t hash(const VoxelCoordKey &p_k) {
		uint32_t h = uint32_t(p_k.x) * 73856093u ^ uint32_t(p_k.y) * 19349663u ^ uint32_t(p_k.z) * 83492791u;
		h ^= h >> 16u;
		h *= 0x7feb352du;
		h ^= h >> 15u;
		return h;
	}
};

struct BrickCoordKeyCompare {
	_FORCE_INLINE_ bool operator()(const VoxelCoordKey &p_a, const VoxelCoordKey &p_b) const {
		if (p_a.x != p_b.x) {
			return p_a.x < p_b.x;
		}
		if (p_a.y != p_b.y) {
			return p_a.y < p_b.y;
		}
		return p_a.z < p_b.z;
	}
};

struct VoxelAccum {
	double area_sum = 0.0;
	double proj_area[3] = { 0.0, 0.0, 0.0 };
	double nx = 0.0;
	double ny = 0.0;
	double nz = 0.0;
	double u = 0.0;
	double v = 0.0;
	double uv_grad_sum = 0.0;
	// Track up to 4 surfaces per voxel cleanly; surfaces in NextGeo are < 32.
	uint16_t surf_ids[4] = { 0, 0, 0, 0 };
	double surf_areas[4] = { 0.0, 0.0, 0.0, 0.0 };
	uint32_t surf_count = 0;

	void add_surface_area(uint16_t p_surface_id, double p_area) {
		for (uint32_t i = 0; i < surf_count; i++) {
			if (surf_ids[i] == p_surface_id) {
				surf_areas[i] += p_area;
				return;
			}
		}
		if (surf_count < 4u) {
			surf_ids[surf_count] = p_surface_id;
			surf_areas[surf_count] = p_area;
			surf_count++;
		} else {
			uint32_t min_idx = 0;
			for (uint32_t i = 1; i < 4u; i++) {
				if (surf_areas[i] < surf_areas[min_idx]) {
					min_idx = i;
				}
			}
			if (p_area > surf_areas[min_idx]) {
				surf_ids[min_idx] = p_surface_id;
				surf_areas[min_idx] = p_area;
			}
		}
	}

	uint16_t dominant_surface() const {
		if (surf_count == 0u) {
			return 0u;
		}
		uint32_t best = 0u;
		for (uint32_t i = 1u; i < surf_count; i++) {
			if (surf_areas[i] > surf_areas[best] || (surf_areas[i] == surf_areas[best] && surf_ids[i] < surf_ids[best])) {
				best = i;
			}
		}
		return surf_ids[best];
	}
};

struct BrickAccum {
	uint32_t occ_lo = 0u;
	uint32_t occ_hi = 0u;
	VoxelAccum cells[64];
};

static double _triangle_uv_gradient(const Vector3 p_p[3], const Vector2 p_uv[3], const Vector3 &p_gn_unit) {
	const Vector3 e1 = p_p[1] - p_p[0];
	const Vector3 e2 = p_p[2] - p_p[0];
	const double e1_len = double(e1.length());
	if (!(e1_len > 1.0e-8)) {
		return 0.0;
	}
	const Vector3 t_axis = e1 / real_t(e1_len);
	Vector3 b_axis = p_gn_unit.cross(t_axis);
	const double b_len = double(b_axis.length());
	if (!(b_len > 1.0e-8)) {
		return 0.0;
	}
	b_axis /= real_t(b_len);
	const double e2_t = double(e2.dot(t_axis));
	const double e2_b = double(e2.dot(b_axis));
	if (!(std::abs(e2_b) > 1.0e-8)) {
		return 0.0;
	}
	const Vector2 du1 = p_uv[1] - p_uv[0];
	const Vector2 du2 = p_uv[2] - p_uv[0];
	const double dudt_x = double(du1.x) / e1_len;
	const double dudt_y = double(du1.y) / e1_len;
	const double dudb_x = (double(du2.x) - dudt_x * e2_t) / e2_b;
	const double dudb_y = (double(du2.y) - dudt_y * e2_t) / e2_b;
	const double mag2 = 0.5 * (dudt_x * dudt_x + dudt_y * dudt_y + dudb_x * dudb_x + dudb_y * dudb_y);
	if (!std::isfinite(mag2) || !(mag2 > 0.0)) {
		return 0.0;
	}
	return MIN(std::sqrt(mag2), 1024.0);
}

static inline int32_t _floor_div4(int32_t p_v) {
	return p_v >= 0 ? (p_v / 4) : ((p_v - 3) / 4);
}

} // namespace

uint32_t nextgeo_voxel_bricks_bytes(const NextGeoVoxelBrick *p_bricks, uint32_t p_brick_count) {
	uint32_t bytes = 0u;
	for (uint32_t i = 0; i < p_brick_count; i++) {
		const uint32_t occ = p_bricks[i].meta & 0xFFu;
		bytes += RendererRD::NEXTGEO_VOXEL_BRICK_HEADER_WORDS * 4u + occ * RendererRD::NEXTGEO_VOXEL_DATA_WORDS * 4u;
	}
	return bytes;
}

uint32_t nextgeo_voxel_page_section_max_bytes(uint32_t p_page_group_count, uint32_t p_bricks_bytes) {
	if (p_bricks_bytes == 0u) {
		return 0u;
	}
	// Up to 3 alignment pad bytes + 16-byte voxel header + 16*group_count directory + bricks + 8-byte trailer.
	return 3u + 16u + p_page_group_count * 16u + p_bricks_bytes + 8u;
}

void nextgeo_voxelize_triangles(const LocalVector<NextGeoVoxelTriInput> &p_tris, float p_voxel_size, LocalVector<NextGeoVoxelBrick> &r_bricks, LocalVector<NextGeoVoxelData> &r_voxels) {
	r_bricks.clear();
	r_voxels.clear();
	if (p_tris.is_empty() || !std::isfinite(p_voxel_size) || !(p_voxel_size > 1.0e-6f)) {
		return;
	}

	double aabb_min[3] = { 1.0e30, 1.0e30, 1.0e30 };
	double aabb_max[3] = { -1.0e30, -1.0e30, -1.0e30 };
	bool have_finite = false;
	for (uint32_t t = 0; t < p_tris.size(); t++) {
		const NextGeoVoxelTriInput &tri = p_tris[t];
		for (int c = 0; c < 3; c++) {
			const double x = double(tri.p[c].x);
			const double y = double(tri.p[c].y);
			const double z = double(tri.p[c].z);
			if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
				continue;
			}
			aabb_min[0] = MIN(aabb_min[0], x);
			aabb_min[1] = MIN(aabb_min[1], y);
			aabb_min[2] = MIN(aabb_min[2], z);
			aabb_max[0] = MAX(aabb_max[0], x);
			aabb_max[1] = MAX(aabb_max[1], y);
			aabb_max[2] = MAX(aabb_max[2], z);
			have_finite = true;
		}
	}
	if (!have_finite) {
		return;
	}

	const double s = double(p_voxel_size);
	const double inv_s = 1.0 / s;
	const double cell_area = s * s;
	const double clip_eps = 1.0e-5 * s;
	const double grid_origin[3] = { aabb_min[0], aabb_min[1], aabb_min[2] };

	HashMap<VoxelCoordKey, BrickAccum, VoxelCoordKeyHasher> brick_map;

	for (uint32_t t = 0; t < p_tris.size(); t++) {
		const NextGeoVoxelTriInput &tri = p_tris[t];
		const Vector3 gn = (tri.p[1] - tri.p[0]).cross(tri.p[2] - tri.p[0]);
		const double gn_len = double(gn.length());
		if (!(gn_len > 1.0e-12) || !std::isfinite(gn_len)) {
			continue;
		}
		const Vector3 gn_unit = gn / real_t(gn_len);
		const double abs_gn[3] = {
			std::abs(double(gn_unit.x)),
			std::abs(double(gn_unit.y)),
			std::abs(double(gn_unit.z)),
		};
		const double uv_grad = _triangle_uv_gradient(tri.p, tri.uv, gn_unit);

		int32_t lo[3];
		int32_t hi[3];
		for (int a = 0; a < 3; a++) {
			const double tmin = MIN(double(tri.p[0][a]), MIN(double(tri.p[1][a]), double(tri.p[2][a])));
			const double tmax = MAX(double(tri.p[0][a]), MAX(double(tri.p[1][a]), double(tri.p[2][a])));
			const double fmin = (tmin - grid_origin[a]) * inv_s;
			const double fmax = (tmax - grid_origin[a]) * inv_s;
			// Coplanar face lying strictly on an integer grid plane: assign uniquely to one side
			// (using outward normal sign) so shared faces never double-count across adjacent cells.
			if (std::abs(fmax - fmin) < 1.0e-5) {
				const double nearest = std::round(fmin);
				if (std::abs(fmin - nearest) < 1.0e-4) {
					const int32_t k = int32_t(nearest);
					const int32_t cell = (k > 0 && double(gn_unit[a]) > 0.0) ? (k - 1) : k;
					lo[a] = cell;
					hi[a] = cell;
					continue;
				}
			}
			lo[a] = int32_t(std::floor(fmin + 1.0e-6));
			hi[a] = int32_t(std::floor(fmax - 1.0e-6));
			if (hi[a] < lo[a]) {
				hi[a] = lo[a];
			}
		}

		// Guard pathological stretched inputs from unbounded cell loops.
		const int64_t span_x = int64_t(hi[0]) - int64_t(lo[0]) + 1;
		const int64_t span_y = int64_t(hi[1]) - int64_t(lo[1]) + 1;
		const int64_t span_z = int64_t(hi[2]) - int64_t(lo[2]) + 1;
		if (span_x <= 0 || span_y <= 0 || span_z <= 0 || span_x * span_y * span_z > 65536) {
			continue;
		}

		for (int32_t iz = lo[2]; iz <= hi[2]; iz++) {
			const double cmin_z = grid_origin[2] + double(iz) * s;
			const double cmax_z = cmin_z + s;
			for (int32_t iy = lo[1]; iy <= hi[1]; iy++) {
				const double cmin_y = grid_origin[1] + double(iy) * s;
				const double cmax_y = cmin_y + s;
				for (int32_t ix = lo[0]; ix <= hi[0]; ix++) {
					const double cmin[3] = { grid_origin[0] + double(ix) * s, cmin_y, cmin_z };
					const double cmax[3] = { cmin[0] + s, cmax_y, cmax_z };

					double area = 0.0;
					double w0 = 0.0;
					double w1 = 0.0;
					if (!_clip_triangle_to_aabb(tri.p, cmin, cmax, clip_eps, area, w0, w1)) {
						continue;
					}
					const double w2 = 1.0 - w0 - w1;
					Vector3 n_interp = tri.n[0] * real_t(w0) + tri.n[1] * real_t(w1) + tri.n[2] * real_t(w2);
					const double nlen = double(n_interp.length());
					if (nlen > 1.0e-8 && std::isfinite(nlen)) {
						n_interp /= real_t(nlen);
					} else {
						n_interp = gn_unit;
					}
					const Vector2 uv_interp = tri.uv[0] * real_t(w0) + tri.uv[1] * real_t(w1) + tri.uv[2] * real_t(w2);

					const int32_t bx = _floor_div4(ix);
					const int32_t by = _floor_div4(iy);
					const int32_t bz = _floor_div4(iz);
					const uint32_t lx = uint32_t(ix - bx * 4);
					const uint32_t ly = uint32_t(iy - by * 4);
					const uint32_t lz = uint32_t(iz - bz * 4);
					const uint32_t v_idx = lx + ly * 4u + lz * 16u;

					BrickAccum &brick = brick_map[VoxelCoordKey{ bx, by, bz }];
					if (v_idx < 32u) {
						brick.occ_lo |= (1u << v_idx);
					} else {
						brick.occ_hi |= (1u << (v_idx - 32u));
					}
					VoxelAccum &acc = brick.cells[v_idx];
					acc.area_sum += area;
					acc.proj_area[0] += area * abs_gn[0];
					acc.proj_area[1] += area * abs_gn[1];
					acc.proj_area[2] += area * abs_gn[2];
					acc.nx += double(n_interp.x) * area;
					acc.ny += double(n_interp.y) * area;
					acc.nz += double(n_interp.z) * area;
					acc.u += double(uv_interp.x) * area;
					acc.v += double(uv_interp.y) * area;
					acc.uv_grad_sum += uv_grad * area;
					acc.add_surface_area(tri.surface_id, area);
				}
			}
		}
	}

	if (brick_map.is_empty()) {
		return;
	}

	// Sort brick keys deterministically so repeated offline builds are byte-identical.
	LocalVector<VoxelCoordKey> sorted_keys;
	sorted_keys.reserve(brick_map.size());
	for (const KeyValue<VoxelCoordKey, BrickAccum> &E : brick_map) {
		sorted_keys.push_back(E.key);
	}
	sorted_keys.sort_custom<BrickCoordKeyCompare>();

	r_bricks.reserve(sorted_keys.size());
	for (uint32_t b = 0; b < sorted_keys.size(); b++) {
		const VoxelCoordKey &key = sorted_keys[b];
		const BrickAccum &src = brick_map[key];

		uint32_t occ_lo = 0u;
		uint32_t occ_hi = 0u;
		uint32_t occ_count = 0u;
		const uint32_t first_voxel_offset = r_voxels.size();

		bool uniform_surface = true;
		uint16_t first_surface = 0u;
		double total_uv_grad_weight = 0.0;
		double total_uv_grad_sum = 0.0;

		for (uint32_t v = 0; v < 64u; v++) {
			const bool set = v < 32u ? ((src.occ_lo & (1u << v)) != 0u) : ((src.occ_hi & (1u << (v - 32u))) != 0u);
			if (!set) {
				continue;
			}
			const VoxelAccum &acc = src.cells[v];
			if (!(acc.area_sum > 1.0e-12)) {
				continue;
			}
			// Axis-projected fractional coverage preserves exact surface fraction for both axis-aligned
			// and oblique planes (clamped to [1/255, 1]).
			const double max_proj = MAX(acc.proj_area[0], MAX(acc.proj_area[1], acc.proj_area[2]));
			const double cov_f = CLAMP(max_proj / cell_area, 0.0, 1.0);
			const uint8_t cov_u8 = uint8_t(CLAMP(int(std::lround(cov_f * 255.0)), 1, 255));

			Vector3 mean_n(float(acc.nx / acc.area_sum), float(acc.ny / acc.area_sum), float(acc.nz / acc.area_sum));
			const float nlen = float(mean_n.length());
			if (nlen > 1.0e-6f && std::isfinite(nlen)) {
				mean_n /= nlen;
			} else {
				mean_n = Vector3(0.0f, 1.0f, 0.0f);
			}
			const Vector2 mean_uv(float(acc.u / acc.area_sum), float(acc.v / acc.area_sum));
			const uint16_t surf_id = acc.dominant_surface();
			const uint8_t surf_u8 = uint8_t(MIN<uint16_t>(surf_id, 255u));

			if (occ_count == 0u) {
				first_surface = surf_id;
			} else if (surf_id != first_surface) {
				uniform_surface = false;
			}
			total_uv_grad_sum += acc.uv_grad_sum;
			total_uv_grad_weight += acc.area_sum;

			if (v < 32u) {
				occ_lo |= (1u << v);
			} else {
				occ_hi |= (1u << (v - 32u));
			}
			occ_count++;

			NextGeoVoxelData vd;
			vd.uv_half2 = RendererRD::nextgeo_voxel_pack_uv_half2(mean_uv);
			vd.word1 = RendererRD::nextgeo_voxel_pack_word1(RendererRD::nextgeo_voxel_encode_oct16(mean_n), cov_u8, surf_u8);
			r_voxels.push_back(vd);
		}

		if (occ_count == 0u) {
			continue;
		}
		NextGeoVoxelBrick brick;
		brick.origin[0] = float(grid_origin[0] + double(key.x) * 4.0 * s);
		brick.origin[1] = float(grid_origin[1] + double(key.y) * 4.0 * s);
		brick.origin[2] = float(grid_origin[2] + double(key.z) * 4.0 * s);
		brick.voxel_size = p_voxel_size;
		brick.occ_lo = occ_lo;
		brick.occ_hi = occ_hi;
		brick.meta = RendererRD::nextgeo_voxel_pack_brick_meta(occ_count, first_surface, uniform_surface);
		const double avg_uv_grad = total_uv_grad_weight > 0.0 ? (total_uv_grad_sum / total_uv_grad_weight) : 0.0;
		brick.uv_footprint = float(CLAMP(avg_uv_grad * s, 0.0, 64.0));
		brick.voxel_offset = first_voxel_offset;
		r_bricks.push_back(brick);
	}
}
