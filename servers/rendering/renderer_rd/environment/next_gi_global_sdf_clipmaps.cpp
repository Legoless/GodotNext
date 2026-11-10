/**************************************************************************/
/*  next_gi_global_sdf_clipmaps.cpp                                       */
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

#include "next_gi_global_sdf_clipmaps.h"

#include "core/math/math_funcs.h"
#include "servers/rendering/renderer_rd/environment/next_gi_global_sdf_gpu.h"

// GodotNext: ps_swrt_trace (#186)

namespace {

int _floor_div_brick(int p_value) {
	const int size = int(NextGIGlobalSDF::BRICK_SIZE);
	int quotient = p_value / size;
	if ((p_value % size != 0) && ((p_value < 0) != (size < 0))) {
		quotient--;
	}
	return quotient;
}

} // namespace

float NextGIGlobalSDFClipmaps::dirty_margin(uint32_t p_level) {
	// The mesh SDF builder pads its bounds by its narrow band (4 voxels at
	// 10 voxels/m, more for large meshes). Two level voxels cover the brick
	// cull's own one-voxel growth.
	return MAX(2.0f * NextGIGlobalSDF::level_voxel_size(p_level), 1.0f);
}

void NextGIGlobalSDFClipmaps::plan_level(uint32_t p_level, const Vector3 &p_camera, uint64_t p_frame, Plan &r_plan) {
	r_plan.update = false;
	r_plan.full = false;
	r_plan.scroll = Vector3i();
	r_plan.bricks.clear();
	ERR_FAIL_UNSIGNED_INDEX(p_level, NextGIGlobalSDF::MAX_LEVELS);
	Level &level = levels[p_level];
	r_plan.origin = NextGIGlobalSDF::scrolled_origin(p_camera, p_level);
	if (!level.valid) {
		r_plan.update = true;
		r_plan.full = true;
		r_plan.bricks.resize(NextGIGlobalSDF::BRICKS_PER_LEVEL);
		uint32_t count = 0;
		for (int z = 0; z < int(NextGIGlobalSDF::PAGE_SIZE); z++) {
			for (int y = 0; y < int(NextGIGlobalSDF::PAGE_SIZE); y++) {
				for (int x = 0; x < int(NextGIGlobalSDF::PAGE_SIZE); x++) {
					r_plan.bricks[count++] = Vector3i(x, y, z);
				}
			}
		}
		return;
	}
	if (!NextGIGlobalSDF::level_updates_on_frame(p_level, p_frame)) {
		return;
	}
	r_plan.update = true;
	uint8_t marked[NextGIGlobalSDF::BRICKS_PER_LEVEL] = {};
	auto mark = [&](const Vector3i &p_brick) {
		const uint32_t index = uint32_t(p_brick.x) + NextGIGlobalSDF::PAGE_SIZE * (uint32_t(p_brick.y) + NextGIGlobalSDF::PAGE_SIZE * uint32_t(p_brick.z));
		if (marked[index] == 0) {
			marked[index] = 1;
			r_plan.bricks.push_back(p_brick);
		}
	};
	if (r_plan.origin != level.origin) {
		r_plan.scroll = NextGIGlobalSDF::scroll_brick_delta(level.origin, r_plan.origin);
		LocalVector<Vector3i> edges;
		edges.resize(NextGIGlobalSDF::BRICKS_PER_LEVEL);
		const uint32_t edge_count = NextGIGlobalSDF::scrolled_edge_bricks(r_plan.scroll, edges.ptr(), NextGIGlobalSDF::BRICKS_PER_LEVEL);
		for (uint32_t index = 0; index < edge_count; index++) {
			mark(edges[index]);
		}
	}
	const float brick_world = NextGIGlobalSDF::level_voxel_size(p_level) * float(NextGIGlobalSDF::BRICK_SIZE);
	const float margin = dirty_margin(p_level);
	const Vector3i origin_brick(_floor_div_brick(r_plan.origin.x), _floor_div_brick(r_plan.origin.y), _floor_div_brick(r_plan.origin.z));
	const int page = int(NextGIGlobalSDF::PAGE_SIZE);
	for (const AABB &volume : level.pending) {
		if (volume.size.x < 0.0f || volume.size.y < 0.0f || volume.size.z < 0.0f) {
			continue;
		}
		const AABB grown = volume.grow(margin);
		Vector3i lo;
		Vector3i hi;
		for (int axis = 0; axis < 3; axis++) {
			lo[axis] = CLAMP(int(Math::floor(grown.position[axis] / brick_world)) - origin_brick[axis], 0, page);
			hi[axis] = CLAMP(int(Math::floor((grown.position[axis] + grown.size[axis]) / brick_world)) - origin_brick[axis], -1, page - 1);
		}
		for (int z = lo.z; z <= hi.z; z++) {
			for (int y = lo.y; y <= hi.y; y++) {
				for (int x = lo.x; x <= hi.x; x++) {
					mark(Vector3i(x, y, z));
				}
			}
		}
	}
}

void NextGIGlobalSDFClipmaps::commit_level(uint32_t p_level, const Plan &p_plan, bool p_built) {
	ERR_FAIL_UNSIGNED_INDEX(p_level, NextGIGlobalSDF::MAX_LEVELS);
	if (!p_plan.update) {
		return;
	}
	Level &level = levels[p_level];
	level.origin = p_plan.origin;
	level.pending.clear();
	// A level whose rebuild failed starts over; its scroll was already applied.
	level.valid = p_built;
	full_builds += p_plan.full ? 1 : 0;
}

void NextGIGlobalSDFClipmaps::add_dirty_volumes(const AABB *p_volumes, uint32_t p_count) {
	if (p_volumes == nullptr || p_count == 0) {
		return;
	}
	for (uint32_t level = 0; level < NextGIGlobalSDF::MAX_LEVELS; level++) {
		if (!levels[level].valid) {
			continue; // A full build picks the change up anyway.
		}
		for (uint32_t index = 0; index < p_count; index++) {
			levels[level].pending.push_back(p_volumes[index]);
		}
	}
}

void NextGIGlobalSDFClipmaps::reset() {
	for (Level &level : levels) {
		level.valid = false;
		level.pending.clear();
	}
}

void NextGIGlobalSDFClipmaps::invalidate_level(uint32_t p_level) {
	ERR_FAIL_UNSIGNED_INDEX(p_level, NextGIGlobalSDF::MAX_LEVELS);
	levels[p_level].valid = false;
	levels[p_level].pending.clear();
}

NextGIGlobalSDFClipmaps::Stats NextGIGlobalSDFClipmaps::update(NextGIGlobalSDFGPU &r_field, const Vector<NextGIGlobalSDF::Instance> &p_instances, const Vector3 &p_camera, uint64_t p_frame, uint32_t p_levels) {
	Stats stats;
	const uint32_t count = MIN(p_levels, NextGIGlobalSDF::MAX_LEVELS);
	Plan plan;
	for (uint32_t level = 0; level < count; level++) {
		plan_level(level, p_camera, p_frame, plan);
		if (!plan.update) {
			continue;
		}
		if (plan.full) {
			r_field.release_level(level);
			stats.full_builds++;
		} else if (plan.scroll != Vector3i()) {
			r_field.scroll_level(level, plan.scroll);
		}
		const bool built = r_field.update_bricks(p_instances, level, plan.origin, plan.bricks.ptr(), plan.bricks.size());
		// A level that ran out of pool slots stays resident with the bricks it got. The
		// caller either frees slots (drops a level) and rebuilds it, or keeps it partial at
		// its level floor; a full rebuild on every update would fail the same way.
		const bool out_of_slots = !built && r_field.is_allocation_failed();
		commit_level(level, plan, built || out_of_slots);
		stats.levels_updated++;
		stats.bricks_rebuilt += plan.bricks.size();
		if (out_of_slots) {
			stats.allocation_failed = true;
			stats.failed_level = level;
			break;
		}
	}
	return stats;
}

uint32_t NextGIGlobalSDFClipmaps::drop_top_level(NextGIGlobalSDFGPU &r_field, NextGIGlobalSDFClipmaps &r_clipmaps, uint32_t p_levels, uint32_t p_min_levels) {
	if (p_levels <= p_min_levels || p_levels == 0) {
		return p_levels;
	}
	const uint32_t top = p_levels - 1;
	r_field.release_level(top);
	r_clipmaps.invalidate_level(top);
	return top;
}
