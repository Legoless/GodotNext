/**************************************************************************/
/*  test_light_grid.cpp                                                   */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_light_grid)

#include "core/math/random_pcg.h"
#include "scene/3d/light_3d.h"
#include "servers/rendering/renderer_rd/light_grid_math.h"
#include "servers/rendering/renderer_scene_cull.h"
#include "servers/rendering/rendering_server.h"

namespace {

namespace LG = RendererRD::LightGrid;

// Mirrors light_grid_build.glsl's two-pass coarse (4x4x4 froxels) + fine build on CPU
// so doctests verify coarse-to-fine monotonicity and exact parity with the brute-force reference.
struct TwoPassResult {
	LG::ReferenceGrid grid;
	uint32_t coarse_dropped_lights = 0;
};

TwoPassResult run_two_pass_cpu(
		const LG::PackedLightData *p_lights,
		uint32_t p_light_count,
		uint32_t p_omni_count,
		uint32_t p_spot_count,
		const float p_inv_proj[16],
		const Size2i &p_screen_size,
		float p_z_near,
		float p_z_far,
		bool p_orthogonal,
		const Vector2 *p_tile_depths) {
	TwoPassResult out;
	out.grid.grid_dims = LG::compute_grid_dims(p_screen_size);
	const Vector3i cdims = LG::compute_coarse_dims(out.grid.grid_dims);
	const uint32_t gx = uint32_t(out.grid.grid_dims.x);
	const uint32_t gy = uint32_t(out.grid.grid_dims.y);
	const uint32_t gz = uint32_t(out.grid.grid_dims.z);
	const uint32_t cx_n = uint32_t(cdims.x);
	const uint32_t cy_n = uint32_t(cdims.y);
	const uint32_t cz_n = uint32_t(cdims.z);
	const uint32_t screen_w = uint32_t(MAX(p_screen_size.x, 1));
	const uint32_t screen_h = uint32_t(MAX(p_screen_size.y, 1));
	const LG::ZParams zp = LG::compute_z_params(p_z_near, p_z_far, p_orthogonal);

	const uint32_t coarse_total = cx_n * cy_n * cz_n;
	LocalVector<uint32_t> coarse_counts;
	coarse_counts.resize(coarse_total);
	LocalVector<uint32_t> coarse_indices;
	coarse_indices.resize(coarse_total * LG::COARSE_LIST_CAP);

	// Pass 1: Coarse 4x4x4 froxel binning (identical predicates to MODE_COARSE).
	for (uint32_t cz = 0; cz < cz_n; cz++) {
		const uint32_t tz0 = cz * LG::COARSE_GROUP;
		const uint32_t tz1 = MIN(tz0 + LG::COARSE_GROUP, gz);
		float cz0 = 0.0f;
		float cz1 = 0.0f;
		LG::slice_depth_bounds(tz0, tz1, zp, cz0, cz1);

		for (uint32_t cy = 0; cy < cy_n; cy++) {
			const uint32_t ty0 = cy * LG::COARSE_GROUP;
			const uint32_t ty1 = MIN(ty0 + LG::COARSE_GROUP, gy);
			for (uint32_t cx = 0; cx < cx_n; cx++) {
				const uint32_t tx0 = cx * LG::COARSE_GROUP;
				const uint32_t tx1 = MIN(tx0 + LG::COARSE_GROUP, gx);
				const uint32_t cidx = (cz * cy_n + cy) * cx_n + cx;

				float min_tile_z = 3.402823466e+38f;
				float max_tile_z = 0.0f;
				if (p_tile_depths != nullptr) {
					for (uint32_t ty = ty0; ty < ty1; ty++) {
						for (uint32_t tx = tx0; tx < tx1; tx++) {
							const Vector2 td = p_tile_depths[ty * gx + tx];
							if (float(td.y) > 0.0f) {
								min_tile_z = MIN(min_tile_z, float(td.x));
								max_tile_z = MAX(max_tile_z, float(td.y));
							}
						}
					}
				} else {
					min_tile_z = zp.z_near;
					max_tile_z = zp.z_far;
				}

				if (max_tile_z <= 0.0f || cz0 > max_tile_z * LG::HIZ_FAR_TOLERANCE || cz1 < min_tile_z * LG::HIZ_NEAR_TOLERANCE) {
					coarse_counts[cidx] = 0u;
					continue;
				}

				float z0 = MAX(cz0, min_tile_z * LG::HIZ_NEAR_TOLERANCE);
				float z1 = MIN(cz1, max_tile_z * LG::HIZ_FAR_TOLERANCE);
				if (z1 < z0) {
					z1 = z0;
				}
				const uint32_t px0 = tx0 * LG::TILE_SIZE;
				const uint32_t py0 = ty0 * LG::TILE_SIZE;
				const uint32_t px1 = MIN(tx1 * LG::TILE_SIZE, screen_w);
				const uint32_t py1 = MIN(ty1 * LG::TILE_SIZE, screen_h);
				const LG::CellBounds cb = LG::compute_cell_bounds(p_inv_proj, screen_w, screen_h, px0, py0, px1, py1, z0, z1);

				uint32_t count = 0u;
				const uint32_t cbase = cidx * LG::COARSE_LIST_CAP;
				for (uint32_t li = 0; li < p_light_count; li++) {
					// Monotone coarse test matching MODE_COARSE (padded radius, no spot bounding-sphere rejection,
					// and monotone light-local barn-door extent bound for area lights).
					LG::PackedLightData L = p_lights[li];
					L.radius = L.radius * 1.001f + 1e-4f;
					const uint32_t ltype = LG::unpack_entry_type(L.packed_entry);
					bool hit = false;
					if (ltype == LG::LIGHT_TYPE_SPOT && L.cos_spot_angle > 0.0f) {
						const Vector3 pos(L.position[0], L.position[1], L.position[2]);
						if (LG::sphere_intersects_cell(pos, L.radius, cb)) {
							const Vector3 center = (cb.bmin + cb.bmax) * 0.5f;
							const Vector3 extents = (cb.bmax - cb.bmin) * 0.5f;
							const Vector3 dir(L.direction[0], L.direction[1], L.direction[2]);
							const Vector3 rel = center - pos;
							const float proj_r = std::fabs(float(dir.x)) * float(extents.x) + std::fabs(float(dir.y)) * float(extents.y) + std::fabs(float(dir.z)) * float(extents.z);
							const float v1 = float(rel.x) * float(dir.x) + float(rel.y) * float(dir.y) + float(rel.z) * float(dir.z);
							if (v1 + proj_r >= 0.0f && v1 - proj_r <= L.radius) {
								const Vector3 ref_up = (std::fabs(float(dir.y)) < 0.999f) ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
								const Vector3 cu = dir.cross(ref_up).normalized();
								const Vector3 cv = cu.cross(dir);
								const float cos_a = L.cos_spot_angle;
								const float sin_a = L.axis_x[0];
								const Vector3 sp[4] = {
									cu * cos_a - dir * sin_a,
									-cu * cos_a - dir * sin_a,
									cv * cos_a - dir * sin_a,
									-cv * cos_a - dir * sin_a,
								};
								hit = true;
								for (int p = 0; p < 4; p++) {
									const float dc = float(rel.x) * float(sp[p].x) + float(rel.y) * float(sp[p].y) + float(rel.z) * float(sp[p].z);
									const float de = std::fabs(float(sp[p].x)) * float(extents.x) + std::fabs(float(sp[p].y)) * float(extents.y) + std::fabs(float(sp[p].z)) * float(extents.z);
									if (dc - de > 1e-4f) {
										hit = false;
										break;
									}
								}
							}
						}
					} else if (ltype == LG::LIGHT_TYPE_AREA) {
						const Vector3 pos(L.position[0], L.position[1], L.position[2]);
						const Vector3 dir(L.direction[0], L.direction[1], L.direction[2]);
						const Vector3 ax(L.axis_x[0], L.axis_x[1], L.axis_x[2]);
						const Vector3 ay(L.axis_y[0], L.axis_y[1], L.axis_y[2]);
						const float hw = L.half_width;
						const float hh = L.half_height;
						hit = true;
						for (int i = 0; i < 4; i++) {
							const Vector4 &pl = cb.planes[i];
							const float support = L.radius + std::fabs(float(pl.x) * float(ax.x) + float(pl.y) * float(ax.y) + float(pl.z) * float(ax.z)) * hw + std::fabs(float(pl.x) * float(ay.x) + float(pl.y) * float(ay.y) + float(pl.z) * float(ay.z)) * hh;
							const float dist = float(pl.x) * float(pos.x) + float(pl.y) * float(pos.y) + float(pl.z) * float(pos.z) + float(pl.w);
							if (dist > support) {
								hit = false;
								break;
							}
						}
						if (hit) {
							const Vector3 center = (cb.bmin + cb.bmax) * 0.5f;
							const Vector3 extents = (cb.bmax - cb.bmin) * 0.5f;
							const Vector3 rel = center - pos;
							const float f_c = float(rel.x) * float(dir.x) + float(rel.y) * float(dir.y) + float(rel.z) * float(dir.z);
							const float f_e = std::fabs(float(dir.x)) * float(extents.x) + std::fabs(float(dir.y)) * float(extents.y) + std::fabs(float(dir.z)) * float(extents.z);
							const float f_min = f_c - f_e;
							const float f_max = f_c + f_e;
							if (f_max < 0.0f || f_min > L.radius) {
								hit = false;
							} else {
								const float x_c = float(rel.x) * float(ax.x) + float(rel.y) * float(ax.y) + float(rel.z) * float(ax.z);
								const float x_e = std::fabs(float(ax.x)) * float(extents.x) + std::fabs(float(ax.y)) * float(extents.y) + std::fabs(float(ax.z)) * float(extents.z);
								const float x_min = x_c - x_e;
								const float x_max = x_c + x_e;
								const float y_c = float(rel.x) * float(ay.x) + float(rel.y) * float(ay.y) + float(rel.z) * float(ay.z);
								const float y_e = std::fabs(float(ay.x)) * float(extents.x) + std::fabs(float(ay.y)) * float(extents.y) + std::fabs(float(ay.z)) * float(extents.z);
								const float y_min = y_c - y_e;
								const float y_max = y_c + y_e;
								const float dx = MAX(0.0f, MAX(-hw - x_max, x_min - hw));
								const float dy = MAX(0.0f, MAX(-hh - y_max, y_min - hh));
								const float df = MAX(0.0f, f_min);
								if (dx * dx + dy * dy + df * df > L.radius * L.radius) {
									hit = false;
								} else if (LG::is_barn_door_active_rad(L.barn_door_angle_rad, L.barn_door_length)) {
									const float ext_x = LG::barn_door_max_extent_at_depth(hw, L.radius, L.barn_door_angle_rad, L.barn_door_length, MAX(f_max, 0.0f));
									const float ext_y = LG::barn_door_max_extent_at_depth(hh, L.radius, L.barn_door_angle_rad, L.barn_door_length, MAX(f_max, 0.0f));
									if (x_min > ext_x + 1e-4f || -x_max > ext_x + 1e-4f || y_min > ext_y + 1e-4f || -y_max > ext_y + 1e-4f) {
										hit = false;
									}
								}
							}
						}
					} else {
						hit = LG::light_intersects_cell(L, cb);
					}
					if (hit) {
						if (count < LG::COARSE_LIST_CAP) {
							coarse_indices[cbase + count] = li;
						}
						count++;
					}
				}
				coarse_counts[cidx] = count;
			}
		}
	}

	// Pass 2: Fine froxel refinement from parent coarse lists.
	const uint32_t total_cells = gx * gy * gz;
	out.grid.headers.resize(total_cells);
	out.grid.indices.resize(total_cells * LG::MAX_LIGHTS_PER_CELL);

	for (uint32_t tz = 0; tz < gz; tz++) {
		float slice_z0 = 0.0f;
		float slice_z1 = 0.0f;
		LG::slice_depth_bounds(tz, tz + 1u, zp, slice_z0, slice_z1);

		for (uint32_t ty = 0; ty < gy; ty++) {
			const uint32_t py0 = ty * LG::TILE_SIZE;
			const uint32_t py1 = MIN(py0 + LG::TILE_SIZE, screen_h);

			for (uint32_t tx = 0; tx < gx; tx++) {
				const uint32_t px0 = tx * LG::TILE_SIZE;
				const uint32_t px1 = MIN(px0 + LG::TILE_SIZE, screen_w);
				const uint32_t ci = LG::cell_index(tx, ty, tz, out.grid.grid_dims);
				const uint32_t base = ci * LG::MAX_LIGHTS_PER_CELL;

				const uint32_t cidx = ((tz / LG::COARSE_GROUP) * cy_n + (ty / LG::COARSE_GROUP)) * cx_n + (tx / LG::COARSE_GROUP);
				const uint32_t c_count = coarse_counts[cidx];
				if (c_count == 0u || p_light_count == 0u) {
					out.grid.headers[ci] = { base, 0u };
					continue;
				}

				float z0 = slice_z0;
				float z1 = slice_z1;
				if (p_tile_depths != nullptr) {
					const Vector2 td = p_tile_depths[ty * gx + tx];
					if (float(td.y) <= 0.0f || slice_z0 > float(td.y) * LG::HIZ_FAR_TOLERANCE || slice_z1 < float(td.x) * LG::HIZ_NEAR_TOLERANCE) {
						out.grid.headers[ci] = { base, 0u };
						out.grid.hiz_occluded_cells++;
						continue;
					}
					z0 = MAX(slice_z0, float(td.x) * LG::HIZ_NEAR_TOLERANCE);
					z1 = MIN(slice_z1, float(td.y) * LG::HIZ_FAR_TOLERANCE);
					if (z1 < z0) {
						z1 = z0;
					}
				}

				const LG::CellBounds bounds = LG::compute_cell_bounds(p_inv_proj, screen_w, screen_h, px0, py0, px1, py1, z0, z1);
				const bool coarse_overflowed = c_count > LG::COARSE_LIST_CAP;
				const uint32_t iter_count = coarse_overflowed ? p_light_count : c_count;
				const uint32_t cbase = cidx * LG::COARSE_LIST_CAP;

				uint32_t count = 0u;
				uint32_t overflow = 0u;
				float min_int = 3.402823466e+38f;
				uint32_t min_li = 0u;
				uint32_t min_slot = 0u;

				for (uint32_t k = 0; k < iter_count; k++) {
					const uint32_t li = coarse_overflowed ? k : coarse_indices[cbase + k];
					const LG::PackedLightData &L = p_lights[li];
					if (!LG::light_intersects_cell(L, bounds)) {
						continue;
					}
					if (count < LG::MAX_LIGHTS_PER_CELL) {
						out.grid.indices[base + count] = L.packed_entry;
						if (count == 0u || LG::is_weaker_light(L.intensity, li, min_int, min_li)) {
							min_int = L.intensity;
							min_li = li;
							min_slot = count;
						}
						count++;
					} else {
						overflow++;
						if (LG::is_stronger_light(L.intensity, li, min_int, min_li)) {
							out.grid.indices[base + min_slot] = L.packed_entry;
							min_int = 3.402823466e+38f;
							min_li = 0u;
							min_slot = 0u;
							for (uint32_t s = 0; s < LG::MAX_LIGHTS_PER_CELL; s++) {
								const uint32_t s_li = LG::entry_to_global_index(out.grid.indices[base + s], p_omni_count, p_spot_count);
								const float s_int = p_lights[s_li].intensity;
								if (s == 0u || LG::is_weaker_light(s_int, s_li, min_int, min_li)) {
									min_int = s_int;
									min_li = s_li;
									min_slot = s;
								}
							}
						}
					}
				}

				out.grid.headers[ci] = { base, count };
				if (overflow > 0u) {
					out.grid.stats.overflow_cells++;
					out.grid.stats.overflow_lights += overflow;
				}
				out.grid.stats.total_assignments += count;
				if (count > out.grid.stats.max_cell_count) {
					out.grid.stats.max_cell_count = count;
				}
			}
		}
	}

	return out;
}

} // namespace

TEST_CASE("[Renderer][LightGrid] Grid layout, Z slicing, entry packing, and selection rule (#139)") {
	CHECK_EQ(sizeof(LG::PackedLightData), 80u);
	CHECK_EQ(sizeof(LG::CellHeader), 8u);
	CHECK_EQ(sizeof(LG::GridStats), 32u);
	CHECK_EQ(sizeof(LG::BuildParamsUBO), 144u);

	// 1080p: 64x64 px tiles -> 30x17x32 fine cells, 8x5x8 coarse cells.
	const Vector3i dims_1080p = LG::compute_grid_dims(Size2i(1920, 1080));
	CHECK_EQ(dims_1080p, Vector3i(30, 17, 32));
	CHECK_EQ(LG::compute_coarse_dims(dims_1080p), Vector3i(8, 5, 8));

	// 4K: 60x34x32 fine cells, 15x9x8 coarse cells.
	const Vector3i dims_4k = LG::compute_grid_dims(Size2i(3840, 2160));
	CHECK_EQ(dims_4k, Vector3i(60, 34, 32));
	CHECK_EQ(LG::compute_coarse_dims(dims_4k), Vector3i(15, 9, 8));

	// Logarithmic Z slicing (perspective) and linear Z slicing (orthographic).
	for (int mode = 0; mode < 2; mode++) {
		const bool ortho = (mode == 1);
		const LG::ZParams zp = LG::compute_z_params(0.05f, 200.0f, ortho);
		float prev_z1 = zp.z_near;
		for (uint32_t s = 0; s < LG::Z_SLICES; s++) {
			float z0 = 0.0f;
			float z1 = 0.0f;
			LG::slice_depth_bounds(s, s + 1u, zp, z0, z1);
			CHECK(z1 > z0);
			CHECK(Math::is_equal_approx(z0, prev_z1, 1e-4f));
			const float mid = 0.5f * (z0 + z1);
			CHECK_EQ(LG::depth_to_slice(mid, zp), s);
			prev_z1 = z1;
		}
		CHECK(Math::is_equal_approx(prev_z1, zp.z_far, 1e-3f));
		CHECK_EQ(LG::depth_to_slice(0.001f, zp), 0u);
		CHECK_EQ(LG::depth_to_slice(500.0f, zp), LG::Z_SLICES - 1u);
	}

	// Packed entry encoding up to 65,536 lights per type + MegaLights flag.
	const uint32_t e_omni = LG::pack_entry(LG::LIGHT_TYPE_OMNI, 65535u, false);
	CHECK_EQ(LG::unpack_entry_type(e_omni), LG::LIGHT_TYPE_OMNI);
	CHECK_EQ(LG::unpack_entry_index(e_omni), 65535u);
	CHECK_FALSE(LG::unpack_entry_megalights(e_omni));

	const uint32_t e_spot = LG::pack_entry(LG::LIGHT_TYPE_SPOT, 12345u, true);
	CHECK_EQ(LG::unpack_entry_type(e_spot), LG::LIGHT_TYPE_SPOT);
	CHECK_EQ(LG::unpack_entry_index(e_spot), 12345u);
	CHECK(LG::unpack_entry_megalights(e_spot));
	CHECK_EQ(LG::entry_to_global_index(e_spot, 1000u, 20000u), 13345u);

	const uint32_t e_area = LG::pack_entry(LG::LIGHT_TYPE_AREA, 42u, false);
	CHECK_EQ(LG::unpack_entry_type(e_area), LG::LIGHT_TYPE_AREA);
	CHECK_EQ(LG::unpack_entry_index(e_area), 42u);
	CHECK_EQ(LG::entry_to_global_index(e_area, 1000u, 2000u), 3042u);

	// Selection rule: packed grid active only when MegaLights is enabled or light count exceeds bitmask cap.
	CHECK_FALSE(LG::should_use_packed_grid(false, 0u, 512u));
	CHECK_FALSE(LG::should_use_packed_grid(false, 512u, 512u));
	CHECK(LG::should_use_packed_grid(false, 513u, 512u));
	CHECK(LG::should_use_packed_grid(false, 4096u, 512u));
	CHECK(LG::should_use_packed_grid(true, 0u, 512u));
	CHECK(LG::should_use_packed_grid(true, 64u, 512u));
}

TEST_CASE("[Renderer][LightGrid] Overflow keeps 64 highest-intensity lights and counts overflow (#139)") {
	const Size2i screen(128, 128);
	Projection proj;
	proj.set_perspective(60.0, 1.0, 0.1, 100.0);
	Projection corr;
	corr.set_depth_correction(true);
	const Projection full_proj = corr * proj;
	float inv_proj[16];
	LG::store_projection_f32(full_proj.inverse(), inv_proj);

	// 100 overlapping omni lights covering the entire frustum with known intensities.
	const uint32_t total_lights = 100u;
	LocalVector<LG::PackedLightData> lights;
	lights.resize(total_lights);
	for (uint32_t i = 0; i < total_lights; i++) {
		// Non-monotonic intensity pattern with deliberate ties on pairs (2k, 2k+1).
		const float intensity = float((i / 2u) * 3u % 50u) + 1.0f;
		lights[i] = LG::make_packed_light(
				LG::LIGHT_TYPE_OMNI,
				i,
				Transform3D(),
				Transform3D(Basis(), Vector3(0.0f, 0.0f, -5.0f)),
				200.0f,
				45.0f,
				Vector2(1.0f, 1.0f),
				90.0f,
				0.2f,
				intensity,
				false);
	}

	const LG::ReferenceGrid ref = LG::build_cpu_reference(lights.ptr(), total_lights, total_lights, 0u, inv_proj, screen, 0.1f, 100.0f, false, nullptr);
	const TwoPassResult two_pass = run_two_pass_cpu(lights.ptr(), total_lights, total_lights, 0u, inv_proj, screen, 0.1f, 100.0f, false, nullptr);

	CHECK_EQ(ref.stats.max_cell_count, LG::MAX_LIGHTS_PER_CELL);
	CHECK_EQ(ref.stats.overflow_cells, uint32_t(ref.headers.size()));
	CHECK_EQ(ref.stats.overflow_lights, uint32_t(ref.headers.size()) * (total_lights - LG::MAX_LIGHTS_PER_CELL));

	// Two-pass result must match brute-force reference with zero discrepancies even under overflow.
	const LG::GridDiff diff = LG::compare_grids(ref, two_pass.grid.headers.ptr(), two_pass.grid.indices.ptr(), nullptr, 0.1f, 100.0f, false);
	CHECK_EQ(diff.missing_lights, 0u);
	CHECK_EQ(diff.extra_lights, 0u);
	CHECK_EQ(diff.mismatched_cells, 0u);
	CHECK_EQ(two_pass.grid.stats.overflow_cells, ref.stats.overflow_cells);
	CHECK_EQ(two_pass.grid.stats.overflow_lights, ref.stats.overflow_lights);

	// Verify cell 0 contains strictly the top 64 lights according to (intensity, -index).
	LocalVector<uint32_t> sorted_all;
	sorted_all.resize(total_lights);
	for (uint32_t i = 0; i < total_lights; i++) {
		sorted_all[i] = i;
	}
	for (uint32_t i = 0; i < total_lights; i++) {
		for (uint32_t j = i + 1; j < total_lights; j++) {
			if (LG::is_stronger_light(lights[sorted_all[j]].intensity, sorted_all[j], lights[sorted_all[i]].intensity, sorted_all[i])) {
				SWAP(sorted_all[i], sorted_all[j]);
			}
		}
	}

	const LG::CellHeader h0 = two_pass.grid.headers[0];
	CHECK_EQ(h0.count, LG::MAX_LIGHTS_PER_CELL);
	for (uint32_t rank = 0; rank < LG::MAX_LIGHTS_PER_CELL; rank++) {
		const uint32_t expected_entry = lights[sorted_all[rank]].packed_entry;
		bool found = false;
		for (uint32_t k = 0; k < h0.count; k++) {
			if (two_pass.grid.indices[h0.offset + k] == expected_entry) {
				found = true;
				break;
			}
		}
		CHECK(found);
	}
}

TEST_CASE("[Renderer][LightGrid] Barn-door frustum culling and AreaLight3D bounds (#139)") {
	// Open defaults preserve legacy full range box.
	CHECK_FALSE(LG::is_barn_door_active(90.0f, 0.2f));
	CHECK_FALSE(LG::is_barn_door_active(30.0f, 0.0f));
	CHECK(LG::is_barn_door_active(30.0f, 0.5f));

	const Vector2 open_ext = LG::area_light_barn_door_aabb_extents(Vector2(1.0f, 1.0f), 10.0f, 90.0f, 0.2f);
	CHECK(Math::is_equal_approx(open_ext.x, 10.5f));
	CHECK(Math::is_equal_approx(open_ext.y, 10.5f));

	// Narrow collimated barn doors shrink AABB significantly below the full range box.
	const Vector2 tight_ext = LG::area_light_barn_door_aabb_extents(Vector2(0.5f, 0.5f), 10.0f, 10.0f, 1.5f);
	CHECK(tight_ext.x < 6.0f);
	CHECK(tight_ext.y < 6.0f);
	CHECK(tight_ext.x > 0.25f);

	// Zero-angle hood longer than range collimates strictly to the emitter half-extents.
	const Vector2 hood_ext = LG::area_light_barn_door_aabb_extents(Vector2(1.0f, 0.6f), 2.0f, 0.0f, 3.0f);
	CHECK(Math::is_equal_approx(hood_ext.x, 0.5f, 1e-4f));
	CHECK(Math::is_equal_approx(hood_ext.y, 0.3f, 1e-4f));

	// Cell culling: area light at (0, 0, -5) shining along +Z (toward camera) or -Z (away from camera).
	// Here light at (0, 0, -2) shining along -Z (identity transform -> forward = -Z).
	const LG::PackedLightData open_area = LG::make_packed_light(
			LG::LIGHT_TYPE_AREA, 0u, Transform3D(), Transform3D(Basis(), Vector3(0.0f, 0.0f, -2.0f)),
			10.0f, 45.0f, Vector2(0.5f, 0.5f), 90.0f, 0.2f, 5.0f);
	const LG::PackedLightData narrow_area = LG::make_packed_light(
			LG::LIGHT_TYPE_AREA, 0u, Transform3D(), Transform3D(Basis(), Vector3(0.0f, 0.0f, -2.0f)),
			10.0f, 45.0f, Vector2(0.5f, 0.5f), 10.0f, 1.5f, 5.0f);

	auto make_box_cell = [](const Vector3 &p_min, const Vector3 &p_max) {
		LG::CellBounds cb;
		cb.bmin = p_min;
		cb.bmax = p_max;
		for (int i = 0; i < 4; i++) {
			cb.planes[i] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
		}
		return cb;
	};

	// Directly in front of the emitter (f in [2, 3], x in [-0.2, 0.2]): both open and narrow hit.
	const LG::CellBounds on_axis = make_box_cell(Vector3(-0.2f, -0.2f, -5.0f), Vector3(0.2f, 0.2f, -4.0f));
	CHECK(LG::light_intersects_cell(open_area, on_axis));
	CHECK(LG::light_intersects_cell(narrow_area, on_axis));

	// Far off-axis at shallow depth (f in [0.5, 1.0], x in [4.0, 5.0]): within range=10, but outside narrow barn doors!
	const LG::CellBounds off_axis = make_box_cell(Vector3(4.0f, -0.2f, -3.0f), Vector3(5.0f, 0.2f, -2.5f));
	CHECK(LG::light_intersects_cell(open_area, off_axis));
	CHECK_FALSE(LG::light_intersects_cell(narrow_area, off_axis));

	// Behind the emitter plane (f in [-2.0, -0.5]): both reject.
	const LG::CellBounds behind = make_box_cell(Vector3(-0.2f, -0.2f, -1.5f), Vector3(0.2f, 0.2f, -0.5f));
	CHECK_FALSE(LG::light_intersects_cell(open_area, behind));
	CHECK_FALSE(LG::light_intersects_cell(narrow_area, behind));

	// Spot cone culling: narrow 15 deg spot rejects off-axis cell inside bounding sphere; wide 120 deg accepts.
	const LG::PackedLightData narrow_spot = LG::make_packed_light(
			LG::LIGHT_TYPE_SPOT, 0u, Transform3D(), Transform3D(Basis(), Vector3(0.0f, 0.0f, -2.0f)),
			10.0f, 15.0f, Vector2(1.0f, 1.0f), 90.0f, 0.2f, 5.0f);
	const LG::PackedLightData wide_spot = LG::make_packed_light(
			LG::LIGHT_TYPE_SPOT, 0u, Transform3D(), Transform3D(Basis(), Vector3(0.0f, 0.0f, -2.0f)),
			10.0f, 120.0f, Vector2(1.0f, 1.0f), 90.0f, 0.2f, 5.0f);
	CHECK(LG::light_intersects_cell(narrow_spot, on_axis));
	CHECK_FALSE(LG::light_intersects_cell(narrow_spot, off_axis));
	CHECK(LG::light_intersects_cell(wide_spot, off_axis));
	CHECK(LG::light_intersects_cell(wide_spot, behind));
}

TEST_CASE("[Renderer][LightGrid] 8 fixed cameras x 4096 lights brute-force reference parity & Hi-Z culling (#139)") {
	// Generate a deterministic 4,096-light hall (omni, spot, and barn-door rect area lights)
	// distributed across a 64m x 12m x 64m volume with realistic local ranges (3m..6m).
	RandomPCG rng(1394096u, 7u);
	const uint32_t omni_count = 3072u;
	const uint32_t spot_count = 512u;
	const uint32_t area_count = 512u;
	const uint32_t total_lights = omni_count + spot_count + area_count;
	CHECK_EQ(total_lights, 4096u);

	struct WorldLight {
		uint32_t type;
		uint32_t per_type_index;
		Transform3D xform;
		float range;
		float spot_angle;
		Vector2 area_size;
		float bd_angle;
		float bd_length;
		float intensity;
	};

	LocalVector<WorldLight> world_lights;
	world_lights.resize(total_lights);

	auto fill_light = [&](uint32_t p_global, uint32_t p_type, uint32_t p_idx) {
		WorldLight &wl = world_lights[p_global];
		wl.type = p_type;
		wl.per_type_index = p_idx;
		const uint32_t gx = p_global % 32u;
		const uint32_t gz = (p_global / 32u) % 32u;
		const uint32_t gy = p_global / 1024u;
		const Vector3 pos(
				-46.5f + float(gx) * 3.0f + rng.random(-0.25f, 0.25f),
				1.2f + float(gy) * 3.2f + rng.random(-0.2f, 0.2f),
				-46.5f + float(gz) * 3.0f + rng.random(-0.25f, 0.25f));
		Basis rot;
		rot = rot.rotated(Vector3(0, 1, 0), rng.random(0.0f, float(Math::TAU)));
		rot = rot.rotated(Vector3(1, 0, 0), rng.random(-1.1f, 0.4f));
		wl.xform = Transform3D(rot, pos);
		wl.range = rng.random(1.8f, 2.8f);
		wl.spot_angle = rng.random(15.0f, 55.0f);
		wl.area_size = Vector2(rng.random(0.4f, 1.2f), rng.random(0.3f, 0.9f));
		wl.bd_angle = rng.random(15.0f, 75.0f);
		wl.bd_length = rng.random(0.2f, 0.8f);
		wl.intensity = rng.random(0.5f, 10.0f);
	};

	for (uint32_t i = 0; i < omni_count; i++) {
		fill_light(i, LG::LIGHT_TYPE_OMNI, i);
	}
	for (uint32_t i = 0; i < spot_count; i++) {
		fill_light(omni_count + i, LG::LIGHT_TYPE_SPOT, i);
	}
	for (uint32_t i = 0; i < area_count; i++) {
		fill_light(omni_count + spot_count + i, LG::LIGHT_TYPE_AREA, i);
	}

	// 8 fixed cameras covering corners, corridors, top-down, tilted, and orthographic views.
	struct CameraSpec {
		Vector3 pos;
		Vector3 target;
		float fov_or_size;
		bool ortho;
		float z_near;
		float z_far;
	};
	const CameraSpec cameras[8] = {
		{ Vector3(0.0f, 4.0f, 34.0f), Vector3(0.0f, 3.0f, 0.0f), 65.0f, false, 0.1f, 100.0f },
		{ Vector3(-28.0f, 6.0f, -28.0f), Vector3(0.0f, 2.0f, 0.0f), 70.0f, false, 0.1f, 100.0f },
		{ Vector3(28.0f, 2.5f, -24.0f), Vector3(-10.0f, 3.0f, 12.0f), 55.0f, false, 0.05f, 90.0f },
		{ Vector3(-24.0f, 8.0f, 22.0f), Vector3(12.0f, 1.5f, -15.0f), 60.0f, false, 0.1f, 100.0f },
		{ Vector3(0.0f, 22.0f, 2.0f), Vector3(0.0f, 0.0f, 0.0f), 75.0f, false, 0.2f, 80.0f },
		{ Vector3(14.0f, 2.0f, 0.0f), Vector3(-20.0f, 4.0f, 0.0f), 50.0f, false, 0.1f, 100.0f },
		{ Vector3(-12.0f, 1.8f, 14.0f), Vector3(16.0f, 5.0f, -16.0f), 80.0f, false, 0.1f, 120.0f },
		{ Vector3(0.0f, 30.0f, 0.5f), Vector3(0.0f, 0.0f, 0.0f), 40.0f, true, 0.5f, 60.0f },
	};

	const Size2i screen(1920, 1080); // Exact 1080p spec: 30x17x32 froxels, 8x5x8 coarse cells.
	const Vector3i gdims = LG::compute_grid_dims(screen);
	const uint32_t tile_count = uint32_t(gdims.x * gdims.y);

	LocalVector<LG::PackedLightData> view_lights;
	view_lights.resize(total_lights);
	LocalVector<Vector2> tile_depths;
	tile_depths.resize(tile_count);

	for (int cam_idx = 0; cam_idx < 8; cam_idx++) {
		const CameraSpec &cs = cameras[cam_idx];
		Transform3D cam_xform;
		cam_xform.origin = cs.pos;
		cam_xform.set_look_at(cs.pos, cs.target, Vector3(0, 1, 0));
		const Transform3D view_from_world = cam_xform.affine_inverse();

		Projection proj;
		const real_t aspect = real_t(screen.x) / real_t(screen.y);
		if (cs.ortho) {
			proj.set_orthogonal(cs.fov_or_size, aspect, cs.z_near, cs.z_far);
		} else {
			proj.set_perspective(cs.fov_or_size, aspect, cs.z_near, cs.z_far);
		}
		Projection corr;
		corr.set_depth_correction(true);
		const Projection full_proj = corr * proj;
		float inv_proj[16];
		LG::store_projection_f32(full_proj.inverse(), inv_proj);

		for (uint32_t i = 0; i < total_lights; i++) {
			const WorldLight &wl = world_lights[i];
			view_lights[i] = LG::make_packed_light(
					wl.type, wl.per_type_index, view_from_world, wl.xform,
					wl.range, wl.spot_angle, wl.area_size, wl.bd_angle, wl.bd_length, wl.intensity, false);
		}

		// Simulate prepass Hi-Z depth buffer: left half of screen hits an opaque baffle wall at z=8m,
		// right half sees a receding hall surface across [6m, 26m] with realistic per-tile depth spread,
		// verifying Hi-Z culls cells behind opaque geometry while preserving 0 overflow at 4,096 lights.
		for (int ty = 0; ty < gdims.y; ty++) {
			for (int tx = 0; tx < gdims.x; tx++) {
				if (tx < gdims.x / 2) {
					tile_depths[ty * gdims.x + tx] = Vector2(7.5f, 8.0f);
				} else {
					const float t = float(ty) / float(MAX(gdims.y - 1, 1));
					const float d = 6.0f + t * 20.0f;
					tile_depths[ty * gdims.x + tx] = Vector2(d - 1.0f, d + 1.5f);
				}
			}
		}

		const LG::ReferenceGrid ref = LG::build_cpu_reference(
				view_lights.ptr(), total_lights, omni_count, spot_count,
				inv_proj, screen, cs.z_near, cs.z_far, cs.ortho, tile_depths.ptr());
		const TwoPassResult tp = run_two_pass_cpu(
				view_lights.ptr(), total_lights, omni_count, spot_count,
				inv_proj, screen, cs.z_near, cs.z_far, cs.ortho, tile_depths.ptr());

		const LG::GridDiff diff = LG::compare_grids(
				ref, tp.grid.headers.ptr(), tp.grid.indices.ptr(),
				tile_depths.ptr(), cs.z_near, cs.z_far, cs.ortho);

		CHECK_EQ(diff.missing_lights, 0u);
		CHECK_EQ(diff.extra_lights, 0u);
		CHECK_EQ(diff.mismatched_cells, 0u);
		CHECK_EQ(diff.hiz_occluded_nonempty_cells, 0u);
		CHECK_EQ(tp.grid.stats.overflow_cells, 0u);
		CHECK_EQ(tp.grid.stats.overflow_lights, 0u);
		CHECK(ref.hiz_occluded_cells > 0u);
		CHECK(tp.grid.stats.total_assignments > 0u);
	}
}

TEST_CASE("[Renderer][LightGrid] CPU light-geometry pairing skip rule (#139)") {
	// Mobile / Compatibility renderer NEVER skips geometry pairing.
	CHECK_FALSE(RendererSceneCull::light_should_skip_geometry_pairing(true, true, RSE::LIGHT_SHADOW_METHOD_RAY_TRACED, true, false, false));
	CHECK_FALSE(RendererSceneCull::light_should_skip_geometry_pairing(true, false, RSE::LIGHT_SHADOW_METHOD_PROJECT, false, false, false));
	CHECK_FALSE(RendererSceneCull::light_should_skip_geometry_pairing(true, true, RSE::LIGHT_SHADOW_METHOD_ATLAS, false, false, false));

	// Forward+ skips RAY_TRACED lights unconditionally (no shadow map needed).
	CHECK(RendererSceneCull::light_should_skip_geometry_pairing(false, true, RSE::LIGHT_SHADOW_METHOD_RAY_TRACED, false, false, false));
	CHECK(RendererSceneCull::light_should_skip_geometry_pairing(false, true, RSE::LIGHT_SHADOW_METHOD_RAY_TRACED, false, true, true));
	CHECK(RendererSceneCull::light_should_skip_geometry_pairing(false, true, RSE::LIGHT_SHADOW_METHOD_RAY_TRACED, true, false, false));

	// Forward+ skips PROJECT lights under MegaLights, while keeping explicit ATLAS lights paired.
	CHECK(RendererSceneCull::light_should_skip_geometry_pairing(false, true, RSE::LIGHT_SHADOW_METHOD_PROJECT, true, false, false));
	CHECK_FALSE(RendererSceneCull::light_should_skip_geometry_pairing(false, true, RSE::LIGHT_SHADOW_METHOD_ATLAS, true, false, false));

	// Forward+ keeps shadow-map lights (PROJECT/ATLAS/VIRTUAL) paired when MegaLights is off.
	CHECK_FALSE(RendererSceneCull::light_should_skip_geometry_pairing(false, true, RSE::LIGHT_SHADOW_METHOD_PROJECT, false, false, false));
	CHECK_FALSE(RendererSceneCull::light_should_skip_geometry_pairing(false, true, RSE::LIGHT_SHADOW_METHOD_ATLAS, false, false, false));
	CHECK_FALSE(RendererSceneCull::light_should_skip_geometry_pairing(false, true, RSE::LIGHT_SHADOW_METHOD_VIRTUAL, false, false, false));

	// Unshadowed lights on Forward+: skip when no projector/softshadow flag, keep when projector or softshadow is used.
	CHECK(RendererSceneCull::light_should_skip_geometry_pairing(false, false, RSE::LIGHT_SHADOW_METHOD_PROJECT, false, false, false));
	CHECK_FALSE(RendererSceneCull::light_should_skip_geometry_pairing(false, false, RSE::LIGHT_SHADOW_METHOD_PROJECT, false, true, false));
	CHECK_FALSE(RendererSceneCull::light_should_skip_geometry_pairing(false, false, RSE::LIGHT_SHADOW_METHOD_PROJECT, false, false, true));
}

TEST_CASE("[SceneTree][LightGrid] AreaLight3D barn_door_angle and barn_door_length properties (#139)") {
	AreaLight3D *area = memnew(AreaLight3D);
	CHECK(Math::is_equal_approx(float(area->get_param(Light3D::PARAM_BARN_DOOR_ANGLE)), 90.0f));
	CHECK(Math::is_equal_approx(float(area->get_param(Light3D::PARAM_BARN_DOOR_LENGTH)), 0.2f));
	CHECK(Math::is_equal_approx(float(area->get("barn_door_angle")), 90.0f));
	CHECK(Math::is_equal_approx(float(area->get("barn_door_length")), 0.2f));

	area->set("barn_door_angle", 25.0);
	area->set("barn_door_length", 1.25);
	CHECK(Math::is_equal_approx(float(area->get_param(Light3D::PARAM_BARN_DOOR_ANGLE)), 25.0f));
	CHECK(Math::is_equal_approx(float(area->get_param(Light3D::PARAM_BARN_DOOR_LENGTH)), 1.25f));
	CHECK(Math::is_equal_approx(float(area->get("barn_door_angle")), 25.0f));
	CHECK(Math::is_equal_approx(float(area->get("barn_door_length")), 1.25f));

	memdelete(area);
}
