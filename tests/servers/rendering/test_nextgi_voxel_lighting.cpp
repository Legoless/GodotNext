/**************************************************************************/
/*  test_nextgi_voxel_lighting.cpp                                        */
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

TEST_FORCE_LINK(test_nextgi_voxel_lighting)

#include "core/math/mesh_sdf_builder.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "servers/rendering/renderer_rd/environment/next_gi_global_sdf.h"
#include "servers/rendering/renderer_rd/environment/next_gi_src.h"
#include "servers/rendering/renderer_rd/environment/next_gi_voxel_lighting.h"

#include <cmath>

namespace {

static MeshSDFData _box_sdf(const Vector3 &p_size, float p_voxels_per_meter, int p_max_voxels) {
	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size(p_size);
	MeshSDFSettings settings;
	settings.voxels_per_meter = p_voxels_per_meter;
	settings.max_voxels_per_axis = p_max_voxels;
	settings.prefer_static_raycaster = false;
	MeshSDFData data;
	const Error err = MeshSDFBuilder::build(box->get_faces(), settings, data);
	CHECK(err == OK);
	CHECK(data.is_valid());
	return data;
}

static MeshSDFData _unit_box_sdf() {
	return _box_sdf(Vector3(1.0f, 1.0f, 1.0f), 8.0f, 32);
}

static NextGIVoxelLighting::CardQuery _card(const Vector3 &p_origin, const Vector3 &p_axis_u, const Vector3 &p_axis_v, const Vector3 &p_axis_n, float p_half_u, float p_half_v, float p_texel) {
	NextGIVoxelLighting::CardQuery card;
	card.origin = p_origin;
	card.axis_u = p_axis_u;
	card.axis_v = p_axis_v;
	card.axis_n = p_axis_n;
	card.half_u = p_half_u;
	card.half_v = p_half_v;
	card.texel_size = p_texel;
	return card;
}

static NextGIVoxelLighting::TraceInstance _instance(const MeshSDFData *p_field, uint32_t p_id, const Transform3D &p_transform) {
	NextGIVoxelLighting::TraceInstance instance;
	instance.id = p_id;
	instance.field = p_field;
	instance.transform = p_transform;
	instance.inverse = p_transform.affine_inverse();
	instance.world_aabb = p_transform.xform(p_field->bounds);
	instance.page_table = 0;
	instance.card_set = 0;
	return instance;
}

static uint32_t _drain(NextGIVoxelLighting &p_lighting, const NextGIVoxelLighting::TraceInstance *p_instances, uint32_t p_count, uint32_t &r_first) {
	const uint32_t pending = p_lighting.pending_bricks();
	const uint32_t brick_rays = NextGIVoxelLighting::SDF_BRICK_VOXELS * NextGIVoxelLighting::SDF_BRICK_VOXELS * NextGIVoxelLighting::SDF_BRICK_VOXELS * NextGIVoxelLighting::DIRECTIONS;
	uint32_t total = 0;
	r_first = 0;
	uint32_t calls = 0;
	while (p_lighting.pending_bricks() > 0 && calls < 4000) {
		const uint32_t rays = p_lighting.retrace(p_instances, p_count);
		if (calls == 0) {
			r_first = rays;
		}
		total += rays;
		calls++;
	}
	CHECK_EQ(p_lighting.pending_bricks(), 0u);
	CHECK_EQ(total, pending * brick_rays);
	return total;
}

// Level-local voxel whose cell contains a world point.
static Vector3i _local_voxel(uint32_t p_level, const Vector3i &p_sdf_origin, const Vector3 &p_world) {
	const float size = NextGIVoxelLighting::level_voxel_size(p_level);
	const Vector3 origin_world = Vector3(p_sdf_origin) * NextGIGlobalSDF::level_voxel_size(p_level);
	const Vector3 local = (p_world - origin_world) / size;
	return Vector3i(int(Math::floor(local.x)), int(Math::floor(local.y)), int(Math::floor(local.z)));
}

static void _fill_axis(NextGIVoxelLighting::DirectionSample *r_samples, uint32_t p_direction, const Color &p_radiance, float p_weight) {
	r_samples[p_direction].radiance = p_radiance;
	r_samples[p_direction].weight = p_weight;
}

// Captured depth of a sphere of radius p_radius at the origin, per card texel, the way the
// SRC capture fills it: distance from the card plane to the first surface along -axis_n.
static LocalVector<float> _sphere_card_depth(const RendererRD::NextGICardDesc &p_card, float p_radius) {
	LocalVector<float> depth;
	depth.resize(p_card.texels_u * p_card.texels_v);
	for (uint32_t ty = 0; ty < p_card.texels_v; ty++) {
		for (uint32_t tx = 0; tx < p_card.texels_u; tx++) {
			const float x = ((float(tx) + 0.5f) / float(p_card.texels_u) * 2.0f - 1.0f) * p_card.half_u;
			const float y = ((float(ty) + 0.5f) / float(p_card.texels_v) * 2.0f - 1.0f) * p_card.half_v;
			const Vector3 start = p_card.origin + p_card.axis_u * x + p_card.axis_v * y;
			const Vector3 dir = -p_card.axis_n;
			const float b = start.dot(dir);
			const float c = start.dot(start) - p_radius * p_radius;
			const float disc = b * b - c;
			float value = 0.0f;
			if (disc >= 0.0f) {
				const float t = -b - Math::sqrt(disc);
				if (t >= 0.0f && t <= p_card.depth_range) {
					value = t;
				}
			}
			depth[ty * p_card.texels_u + tx] = value;
		}
	}
	return depth;
}

} // namespace

TEST_CASE("[NextGI] Voxel lighting packs instance:20 | card:4 | hitT:8") {
	const uint32_t packed = NextGIVoxelLighting::pack_visibility(0xABCDE, 0xA, 0x7F);
	uint32_t instance = 0;
	uint32_t card = 0;
	uint32_t hit_t = 0;
	NextGIVoxelLighting::unpack_visibility(packed, instance, card, hit_t);
	CHECK_EQ(instance, 0xABCDEu);
	CHECK_EQ(card, 0xAu);
	CHECK_EQ(hit_t, 0x7Fu);
	// Bits past each field are discarded. hitT sits in the high byte.
	CHECK_EQ(NextGIVoxelLighting::pack_visibility(0x1ABCDE, 0x1A, 0x17F), packed);
	CHECK_EQ(packed >> 24, 0x7Fu);

	CHECK_EQ(NextGIVoxelLighting::quantize_hit_t(0.0f, 2.0f), 0u);
	CHECK_EQ(NextGIVoxelLighting::quantize_hit_t(2.0f, 2.0f), NextGIVoxelLighting::MAX_HIT_T);
	CHECK_EQ(NextGIVoxelLighting::quantize_hit_t(1.0f, 2.0f), 127u);
}

TEST_CASE("[NextGI] Voxel lighting atomic min keeps the closest hit") {
	const uint32_t close_far_id = NextGIVoxelLighting::pack_visibility(0xFFFFF, 0xF, 10);
	const uint32_t far_small_id = NextGIVoxelLighting::pack_visibility(1, 0, 11);
	CHECK_EQ(NextGIVoxelLighting::atomic_min_visibility(far_small_id, close_far_id), close_far_id);
	CHECK_EQ(NextGIVoxelLighting::atomic_min_visibility(close_far_id, far_small_id), close_far_id);

	const uint32_t card_high = NextGIVoxelLighting::pack_visibility(4, 3, 20);
	const uint32_t card_low = NextGIVoxelLighting::pack_visibility(9, 1, 20);
	CHECK_EQ(NextGIVoxelLighting::atomic_min_visibility(card_high, card_low), card_low);

	const uint32_t instance_high = NextGIVoxelLighting::pack_visibility(8, 2, 20);
	const uint32_t instance_low = NextGIVoxelLighting::pack_visibility(3, 2, 20);
	CHECK_EQ(NextGIVoxelLighting::atomic_min_visibility(instance_high, instance_low), instance_low);

	const uint32_t hit = NextGIVoxelLighting::pack_visibility(1, 0, 0);
	CHECK_EQ(NextGIVoxelLighting::atomic_min_visibility(NextGIVoxelLighting::EMPTY_VISIBILITY, hit), hit);
	CHECK_EQ(NextGIVoxelLighting::atomic_min_visibility(hit, NextGIVoxelLighting::EMPTY_VISIBILITY), hit);
}

TEST_CASE("[NextGI] Voxel lighting normalizes coverage and falls back below 0.5") {
	CHECK_EQ(NextGIVoxelLighting::facing_direction(1, true), 4u);
	CHECK_EQ(NextGIVoxelLighting::facing_direction(1, false), 1u);
	// Gradient +X/+Y reads the -X and -Y slots. The +axis slots are decoys.
	NextGIVoxelLighting::DirectionSample samples[NextGIVoxelLighting::DIRECTIONS];
	_fill_axis(samples, 0, Color(0.0f, 1.0f, 0.0f), 1.0f);
	_fill_axis(samples, 1, Color(0.0f, 1.0f, 0.0f), 1.0f);
	_fill_axis(samples, 3, Color(1.0f, 0.0f, 0.0f), 1.0f);
	_fill_axis(samples, 4, Color(0.0f, 0.0f, 1.0f), 1.0f);
	const Color wrc(0.2f, 0.3f, 0.4f);
	const NextGIVoxelLighting::LookupResult blended = NextGIVoxelLighting::lookup(Vector3(1.0f, 1.0f, 0.0f), samples, wrc);
	CHECK_FALSE(blended.used_wrc);
	CHECK(Math::is_equal_approx(blended.weight, 1.0f));
	CHECK(blended.radiance.is_equal_approx(Color(0.5f, 0.0f, 0.5f)));

	_fill_axis(samples, 3, Color(1.0f, 0.0f, 0.0f), 0.4f);
	_fill_axis(samples, 4, Color(0.0f, 0.0f, 1.0f), 0.4f);
	const NextGIVoxelLighting::LookupResult low = NextGIVoxelLighting::lookup(Vector3(1.0f, 1.0f, 0.0f), samples, wrc);
	CHECK(low.used_wrc);
	CHECK(Math::is_equal_approx(low.weight, 0.4f));
	CHECK(low.radiance.is_equal_approx(wrc));

	// Weight of exactly 0.5 stays on the volume. The covered axis wins.
	_fill_axis(samples, 3, Color(1.0f, 0.0f, 0.0f), 1.0f);
	_fill_axis(samples, 4, Color(0.0f, 0.0f, 1.0f), 0.0f);
	const NextGIVoxelLighting::LookupResult half = NextGIVoxelLighting::lookup(Vector3(1.0f, 1.0f, 0.0f), samples, wrc);
	CHECK_FALSE(half.used_wrc);
	CHECK(Math::is_equal_approx(half.weight, 0.5f));
	CHECK(half.radiance.is_equal_approx(Color(1.0f, 0.0f, 0.0f)));

	const NextGIVoxelLighting::LookupResult flat = NextGIVoxelLighting::lookup(Vector3(), samples, wrc);
	CHECK(flat.used_wrc);
	CHECK(flat.radiance.is_equal_approx(wrc));
}

TEST_CASE("[NextGI] Voxel lighting weight below 0.5 falls back to the WRC") {
	CHECK(Math::is_equal_approx(NextGIVoxelLighting::shade_weight(true, 1.0f, 0.0f), 0.0f));
	CHECK(Math::is_equal_approx(NextGIVoxelLighting::shade_weight(false, -0.25f, 0.0f), 0.0f));
	CHECK(Math::is_equal_approx(NextGIVoxelLighting::shade_weight(false, 0.25f, 0.0f), 0.25f));
	CHECK(Math::is_equal_approx(NextGIVoxelLighting::shade_weight(false, 1.0f, NextGIVoxelLighting::CARD_DEPTH_TEXELS), 1.0f));
	CHECK(Math::is_equal_approx(NextGIVoxelLighting::shade_weight(false, 1.0f, NextGIVoxelLighting::CARD_DEPTH_TEXELS + 1.0f), 0.0f));

	NextGIVoxelLighting::DirectionSample samples[NextGIVoxelLighting::DIRECTIONS];
	_fill_axis(samples, 0, Color(0.0f, 1.0f, 0.0f), 1.0f);
	_fill_axis(samples, 3, Color(1.0f, 0.0f, 0.0f), 0.0f);
	const Color wrc(0.1f, 0.2f, 0.3f);
	const NextGIVoxelLighting::LookupResult missed = NextGIVoxelLighting::lookup(Vector3(1.0f, 0.0f, 0.0f), samples, wrc);
	CHECK(missed.used_wrc);
	CHECK(missed.radiance.is_equal_approx(wrc));
}

TEST_CASE("[NextGI] Voxel lighting volume stays within 32 MiB") {
	const uint64_t voxels = uint64_t(NextGIVoxelLighting::LEVELS) * 64ull * 64ull * 64ull;
	const uint64_t radiance = voxels * 6ull * 4ull;
	const uint64_t weights = voxels * 6ull;
	const uint64_t staging = 2ull * 1024ull * 1024ull;
	CHECK_EQ(NextGIVoxelLighting::radiance_buffer_bytes(), radiance);
	CHECK_EQ(NextGIVoxelLighting::weight_buffer_bytes(), weights);
	CHECK_EQ(NextGIVoxelLighting::staging_buffer_bytes(), staging);
	CHECK_EQ(NextGIVoxelLighting::visibility_buffer_bytes(), 0ull);
	CHECK_EQ(NextGIVoxelLighting::allocated_volume_bytes(), radiance + weights + staging);
	CHECK_EQ(NextGIVoxelLighting::allocated_volume_bytes(), 33554432ull);
	CHECK(NextGIVoxelLighting::allocated_volume_bytes() <= 32ull * 1024ull * 1024ull);
	NextGIVoxelLighting lighting;
	CHECK_EQ(lighting.get_allocated_bytes(), 0ull);
	CHECK(Math::is_equal_approx(NextGIVoxelLighting::level_voxel_size(0), 0.2f));
	// GPU counters are only known after a debug readback. Nothing was read back.
	CHECK_EQ(lighting.get_coverage_ppm(), -1);
	CHECK_EQ(lighting.get_resampled_nonzero(), -1);
	CHECK_EQ(lighting.get_debug_readbacks(), 0ull);
}

TEST_CASE("[NextGI] Voxel lighting gradient picks the facing axis") {
	const float step = 1.0f;
	const float taps[6] = { step, -step, 0.0f, 0.0f, 0.0f, 0.0f };
	const Vector3 gradient = NextGIVoxelLighting::gradient_from_taps(taps, step);
	CHECK(gradient.is_equal_approx(Vector3(1.0f, 0.0f, 0.0f)));

	// +X gradient reads the -X slot. Green in the +X slot must not leak through.
	NextGIVoxelLighting::DirectionSample samples[NextGIVoxelLighting::DIRECTIONS];
	_fill_axis(samples, 0, Color(0.0f, 1.0f, 0.0f), 1.0f);
	_fill_axis(samples, 3, Color(1.0f, 0.0f, 0.0f), 1.0f);
	const NextGIVoxelLighting::LookupResult facing = NextGIVoxelLighting::lookup(gradient, samples, Color(0.0f, 0.0f, 0.0f));
	CHECK_FALSE(facing.used_wrc);
	CHECK(facing.radiance.is_equal_approx(Color(1.0f, 0.0f, 0.0f)));
}

TEST_CASE("[NextGI] Voxel lighting re-traces dirty bricks only") {
	NextGIVoxelLighting lighting;
	lighting.set_clipmap_origins_from_camera(Vector3());
	// New levels wait for refresh_instances; an empty feed leaves nothing to trace.
	CHECK_EQ(lighting.get_levels_to_fill(), 0xFu);
	lighting.refresh_instances(nullptr, 0);
	CHECK_EQ(lighting.get_levels_to_fill(), 0u);
	CHECK_EQ(lighting.pending_bricks(), 0u);
	const Vector3i origin = NextGIGlobalSDF::scrolled_origin(Vector3(), 0);
	const Vector3 center = NextGIVoxelLighting::voxel_center(0, origin, Vector3i(32, 32, 32));
	const AABB volume(center - Vector3(0.01f, 0.01f, 0.01f), Vector3(0.02f, 0.02f, 0.02f));
	lighting.enqueue_dirty_volumes(&volume, 1);
	const uint32_t pending = lighting.pending_bricks();
	CHECK_GT(pending, NextGIVoxelLighting::LEVELS);
	CHECK_GT(lighting.pending_bricks_at_level(0), 1u);
	const uint32_t level_counts[NextGIVoxelLighting::LEVELS] = {
		lighting.pending_bricks_at_level(0),
		lighting.pending_bricks_at_level(1),
		lighting.pending_bricks_at_level(2),
		lighting.pending_bricks_at_level(3),
	};
	CHECK_GT(level_counts[3], 0u);

	uint32_t first = 0;
	_drain(lighting, nullptr, 0, first);
	CHECK_EQ(first, NextGIVoxelLighting::RAYS_PER_FRAME);
	CHECK_EQ(lighting.retrace(nullptr, 0), 0u);
	lighting.shade(nullptr, 0);
	CHECK_EQ(lighting.get_retrace_rays(), 0u);
	CHECK_EQ(lighting.get_shaded_hits(), 0u);

	// Scroll one brick on X. Level 0 gains only the new slab. Levels 1-3 stay.
	lighting.enqueue_dirty_volumes(&volume, 1);
	const uint32_t before[NextGIVoxelLighting::LEVELS] = {
		lighting.pending_bricks_at_level(0),
		lighting.pending_bricks_at_level(1),
		lighting.pending_bricks_at_level(2),
		lighting.pending_bricks_at_level(3),
	};
	CHECK_GT(before[3], 0u);
	lighting.set_clipmap_origins_from_camera(Vector3(0.8f, 0.0f, 0.0f));
	Vector3i edge_bricks[4096];
	const Vector3i old_origin = NextGIGlobalSDF::scrolled_origin(Vector3(), 0);
	const Vector3i new_origin = NextGIGlobalSDF::scrolled_origin(Vector3(0.8f, 0.0f, 0.0f), 0);
	const uint32_t edge_count = NextGIGlobalSDF::scrolled_edge_bricks(NextGIGlobalSDF::scroll_brick_delta(old_origin, new_origin), edge_bricks, 4096);
	CHECK_GT(edge_count, 0u);
	CHECK_EQ(lighting.pending_bricks_at_level(0), before[0] + edge_count);
	CHECK_EQ(lighting.pending_bricks_at_level(1), before[1]);
	CHECK_EQ(lighting.pending_bricks_at_level(2), before[2]);
	CHECK_EQ(lighting.pending_bricks_at_level(3), before[3]);
	CHECK_EQ(lighting.get_levels_to_fill(), 0u);

	// A shift that is not a whole brick drops that level, leaves the others queued, and
	// marks the dropped level for a refill.
	Vector3i origins[NextGIVoxelLighting::LEVELS];
	for (uint32_t level = 0; level < NextGIVoxelLighting::LEVELS; level++) {
		origins[level] = NextGIGlobalSDF::scrolled_origin(Vector3(0.8f, 0.0f, 0.0f), level);
	}
	origins[0].x += 1;
	const uint32_t kept_level3 = lighting.pending_bricks_at_level(3);
	lighting.set_clipmap_origins(origins, NextGIVoxelLighting::LEVELS);
	CHECK_EQ(lighting.pending_bricks_at_level(0), 0u);
	CHECK_EQ(lighting.pending_bricks_at_level(3), kept_level3);
	CHECK_LT(lighting.pending_bricks(), 4096u);
	CHECK_EQ(lighting.get_levels_to_fill(), 1u);
}

TEST_CASE("[NextGI] Voxel lighting fills a new volume from static geometry and late mesh SDFs") {
	const MeshSDFData box = _unit_box_sdf();
	const NextGIVoxelLighting::TraceInstance instance = _instance(&box, 7, Transform3D());
	NextGIVoxelLighting::InstanceBounds bounds;
	bounds.mesh = 42;
	bounds.world_aabb = instance.world_aabb;
	bounds.field = &box;

	// A volume created after the scene loaded (a tier change) gets no registry event.
	NextGIVoxelLighting lighting;
	lighting.set_clipmap_origins_from_camera(Vector3());
	CHECK_EQ(lighting.pending_bricks(), 0u);
	lighting.refresh_instances(&bounds, 1);
	for (uint32_t level = 0; level < NextGIVoxelLighting::LEVELS; level++) {
		CHECK_GT(lighting.pending_bricks_at_level(level), 0u);
	}
	uint32_t first = 0;
	_drain(lighting, &instance, 1, first);
	CHECK_GT(lighting.get_stored_hits(), 0u);

	// A static scene does not re-trace.
	lighting.refresh_instances(&bounds, 1);
	CHECK_EQ(lighting.pending_bricks(), 0u);

	// A mesh SDF that finishes after its bricks were traced queues them again. The frame step
	// update_render takes: begin_frame with the SDF atlas generation, then refresh_instances
	// whenever it returns true.
	NextGIVoxelLighting late;
	late.set_clipmap_origins_from_camera(Vector3());
	NextGIVoxelLighting::InstanceBounds building = bounds;
	building.field = nullptr;
	uint64_t generation = 5;
	REQUIRE(late.begin_frame(generation));
	late.refresh_instances(&building, 1);
	CHECK_EQ(late.pending_bricks(), 0u);
	// Nothing changed: the frame skips the feed.
	CHECK_FALSE(late.begin_frame(generation));
	// The mesh SDF was built, which moves the atlas generation. There is no registry event.
	generation++;
	REQUIRE(late.begin_frame(generation));
	late.refresh_instances(&bounds, 1);
	CHECK_GT(late.pending_bricks(), 0u);
	_drain(late, &instance, 1, first);
	CHECK_EQ(late.get_stored_hits(), lighting.get_stored_hits());
	CHECK_FALSE(late.begin_frame(generation));
	// A level dropped by a non-aligned shift waits for its fill, with the generation unchanged.
	Vector3i origins[NextGIVoxelLighting::LEVELS];
	for (uint32_t level = 0; level < NextGIVoxelLighting::LEVELS; level++) {
		origins[level] = NextGIGlobalSDF::scrolled_origin(Vector3(), level);
	}
	origins[1].y += 1;
	late.set_clipmap_origins(origins, NextGIVoxelLighting::LEVELS);
	CHECK(late.begin_frame(generation));
}

TEST_CASE("[NextGI] Voxel lighting debug probes keep every direction slot") {
	// Two slots per voxel, stored in direction order as _trace_brick stores them, and twice as
	// many hits as probes. Every second hit would be the same slot of each voxel.
	HashMap<uint32_t, NextGIVoxelLighting::AxisHit> hits;
	const uint32_t voxels = NextGIVoxelLighting::MAX_DEBUG_PROBES;
	for (uint32_t voxel = 0; voxel < voxels; voxel++) {
		for (uint32_t direction : { 2u, 5u }) {
			NextGIVoxelLighting::AxisHit hit;
			hit.hit = true;
			hits.insert(NextGIVoxelLighting::direction_index(0, voxel, direction), hit);
		}
	}
	LocalVector<uint32_t> keys;
	NextGIVoxelLighting::select_debug_probes(hits, NextGIVoxelLighting::MAX_DEBUG_PROBES, keys);
	CHECK_LE(keys.size(), NextGIVoxelLighting::MAX_DEBUG_PROBES);
	CHECK_GT(keys.size(), NextGIVoxelLighting::MAX_DEBUG_PROBES / 4);
	HashSet<uint32_t> picked;
	uint32_t per_direction[NextGIVoxelLighting::DIRECTIONS] = {};
	for (uint32_t key : keys) {
		REQUIRE(hits.has(key));
		picked.insert(key);
		per_direction[key % NextGIVoxelLighting::DIRECTIONS]++;
	}
	CHECK_EQ(picked.size(), keys.size());
	// Whole voxels: both slots of a picked voxel, so the two directions get the same count.
	CHECK_GT(per_direction[2], 0u);
	CHECK_EQ(per_direction[2], per_direction[5]);
	for (uint32_t key : keys) {
		const uint32_t voxel_base = key - key % NextGIVoxelLighting::DIRECTIONS;
		CHECK(picked.has(voxel_base + 2u));
		CHECK(picked.has(voxel_base + 5u));
	}
	// Under the cap, every hit is probed.
	NextGIVoxelLighting::select_debug_probes(hits, 3u * NextGIVoxelLighting::MAX_DEBUG_PROBES, keys);
	CHECK_EQ(keys.size(), hits.size());
}

TEST_CASE("[NextGI] Voxel lighting stores an entry in the slot that faces the ray") {
	const MeshSDFData box = _unit_box_sdf();
	const NextGIVoxelLighting::TraceInstance instance = _instance(&box, 7, Transform3D());
	NextGIVoxelLighting::AxisHit slots[NextGIVoxelLighting::DIRECTIONS];
	const uint32_t filled = NextGIVoxelLighting::trace_voxel(&instance, 1, Vector3(-1.0f, 0.1f, 0.05f), 2.0f, slots);
	CHECK_EQ(filled, 1u);
	const NextGIVoxelLighting::AxisHit &hit = slots[0];
	CHECK(hit.hit);
	CHECK_FALSE(hit.exit);
	CHECK_EQ(hit.instance, 7u);
	// Refined to the field's zero crossing on the -X face.
	CHECK(Math::abs(hit.position.x + 0.5f) < 0.02f);
	CHECK(Math::abs(hit.distance - 0.5f) < 0.02f);
	CHECK(hit.normal.x < -0.9f);
	CHECK(hit.object_position.is_equal_approx(hit.position));
	uint32_t packed_instance = 0;
	uint32_t card = 0;
	uint32_t hit_t = 0;
	NextGIVoxelLighting::unpack_visibility(hit.packed, packed_instance, card, hit_t);
	CHECK_EQ(packed_instance, 7u);
	CHECK_EQ(card, NextGIVoxelLighting::NO_CARD);
	CHECK_EQ(hit_t, NextGIVoxelLighting::quantize_hit_t(hit.distance, 2.0f));
	for (uint32_t direction = 1; direction < NextGIVoxelLighting::DIRECTIONS; direction++) {
		CHECK_FALSE(slots[direction].hit);
	}
}

TEST_CASE("[NextGI] Voxel lighting stores a wall's faces in their own slots from inside it") {
	// A 0.4 m wall at z = -0.2..0.2. Level-0 lighting voxels are 0.2 m, so voxel centers at
	// z = -0.1 and 0.1 lie inside it.
	const MeshSDFData wall = _box_sdf(Vector3(3.0f, 3.0f, 0.4f), 10.0f, 128);
	const NextGIVoxelLighting::TraceInstance instance = _instance(&wall, 3, Transform3D());
	NextGIVoxelLighting lighting;
	lighting.set_clipmap_origins_from_camera(Vector3());
	const AABB core(Vector3(-0.5f, -0.5f, -0.3f), Vector3(1.0f, 1.0f, 0.6f));
	lighting.enqueue_dirty_volumes(&core, 1);
	uint32_t first = 0;
	_drain(lighting, &instance, 1, first);
	const Vector3i origin = NextGIGlobalSDF::scrolled_origin(Vector3(), 0);

	const Vector3i inside = _local_voxel(0, origin, Vector3(0.05f, 0.05f, 0.05f));
	const Vector3 inside_center = NextGIVoxelLighting::voxel_center(0, origin, inside);
	CHECK(Math::is_equal_approx(inside_center.z, 0.1f));
	NextGIVoxelLighting::AxisHit front;
	NextGIVoxelLighting::AxisHit back;
	// Slot 5 (-Z) holds the surface facing +Z, slot 2 (+Z) the one facing -Z.
	REQUIRE(lighting.get_stored_hit(0, inside, 5, front));
	REQUIRE(lighting.get_stored_hit(0, inside, 2, back));
	CHECK(front.exit);
	CHECK(Math::abs(front.position.z - 0.2f) < 0.01f);
	CHECK(front.normal.z > 0.9f);
	CHECK(back.exit);
	CHECK(Math::abs(back.position.z + 0.2f) < 0.01f);
	CHECK(back.normal.z < -0.9f);

	const Vector3i outside = _local_voxel(0, origin, Vector3(0.05f, 0.05f, 0.25f));
	NextGIVoxelLighting::AxisHit entry;
	REQUIRE(lighting.get_stored_hit(0, outside, 5, entry));
	CHECK_FALSE(entry.exit);
	CHECK(Math::abs(entry.position.z - 0.2f) < 0.01f);
	CHECK(entry.normal.z > 0.9f);
	NextGIVoxelLighting::AxisHit beyond;
	CHECK_FALSE(lighting.get_stored_hit(0, outside, 2, beyond));

	// No slot anywhere near the wall holds a surface that faces the wrong way. A lookup on the
	// +Z face blends the voxels at z = 0.1 and 0.3 from slot 5, so both must hold the +Z face.
	uint32_t checked = 0;
	for (int z = -2; z <= 2; z++) {
		for (int y = -2; y <= 2; y++) {
			for (int x = -2; x <= 2; x++) {
				const Vector3i voxel = inside + Vector3i(x, y, z);
				for (uint32_t direction = 0; direction < NextGIVoxelLighting::DIRECTIONS; direction++) {
					NextGIVoxelLighting::AxisHit stored;
					if (!lighting.get_stored_hit(0, voxel, direction, stored)) {
						continue;
					}
					checked++;
					CHECK(stored.normal.dot(NextGIVoxelLighting::direction_vector(direction)) < -0.9f);
				}
			}
		}
	}
	CHECK_GT(checked, 0u);
}

TEST_CASE("[NextGI] Voxel lighting keeps the far room out of voxels inside a thin wall") {
	// A 0.2 m wall at the default 10 mesh-SDF voxels per meter. Its field never drops much
	// below -eps, so the level-0 voxel center at z = 0.1 inside it is within eps of a face.
	// Room A (z < low) is dark; room B holds a lit box whose -Z face also faces -Z. A lookup on
	// the wall's room-A face blends slot 2 (+Z) of the voxels at z = -0.1 and 0.1: both must
	// hold that face, never the box beyond the wall.
	const MeshSDFData wall_field = _box_sdf(Vector3(3.0f, 3.0f, 0.2f), 10.0f, 128);
	const MeshSDFData box_field = _box_sdf(Vector3(3.0f, 3.0f, 0.4f), 10.0f, 128);
	const float eps = wall_field.voxel_size * 0.5f;
	const Vector3 center(0.1f, 0.1f, 0.1f);
	// Wall at low..low + 0.2. The box's -Z face 0.37 m beyond it, or 0.03 m beyond it, nearer
	// the center than the wall's room-A face.
	const float lows[2] = { -0.07f, -0.03f };
	const float gaps[2] = { 0.37f, 0.03f };
	for (float low : lows) {
		const float high = low + 0.2f;
		for (float gap : gaps) {
			const float box_face = high + gap;
			const NextGIVoxelLighting::TraceInstance instances[2] = {
				_instance(&wall_field, 3, Transform3D(Basis(), Vector3(0.0f, 0.0f, low + 0.1f))),
				_instance(&box_field, 8, Transform3D(Basis(), Vector3(0.0f, 0.0f, box_face + 0.2f))),
			};
			const float start = wall_field.sample_distance(instances[0].inverse.xform(center));
			REQUIRE(start < 0.0f);
			if (low < -0.05f) {
				// 3 cm from the +Z face: within eps, the case that used to leak.
				REQUIRE(start >= -eps);
			}
			NextGIVoxelLighting::AxisHit slots[NextGIVoxelLighting::DIRECTIONS];
			NextGIVoxelLighting::trace_voxel(instances, 2, center, 0.8f, slots);
			// Slot 2 (+Z) holds the room-A face, the exit found along -Z.
			REQUIRE(slots[2].hit);
			CHECK_EQ(slots[2].instance, 3u);
			CHECK(slots[2].exit);
			CHECK(Math::abs(slots[2].position.z - low) < 0.02f);
			CHECK(slots[2].normal.z < -0.9f);
			// Slot 5 (-Z) holds the room-B face, the exit found along +Z.
			REQUIRE(slots[5].hit);
			CHECK_EQ(slots[5].instance, 3u);
			CHECK(slots[5].exit);
			CHECK(Math::abs(slots[5].position.z - high) < 0.02f);
			CHECK(slots[5].normal.z > 0.9f);
			for (uint32_t direction = 0; direction < NextGIVoxelLighting::DIRECTIONS; direction++) {
				CHECK_FALSE((slots[direction].hit && slots[direction].instance == 8u));
			}
		}
	}

	// Through the volume, for the 3 cm case with the box 0.37 m beyond the wall.
	const NextGIVoxelLighting::TraceInstance instances[2] = {
		_instance(&wall_field, 3, Transform3D(Basis(), Vector3(0.0f, 0.0f, 0.03f))),
		_instance(&box_field, 8, Transform3D(Basis(), Vector3(0.0f, 0.0f, 0.7f))),
	};
	NextGIVoxelLighting lighting;
	lighting.set_clipmap_origins_from_camera(Vector3());
	const AABB core(Vector3(-0.5f, -0.5f, -0.3f), Vector3(1.0f, 1.0f, 0.6f));
	lighting.enqueue_dirty_volumes(&core, 1);
	uint32_t first = 0;
	_drain(lighting, instances, 2, first);
	const Vector3i origin = NextGIGlobalSDF::scrolled_origin(Vector3(), 0);
	const Vector3i in_wall = _local_voxel(0, origin, Vector3(0.05f, 0.05f, 0.05f));
	REQUIRE(Math::is_equal_approx(NextGIVoxelLighting::voxel_center(0, origin, in_wall).z, 0.1f));
	const Vector3i room_a = in_wall - Vector3i(0, 0, 1);
	uint32_t checked = 0;
	for (int y = -2; y <= 2; y++) {
		for (int x = -2; x <= 2; x++) {
			for (const Vector3i &voxel : { room_a, in_wall }) {
				NextGIVoxelLighting::AxisHit stored;
				REQUIRE(lighting.get_stored_hit(0, voxel + Vector3i(x, y, 0), 2, stored));
				CHECK_EQ(stored.instance, 3u);
				CHECK(Math::abs(stored.position.z + 0.07f) < 0.02f);
				checked++;
			}
		}
	}
	CHECK_EQ(checked, 50u);
}

TEST_CASE("[NextGI] Voxel lighting files a touched floor only in the slot that faces it") {
	// A 0.4 m slab whose top face lies on, just above or just below the level-0 voxel-center
	// row at y = 0.1, within the mesh SDF's eps. Those centers touch the floor.
	const MeshSDFData slab = _box_sdf(Vector3(3.0f, 0.4f, 3.0f), 10.0f, 128);
	const float eps = slab.voxel_size * 0.5f;
	const float offsets[3] = { 0.0f, 0.6f * eps, -0.6f * eps };
	const uint32_t along_floor[4] = { 0, 2, 3, 5 };
	for (float offset : offsets) {
		const float top = 0.1f + offset;
		const NextGIVoxelLighting::TraceInstance floor = _instance(&slab, 5, Transform3D(Basis(), Vector3(0.0f, top - 0.2f, 0.0f)));
		NextGIVoxelLighting::AxisHit slots[NextGIVoxelLighting::DIRECTIONS];
		NextGIVoxelLighting::trace_voxel(&floor, 1, Vector3(0.1f, 0.1f, 0.1f), 0.8f, slots);
		const float start = slab.sample_distance(floor.inverse.xform(Vector3(0.1f, 0.1f, 0.1f)));
		if (offset > 0.0f) {
			REQUIRE(start < 0.0f);
		} else if (offset < 0.0f) {
			REQUIRE(start > 0.0f);
		}
		// The floor faces +Y: it belongs to slot 4 (-Y). From outside it is the touch at
		// distance 0. From inside the slab it is the exit found along +Y, refined to the face.
		REQUIRE(slots[4].hit);
		CHECK_EQ(slots[4].instance, 5u);
		CHECK(slots[4].normal.y > 0.9f);
		CHECK(Math::abs(slots[4].position.y - top) < 0.02f);
		if (start < 0.0f) {
			CHECK(slots[4].exit);
			CHECK(Math::abs(slots[4].distance + start) < 0.02f);
		} else {
			CHECK_FALSE(slots[4].exit);
			CHECK(slots[4].distance == 0.0f);
		}
		// It runs along X and Z, so no X or Z slot holds it, and nothing lies above.
		for (uint32_t direction : along_floor) {
			CHECK_FALSE(slots[direction].hit);
		}
		// A center inside the slab (its top 0.6 eps above) is inside a solid: slot 1 (+Y) holds
		// the slab's bottom, the exit found along -Y, 0.37 m down. Outside the slab it is empty.
		CHECK(slots[1].hit == (start < 0.0f));
		if (slots[1].hit) {
			CHECK_EQ(slots[1].instance, 5u);
			CHECK(slots[1].exit);
			CHECK(slots[1].normal.y < -0.9f);
			CHECK(Math::abs(slots[1].position.y - (top - 0.4f)) < 0.02f);
		}
	}

	// Through the volume: no slot near the touched row holds a surface that does not face it.
	const NextGIVoxelLighting::TraceInstance floor = _instance(&slab, 5, Transform3D(Basis(), Vector3(0.0f, -0.1f, 0.0f)));
	NextGIVoxelLighting lighting;
	lighting.set_clipmap_origins_from_camera(Vector3());
	const AABB core(Vector3(-0.5f, -0.3f, -0.5f), Vector3(1.0f, 0.8f, 1.0f));
	lighting.enqueue_dirty_volumes(&core, 1);
	uint32_t first = 0;
	_drain(lighting, &floor, 1, first);
	const Vector3i origin = NextGIGlobalSDF::scrolled_origin(Vector3(), 0);
	const Vector3i touching = _local_voxel(0, origin, Vector3(0.05f, 0.05f, 0.05f));
	REQUIRE(Math::is_equal_approx(NextGIVoxelLighting::voxel_center(0, origin, touching).y, 0.1f));
	NextGIVoxelLighting::AxisHit top_hit;
	REQUIRE(lighting.get_stored_hit(0, touching, 4, top_hit));
	CHECK(top_hit.normal.y > 0.9f);
	uint32_t checked = 0;
	for (int z = -2; z <= 2; z++) {
		for (int y = -1; y <= 2; y++) {
			for (int x = -2; x <= 2; x++) {
				const Vector3i voxel = touching + Vector3i(x, y, z);
				for (uint32_t direction = 0; direction < NextGIVoxelLighting::DIRECTIONS; direction++) {
					NextGIVoxelLighting::AxisHit stored;
					if (!lighting.get_stored_hit(0, voxel, direction, stored)) {
						continue;
					}
					checked++;
					CHECK(stored.normal.dot(NextGIVoxelLighting::direction_vector(direction)) < -0.9f);
				}
			}
		}
	}
	CHECK_GT(checked, 0u);
}

TEST_CASE("[NextGI] Voxel lighting finds a wall of the touched floor's own mesh") {
	// One mesh: a floor slab (x -1.5..0.3, top at y = 0.07) and a wall past its +X edge
	// (x 0.4..0.6). The level-0 voxel center at (0.1, 0.1, 0.1) is 3 cm above the floor, within
	// the mesh SDF's eps, so it touches the floor, which runs along X. The +X ray must still find
	// the same mesh's wall 0.3 m away.
	Vector<Face3> faces;
	const struct {
		Vector3 size;
		Vector3 center;
	} parts[2] = {
		{ Vector3(1.8f, 0.4f, 3.0f), Vector3(-0.6f, -0.13f, 0.0f) },
		{ Vector3(0.2f, 2.4f, 3.0f), Vector3(0.5f, 0.87f, 0.0f) },
	};
	for (const auto &part : parts) {
		Ref<BoxMesh> box;
		box.instantiate();
		box->set_size(part.size);
		for (const Face3 &face : box->get_faces()) {
			faces.push_back(Face3(face.vertex[0] + part.center, face.vertex[1] + part.center, face.vertex[2] + part.center));
		}
	}
	MeshSDFSettings settings;
	settings.voxels_per_meter = 10.0f;
	settings.max_voxels_per_axis = 128;
	settings.prefer_static_raycaster = false;
	MeshSDFData field;
	REQUIRE(MeshSDFBuilder::build(faces, settings, field) == OK);
	const float eps = field.voxel_size * 0.5f;
	const NextGIVoxelLighting::TraceInstance room = _instance(&field, 4, Transform3D());
	const Vector3 center(0.1f, 0.1f, 0.1f);
	const float start = field.sample_distance(center);
	REQUIRE(start > 0.0f);
	REQUIRE(start <= eps);
	NextGIVoxelLighting::AxisHit slots[NextGIVoxelLighting::DIRECTIONS];
	NextGIVoxelLighting::trace_voxel(&room, 1, center, 0.8f, slots);
	REQUIRE(slots[4].hit);
	CHECK(slots[4].normal.y > 0.9f);
	CHECK(slots[4].distance == 0.0f);
	REQUIRE(slots[0].hit);
	CHECK_EQ(slots[0].instance, 4u);
	CHECK_FALSE(slots[0].exit);
	CHECK(slots[0].normal.x < -0.9f);
	CHECK(Math::abs(slots[0].position.x - 0.4f) < 0.02f);
	CHECK(Math::abs(slots[0].distance - 0.3f) < 0.02f);
	// Along the floor in the other directions there is nothing to find, and nothing above.
	for (uint32_t direction : { 1u, 2u, 3u, 5u }) {
		CHECK_FALSE(slots[direction].hit);
	}
}

TEST_CASE("[NextGI] Voxel lighting keeps a nearly level floor out of the slot of a wall beside it") {
	// The center touches a floor tilted 5 degrees, so the floor faces -X a little (cosine 0.087).
	// A wall 0.3 m along +X faces -X. Slot 0 (+X) must hold the wall: a lookup on the wall reads
	// it, and the floor at distance 0 would hand the wall the floor's radiance.
	const MeshSDFData slab = _box_sdf(Vector3(3.0f, 0.4f, 3.0f), 10.0f, 128);
	const MeshSDFData wall_field = _box_sdf(Vector3(0.2f, 3.0f, 3.0f), 10.0f, 128);
	const Vector3 center(0.1f, 0.1f, 0.1f);
	const Basis tilt(Vector3(0.0f, 0.0f, 1.0f), Math::deg_to_rad(5.0f));
	const Vector3 floor_normal = tilt.xform(Vector3(0.0f, 1.0f, 0.0f));
	REQUIRE(floor_normal.x < -0.08f);
	const NextGIVoxelLighting::TraceInstance instances[2] = {
		_instance(&slab, 5, Transform3D(tilt, center - floor_normal * 0.2f)),
		_instance(&wall_field, 6, Transform3D(Basis(), center + Vector3(0.4f, 0.0f, 0.0f))),
	};
	NextGIVoxelLighting::AxisHit slots[NextGIVoxelLighting::DIRECTIONS];
	NextGIVoxelLighting::trace_voxel(instances, 2, center, 0.8f, slots);
	REQUIRE(slots[0].hit);
	CHECK_EQ(slots[0].instance, 6u);
	CHECK(slots[0].normal.x < -0.9f);
	CHECK(Math::abs(slots[0].distance - 0.3f) < 0.02f);
	REQUIRE(slots[4].hit);
	CHECK_EQ(slots[4].instance, 5u);
	CHECK(slots[4].distance == 0.0f);
	CHECK_FALSE(slots[3].hit);

	// The same floor 3 cm above the center, so the center is inside it, and the wall 0.4 m away.
	// The -X ray leaves the floor through its top 0.34 m away, at that shallow angle; slot 0 must
	// still hold the wall, not that exit. The floor's top goes to slot 4 as the exit along +Y.
	const NextGIVoxelLighting::TraceInstance sunk[2] = {
		_instance(&slab, 5, Transform3D(tilt, center - floor_normal * 0.17f)),
		_instance(&wall_field, 6, Transform3D(Basis(), center + Vector3(0.5f, 0.0f, 0.0f))),
	};
	const float start = slab.sample_distance(sunk[0].inverse.xform(center));
	REQUIRE(start < 0.0f);
	REQUIRE(start >= -slab.voxel_size * 0.5f);
	NextGIVoxelLighting::trace_voxel(sunk, 2, center, 0.8f, slots);
	REQUIRE(slots[0].hit);
	CHECK_EQ(slots[0].instance, 6u);
	CHECK(Math::abs(slots[0].distance - 0.4f) < 0.02f);
	REQUIRE(slots[4].hit);
	CHECK_EQ(slots[4].instance, 5u);
	CHECK(slots[4].exit);
	CHECK(Math::abs(slots[4].distance - 0.03f) < 0.02f);
	CHECK_FALSE(slots[3].hit);
}

TEST_CASE("[NextGI] Voxel lighting drops a surface an axis ray only grazes") {
	// The +X ray passes just over the top of a sphere, within the mesh SDF's eps but without
	// crossing it. The nearest approach faces +Y, not -X, so slot 0 must not take it.
	const float radius = 0.5f;
	Ref<SphereMesh> sphere;
	sphere.instantiate();
	sphere->set_radius(radius);
	sphere->set_height(radius * 2.0f);
	MeshSDFSettings settings;
	settings.voxels_per_meter = 10.0f;
	settings.max_voxels_per_axis = 64;
	settings.prefer_static_raycaster = false;
	MeshSDFData field;
	REQUIRE(MeshSDFBuilder::build(sphere->get_faces(), settings, field) == OK);
	const float eps = field.voxel_size * 0.5f;
	const NextGIVoxelLighting::TraceInstance instance = _instance(&field, 4, Transform3D());
	const Vector3 center(-1.0f, radius + 0.5f * eps, 0.0f);
	NextGIVoxelLighting::AxisHit slots[NextGIVoxelLighting::DIRECTIONS];
	NextGIVoxelLighting::trace_voxel(&instance, 1, center, 2.0f, slots);
	for (uint32_t direction = 0; direction < NextGIVoxelLighting::DIRECTIONS; direction++) {
		if (slots[direction].hit) {
			CHECK(slots[direction].normal.dot(NextGIVoxelLighting::direction_vector(direction)) < -NextGIVoxelLighting::SLOT_FACING_MIN);
		}
	}
	CHECK_FALSE(slots[0].hit);
}

TEST_CASE("[NextGI] Voxel lighting traces dirty bricks before a pending level fill") {
	const MeshSDFData box = _unit_box_sdf();
	// A box that moved, inside a region whose level fill is still queued.
	const NextGIVoxelLighting::TraceInstance moved = _instance(&box, 2, Transform3D(Basis(), Vector3(4.5f, 4.5f, 4.5f)));
	const Vector3i origin = NextGIGlobalSDF::scrolled_origin(Vector3(), 0);
	const Vector3i above = _local_voxel(0, origin, Vector3(4.55f, 5.25f, 4.55f));
	const Vector3 center = NextGIVoxelLighting::voxel_center(0, origin, above);
	const AABB dirty(center - Vector3(0.01f, 0.01f, 0.01f), Vector3(0.02f, 0.02f, 0.02f));

	NextGIVoxelLighting dirty_only;
	dirty_only.set_clipmap_origins_from_camera(Vector3());
	dirty_only.refresh_instances(nullptr, 0);
	dirty_only.enqueue_dirty_volumes(&dirty, 1);
	const uint32_t dirty_bricks = dirty_only.pending_bricks();
	REQUIRE(dirty_bricks > 0u);

	NextGIVoxelLighting lighting;
	lighting.set_clipmap_origins_from_camera(Vector3());
	NextGIVoxelLighting::InstanceBounds region;
	region.mesh = 11;
	region.world_aabb = AABB(Vector3(-2.0f, -2.0f, -2.0f), Vector3(8.0f, 8.0f, 8.0f));
	region.field = &box;
	lighting.refresh_instances(&region, 1);
	const uint32_t fill_bricks = lighting.pending_bricks();
	REQUIRE(fill_bricks > 4u * dirty_bricks);
	// The dirty bricks are already queued as fill. They move up, and nothing is queued twice.
	lighting.enqueue_dirty_volumes(&dirty, 1);
	CHECK_EQ(lighting.pending_bricks(), fill_bricks);

	const uint32_t brick_rays = NextGIVoxelLighting::SDF_BRICK_VOXELS * NextGIVoxelLighting::SDF_BRICK_VOXELS * NextGIVoxelLighting::SDF_BRICK_VOXELS * NextGIVoxelLighting::DIRECTIONS;
	const uint32_t frames = (dirty_bricks * brick_rays + NextGIVoxelLighting::RAYS_PER_FRAME - 1) / NextGIVoxelLighting::RAYS_PER_FRAME;
	uint32_t traced = 0;
	for (uint32_t frame = 0; frame < frames; frame++) {
		traced += lighting.retrace(&moved, 1) / brick_rays;
	}
	CHECK_EQ(lighting.pending_bricks(), fill_bricks - traced);
	// The moved box's top is stored after the dirty bricks' own frames, not after the fill.
	NextGIVoxelLighting::AxisHit top;
	REQUIRE(lighting.get_stored_hit(0, above, 4, top));
	CHECK_EQ(top.instance, 2u);
	CHECK(Math::abs(top.position.y - 5.0f) < 0.02f);
	uint32_t first = 0;
	_drain(lighting, &moved, 1, first);
}

TEST_CASE("[NextGI] Voxel lighting keeps the nearest instances when a brick is over the cap") {
	const MeshSDFData box = _unit_box_sdf();
	NextGIVoxelLighting lighting;
	lighting.set_clipmap_origins_from_camera(Vector3());
	const Vector3i origin = NextGIGlobalSDF::scrolled_origin(Vector3(), 0);
	const Vector3i brick(8, 8, 8);
	const Vector3 brick_center = NextGIVoxelLighting::voxel_center(0, origin, brick * int(NextGIVoxelLighting::SDF_BRICK_VOXELS) + Vector3i(2, 2, 2)) - Vector3(0.1f, 0.1f, 0.1f);
	// 39 farther unit boxes first in feed order, then the one at the brick center. All of them
	// overlap the brick grown by the trace reach.
	const uint32_t count = NextGIVoxelLighting::MAX_TRACE_INSTANCES + 8;
	LocalVector<NextGIVoxelLighting::TraceInstance> instances;
	for (uint32_t index = 0; index < count; index++) {
		const bool center = index + 1 == count;
		const Vector3 position = brick_center + Vector3(center ? 0.0f : 1.0f + 0.005f * float(index), 0.0f, 0.0f);
		NextGIVoxelLighting::TraceInstance instance = _instance(&box, index + 1, Transform3D(Basis(), position));
		instance.world_aabb = AABB(position - Vector3(0.5f, 0.5f, 0.5f), Vector3(1.0f, 1.0f, 1.0f));
		instances.push_back(instance);
	}
	LocalVector<uint32_t> kept;
	uint32_t overlapping = 0;
	lighting.select_brick_instances(0, brick, instances.ptr(), instances.size(), kept, overlapping);
	CHECK_EQ(overlapping, count);
	CHECK_EQ(kept.size(), NextGIVoxelLighting::MAX_TRACE_INSTANCES);
	CHECK_EQ(kept[0], count - 1);
	// The farthest eight (largest offsets) are the ones dropped.
	for (uint32_t index : kept) {
		const bool near_one = index < NextGIVoxelLighting::MAX_TRACE_INSTANCES - 1 || index == count - 1;
		CHECK(near_one);
	}
}

TEST_CASE("[NextGI] Voxel lighting stores hits in instance-local space under non-uniform scale") {
	const MeshSDFData box = _unit_box_sdf();
	const Transform3D stretched(Basis::from_scale(Vector3(4.0f, 1.0f, 1.0f)), Vector3());
	const NextGIVoxelLighting::TraceInstance instance = _instance(&box, 9, stretched);
	NextGIVoxelLighting::AxisHit slots[NextGIVoxelLighting::DIRECTIONS];
	// Above the +Y face at world x = 1.5, which is local x = 0.375.
	NextGIVoxelLighting::trace_voxel(&instance, 1, Vector3(1.5f, 1.0f, 0.1f), 2.0f, slots);
	const NextGIVoxelLighting::AxisHit &hit = slots[4];
	REQUIRE(hit.hit);
	CHECK(Math::abs(hit.position.y - 0.5f) < 0.02f);
	CHECK(Math::abs(hit.object_position.x - 0.375f) < 0.01f);
	CHECK(Math::abs(hit.object_position.y - 0.5f) < 0.02f);
	CHECK(hit.object_normal.y > 0.9f);
	CHECK(hit.normal.y > 0.9f);
	// The +Y card of the unit box in its own space: u along local X, half extent 0.5.
	NextGIVoxelLighting::CardQuery card = _card(Vector3(0.0f, 0.5f + 0.01f, 0.0f), Vector3(1.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f), Vector3(0.0f, 1.0f, 0.0f), 0.5f, 0.5f, 1.0f / 128.0f);
	card.depth_range = 1.02f;
	const NextGIVoxelLighting::CardChoice choice = NextGIVoxelLighting::choose_card(&card, 1, hit.object_position, hit.object_normal);
	CHECK_FALSE(choice.missing);
	CHECK(Math::abs(choice.u - 0.875f) < 0.02f);
	CHECK(Math::abs(choice.v - 0.6f) < 0.02f);
	// The far +X face of the stretched box lies at world x = 2, local x = 0.5.
	NextGIVoxelLighting::trace_voxel(&instance, 1, Vector3(2.6f, 0.0f, 0.1f), 2.0f, slots);
	REQUIRE(slots[3].hit);
	CHECK(Math::abs(slots[3].position.x - 2.0f) < 0.05f);
	CHECK(Math::abs(slots[3].object_position.x - 0.5f) < 0.02f);
}

TEST_CASE("[NextGI] Voxel lighting bricks cover four lighting voxels") {
	Vector3i voxels[64];
	const uint32_t first = NextGIVoxelLighting::voxels_for_sdf_brick(Vector3i(0, 0, 0), voxels, 64);
	CHECK_EQ(first, 64u);
	CHECK_EQ(voxels[0], Vector3i(0, 0, 0));
	const uint32_t second = NextGIVoxelLighting::voxels_for_sdf_brick(Vector3i(1, 0, 0), voxels, 64);
	CHECK_EQ(second, 64u);
	CHECK_EQ(voxels[0].x, 4);

	CHECK_EQ(NextGIVoxelLighting::pack_r11g11b10(Color(0.0f, 0.0f, 0.0f)), 0u);
	const Color white = NextGIVoxelLighting::unpack_r11g11b10(NextGIVoxelLighting::pack_r11g11b10(Color(1.0f, 1.0f, 1.0f)));
	CHECK(Math::is_equal_approx(white.r, 1.0f));
	CHECK(Math::is_equal_approx(white.g, 1.0f));
	CHECK(Math::is_equal_approx(white.b, 1.0f));

	// CPU twin of nextgi_voxel_pack_r11g11b10. 40000 sits one 512-wide step above 32768 and must not clamp.
	const Color hi = NextGIVoxelLighting::unpack_r11g11b10(NextGIVoxelLighting::pack_r11g11b10(Color(40000.0f, 40000.0f, 40000.0f)));
	CHECK(hi.r < NextGIVoxelLighting::R11G11B10_MAX_11);
	CHECK(Math::abs(hi.r - 40000.0f) <= 512.0f);
	CHECK(Math::abs(hi.g - 40000.0f) <= 512.0f);
	const Color just_under = NextGIVoxelLighting::unpack_r11g11b10(NextGIVoxelLighting::pack_r11g11b10(Color(32767.9f, 32767.9f, 32767.9f)));
	CHECK(just_under.r < NextGIVoxelLighting::R11G11B10_MAX_11);
	CHECK(just_under.r > 30000.0f);
	const Color at_exp = NextGIVoxelLighting::unpack_r11g11b10(NextGIVoxelLighting::pack_r11g11b10(Color(32768.0f, 32768.0f, 32768.0f)));
	CHECK(Math::is_equal_approx(at_exp.r, 32768.0f));
	CHECK(Math::is_equal_approx(at_exp.g, 32768.0f));
	const Color denorm = NextGIVoxelLighting::unpack_r11g11b10(NextGIVoxelLighting::pack_r11g11b10(Color(1.0e-5f, 1.0e-5f, 1.0e-5f)));
	CHECK(denorm.r > 0.0f);
	CHECK(denorm.r < 1.0e-4f);
	const float powers[5] = { 1.0f, 2.0f, 0.5f, 256.0f, 1024.0f };
	for (float value : powers) {
		const Color decoded = NextGIVoxelLighting::unpack_r11g11b10(NextGIVoxelLighting::pack_r11g11b10(Color(value, value, value)));
		CHECK(Math::is_equal_approx(decoded.r, value));
		CHECK(Math::is_equal_approx(decoded.g, value));
		CHECK(Math::is_equal_approx(decoded.b, value));
	}
	// Halfway mantissa bit rounds up: 1 + 2^-7 becomes 1 + 1/64.
	const Color halfway = NextGIVoxelLighting::unpack_r11g11b10(NextGIVoxelLighting::pack_r11g11b10(Color(1.0f + (1.0f / 128.0f), 1.0f, 1.0f)));
	CHECK(Math::is_equal_approx(halfway.r, 1.0f + (1.0f / 64.0f)));
	// A denormal that rounds past the max mantissa stays denormal.
	const float denorm_scale = std::ldexp(1.0f, 1 - NextGIVoxelLighting::R11G11B10_BIAS - 6);
	const Color denorm_clamp = NextGIVoxelLighting::unpack_r11g11b10(NextGIVoxelLighting::pack_r11g11b10(Color(63.5f * denorm_scale, 0.0f, 0.0f)));
	CHECK(denorm_clamp.r > 0.0f);
	CHECK(denorm_clamp.r < std::ldexp(1.0f, 1 - NextGIVoxelLighting::R11G11B10_BIAS));
	const Color huge = NextGIVoxelLighting::unpack_r11g11b10(NextGIVoxelLighting::pack_r11g11b10(Color(1.0e20f, 1.0e20f, 1.0e20f)));
	CHECK(Math::is_equal_approx(huge.r, NextGIVoxelLighting::R11G11B10_MAX_11));
	CHECK(Math::is_equal_approx(huge.g, NextGIVoxelLighting::R11G11B10_MAX_11));
	CHECK(Math::is_equal_approx(huge.b, 64512.0f));

	// Hit normals reach the GPU as two unorm16 octahedral components.
	const Vector3 normals[4] = { Vector3(0.0f, 0.0f, 1.0f), Vector3(0.0f, 0.0f, -1.0f), Vector3(1.0f, -2.0f, 0.5f).normalized(), Vector3(-0.3f, 0.1f, -0.9f).normalized() };
	for (const Vector3 &normal : normals) {
		CHECK(NextGIVoxelLighting::unpack_normal(NextGIVoxelLighting::pack_normal(normal)).dot(normal) > 0.9999f);
	}
}

TEST_CASE("[NextGI] Voxel lighting card choice applies the D6 material-depth test") {
	// Two cards seeing the same point. Card 1 is better aligned but its captured depth is a
	// surface 3 texels nearer, so the point is hidden from it: D6 picks card 0.
	const float texel = 0.01f;
	const float near_depth[1] = { 0.5f };
	const float far_depth[1] = { 0.47f };
	NextGIVoxelLighting::CardQuery cards[2];
	cards[0] = _card(Vector3(0.0f, 0.0f, 0.5f), Vector3(1.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f), 1.0f, 1.0f, texel);
	cards[0].depth = near_depth;
	cards[0].depth_width = 1;
	cards[0].depth_height = 1;
	const Vector3 tilted = Vector3(0.1f, 0.0f, 1.0f).normalized();
	cards[1] = _card(tilted * 0.5f, Vector3(0.0f, 1.0f, 0.0f).cross(tilted).normalized(), Vector3(0.0f, 1.0f, 0.0f), tilted, 1.0f, 1.0f, texel);
	cards[1].depth = far_depth;
	cards[1].depth_width = 1;
	cards[1].depth_height = 1;
	// The point is 50 texels below card 0's plane, like most of a curved or stepped mesh, and
	// still matches card 0's captured depth.
	const NextGIVoxelLighting::CardChoice hidden = NextGIVoxelLighting::choose_card(cards, 2, Vector3(), tilted);
	CHECK_FALSE(hidden.missing);
	CHECK_EQ(hidden.card, 0u);
	CHECK(hidden.depth_error <= NextGIVoxelLighting::CARD_DEPTH_TEXELS);
	CHECK(Math::is_equal_approx(hidden.u, 0.5f));
	// With a matching capture the better-aligned card 1 wins.
	cards[1].depth = near_depth;
	CHECK_EQ(NextGIVoxelLighting::choose_card(cards, 2, Vector3(), tilted).card, 1u);
	cards[1].depth = far_depth;

	// An uncovered texel (depth 0) never supplies radiance.
	const float uncovered[1] = { 0.0f };
	cards[0].depth = uncovered;
	CHECK(NextGIVoxelLighting::choose_card(cards, 1, Vector3(), Vector3(0.0f, 0.0f, 1.0f)).missing);
	// Outside the card rect.
	cards[0].depth = near_depth;
	CHECK(NextGIVoxelLighting::choose_card(cards, 1, Vector3(1.5f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f)).missing);

	// Without captured depth the capture volume stands in.
	NextGIVoxelLighting::CardQuery volume = _card(Vector3(0.0f, 0.0f, 1.0f), Vector3(1.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f), 1.0f, 1.0f, texel);
	volume.depth_range = 2.0f;
	CHECK_FALSE(NextGIVoxelLighting::choose_card(&volume, 1, Vector3(), Vector3(0.0f, 0.0f, 1.0f)).missing);
	CHECK(NextGIVoxelLighting::choose_card(&volume, 1, Vector3(0.0f, 0.0f, -1.5f), Vector3(0.0f, 0.0f, 1.0f)).missing);
}

TEST_CASE("[NextGI] Voxel lighting D6 covers a curved mesh with production cards") {
	// A sphere clustered into SRC cards at the production texel size (max_extent / 128).
	const float radius = 0.8f;
	Ref<SphereMesh> sphere;
	sphere.instantiate();
	sphere->set_radius(radius);
	sphere->set_height(radius * 2.0f);
	const Vector<Face3> faces = sphere->get_faces();
	LocalVector<float> positions;
	LocalVector<uint32_t> indices;
	for (int face = 0; face < faces.size(); face++) {
		for (int corner = 0; corner < 3; corner++) {
			const Vector3 vertex = faces[face].vertex[corner];
			indices.push_back(positions.size() / 3);
			positions.push_back(vertex.x);
			positions.push_back(vertex.y);
			positions.push_back(vertex.z);
		}
	}
	RendererRD::NextGICardClusterResult clustered;
	RendererRD::nextgi_src_cluster_cards(positions.ptr(), positions.size() / 3, indices.ptr(), indices.size(), clustered);
	REQUIRE(clustered.cards.size() > 0);
	REQUIRE(clustered.cards.size() <= NextGIVoxelLighting::MAX_CARDS);
	CHECK(Math::is_equal_approx(clustered.texel_size, clustered.max_extent / 128.0f));
	LocalVector<LocalVector<float>> depths;
	NextGIVoxelLighting::CardQuery cards[NextGIVoxelLighting::MAX_CARDS];
	for (uint32_t index = 0; index < clustered.cards.size(); index++) {
		depths.push_back(_sphere_card_depth(clustered.cards[index], radius));
	}
	for (uint32_t index = 0; index < clustered.cards.size(); index++) {
		const RendererRD::NextGICardDesc &desc = clustered.cards[index];
		cards[index] = _card(desc.origin, desc.axis_u, desc.axis_v, desc.axis_n, desc.half_u, desc.half_v, clustered.texel_size);
		cards[index].depth_range = desc.depth_range;
		cards[index].depth = depths[index].ptr();
		cards[index].depth_width = desc.texels_u;
		cards[index].depth_height = desc.texels_v;
	}
	// Fibonacci points over the whole sphere. Every visible surface point has a card under D6;
	// a test against the card plane alone would accept only each cluster's outermost texel.
	const uint32_t samples = 2000;
	uint32_t covered = 0;
	for (uint32_t index = 0; index < samples; index++) {
		const float y = 1.0f - 2.0f * (float(index) + 0.5f) / float(samples);
		const float ring = Math::sqrt(MAX(0.0f, 1.0f - y * y));
		const float angle = float(index) * 2.39996323f;
		const Vector3 normal(Math::cos(angle) * ring, y, Math::sin(angle) * ring);
		const NextGIVoxelLighting::CardChoice choice = NextGIVoxelLighting::choose_card(cards, clustered.cards.size(), normal * radius, normal);
		if (!choice.missing && NextGIVoxelLighting::shade_weight(false, choice.projection, choice.depth_error) >= NextGIVoxelLighting::WEIGHT_FALLBACK) {
			covered++;
		}
	}
	CHECK(float(covered) / float(samples) >= 0.95f);
}

TEST_CASE("[NextGI] Voxel lighting drops ghost hits when the instance leaves") {
	const MeshSDFData box = _unit_box_sdf();
	const NextGIVoxelLighting::TraceInstance floor = _instance(&box, 7, Transform3D());
	NextGIVoxelLighting lighting;
	lighting.set_clipmap_origins_from_camera(Vector3());
	const Vector3i origin = NextGIGlobalSDF::scrolled_origin(Vector3(), 0);
	const Vector3i above = _local_voxel(0, origin, Vector3(0.05f, 0.75f, 0.05f));
	const Vector3 center = NextGIVoxelLighting::voxel_center(0, origin, above);
	const AABB volume(center - Vector3(0.01f, 0.01f, 0.01f), Vector3(0.02f, 0.02f, 0.02f));
	lighting.enqueue_dirty_volumes(&volume, 1);
	uint32_t first = 0;
	_drain(lighting, &floor, 1, first);
	lighting.shade(&floor, 1);
	CHECK_GT(lighting.get_stored_hits(), 0u);
	CHECK_EQ(lighting.get_shaded_hits(), lighting.get_stored_hits());
	// The voxel above the box stores its top face in slot 4 (traced along -Y), facing +Y.
	NextGIVoxelLighting::AxisHit top;
	REQUIRE(lighting.get_stored_hit(0, above, 4, top));
	CHECK(Math::abs(top.position.y - 0.5f) < 0.02f);
	CHECK(top.normal.y > 0.9f);

	lighting.enqueue_dirty_volumes(&volume, 1);
	_drain(lighting, nullptr, 0, first);
	lighting.shade(nullptr, 0);
	CHECK_EQ(lighting.get_stored_hits(), 0u);
	CHECK_EQ(lighting.get_shaded_hits(), 0u);
	CHECK_FALSE(lighting.get_stored_hit(0, above, 4, top));
}
