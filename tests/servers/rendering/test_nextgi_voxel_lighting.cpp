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
#include "servers/rendering/renderer_rd/environment/next_gi_voxel_lighting.h"

#include <cmath>

namespace {

static MeshSDFData _unit_box_sdf() {
	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size(Vector3(1.0f, 1.0f, 1.0f));
	MeshSDFSettings settings;
	settings.voxels_per_meter = 8.0f;
	settings.max_voxels_per_axis = 32;
	settings.prefer_static_raycaster = false;
	MeshSDFData data;
	const Error err = MeshSDFBuilder::build(box->get_faces(), settings, data);
	CHECK(err == OK);
	CHECK(data.is_valid());
	return data;
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

static NextGIVoxelLighting::TraceInstance _box_instance(const MeshSDFData *p_field, uint32_t p_id, uint32_t p_card_count) {
	NextGIVoxelLighting::TraceInstance instance;
	instance.id = p_id;
	instance.field = p_field;
	instance.inverse = Transform3D();
	instance.world_scale = 1.0f;
	instance.world_aabb = AABB(Vector3(-0.5f, -0.5f, -0.5f), Vector3(1.0f, 1.0f, 1.0f));
	instance.page_table = 0;
	instance.card_count = p_card_count;
	if (p_card_count > 0) {
		instance.cards[0] = _card(Vector3(-0.5f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f), Vector3(-1.0f, 0.0f, 0.0f), 0.6f, 0.6f, 0.1f);
	}
	return instance;
}

static uint32_t _drain(NextGIVoxelLighting &p_lighting, const NextGIVoxelLighting::TraceInstance *p_instances, uint32_t p_count, uint32_t &r_first) {
	const uint32_t pending = p_lighting.pending_bricks();
	const uint32_t brick_rays = NextGIVoxelLighting::SDF_BRICK_VOXELS * NextGIVoxelLighting::SDF_BRICK_VOXELS * NextGIVoxelLighting::SDF_BRICK_VOXELS * NextGIVoxelLighting::DIRECTIONS;
	uint32_t total = 0;
	r_first = 0;
	uint32_t calls = 0;
	while (p_lighting.pending_bricks() > 0 && calls < 200) {
		const uint32_t rays = p_lighting.retrace(p_instances, p_count);
		if (calls == 0) {
			r_first = rays;
		}
		total += rays;
		calls++;
	}
	CHECK_EQ(p_lighting.pending_bricks(), 0u);
	CHECK_EQ(total, pending * brick_rays);
	if (pending > 4u) {
		CHECK_EQ(r_first, NextGIVoxelLighting::RAYS_PER_FRAME);
	}
	return total;
}

static void _fill_axis(NextGIVoxelLighting::DirectionSample *r_samples, uint32_t p_direction, const Color &p_radiance, float p_weight) {
	r_samples[p_direction].radiance = p_radiance;
	r_samples[p_direction].weight = p_weight;
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

	// A shift that is not a whole brick drops that level and leaves the others queued.
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
}

TEST_CASE("[NextGI] Voxel lighting traces a mesh SDF onto a card") {
	const MeshSDFData box = _unit_box_sdf();
	NextGIVoxelLighting::TraceInstance with_card = _box_instance(&box, 7, 1);
	const NextGIVoxelLighting::AxisHit hit = NextGIVoxelLighting::trace_axis(&with_card, 1, Vector3(-1.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f), 2.0f);
	CHECK(hit.hit);
	CHECK_EQ(hit.instance, 7u);
	CHECK_EQ(hit.card, 0u);
	CHECK_EQ(hit.missing, 0u);
	CHECK(hit.normal.x < -0.5f);
	CHECK(hit.projection > 0.5f);
	CHECK(hit.card_u >= 0.0f);
	CHECK(hit.card_u <= 1.0f);
	CHECK(hit.card_v >= 0.0f);
	CHECK(hit.card_v <= 1.0f);
	uint32_t instance = 0;
	uint32_t card = 0;
	uint32_t hit_t = 0;
	NextGIVoxelLighting::unpack_visibility(hit.packed, instance, card, hit_t);
	CHECK_EQ(instance, 7u);
	CHECK_EQ(card, 0u);
	CHECK(hit_t <= NextGIVoxelLighting::MAX_HIT_T);

	NextGIVoxelLighting::TraceInstance no_card = _box_instance(&box, 7, 0);
	const NextGIVoxelLighting::AxisHit missed = NextGIVoxelLighting::trace_axis(&no_card, 1, Vector3(-1.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f), 2.0f);
	CHECK(missed.hit);
	CHECK_EQ(missed.card, NextGIVoxelLighting::NO_CARD);
	CHECK_EQ(missed.missing, 1u);
	CHECK(Math::is_equal_approx(NextGIVoxelLighting::shade_weight(missed.missing != 0, missed.projection, missed.depth_error), 0.0f));
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

	// Shared production pack. 40000 sits one 512-wide step above 32768 and must not clamp.
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
}

TEST_CASE("[NextGI] Voxel lighting card choice uses containment and depth") {
	NextGIVoxelLighting::CardQuery cards[2];
	cards[0] = _card(Vector3(5.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f), Vector3(1.0f, 0.0f, 0.0f), 0.25f, 0.25f, 0.1f);
	const Vector3 second_n = Vector3(0.9f, 0.1f, 0.0f).normalized();
	cards[1] = _card(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f), second_n, 1.0f, 1.0f, 0.1f);
	const NextGIVoxelLighting::CardChoice contained = NextGIVoxelLighting::choose_card(cards, 2, Vector3(), Vector3(1.0f, 0.0f, 0.0f));
	CHECK_FALSE(contained.missing);
	CHECK_EQ(contained.card, 1u);
	CHECK(contained.u >= 0.0f);
	CHECK(contained.u <= 1.0f);

	cards[0] = _card(Vector3(0.0f, 0.0f, 1.0f), Vector3(1.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f), 2.0f, 2.0f, 0.1f);
	cards[1] = _card(Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f), 1.0f, 1.0f, 0.1f);
	const NextGIVoxelLighting::CardChoice depth = NextGIVoxelLighting::choose_card(cards, 2, Vector3(), Vector3(0.0f, 0.0f, 1.0f));
	CHECK_EQ(depth.card, 1u);
	const NextGIVoxelLighting::CardChoice rejected = NextGIVoxelLighting::choose_card(cards, 1, Vector3(), Vector3(0.0f, 0.0f, 1.0f));
	CHECK(rejected.missing);
}

TEST_CASE("[NextGI] Voxel lighting looks up a floor from above and drops ghost hits") {
	const MeshSDFData box = _unit_box_sdf();
	NextGIVoxelLighting::TraceInstance floor = _box_instance(&box, 7, 1);
	floor.cards[0] = _card(Vector3(0.0f, 0.5f, 0.0f), Vector3(1.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f), Vector3(0.0f, 1.0f, 0.0f), 0.6f, 0.6f, 0.1f);
	floor.page_table = 0;
	floor.debug_radiance = Color(0.0f, 1.0f, 0.0f);

	NextGIVoxelLighting lighting;
	lighting.set_clipmap_origins_from_camera(Vector3());
	const Vector3i origin = NextGIGlobalSDF::scrolled_origin(Vector3(), 0);
	const Vector3 center = NextGIVoxelLighting::voxel_center(0, origin, Vector3i(32, 35, 32));
	const AABB volume(center - Vector3(0.01f, 0.01f, 0.01f), Vector3(0.02f, 0.02f, 0.02f));
	lighting.enqueue_dirty_volumes(&volume, 1);
	uint32_t first = 0;
	_drain(lighting, &floor, 1, first);
	lighting.shade(&floor, 1);
	CHECK_GT(lighting.get_stored_hits(), 0u);

	const Color wrc(0.2f, 0.1f, 0.4f);
	const NextGIVoxelLighting::LookupResult above = lighting.lookup_at(center, Vector3(0.0f, 1.0f, 0.0f), wrc);
	CHECK_FALSE(above.used_wrc);
	CHECK(above.weight >= NextGIVoxelLighting::WEIGHT_FALLBACK);
	CHECK(above.radiance.is_equal_approx(Color(0.0f, 1.0f, 0.0f)));
	const NextGIVoxelLighting::LookupResult wrong_side = lighting.lookup_at(center, Vector3(0.0f, -1.0f, 0.0f), wrc);
	CHECK(wrong_side.used_wrc);
	CHECK(wrong_side.radiance.is_equal_approx(wrc));

	floor.page_table = NextGIVoxelLighting::EMPTY_VISIBILITY;
	lighting.shade(&floor, 1);
	const NextGIVoxelLighting::LookupResult unpublished = lighting.lookup_at(center, Vector3(0.0f, 1.0f, 0.0f), wrc);
	CHECK(unpublished.used_wrc);

	floor.page_table = 0;
	lighting.shade(&floor, 1);
	const NextGIVoxelLighting::LookupResult published = lighting.lookup_at(center, Vector3(0.0f, 1.0f, 0.0f), wrc);
	CHECK_FALSE(published.used_wrc);
	CHECK(published.radiance.is_equal_approx(Color(0.0f, 1.0f, 0.0f)));

	floor.card_count = 0;
	lighting.shade(&floor, 1);
	CHECK(lighting.lookup_at(center, Vector3(0.0f, 1.0f, 0.0f), wrc).used_wrc);
	floor.card_count = 1;
	lighting.shade(&floor, 1);
	CHECK_FALSE(lighting.lookup_at(center, Vector3(0.0f, 1.0f, 0.0f), wrc).used_wrc);

	lighting.enqueue_dirty_volumes(&volume, 1);
	_drain(lighting, nullptr, 0, first);
	lighting.shade(nullptr, 0);
	CHECK_EQ(lighting.get_stored_hits(), 0u);
	CHECK_EQ(lighting.get_shaded_hits(), 0u);
	const NextGIVoxelLighting::LookupResult ghost = lighting.lookup_at(center, Vector3(0.0f, 1.0f, 0.0f), wrc);
	CHECK(ghost.used_wrc);
	CHECK(ghost.radiance.is_equal_approx(wrc));
}
