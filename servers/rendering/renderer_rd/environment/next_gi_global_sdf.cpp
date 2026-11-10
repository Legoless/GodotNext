/**************************************************************************/
/*  next_gi_global_sdf.cpp                                                */
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

#include "next_gi_global_sdf.h"

#include "core/math/math_funcs.h"

static int _floor_div(int p_value, int p_divisor) {
	int quotient = p_value / p_divisor;
	if ((p_value % p_divisor) != 0 && ((p_value < 0) != (p_divisor < 0))) {
		quotient--;
	}
	return quotient;
}

static int _floor_mod(int p_value, int p_divisor) {
	return p_value - _floor_div(p_value, p_divisor) * p_divisor;
}

uint64_t NextGIGlobalSDF::_brick_key(const Vector3i &p_brick) {
	// 21 bits per axis covers every level and scroll position.
	return (uint64_t(uint32_t(p_brick.x) & 0x1FFFFFu)) |
			(uint64_t(uint32_t(p_brick.y) & 0x1FFFFFu) << 21) |
			(uint64_t(uint32_t(p_brick.z) & 0x1FFFFFu) << 42);
}

uint32_t NextGIGlobalSDF::level_count(float p_view_distance) {
	uint32_t count = BASE_LEVELS;
	while (count < MAX_LEVELS && level_extent(count - 1) < p_view_distance) {
		count++;
	}
	return count;
}

float NextGIGlobalSDF::level_voxel_size(uint32_t p_level) {
	return BASE_VOXEL_SIZE * float(1u << MIN(p_level, 30u));
}

float NextGIGlobalSDF::level_extent(uint32_t p_level) {
	return float(LEVEL_SIZE) * level_voxel_size(p_level);
}

uint32_t NextGIGlobalSDF::level_for_distance(float p_distance, uint32_t p_levels) {
	if (p_levels == 0) {
		return 0;
	}
	const float half_extent = level_extent(0) * 0.5f;
	if (p_distance <= half_extent) {
		return 0;
	}
	const uint32_t level = uint32_t(Math::ceil(Math::log2(p_distance / half_extent)));
	return MIN(level, p_levels - 1);
}

Vector3i NextGIGlobalSDF::scrolled_origin(const Vector3 &p_camera, uint32_t p_level) {
	const float voxel_size = level_voxel_size(p_level);
	const Vector3i voxel(
			int(Math::floor(p_camera.x / voxel_size)) - int(LEVEL_SIZE / 2),
			int(Math::floor(p_camera.y / voxel_size)) - int(LEVEL_SIZE / 2),
			int(Math::floor(p_camera.z / voxel_size)) - int(LEVEL_SIZE / 2));
	return Vector3i(
			_floor_div(voxel.x, int(BRICK_SIZE)) * int(BRICK_SIZE),
			_floor_div(voxel.y, int(BRICK_SIZE)) * int(BRICK_SIZE),
			_floor_div(voxel.z, int(BRICK_SIZE)) * int(BRICK_SIZE));
}

bool NextGIGlobalSDF::level_updates_on_frame(uint32_t p_level, uint64_t p_frame) {
	if (p_level >= MAX_LEVELS) {
		return false;
	}
	const uint64_t interval = uint64_t(1) << p_level;
	return (p_frame & (interval - 1)) == 0;
}

Vector3i NextGIGlobalSDF::scroll_brick_delta(const Vector3i &p_old_origin, const Vector3i &p_new_origin) {
	return Vector3i(
			_floor_div(p_new_origin.x, int(BRICK_SIZE)) - _floor_div(p_old_origin.x, int(BRICK_SIZE)),
			_floor_div(p_new_origin.y, int(BRICK_SIZE)) - _floor_div(p_old_origin.y, int(BRICK_SIZE)),
			_floor_div(p_new_origin.z, int(BRICK_SIZE)) - _floor_div(p_old_origin.z, int(BRICK_SIZE)));
}

uint32_t NextGIGlobalSDF::scrolled_edge_bricks(const Vector3i &p_delta_bricks, Vector3i *r_bricks, uint32_t p_max_bricks) {
	uint32_t count = 0;
	const int page = int(PAGE_SIZE);
	auto append = [&](int x, int y, int z) {
		if (x < 0 || y < 0 || z < 0 || x >= page || y >= page || z >= page || count >= p_max_bricks) {
			return;
		}
		r_bricks[count++] = Vector3i(x, y, z);
	};
	// Walk the new page table and keep bricks that fall outside the shifted
	// old range. A full-table walk is cheap (4096 entries) and exact for
	// multi-axis scrolls.
	for (int z = 0; z < page; z++) {
		for (int y = 0; y < page; y++) {
			for (int x = 0; x < page; x++) {
				const int old_x = x + p_delta_bricks.x;
				const int old_y = y + p_delta_bricks.y;
				const int old_z = z + p_delta_bricks.z;
				if (old_x >= 0 && old_y >= 0 && old_z >= 0 && old_x < page && old_y < page && old_z < page) {
					continue; // Reused by the shift.
				}
				append(x, y, z);
			}
		}
	}
	return count;
}

Vector3i NextGIGlobalSDF::world_to_voxel(uint32_t p_level, const Vector3i &p_origin, const Vector3 &p_world) {
	const float voxel_size = level_voxel_size(p_level);
	return Vector3i(
			int(Math::floor(p_world.x / voxel_size)) - p_origin.x,
			int(Math::floor(p_world.y / voxel_size)) - p_origin.y,
			int(Math::floor(p_world.z / voxel_size)) - p_origin.z);
}

Vector3 NextGIGlobalSDF::voxel_center(uint32_t p_level, const Vector3i &p_origin, const Vector3i &p_voxel) {
	const float voxel_size = level_voxel_size(p_level);
	return Vector3(
			float(p_origin.x + p_voxel.x) * voxel_size + voxel_size * 0.5f,
			float(p_origin.y + p_voxel.y) * voxel_size + voxel_size * 0.5f,
			float(p_origin.z + p_voxel.z) * voxel_size + voxel_size * 0.5f);
}

float NextGIGlobalSDF::sample_instance(const Instance &p_instance, const Vector3 &p_world) {
	if (p_instance.field == nullptr || !p_instance.field->is_valid()) {
		return 1e30f;
	}
	// GodotNext: ps_swrt_trace (#186). A lower bound in world meters under any scale, outside
	// the bounds included (nextgi_mesh_sdf_distance_inc.glsl, shared with the composite).
	const Vector3 local = p_instance.transform.affine_inverse().xform(p_world);
	return mesh_world_distance(local, p_instance.field->bounds, mesh_surface_bounds(*p_instance.field), p_instance.field->sample_distance(local), instance_axis_scale(p_instance.transform.basis));
}

// GodotNext: ps_swrt_trace (#186)
Vector3 NextGIGlobalSDF::instance_axis_scale(const Basis &p_basis) {
	const Vector3 columns[3] = { p_basis.get_column(0), p_basis.get_column(1), p_basis.get_column(2) };
	const Vector3 lengths(columns[0].length(), columns[1].length(), columns[2].length());
	bool orthogonal = true;
	for (int i = 0; i < 3; i++) {
		for (int j = i + 1; j < 3; j++) {
			orthogonal = orthogonal && Math::abs(columns[i].dot(columns[j])) <= 1.0e-4f * lengths[i] * lengths[j];
		}
	}
	if (orthogonal) {
		// A rotation times a scale: a local axis keeps its own scale.
		return lengths;
	}
	// Sheared: the smallest singular value, the square root of the smallest eigenvalue of B^T B.
	Basis gram = p_basis.transposed() * p_basis;
	const Basis eigenvectors = gram.diagonalize(); // gram becomes the diagonal of eigenvalues.
	(void)eigenvectors;
	const float smallest = Math::sqrt(MAX(MIN(gram.rows[0][0], MIN(gram.rows[1][1], gram.rows[2][2])), 0.0f));
	return Vector3(smallest, smallest, smallest);
}

AABB NextGIGlobalSDF::mesh_surface_bounds(const MeshSDFData &p_field) {
	const float band = p_field.mip_bands.is_empty() ? 0.0f : p_field.mip_bands[0];
	const Vector3 center = p_field.bounds.get_center();
	const Vector3 minimum = (p_field.bounds.position + Vector3(band, band, band)).min(center);
	const Vector3 maximum = (p_field.bounds.get_end() - Vector3(band, band, band)).max(center);
	return AABB(minimum, maximum - minimum);
}

float NextGIGlobalSDF::mesh_world_distance(const Vector3 &p_local, const AABB &p_bounds, const AABB &p_surface, float p_sampled, const Vector3 &p_axis_scale) {
	float distance = p_sampled * MAX(MIN(p_axis_scale.x, MIN(p_axis_scale.y, p_axis_scale.z)), 1.0e-6f);
	const Vector3 excess = (p_local - p_local.clamp(p_bounds.position, p_bounds.get_end())) * p_axis_scale;
	if (excess.length_squared() > 0.0f) {
		const float boundary = MAX(distance, 0.0f);
		distance = Math::sqrt(excess.length_squared() + boundary * boundary);
	}
	const float oriented = ((p_local - p_local.clamp(p_surface.position, p_surface.get_end())) * p_axis_scale).length();
	if (oriented > 0.0f) {
		distance = MAX(distance, oriented);
	}
	return distance;
}

float NextGIGlobalSDF::reference_distance(const Vector<Instance> &p_instances, const Vector3 &p_world) {
	float distance = 1e30f;
	for (const Instance &instance : p_instances) {
		distance = MIN(distance, sample_instance(instance, p_world));
	}
	return distance;
}

void NextGIGlobalSDF::compose_brick(const Vector<Instance> &p_instances, uint32_t p_level, const Vector3i &p_origin, const Vector3i &p_brick, float *r_voxels) {
	const Vector3i base = p_brick * int(BRICK_SIZE);
	for (int z = 0; z < int(BRICK_SIZE); z++) {
		for (int y = 0; y < int(BRICK_SIZE); y++) {
			for (int x = 0; x < int(BRICK_SIZE); x++) {
				const Vector3 world = voxel_center(p_level, p_origin, base + Vector3i(x, y, z));
				r_voxels[x + y * int(BRICK_SIZE) + z * int(BRICK_SIZE * BRICK_SIZE)] = reference_distance(p_instances, world);
			}
		}
	}
}

static uint32_t _voxel_index(const Vector3i &p_voxel) {
	return uint32_t(p_voxel.x) + uint32_t(p_voxel.y) * NextGIGlobalSDF::BRICK_SIZE + uint32_t(p_voxel.z) * NextGIGlobalSDF::BRICK_SIZE * NextGIGlobalSDF::BRICK_SIZE;
}

bool NextGIGlobalSDF::get_coverage_bit(const BrickSplit &p_split, const Vector3i &p_voxel) {
	const uint32_t index = _voxel_index(p_voxel);
	return (p_split.coverage[index >> 3] & (1u << (index & 7))) != 0;
}

void NextGIGlobalSDF::set_coverage_bit(BrickSplit &r_split, const Vector3i &p_voxel, bool p_value) {
	const uint32_t index = _voxel_index(p_voxel);
	if (p_value) {
		r_split.coverage[index >> 3] |= uint8_t(1u << (index & 7));
	} else {
		r_split.coverage[index >> 3] &= uint8_t(~(1u << (index & 7)));
	}
}

void NextGIGlobalSDF::compose_brick_split(const Vector<Instance> &p_instances, const uint32_t *p_culled, uint32_t p_culled_count, uint32_t p_level, const Vector3i &p_origin, const Vector3i &p_brick, BrickSplit &r_split) {
	r_split.static_distance.resize(BRICK_VOXELS);
	r_split.dynamic_distance.resize(BRICK_VOXELS);
	for (uint32_t voxel = 0; voxel < BRICK_VOXELS; voxel++) {
		r_split.static_distance[voxel] = 1e30f;
		r_split.dynamic_distance[voxel] = 1e30f;
	}
	memset(r_split.coverage, 0, sizeof(r_split.coverage));
	const Vector3i base = p_brick * int(BRICK_SIZE);
	for (uint32_t culled = 0; culled < p_culled_count; culled++) {
		const uint32_t index = p_culled[culled];
		if (index >= uint32_t(p_instances.size())) {
			continue;
		}
		const Instance &instance = p_instances[index];
		if (instance.field == nullptr || !instance.field->is_valid()) {
			continue;
		}
		LocalVector<float> &target = instance.dynamic ? r_split.dynamic_distance : r_split.static_distance;
		for (int z = 0; z < int(BRICK_SIZE); z++) {
			for (int y = 0; y < int(BRICK_SIZE); y++) {
				for (int x = 0; x < int(BRICK_SIZE); x++) {
					const Vector3 world = voxel_center(p_level, p_origin, base + Vector3i(x, y, z));
					const float distance = sample_instance(instance, world);
					const uint32_t voxel = _voxel_index(Vector3i(x, y, z));
					if (distance < target[voxel]) {
						target[voxel] = distance;
					}
				}
			}
		}
	}
	// GodotNext: ps_swrt_trace (#186). Unlisted instances lie outside the cull box (brick_cap()).
	const AABB cull_box = brick_cull_box(p_level, p_origin, p_brick);
	for (int z = 0; z < int(BRICK_SIZE); z++) {
		for (int y = 0; y < int(BRICK_SIZE); y++) {
			for (int x = 0; x < int(BRICK_SIZE); x++) {
				const uint32_t voxel = _voxel_index(Vector3i(x, y, z));
				r_split.static_distance[voxel] = MIN(r_split.static_distance[voxel], brick_cap(cull_box, voxel_center(p_level, p_origin, base + Vector3i(x, y, z))));
			}
		}
	}
	// Recompute the coverage from the winning samples (two-sided instance wins
	// tie with the static side).
	for (uint32_t culled = 0; culled < p_culled_count; culled++) {
		const uint32_t index = p_culled[culled];
		if (index >= uint32_t(p_instances.size())) {
			continue;
		}
		const Instance &instance = p_instances[index];
		if (instance.field == nullptr || !instance.field->is_valid() || !instance.two_sided) {
			continue;
		}
		LocalVector<float> &target = instance.dynamic ? r_split.dynamic_distance : r_split.static_distance;
		for (int z = 0; z < int(BRICK_SIZE); z++) {
			for (int y = 0; y < int(BRICK_SIZE); y++) {
				for (int x = 0; x < int(BRICK_SIZE); x++) {
					const Vector3 world = voxel_center(p_level, p_origin, base + Vector3i(x, y, z));
					const float distance = sample_instance(instance, world);
					const uint32_t voxel = _voxel_index(Vector3i(x, y, z));
					if (distance < 1e29f && Math::is_equal_approx(distance, target[voxel])) {
						set_coverage_bit(r_split, Vector3i(x, y, z), true);
					}
				}
			}
		}
	}
}

uint32_t NextGIGlobalSDF::cull_instances_for_brick(const Vector<Instance> &p_instances, uint32_t p_level, const Vector3i &p_origin, const Vector3i &p_brick, uint32_t *r_indices, uint32_t p_max_indices) {
	// GodotNext: ps_swrt_trace (#186). brick_cull_box(), shared with bin_instances() and brick_cap().
	const AABB brick_aabb = brick_cull_box(p_level, p_origin, p_brick);
	uint32_t count = 0;
	for (uint32_t index = 0; index < uint32_t(p_instances.size()); index++) {
		const Instance &instance = p_instances[index];
		if (instance.field == nullptr || !instance.field->is_valid()) {
			continue;
		}
		const AABB world_bounds = instance.transform.xform(instance.field->bounds);
		if (!world_bounds.intersects(brick_aabb)) {
			continue;
		}
		if (count < p_max_indices) {
			r_indices[count] = index;
		}
		count++;
	}
	return MIN(count, p_max_indices);
}

// GodotNext: ps_swrt_trace (#186)
AABB NextGIGlobalSDF::brick_cull_box(uint32_t p_level, const Vector3i &p_origin, const Vector3i &p_brick) {
	const float voxel = level_voxel_size(p_level);
	const float brick_size = voxel * float(BRICK_SIZE);
	const Vector3 brick_min(
			float(p_origin.x + p_brick.x * int(BRICK_SIZE)) * voxel,
			float(p_origin.y + p_brick.y * int(BRICK_SIZE)) * voxel,
			float(p_origin.z + p_brick.z * int(BRICK_SIZE)) * voxel);
	AABB box(brick_min, Vector3(brick_size, brick_size, brick_size));
	box.grow_by(voxel);
	return box;
}

float NextGIGlobalSDF::brick_cap(const AABB &p_cull_box, const Vector3 &p_world) {
	const Vector3 inside = (p_world - p_cull_box.position).min(p_cull_box.get_end() - p_world);
	return MAX(MIN(inside.x, MIN(inside.y, inside.z)), 0.0f);
}

float NextGIGlobalSDF::composite_distance(const Vector<Instance> &p_instances, const uint32_t *p_culled, uint32_t p_culled_count, const AABB &p_cull_box, const Vector3 &p_world) {
	float distance = brick_cap(p_cull_box, p_world);
	for (uint32_t index = 0; index < p_culled_count; index++) {
		if (p_culled[index] < uint32_t(p_instances.size())) {
			distance = MIN(distance, sample_instance(p_instances[p_culled[index]], p_world));
		}
	}
	return distance;
}

void NextGIGlobalSDF::bin_instances(const Vector<Instance> &p_instances, uint32_t p_level, const Vector3i &p_origin, LocalVector<uint32_t> &r_offsets, LocalVector<uint32_t> &r_indices) {
	r_offsets.resize(BRICKS_PER_LEVEL + 1);
	for (uint32_t index = 0; index <= BRICKS_PER_LEVEL; index++) {
		r_offsets[index] = 0;
	}
	r_indices.clear();
	const float voxel = level_voxel_size(p_level);
	const float brick_size = voxel * float(BRICK_SIZE);
	const Vector3 level_min = Vector3(p_origin) * voxel;
	const int page = int(PAGE_SIZE);
	LocalVector<AABB> bounds;
	bounds.resize(p_instances.size());
	// Brick ranges first (counting pass), then the fill pass in instance order.
	LocalVector<int> ranges;
	ranges.resize(p_instances.size() * 6);
	for (uint32_t index = 0; index < uint32_t(p_instances.size()); index++) {
		int *range = &ranges[index * 6];
		for (int axis = 0; axis < 3; axis++) {
			range[axis] = 1; // Empty until the bounds are known.
			range[axis + 3] = 0;
		}
		const Instance &instance = p_instances[index];
		if (instance.field == nullptr || !instance.field->is_valid()) {
			continue;
		}
		const AABB world_bounds = instance.transform.xform(instance.field->bounds);
		bounds[index] = world_bounds;
		// A brick is reached when the bounds touch the brick grown by one voxel.
		const Vector3 lo = (world_bounds.position - Vector3(voxel, voxel, voxel) - level_min) / brick_size;
		const Vector3 hi = (world_bounds.position + world_bounds.size + Vector3(voxel, voxel, voxel) - level_min) / brick_size;
		for (int axis = 0; axis < 3; axis++) {
			range[axis] = CLAMP(int(Math::floor(lo[axis])), 0, page);
			range[axis + 3] = CLAMP(int(Math::floor(hi[axis])), -1, page - 1);
		}
		for (int z = range[2]; z <= range[5]; z++) {
			for (int y = range[1]; y <= range[4]; y++) {
				for (int x = range[0]; x <= range[3]; x++) {
					r_offsets[uint32_t(x) + PAGE_SIZE * (uint32_t(y) + PAGE_SIZE * uint32_t(z)) + 1]++;
				}
			}
		}
	}
	for (uint32_t brick = 0; brick < BRICKS_PER_LEVEL; brick++) {
		r_offsets[brick + 1] += r_offsets[brick];
	}
	LocalVector<uint32_t> cursor;
	cursor.resize(BRICKS_PER_LEVEL);
	for (uint32_t brick = 0; brick < BRICKS_PER_LEVEL; brick++) {
		cursor[brick] = r_offsets[brick];
	}
	r_indices.resize(r_offsets[BRICKS_PER_LEVEL]);
	uint32_t kept = 0;
	for (uint32_t index = 0; index < uint32_t(p_instances.size()); index++) {
		const int *range = &ranges[index * 6];
		for (int z = range[2]; z <= range[5]; z++) {
			for (int y = range[1]; y <= range[4]; y++) {
				for (int x = range[0]; x <= range[3]; x++) {
					const uint32_t brick = uint32_t(x) + PAGE_SIZE * (uint32_t(y) + PAGE_SIZE * uint32_t(z));
					// The floor-based range can include a brick the bounds only touch
					// at an edge; keep exactly the bricks the per-brick cull keeps.
					if (!bounds[index].intersects(brick_cull_box(p_level, p_origin, Vector3i(x, y, z)))) {
						continue;
					}
					r_indices[cursor[brick]++] = index;
				}
			}
		}
	}
	// Compact away the slots rejected by the exact test.
	LocalVector<uint32_t> compact;
	compact.resize(r_indices.size());
	uint32_t write = 0;
	for (uint32_t brick = 0; brick < BRICKS_PER_LEVEL; brick++) {
		const uint32_t begin = r_offsets[brick];
		const uint32_t end = cursor[brick];
		r_offsets[brick] = write;
		for (uint32_t slot = begin; slot < end; slot++) {
			compact[write++] = r_indices[slot];
		}
		kept = write;
	}
	r_offsets[BRICKS_PER_LEVEL] = kept;
	compact.resize(kept);
	r_indices = compact;
}

void NextGIGlobalSDF::dirty_bricks(const AABB &p_old_world, const AABB &p_new_world, const Vector3i &p_origin, Vector3i *r_bricks, uint32_t p_max_bricks, uint32_t &r_count, uint32_t p_level) {
	r_count = 0;
	const float brick_world = level_voxel_size(p_level) * float(BRICK_SIZE);
	auto add = [&](const AABB &p_aabb) {
		if (p_aabb.size.x < 0.0f || p_aabb.size.y < 0.0f || p_aabb.size.z < 0.0f) {
			return;
		}
		const Vector3i min_brick(
				int(Math::floor(p_aabb.position.x / brick_world)) - _floor_div(p_origin.x, int(BRICK_SIZE)),
				int(Math::floor(p_aabb.position.y / brick_world)) - _floor_div(p_origin.y, int(BRICK_SIZE)),
				int(Math::floor(p_aabb.position.z / brick_world)) - _floor_div(p_origin.z, int(BRICK_SIZE)));
		const Vector3i max_brick(
				int(Math::floor((p_aabb.position.x + p_aabb.size.x) / brick_world)) - _floor_div(p_origin.x, int(BRICK_SIZE)),
				int(Math::floor((p_aabb.position.y + p_aabb.size.y) / brick_world)) - _floor_div(p_origin.y, int(BRICK_SIZE)),
				int(Math::floor((p_aabb.position.z + p_aabb.size.z) / brick_world)) - _floor_div(p_origin.z, int(BRICK_SIZE)));
		for (int z = min_brick.z; z <= max_brick.z; z++) {
			for (int y = min_brick.y; y <= max_brick.y; y++) {
				for (int x = min_brick.x; x <= max_brick.x; x++) {
					const Vector3i brick(x, y, z);
					if (brick.x < 0 || brick.y < 0 || brick.z < 0 || brick.x >= int(PAGE_SIZE) || brick.y >= int(PAGE_SIZE) || brick.z >= int(PAGE_SIZE)) {
						continue;
					}
					bool exists = false;
					for (uint32_t index = 0; index < r_count; index++) {
						if (r_bricks[index] == brick) {
							exists = true;
							break;
						}
					}
					if (!exists && r_count < p_max_bricks) {
						r_bricks[r_count++] = brick;
					}
				}
			}
		}
	};
	add(p_old_world);
	add(p_new_world);
}

void NextGIGlobalSDF::build_coarse(const Vector<Instance> &p_instances, uint32_t p_level, const Vector3i &p_origin, uint32_t p_iterations, LocalVector<float> &r_coarse) {
	r_coarse.resize(COARSE_VOXELS);
	const float step = coarse_voxel_size(p_level);
	for (uint32_t z = 0; z < COARSE_SIZE; z++) {
		for (uint32_t y = 0; y < COARSE_SIZE; y++) {
			for (uint32_t x = 0; x < COARSE_SIZE; x++) {
				// The coarse voxel sits at 4x the fine voxel stride.
				const Vector3 world(
						float(p_origin.x + int(x) * 4) * level_voxel_size(p_level) + step * 0.5f,
						float(p_origin.y + int(y) * 4) * level_voxel_size(p_level) + step * 0.5f,
						float(p_origin.z + int(z) * 4) * level_voxel_size(p_level) + step * 0.5f);
				r_coarse[x + y * COARSE_SIZE + z * COARSE_SIZE * COARSE_SIZE] = reference_distance(p_instances, world);
			}
		}
	}
	// Fast-sweep relaxation: a neighbor that already holds a finite distance
	// can only lower this voxel, so apply the six-neighbour rule in both
	// directions p_iterations times.
	auto relax = [&](uint32_t x, uint32_t y, uint32_t z) {
		const uint32_t index = x + y * COARSE_SIZE + z * COARSE_SIZE * COARSE_SIZE;
		float value = r_coarse[index];
		if (x > 0) {
			value = MIN(value, r_coarse[index - 1] + step);
		}
		if (y > 0) {
			value = MIN(value, r_coarse[index - COARSE_SIZE] + step);
		}
		if (z > 0) {
			value = MIN(value, r_coarse[index - COARSE_SIZE * COARSE_SIZE] + step);
		}
		if (x + 1 < COARSE_SIZE) {
			value = MIN(value, r_coarse[index + 1] + step);
		}
		if (y + 1 < COARSE_SIZE) {
			value = MIN(value, r_coarse[index + COARSE_SIZE] + step);
		}
		if (z + 1 < COARSE_SIZE) {
			value = MIN(value, r_coarse[index + COARSE_SIZE * COARSE_SIZE] + step);
		}
		r_coarse[index] = value;
	};
	for (uint32_t iteration = 0; iteration < p_iterations; iteration++) {
		for (uint32_t z = 0; z < COARSE_SIZE; z++) {
			for (uint32_t y = 0; y < COARSE_SIZE; y++) {
				for (uint32_t x = 0; x < COARSE_SIZE; x++) {
					relax(x, y, z);
				}
			}
		}
		for (int z = int(COARSE_SIZE) - 1; z >= 0; z--) {
			for (int y = int(COARSE_SIZE) - 1; y >= 0; y--) {
				for (int x = int(COARSE_SIZE) - 1; x >= 0; x--) {
					relax(uint32_t(x), uint32_t(y), uint32_t(z));
				}
			}
		}
	}
}

uint32_t NextGIGlobalSDF::trace_coarse_steps(const LocalVector<float> &p_coarse, uint32_t p_level, const Vector3i &p_origin, const Vector3 &p_ray_origin, const Vector3 &p_ray_direction, uint32_t p_max_steps) {
	if (p_level >= MAX_LEVELS || p_coarse.size() != COARSE_VOXELS || p_ray_direction.length_squared() < 1e-20f) {
		return 0;
	}
	const float fine_step = level_voxel_size(p_level);
	const float coarse_step = coarse_voxel_size(p_level);
	const Vector3 direction = p_ray_direction.normalized();
	Vector3 position = p_ray_origin;
	for (uint32_t step = 0; step < p_max_steps; step++) {
		const Vector3i voxel(
				int(Math::floor(position.x / coarse_step)) - _floor_div(p_origin.x, 4),
				int(Math::floor(position.y / coarse_step)) - _floor_div(p_origin.y, 4),
				int(Math::floor(position.z / coarse_step)) - _floor_div(p_origin.z, 4));
		if (voxel.x < 0 || voxel.y < 0 || voxel.z < 0 || voxel.x >= int(COARSE_SIZE) || voxel.y >= int(COARSE_SIZE) || voxel.z >= int(COARSE_SIZE)) {
			return MAX(step, 1u);
		}
		const uint32_t index = voxel.x + COARSE_SIZE * (voxel.y + COARSE_SIZE * voxel.z);
		const float distance = Math::abs(p_coarse[index]);
		if (distance <= fine_step) {
			return step + 1;
		}
		const float advance = CLAMP(distance - coarse_step * Math::sqrt(0.75f), fine_step, level_extent(p_level));
		position += direction * advance;
	}
	return p_max_steps;
}

NextGIGlobalSDF::NextGIGlobalSDF() {
	bricks.resize(MAX_LEVELS);
}

NextGIGlobalSDF::~NextGIGlobalSDF() {
}

uint32_t NextGIGlobalSDF::update_region(uint32_t p_level, const Vector3i &p_origin, const AABB &p_world_bounds, const Vector<Instance> &p_instances) {
	if (p_level >= MAX_LEVELS || p_world_bounds.size.x < 0.0f || p_world_bounds.size.y < 0.0f || p_world_bounds.size.z < 0.0f) {
		return 0;
	}
	const Vector3i min_voxel = world_to_voxel(p_level, p_origin, p_world_bounds.position);
	const Vector3i max_voxel = world_to_voxel(p_level, p_origin, p_world_bounds.position + p_world_bounds.size);
	const Vector3i min_brick(_floor_div(min_voxel.x, int(BRICK_SIZE)), _floor_div(min_voxel.y, int(BRICK_SIZE)), _floor_div(min_voxel.z, int(BRICK_SIZE)));
	const Vector3i max_brick(_floor_div(max_voxel.x, int(BRICK_SIZE)), _floor_div(max_voxel.y, int(BRICK_SIZE)), _floor_div(max_voxel.z, int(BRICK_SIZE)));
	uint32_t updated = 0;
	for (int z = MAX(min_brick.z, 0); z <= MIN(max_brick.z, int(PAGE_SIZE) - 1); z++) {
		for (int y = MAX(min_brick.y, 0); y <= MIN(max_brick.y, int(PAGE_SIZE) - 1); y++) {
			for (int x = MAX(min_brick.x, 0); x <= MIN(max_brick.x, int(PAGE_SIZE) - 1); x++) {
				const Vector3i brick(x, y, z);
				HashMap<uint64_t, LocalVector<float>> &level = bricks.write[p_level];
				LocalVector<float> &voxels = level[_brick_key(brick)];
				voxels.resize(BRICK_VOXELS);
				compose_brick(p_instances, p_level, p_origin, brick, voxels.ptr());
				updated++;
			}
		}
	}
	return updated;
}

float NextGIGlobalSDF::sample(uint32_t p_level, const Vector3i &p_origin, const Vector3 &p_world) const {
	if (p_level >= MAX_LEVELS) {
		return 1e30f;
	}
	const Vector3i voxel = world_to_voxel(p_level, p_origin, p_world);
	const Vector3i brick(_floor_div(voxel.x, int(BRICK_SIZE)), _floor_div(voxel.y, int(BRICK_SIZE)), _floor_div(voxel.z, int(BRICK_SIZE)));
	if (brick.x < 0 || brick.y < 0 || brick.z < 0 || brick.x >= int(PAGE_SIZE) || brick.y >= int(PAGE_SIZE) || brick.z >= int(PAGE_SIZE)) {
		return 1e30f;
	}
	const HashMap<uint64_t, LocalVector<float>> &level = bricks[p_level];
	const LocalVector<float> *voxels = level.getptr(_brick_key(brick));
	if (voxels == nullptr) {
		return 1e30f;
	}
	const int local_x = _floor_mod(voxel.x, int(BRICK_SIZE));
	const int local_y = _floor_mod(voxel.y, int(BRICK_SIZE));
	const int local_z = _floor_mod(voxel.z, int(BRICK_SIZE));
	return (*voxels)[local_x + local_y * int(BRICK_SIZE) + local_z * int(BRICK_SIZE * BRICK_SIZE)];
}

void NextGIGlobalSDF::clear() {
	for (HashMap<uint64_t, LocalVector<float>> &level : bricks) {
		level.clear();
	}
}

uint32_t NextGIGlobalSDF::get_brick_count(uint32_t p_level) const {
	return p_level < MAX_LEVELS ? bricks[p_level].size() : 0;
}

bool NextGIGlobalSDF::has_brick(uint32_t p_level, const Vector3i &p_brick) const {
	return p_level < MAX_LEVELS && bricks[p_level].has(_brick_key(p_brick));
}

const float *NextGIGlobalSDF::get_brick(uint32_t p_level, const Vector3i &p_brick) const {
	if (p_level >= MAX_LEVELS) {
		return nullptr;
	}
	const LocalVector<float> *voxels = bricks[p_level].getptr(_brick_key(p_brick));
	return voxels != nullptr ? voxels->ptr() : nullptr;
}
