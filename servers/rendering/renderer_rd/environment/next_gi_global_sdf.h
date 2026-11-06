/**************************************************************************/
/*  next_gi_global_sdf.h                                                  */
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
#include "core/math/mesh_sdf_builder.h"
#include "core/math/transform_3d.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"

// GodotNext: ps_global_sdf (#182, plan §21.2 L4, gate GS-6)
//
// The merged distance field SWRT traces. Six sparse 128^3 levels at 0.10 m *
// 2^level, each a 16^3 page table of 8^3 bricks, composited from the mesh SDFs
// instead of raster voxelization. This is the CPU layout/compositing reference
// and dirty-brick model; the renderer mirrors it into the R16F brick pool.
class NextGIGlobalSDF {
public:
	static constexpr uint32_t LEVEL_SIZE = 128; // Voxels per level axis.
	static constexpr uint32_t PAGE_SIZE = 16; // Bricks per level axis.
	static constexpr uint32_t BRICK_SIZE = 8; // Voxels per brick axis.
	static constexpr uint32_t BRICKS_PER_LEVEL = PAGE_SIZE * PAGE_SIZE * PAGE_SIZE;
	static constexpr uint32_t BRICK_VOXELS = BRICK_SIZE * BRICK_SIZE * BRICK_SIZE;
	static constexpr uint32_t BASE_LEVELS = 6; // Start count; more when the view distance grows.
	static constexpr uint32_t MAX_LEVELS = 10;
	static constexpr float BASE_VOXEL_SIZE = 0.1f;
	static constexpr uint32_t COARSE_SIZE = 32;

	// One mesh instance contributing to the field. The mesh SDF is in mesh-local
	// space; the world position is transformed into it before sampling.
	struct Instance {
		Transform3D transform;
		const MeshSDFData *field = nullptr;
		bool dynamic = false;
		bool two_sided = false;
	};

	// Layout.
	static uint32_t level_count(float p_view_distance);
	static float level_voxel_size(uint32_t p_level);
	static float level_extent(uint32_t p_level);
	// The level whose extent covers p_distance (0 at the camera).
	static uint32_t level_for_distance(float p_distance, uint32_t p_levels);
	// Camera-scrolled level origin in voxels, snapped to the brick size.
	static Vector3i scrolled_origin(const Vector3 &p_camera, uint32_t p_level);
	// Level i updates every 2^i frames (plan §21.2 step 3).
	static bool level_updates_on_frame(uint32_t p_level, uint64_t p_frame);
	// The brick-space shift from the old level origin to the new one.
	static Vector3i scroll_brick_delta(const Vector3i &p_old_origin, const Vector3i &p_new_origin);
	// The new page-table bricks on the leading edges of a scroll; the rest is
	// reused by shifting. Returns the number written to r_bricks.
	static uint32_t scrolled_edge_bricks(const Vector3i &p_delta_bricks, Vector3i *r_bricks, uint32_t p_max_bricks);
	static Vector3i world_to_voxel(uint32_t p_level, const Vector3i &p_origin, const Vector3 &p_world);
	static Vector3 voxel_center(uint32_t p_level, const Vector3i &p_origin, const Vector3i &p_voxel);

	// CPU compositing.
	// The minimum of the mesh SDF samples over the instances.
	static float reference_distance(const Vector<Instance> &p_instances, const Vector3 &p_world);
	// Composes one brick's voxel centers, in brick-local voxel order.
	static void compose_brick(const Vector<Instance> &p_instances, uint32_t p_level, const Vector3i &p_origin, const Vector3i &p_brick, float *r_voxels);
	// The level-0 bricks an instance's old and new AABBs touch; the caller
	// recomposes only these (dirty bricks).
	static void dirty_bricks(const AABB &p_old_world, const AABB &p_new_world, const Vector3i &p_origin, Vector3i *r_bricks, uint32_t p_max_bricks, uint32_t &r_count);

	NextGIGlobalSDF();
	~NextGIGlobalSDF();

	static constexpr uint32_t COARSE_VOXELS = COARSE_SIZE * COARSE_SIZE * COARSE_SIZE;
	// 1/4-resolution coarse level: resample the field at the coarse centres,
	// then relax with 4 forward/backward neighbour sweeps (plan §21.2 step 4).
	static void build_coarse(const Vector<Instance> &p_instances, uint32_t p_level, const Vector3i &p_origin, uint32_t p_iterations, LocalVector<float> &r_coarse);
	static float coarse_voxel_size(uint32_t p_level) { return 4.0f * level_voxel_size(p_level); }

	// Rebuilds the bricks of one level overlapping p_world_bounds and returns
	// how many were written. Bricks outside the level are skipped.
	uint32_t update_region(uint32_t p_level, const Vector3i &p_origin, const AABB &p_world_bounds, const Vector<Instance> &p_instances);
	// Nearest-voxel sample; an absent brick reads as +infinity (far outside).
	float sample(uint32_t p_level, const Vector3i &p_origin, const Vector3 &p_world) const;
	void clear();

	uint32_t get_brick_count(uint32_t p_level) const;
	bool has_brick(uint32_t p_level, const Vector3i &p_brick) const;
	const float *get_brick(uint32_t p_level, const Vector3i &p_brick) const;

private:
	static uint64_t _brick_key(const Vector3i &p_brick);
	// p_level -> packed brick coordinate -> BRICK_VOXELS distances.
	Vector<HashMap<uint64_t, LocalVector<float>>> bricks;
};
