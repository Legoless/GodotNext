/**************************************************************************/
/*  test_nextgi_shadow_policy.cpp                                         */
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

// GodotNext: nextgi_shadow_policy (#145). The device-free RT visibility policy: the caster-mask allocation and its
// hysteresis, the TLAS instance masks, the light words, a model of trace_shadow() against the raster caster rule
// over the 20 layers x 8 lights matrix, and GM-4's ray builder.

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_nextgi_shadow_policy)

#include "servers/rendering/renderer_rd/environment/next_gi_hit_materials.h"
#include "servers/rendering/renderer_rd/environment/next_gi_relight.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_scene.h"
#include "servers/rendering/renderer_rd/environment/next_gi_shadow_policy.h"

using namespace RendererRD;

namespace TestNextGIShadowPolicy {

typedef NextGIShadowMaskAllocator::Allocation Allocation;

static Allocation _allocation(const uint32_t *p_masks, uint32_t p_count) {
	NextGIShadowMaskAllocator allocator;
	allocator.update(p_masks, p_count);
	return allocator.get_allocation();
}

// The RT Shadow Yard's lights: 44 omnis and the sun cast every layer; matrix light i casts layer i only.
static LocalVector<uint32_t> _yard_masks() {
	LocalVector<uint32_t> masks;
	for (int i = 0; i < 45; i++) {
		masks.push_back(0xFFFFFFFFu);
	}
	for (int i = 0; i < 20; i++) {
		masks.push_back(1u << i);
	}
	return masks;
}

TEST_CASE("[NextGIShadow] The six most frequent caster masks, layers only, ties by the lower mask") {
	const LocalVector<uint32_t> yard = _yard_masks();
	LocalVector<NextGIShadowMaskCount> top;
	nextgi_shadow_top_masks(yard.ptr(), yard.size(), top);
	REQUIRE_EQ(top.size(), 6u);
	CHECK_EQ(top[0].mask, 0xFFFFFu); // Godot's default 0xFFFFFFFF, masked to the 20 layers.
	CHECK_EQ(top[0].count, 45u);
	for (uint32_t i = 1; i < 6; i++) {
		CHECK_EQ(top[i].mask, 1u << (i - 1));
		CHECK_EQ(top[i].count, 1u);
	}
	// Masks that differ only above the 20 layers are one mask; a mask with no layer is left out.
	const uint32_t masks[5] = { 0x00100001u, 0x00000001u, 0xFFF00000u, 0u, 0x2u };
	nextgi_shadow_top_masks(masks, 5, top);
	REQUIRE_EQ(top.size(), 2u);
	CHECK_EQ(top[0].mask, 0x1u);
	CHECK_EQ(top[0].count, 2u);
	CHECK_EQ(top[1].mask, 0x2u);
}

TEST_CASE("[NextGIShadow] The allocation changes only after 60 stable frames, and kept masks keep their bits") {
	NextGIShadowMaskAllocator allocator;
	const uint32_t none = 0;
	CHECK_FALSE(allocator.update(&none, 0));
	CHECK_EQ(allocator.get_allocation().count(), 0u);

	// The first choice applies at once.
	LocalVector<uint32_t> yard = _yard_masks();
	CHECK(allocator.update(yard.ptr(), yard.size()));
	const Allocation first = allocator.get_allocation();
	CHECK_EQ(first.count(), 6u);
	CHECK_EQ(first.find(0xFFFFFu), 0);
	CHECK_EQ(first.find(1u << 4), 5);
	CHECK_EQ(first.find(1u << 5), -1);
	const uint64_t generation = first.generation;

	// Three matrix lights switch to a multi-layer mask: it becomes the second most frequent, and layer 4 drops
	// out of the six. The allocation holds for 59 frames, then flips on the 60th.
	for (int i = 5; i < 8; i++) {
		yard[45 + i] = 0x0F0F0u;
	}
	for (int frame = 1; frame < NEXTGI_SHADOW_HYSTERESIS_FRAMES; frame++) {
		CHECK_FALSE(allocator.update(yard.ptr(), yard.size()));
		CHECK_EQ(allocator.get_allocation().generation, generation);
		CHECK_EQ(allocator.get_stable_frames(), uint32_t(frame));
	}
	CHECK(allocator.update(yard.ptr(), yard.size()));
	const Allocation second = allocator.get_allocation();
	CHECK_EQ(second.generation, generation + 1);
	// Masks in both choices keep their bits; the new mask takes the bit layer 4 freed.
	for (int layer = 0; layer < 4; layer++) {
		CHECK_EQ(second.find(1u << layer), first.find(1u << layer));
	}
	CHECK_EQ(second.find(0xFFFFFu), 0);
	CHECK_EQ(second.find(0x0F0F0u), 5);
	CHECK_EQ(second.find(1u << 4), -1);

	// A choice that flaps back before 60 frames never applies, and the count starts again.
	LocalVector<uint32_t> other = _yard_masks();
	for (int frame = 0; frame < 30; frame++) {
		CHECK_FALSE(allocator.update(other.ptr(), other.size()));
	}
	CHECK_FALSE(allocator.update(yard.ptr(), yard.size()));
	CHECK_EQ(allocator.get_stable_frames(), 0u);
	for (int frame = 0; frame < 30; frame++) {
		CHECK_FALSE(allocator.update(other.ptr(), other.size()));
	}
	CHECK_EQ(allocator.get_allocation().generation, generation + 1);
	CHECK_EQ(allocator.get_stable_frames(), 30u);

	// Lights going away count as a change too, and apply after 60 frames.
	for (int frame = 1; frame < NEXTGI_SHADOW_HYSTERESIS_FRAMES; frame++) {
		CHECK_FALSE(allocator.update(&none, 0));
	}
	CHECK(allocator.update(&none, 0));
	CHECK_EQ(allocator.get_allocation().count(), 0u);
}

TEST_CASE("[NextGIShadow] Instance masks: allocated bits on every caster, bit 6 outside them, bit 7 for GI") {
	const uint32_t masks[3] = { 0x1u, 0x6u, 0xF0000u };
	const Allocation allocation = _allocation(masks, 3);
	REQUIRE_EQ(allocation.count(), 3u);
	const int bit_1 = allocation.find(0x1u);
	const int bit_6 = allocation.find(0x6u);
	const int bit_f = allocation.find(0xF0000u);
	// A caster on layers 1 and 2 meets 0x1 and 0x6; on layer 3 only 0x6.
	CHECK_EQ(nextgi_shadow_instance_mask(allocation, 0x3u, true), (1u << bit_1) | (1u << bit_6) | NEXTGI_SHADOW_MASK_GI);
	CHECK_EQ(nextgi_shadow_instance_mask(allocation, 0x4u, false), 1u << bit_6);
	CHECK_EQ(nextgi_shadow_instance_mask(allocation, 0x80000u, true), (1u << bit_f) | NEXTGI_SHADOW_MASK_GI);
	// Layers no allocated mask covers: the unallocated bit only, so allocated lights never see it.
	CHECK_EQ(nextgi_shadow_instance_mask(allocation, 0x100u, true), uint32_t(NEXTGI_SHADOW_MASK_UNALLOCATED | NEXTGI_SHADOW_MASK_GI));
	// No shadow (cast_shadow off): GI only. A shadow-only instance that casts nothing has no bit at all.
	CHECK_EQ(nextgi_shadow_instance_mask(allocation, 0u, true), uint32_t(NEXTGI_SHADOW_MASK_GI));
	CHECK_EQ(nextgi_shadow_instance_mask(allocation, 0u, false), 0u);
	// Layers above the 20 do not count.
	CHECK_EQ(nextgi_shadow_instance_mask(allocation, 0x100000u, false), 0u);
}

TEST_CASE("[NextGIShadow] Light words: the fast path, the candidate loop and the fallback") {
	const uint32_t masks[2] = { 0xFFFFFu, 0x10u };
	const Allocation allocation = _allocation(masks, 2);
	const int all_bit = allocation.find(0xFFFFFu);
	const int layer_bit = allocation.find(0x10u);
	// Allocated and no alpha-tested caster with the bit: cull mask only, the opaque fast path.
	CHECK_EQ(nextgi_shadow_light_mode(allocation, 0xFFFFFFFFu, 0u), 1u << all_bit);
	CHECK_EQ(nextgi_shadow_light_mode(allocation, 0x10u, 1u << all_bit), 1u << layer_bit);
	// An alpha-tested caster carries the bit: the candidate loop.
	CHECK_EQ(nextgi_shadow_light_mode(allocation, 0x10u, 1u << layer_bit), (1u << layer_bit) | NEXTGI_SHADOW_MODE_LOOP);
	// Not allocated: every caster bit, every hit a candidate.
	CHECK_EQ(nextgi_shadow_light_mode(allocation, 0x20u, 0u), uint32_t(NEXTGI_SHADOW_MASK_CASTERS) | NEXTGI_SHADOW_MODE_LOOP | NEXTGI_SHADOW_MODE_FALLBACK);
	// No layer: nothing traced, never shadowed.
	CHECK_EQ(nextgi_shadow_light_mode(allocation, 0xFFF00000u, 0xFFu), 0u);
	const NextGIShadowModelHit hit;
	CHECK_FALSE(nextgi_shadow_model_occluded(0u, 0xFFF00000u, &hit, 1));
}

// Every instance shape the TLAS holds: single-layer opaque casters on each of the 20 layers, multi-layer casters,
// a caster that casts no shadow, a translucent caster and alpha-tested casters whose texel is kept or cut.
struct MatrixInstance {
	uint32_t layers = 0;
	bool gi_visible = true;
	NextGIShadowClass shadow_class = NEXTGI_SHADOW_CLASS_OPAQUE;
	bool alpha_kept = true;
};

static LocalVector<MatrixInstance> _matrix_instances() {
	LocalVector<MatrixInstance> instances;
	for (int layer = 0; layer < 20; layer++) {
		MatrixInstance instance;
		instance.layers = 1u << layer;
		instances.push_back(instance);
	}
	const uint32_t multi[4] = { 0x3u, 0x0F0F0u, 0xAAAAAu, 0xFFFFFu };
	for (uint32_t layers : multi) {
		MatrixInstance instance;
		instance.layers = layers;
		instances.push_back(instance);
	}
	MatrixInstance no_shadow;
	no_shadow.layers = 0;
	instances.push_back(no_shadow);
	for (int layer = 0; layer < 20; layer += 3) {
		MatrixInstance glass;
		glass.layers = 1u << layer;
		glass.gi_visible = false;
		glass.shadow_class = NEXTGI_SHADOW_CLASS_TRANSLUCENT;
		instances.push_back(glass);
		MatrixInstance kept;
		kept.layers = 1u << layer;
		kept.shadow_class = NEXTGI_SHADOW_CLASS_ALPHA_TESTED;
		instances.push_back(kept);
		MatrixInstance cut = kept;
		cut.alpha_kept = false;
		cut.gi_visible = false;
		instances.push_back(cut);
	}
	return instances;
}

static NextGIShadowModelHit _hit(const Allocation &p_allocation, const MatrixInstance &p_instance) {
	NextGIShadowModelHit hit;
	hit.layers = p_instance.layers;
	hit.mask = nextgi_shadow_instance_mask(p_allocation, p_instance.layers, p_instance.gi_visible);
	hit.opaque_instance = p_instance.shadow_class != NEXTGI_SHADOW_CLASS_ALPHA_TESTED;
	hit.geometry_class = p_instance.shadow_class;
	hit.alpha_kept = p_instance.alpha_kept;
	return hit;
}

static uint32_t _alpha_bits(const Allocation &p_allocation, const LocalVector<MatrixInstance> &p_instances) {
	uint32_t bits = 0;
	for (const MatrixInstance &instance : p_instances) {
		if (instance.shadow_class == NEXTGI_SHADOW_CLASS_ALPHA_TESTED) {
			bits |= nextgi_shadow_instance_mask(p_allocation, instance.layers, instance.gi_visible);
		}
	}
	return bits;
}

TEST_CASE("[NextGIShadow] The 20 layers x 8 lights matrix agrees with the raster caster rule") {
	uint32_t lights[NEXTGI_SHADOW_CHECK_LIGHTS];
	nextgi_shadow_check_masks(lights);
	const LocalVector<MatrixInstance> instances = _matrix_instances();
	// The yard's allocation, one where the multi-layer masks hold bits, and none at all.
	LocalVector<Allocation> allocations;
	const LocalVector<uint32_t> yard = _yard_masks();
	allocations.push_back(_allocation(yard.ptr(), yard.size()));
	const uint32_t multi[6] = { 0x0F0F0u, 0x55555u, 0xFFFFFu, 0x80u, 0x2u, 0x80000u };
	allocations.push_back(_allocation(multi, 6));
	allocations.push_back(Allocation());
	uint32_t fast = 0;
	uint32_t loop = 0;
	uint32_t fallback = 0;
	uint32_t compared = 0;
	uint32_t disagreements = 0;
	for (const Allocation &allocation : allocations) {
		const uint32_t alpha_bits = _alpha_bits(allocation, instances);
		for (uint32_t light = 0; light < NEXTGI_SHADOW_CHECK_LIGHTS; light++) {
			const uint32_t mode = nextgi_shadow_light_mode(allocation, lights[light], alpha_bits);
			if ((mode & NEXTGI_SHADOW_MODE_FALLBACK) != 0) {
				fallback++;
			} else if ((mode & NEXTGI_SHADOW_MODE_LOOP) != 0) {
				loop++;
			} else {
				fast++;
			}
			// Rays that cross one instance, and rays that cross two in either order.
			for (uint32_t a = 0; a < instances.size(); a++) {
				NextGIShadowModelHit hits[2] = { _hit(allocation, instances[a]) };
				compared++;
				disagreements += nextgi_shadow_model_occluded(mode, lights[light], hits, 1) != nextgi_shadow_reference_occluded(lights[light], hits, 1) ? 1 : 0;
				for (uint32_t b = 0; b < instances.size(); b++) {
					hits[1] = _hit(allocation, instances[b]);
					compared++;
					disagreements += nextgi_shadow_model_occluded(mode, lights[light], hits, 2) != nextgi_shadow_reference_occluded(lights[light], hits, 2) ? 1 : 0;
				}
			}
		}
	}
	CHECK_EQ(disagreements, 0u);
	CHECK_GT(compared, 10000u);
	// All three paths ran.
	CHECK_GT(fast, 0u);
	CHECK_GT(loop, 0u);
	CHECK_GT(fallback, 0u);
}

TEST_CASE("[NextGIShadow] The fallback rejects any number of other casters; the alpha cap counts alpha samples only") {
	const LocalVector<uint32_t> yard = _yard_masks();
	const Allocation allocation = _allocation(yard.ptr(), yard.size());
	const uint32_t light = 1u << 12; // Not allocated in the yard.
	const uint32_t mode = nextgi_shadow_light_mode(allocation, light, 0u);
	REQUIRE((mode & NEXTGI_SHADOW_MODE_FALLBACK) != 0);
	// Nineteen occluders on other layers, then the light's own.
	LocalVector<NextGIShadowModelHit> hits;
	for (int layer = 0; layer < 20; layer++) {
		if (layer == 12) {
			continue;
		}
		MatrixInstance instance;
		instance.layers = 1u << layer;
		hits.push_back(_hit(allocation, instance));
	}
	CHECK_FALSE(nextgi_shadow_model_occluded(mode, light, hits.ptr(), hits.size()));
	MatrixInstance own;
	own.layers = light;
	hits.push_back(_hit(allocation, own));
	CHECK(nextgi_shadow_model_occluded(mode, light, hits.ptr(), hits.size()));

	// Four cut alpha samples, then a kept one: the fifth candidate is past the cap and shadows as opaque.
	MatrixInstance cut;
	cut.layers = 0xFFFFFu;
	cut.shadow_class = NEXTGI_SHADOW_CLASS_ALPHA_TESTED;
	cut.alpha_kept = false;
	const uint32_t alpha_bits = nextgi_shadow_instance_mask(allocation, cut.layers, false);
	const uint32_t all_mode = nextgi_shadow_light_mode(allocation, 0xFFFFFu, alpha_bits);
	REQUIRE((all_mode & NEXTGI_SHADOW_MODE_LOOP) != 0);
	NextGIShadowModelHit four[5] = { _hit(allocation, cut), _hit(allocation, cut), _hit(allocation, cut), _hit(allocation, cut), _hit(allocation, cut) };
	CHECK_FALSE(nextgi_shadow_model_occluded(all_mode, 0xFFFFFu, four, 4));
	CHECK(nextgi_shadow_model_occluded(all_mode, 0xFFFFFu, four, 5));
	// Layer rejections take no sample: on the fallback, four cut samples after many rejections still pass.
	LocalVector<NextGIShadowModelHit> mixed;
	for (uint32_t i = 0; i + 1 < hits.size(); i++) {
		mixed.push_back(hits[i]);
	}
	for (int i = 0; i < 4; i++) {
		mixed.push_back(_hit(allocation, cut));
	}
	CHECK_FALSE(nextgi_shadow_model_occluded(mode, light, mixed.ptr(), mixed.size()));
}

// A box of 12 triangles, as BoxMesh writes it (both windings cross: shadow rays hit either face).
static void _box(LocalVector<float> &r_positions, LocalVector<uint32_t> &r_indices) {
	r_positions.clear();
	r_indices.clear();
	for (int i = 0; i < 8; i++) {
		r_positions.push_back((i & 1) ? 0.5f : -0.5f);
		r_positions.push_back((i & 2) ? 0.5f : -0.5f);
		r_positions.push_back((i & 4) ? 0.5f : -0.5f);
	}
	const uint32_t faces[6][4] = { { 0, 2, 6, 4 }, { 1, 5, 7, 3 }, { 0, 4, 5, 1 }, { 2, 3, 7, 6 }, { 0, 1, 3, 2 }, { 4, 6, 7, 5 } };
	for (const uint32_t *face : faces) {
		r_indices.push_back(face[0]);
		r_indices.push_back(face[1]);
		r_indices.push_back(face[2]);
		r_indices.push_back(face[0]);
		r_indices.push_back(face[2]);
		r_indices.push_back(face[3]);
	}
}

static NextGIShadowCheckInstance _box_instance(const LocalVector<float> &p_positions, const LocalVector<uint32_t> &p_indices, const Vector3 &p_center, const Vector3 &p_size, uint32_t p_layers) {
	NextGIShadowCheckInstance instance;
	instance.transform = Transform3D(Basis::from_scale(p_size), p_center);
	instance.world_bounds = AABB(p_center - p_size * 0.5f, p_size);
	instance.layers = p_layers;
	instance.positions = p_positions.ptr();
	instance.vertex_count = p_positions.size() / 3;
	instance.indices = p_indices.ptr();
	instance.index_count = p_indices.size();
	return instance;
}

TEST_CASE("[NextGIShadow] GM-4's rays: clear answers only, deterministic, both outcomes for every light") {
	LocalVector<float> positions;
	LocalVector<uint32_t> indices;
	_box(positions, indices);
	// The RT Shadow Yard's matrix row: 20 occluders on layers 1 to 20, and the ground on layer 1.
	LocalVector<NextGIShadowCheckInstance> instances;
	for (int layer = 0; layer < 20; layer++) {
		instances.push_back(_box_instance(positions, indices, Vector3(-19.0f + layer * 2.0f, 1.5f, -12.0f), Vector3(1.2f, 3.0f, 1.2f), 1u << layer));
	}
	instances.push_back(_box_instance(positions, indices, Vector3(0.0f, -0.05f, 0.0f), Vector3(120.0f, 0.1f, 120.0f), 1u));

	// One instance: a ray through the middle of a box hits it, one beside it clears it, one that ends inside the
	// margin of its face is ambiguous.
	const NextGIShadowCheckInstance &box = instances[3];
	// (Off the faces' diagonals, which split each face into two triangles.)
	CHECK_EQ(nextgi_shadow_check_instance_hit(box, Vector3(-12.8f, 1.0f, -6.0f), Vector3(0, 0, -1), 10.0f), 1);
	CHECK_EQ(nextgi_shadow_check_instance_hit(box, Vector3(-12.0f, 1.0f, -6.0f), Vector3(0, 0, -1), 10.0f), 0);
	CHECK_EQ(nextgi_shadow_check_instance_hit(box, Vector3(-12.8f, 1.0f, -6.0f), Vector3(0, 0, -1), 5.4f + 0.001f), -1);
	// Along a side face: ambiguous.
	CHECK_EQ(nextgi_shadow_check_instance_hit(box, Vector3(-13.6f, 1.0f, -6.0f), Vector3(0, 0, -1), 10.0f), -1);

	uint32_t masks[NEXTGI_SHADOW_CHECK_LIGHTS];
	nextgi_shadow_check_masks(masks);
	LocalVector<NextGIShadowCheckRay> rays;
	LocalVector<uint8_t> expected;
	REQUIRE_EQ(nextgi_shadow_check_build(instances.ptr(), instances.size(), masks, NEXTGI_SHADOW_CHECK_RAYS, 1, rays, expected), uint32_t(NEXTGI_SHADOW_CHECK_RAYS));
	REQUIRE_EQ(expected.size(), uint32_t(NEXTGI_SHADOW_CHECK_RAYS));
	uint32_t occluded[NEXTGI_SHADOW_CHECK_LIGHTS] = {};
	uint32_t per_light[NEXTGI_SHADOW_CHECK_LIGHTS] = {};
	for (uint32_t i = 0; i < rays.size(); i++) {
		REQUIRE_LT(rays[i].light, uint32_t(NEXTGI_SHADOW_CHECK_LIGHTS));
		per_light[rays[i].light]++;
		occluded[rays[i].light] += expected[i];
		// Recomputed from scratch, each ray's answer is the same and unambiguous.
		const Vector3 origin(rays[i].origin[0], rays[i].origin[1], rays[i].origin[2]);
		const Vector3 direction(rays[i].direction[0], rays[i].direction[1], rays[i].direction[2]);
		if (i % 257 == 0) {
			bool any = false;
			for (const NextGIShadowCheckInstance &instance : instances) {
				const int hit = nextgi_shadow_check_instance_hit(instance, origin, direction, rays[i].tmax);
				REQUIRE_GE(hit, 0);
				any = any || (hit > 0 && (instance.layers & masks[rays[i].light]) != 0);
			}
			CHECK_EQ(uint8_t(any ? 1 : 0), expected[i]);
		}
	}
	for (int light = 0; light < NEXTGI_SHADOW_CHECK_LIGHTS; light++) {
		CHECK_EQ(per_light[light], uint32_t(NEXTGI_SHADOW_CHECK_RAYS / NEXTGI_SHADOW_CHECK_LIGHTS));
		CHECK_GT(occluded[light], 0u);
		CHECK_LT(occluded[light], per_light[light]);
	}
	// The same seed builds the same rays.
	LocalVector<NextGIShadowCheckRay> again;
	LocalVector<uint8_t> again_expected;
	nextgi_shadow_check_build(instances.ptr(), instances.size(), masks, 4096, 1, again, again_expected);
	REQUIRE_EQ(again.size(), 4096u);
	uint32_t same = 0;
	for (uint32_t i = 0; i < again.size(); i++) {
		bool equal = again_expected[i] == expected[i] && again[i].light == rays[i].light && again[i].tmax == rays[i].tmax;
		for (int k = 0; k < 3; k++) {
			equal = equal && again[i].origin[k] == rays[i].origin[k] && again[i].direction[k] == rays[i].direction[k];
		}
		same += equal ? 1 : 0;
	}
	CHECK_EQ(same, 4096u);
}

// BaseMaterial3D's generated lines the bindless-texture path relies on (scene/resources/material.cpp), with p_alpha
// as the transparency line and p_extra anywhere in the fragment.
static String _base_material_code(const String &p_alpha, const String &p_extra = String()) {
	String code = "shader_type spatial;\nuniform vec4 albedo : source_color;\nuniform sampler2D texture_albedo : source_color, filter_linear_mipmap, repeat_enable;\n";
	code += "void vertex() {\n\tUV = UV * uv1_scale.xy + uv1_offset.xy;\n}\n";
	code += "void fragment() {\n\tvec2 base_uv = UV;\n\tvec4 albedo_tex = texture(texture_albedo, base_uv);\n";
	code += p_extra;
	code += "\tALBEDO = albedo.rgb * albedo_tex.rgb;\n\tALPHA *= albedo.a * albedo_tex.a;\n";
	code += p_alpha;
	code += "}\n";
	return code;
}

TEST_CASE("[NextGIShadow] BaseMaterial3D's texture alpha takes the bindless path; any other alpha goes to the retrace") {
	bool hash = true;
	CHECK(nextgi_shadow_code_has_texture_alpha(_base_material_code("\tALPHA_SCISSOR_THRESHOLD = alpha_scissor_threshold;\n"), hash));
	CHECK_FALSE(hash);
	CHECK(nextgi_shadow_code_has_texture_alpha(_base_material_code("\tALPHA_HASH_SCALE = alpha_hash_scale;\n"), hash));
	CHECK(hash);
	// Alpha blending has neither: not alpha-tested.
	CHECK_FALSE(nextgi_shadow_code_has_texture_alpha(_base_material_code(""), hash));
	// Whatever moves the UV or changes the alpha after the albedo texture: the shader decides.
	const char *changes[] = {
		"\tvec4 albedo_tex = triplanar_texture(texture_albedo, uv1_power_normal, uv1_triplanar_pos);\n",
		"\t// Albedo Texture MSDF: Enabled\n\tfloat msdf_size = 1.0;\n",
		"\talbedo_tex *= COLOR;\n",
		"\tfloat depth = texture(texture_heightmap, base_uv).r;\n",
		"\tvec4 point = texture(texture_albedo, POINT_COORD);\n",
		"\tALPHA = 1.0;\n",
		"\tALPHA *= clamp(smoothstep(distance_fade_min, distance_fade_max, length(VERTEX)), 0.0, 1.0);\n",
	};
	for (const char *change : changes) {
		CHECK_FALSE(nextgi_shadow_code_has_texture_alpha(_base_material_code("\tALPHA_SCISSOR_THRESHOLD = alpha_scissor_threshold;\n", change), hash));
	}
	// Particle animation frames move UV in vertex().
	String animated = _base_material_code("\tALPHA_SCISSOR_THRESHOLD = alpha_scissor_threshold;\n").replace("\tUV = UV * uv1_scale.xy + uv1_offset.xy;\n", "\tUV = UV * uv1_scale.xy + uv1_offset.xy;\n\tUV /= vec2(h_frames, v_frames);\n");
	CHECK_FALSE(nextgi_shadow_code_has_texture_alpha(animated, hash));
	// A ShaderMaterial's own alpha scissor.
	const String custom = "shader_type spatial;\nuniform sampler2D noise;\nvoid fragment() {\n\tALPHA = texture(noise, UV).r;\n\tALPHA_SCISSOR_THRESHOLD = 0.5;\n}\n";
	CHECK_FALSE(nextgi_shadow_code_has_texture_alpha(custom, hash));
}

TEST_CASE("[NextGIShadow] The retrace takes rays only from allocated lights, after nothing else occluded them") {
	const LocalVector<uint32_t> yard = _yard_masks();
	const Allocation allocation = _allocation(yard.ptr(), yard.size());
	MatrixInstance shader_alpha;
	shader_alpha.layers = 1u;
	shader_alpha.shadow_class = NEXTGI_SHADOW_CLASS_ALPHA_TESTED;
	shader_alpha.alpha_kept = false; // The material's any-hit cuts the texel.
	NextGIShadowModelHit cut_hit = _hit(allocation, shader_alpha);
	cut_hit.retrace = true;
	NextGIShadowModelHit kept_hit = cut_hit;
	kept_hit.alpha_kept = true;
	const uint32_t alpha_bits = cut_hit.mask;
	const uint32_t allocated = nextgi_shadow_light_mode(allocation, 1u, alpha_bits);
	REQUIRE((allocated & NEXTGI_SHADOW_MODE_LOOP) != 0);
	REQUIRE((allocated & NEXTGI_SHADOW_MODE_FALLBACK) == 0);

	// Deferred: the first pass does not know the shader's alpha; the retrace answers as the any-hit stage does.
	NextGIShadowModelResult result = nextgi_shadow_model_trace(allocated, 1u, &cut_hit, 1, true);
	CHECK_FALSE(result.occluded);
	CHECK(result.deferred);
	CHECK_FALSE(result.retraced_occluded);
	CHECK_EQ(result.alpha_samples, 0u);
	result = nextgi_shadow_model_trace(allocated, 1u, &kept_hit, 1, true);
	CHECK(result.deferred);
	CHECK(result.retraced_occluded);
	// Without records the candidate shadows as opaque and counts as a sample.
	result = nextgi_shadow_model_trace(allocated, 1u, &cut_hit, 1, false);
	CHECK(result.occluded);
	CHECK_FALSE(result.deferred);
	CHECK_EQ(result.alpha_samples, 1u);

	// An opaque caster behind it occludes in the first pass: no record.
	MatrixInstance wall;
	wall.layers = 1u;
	const NextGIShadowModelHit hits[2] = { cut_hit, _hit(allocation, wall) };
	result = nextgi_shadow_model_trace(allocated, 1u, hits, 2, true);
	CHECK(result.occluded);
	CHECK_FALSE(result.deferred);

	// A light without a bit needs the layer check, which the retrace's cull mask cannot do: opaque, never deferred.
	const uint32_t fallback = nextgi_shadow_light_mode(allocation, 1u << 12, alpha_bits);
	REQUIRE((fallback & NEXTGI_SHADOW_MODE_FALLBACK) != 0);
	MatrixInstance other = shader_alpha;
	other.layers = 1u << 12;
	NextGIShadowModelHit fallback_hit = _hit(allocation, other);
	fallback_hit.retrace = true;
	result = nextgi_shadow_model_trace(fallback, 1u << 12, &fallback_hit, 1, true);
	CHECK(result.occluded);
	CHECK_FALSE(result.deferred);

	// The records: a 16-byte header, then three uvec4 per ray.
	CHECK_EQ(nextgi_shadow_retrace_buffer_size(0), 16u);
	CHECK_EQ(nextgi_shadow_retrace_buffer_size(NEXTGI_SHADOW_RETRACE_MAX_RECORDS), 16u + 48u * NEXTGI_SHADOW_RETRACE_MAX_RECORDS);
}

TEST_CASE("[NextGIShadow] The hardware relight's alpha variant adds the alpha path and the retrace records") {
	// A ShaderRD of its own: the baked relight keeps get_shader_variant_defines() unchanged.
	const Vector<String> defines = NextGIRelight::get_shadow_alpha_variant_defines(true);
	REQUIRE_EQ(defines.size(), 1);
	CHECK_EQ(defines[0], NextGIRelight::get_shader_variant_defines(true)[0] + "#define NEXTGI_SHADOW_ALPHA_TEXTURES\n");
	CHECK_EQ(NextGIRelight::get_shadow_alpha_variant_defines(false)[0], NextGIRelight::get_shader_variant_defines(false)[0] + "#define NEXTGI_SHADOW_ALPHA_TEXTURES\n");
	CHECK_EQ(NextGIRelight::get_shader_variant_defines(true).size(), 1);
	const String opaque = NextGIRelight::build_hardware_shader_source(false);
	const String alpha = NextGIRelight::build_hardware_shader_source(true);
	REQUIRE_FALSE(opaque.is_empty());
	REQUIRE_FALSE(alpha.is_empty());
	CHECK(opaque.find("#define NEXTGI_SHADOW_ALPHA_TEXTURES") == -1);
	CHECK(alpha.find("#define NEXTGI_SHADOW_ALPHA_TEXTURES") != -1);
	// Both trace through the shared include; only the alpha variant appends retrace records and reads the texture
	// table, and it takes its retrace turn per visit of a light pair.
	CHECK(opaque.find("float trace_shadow(") != -1);
	CHECK(alpha.find("float trace_shadow(") != -1);
	CHECK(alpha.find("nextgi_shadow_retrace_append(origin, to_light, tmax") != -1);
	CHECK(alpha.find("layout(set = NEXTGI_SHADOW_TEXTURE_SET, binding = 0) uniform texture2D nextgi_shadow_alpha_textures[]") != -1);
	CHECK(alpha.find("uint turn_period = (page.shadowed_count & 1u) != 0u ? page.shadowed_count : page.shadowed_count / 2u;") != -1);
	CHECK(alpha.find("((params.frame / max(turn_period, 1u)) & 1u)") != -1);
}

// The lights of the pair a mask texel traces on frame p_frame, in the order the retrace turn gives them
// (nextgi_src_relight_inc.glsl): the first is the one that gets the frame's record when both need one.
static uint32_t _first_light(uint32_t p_frame, uint32_t p_count, uint32_t p_turn) {
	uint32_t slots[2] = { (p_frame * 2u) % p_count, (p_frame * 2u + 1u) % p_count };
	const uint32_t count = slots[1] != slots[0] ? 2u : 1u;
	return slots[p_turn % count];
}

TEST_CASE("[NextGIShadow] Every shadowed light of a page gets the retrace record in turn") {
	for (uint32_t n = 1; n <= 8; n++) {
		// In every window of 2n frames, each of the n lights goes first at least once.
		for (uint32_t start = 0; start < 4 * n; start++) {
			uint32_t first = 0;
			for (uint32_t frame = start; frame < start + 2 * n; frame++) {
				first |= 1u << _first_light(frame, n, nextgi_shadow_retrace_turn(frame, n));
			}
			CHECK_MESSAGE(first == (1u << n) - 1u, vformat("%d lights, frames %d to %d: first lights 0x%x", n, start, start + 2 * n - 1, first));
		}
		// Each visit of a pair flips which of its lights goes first.
		const uint32_t period = (n & 1u) != 0u ? n : n / 2u;
		if (n > 1) {
			for (uint32_t frame = 0; frame < 4 * n; frame++) {
				CHECK_NE(_first_light(frame, n, nextgi_shadow_retrace_turn(frame, n)), _first_light(frame + period, n, nextgi_shadow_retrace_turn(frame + period, n)));
			}
		}
	}
	// The frame's parity alone, the rule before: with 4 lights, lights 1 and 2 never went first; with 8 lights,
	// lights 1, 2, 5 and 6 never did.
	for (uint32_t n : { 4u, 8u }) {
		uint32_t first = 0;
		for (uint32_t frame = 0; frame < 64; frame++) {
			first |= 1u << _first_light(frame, n, frame & 1u);
		}
		CHECK_EQ(first, n == 4 ? 0x9u : 0x99u);
	}
}

TEST_CASE("[NextGIShadow] A shader the hit groups refuse shadows as opaque and keeps that verdict") {
	// What the hit groups report replaces the kept verdict; while they report nothing (freed, or the shader still
	// compiles), the kept verdict stays: a refused shader never asks for the hit groups again.
	CHECK_EQ(nextgi_shadow_retrace_verdict(NEXTGI_SHADOW_VERDICT_UNKNOWN, NEXTGI_SHADOW_VERDICT_UNKNOWN), NEXTGI_SHADOW_VERDICT_UNKNOWN);
	CHECK_EQ(nextgi_shadow_retrace_verdict(NEXTGI_SHADOW_VERDICT_UNKNOWN, NEXTGI_SHADOW_VERDICT_OPAQUE), NEXTGI_SHADOW_VERDICT_OPAQUE);
	CHECK_EQ(nextgi_shadow_retrace_verdict(NEXTGI_SHADOW_VERDICT_OPAQUE, NEXTGI_SHADOW_VERDICT_UNKNOWN), NEXTGI_SHADOW_VERDICT_OPAQUE);
	CHECK_EQ(nextgi_shadow_retrace_verdict(NEXTGI_SHADOW_VERDICT_RETRACE, NEXTGI_SHADOW_VERDICT_UNKNOWN), NEXTGI_SHADOW_VERDICT_RETRACE);
	CHECK_EQ(nextgi_shadow_retrace_verdict(NEXTGI_SHADOW_VERDICT_UNKNOWN, NEXTGI_SHADOW_VERDICT_RETRACE), NEXTGI_SHADOW_VERDICT_RETRACE);
	// The cycle the kept verdict prevents: refused, hit groups freed after the hold, and the next phase reports nothing.
	NextGIShadowRetraceVerdict kept = NEXTGI_SHADOW_VERDICT_UNKNOWN;
	const NextGIShadowRetraceVerdict reports[] = { NEXTGI_SHADOW_VERDICT_UNKNOWN, NEXTGI_SHADOW_VERDICT_UNKNOWN, NEXTGI_SHADOW_VERDICT_OPAQUE, NEXTGI_SHADOW_VERDICT_UNKNOWN, NEXTGI_SHADOW_VERDICT_UNKNOWN };
	for (const NextGIShadowRetraceVerdict report : reports) {
		kept = nextgi_shadow_retrace_verdict(kept, report);
	}
	CHECK_EQ(kept, NEXTGI_SHADOW_VERDICT_OPAQUE);
}

TEST_CASE("[NextGIShadow] The alpha path follows the albedo texture's filter and repeat") {
	const String linear = "uniform sampler2D texture_albedo : source_color, filter_linear_mipmap, repeat_enable;\n";
	CHECK_EQ(nextgi_shadow_code_albedo_sampler_flags(linear), 0u);
	CHECK_EQ(nextgi_shadow_code_albedo_sampler_flags("uniform sampler2D texture_albedo : source_color, filter_nearest, repeat_enable;\n"), uint32_t(NEXTGI_SHADOW_ALPHA_NEAREST));
	CHECK_EQ(nextgi_shadow_code_albedo_sampler_flags("uniform sampler2D texture_albedo : source_color, filter_nearest_mipmap_anisotropic, repeat_disable;\n"), uint32_t(NEXTGI_SHADOW_ALPHA_NEAREST | NEXTGI_SHADOW_ALPHA_CLAMP));
	CHECK_EQ(nextgi_shadow_code_albedo_sampler_flags("uniform sampler2D texture_albedo : source_color, filter_linear, repeat_disable;\n"), uint32_t(NEXTGI_SHADOW_ALPHA_CLAMP));
	// Only the albedo texture's own declaration counts.
	CHECK_EQ(nextgi_shadow_code_albedo_sampler_flags(linear + "uniform sampler2D texture_roughness : hint_roughness_r, filter_nearest, repeat_disable;\n"), 0u);
	CHECK_EQ(nextgi_shadow_code_albedo_sampler_flags(_base_material_code("\tALPHA_SCISSOR_THRESHOLD = alpha_scissor_threshold;\n")), 0u);
	// The flags fit the alpha table entry's flag word next to the class and the path bits.
	CHECK_EQ((uint32_t(NEXTGI_SHADOW_ALPHA_NEAREST | NEXTGI_SHADOW_ALPHA_CLAMP) & (NEXTGI_SHADOW_ALPHA_CLASS_MASK | NEXTGI_SHADOW_ALPHA_TEXTURE | NEXTGI_SHADOW_ALPHA_RETRACE | NEXTGI_SHADOW_ALPHA_HASH | NEXTGI_SHADOW_ALPHA_CUT)), 0u);
}

// GodotNext: nextgi_skinned_alpha_shadows (#302).
TEST_CASE("[NextGIShadow][Skinned] Attribute stream eligibility and static-UV reuse on posed slots") {
	// Static arena surfaces and bone-driven skinned surfaces both carry #130 attribute streams when enabled;
	// surfaces in neither set (e.g. non-skinned blend-shape-only or virtual-only) do not.
	CHECK_FALSE(nextgi_rt_surface_wants_hit_attributes(false, false));
	CHECK(nextgi_rt_surface_wants_hit_attributes(true, false));
	CHECK(nextgi_rt_surface_wants_hit_attributes(false, true));
	CHECK(nextgi_rt_surface_wants_hit_attributes(true, true));

	// Pack a 4-vertex skinned surface's static UVs, normals and vertex colors into the RT arena in #130 layout
	// (reused by posed skinned slots where skinning only deforms positions and normals).
	Array arrays;
	arrays.resize(RSE::ARRAY_MAX);
	PackedVector3Array normals = { Vector3(0, 1, 0), Vector3(0, 1, 0), Vector3(0, 1, 0), Vector3(0, 1, 0) };
	PackedVector2Array uvs = { Vector2(0.0f, 0.0f), Vector2(1.0f, 0.0f), Vector2(1.0f, 1.0f), Vector2(0.0f, 1.0f) };
	PackedColorArray colors = { Color(1, 1, 1, 1), Color(1, 0.5f, 0.25f, 1), Color(0, 1, 0, 1), Color(0, 0, 1, 1) };
	arrays[RSE::ARRAY_NORMAL] = normals;
	arrays[RSE::ARRAY_TEX_UV] = uvs;
	arrays[RSE::ARRAY_COLOR] = colors;

	NextGIArenaAllocator allocator(4096);
	uint64_t attr_bytes = 0;
	uint32_t chunk = UINT32_MAX;
	uint32_t offset = 0;
	uint32_t size = 0;
	Vector<uint8_t> cpu;
	REQUIRE(nextgi_hit_allocate_surface_stream(arrays, 4, allocator, attr_bytes, chunk, offset, size, cpu));
	CHECK_EQ(chunk, 0u);
	CHECK_EQ(offset, 0u);
	CHECK_EQ(size, ((nextgi_hit_stream_size(4) + NEXTGI_RT_ALIGNMENT - 1u) & ~uint32_t(NEXTGI_RT_ALIGNMENT - 1u)));
	CHECK_EQ(attr_bytes, uint64_t(size));
	REQUIRE_EQ(uint32_t(cpu.size()), nextgi_hit_stream_size(4));

	const NextGIHitStreamHeader &header = *reinterpret_cast<const NextGIHitStreamHeader *>(cpu.ptr());
	const NextGIHitAttributes *verts = reinterpret_cast<const NextGIHitAttributes *>(cpu.ptr() + NEXTGI_HIT_ATTRIBUTE_HEADER);
	for (int i = 0; i < 4; i++) {
		const Vector2 unpacked = nextgi_hit_unpack_uv0(header, verts[i]);
		CHECK(Math::is_equal_approx(unpacked.x, uvs[i].x, 1e-4f));
		CHECK(Math::is_equal_approx(unpacked.y, uvs[i].y, 1e-4f));
	}

	// Patching an attribute-buffer UV region on the skinned mesh updates its shared stream in place.
	const uint64_t format = RSE::ARRAY_FORMAT_VERTEX | RSE::ARRAY_FORMAT_NORMAL | RSE::ARRAY_FORMAT_COLOR | RSE::ARRAY_FORMAT_TEX_UV | RSE::ARRAY_FORMAT_BONES | RSE::ARRAY_FORMAT_WEIGHTS;
	const NextGIHitSourceLayout layout = nextgi_hit_source_layout(format, 4);
	REQUIRE(layout.valid);
	REQUIRE_GE(layout.uv_offset, 0);
	const float new_uv[2] = { 0.25f, 0.75f };
	uint32_t begin = 0;
	uint32_t end = 0;
	REQUIRE(nextgi_hit_patch_stream(layout, 4, true, uint32_t(layout.attribute_stride * 2 + layout.uv_offset), reinterpret_cast<const uint8_t *>(new_uv), sizeof(new_uv), cpu.ptrw(), begin, end));
	const Vector2 patched = nextgi_hit_unpack_uv0(header, verts[2]);
	CHECK(Math::is_equal_approx(patched.x, 0.25f, 1e-4f));
	CHECK(Math::is_equal_approx(patched.y, 0.75f, 1e-4f));

	nextgi_hit_free_surface_stream(allocator, attr_bytes, chunk, offset, size, cpu);
	CHECK_EQ(chunk, UINT32_MAX);
	CHECK_EQ(attr_bytes, 0u);
	CHECK_EQ(allocator.release_trailing_free_chunks(), 0u);
}

TEST_CASE("[NextGIShadow][Skinned] Policy routes alpha-tested and translucent skinned surfaces per D17 while keeping opaque slots unaffected") {
	// 1. Opaque mesh material + opaque effective material: GI-visible, opaque TLAS instance, unaffected.
	{
		const NextGISkinnedShadowPolicy p = nextgi_shadow_skinned_policy(false, NEXTGI_SHADOW_CLASS_OPAQUE, 1u);
		CHECK(p.gi_visible);
		CHECK_FALSE(p.shadow_only);
		CHECK_FALSE(p.skip);
		CHECK_FALSE(p.alpha_tested);
		CHECK_FALSE(p.force_no_opaque);
	}
	// 2. Opaque mesh material + cast_shadows off: still GI-visible, not skipped, opaque.
	{
		const NextGISkinnedShadowPolicy p = nextgi_shadow_skinned_policy(false, NEXTGI_SHADOW_CLASS_OPAQUE, 0u);
		CHECK(p.gi_visible);
		CHECK_FALSE(p.shadow_only);
		CHECK_FALSE(p.skip);
		CHECK_FALSE(p.alpha_tested);
		CHECK_FALSE(p.force_no_opaque);
	}
	// 3. Opaque mesh material + alpha-tested instance override: stays GI-visible (GI forces opacity),
	//    but shadow rays see FORCE_NO_OPAQUE + alpha table entry.
	{
		const NextGISkinnedShadowPolicy p = nextgi_shadow_skinned_policy(false, NEXTGI_SHADOW_CLASS_ALPHA_TESTED, 1u);
		CHECK(p.gi_visible);
		CHECK_FALSE(p.shadow_only);
		CHECK_FALSE(p.skip);
		CHECK(p.alpha_tested);
		CHECK(p.force_no_opaque);
	}
	// 4. Opaque mesh material + alpha-tested override + cast_shadows off: GI-visible, no alpha entry needed.
	{
		const NextGISkinnedShadowPolicy p = nextgi_shadow_skinned_policy(false, NEXTGI_SHADOW_CLASS_ALPHA_TESTED, 0u);
		CHECK(p.gi_visible);
		CHECK_FALSE(p.shadow_only);
		CHECK_FALSE(p.skip);
		CHECK_FALSE(p.alpha_tested);
		CHECK_FALSE(p.force_no_opaque);
	}
	// 5. Alpha-tested mesh material + casts shadows: excluded from GI (D17), shadow-only + FORCE_NO_OPAQUE.
	{
		const NextGISkinnedShadowPolicy p = nextgi_shadow_skinned_policy(true, NEXTGI_SHADOW_CLASS_ALPHA_TESTED, 1u);
		CHECK_FALSE(p.gi_visible);
		CHECK(p.shadow_only);
		CHECK_FALSE(p.skip);
		CHECK(p.alpha_tested);
		CHECK(p.force_no_opaque);
	}
	// 6. Alpha-tested mesh material + cast_shadows off (or layers above 20): neither GI nor shadow rays see it.
	{
		const NextGISkinnedShadowPolicy p0 = nextgi_shadow_skinned_policy(true, NEXTGI_SHADOW_CLASS_ALPHA_TESTED, 0u);
		CHECK_FALSE(p0.gi_visible);
		CHECK_FALSE(p0.shadow_only);
		CHECK(p0.skip);
		CHECK_FALSE(p0.alpha_tested);
		CHECK_FALSE(p0.force_no_opaque);

		const NextGISkinnedShadowPolicy p_hi = nextgi_shadow_skinned_policy(true, NEXTGI_SHADOW_CLASS_ALPHA_TESTED, 1u << 20);
		CHECK(p_hi.skip);
	}
	// 7. Translucent (alpha-blended) mesh material + casts shadows: shadow-only (D17), opaque shadow (LQ6).
	{
		const NextGISkinnedShadowPolicy p = nextgi_shadow_skinned_policy(true, NEXTGI_SHADOW_CLASS_TRANSLUCENT, 1u);
		CHECK_FALSE(p.gi_visible);
		CHECK(p.shadow_only);
		CHECK_FALSE(p.skip);
		CHECK_FALSE(p.alpha_tested);
		CHECK_FALSE(p.force_no_opaque);
	}
	// 8. Alpha mesh material overridden with opaque instance material: stays shadow-only (D17), opaque shadow.
	{
		const NextGISkinnedShadowPolicy p = nextgi_shadow_skinned_policy(true, NEXTGI_SHADOW_CLASS_OPAQUE, 1u);
		CHECK_FALSE(p.gi_visible);
		CHECK(p.shadow_only);
		CHECK_FALSE(p.skip);
		CHECK_FALSE(p.alpha_tested);
		CHECK_FALSE(p.force_no_opaque);
	}

	// Verify TLAS mask GI isolation and shadow trace agreement across cutout, retrace and opaque skinned casters.
	const uint32_t light_masks[2] = { 0xFFFFFu, 1u << 2 };
	const Allocation allocation = _allocation(light_masks, 2);
	const NextGISkinnedShadowPolicy shadow_only_alpha = nextgi_shadow_skinned_policy(true, NEXTGI_SHADOW_CLASS_ALPHA_TESTED, 1u << 2);
	const NextGISkinnedShadowPolicy gi_override_alpha = nextgi_shadow_skinned_policy(false, NEXTGI_SHADOW_CLASS_ALPHA_TESTED, 1u << 2);
	const NextGISkinnedShadowPolicy opaque_skinned = nextgi_shadow_skinned_policy(false, NEXTGI_SHADOW_CLASS_OPAQUE, 1u << 2);

	const uint32_t mask_shadow_only = nextgi_shadow_instance_mask(allocation, 1u << 2, shadow_only_alpha.gi_visible);
	const uint32_t mask_gi_override = nextgi_shadow_instance_mask(allocation, 1u << 2, gi_override_alpha.gi_visible);
	const uint32_t mask_opaque = nextgi_shadow_instance_mask(allocation, 1u << 2, opaque_skinned.gi_visible);
	CHECK_EQ(mask_shadow_only & NEXTGI_SHADOW_MASK_GI, 0u);
	CHECK_NE(mask_gi_override & NEXTGI_SHADOW_MASK_GI, 0u);
	CHECK_NE(mask_opaque & NEXTGI_SHADOW_MASK_GI, 0u);

	const uint32_t alpha_bits = mask_shadow_only | mask_gi_override;
	const uint32_t mode = nextgi_shadow_light_mode(allocation, 1u << 2, alpha_bits);
	REQUIRE((mode & NEXTGI_SHADOW_MODE_LOOP) != 0u);
	REQUIRE((mode & NEXTGI_SHADOW_MODE_FALLBACK) == 0u);

	// Cutout texel on shadow-only skinned texture caster passes; solid ring occludes.
	NextGIShadowModelHit skinned_tex;
	skinned_tex.mask = mask_shadow_only;
	skinned_tex.layers = 1u << 2;
	skinned_tex.opaque_instance = !shadow_only_alpha.force_no_opaque;
	skinned_tex.geometry_class = NEXTGI_SHADOW_CLASS_ALPHA_TESTED;
	skinned_tex.alpha_kept = false;
	CHECK_FALSE(nextgi_shadow_model_occluded(mode, 1u << 2, &skinned_tex, 1));
	CHECK_EQ(nextgi_shadow_model_occluded(mode, 1u << 2, &skinned_tex, 1), nextgi_shadow_reference_occluded(1u << 2, &skinned_tex, 1));
	skinned_tex.alpha_kept = true;
	CHECK(nextgi_shadow_model_occluded(mode, 1u << 2, &skinned_tex, 1));
	CHECK_EQ(nextgi_shadow_model_occluded(mode, 1u << 2, &skinned_tex, 1), nextgi_shadow_reference_occluded(1u << 2, &skinned_tex, 1));

	// Shader-alpha skinned override defers to Full HWRT retrace and agrees on both cut and kept texels.
	NextGIShadowModelHit skinned_shader;
	skinned_shader.mask = mask_gi_override;
	skinned_shader.layers = 1u << 2;
	skinned_shader.opaque_instance = !gi_override_alpha.force_no_opaque;
	skinned_shader.geometry_class = NEXTGI_SHADOW_CLASS_ALPHA_TESTED;
	skinned_shader.retrace = true;
	skinned_shader.alpha_kept = false;
	NextGIShadowModelResult retrace_cut = nextgi_shadow_model_trace(mode, 1u << 2, &skinned_shader, 1, true);
	CHECK_FALSE(retrace_cut.occluded);
	CHECK(retrace_cut.deferred);
	CHECK_FALSE(retrace_cut.retraced_occluded);
	skinned_shader.alpha_kept = true;
	NextGIShadowModelResult retrace_solid = nextgi_shadow_model_trace(mode, 1u << 2, &skinned_shader, 1, true);
	CHECK(retrace_solid.deferred);
	CHECK(retrace_solid.retraced_occluded);

	// Opaque skinned control behind a cutout skinned surface still commits immediately.
	NextGIShadowModelHit skinned_solid;
	skinned_solid.mask = mask_opaque;
	skinned_solid.layers = 1u << 2;
	skinned_solid.opaque_instance = !opaque_skinned.force_no_opaque;
	skinned_solid.geometry_class = NEXTGI_SHADOW_CLASS_OPAQUE;
	skinned_tex.alpha_kept = false;
	const NextGIShadowModelHit combined[2] = { skinned_tex, skinned_solid };
	CHECK(nextgi_shadow_model_occluded(mode, 1u << 2, combined, 2));
}

TEST_CASE("[NextGIShadow] NextGeo proxy UVs pack into #130 stream and plan shadow-only parts without altering opaque proxies (#303)") {
	// 1. Proxy UV0 packing into #130 attribute stream (header ranges + smooth CW normal + UV0 round-trip).
	const float positions[9] = {
		0.0f, 0.0f, 0.0f,
		1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f,
	};
	// CW winding from +Y: (c - a) x (b - a) = (0,0,1) x (1,0,0) = (0, +1, 0).
	const uint32_t indices[3] = { 0, 1, 2 };
	const float uvs[6] = { -0.5f, 0.25f, 1.5f, 0.25f, -0.5f, 2.25f };
	Vector<uint8_t> stream;
	REQUIRE(nextgi_hit_pack_proxy_uvs(uvs, 3, positions, indices, 3, stream));
	REQUIRE_EQ(uint32_t(stream.size()), nextgi_hit_stream_size(3));

	NextGIHitStreamHeader header;
	memcpy(&header, stream.ptr(), sizeof(header));
	CHECK(header.uv_min[0] == doctest::Approx(-0.5f));
	CHECK(header.uv_min[1] == doctest::Approx(0.25f));
	CHECK(header.uv_extent[0] == doctest::Approx(2.0f));
	CHECK(header.uv_extent[1] == doctest::Approx(2.0f));
	for (uint32_t v = 0; v < 3; v++) {
		NextGIHitAttributes attr;
		memcpy(&attr, stream.ptr() + NEXTGI_HIT_ATTRIBUTE_HEADER + v * NEXTGI_HIT_ATTRIBUTE_STRIDE, sizeof(attr));
		const Vector2 decoded_uv = nextgi_hit_unpack_uv0(header, attr);
		CHECK(decoded_uv.x == doctest::Approx(uvs[v * 2 + 0]).epsilon(1e-3));
		CHECK(decoded_uv.y == doctest::Approx(uvs[v * 2 + 1]).epsilon(1e-3));
		Vector3 nrm;
		Vector3 tan;
		float sign = 0.0f;
		nextgi_hit_unpack_tbn(attr, nrm, tan, sign);
		CHECK(nrm.y == doctest::Approx(1.0f).epsilon(1e-2));
	}

	// Without positions/indices, normals default to +Z while UV0s still unpack cleanly.
	Vector<uint8_t> uv_only_stream;
	REQUIRE(nextgi_hit_pack_proxy_uvs(uvs, 3, nullptr, nullptr, 0, uv_only_stream));
	NextGIHitAttributes attr0;
	memcpy(&attr0, uv_only_stream.ptr() + NEXTGI_HIT_ATTRIBUTE_HEADER, sizeof(attr0));
	Vector3 nrm0;
	Vector3 tan0;
	float sign0 = 0.0f;
	nextgi_hit_unpack_tbn(attr0, nrm0, tan0, sign0);
	CHECK(nrm0.z == doctest::Approx(1.0f).epsilon(1e-2));

	// Reject malformed inputs.
	CHECK_FALSE(nextgi_hit_pack_proxy_uvs(nullptr, 3, nullptr, nullptr, 0, stream));
	CHECK_FALSE(nextgi_hit_pack_proxy_uvs(uvs, 0, nullptr, nullptr, 0, stream));

	// 2. nextgi_rt_plan_vgd_proxy(): 2 virtual surfaces in 1 VGD part (surface 0 = 1 tri, surface 1 = 1 tri).
	RenderingServerTypes::VirtualGeometryDesc desc;
	desc.eligible_surface_mask = 0x3u;
	desc.rt_proxy_eps = 0.01f;
	desc.rt_proxy_positions = PackedFloat32Array({
			0.0f, 0.0f, 0.0f,
			1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f,
			2.0f, 0.0f, 0.0f,
			3.0f, 0.0f, 0.0f,
			2.0f, 0.0f, 1.0f,
	});
	desc.rt_proxy_uvs = PackedFloat32Array({
			0.0f, 0.0f,
			1.0f, 0.0f,
			0.0f, 1.0f,
			0.0f, 0.0f,
			1.0f, 0.0f,
			0.0f, 1.0f,
	});
	desc.rt_proxy_indices.resize(6 * 4);
	for (uint32_t i = 0; i < 6; i++) {
		memcpy(desc.rt_proxy_indices.ptrw() + i * 4, &i, sizeof(uint32_t));
	}
	desc.rt_proxy_parts = PackedInt32Array({ 0, 6, 0, 6 });
	desc.rt_proxy_surface_ranges = PackedInt32Array({ 0, 3, 3, 3 });
	desc.card_descriptors.resize(32);
	desc.rt_proxy_triangle_groups.resize(4);

	// 2a. Opaque-only NextGeo mesh: shadow_uploads stays empty and GI upload payload/allocation are byte-identical.
	{
		const uint8_t none_alpha[2] = { 0, 0 };
		NextGIArenaAllocator alloc_a(65536);
		LocalVector<NextGIRTProxyPartUpload> gi_a;
		uint32_t excluded_a = 0;
		REQUIRE(nextgi_rt_plan_vgd_proxy(true, true, 1, desc, 2, none_alpha, nullptr, alloc_a, gi_a, excluded_a, nullptr));

		NextGIArenaAllocator alloc_b(65536);
		LocalVector<NextGIRTProxyPartUpload> gi_b;
		LocalVector<NextGIRTProxyPartUpload> shadow_b;
		uint32_t excluded_b = 0;
		REQUIRE(nextgi_rt_plan_vgd_proxy(true, true, 1, desc, 2, none_alpha, nullptr, alloc_b, gi_b, excluded_b, &shadow_b));

		CHECK_EQ(excluded_a, 0u);
		CHECK_EQ(excluded_b, 0u);
		CHECK(shadow_b.is_empty());
		REQUIRE_EQ(gi_a.size(), 1u);
		REQUIRE_EQ(gi_b.size(), 1u);
		CHECK_EQ(alloc_a.get_allocated_bytes(), alloc_b.get_allocated_bytes());
		CHECK(gi_a[0].payload == gi_b[0].payload);
		CHECK_EQ(gi_b[0].proxy_vertex_offset, 0u);
		CHECK_EQ(gi_b[0].proxy_vertex_count, 6u);
	}

	// 2b. Mixed mesh (surface 0 opaque, surface 1 alpha-tested): surface 0 stays in GI with cards; surface 1 goes to shadow-only without cards.
	{
		const uint8_t mixed_alpha[2] = { 0, 1 };
		NextGIArenaAllocator alloc(65536);
		LocalVector<NextGIRTProxyPartUpload> gi_uploads;
		LocalVector<NextGIRTProxyPartUpload> shadow_uploads;
		uint32_t excluded = 0;
		REQUIRE(nextgi_rt_plan_vgd_proxy(true, true, 1, desc, 2, mixed_alpha, nullptr, alloc, gi_uploads, excluded, &shadow_uploads));
		CHECK_EQ(excluded, 1u);
		REQUIRE_EQ(gi_uploads.size(), 1u);
		REQUIRE_EQ(gi_uploads[0].geometries.size(), 1u);
		CHECK_EQ(gi_uploads[0].geometries[0].surface, 0u);
		CHECK(gi_uploads[0].geometries[0].has_stage2_cards);
		CHECK_FALSE(gi_uploads[0].geometries[0].is_legacy_surface);

		REQUIRE_EQ(shadow_uploads.size(), 1u);
		REQUIRE_EQ(shadow_uploads[0].geometries.size(), 1u);
		CHECK_EQ(shadow_uploads[0].geometries[0].surface, 1u);
		CHECK_EQ(shadow_uploads[0].geometries[0].triangle_count, 1u);
		CHECK(shadow_uploads[0].geometries[0].is_proxy_surface);
		CHECK_FALSE(shadow_uploads[0].geometries[0].is_legacy_surface);
		CHECK_FALSE(shadow_uploads[0].geometries[0].has_stage2_cards);
		CHECK_EQ(shadow_uploads[0].proxy_vertex_offset, 0u);
		CHECK_EQ(shadow_uploads[0].proxy_vertex_count, 6u);
		// Compacted shadow index buffer holds the 3 indices of surface 1: {3, 4, 5}.
		const uint32_t rel_idx = shadow_uploads[0].geometries[0].index_offset - shadow_uploads[0].allocation.offset;
		REQUIRE(rel_idx + 12u <= uint32_t(shadow_uploads[0].payload.size()));
		uint32_t shadow_tri[3] = {};
		memcpy(shadow_tri, shadow_uploads[0].payload.ptr() + rel_idx, sizeof(shadow_tri));
		CHECK_EQ(shadow_tri[0], 3u);
		CHECK_EQ(shadow_tri[1], 4u);
		CHECK_EQ(shadow_tri[2], 5u);
	}

	// 2c. All-alpha VGD mesh: GI uploads stay empty (D17), shadow uploads carry both alpha-tested surfaces.
	{
		const uint8_t all_alpha[2] = { 1, 1 };
		NextGIArenaAllocator alloc(65536);
		LocalVector<NextGIRTProxyPartUpload> gi_uploads;
		LocalVector<NextGIRTProxyPartUpload> shadow_uploads;
		uint32_t excluded = 0;
		// Without shadow output, returns false (backwards-compatible) and frees nothing leaked.
		CHECK_FALSE(nextgi_rt_plan_vgd_proxy(true, true, 1, desc, 2, all_alpha, nullptr, alloc, gi_uploads, excluded, nullptr));
		CHECK_EQ(excluded, 2u);
		CHECK_EQ(alloc.get_allocated_bytes(), 0u);

		REQUIRE(nextgi_rt_plan_vgd_proxy(true, true, 1, desc, 2, all_alpha, nullptr, alloc, gi_uploads, excluded, &shadow_uploads));
		CHECK_EQ(excluded, 2u);
		CHECK(gi_uploads.is_empty());
		REQUIRE_EQ(shadow_uploads.size(), 1u);
		CHECK_EQ(shadow_uploads[0].geometries.size(), 2u);
		CHECK_EQ(shadow_uploads[0].triangle_count, 2u);
	}
}

TEST_CASE("[NextGIShadow] Dual-route alpha matrix across HWRT modes and caster kinds (#307)") {
	const NextGIHitMode modes[3] = {
		NEXTGI_HIT_MODE_FULL,
		NEXTGI_HIT_MODE_SIMPLIFIED_MATERIAL,
		NEXTGI_HIT_MODE_INLINE,
	};

	// 1. Opaque and translucent casters never take texture-alpha or retrace in any mode.
	for (NextGIHitMode mode : modes) {
		for (NextGIShadowClass cls : { NEXTGI_SHADOW_CLASS_OPAQUE, NEXTGI_SHADOW_CLASS_TRANSLUCENT }) {
			const NextGIShadowAlphaRoute r = nextgi_shadow_alpha_route_for_mode(cls, true, true, mode, true, true, NEXTGI_SHADOW_VERDICT_RETRACE);
			CHECK_FALSE(r.use_texture_alpha);
			CHECK_FALSE(r.poll_verdict);
			CHECK_FALSE(r.use_retrace);
		}
	}

	// 2. BaseMaterial3D alpha-scissor (no discard): texture-alpha only, never polls verdict or wakes hit groups.
	for (NextGIHitMode mode : modes) {
		const NextGIShadowAlphaRoute r = nextgi_shadow_alpha_route_for_mode(NEXTGI_SHADOW_CLASS_ALPHA_TESTED, true, false, mode, true, true, NEXTGI_SHADOW_VERDICT_UNKNOWN);
		CHECK(r.use_texture_alpha);
		CHECK_FALSE(r.poll_verdict);
		CHECK_FALSE(r.use_retrace);
	}

	// 3. BaseMaterial3D alpha-scissor + extra discard in fragment():
	//    - Full mode (CPU instance, valid serial, verdict != OPAQUE): dual route (BOTH texture-alpha AND retrace).
	//    - Simplified Material & Inline: keeps texture-alpha, never polls verdict or sets retrace.
	//    - GPU-written (p_instance_retrace = false) or serial 0: keeps texture-alpha, no retrace.
	for (NextGIHitMode mode : modes) {
		const NextGIShadowAlphaRoute r_unknown = nextgi_shadow_alpha_route_for_mode(NEXTGI_SHADOW_CLASS_ALPHA_TESTED, true, true, mode, true, true, NEXTGI_SHADOW_VERDICT_UNKNOWN);
		const NextGIShadowAlphaRoute r_retrace = nextgi_shadow_alpha_route_for_mode(NEXTGI_SHADOW_CLASS_ALPHA_TESTED, true, true, mode, true, true, NEXTGI_SHADOW_VERDICT_RETRACE);
		const bool expect_retrace = (mode == NEXTGI_HIT_MODE_FULL);
		CHECK(r_unknown.use_texture_alpha);
		CHECK_EQ(r_unknown.poll_verdict, expect_retrace);
		CHECK_EQ(r_unknown.use_retrace, expect_retrace);
		CHECK(r_retrace.use_texture_alpha);
		CHECK_EQ(r_retrace.poll_verdict, expect_retrace);
		CHECK_EQ(r_retrace.use_retrace, expect_retrace);

		// Helper-function discard refused per #304 (verdict = OPAQUE): drops retrace AND keeps texture-alpha cut.
		const NextGIShadowAlphaRoute r_refused = nextgi_shadow_alpha_route_for_mode(NEXTGI_SHADOW_CLASS_ALPHA_TESTED, true, true, mode, true, true, NEXTGI_SHADOW_VERDICT_OPAQUE);
		CHECK(r_refused.use_texture_alpha);
		CHECK_EQ(r_refused.poll_verdict, expect_retrace);
		CHECK_FALSE(r_refused.use_retrace);

		// GPU-written instance or missing serial: keeps texture-alpha cut, no retrace.
		const NextGIShadowAlphaRoute r_gpu = nextgi_shadow_alpha_route_for_mode(NEXTGI_SHADOW_CLASS_ALPHA_TESTED, true, true, mode, false, true, NEXTGI_SHADOW_VERDICT_RETRACE);
		CHECK(r_gpu.use_texture_alpha);
		CHECK_FALSE(r_gpu.poll_verdict);
		CHECK_FALSE(r_gpu.use_retrace);

		const NextGIShadowAlphaRoute r_noserial = nextgi_shadow_alpha_route_for_mode(NEXTGI_SHADOW_CLASS_ALPHA_TESTED, true, true, mode, true, false, NEXTGI_SHADOW_VERDICT_RETRACE);
		CHECK(r_noserial.use_texture_alpha);
		CHECK_FALSE(r_noserial.poll_verdict);
		CHECK_FALSE(r_noserial.use_retrace);
	}

	// 4. Custom discard / custom shader alpha (not BaseMaterial3D pattern):
	//    - Full mode: retrace when not refused; opaque when refused (helper-function discard), GPU-written, or serial 0.
	//    - Simplified Material & Inline: neither texture-alpha nor retrace (opaque, never polls verdict).
	for (NextGIHitMode mode : modes) {
		const bool expect_retrace = (mode == NEXTGI_HIT_MODE_FULL);
		const NextGIShadowAlphaRoute r_custom = nextgi_shadow_alpha_route_for_mode(NEXTGI_SHADOW_CLASS_ALPHA_TESTED, false, true, mode, true, true, NEXTGI_SHADOW_VERDICT_RETRACE);
		CHECK_FALSE(r_custom.use_texture_alpha);
		CHECK_EQ(r_custom.poll_verdict, expect_retrace);
		CHECK_EQ(r_custom.use_retrace, expect_retrace);

		const NextGIShadowAlphaRoute r_refused = nextgi_shadow_alpha_route_for_mode(NEXTGI_SHADOW_CLASS_ALPHA_TESTED, false, true, mode, true, true, NEXTGI_SHADOW_VERDICT_OPAQUE);
		CHECK_FALSE(r_refused.use_texture_alpha);
		CHECK_EQ(r_refused.poll_verdict, expect_retrace);
		CHECK_FALSE(r_refused.use_retrace);

		const NextGIShadowAlphaRoute r_gpu = nextgi_shadow_alpha_route_for_mode(NEXTGI_SHADOW_CLASS_ALPHA_TESTED, false, true, mode, false, true, NEXTGI_SHADOW_VERDICT_RETRACE);
		CHECK_FALSE(r_gpu.use_texture_alpha);
		CHECK_FALSE(r_gpu.poll_verdict);
		CHECK_FALSE(r_gpu.use_retrace);
	}

	// 5. Shader pattern + #304 hit classification: BaseMaterial3D alpha-scissor + fragment() discard matches
	//    texture-alpha AND is hit-group eligible; helper-function discard stays refused (NEXTGI_HIT_INELIGIBLE_DISCARD).
	const String base_code =
			"shader_type spatial;\n"
			"uniform vec4 albedo : source_color;\n"
			"uniform sampler2D texture_albedo : source_color, filter_linear_mipmap, repeat_enable;\n"
			"uniform float alpha_scissor_threshold;\n"
			"uniform vec3 uv1_scale;\n"
			"uniform vec3 uv1_offset;\n"
			"void vertex() {\n"
			"\tUV = UV * uv1_scale.xy + uv1_offset.xy;\n"
			"}\n"
			"void fragment() {\n"
			"\tvec2 base_uv = UV;\n"
			"\tvec4 albedo_tex = texture(texture_albedo, base_uv);\n"
			"\tALPHA *= albedo.a * albedo_tex.a;\n"
			"\tALPHA_SCISSOR_THRESHOLD = alpha_scissor_threshold;\n"
			"\tif (base_uv.x > 0.55) { discard; }\n"
			"}\n";
	bool is_hash = true;
	CHECK(nextgi_shadow_code_has_texture_alpha(base_code, is_hash));
	CHECK_FALSE(is_hash);

	NextGIHitShaderSource frag_discard_src;
	frag_discard_src.fragment_code = "if (base_uv.x > 0.55) { NEXTGI_HIT_DISCARD; }";
	CHECK_EQ(nextgi_hit_classify(frag_discard_src), NEXTGI_HIT_ELIGIBLE);

	NextGIHitShaderSource helper_discard_src;
	helper_discard_src.globals = "void cut(vec2 uv) { if (uv.x > 0.55) { discard; } }";
	helper_discard_src.fragment_code = "cut(base_uv);";
	CHECK_EQ(nextgi_hit_classify(helper_discard_src), NEXTGI_HIT_INELIGIBLE_DISCARD);

	// 6. CPU trace model: dual-route entry retraces at Full (allocated light) and falls back to texture-alpha
	//    (not opaque) when retrace is unavailable or the light uses the fallback path.
	const uint32_t light_masks[2] = { 1u << 0, 1u << 1 };
	const Allocation allocation = _allocation(light_masks, 2);
	const uint32_t inst_mask = nextgi_shadow_instance_mask(allocation, 0xFFFFFu, true);
	const uint32_t mode_alloc = nextgi_shadow_light_mode(allocation, 1u << 0, inst_mask);
	const uint32_t mode_fallback = nextgi_shadow_light_mode(allocation, 1u << 5, inst_mask);
	REQUIRE((mode_alloc & NEXTGI_SHADOW_MODE_LOOP) != 0u);
	REQUIRE((mode_alloc & NEXTGI_SHADOW_MODE_FALLBACK) == 0u);
	REQUIRE((mode_fallback & NEXTGI_SHADOW_MODE_FALLBACK) != 0u);

	// Region A: cut by texture-alpha (alpha_kept = false, texture_alpha_kept = false).
	NextGIShadowModelHit hit_tex_cut;
	hit_tex_cut.mask = inst_mask;
	hit_tex_cut.layers = 0xFFFFFu;
	hit_tex_cut.opaque_instance = false;
	hit_tex_cut.geometry_class = NEXTGI_SHADOW_CLASS_ALPHA_TESTED;
	hit_tex_cut.retrace = true;
	hit_tex_cut.texture_alpha = true;
	hit_tex_cut.alpha_kept = false;
	hit_tex_cut.texture_alpha_kept = false;

	// Full allocated: defers to retrace -> clear.
	NextGIShadowModelResult r_full_tex = nextgi_shadow_model_trace(mode_alloc, 1u << 0, &hit_tex_cut, 1, true);
	CHECK_FALSE(r_full_tex.occluded);
	CHECK(r_full_tex.deferred);
	CHECK_FALSE(r_full_tex.retraced_occluded);
	// No retrace (Simplified Material / Inline / Metal): falls back to texture-alpha -> STILL CLEAR (not opaque)!
	NextGIShadowModelResult r_inline_tex = nextgi_shadow_model_trace(mode_alloc, 1u << 0, &hit_tex_cut, 1, false);
	CHECK_FALSE(r_inline_tex.occluded);
	CHECK_FALSE(r_inline_tex.deferred);
	// Fallback light: cannot defer -> falls back to texture-alpha -> STILL CLEAR (not opaque)!
	NextGIShadowModelResult r_fb_tex = nextgi_shadow_model_trace(mode_fallback, 1u << 5, &hit_tex_cut, 1, true);
	CHECK_FALSE(r_fb_tex.occluded);
	CHECK_FALSE(r_fb_tex.deferred);

	// Region B: cut ONLY by extra discard (alpha_kept = false, texture_alpha_kept = true).
	NextGIShadowModelHit hit_extra_discard = hit_tex_cut;
	hit_extra_discard.alpha_kept = false;
	hit_extra_discard.texture_alpha_kept = true;

	// Full allocated: defers to retrace -> any-hit discards -> CLEAR!
	NextGIShadowModelResult r_full_disc = nextgi_shadow_model_trace(mode_alloc, 1u << 0, &hit_extra_discard, 1, true);
	CHECK_FALSE(r_full_disc.occluded);
	CHECK(r_full_disc.deferred);
	CHECK_FALSE(r_full_disc.retraced_occluded);
	// No retrace or fallback light: inline texture-alpha keeps texel -> OCCLUDED!
	NextGIShadowModelResult r_inline_disc = nextgi_shadow_model_trace(mode_alloc, 1u << 0, &hit_extra_discard, 1, false);
	CHECK(r_inline_disc.occluded);
	CHECK_FALSE(r_inline_disc.deferred);
	NextGIShadowModelResult r_fb_disc = nextgi_shadow_model_trace(mode_fallback, 1u << 5, &hit_extra_discard, 1, true);
	CHECK(r_fb_disc.occluded);
	CHECK_FALSE(r_fb_disc.deferred);

	// Region C: solid ring (alpha_kept = true, texture_alpha_kept = true) -> always occluded.
	NextGIShadowModelHit hit_solid = hit_tex_cut;
	hit_solid.alpha_kept = true;
	hit_solid.texture_alpha_kept = true;
	NextGIShadowModelResult r_full_solid = nextgi_shadow_model_trace(mode_alloc, 1u << 0, &hit_solid, 1, true);
	CHECK(r_full_solid.deferred);
	CHECK(r_full_solid.retraced_occluded);
	CHECK(nextgi_shadow_model_trace(mode_alloc, 1u << 0, &hit_solid, 1, false).occluded);
	CHECK(nextgi_shadow_model_trace(mode_fallback, 1u << 5, &hit_solid, 1, true).occluded);
}

} // namespace TestNextGIShadowPolicy
