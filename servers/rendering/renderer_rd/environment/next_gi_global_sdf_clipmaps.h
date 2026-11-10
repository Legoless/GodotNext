/**************************************************************************/
/*  next_gi_global_sdf_clipmaps.h                                         */
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

#include "core/math/aabb.h"
#include "core/math/vector3i.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/environment/next_gi_global_sdf.h"

class NextGIGlobalSDFGPU;

// GodotNext: ps_swrt_trace (#186, plan §21.2 L4)
//
// Keeps the clipmaps of one NextGIGlobalSDFGPU resident around the camera,
// for every instance the caller passes (no instance or brick cap):
// - A level is built in full the first time, and again after reset().
// - Afterwards a level rebuilds only the bricks a camera scroll exposes (the
//   rest of the page table is shifted, never rebuilt from scroll alone) and
//   the bricks the registry's dirty volumes touch.
// - Level i updates on every 2^i-th frame. Dirty volumes that arrive on a
//   frame a level skips wait for that level's next update.
// The SWRT tier owns one; the HWRT debug global SDF (NextGIRTScene) another.
class NextGIGlobalSDFClipmaps {
public:
	// One level's work for one frame.
	struct Plan {
		bool update = false; // False: the level waits for its frame.
		bool full = false; // Every brick of the level.
		Vector3i origin;
		Vector3i scroll; // Brick delta to shift the resident page table by first.
		LocalVector<Vector3i> bricks;
	};

	struct Stats {
		uint32_t levels_updated = 0;
		uint32_t bricks_rebuilt = 0;
		uint32_t full_builds = 0;
		bool allocation_failed = false; // The pool ran out of slots.
		uint32_t failed_level = UINT32_MAX;
	};

	// Registry dirty volumes are instance AABBs. A mesh SDF reaches further than
	// its mesh by its narrow band, and a brick's cap depends on the instances within
	// its clearance radius (NextGIGlobalSDF::brick_clearance()), so bricks are dirtied
	// this far around them.
	static float dirty_margin(uint32_t p_level);

	// CPU planning for one level. Consumes the level's pending dirty volumes when
	// the level updates. Does not touch a device.
	void plan_level(uint32_t p_level, const Vector3 &p_camera, uint64_t p_frame, Plan &r_plan);
	// Applies a plan's bookkeeping after the caller rebuilt it (or failed to).
	void commit_level(uint32_t p_level, const Plan &p_plan, bool p_built);

	void add_dirty_volumes(const AABB *p_volumes, uint32_t p_count);
	// Every level rebuilds in full on its next update.
	void reset();
	void invalidate_level(uint32_t p_level);

	// Plans and rebuilds the first p_levels levels into p_field, coarsest first: a pool that
	// runs out leaves a finer level partial, and the trace reads its missing bricks
	// (NextGIGlobalSDFGPU::PAGE_MISSING) from the complete level above.
	Stats update(NextGIGlobalSDFGPU &r_field, const Vector<NextGIGlobalSDF::Instance> &p_instances, const Vector3 &p_camera, uint64_t p_frame, uint32_t p_levels);

	// SWRT_REDUCED: frees the top level's slots and returns the new level count,
	// never below p_min_levels.
	static uint32_t drop_top_level(NextGIGlobalSDFGPU &r_field, NextGIGlobalSDFClipmaps &r_clipmaps, uint32_t p_levels, uint32_t p_min_levels);

	bool is_level_valid(uint32_t p_level) const { return p_level < NextGIGlobalSDF::MAX_LEVELS && levels[p_level].valid; }
	Vector3i get_origin(uint32_t p_level) const { return p_level < NextGIGlobalSDF::MAX_LEVELS ? levels[p_level].origin : Vector3i(); }
	uint32_t get_pending_dirty(uint32_t p_level) const { return p_level < NextGIGlobalSDF::MAX_LEVELS ? levels[p_level].pending.size() : 0; }
	// Full level builds committed so far (a still camera over a still scene adds none).
	uint64_t get_full_builds() const { return full_builds; }

private:
	struct Level {
		bool valid = false;
		Vector3i origin;
		LocalVector<AABB> pending;
	};
	Level levels[NextGIGlobalSDF::MAX_LEVELS];
	uint64_t full_builds = 0;
};
