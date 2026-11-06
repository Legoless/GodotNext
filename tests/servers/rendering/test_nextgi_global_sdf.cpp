/**************************************************************************/
/*  test_nextgi_global_sdf.cpp                                            */
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

TEST_FORCE_LINK(test_nextgi_global_sdf)

#include "core/math/mesh_sdf_builder.h"
#include "core/math/random_number_generator.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "servers/rendering/renderer_rd/environment/next_gi_global_sdf.h"
#include "servers/rendering/renderer_rd/environment/next_gi_global_sdf_gpu.h"

namespace {

static MeshSDFData _wall_sdf() {
	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size(Vector3(0.2f, 4.0f, 4.0f));
	MeshSDFSettings settings;
	settings.voxels_per_meter = 20.0f;
	settings.max_voxels_per_axis = 128;
	MeshSDFData data;
	const Error err = MeshSDFBuilder::build(box->get_faces(), settings, data);
	CHECK(err == OK);
	CHECK(data.is_valid());
	return data;
}

} // namespace

TEST_CASE("[NextGI][GlobalSDF] Level layout and camera scroll") {
	using SDF = NextGIGlobalSDF;
	CHECK(Math::is_equal_approx(SDF::level_voxel_size(0), 0.1f));
	CHECK(Math::is_equal_approx(SDF::level_voxel_size(5), 3.2f));
	CHECK(Math::is_equal_approx(SDF::level_extent(0), 12.8f));
	// Six start levels reach the default 200 m scene view distance.
	CHECK(SDF::level_extent(SDF::BASE_LEVELS - 1) >= 200.0f);
	CHECK_EQ(SDF::level_count(200.0f), SDF::BASE_LEVELS);
	// Raising the view distance adds levels, up to the cap.
	CHECK(SDF::level_count(800.0f) > SDF::BASE_LEVELS);
	CHECK(SDF::level_count(1e9f) <= SDF::MAX_LEVELS);

	CHECK_EQ(SDF::level_for_distance(5.0f, 6), uint32_t(0));
	CHECK_EQ(SDF::level_for_distance(20.0f, 6), uint32_t(1));
	CHECK_EQ(SDF::level_for_distance(200.0f, 6), uint32_t(4));
	CHECK_EQ(SDF::level_for_distance(1e9f, 6), uint32_t(5));

	// The origin snaps down to a brick boundary, also for negative cameras.
	const Vector3i origin = SDF::scrolled_origin(Vector3(-0.35f, 1.37f, 0.0f), 0);
	CHECK_EQ(origin, Vector3i(-8, 8, 0));
	CHECK_EQ(origin.x % int(SDF::BRICK_SIZE), 0);
	CHECK_EQ(origin.y % int(SDF::BRICK_SIZE), 0);

	// World -> voxel -> centre round-trips within one voxel.
	const Vector3i voxel = SDF::world_to_voxel(1, origin, Vector3(1.0f, 2.0f, 3.0f));
	const Vector3 centre = SDF::voxel_center(1, origin, voxel);
	CHECK(centre.distance_to(Vector3(1.0f, 2.0f, 3.0f)) <= SDF::level_voxel_size(1));
}

TEST_CASE("[NextGI][GlobalSDF] Brick compositing matches the brute-force field") {
	const MeshSDFData wall = _wall_sdf();
	Vector<NextGIGlobalSDF::Instance> instances;
	NextGIGlobalSDF::Instance instance;
	instance.transform = Transform3D();
	instance.field = &wall;
	instances.push_back(instance);

	NextGIGlobalSDF sdf;
	// Centre the level so the sampled region sits inside its 16^3 page table.
	const Vector3i origin = NextGIGlobalSDF::scrolled_origin(Vector3(-1.0f, -1.0f, -1.0f), 0);
	const AABB bounds(Vector3(-1.0f, -1.0f, -1.0f), Vector3(2.0f, 2.0f, 2.0f));
	const uint32_t bricks = sdf.update_region(0, origin, bounds, instances);
	CHECK(bricks > 0);
	CHECK_EQ(sdf.get_brick_count(0), bricks);

	RandomNumberGenerator rng;
	rng.set_seed(424242);
	float max_error = 0.0f;
	uint32_t compared = 0;
	uint32_t sign_mismatches = 0;
	for (int index = 0; index < 10000; index++) {
		const Vector3 point(rng.randf_range(-0.5f, 0.5f), rng.randf_range(-0.5f, 0.5f), rng.randf_range(-0.5f, 0.5f));
		const float reference = NextGIGlobalSDF::reference_distance(instances, point);
		const float sampled = sdf.sample(0, origin, point);
		REQUIRE(sampled < 1e29f);
		max_error = MAX(max_error, Math::abs(sampled - reference));
		compared++;
		// More than one voxel from the surface the sign must stay watertight.
		if (Math::abs(reference) > 0.15f && ((sampled < 0.0f) != (reference < 0.0f))) {
			sign_mismatches++;
		}
	}
	CHECK_EQ(compared, uint32_t(10000));
	CHECK(sign_mismatches == 0);
	// Nearest-voxel sampling costs up to half a voxel; stay within one.
	CHECK(max_error <= NextGIGlobalSDF::level_voxel_size(0));

	// A point deep inside the 20 cm wall is negative, outside is positive.
	CHECK(sdf.has_brick(0, Vector3i(1, 1, 1)));
	CHECK(sdf.sample(0, origin, Vector3(0.0f, 0.0f, 0.0f)) < 0.0f);
	CHECK(sdf.sample(0, origin, Vector3(0.5f, 0.0f, 0.0f)) > 0.0f);
}

TEST_CASE("[NextGI][GlobalSDF] Dirty bricks cover the old and new bounds") {
	Vector3i bricks[4096];
	uint32_t count = 0;
	const Vector3i origin = NextGIGlobalSDF::scrolled_origin(Vector3(), 0);
	const AABB old_bounds(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.2f, 0.2f, 0.2f));
	const AABB new_bounds(Vector3(0.8f, 0.0f, 0.0f), Vector3(0.2f, 0.2f, 0.2f));
	NextGIGlobalSDF::dirty_bricks(old_bounds, new_bounds, origin, bricks, 4096, count);
	CHECK(count >= 2);
	bool covers_old = false;
	bool covers_new = false;
	for (uint32_t index = 0; index < count; index++) {
		const Vector3i brick = bricks[index];
		const AABB brick_world(
				Vector3(brick.x * 8 * 0.1f, brick.y * 8 * 0.1f, brick.z * 8 * 0.1f),
				Vector3(0.8f, 0.8f, 0.8f));
		covers_old = covers_old || brick_world.intersects(old_bounds);
		covers_new = covers_new || brick_world.intersects(new_bounds);
	}
	CHECK(covers_old);
	CHECK(covers_new);
}

TEST_CASE("[NextGI][GlobalSDF] GPU brick pool and page tables") {
	NextGIGlobalSDFGPU gpu(3);
	CHECK_EQ(gpu.get_capacity(), uint32_t(3));
	CHECK_EQ(gpu.get_used_bricks(), uint32_t(0));
	CHECK_EQ(gpu.get_page_table(0).size(), int(NextGIGlobalSDF::BRICKS_PER_LEVEL));

	float voxels[NextGIGlobalSDF::BRICK_VOXELS];
	for (uint32_t index = 0; index < NextGIGlobalSDF::BRICK_VOXELS; index++) {
		voxels[index] = float(index) / 512.0f;
	}
	const Vector3i brick_a(1, 2, 3);
	const Vector3i brick_b(2, 2, 3);
	REQUIRE(gpu.upload_brick(0, brick_a, voxels));
	CHECK(gpu.has_brick(0, brick_a));
	const int32_t slot_a = gpu.get_slot(0, brick_a);
	CHECK(slot_a >= 0);
	CHECK(gpu.has_brick(0, brick_a)); // The page table entry points at the slot.
	CHECK(gpu.get_page_table(0)[1 + 2 * 16 + 3 * 256] == slot_a);

	// Half-float round-trip for every voxel.
	for (int z = 0; z < 8; z++) {
		for (int y = 0; y < 8; y++) {
			for (int x = 0; x < 8; x++) {
				const uint32_t index = uint32_t(x + y * 8 + z * 64);
				CHECK(Math::is_equal_approx(gpu.get_voxel(0, brick_a, Vector3i(x, y, z)), float(index) / 512.0f));
			}
		}
	}

	REQUIRE(gpu.upload_brick(0, brick_b, voxels));
	CHECK(gpu.get_slot(0, brick_b) != slot_a);
	CHECK_EQ(gpu.get_used_bricks(), uint32_t(2));

	// The same brick coordinate in another level gets its own slot.
	REQUIRE(gpu.upload_brick(1, brick_a, voxels));
	CHECK(gpu.get_slot(1, brick_a) != slot_a);
	CHECK_EQ(gpu.get_used_bricks(), uint32_t(3));

	// Re-uploading an existing brick keeps its slot.
	REQUIRE(gpu.upload_brick(0, brick_a, voxels));
	CHECK_EQ(gpu.get_slot(0, brick_a), slot_a);
	CHECK_EQ(gpu.get_used_bricks(), uint32_t(3));

	// The fourth allocation exhausts the pool.
	CHECK_FALSE(gpu.upload_brick(0, Vector3i(3, 2, 3), voxels));
	CHECK_EQ(gpu.get_used_bricks(), uint32_t(3));

	gpu.release_level(0);
	CHECK_FALSE(gpu.has_brick(0, brick_a));
	CHECK_FALSE(gpu.has_brick(0, brick_b));
	CHECK(gpu.has_brick(1, brick_a));
	CHECK_EQ(gpu.get_used_bricks(), uint32_t(1));
	CHECK(gpu.get_page_table(0)[1 + 2 * 16 + 3 * 256] == NextGIGlobalSDFGPU::PAGE_UNALLOCATED);

	// Released slots are reused.
	REQUIRE(gpu.upload_brick(0, brick_b, voxels));
	CHECK(gpu.has_brick(0, brick_b));
	CHECK_EQ(gpu.get_used_bricks(), uint32_t(2));

	// Out-of-page-table bricks are rejected.
	CHECK_FALSE(gpu.upload_brick(0, Vector3i(-1, 0, 0), voxels));
	CHECK_FALSE(gpu.upload_brick(0, Vector3i(16, 0, 0), voxels));
	CHECK_FALSE(gpu.upload_brick(NextGIGlobalSDF::MAX_LEVELS, brick_a, voxels));
	CHECK_EQ(gpu.get_slot(0, Vector3i(-1, 0, 0)), NextGIGlobalSDFGPU::PAGE_UNALLOCATED);

	gpu.free_all();
	CHECK_EQ(gpu.get_used_bricks(), uint32_t(0));
}

TEST_CASE("[NextGI][GlobalSDF] Level cadence and scroll reuse") {
	using SDF = NextGIGlobalSDF;
	// Level i rebuilds every 2^i frames.
	for (uint32_t level = 0; level < 4; level++) {
		const uint64_t interval = uint64_t(1) << level;
		for (uint64_t frame = 0; frame < 32; frame++) {
			CHECK_EQ(SDF::level_updates_on_frame(level, frame), (frame % interval) == 0);
		}
	}
	CHECK_FALSE(SDF::level_updates_on_frame(SDF::MAX_LEVELS, 0));

	// One brick of scroll on +x rebuilds exactly one page-table slab.
	const Vector3i delta(1, 0, 0);
	CHECK_EQ(SDF::scroll_brick_delta(Vector3i(), Vector3i(8, 0, 0)), delta);
	CHECK_EQ(SDF::scroll_brick_delta(Vector3i(8, 0, 0), Vector3i()), Vector3i(-1, 0, 0));
	Vector3i bricks[4096];
	uint32_t count = SDF::scrolled_edge_bricks(delta, bricks, 4096);
	CHECK_EQ(count, uint32_t(16 * 16));
	for (uint32_t index = 0; index < count; index++) {
		CHECK_EQ(bricks[index].x, int(SDF::PAGE_SIZE) - 1);
	}

	// Two bricks in x and one in y: x slabs plus the remaining y row.
	count = SDF::scrolled_edge_bricks(Vector3i(2, 1, 0), bricks, 4096);
	CHECK_EQ(count, uint32_t(2 * 16 * 16 + 14 * 16));

	// A full-page scroll rebuilds everything, and a zero scroll nothing.
	count = SDF::scrolled_edge_bricks(Vector3i(int(SDF::PAGE_SIZE), 0, 0), bricks, 4096);
	CHECK_EQ(count, SDF::BRICKS_PER_LEVEL);
	CHECK_EQ(SDF::scrolled_edge_bricks(Vector3i(), bricks, 4096), uint32_t(0));
}

TEST_CASE("[NextGI][GlobalSDF] Coarse level resample and propagation") {
	const MeshSDFData wall = _wall_sdf();
	Vector<NextGIGlobalSDF::Instance> instances;
	NextGIGlobalSDF::Instance instance;
	instance.field = &wall;
	instances.push_back(instance);

	CHECK_EQ(NextGIGlobalSDF::COARSE_VOXELS, uint32_t(32 * 32 * 32));
	CHECK(Math::is_equal_approx(NextGIGlobalSDF::coarse_voxel_size(0), 0.4f));

	const Vector3i origin = NextGIGlobalSDF::scrolled_origin(Vector3(-1.0f, -1.0f, -1.0f), 0);
	LocalVector<float> raw;
	LocalVector<float> relaxed;
	NextGIGlobalSDF::build_coarse(instances, 0, origin, 0, raw);
	NextGIGlobalSDF::build_coarse(instances, 0, origin, 4, relaxed);
	CHECK_EQ(raw.size(), NextGIGlobalSDF::COARSE_VOXELS);
	REQUIRE_EQ(relaxed.size(), NextGIGlobalSDF::COARSE_VOXELS);

	// The raw resample matches the brute-force field at the coarse centres.
	float max_error = 0.0f;
	for (uint32_t z = 0; z < 32; z++) {
		for (uint32_t y = 0; y < 32; y++) {
			for (uint32_t x = 0; x < 32; x++) {
				const Vector3 world(
						float(origin.x + int(x) * 4) * 0.1f + 0.2f,
						float(origin.y + int(y) * 4) * 0.1f + 0.2f,
						float(origin.z + int(z) * 4) * 0.1f + 0.2f);
				const uint32_t index = x + y * 32 + z * 32 * 32;
				max_error = MAX(max_error, Math::abs(raw[index] - NextGIGlobalSDF::reference_distance(instances, world)));
			}
		}
	}
	CHECK(max_error <= NextGIGlobalSDF::coarse_voxel_size(0));

	// Propagation only lowers values and fills the whole level.
	for (uint32_t index = 0; index < NextGIGlobalSDF::COARSE_VOXELS; index++) {
		CHECK(relaxed[index] <= raw[index] + 0.0001f);
		CHECK(relaxed[index] < 1e29f);
	}
}
