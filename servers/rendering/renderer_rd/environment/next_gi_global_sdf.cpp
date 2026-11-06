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
	const float extent = level_extent(0);
	if (p_distance <= extent) {
		return 0;
	}
	const uint32_t level = uint32_t(Math::ceil(Math::log2(p_distance / extent)));
	return MIN(level, p_levels - 1);
}

Vector3i NextGIGlobalSDF::scrolled_origin(const Vector3 &p_camera, uint32_t p_level) {
	const float voxel_size = level_voxel_size(p_level);
	const Vector3i voxel(
			int(Math::floor(p_camera.x / voxel_size)),
			int(Math::floor(p_camera.y / voxel_size)),
			int(Math::floor(p_camera.z / voxel_size)));
	return Vector3i(
			_floor_div(voxel.x, int(BRICK_SIZE)) * int(BRICK_SIZE),
			_floor_div(voxel.y, int(BRICK_SIZE)) * int(BRICK_SIZE),
			_floor_div(voxel.z, int(BRICK_SIZE)) * int(BRICK_SIZE));
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

float NextGIGlobalSDF::reference_distance(const Vector<Instance> &p_instances, const Vector3 &p_world) {
	float distance = 1e30f;
	for (const Instance &instance : p_instances) {
		if (instance.field == nullptr || !instance.field->is_valid()) {
			continue;
		}
		const Vector3 local = instance.transform.affine_inverse().xform(p_world);
		const Vector3 scale = instance.transform.basis.get_scale();
		// The mesh SDF is in local metres. A non-uniform scale is bounded by
		// the largest axis, which keeps the field conservative.
		const float world_scale = MAX(scale.x, MAX(scale.y, scale.z));
		distance = MIN(distance, instance.field->sample_distance(local) * world_scale);
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

void NextGIGlobalSDF::dirty_bricks(const AABB &p_old_world, const AABB &p_new_world, const Vector3i &p_origin, Vector3i *r_bricks, uint32_t p_max_bricks, uint32_t &r_count) {
	r_count = 0;
	const float brick_world = level_voxel_size(0) * float(BRICK_SIZE);
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
