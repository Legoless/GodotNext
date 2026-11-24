/**************************************************************************/
/*  next_gi_shadow_policy.cpp                                             */
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

#include "next_gi_shadow_policy.h"

#include "core/math/random_pcg.h"
#include "core/templates/sort_array.h"

namespace RendererRD {

// GodotNext: nextgi_shadow_policy (#145)

namespace {

struct ShadowMaskCountOrder {
	_FORCE_INLINE_ bool operator()(const NextGIShadowMaskCount &p_a, const NextGIShadowMaskCount &p_b) const {
		return p_a.count != p_b.count ? p_a.count > p_b.count : p_a.mask < p_b.mask;
	}
};

bool _contains(const uint32_t *p_masks, uint32_t p_count, uint32_t p_mask) {
	for (uint32_t i = 0; i < p_count; i++) {
		if (p_masks[i] == p_mask) {
			return true;
		}
	}
	return false;
}

} // namespace

void nextgi_shadow_top_masks(const uint32_t *p_caster_masks, uint32_t p_count, LocalVector<NextGIShadowMaskCount> &r_top) {
	r_top.clear();
	LocalVector<uint32_t> masks;
	masks.reserve(p_count);
	for (uint32_t i = 0; i < p_count; i++) {
		const uint32_t layers = p_caster_masks[i] & NEXTGI_SHADOW_LAYER_MASK;
		if (layers != 0) {
			masks.push_back(layers);
		}
	}
	if (masks.is_empty()) {
		return;
	}
	masks.sort();
	LocalVector<NextGIShadowMaskCount> counts;
	for (uint32_t i = 0; i < masks.size(); i++) {
		if (counts.is_empty() || counts[counts.size() - 1].mask != masks[i]) {
			NextGIShadowMaskCount entry;
			entry.mask = masks[i];
			counts.push_back(entry);
		}
		counts[counts.size() - 1].count++;
	}
	SortArray<NextGIShadowMaskCount, ShadowMaskCountOrder> sorter;
	sorter.sort(counts.ptr(), counts.size());
	const uint32_t keep = MIN(counts.size(), uint32_t(NEXTGI_SHADOW_DYNAMIC_BITS));
	for (uint32_t i = 0; i < keep; i++) {
		r_top.push_back(counts[i]);
	}
}

int NextGIShadowMaskAllocator::Allocation::find(uint32_t p_layers) const {
	const uint32_t layers = p_layers & NEXTGI_SHADOW_LAYER_MASK;
	if (layers == 0) {
		return -1;
	}
	for (int i = 0; i < NEXTGI_SHADOW_DYNAMIC_BITS; i++) {
		if ((used & (1u << i)) != 0 && masks[i] == layers) {
			return i;
		}
	}
	return -1;
}

uint32_t NextGIShadowMaskAllocator::Allocation::count() const {
	uint32_t n = 0;
	for (int i = 0; i < NEXTGI_SHADOW_DYNAMIC_BITS; i++) {
		n += (used >> i) & 1u;
	}
	return n;
}

bool NextGIShadowMaskAllocator::Allocation::same_set(const uint32_t *p_masks, uint32_t p_count) const {
	if (p_count != count()) {
		return false;
	}
	for (uint32_t i = 0; i < p_count; i++) {
		if (find(p_masks[i]) < 0) {
			return false;
		}
	}
	return true;
}

void NextGIShadowMaskAllocator::_adopt(const uint32_t *p_masks, uint32_t p_count, uint32_t p_reserved_bits) {
	Allocation next;
	next.generation = allocation.generation + 1;
	// Masks in both keep their bits, so their instances keep their masks, unless the bit is now reserved.
	for (int i = 0; i < NEXTGI_SHADOW_DYNAMIC_BITS; i++) {
		if ((p_reserved_bits & (1u << i)) != 0) {
			continue;
		}
		if ((allocation.used & (1u << i)) != 0 && _contains(p_masks, p_count, allocation.masks[i])) {
			next.masks[i] = allocation.masks[i];
			next.used |= 1u << i;
		}
	}
	// New masks take the free non-reserved bits, lowest first, in the order of the choice.
	for (uint32_t k = 0; k < p_count; k++) {
		if (next.find(p_masks[k]) >= 0) {
			continue;
		}
		for (int i = 0; i < NEXTGI_SHADOW_DYNAMIC_BITS; i++) {
			if ((p_reserved_bits & (1u << i)) == 0 && (next.used & (1u << i)) == 0) {
				next.masks[i] = p_masks[k];
				next.used |= 1u << i;
				break;
			}
		}
	}
	allocation = next;
}

bool NextGIShadowMaskAllocator::update(const uint32_t *p_caster_masks, uint32_t p_count, uint32_t p_reserved_bits) {
	LocalVector<NextGIShadowMaskCount> top;
	nextgi_shadow_top_masks(p_caster_masks, p_count, top);
	const uint32_t reserved = p_reserved_bits & NEXTGI_SHADOW_MASK_DYNAMIC;
	uint32_t available_slots = 0;
	for (int i = 0; i < NEXTGI_SHADOW_DYNAMIC_BITS; i++) {
		if ((reserved & (1u << i)) == 0) {
			available_slots++;
		}
	}
	uint32_t choice[NEXTGI_SHADOW_DYNAMIC_BITS] = {};
	const uint32_t choice_count = MIN(top.size(), available_slots);
	for (uint32_t i = 0; i < choice_count; i++) {
		choice[i] = top[i].mask;
	}
	const bool violates_reserved = (allocation.used & reserved) != 0;
	if (!violates_reserved && allocation.same_set(choice, choice_count)) {
		pending_count = 0;
		stable_frames = 0;
		return false;
	}
	if (!has_allocation || violates_reserved) {
		if (!violates_reserved && choice_count == 0) {
			return false;
		}
		_adopt(choice, choice_count, reserved);
		has_allocation = has_allocation || choice_count > 0;
		pending_count = 0;
		stable_frames = 0;
		return true;
	}
	bool same_pending = stable_frames > 0 && pending_count == choice_count;
	for (uint32_t i = 0; same_pending && i < choice_count; i++) {
		same_pending = _contains(pending, pending_count, choice[i]);
	}
	if (same_pending) {
		stable_frames++;
	} else {
		for (uint32_t i = 0; i < choice_count; i++) {
			pending[i] = choice[i];
		}
		pending_count = choice_count;
		stable_frames = 1;
	}
	if (stable_frames < NEXTGI_SHADOW_HYSTERESIS_FRAMES) {
		return false;
	}
	_adopt(choice, choice_count, reserved);
	pending_count = 0;
	stable_frames = 0;
	return true;
}

void NextGIShadowMaskAllocator::reset() {
	allocation = Allocation();
	pending_count = 0;
	stable_frames = 0;
	has_allocation = false;
}

uint32_t nextgi_shadow_instance_mask(const NextGIShadowMaskAllocator::Allocation &p_allocation, uint32_t p_shadow_layers, bool p_gi_visible) {
	uint32_t mask = p_gi_visible ? uint32_t(NEXTGI_SHADOW_MASK_GI) : 0u;
	const uint32_t layers = p_shadow_layers & NEXTGI_SHADOW_LAYER_MASK;
	if (layers == 0) {
		return mask;
	}
	uint32_t dynamic = 0;
	for (int i = 0; i < NEXTGI_SHADOW_DYNAMIC_BITS; i++) {
		if ((p_allocation.used & (1u << i)) != 0 && (layers & p_allocation.masks[i]) != 0) {
			dynamic |= 1u << i;
		}
	}
	return mask | (dynamic != 0 ? dynamic : uint32_t(NEXTGI_SHADOW_MASK_UNALLOCATED));
}

uint32_t nextgi_shadow_light_mode(const NextGIShadowMaskAllocator::Allocation &p_allocation, uint32_t p_caster_mask, uint32_t p_alpha_bits) {
	const uint32_t layers = p_caster_mask & NEXTGI_SHADOW_LAYER_MASK;
	if (layers == 0) {
		return 0;
	}
	const int bit = p_allocation.find(layers);
	if (bit >= 0) {
		const uint32_t cull = 1u << bit;
		return cull | ((p_alpha_bits & cull) != 0 ? uint32_t(NEXTGI_SHADOW_MODE_LOOP) : 0u);
	}
	return uint32_t(NEXTGI_SHADOW_MASK_CASTERS) | NEXTGI_SHADOW_MODE_LOOP | NEXTGI_SHADOW_MODE_FALLBACK;
}

// GodotNext: nextgi_skinned_alpha_shadows (#302)
NextGISkinnedShadowPolicy nextgi_shadow_skinned_policy(bool p_mesh_material_is_alpha, NextGIShadowClass p_effective_shadow_class, uint32_t p_shadow_layers) {
	NextGISkinnedShadowPolicy policy;
	const bool casts_shadow = (p_shadow_layers & NEXTGI_SHADOW_LAYER_MASK) != 0u;
	policy.gi_visible = !p_mesh_material_is_alpha;
	policy.shadow_only = !policy.gi_visible && casts_shadow;
	policy.skip = !policy.gi_visible && !casts_shadow;
	policy.alpha_tested = casts_shadow && p_effective_shadow_class == NEXTGI_SHADOW_CLASS_ALPHA_TESTED;
	policy.force_no_opaque = policy.alpha_tested;
	return policy;
}

// GodotNext: nextgi_shadow_dual_route (#307).
NextGIShadowAlphaRoute nextgi_shadow_alpha_route(
		NextGIShadowClass p_shadow_class,
		bool p_code_has_texture_alpha,
		bool p_uses_discard,
		bool p_retrace_mode_available,
		bool p_instance_retrace,
		bool p_has_shader_serial,
		NextGIShadowRetraceVerdict p_verdict) {
	NextGIShadowAlphaRoute route;
	if (p_shadow_class != NEXTGI_SHADOW_CLASS_ALPHA_TESTED) {
		return route;
	}
	route.use_texture_alpha = p_code_has_texture_alpha;
	const bool wants_retrace = !p_code_has_texture_alpha || p_uses_discard;
	const bool can_retrace = wants_retrace && p_retrace_mode_available && p_instance_retrace && p_has_shader_serial;
	route.poll_verdict = can_retrace;
	route.use_retrace = can_retrace && p_verdict != NEXTGI_SHADOW_VERDICT_OPAQUE;
	return route;
}

NextGIShadowModelResult nextgi_shadow_model_trace(uint32_t p_mode, uint32_t p_caster_mask, const NextGIShadowModelHit *p_hits, uint32_t p_count, bool p_retrace) {
	NextGIShadowModelResult result;
	const uint32_t cull = p_mode & NEXTGI_SHADOW_MODE_CULL_MASK;
	if (cull == 0) {
		return result;
	}
	const uint32_t layers = p_caster_mask & NEXTGI_SHADOW_LAYER_MASK;
	const bool loop = (p_mode & NEXTGI_SHADOW_MODE_LOOP) != 0;
	const bool fallback = (p_mode & NEXTGI_SHADOW_MODE_FALLBACK) != 0;
	// The retrace traces with the cull mask alone, which is exact only for an allocated bit.
	const bool defer = p_retrace && loop && !fallback;
	for (uint32_t i = 0; i < p_count && !result.occluded; i++) {
		const NextGIShadowModelHit &hit = p_hits[i];
		if ((hit.mask & cull) == 0) {
			continue; // Culled by the instance mask.
		}
		if (!loop) {
			result.occluded = true; // gl_RayFlagsOpaqueEXT: every hit commits.
			break;
		}
		if (!fallback && hit.opaque_instance) {
			result.occluded = true; // An opaque instance commits without a candidate.
			break;
		}
		if ((hit.layers & layers) == 0) {
			continue; // The layer check (only the fallback can fail it).
		}
		if (hit.geometry_class == NEXTGI_SHADOW_CLASS_ALPHA_TESTED) {
			if (defer && hit.retrace) {
				result.deferred = true;
				continue;
			}
			if (result.alpha_samples < NEXTGI_SHADOW_MAX_ALPHA_SAMPLES) {
				result.alpha_samples++;
				// GodotNext: nextgi_shadow_dual_route (#307). Matches nextgi_shadow_ray_inc.glsl: when retrace
				// cannot run, a dual-route entry falls back to its inline texture-alpha cut instead of opaque;
				// a retrace-only entry without texture alpha shadows as opaque.
				const bool inline_kept = hit.retrace ? (hit.texture_alpha ? hit.texture_alpha_kept : true) : hit.alpha_kept;
				if (!inline_kept) {
					continue;
				}
			}
			// Past the sample cap, the remaining candidates shadow as opaque.
		}
		result.occluded = true;
	}
	if (result.occluded) {
		result.deferred = false;
		return result;
	}
	if (result.deferred) {
		// The retrace: one RT-pipeline trace with the cull mask, each alpha-tested geometry's any-hit stage
		// running the material's alpha test. Opaque geometry commits.
		for (uint32_t i = 0; i < p_count; i++) {
			const NextGIShadowModelHit &hit = p_hits[i];
			if ((hit.mask & cull) == 0) {
				continue;
			}
			if (hit.geometry_class == NEXTGI_SHADOW_CLASS_ALPHA_TESTED && !hit.opaque_instance && !hit.alpha_kept) {
				continue;
			}
			result.retraced_occluded = true;
			break;
		}
	}
	return result;
}

bool nextgi_shadow_model_occluded(uint32_t p_mode, uint32_t p_caster_mask, const NextGIShadowModelHit *p_hits, uint32_t p_count) {
	return nextgi_shadow_model_trace(p_mode, p_caster_mask, p_hits, p_count, false).occluded;
}

bool nextgi_shadow_reference_occluded(uint32_t p_caster_mask, const NextGIShadowModelHit *p_hits, uint32_t p_count) {
	const uint32_t layers = p_caster_mask & NEXTGI_SHADOW_LAYER_MASK;
	for (uint32_t i = 0; i < p_count; i++) {
		const NextGIShadowModelHit &hit = p_hits[i];
		if ((hit.layers & layers) == 0) {
			continue;
		}
		if (hit.geometry_class == NEXTGI_SHADOW_CLASS_ALPHA_TESTED && !hit.alpha_kept) {
			continue;
		}
		return true;
	}
	return false;
}

bool nextgi_shadow_code_has_texture_alpha(const String &p_code, bool &r_hash) {
	r_hash = false;
	// BaseMaterial3D's lines (scene/resources/material.cpp), exactly.
	if (!p_code.contains("uniform sampler2D texture_albedo") || !p_code.contains("vec4 albedo_tex = texture(texture_albedo, base_uv);") || !p_code.contains("ALPHA *= albedo.a * albedo_tex.a;") || !p_code.contains("UV = UV * uv1_scale.xy + uv1_offset.xy;")) {
		return false;
	}
	// Everything that changes the UV or the alpha on the way: triplanar mapping, MSDF, vertex color in the albedo,
	// height mapping, point size, particle animation frames, refraction, and the distance and proximity fades (a
	// second ALPHA *=).
	static const char *changes[] = { "triplanar", "msdf", "albedo_tex *= COLOR", "texture_heightmap", "POINT_COORD", "UV /=", "ALPHA =" };
	for (const char *change : changes) {
		if (p_code.contains(change)) {
			return false;
		}
	}
	if (p_code.count("ALPHA *=") != 1) {
		return false;
	}
	if (p_code.contains("ALPHA_HASH_SCALE = alpha_hash_scale;")) {
		r_hash = true;
		return true;
	}
	return p_code.contains("ALPHA_SCISSOR_THRESHOLD = alpha_scissor_threshold;");
}

uint32_t nextgi_shadow_code_albedo_sampler_flags(const String &p_code) {
	// BaseMaterial3D writes "uniform sampler2D texture_albedo : source_color, filter_<filter>, repeat_<enable|disable>;".
	const int start = p_code.find("uniform sampler2D texture_albedo");
	if (start < 0) {
		return 0;
	}
	const int end = p_code.find(";", start);
	const String declaration = p_code.substr(start, end < 0 ? -1 : end - start);
	uint32_t flags = 0;
	if (declaration.contains("filter_nearest")) {
		flags |= NEXTGI_SHADOW_ALPHA_NEAREST;
	}
	if (declaration.contains("repeat_disable")) {
		flags |= NEXTGI_SHADOW_ALPHA_CLAMP;
	}
	return flags;
}

void nextgi_shadow_check_masks(uint32_t r_masks[NEXTGI_SHADOW_CHECK_LIGHTS]) {
	// All layers (Godot's default caster mask), five single layers, and two multi-layer masks.
	static const uint32_t masks[NEXTGI_SHADOW_CHECK_LIGHTS] = {
		0xFFFFFu,
		1u << 0,
		1u << 1,
		1u << 4,
		1u << 7,
		1u << 19,
		0x0F0F0u,
		0x55555u,
	};
	for (int i = 0; i < NEXTGI_SHADOW_CHECK_LIGHTS; i++) {
		r_masks[i] = masks[i];
	}
}

namespace {

// 5 mm: far above the float error of a traversal at the yard's 100 m scale.
constexpr float SHADOW_CHECK_MARGIN = 0.005f;

float _edge_distance(const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_normal, const Vector3 &p_point) {
	const Vector3 inward = p_normal.cross(p_b - p_a);
	const float length = inward.length();
	return length > 0.0f ? (p_point - p_a).dot(inward) / length : -1.0f;
}

} // namespace

int nextgi_shadow_check_instance_hit(const NextGIShadowCheckInstance &p_instance, const Vector3 &p_origin, const Vector3 &p_direction, float p_tmax) {
	const Vector3 end = p_origin + p_direction * p_tmax;
	if (!p_instance.world_bounds.grow(2.0f * SHADOW_CHECK_MARGIN).intersects_segment(p_origin, end)) {
		return 0;
	}
	if (p_instance.positions == nullptr || p_instance.indices == nullptr) {
		return -1;
	}
	bool hit = false;
	for (uint32_t i = 0; i + 2 < p_instance.index_count; i += 3) {
		const uint32_t i0 = p_instance.indices[i];
		const uint32_t i1 = p_instance.indices[i + 1];
		const uint32_t i2 = p_instance.indices[i + 2];
		if (i0 >= p_instance.vertex_count || i1 >= p_instance.vertex_count || i2 >= p_instance.vertex_count) {
			return -1;
		}
		const Vector3 a = p_instance.transform.xform(Vector3(p_instance.positions[i0 * 3], p_instance.positions[i0 * 3 + 1], p_instance.positions[i0 * 3 + 2]));
		const Vector3 b = p_instance.transform.xform(Vector3(p_instance.positions[i1 * 3], p_instance.positions[i1 * 3 + 1], p_instance.positions[i1 * 3 + 2]));
		const Vector3 c = p_instance.transform.xform(Vector3(p_instance.positions[i2 * 3], p_instance.positions[i2 * 3 + 1], p_instance.positions[i2 * 3 + 2]));
		Vector3 normal = (b - a).cross(c - a);
		const float area = normal.length();
		if (area < 1e-10f) {
			continue; // Degenerate: neither traversal reports it.
		}
		normal /= area;
		const float distance_origin = normal.dot(p_origin - a);
		const float distance_end = normal.dot(end - a);
		const float denominator = normal.dot(p_direction);
		if (Math::abs(denominator) < 0.02f) {
			// Nearly parallel to the plane: ambiguous wherever it runs within the margin of the triangle.
			if (MIN(Math::abs(distance_origin), Math::abs(distance_end)) < SHADOW_CHECK_MARGIN || (distance_origin > 0.0f) != (distance_end > 0.0f)) {
				AABB triangle(a, Vector3());
				triangle.expand_to(b);
				triangle.expand_to(c);
				if (triangle.grow(SHADOW_CHECK_MARGIN).intersects_segment(p_origin, end)) {
					return -1;
				}
			}
			continue;
		}
		const float t = -distance_origin / denominator;
		if (t < -SHADOW_CHECK_MARGIN || t > p_tmax + SHADOW_CHECK_MARGIN) {
			continue;
		}
		const Vector3 point = p_origin + p_direction * t;
		const float inside = MIN(_edge_distance(a, b, normal, point), MIN(_edge_distance(b, c, normal, point), _edge_distance(c, a, normal, point)));
		if (Math::abs(inside) < SHADOW_CHECK_MARGIN) {
			return -1;
		}
		if (inside < 0.0f) {
			continue;
		}
		if (t < SHADOW_CHECK_MARGIN || t > p_tmax - SHADOW_CHECK_MARGIN) {
			return -1;
		}
		hit = true;
	}
	return hit ? 1 : 0;
}

uint32_t nextgi_shadow_check_build(const NextGIShadowCheckInstance *p_instances, uint32_t p_instance_count, const uint32_t p_masks[NEXTGI_SHADOW_CHECK_LIGHTS], uint32_t p_ray_count, uint32_t p_seed, LocalVector<NextGIShadowCheckRay> &r_rays, LocalVector<uint8_t> &r_expected) {
	r_rays.clear();
	r_expected.clear();
	LocalVector<uint32_t> by_layer[20];
	for (uint32_t i = 0; i < p_instance_count; i++) {
		const NextGIShadowCheckInstance &instance = p_instances[i];
		if (!instance.opaque_caster || instance.positions == nullptr) {
			continue;
		}
		for (int layer = 0; layer < 20; layer++) {
			if ((instance.layers & (1u << layer)) != 0) {
				by_layer[layer].push_back(i);
			}
		}
	}
	LocalVector<int> layers;
	for (int layer = 0; layer < 20; layer++) {
		if (!by_layer[layer].is_empty()) {
			layers.push_back(layer);
		}
	}
	if (layers.is_empty()) {
		return 0;
	}
	RandomPCG rng(p_seed);
	r_rays.reserve(p_ray_count);
	r_expected.reserve(p_ray_count);
	for (uint32_t r = 0; r < p_ray_count; r++) {
		const uint32_t light = r % NEXTGI_SHADOW_CHECK_LIGHTS;
		const LocalVector<uint32_t> &targets = by_layer[layers[(r / NEXTGI_SHADOW_CHECK_LIGHTS) % layers.size()]];
		for (int attempt = 0; attempt < 256; attempt++) {
			const NextGIShadowCheckInstance &target = p_instances[targets[rng.rand() % targets.size()]];
			const Vector3 point = target.world_bounds.get_center() + target.world_bounds.size * Vector3(rng.randf() - 0.5f, rng.randf() - 0.5f, rng.randf() - 0.5f) * 0.8f;
			// From above the target, as a light shines: at least 0.2 of the way up.
			Vector3 toward(rng.randf() * 2.0f - 1.0f, 0.2f + rng.randf() * 0.8f, rng.randf() * 2.0f - 1.0f);
			toward.normalize();
			const float distance = 2.0f + rng.randf() * 8.0f;
			const Vector3 origin = point + toward * distance;
			const Vector3 direction = -toward;
			const float tmax = distance * (0.5f + rng.randf());
			bool ambiguous = false;
			bool occluded = false;
			for (uint32_t i = 0; i < p_instance_count && !ambiguous; i++) {
				const NextGIShadowCheckInstance &instance = p_instances[i];
				const int hit = nextgi_shadow_check_instance_hit(instance, origin, direction, tmax);
				if (hit < 0 || (hit > 0 && !instance.opaque_caster)) {
					ambiguous = true; // Ambiguous, or an alpha-tested or translucent surface the caster rule does not cover.
				} else if (hit > 0 && (instance.layers & p_masks[light] & NEXTGI_SHADOW_LAYER_MASK) != 0) {
					occluded = true;
				}
			}
			if (ambiguous) {
				continue;
			}
			NextGIShadowCheckRay ray;
			ray.origin[0] = origin.x;
			ray.origin[1] = origin.y;
			ray.origin[2] = origin.z;
			ray.tmax = tmax;
			ray.direction[0] = direction.x;
			ray.direction[1] = direction.y;
			ray.direction[2] = direction.z;
			ray.light = light;
			r_rays.push_back(ray);
			r_expected.push_back(occluded ? 1 : 0);
			break;
		}
	}
	return r_rays.size();
}

} // namespace RendererRD
