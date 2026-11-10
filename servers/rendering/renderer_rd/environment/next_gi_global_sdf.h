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
#include "core/math/vector4.h"
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
	// Camera-centred level origin in voxels, snapped to the brick size.
	static Vector3i scrolled_origin(const Vector3 &p_camera, uint32_t p_level);
	static float sample_instance(const Instance &p_instance, const Vector3 &p_world);
	// GodotNext: ps_swrt_trace (#186). The CPU twin of nextgi_mesh_sdf_distance_inc.glsl,
	// which the composite and the SWRT detail trace share (the comments there give the
	// math). sample_instance() is mesh_world_distance() of the mip-0 sample.
	// xyz: the world distance between the face planes of each local axis per local unit (the
	// column lengths of an orthogonal basis). w: the smallest singular value, negative when
	// the basis is sheared (a rotated node under a non-uniformly scaled parent).
	static Vector4 instance_axis_scale(const Basis &p_basis);
	// The mesh's own bounds: the SDF bounds less the mip-0 band the builder pads them by.
	static AABB mesh_surface_bounds(const MeshSDFData &p_field);
	// A lower bound of the world distance from a local sample (p_sampled at p_local,
	// clamped to p_bounds) of an instance with p_axis_scale.
	static float mesh_world_distance(const Vector3 &p_local, const AABB &p_bounds, const AABB &p_surface, float p_sampled, const Vector4 &p_axis_scale);
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
	// The instances whose mesh-SDF band can reach a level brick. The caller
	// min-composites only these into the brick; dynamic and two-sided instances
	// keep their flags for the static/dynamic split and the coverage bit.
	static uint32_t cull_instances_for_brick(const Vector<Instance> &p_instances, uint32_t p_level, const Vector3i &p_origin, const Vector3i &p_brick, uint32_t *r_indices, uint32_t p_max_indices);
	// GodotNext: ps_swrt_trace (#186). The cull box of a brick: the brick grown by one
	// voxel. cull_instances_for_brick() keeps the instances whose world bounds overlap it.
	static AABB brick_cull_box(uint32_t p_level, const Vector3i &p_origin, const Vector3i &p_brick);
	// Every instance a brick does not list lies outside its cull box, at least
	// brick_clearance() from it. A segment from a point of the box to such an instance
	// leaves the box first, so the point's distance to the box's exterior plus the clearance
	// bounds their distance. The composite caps each voxel with it (nextgi_global_sdf.glsl):
	// the minimum over the listed instances alone overestimates next to an unlisted one,
	// and rays stepped through it. Without the clearance the cap held every voxel to
	// 1.5-4.5 voxels, and rays near large surfaces ran out of trace steps.
	static float brick_cap(const AABB &p_cull_box, float p_clearance, const Vector3 &p_world);
	// How far each level's brick_clearance() searches: CLEARANCE_BRICKS bricks around the
	// brick, less the cull box's own voxel. Dirty volumes reach this much farther
	// (NextGIGlobalSDFClipmaps::dirty_margin()), so a moved instance updates every brick
	// whose clearance it shrinks.
	static constexpr uint32_t CLEARANCE_BRICKS = 1;
	static float clearance_radius(uint32_t p_level);
	// What brick_clearance() searches instead of every instance, for one level and origin:
	// the bins of bin_instances() for the same instances, each instance's world bounds, and
	// the instances that reach out of the level within clearance_radius() of it (the bins
	// hold only their parts inside the level).
	struct ClearanceIndex {
		const LocalVector<uint32_t> *offsets = nullptr;
		const LocalVector<uint32_t> *indices = nullptr;
		LocalVector<AABB> bounds;
		LocalVector<uint32_t> outside;
	};
	static void build_clearance_index(const Vector<Instance> &p_instances, uint32_t p_level, const Vector3i &p_origin, const LocalVector<uint32_t> &p_offsets, const LocalVector<uint32_t> &p_indices, ClearanceIndex &r_index);
	// The world gap from the brick's cull box to the nearest instance it does not list, at
	// most clearance_radius(). It depends on the brick's place in the world, not in the
	// level, so a brick a scroll keeps (shifted, never rebuilt) stays exact. With p_index
	// only the bins of the bricks within CLEARANCE_BRICKS and the instances that reach out
	// of the level are searched; every instance nearer than the limit is among them, so
	// the result is the same.
	static float brick_clearance(const Vector<Instance> &p_instances, uint32_t p_level, const Vector3i &p_origin, const Vector3i &p_brick, const ClearanceIndex *p_index = nullptr);
	// One composite voxel on the CPU: the culled instances' minimum, capped by brick_cap().
	static float composite_distance(const Vector<Instance> &p_instances, const uint32_t *p_culled, uint32_t p_culled_count, const AABB &p_cull_box, float p_clearance, const Vector3 &p_world);
	// GodotNext: ps_swrt_trace (#186). The same test for every brick of a level at
	// once: r_offsets has BRICKS_PER_LEVEL + 1 entries and brick b's instances are
	// r_indices[r_offsets[b] .. r_offsets[b + 1]), in instance order. A full level
	// costs one pass over the instances instead of one per brick.
	static void bin_instances(const Vector<Instance> &p_instances, uint32_t p_level, const Vector3i &p_origin, LocalVector<uint32_t> &r_offsets, LocalVector<uint32_t> &r_indices);

	// Static/dynamic split and the two-sided coverage bit, the layout the
	// compositing compute writes: static bricks hold only static instances,
	// dynamic bricks only dynamic ones and composite over static at lookup;
	// a voxel whose winning sample is two-sided sets its coverage bit.
	// GodotNext: ps_swrt_trace (#186). The static distance carries the brick_cap() of the
	// voxel center (with the brick's brick_clearance()), so min(static, dynamic) never
	// exceeds it.
	struct BrickSplit {
		LocalVector<float> static_distance;
		LocalVector<float> dynamic_distance;
		uint8_t coverage[BRICK_VOXELS / 8] = {};
	};
	static void compose_brick_split(const Vector<Instance> &p_instances, const uint32_t *p_culled, uint32_t p_culled_count, uint32_t p_level, const Vector3i &p_origin, const Vector3i &p_brick, BrickSplit &r_split);
	static bool get_coverage_bit(const BrickSplit &p_split, const Vector3i &p_voxel);
	static void set_coverage_bit(BrickSplit &r_split, const Vector3i &p_voxel, bool p_value);

	// The bricks an instance's old and new AABBs touch at p_level.
	static void dirty_bricks(const AABB &p_old_world, const AABB &p_new_world, const Vector3i &p_origin, Vector3i *r_bricks, uint32_t p_max_bricks, uint32_t &r_count, uint32_t p_level = 0);

	NextGIGlobalSDF();
	~NextGIGlobalSDF();

	static constexpr uint32_t COARSE_VOXELS = COARSE_SIZE * COARSE_SIZE * COARSE_SIZE;
	// 1/4-resolution coarse level: resample the field at the coarse centers,
	// then relax with 4 forward/backward neighbor sweeps (plan §21.2 step 4).
	static void build_coarse(const Vector<Instance> &p_instances, uint32_t p_level, const Vector3i &p_origin, uint32_t p_iterations, LocalVector<float> &r_coarse);
	static float coarse_voxel_size(uint32_t p_level) { return 4.0f * level_voxel_size(p_level); }
	static uint32_t trace_coarse_steps(const LocalVector<float> &p_coarse, uint32_t p_level, const Vector3i &p_origin, const Vector3 &p_ray_origin, const Vector3 &p_ray_direction, uint32_t p_max_steps = 256);

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
