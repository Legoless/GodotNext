/**************************************************************************/
/*  next_gi_rt_native.cpp                                                 */
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

#include "next_gi_rt_native.h"

#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"
#include "core/templates/hash_set.h"
#include "core/templates/hashfuncs.h"
#include "servers/rendering/renderer_rd/environment/next_gi_hit_materials.h"
#include "servers/rendering/renderer_rd/shaders/nextgeo/rt_cut_extract.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"
#include "servers/rendering/rendering_server_globals.h"

#include <cmath>

namespace RendererRD {

// GodotNext: nextgeo_rt_native (#156)

static uint32_t _native_u32(const uint8_t *p_data, uint32_t p_offset) {
	uint32_t value = 0;
	memcpy(&value, p_data + p_offset, sizeof(uint32_t));
	return value;
}

static float _native_f32(const uint8_t *p_data, uint32_t p_offset) {
	float value = 0.0f;
	memcpy(&value, p_data + p_offset, sizeof(float));
	return value;
}

static uint16_t _native_u16(const uint8_t *p_data, uint32_t p_offset) {
	uint16_t value = 0;
	memcpy(&value, p_data + p_offset, sizeof(uint16_t));
	return value;
}

struct SurfaceCursorCompare {
	template <typename T>
	_FORCE_INLINE_ bool operator()(const T &p_a, const T &p_b) const { return p_a.surface < p_b.surface; }
};

static constexpr uint32_t NATIVE_DAG_GROUP_BYTES = 60;
static constexpr uint32_t NATIVE_CARD_BYTES = 32;
static constexpr uint16_t NATIVE_CARD_UNKNOWN = 0xFFFEu;

uint32_t nextgeo_rt_native_band(float p_distance, float p_max_scale) {
	const float scale = (p_max_scale > 0.0f && std::isfinite(p_max_scale)) ? p_max_scale : 1.0f;
	const float distance = p_distance / scale;
	if (!(distance >= NEXTGEO_RT_NATIVE_BAND_BASE_DISTANCE)) {
		return 0; // Nearer than the base, or not a number.
	}
	uint32_t band = 1;
	float edge = NEXTGEO_RT_NATIVE_BAND_BASE_DISTANCE * 2.0f;
	while (distance >= edge && band < uint32_t(NEXTGEO_RT_NATIVE_BAND_COUNT) - 1u) {
		band++;
		edge *= 2.0f;
	}
	return band;
}

float nextgeo_rt_native_error_per_meter(float p_max_pixel_error, float p_internal_height, float p_projection_y) {
	if (!(p_max_pixel_error > 0.0f) || !(p_internal_height >= 1.0f) || !(p_projection_y > 0.0f)) {
		return 0.0f;
	}
	// A pixel spans 2 d / (H |P11|) world units at distance d (perspective), 2 / (H |P11|) in all
	// of an orthographic view.
	const float value = p_max_pixel_error * 2.0f / (p_internal_height * p_projection_y);
	if (!(value > 0.0f) || !std::isfinite(value)) {
		return 0.0f;
	}
	int exponent = 0;
	std::frexp(value, &exponent); // value = m * 2^exponent, m in [0.5, 1).
	return std::ldexp(1.0f, exponent - 1);
}

float nextgeo_rt_native_band_error(uint32_t p_band, float p_error_per_meter, float p_proxy_eps, bool p_orthogonal) {
	const float eps = p_proxy_eps > 0.0f ? p_proxy_eps : 0.0f;
	if (!(p_error_per_meter > 0.0f)) {
		return eps;
	}
	if (p_orthogonal) {
		return MIN(eps, p_error_per_meter);
	}
	const uint32_t band = MIN(p_band, uint32_t(NEXTGEO_RT_NATIVE_BAND_COUNT) - 1u);
	const float near_distance = NEXTGEO_RT_NATIVE_BAND_BASE_DISTANCE * 0.5f * float(1u << band);
	return MIN(eps, p_error_per_meter * near_distance);
}

NextGeoRTNativeExclusion nextgeo_rt_native_exclusion(bool p_skinned, bool p_vertex_offset, bool p_legacy_surfaces, bool p_spline) {
	if (p_skinned) {
		return NEXTGEO_RT_NATIVE_EXCLUDED_SKINNED;
	}
	if (p_spline) {
		return NEXTGEO_RT_NATIVE_EXCLUDED_SPLINE;
	}
	if (p_vertex_offset) {
		return NEXTGEO_RT_NATIVE_EXCLUDED_VERTEX_OFFSET;
	}
	if (p_legacy_surfaces) {
		return NEXTGEO_RT_NATIVE_EXCLUDED_LEGACY_SURFACES;
	}
	return NEXTGEO_RT_NATIVE_INCLUDED;
}

bool NextGeoRTNativeDag::init(const uint8_t *p_data, uint32_t p_size) {
	data = nullptr;
	size = 0;
	group_count = 0;
	if (p_data == nullptr || p_size < 8) {
		return false;
	}
	const uint32_t groups = _native_u32(p_data, 0);
	if (8ull + uint64_t(groups) * NATIVE_DAG_GROUP_BYTES > uint64_t(p_size)) {
		return false;
	}
	data = p_data;
	size = p_size;
	group_count = groups;
	return true;
}

uint32_t NextGeoRTNativeDag::group_cluster_offset(uint32_t p_group) const {
	return _native_u32(data, 8 + p_group * NATIVE_DAG_GROUP_BYTES + 0);
}

uint32_t NextGeoRTNativeDag::group_cluster_count(uint32_t p_group) const {
	return _native_u32(data, 8 + p_group * NATIVE_DAG_GROUP_BYTES + 4);
}

float NextGeoRTNativeDag::group_parent_error(uint32_t p_group) const {
	return _native_f32(data, 8 + p_group * NATIVE_DAG_GROUP_BYTES + 32);
}

AABB NextGeoRTNativeDag::group_aabb(uint32_t p_group) const {
	const uint32_t base = 8 + p_group * NATIVE_DAG_GROUP_BYTES;
	const Vector3 lo(_native_f32(data, base + 36), _native_f32(data, base + 40), _native_f32(data, base + 44));
	const Vector3 hi(_native_f32(data, base + 48), _native_f32(data, base + 52), _native_f32(data, base + 56));
	return AABB(lo, hi - lo);
}

bool nextgeo_rt_native_select_cut(const RendererVirtualMeshStorage &p_storage, const RendererVirtualMeshStorage::MeshInfo &p_info, float p_error, uint32_t p_excluded_surfaces, NextGeoRTNativeCut &r_cut) {
	r_cut = NextGeoRTNativeCut();
	NextGeoRTNativeDag dag;
	if (!dag.init(p_info.desc.dag_header.ptr(), uint32_t(p_info.desc.dag_header.size())) || dag.group_count == 0) {
		return false;
	}
	const uint32_t desc_count = uint32_t(p_info.cluster_blob.size() / sizeof(RendererVirtualMeshStorage::ClusterGPUDesc));
	if (desc_count == 0) {
		return false;
	}
	const RendererVirtualMeshStorage::ClusterGPUDesc *descs = reinterpret_cast<const RendererVirtualMeshStorage::ClusterGPUDesc *>(p_info.cluster_blob.ptr());
	const uint32_t first_cluster = p_info.record.cluster_offset;

	struct Selected {
		uint32_t surface = 0;
		uint32_t cluster = 0;
		bool operator<(const Selected &p_other) const {
			return surface != p_other.surface ? surface < p_other.surface : cluster < p_other.cluster;
		}
	};
	LocalVector<Selected> selected;
	for (uint32_t group = 0; group < dag.group_count; group++) {
		// Coarser groups cover this one (or a NaN error): not in the cut.
		if (!(dag.group_parent_error(group) > p_error)) {
			continue;
		}
		// The group's clusters are not all resident; the parents that refine it stay instead.
		if (!p_storage.mesh_is_group_refine_ok(p_info.mesh_index, group)) {
			continue;
		}
		const uint32_t offset = dag.group_cluster_offset(group);
		const uint32_t count = dag.group_cluster_count(group);
		for (uint32_t i = 0; i < count; i++) {
			const uint32_t global_cluster = offset + i;
			if (global_cluster < first_cluster) {
				continue;
			}
			const uint32_t local = global_cluster - first_cluster;
			if (local >= desc_count || local >= p_info.cluster_refine.size()) {
				continue;
			}
			const RendererVirtualMeshStorage::ClusterRefineInfo &refine = p_info.cluster_refine[local];
			if (!refine.valid) {
				continue;
			}
			const float own_error = refine.group_error;
			if (own_error > p_error && refine.refined_group >= 0 && p_storage.mesh_is_group_refine_ok(p_info.mesh_index, uint32_t(refine.refined_group))) {
				continue; // The finer group it was simplified from is resident and is taken instead.
			}
			const RendererVirtualMeshStorage::ClusterGPUDesc &desc = descs[local];
			const uint32_t stride = desc.stride_and_counts & 0xFFu;
			const uint32_t vertex_count = (desc.stride_and_counts >> 8u) & 0xFFu;
			const uint32_t triangle_count = (desc.stride_and_counts >> 16u) & 0xFFu;
			const uint32_t surface = desc.surface_and_page & 0xFFFFu;
			if (stride < 3u || vertex_count == 0u || triangle_count == 0u) {
				continue;
			}
			if (surface < 32u && (p_excluded_surfaces & (1u << surface)) != 0u) {
				continue;
			}
			Selected entry;
			entry.surface = surface;
			entry.cluster = local;
			selected.push_back(entry);
			r_cut.triangle_count += triangle_count;
			r_cut.vertex_count += vertex_count;
			r_cut.max_error = MAX(r_cut.max_error, own_error);
		}
	}
	selected.sort();
	r_cut.clusters.resize(selected.size());
	uint32_t hash_lo = hash_murmur3_one_32(selected.size(), 0x4E4E5254u);
	uint32_t hash_hi = hash_murmur3_one_32(selected.size(), 0x52544E47u);
	for (uint32_t i = 0; i < selected.size(); i++) {
		r_cut.clusters[i] = selected[i].cluster;
		hash_lo = hash_murmur3_one_32(selected[i].cluster, hash_lo);
		hash_hi = hash_murmur3_one_32(selected[i].cluster ^ 0x9E3779B9u, hash_hi);
	}
	r_cut.hash = (uint64_t(hash_fmix32(hash_hi)) << 32u) | uint64_t(hash_fmix32(hash_lo));
	return true;
}

void nextgeo_rt_native_split_parts(const uint32_t *p_triangles, const uint32_t *p_vertices, const uint32_t *p_surfaces, uint32_t p_count, uint32_t p_max_triangles, LocalVector<NextGeoRTNativePartPlan> &r_parts) {
	r_parts.clear();
	NextGeoRTNativePartPlan part;
	for (uint32_t i = 0; i < p_count; i++) {
		if (part.cluster_count > 0 && part.triangle_count + p_triangles[i] > p_max_triangles) {
			r_parts.push_back(part);
			part = NextGeoRTNativePartPlan();
		}
		if (part.cluster_count == 0) {
			part.first_cluster = i;
		}
		if (part.runs.is_empty() || part.runs[part.runs.size() - 1].surface != p_surfaces[i]) {
			NextGeoRTNativeRun run;
			run.surface = p_surfaces[i];
			run.first_triangle = part.triangle_count;
			part.runs.push_back(run);
		}
		part.runs[part.runs.size() - 1].triangle_count += p_triangles[i];
		part.cluster_count++;
		part.triangle_count += p_triangles[i];
		part.vertex_count += p_vertices[i];
	}
	if (part.cluster_count > 0) {
		r_parts.push_back(part);
	}
}

static uint64_t _native_align16(uint64_t p_value) {
	return (p_value + 15u) & ~uint64_t(15u);
}

void nextgeo_rt_native_part_layout(const NextGeoRTNativePartPlan &p_part, NextGeoRTNativePartLayout &r_layout, bool p_with_attributes) {
	uint64_t cursor = _native_align16(uint64_t(p_part.vertex_count) * 3u * sizeof(float));
	r_layout.index_offset = uint32_t(cursor);
	cursor = _native_align16(cursor + uint64_t(p_part.triangle_count) * 3u * sizeof(uint32_t));
	r_layout.run_group_offsets.clear();
	for (const NextGeoRTNativeRun &run : p_part.runs) {
		r_layout.run_group_offsets.push_back(uint32_t(cursor));
		cursor = _native_align16(cursor + uint64_t(run.triangle_count) * sizeof(uint16_t));
	}
	if (p_with_attributes && p_part.vertex_count > 0) {
		r_layout.attribute_offset = uint32_t(cursor);
		cursor = _native_align16(cursor + uint64_t(nextgi_hit_stream_size(p_part.vertex_count)));
		r_layout.attribute_scratch_offset = uint32_t(cursor);
		cursor = _native_align16(cursor + 8u * sizeof(uint32_t));
	} else {
		r_layout.attribute_offset = UINT32_MAX;
		r_layout.attribute_scratch_offset = UINT32_MAX;
	}
	r_layout.size = uint32_t(cursor);
}

static AABB _native_card_box(const uint8_t *p_card) {
	const Vector3 center(_native_f32(p_card, 4), _native_f32(p_card, 8), _native_f32(p_card, 12));
	const Vector3 half(_native_f32(p_card, 16), _native_f32(p_card, 20), _native_f32(p_card, 24));
	return AABB(center - half, half * 2.0f);
}

static AABB _native_grown_group_box(const NextGeoRTNativeDag &p_dag, uint32_t p_group, float p_proxy_eps) {
	const AABB box = p_dag.group_aabb(p_group);
	const float tolerance = MAX(p_proxy_eps, 1e-4f * MAX(box.get_longest_axis_size(), 1.0f));
	return box.grow(tolerance);
}

bool nextgeo_rt_native_cards_are_dag_groups(const NextGeoRTNativeDag &p_dag, const uint8_t *p_cards, uint32_t p_card_count, float p_proxy_eps) {
	if (p_cards == nullptr || p_card_count == 0 || p_dag.group_count == 0) {
		return false;
	}
	for (uint32_t i = 0; i < p_card_count; i++) {
		const uint8_t *card = p_cards + i * NATIVE_CARD_BYTES;
		const uint32_t group = _native_u16(card, 0);
		if (group >= p_dag.group_count || !(p_dag.group_parent_error(group) > p_proxy_eps)) {
			return false;
		}
		const AABB group_box = _native_grown_group_box(p_dag, group, p_proxy_eps);
		const AABB card_box = _native_card_box(card);
		const Vector3 card_end = card_box.position + card_box.size;
		const Vector3 group_end = group_box.position + group_box.size;
		for (int axis = 0; axis < 3; axis++) {
			if (card_box.position[axis] < group_box.position[axis] || card_end[axis] > group_end[axis]) {
				return false;
			}
		}
	}
	return true;
}

uint16_t nextgeo_rt_native_card_of_proxy_group(const NextGeoRTNativeDag &p_dag, uint32_t p_group, bool p_direct, const uint8_t *p_cards, uint32_t p_card_count, float p_proxy_eps) {
	if (p_group >= p_dag.group_count) {
		return NEXTGEO_RT_NATIVE_NO_CARD;
	}
	if (p_direct) {
		return p_group < NEXTGEO_RT_NATIVE_NO_CARD ? uint16_t(p_group) : NEXTGEO_RT_NATIVE_NO_CARD;
	}
	if (p_cards == nullptr) {
		return NEXTGEO_RT_NATIVE_NO_CARD;
	}
	const AABB group_box = _native_grown_group_box(p_dag, p_group, p_proxy_eps);
	// Descriptors are sorted by (group, axis): the first match is the lowest card group.
	for (uint32_t i = 0; i < p_card_count; i++) {
		const uint8_t *card = p_cards + i * NATIVE_CARD_BYTES;
		const AABB card_box = _native_card_box(card);
		if (group_box.has_point(card_box.get_center())) {
			return _native_u16(card, 0);
		}
	}
	return NEXTGEO_RT_NATIVE_NO_CARD;
}

uint32_t nextgeo_rt_native_proxy_group(const NextGeoRTNativeDag &p_dag, uint32_t p_group, const uint32_t *p_parent_of, uint32_t p_parent_count, float p_proxy_eps) {
	uint32_t group = p_group;
	for (uint32_t step = 0; step < 256u && group < p_dag.group_count; step++) {
		if (p_dag.group_parent_error(group) > p_proxy_eps) {
			return group;
		}
		if (p_parent_of == nullptr || group >= p_parent_count) {
			return UINT32_MAX;
		}
		group = p_parent_of[group];
	}
	return UINT32_MAX;
}

bool nextgeo_rt_native_proxy_triangle_bases(const uint32_t *p_triangles, const uint32_t *p_surfaces, uint32_t p_count, uint32_t p_max_part_triangles, uint32_t p_total_triangles, LocalVector<uint32_t> &r_bases) {
	r_bases.resize(p_count);
	struct SurfaceCursor {
		uint32_t surface = 0;
		uint32_t triangles = 0;
		uint32_t cursor = 0;
	};
	LocalVector<SurfaceCursor> cursors;
	uint64_t total = 0;
	uint32_t first = 0;
	while (first < p_count) {
		// A part closes before the cluster that would overflow it (never empty).
		uint32_t end = first;
		uint64_t part_triangles = 0;
		while (end < p_count && (part_triangles == 0 || part_triangles + p_triangles[end] <= p_max_part_triangles)) {
			part_triangles += p_triangles[end];
			end++;
		}
		// Stable counting sort by surface inside the part.
		cursors.clear();
		for (uint32_t i = first; i < end; i++) {
			uint32_t at = 0;
			while (at < cursors.size() && cursors[at].surface != p_surfaces[i]) {
				at++;
			}
			if (at == cursors.size()) {
				SurfaceCursor entry;
				entry.surface = p_surfaces[i];
				cursors.push_back(entry);
			}
			cursors[at].triangles += p_triangles[i];
		}
		cursors.sort_custom<SurfaceCursorCompare>();
		uint64_t cursor = total;
		for (SurfaceCursor &entry : cursors) {
			entry.cursor = uint32_t(cursor);
			cursor += entry.triangles;
		}
		for (uint32_t i = first; i < end; i++) {
			for (SurfaceCursor &entry : cursors) {
				if (entry.surface == p_surfaces[i]) {
					r_bases[i] = entry.cursor;
					entry.cursor += p_triangles[i];
					break;
				}
			}
		}
		total += part_triangles;
		first = end;
	}
	return total == uint64_t(p_total_triangles);
}

void nextgeo_rt_native_proxy_ancestors(const NextGeoRTNativeDag &p_dag, uint32_t p_group, const uint32_t *p_parent_first, const uint32_t *p_parents, uint32_t p_group_count, float p_proxy_eps, LocalVector<uint32_t> &r_groups) {
	r_groups.clear();
	if (p_group >= p_dag.group_count) {
		return;
	}
	HashSet<uint32_t> seen;
	LocalVector<uint32_t> stack;
	stack.push_back(p_group);
	seen.insert(p_group);
	while (!stack.is_empty()) {
		const uint32_t group = stack[stack.size() - 1];
		stack.resize(stack.size() - 1);
		if (p_dag.group_parent_error(group) > p_proxy_eps) {
			r_groups.push_back(group);
			continue;
		}
		if (p_parent_first == nullptr || p_parents == nullptr || group >= p_group_count) {
			continue;
		}
		for (uint32_t i = p_parent_first[group]; i < p_parent_first[group + 1]; i++) {
			const uint32_t parent = p_parents[i];
			if (parent < p_dag.group_count && !seen.has(parent)) {
				seen.insert(parent);
				stack.push_back(parent);
			}
		}
	}
	r_groups.sort();
}

void nextgeo_rt_native_card_range(const uint8_t *p_cards, uint32_t p_card_count, uint16_t p_group, uint32_t &r_first, uint32_t &r_end) {
	r_first = 0;
	r_end = 0;
	if (p_cards == nullptr) {
		return;
	}
	uint32_t low = 0;
	uint32_t high = p_card_count;
	while (low < high) {
		const uint32_t mid = low + (high - low) / 2u;
		if (_native_u16(p_cards, mid * NATIVE_CARD_BYTES) < p_group) {
			low = mid + 1u;
		} else {
			high = mid;
		}
	}
	r_first = low;
	r_end = low;
	while (r_end < p_card_count && _native_u16(p_cards, r_end * NATIVE_CARD_BYTES) == p_group) {
		r_end++;
	}
}

bool nextgeo_rt_native_triangle_axis(const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_c, uint32_t &r_axis) {
	// The proxy's rule (nextgeo_rt_proxy.cpp): Godot front faces wind clockwise, so the outward
	// normal is (c - a) x (b - a). In float, as rt_cut_extract.glsl computes it.
	const Vector3 u = p_c - p_a;
	const Vector3 v = p_b - p_a;
	const float nx = float(u.y) * float(v.z) - float(u.z) * float(v.y);
	const float ny = float(u.z) * float(v.x) - float(u.x) * float(v.z);
	const float nz = float(u.x) * float(v.y) - float(u.y) * float(v.x);
	if (!(nx * nx + ny * ny + nz * nz > 1e-24f)) {
		return false;
	}
	const float ax = std::fabs(nx);
	const float ay = std::fabs(ny);
	const float az = std::fabs(nz);
	if (ax >= ay && ax >= az) {
		r_axis = nx >= 0.0f ? 0u : 1u;
	} else if (ay >= az) {
		r_axis = ny >= 0.0f ? 2u : 3u;
	} else {
		r_axis = nz >= 0.0f ? 4u : 5u;
	}
	return true;
}

// rt_cut_extract.glsl's test_card(): the key is (squared distance outside the grown box, half
// thickness along the axis, half area across it), compared in that order.
static void _native_test_card(const uint8_t *p_card, uint32_t p_axis, const Vector3 &p_centroid, float p_tolerance, uint16_t &r_group, float r_key[3]) {
	if (uint32_t(p_card[2]) != p_axis) {
		return;
	}
	float outside_squared = 0.0f;
	float half[3];
	for (int k = 0; k < 3; k++) {
		const float center = _native_f32(p_card, 4 + k * 4);
		half[k] = _native_f32(p_card, 16 + k * 4);
		const float outside = MAX(std::fabs(float(p_centroid[k]) - center) - half[k] - p_tolerance, 0.0f);
		outside_squared += outside * outside;
	}
	const uint32_t n = p_axis >> 1u;
	const float key[3] = { outside_squared, half[n], half[(n + 1u) % 3u] * half[(n + 2u) % 3u] };
	if (key[0] < r_key[0] || (key[0] == r_key[0] && (key[1] < r_key[1] || (key[1] == r_key[1] && key[2] < r_key[2])))) {
		r_key[0] = key[0];
		r_key[1] = key[1];
		r_key[2] = key[2];
		r_group = _native_u16(p_card, 0);
	}
}

uint16_t nextgeo_rt_native_triangle_card(const uint8_t *p_cards, uint32_t p_card_count, const uint32_t *p_candidates, uint32_t p_candidate_count, uint32_t p_search_all_limit, const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_c, float p_tolerance, uint16_t p_default) {
	uint32_t axis = 0;
	if (p_cards == nullptr || !nextgeo_rt_native_triangle_axis(p_a, p_b, p_c, axis)) {
		return p_default;
	}
	const Vector3 centroid = (p_a + p_b + p_c) / 3.0f;
	uint16_t group = p_default;
	float key[3] = { 3.4e38f, 3.4e38f, 3.4e38f };
	for (uint32_t k = 0; p_candidates != nullptr && k < p_candidate_count; k++) {
		if (p_candidates[k] < p_card_count) {
			_native_test_card(p_cards + p_candidates[k] * NATIVE_CARD_BYTES, axis, centroid, p_tolerance, group, key);
		}
	}
	if (key[0] > 0.0f && p_card_count <= p_search_all_limit) {
		for (uint32_t k = 0; k < p_card_count; k++) {
			_native_test_card(p_cards + k * NATIVE_CARD_BYTES, axis, centroid, p_tolerance, group, key);
		}
	}
	return group;
}

/* NextGeoRTNativeSets */

void NextGeoRTNativeSets::_free_set(RID p_mesh, Set &r_set, bool p_now) {
	for (uint32_t i = 0; i < pending_compactions.size();) {
		if (pending_compactions[i].mesh == p_mesh && pending_compactions[i].hash == r_set.hash) {
			pending_compactions.remove_at(i);
		} else {
			i++;
		}
	}
	for (Part &part : r_set.parts) {
		if (part.blas.is_valid()) {
			_part_free_blas(part);
		}
		_part_free_range(p_mesh, part, p_now);
	}
	r_set = Set();
}

void NextGeoRTNativeSets::_unref(MeshState &r_state, uint64_t p_hash, bool p_now) {
	Set *set = r_state.sets.getptr(p_hash);
	if (set == nullptr) {
		return;
	}
	if (set->refs > 0) {
		set->refs--;
	}
	if (set->refs == 0) {
		_free_set(r_state.mesh, *set, p_now);
		r_state.sets.erase(p_hash);
	}
}

void NextGeoRTNativeSets::_drop_current(MeshState &r_state, Bucket &r_bucket, bool p_now) {
	if (r_bucket.has_current) {
		r_bucket.has_current = false;
		_unref(r_state, r_bucket.current, p_now);
	}
}

void NextGeoRTNativeSets::_drop_pending(MeshState &r_state, Bucket &r_bucket, bool p_now) {
	if (r_bucket.has_pending) {
		r_bucket.has_pending = false;
		_unref(r_state, r_bucket.pending, p_now);
	}
}

void NextGeoRTNativeSets::_release_state(MeshState &r_state, bool p_now) {
	for (KeyValue<uint64_t, Set> &entry : r_state.sets) {
		_free_set(r_state.mesh, entry.value, p_now);
	}
	r_state.sets.clear();
	for (Bucket &bucket : r_state.buckets) {
		bucket = Bucket();
	}
}

uint64_t NextGeoRTNativeSets::_reclaimable_bytes(MeshState &r_state, uint64_t p_exclude_hash, bool p_mark) {
	uint64_t bytes = 0;
	for (KeyValue<uint64_t, Set> &entry : r_state.sets) {
		Set &set = entry.value;
		const bool attributes_stale = set.has_attributes != r_state.want_attributes;
		if (entry.key == p_exclude_hash || !set.ready() || set.reclaim || (set.last_band_use_frame == frame && !attributes_stale)) {
			continue;
		}
		// A built set some bucket waits to swap in is about to be traced as its band's set.
		bool wanted = false;
		for (const Bucket &bucket : r_state.buckets) {
			if (bucket.has_pending && bucket.pending == entry.key) {
				wanted = true;
				break;
			}
		}
		if (wanted) {
			continue;
		}
		bytes += set.bytes;
		if (p_mark) {
			set.reclaim = true;
		}
	}
	return bytes;
}

void NextGeoRTNativeSets::_refuse(MeshState &r_state, uint64_t p_hash, uint64_t p_bytes, uint32_t p_triangles, uint64_t p_headroom, uint32_t p_band, bool p_memory) {
	// Every bucket that waits on the set is refused with it; they are cut again only once the cap has
	// room for p_bytes (never, for a set whose BLAS could not be created), or when their cut changes.
	for (Bucket &bucket : r_state.buckets) {
		if (bucket.has_pending && bucket.pending == p_hash) {
			bucket.refused = true;
			bucket.refused_hash = p_hash;
			bucket.refused_bytes = p_bytes;
			_drop_pending(r_state, bucket, false);
		}
	}
	if (p_memory) {
		stats.memory_refusals++;
		stats.last_refusal_band = int32_t(p_band);
		stats.last_refusal_triangles = p_triangles;
		stats.last_refusal_bytes = p_bytes;
		stats.last_refusal_headroom = p_headroom;
	}
}

uint64_t NextGeoRTNativeSets::_memory(uint64_t &r_native_bytes) {
	// The MN4 base: the proxies of the meshes native mode covers, those with sets or demand in the
	// absent window (the window in which their sets stay), so begin_frame_sets() and build() see the
	// same base. Each mesh counts its largest full proxy build size (see MeshState).
	uint64_t base = 0;
	uint64_t base_current = 0;
	r_native_bytes = 0;
	for (KeyValue<RID, MeshState> &entry : meshes) {
		MeshState &state = entry.value;
		uint64_t build_bytes = 0;
		uint64_t current_bytes = 0;
		_mesh_proxy_bytes(entry.key, build_bytes, current_bytes);
		state.proxy_build_bytes = MAX(state.proxy_build_bytes, build_bytes);
		for (const KeyValue<uint64_t, Set> &set : state.sets) {
			r_native_bytes += set.value.admitted ? set.value.bytes : 0u;
		}
		if (!state.sets.is_empty() || state.last_demand_frame + NEXTGEO_RT_NATIVE_ABSENT_FRAMES >= frame) {
			base += state.proxy_build_bytes;
			base_current += current_bytes;
		}
	}
	stats.fallback_bytes = base;
	stats.fallback_compacted_bytes = base_current;
	stats.memory_cap_bytes = nextgeo_rt_native_memory_cap(base, as_budget_bytes, as_proxy_bytes);
	return stats.memory_cap_bytes;
}

void NextGeoRTNativeSets::_hold_cap() {
	uint64_t native_bytes = 0;
	const uint64_t cap = _memory(native_bytes);
	if (native_bytes <= cap) {
		return;
	}
	// The cap shrank under the sets (a mesh left, a proxy was evicted, the AS line filled): sets
	// leave until native memory is within it, untraced ones first, then those no instance traced as
	// its band's set last frame, then the farthest bands. Their buckets are refused with the set's
	// size, so they are cut again only once the cap has room for it.
	struct Candidate {
		MeshState *state = nullptr;
		uint64_t hash = 0;
		uint64_t bytes = 0;
		uint64_t mesh_id = 0;
		uint32_t rank = 0;
		uint32_t band = 0;
		bool operator<(const Candidate &p_other) const {
			if (rank != p_other.rank) {
				return rank < p_other.rank;
			}
			if (band != p_other.band) {
				return band > p_other.band;
			}
			if (bytes != p_other.bytes) {
				return bytes > p_other.bytes;
			}
			if (mesh_id != p_other.mesh_id) {
				return mesh_id < p_other.mesh_id;
			}
			return hash < p_other.hash;
		}
	};
	LocalVector<Candidate> candidates;
	for (KeyValue<RID, MeshState> &entry : meshes) {
		for (const KeyValue<uint64_t, Set> &set_entry : entry.value.sets) {
			const Set &set = set_entry.value;
			if (!set.admitted || set.bytes == 0) {
				continue;
			}
			Candidate candidate;
			candidate.state = &entry.value;
			candidate.hash = set_entry.key;
			candidate.bytes = set.bytes;
			candidate.mesh_id = entry.key.get_id();
			candidate.rank = !set.ready() ? 0u : (set.last_band_use_frame >= previous_frame ? 2u : 1u);
			for (uint32_t band = 0; band < NEXTGEO_RT_NATIVE_BAND_COUNT; band++) {
				const Bucket &bucket = entry.value.buckets[band];
				if ((bucket.has_current && bucket.current == set_entry.key) || (bucket.has_pending && bucket.pending == set_entry.key)) {
					candidate.band = band;
				}
			}
			candidates.push_back(candidate);
		}
	}
	candidates.sort();
	for (const Candidate &candidate : candidates) {
		if (native_bytes <= cap) {
			break;
		}
		for (Bucket &bucket : candidate.state->buckets) {
			const bool is_current = bucket.has_current && bucket.current == candidate.hash;
			const bool is_pending = bucket.has_pending && bucket.pending == candidate.hash;
			if (!is_current && !is_pending) {
				continue;
			}
			bucket.refused = true;
			bucket.refused_hash = candidate.hash;
			bucket.refused_bytes = candidate.bytes;
			if (is_current) {
				_drop_current(*candidate.state, bucket, false);
			}
			if (is_pending) {
				_drop_pending(*candidate.state, bucket, false);
			}
		}
		native_bytes = native_bytes > candidate.bytes ? native_bytes - candidate.bytes : 0u;
		stats.cap_releases++;
	}
}

void NextGeoRTNativeSets::begin_frame_sets(uint64_t p_frame, float p_error_per_meter, bool p_orthogonal) {
	previous_frame = frame;
	frame = p_frame;
	error_per_meter = p_error_per_meter;
	orthogonal = p_orthogonal;
	stats.instances_band = 0;
	stats.instances_previous = 0;
	stats.instances_proxy = 0;
	stats.traced_triangles_max = 0;
	stats.traced_triangles_min = 0;
	for (uint32_t i = 0; i < NEXTGEO_RT_NATIVE_EXCLUSION_MAX; i++) {
		stats.excluded[i] = 0;
	}
	stats.coarse_buckets = 0;
	stats.build_triangles = 0;
	stats.build_count = 0;
	stats.extract_triangles = 0;
	stats.extract_count = 0;
	stats.swaps = 0;

	LocalVector<RID> gone;
	for (KeyValue<RID, MeshState> &entry : meshes) {
		MeshState &state = entry.value;
		if (!_mesh_alive(entry.key)) {
			_release_state(state, false);
			gone.push_back(entry.key);
			continue;
		}
		if (state.want_attributes && state.last_attributes_frame + NEXTGI_SHADOW_HYSTERESIS_FRAMES < frame) {
			state.want_attributes = false;
		}
		for (Bucket &bucket : state.buckets) {
			// Sets MN4 reclaimed for another set of the mesh leave before that set is admitted (see
			// nextgeo_rt_native_reclaim_makes_room); their buckets cut again when next demanded.
			if (bucket.has_current) {
				const Set *set = state.sets.getptr(bucket.current);
				if (set == nullptr || set->reclaim) {
					_drop_current(state, bucket, false);
					bucket.target_error = -1.0f;
				}
			}
			if (bucket.has_pending) {
				const Set *set = state.sets.getptr(bucket.pending);
				if (set == nullptr || set->reclaim) {
					_drop_pending(state, bucket, false);
					bucket.target_error = -1.0f;
				}
			}
			if (bucket.last_use_frame + NEXTGEO_RT_NATIVE_ABSENT_FRAMES < frame) {
				_drop_current(state, bucket, false);
				_drop_pending(state, bucket, false);
				bucket = Bucket();
				continue;
			}
			if (bucket.has_pending) {
				const Set *set = state.sets.getptr(bucket.pending);
				if (set != nullptr && set->ready()) {
					// The finished set replaces the old one in this frame's TLAS; the old BLAS leaves the
					// TLAS here, and RD frees it once the frames that traced it are done.
					_drop_current(state, bucket, false);
					bucket.current = bucket.pending;
					bucket.current_eps = bucket.pending_eps;
					bucket.has_current = true;
					bucket.has_pending = false;
					stats.swaps++;
				}
			}
		}
	}
	for (const RID &mesh : gone) {
		_mesh_released(mesh, false);
		meshes.erase(mesh);
	}
	_hold_cap();
	LocalVector<uint64_t> stale_uses;
	for (const KeyValue<uint64_t, InstanceUse> &entry : instance_uses) {
		if (entry.value.frame + NEXTGEO_RT_NATIVE_ABSENT_FRAMES < frame || !meshes.has(entry.value.mesh)) {
			stale_uses.push_back(entry.key);
		}
	}
	for (uint64_t key : stale_uses) {
		instance_uses.erase(key);
	}
	_update_totals();
}

const LocalVector<NextGeoRTNativeSets::Part> *NextGeoRTNativeSets::select(RID p_mesh, uint64_t p_instance_key, const AABB &p_world_bounds, const Transform3D &p_transform, const Vector3 &p_camera, NextGeoRTNativeExclusion p_exclusion, float &r_eps) {
	if (p_exclusion != NEXTGEO_RT_NATIVE_INCLUDED) {
		stats.excluded[p_exclusion]++;
		return nullptr;
	}
	const Vector3 scale = p_transform.basis.get_scale_abs();
	const float max_scale = MAX(scale.x, MAX(scale.y, scale.z));
	uint32_t band = 0;
	if (!orthogonal) {
		const Vector3 closest = p_camera.clamp(p_world_bounds.position, p_world_bounds.position + p_world_bounds.size);
		band = nextgeo_rt_native_band(closest.distance_to(p_camera), max_scale);
	}
	MeshState *state = meshes.getptr(p_mesh);
	if (state == nullptr) {
		state = &meshes.insert(p_mesh, MeshState())->value;
		state->mesh = p_mesh;
	}
	state->last_demand_frame = frame;
	Bucket &bucket = state->buckets[band];
	bucket.last_demand_frame = frame;
	bucket.last_use_frame = frame;
	Set *own = bucket.has_current ? state->sets.getptr(bucket.current) : nullptr;
	if (own != nullptr && !own->ready()) {
		own = nullptr;
	}

	Bucket *previous_bucket = nullptr;
	Set *previous = nullptr;
	InstanceUse *use = instance_uses.getptr(p_instance_key);
	if (use != nullptr && use->mesh == p_mesh && use->band != band) {
		previous_bucket = &state->buckets[use->band];
		previous = previous_bucket->has_current ? state->sets.getptr(previous_bucket->current) : nullptr;
		if (previous != nullptr && !previous->ready()) {
			previous = nullptr;
		}
	}
	Set *traced = nullptr;
	switch (nextgeo_rt_native_choose(own != nullptr, previous != nullptr)) {
		case NEXTGEO_RT_NATIVE_USE_BAND: {
			InstanceUse new_use;
			new_use.mesh = p_mesh;
			new_use.band = band;
			new_use.frame = frame;
			instance_uses[p_instance_key] = new_use;
			own->last_band_use_frame = frame;
			stats.instances_band++;
			traced = own;
			r_eps = bucket.current_eps;
		} break;
		case NEXTGEO_RT_NATIVE_USE_PREVIOUS: {
			previous_bucket->last_use_frame = frame;
			use->frame = frame;
			stats.instances_previous++;
			traced = previous;
			r_eps = previous_bucket->current_eps;
		} break;
		default: {
			stats.instances_proxy++;
			return nullptr;
		}
	}
	stats.traced_triangles_max = MAX(stats.traced_triangles_max, traced->triangles);
	stats.traced_triangles_min = stats.instances_band + stats.instances_previous == 1u ? traced->triangles : MIN(stats.traced_triangles_min, traced->triangles);
	return &traced->parts;
}

// GodotNext: nextgi_nextgeo_alpha_shadows (#303).
void NextGeoRTNativeSets::request_mesh_attributes(RID p_mesh) {
	MeshState *state = meshes.getptr(p_mesh);
	if (state == nullptr) {
		return;
	}
	state->want_attributes = true;
	state->last_attributes_frame = frame;
}

void NextGeoRTNativeSets::build(uint32_t p_triangle_budget) {
	struct Work {
		MeshState *state = nullptr;
		uint32_t band = 0;
		bool has_current = false;
		uint64_t mesh_id = 0;
		bool operator<(const Work &p_other) const {
			// Buckets with no set first, then nearer bands; the mesh id keeps the order stable.
			if (has_current != p_other.has_current) {
				return !has_current;
			}
			if (band != p_other.band) {
				return band < p_other.band;
			}
			return mesh_id < p_other.mesh_id;
		}
	};
	LocalVector<Work> work;
	for (KeyValue<RID, MeshState> &entry : meshes) {
		for (uint32_t band = 0; band < NEXTGEO_RT_NATIVE_BAND_COUNT; band++) {
			const Bucket &bucket = entry.value.buckets[band];
			if (bucket.last_demand_frame == frame) {
				Work item;
				item.state = &entry.value;
				item.band = band;
				item.has_current = bucket.has_current;
				item.mesh_id = entry.key.get_id();
				work.push_back(item);
			}
		}
	}
	uint64_t native_bytes = 0;
	const uint64_t cap = _memory(native_bytes);
	work.sort();

	// Take the cut again where the residency or the band's target changed. A cut equal to another
	// bucket's set (current or pending) shares it; a new cut is extracted, capped like the builds.
	for (const Work &item : work) {
		MeshState &state = *item.state;
		Bucket &bucket = state.buckets[item.band];
		float eps = 0.0f;
		uint64_t generation = 0;
		if (!_mesh_cut_inputs(state.mesh, eps, generation) || !(eps > 0.0f)) {
			continue;
		}
		const float error = nextgeo_rt_native_band_error(item.band, error_per_meter, eps, orthogonal);
		const uint64_t headroom = cap > native_bytes ? cap - native_bytes : 0u;
		if (bucket.refused && headroom + _reclaimable_bytes(state, bucket.refused_hash, false) >= bucket.refused_bytes) {
			bucket.refused = false;
			bucket.target_error = -1.0f; // The cap has room for the refused set now: cut again.
		}
		if (bucket.target_error == error && bucket.generation == generation && bucket.attributes == state.want_attributes) {
			if (bucket.coarse) {
				stats.coarse_buckets++;
			}
			continue;
		}
		if (stats.extract_triangles >= uint32_t(NEXTGEO_RT_NATIVE_REBUILD_TRIANGLE_CAP)) {
			continue; // Next frame: the target stays stale until this bucket is cut.
		}
		NextGeoRTNativeCut cut;
		if (!_mesh_cut(state.mesh, error, cut)) {
			continue;
		}
		// GodotNext: nextgi_nextgeo_alpha_shadows (#303). Distinguish sets with #130 attribute streams from
		// attribute-less sets in the per-mesh pool so toggling alpha overrides swaps cleanly.
		if (state.want_attributes) {
			cut.hash ^= 0xA77B1B07E5000001ull;
		}
		bucket.target_error = error;
		bucket.generation = generation;
		bucket.attributes = state.want_attributes;
		bucket.coarse = !nextgeo_rt_native_cut_usable(cut, eps);
		if (bucket.coarse) {
			// Pages the cut needs are not resident. A built set stays (it was taken from pages that
			// were); a pending one is dropped.
			stats.coarse_buckets++;
			_drop_pending(state, bucket, false);
			continue;
		}
		// The ray origin eps: the cut is never finer than its own error, and never coarser than the
		// band's target unless pages are missing.
		const float set_eps = MAX(error, cut.max_error);
		if (bucket.has_current && cut.hash == bucket.current) {
			_drop_pending(state, bucket, false);
			bucket.current_eps = set_eps;
			bucket.refused = false;
			continue;
		}
		if (bucket.has_pending && cut.hash == bucket.pending) {
			bucket.pending_eps = set_eps;
			continue;
		}
		if (Set *shared = state.sets.getptr(cut.hash)) {
			if (shared->reclaim) {
				bucket.target_error = -1.0f; // It leaves at the next TLAS phase; cut again after that.
				continue;
			}
			_drop_pending(state, bucket, false); // Its hash differs from cut.hash: shared stays valid.
			shared->refs++;
			bucket.pending = cut.hash;
			bucket.pending_eps = set_eps;
			bucket.has_pending = true;
			bucket.refused = false;
			continue;
		}
		if (bucket.refused && cut.hash == bucket.refused_hash) {
			continue; // The cap has no room for it yet.
		}
		bucket.refused = false;
		_drop_pending(state, bucket, false);
		Set set;
		if (!_set_extract(state, cut, set)) {
			continue;
		}
		set.hash = cut.hash;
		set.has_attributes = state.want_attributes;
		set.max_error = cut.max_error;
		set.triangles = cut.triangle_count;
		set.refs = 1;
		stats.extract_triangles += cut.triangle_count;
		stats.extract_count++;
		state.sets.insert(cut.hash, set);
		bucket.pending = cut.hash;
		bucket.pending_eps = set_eps;
		bucket.has_pending = true;
	}

	// Admit each pending set as a whole against the cap, then build its parts in order within the
	// budget (N7, line 1b); a set admitted earlier continues where it stopped.
	uint32_t remaining = p_triangle_budget;
	for (const Work &item : work) {
		if (remaining == 0) {
			break;
		}
		MeshState &state = *item.state;
		Bucket &bucket = state.buckets[item.band];
		if (!bucket.has_pending) {
			continue;
		}
		const uint64_t hash = bucket.pending;
		Set *set = state.sets.getptr(hash);
		if (set == nullptr || set->parts.is_empty() || set->ready()) {
			continue;
		}
		if (!set->admitted) {
			if (set->parts[0].triangle_count > remaining) {
				remaining = 0;
				break;
			}
			uint64_t total = 0;
			bool created = true;
			for (Part &part : set->parts) {
				uint64_t bytes = 0;
				if (!_part_create(part, bytes)) {
					created = false;
					break;
				}
				part.uncompacted_bytes = bytes;
				part.compacted = false;
				total += bytes;
			}
			if (!created || !nextgeo_rt_native_fits_memory(native_bytes, total, cap)) {
				for (Part &part : set->parts) {
					if (part.blas.is_valid()) {
						_part_free_blas(part);
					}
					part.uncompacted_bytes = 0;
				}
				if (created && nextgeo_rt_native_reclaim_makes_room(native_bytes, _reclaimable_bytes(state, hash, false), total, cap)) {
					// The reclaimed sets leave at the next TLAS phase; this set is admitted after them.
					_reclaimable_bytes(state, hash, true);
					stats.reclaims++;
					continue;
				}
				// The bucket's instances keep their previous set or the proxy.
				_refuse(state, hash, created ? total : UINT64_MAX, set->triangles, cap > native_bytes ? cap - native_bytes : 0u, item.band, created);
				continue;
			}
			set->admitted = true;
			set->bytes = total;
			native_bytes += total;
		}
		for (uint32_t index = set->built; index < set->parts.size(); index++) {
			Part &part = set->parts[index];
			if (part.triangle_count > remaining) {
				remaining = 0;
				break;
			}
			_part_build(part);
			set->built++;
			remaining -= part.triangle_count;
			stats.build_triangles += part.triangle_count;
			stats.build_count++;
			PendingCompact pending;
			pending.mesh = state.mesh;
			pending.hash = hash;
			pending.part = index;
			pending.triangles = part.triangle_count;
			pending_compactions.push_back(pending);
		}
	}
	_update_totals();
}

bool NextGeoRTNativeSets::compact_after_traces(uint32_t &r_triangles_this_frame, uint32_t p_triangle_cap) {
	stats.compactions = 0;
	if (pending_compactions.is_empty()) {
		return false;
	}
	bool queued = false;
	LocalVector<PendingCompact> deferred;
	for (const PendingCompact &pending : pending_compactions) {
		MeshState *state = meshes.getptr(pending.mesh);
		Set *set = state != nullptr ? state->sets.getptr(pending.hash) : nullptr;
		if (set == nullptr || pending.part >= set->built) {
			continue;
		}
		Part &part = set->parts[pending.part];
		if (!part.blas.is_valid() || part.compacted) {
			continue;
		}
		if (r_triangles_this_frame + pending.triangles > p_triangle_cap) {
			deferred.push_back(pending);
			continue;
		}
		if (_part_compact(part)) {
			part.compacted = true;
			r_triangles_this_frame += pending.triangles;
			stats.compactions++;
			queued = true;
		}
	}
	pending_compactions = deferred;
	return queued;
}

void NextGeoRTNativeSets::set_as_line(uint64_t p_budget_bytes, uint64_t p_proxy_bytes) {
	as_budget_bytes = p_budget_bytes;
	as_proxy_bytes = p_proxy_bytes;
	stats.as_budget_bytes = p_budget_bytes;
	stats.as_proxy_bytes = p_proxy_bytes;
}

void NextGeoRTNativeSets::release_mesh(RID p_mesh) {
	// This runs from the instance and virtual-geometry updates (mesh_on_virtual_geometry_changed),
	// between frames, never between a frame's TLAS phase and its traces. The ranges are freed now, as
	// the mesh's proxy frees its own ranges of the same arena in the same callback: no NextGI frame
	// may follow to free retired ones. RD orders a later write to a reused range after the reads of
	// earlier frames (its resource trackers keep a buffer's last usage across frames).
	MeshState *state = meshes.getptr(p_mesh);
	if (state == nullptr) {
		_mesh_released(p_mesh, true);
		return;
	}
	_release_state(*state, true);
	_mesh_released(p_mesh, true);
	meshes.erase(p_mesh);
	_update_totals();
}

void NextGeoRTNativeSets::release_all() {
	for (KeyValue<RID, MeshState> &entry : meshes) {
		_release_state(entry.value, true);
		_mesh_released(entry.key, true);
	}
	meshes.clear();
	instance_uses.clear();
	pending_compactions.clear();
	stats = Stats();
}

void NextGeoRTNativeSets::_update_totals() {
	stats.buckets = 0;
	stats.current_sets = 0;
	stats.pending_sets = 0;
	stats.parts = 0;
	stats.triangles = 0;
	stats.blas_bytes = 0;
	stats.build_bytes = 0;
	stats.band_mask = 0;
	stats.shared_buckets = 0;
	stats.coarser_sets = 0;
	stats.no_card_triangles = 0;
	stats.chosen_card_triangles = 0;
	stats.proxy_card_triangles = 0;
	for (const KeyValue<RID, MeshState> &entry : meshes) {
		const MeshState &state = entry.value;
		for (uint32_t band = 0; band < NEXTGEO_RT_NATIVE_BAND_COUNT; band++) {
			const Bucket &bucket = state.buckets[band];
			if (!bucket.has_current && !bucket.has_pending) {
				continue;
			}
			stats.buckets++;
			if (!bucket.has_current) {
				continue;
			}
			stats.band_mask |= 1u << band;
			for (uint32_t other = 0; other < band; other++) {
				if (state.buckets[other].has_current && state.buckets[other].current == bucket.current) {
					stats.shared_buckets++;
					break;
				}
			}
		}
		for (const KeyValue<uint64_t, Set> &set_entry : state.sets) {
			const Set &set = set_entry.value;
			stats.build_bytes += set.admitted ? set.bytes : 0u;
			for (const Part &part : set.parts) {
				if (part.blas.is_valid()) {
					stats.blas_bytes += _part_bytes(part);
				}
			}
			bool traced = false;
			for (const Bucket &bucket : state.buckets) {
				if (bucket.has_current && bucket.current == set_entry.key) {
					traced = true;
					break;
				}
			}
			if (!traced) {
				stats.pending_sets++;
				continue;
			}
			stats.current_sets++;
			stats.parts += set.parts.size();
			stats.triangles += set.triangles;
			stats.coarser_sets += set.triangles < set.proxy_triangles ? 1u : 0u;
			stats.no_card_triangles += set.no_card_triangles;
			stats.chosen_card_triangles += set.chosen_card_triangles;
			stats.proxy_card_triangles += set.proxy_card_triangles;
		}
	}
	_update_card_totals();
}

/* NextGIRTNative */

struct NextGIRTNative::Extractor {
	RtCutExtractShaderRD shader;
	RID version;
	RID pipeline;
};

NextGIRTNative::NextGIRTNative() {
	if (MeshStorage::get_singleton() != nullptr) {
		MeshStorage::get_singleton()->mesh_nextgi_set_virtual_geometry_changed_callback(&NextGIRTNative::_virtual_geometry_changed, this);
	}
}

NextGIRTNative::~NextGIRTNative() {
	free();
}

void NextGIRTNative::_virtual_geometry_changed(void *p_userdata, RID p_mesh) {
	static_cast<NextGIRTNative *>(p_userdata)->release_mesh(p_mesh);
}

bool NextGIRTNative::_ensure_extractor() {
	if (extractor != nullptr) {
		return extractor->pipeline.is_valid();
	}
	RenderingDevice *rd = RD::get_singleton();
	ERR_FAIL_NULL_V(rd, false);
	extractor = memnew(Extractor);
	Vector<String> defines;
	defines.push_back("");
	extractor->shader.initialize(defines);
	extractor->version = extractor->shader.version_create();
	const RID shader = extractor->shader.version_get_shader(extractor->version, 0);
	if (shader.is_valid()) {
		extractor->pipeline = rd->compute_pipeline_create(shader);
	}
	return extractor->pipeline.is_valid();
}

uint32_t NextGIRTNative::_excluded_surfaces(RID p_mesh) {
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (mesh_storage == nullptr || !mesh_storage->owns_mesh(p_mesh)) {
		return 0;
	}
	uint32_t mask = 0;
	const uint32_t surface_count = MIN(uint32_t(mesh_storage->mesh_get_surface_count(p_mesh)), 32u);
	for (uint32_t surface = 0; surface < surface_count; surface++) {
		// The proxy's rule (#61): alpha-tested and blended surfaces stay out of the TLAS.
		if (nextgi_rt_material_is_alpha(mesh_storage->mesh_surface_get_material(p_mesh, int(surface)))) {
			mask |= 1u << surface;
		}
	}
	return mask;
}

void NextGIRTNative::_mesh_proxy_bytes(RID p_mesh, uint64_t &r_build_bytes, uint64_t &r_current_bytes) {
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	RenderingDevice *rd = RD::get_singleton();
	if (mesh_storage == nullptr || rd == nullptr || !mesh_storage->owns_mesh(p_mesh)) {
		return;
	}
	const LocalVector<MeshStorage::NextGIRTPart> *parts = mesh_storage->mesh_nextgi_rt_get_parts(p_mesh);
	if (parts == nullptr) {
		return;
	}
	for (const MeshStorage::NextGIRTPart &part : *parts) {
		if (part.is_proxy && part.blas.is_valid()) {
			const uint64_t current = rd->acceleration_structure_get_size_bytes(part.blas);
			r_build_bytes += part.uncompacted_bytes > 0 ? part.uncompacted_bytes : current;
			r_current_bytes += current;
		}
	}
}

void NextGIRTNative::_refresh_parents(CardState &r_state, const RendererVirtualMeshStorage::MeshInfo &p_info, const NextGeoRTNativeDag &p_dag) {
	const uint32_t first_cluster = p_info.record.cluster_offset;
	const uint32_t cluster_count = p_info.cluster_refine.size();
	if (r_state.cluster_group.size() != cluster_count || r_state.parent_of.size() != p_dag.group_count) {
		r_state.cluster_group.resize(cluster_count);
		for (uint32_t i = 0; i < cluster_count; i++) {
			r_state.cluster_group[i] = UINT32_MAX;
		}
		for (uint32_t group = 0; group < p_dag.group_count; group++) {
			const uint32_t offset = p_dag.group_cluster_offset(group);
			const uint32_t count = p_dag.group_cluster_count(group);
			for (uint32_t i = 0; i < count; i++) {
				if (offset + i >= first_cluster && offset + i - first_cluster < cluster_count) {
					r_state.cluster_group[offset + i - first_cluster] = group;
				}
			}
		}
		r_state.parent_of.resize(p_dag.group_count);
		r_state.group_card.resize(p_dag.group_count);
		for (uint32_t group = 0; group < p_dag.group_count; group++) {
			r_state.parent_of[group] = UINT32_MAX;
			r_state.group_card[group] = NATIVE_CARD_UNKNOWN;
		}
		r_state.cards_checked = false;
	}
	// A resident cluster that refines group R lies in a parent of R (pages are parents-first, so the
	// chain above any resident cluster is resident too). R's simplified clusters can land in several
	// parent groups: parent_of keeps one, parents all of them.
	LocalVector<uint64_t> edges;
	for (uint32_t i = 0; i < cluster_count; i++) {
		const RendererVirtualMeshStorage::ClusterRefineInfo &refine = p_info.cluster_refine[i];
		if (refine.valid && refine.refined_group >= 0 && uint32_t(refine.refined_group) < p_dag.group_count && r_state.cluster_group[i] != UINT32_MAX) {
			r_state.parent_of[refine.refined_group] = r_state.cluster_group[i];
			edges.push_back((uint64_t(refine.refined_group) << 32u) | uint64_t(r_state.cluster_group[i]));
		}
	}
	edges.sort();
	r_state.parent_first.resize(p_dag.group_count + 1u);
	for (uint32_t group = 0; group <= p_dag.group_count; group++) {
		r_state.parent_first[group] = 0;
	}
	r_state.parents.clear();
	for (uint32_t i = 0; i < edges.size(); i++) {
		if (i > 0 && edges[i] == edges[i - 1]) {
			continue;
		}
		r_state.parent_first[uint32_t(edges[i] >> 32u) + 1u]++;
		r_state.parents.push_back(uint32_t(edges[i] & 0xFFFFFFFFu));
	}
	for (uint32_t group = 0; group < p_dag.group_count; group++) {
		r_state.parent_first[group + 1u] += r_state.parent_first[group];
	}
}

uint16_t NextGIRTNative::_card_group(CardState &r_state, const RendererVirtualMeshStorage::MeshInfo &p_info, const NextGeoRTNativeDag &p_dag, uint32_t p_group, float p_proxy_eps) {
	const uint32_t proxy_group = nextgeo_rt_native_proxy_group(p_dag, p_group, r_state.parent_of.ptr(), r_state.parent_of.size(), p_proxy_eps);
	if (proxy_group == UINT32_MAX || proxy_group >= r_state.group_card.size()) {
		return NEXTGEO_RT_NATIVE_NO_CARD;
	}
	const uint8_t *cards = p_info.desc.card_descriptors.ptr();
	const uint32_t card_count = uint32_t(p_info.desc.card_descriptors.size() / NATIVE_CARD_BYTES);
	if (!r_state.cards_checked) {
		r_state.cards_direct = nextgeo_rt_native_cards_are_dag_groups(p_dag, cards, card_count, p_proxy_eps);
		r_state.cards_checked = true;
	}
	uint16_t &memo = r_state.group_card[proxy_group];
	if (memo == NATIVE_CARD_UNKNOWN) {
		memo = nextgeo_rt_native_card_of_proxy_group(p_dag, proxy_group, r_state.cards_direct, cards, card_count, p_proxy_eps);
	}
	return memo;
}

bool NextGIRTNative::_map_proxy(CardState &r_state, const RendererVirtualMeshStorage::MeshInfo &p_info) {
	if (r_state.proxy_map) {
		return true;
	}
	if (r_state.proxy_map_tried == p_info.residency_generation) {
		return false;
	}
	r_state.proxy_map_tried = p_info.residency_generation;
	const PackedInt32Array &clusters = p_info.desc.rt_proxy_clusters;
	const uint32_t total = uint32_t(p_info.desc.rt_proxy_triangle_groups.size() / 2);
	if (clusters.is_empty() || total == 0 || r_state.cluster_group.size() != p_info.cluster_refine.size()) {
		return false;
	}
	const RendererVirtualMeshStorage::ClusterGPUDesc *descs = reinterpret_cast<const RendererVirtualMeshStorage::ClusterGPUDesc *>(p_info.cluster_blob.ptr());
	const uint32_t desc_count = uint32_t(p_info.cluster_blob.size() / sizeof(RendererVirtualMeshStorage::ClusterGPUDesc));
	const uint32_t first_cluster = p_info.record.cluster_offset;
	const uint32_t count = uint32_t(clusters.size());
	LocalVector<uint32_t> locals;
	LocalVector<uint32_t> triangles;
	LocalVector<uint32_t> surfaces;
	locals.resize(count);
	triangles.resize(count);
	surfaces.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		const int64_t global_cluster = clusters[i];
		if (global_cluster < int64_t(first_cluster)) {
			return false;
		}
		const uint32_t local = uint32_t(global_cluster - first_cluster);
		// Every proxy cluster must be resident to rebuild the order; a usable native cut implies it
		// (it is nowhere coarser than the proxy, and pages are parents-first).
		if (local >= desc_count || local >= p_info.cluster_refine.size() || !p_info.cluster_refine[local].valid) {
			return false;
		}
		locals[i] = local;
		triangles[i] = (descs[local].stride_and_counts >> 16u) & 0xFFu;
		surfaces[i] = descs[local].surface_and_page & 0xFFFFu;
	}
	LocalVector<uint32_t> bases;
	if (!nextgeo_rt_native_proxy_triangle_bases(triangles.ptr(), surfaces.ptr(), count, NEXTGI_RT_MAX_TRIANGLES_PER_BLAS, total, bases)) {
		return false;
	}
	r_state.proxy_base.clear();
	for (uint32_t i = 0; i < count; i++) {
		r_state.proxy_base.insert(locals[i], bases[i]);
	}
	// Each proxy-level group's card groups (its depth layers), most common first, for the finer
	// clusters below it.
	const uint8_t *groups = p_info.desc.rt_proxy_triangle_groups.ptr();
	const uint32_t group_count = r_state.parent_of.size();
	HashMap<uint64_t, uint32_t> votes;
	for (uint32_t i = 0; i < count; i++) {
		const uint32_t group = r_state.cluster_group[locals[i]];
		if (group == UINT32_MAX) {
			continue;
		}
		for (uint32_t t = 0; t < triangles[i]; t++) {
			const uint64_t key = (uint64_t(group) << 16u) | _native_u16(groups, (bases[i] + t) * 2u);
			if (uint32_t *vote = votes.getptr(key)) {
				(*vote)++;
			} else {
				votes.insert(key, 1u);
			}
		}
	}
	struct Candidate {
		uint32_t group = 0;
		uint32_t votes = 0;
		uint16_t card = 0;
		bool operator<(const Candidate &p_other) const {
			if (group != p_other.group) {
				return group < p_other.group;
			}
			if (votes != p_other.votes) {
				return votes > p_other.votes;
			}
			return card < p_other.card;
		}
	};
	LocalVector<Candidate> candidates;
	for (const KeyValue<uint64_t, uint32_t> &entry : votes) {
		Candidate candidate;
		candidate.group = uint32_t(entry.key >> 16u);
		candidate.votes = entry.value;
		candidate.card = uint16_t(entry.key & 0xFFFFu);
		if (candidate.group < group_count) {
			candidates.push_back(candidate);
		}
	}
	candidates.sort();
	r_state.group_candidate_first.resize(group_count + 1u);
	for (uint32_t group = 0; group <= group_count; group++) {
		r_state.group_candidate_first[group] = 0;
	}
	for (const Candidate &candidate : candidates) {
		r_state.group_candidate_first[candidate.group + 1u]++;
	}
	for (uint32_t group = 0; group < group_count; group++) {
		r_state.group_candidate_first[group + 1u] += r_state.group_candidate_first[group];
	}
	r_state.group_candidates.resize(candidates.size());
	for (uint32_t i = 0; i < candidates.size(); i++) {
		r_state.group_candidates[i] = candidates[i].card; // Sorted by group, so in place.
	}
	r_state.proxy_map = true;
	return true;
}

bool NextGIRTNative::_set_extract(MeshState &r_mesh, const NextGeoRTNativeCut &p_cut, Set &r_set) {
	RenderingDevice *rd = RD::get_singleton();
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	RendererVirtualMeshStorage *virtual_storage = RSG::virtual_mesh_storage;
	if (rd == nullptr || mesh_storage == nullptr || virtual_storage == nullptr || !_ensure_extractor()) {
		return false;
	}
	const RendererVirtualMeshStorage::MeshInfo *info_ptr = virtual_storage->get_meshes().getptr(r_mesh.mesh);
	if (info_ptr == nullptr) {
		return false;
	}
	const RendererVirtualMeshStorage::MeshInfo &mesh_info = *info_ptr;
	const bool has_cards = !mesh_info.desc.card_descriptors.is_empty();
	CardState &card_state = card_states[r_mesh.mesh];
	const RID page_buffer = virtual_storage->get_page_buffer();
	NextGeoRTNativeDag dag;
	if (page_buffer.is_null() || !dag.init(mesh_info.desc.dag_header.ptr(), uint32_t(mesh_info.desc.dag_header.size()))) {
		return false;
	}
	const RendererVirtualMeshStorage::ClusterGPUDesc *descs = reinterpret_cast<const RendererVirtualMeshStorage::ClusterGPUDesc *>(mesh_info.cluster_blob.ptr());
	const uint32_t cluster_count = p_cut.clusters.size();
	LocalVector<uint32_t> triangles;
	LocalVector<uint32_t> vertices;
	LocalVector<uint32_t> surfaces;
	triangles.resize(cluster_count);
	vertices.resize(cluster_count);
	surfaces.resize(cluster_count);
	for (uint32_t i = 0; i < cluster_count; i++) {
		const RendererVirtualMeshStorage::ClusterGPUDesc &desc = descs[p_cut.clusters[i]];
		vertices[i] = (desc.stride_and_counts >> 8u) & 0xFFu;
		triangles[i] = (desc.stride_and_counts >> 16u) & 0xFFu;
		surfaces[i] = desc.surface_and_page & 0xFFFFu;
	}
	LocalVector<NextGeoRTNativePartPlan> plans;
	nextgeo_rt_native_split_parts(triangles.ptr(), vertices.ptr(), surfaces.ptr(), cluster_count, NEXTGI_RT_MAX_TRIANGLES_PER_BLAS, plans);

	// Stage-2 card groups (#77). A cluster the cut shares with the proxy takes the proxy's own group
	// per triangle. A finer cluster's triangles choose on the GPU (rt_cut_extract.glsl, cards pass),
	// each among the cards of the proxy-level groups above the cluster (its candidates), then among
	// all cards; a proxy-level group's cards are depth layers, and its layers on one axis are
	// unrelated walls on another, so no per-cluster group can be right. The default, for a triangle
	// with no card on its axis, is the most common card group of the walked proxy-level group, or,
	// without the proxy's order (an older import), the card of that group found by its bounds.
	struct Choice {
		uint32_t candidate_first = 0; // In the plan's candidate list.
		uint32_t candidate_count = 0;
		uint16_t fallback = NEXTGEO_RT_NATIVE_NO_CARD;
	};
	LocalVector<Choice> choices;
	LocalVector<uint32_t> candidates; // Card descriptor indices.
	LocalVector<uint32_t> cluster_choice; // Per cut cluster: its choice, or UINT32_MAX for exact groups.
	LocalVector<uint32_t> cluster_proxy_base;
	const uint8_t *proxy_groups = mesh_info.desc.rt_proxy_triangle_groups.ptr();
	const uint32_t proxy_group_count = uint32_t(mesh_info.desc.rt_proxy_triangle_groups.size() / 2);
	const uint8_t *cards = mesh_info.desc.card_descriptors.ptr();
	const uint32_t card_count = uint32_t(mesh_info.desc.card_descriptors.size() / NATIVE_CARD_BYTES);
	const float proxy_eps = mesh_info.desc.rt_proxy_eps;
	if (has_cards) {
		_refresh_parents(card_state, mesh_info, dag);
		const bool proxy_map = _map_proxy(card_state, mesh_info);
		cluster_choice.resize(cluster_count);
		cluster_proxy_base.resize(cluster_count);
		HashMap<uint32_t, uint32_t> group_choice; // DAG group -> its clusters' choice.
		LocalVector<uint32_t> ancestors;
		const uint32_t candidate_groups = card_state.group_candidate_first.is_empty() ? 0u : card_state.group_candidate_first.size() - 1u;
		for (uint32_t i = 0; i < cluster_count; i++) {
			const uint32_t local = p_cut.clusters[i];
			cluster_proxy_base[i] = UINT32_MAX;
			cluster_choice[i] = UINT32_MAX;
			if (proxy_map) {
				if (const uint32_t *base = card_state.proxy_base.getptr(local)) {
					cluster_proxy_base[i] = *base;
					continue;
				}
			}
			const uint32_t group = local < card_state.cluster_group.size() ? card_state.cluster_group[local] : UINT32_MAX;
			if (const uint32_t *existing = group_choice.getptr(group)) {
				cluster_choice[i] = *existing;
				continue;
			}
			Choice choice;
			choice.candidate_first = candidates.size();
			if (group != UINT32_MAX && proxy_map) {
				nextgeo_rt_native_proxy_ancestors(dag, group, card_state.parent_first.ptr(), card_state.parents.ptr(), card_state.parent_first.size() > 0 ? card_state.parent_first.size() - 1u : 0u, proxy_eps, ancestors);
				for (uint32_t ancestor : ancestors) {
					if (ancestor >= candidate_groups) {
						continue;
					}
					for (uint32_t c = card_state.group_candidate_first[ancestor]; c < card_state.group_candidate_first[ancestor + 1u]; c++) {
						uint32_t first = 0;
						uint32_t end = 0;
						nextgeo_rt_native_card_range(cards, card_count, card_state.group_candidates[c], first, end);
						for (uint32_t card = first; card < end; card++) {
							candidates.push_back(card);
						}
					}
				}
				const uint32_t walked = nextgeo_rt_native_proxy_group(dag, group, card_state.parent_of.ptr(), card_state.parent_of.size(), proxy_eps);
				if (walked < candidate_groups && card_state.group_candidate_first[walked] < card_state.group_candidate_first[walked + 1u]) {
					choice.fallback = card_state.group_candidates[card_state.group_candidate_first[walked]];
				}
			} else if (group != UINT32_MAX) {
				choice.fallback = _card_group(card_state, mesh_info, dag, group, proxy_eps);
			}
			choice.candidate_count = candidates.size() - choice.candidate_first;
			cluster_choice[i] = choices.size();
			group_choice.insert(group, choices.size());
			choices.push_back(choice);
		}
	}

	const RID shader = extractor->shader.version_get_shader(extractor->version, 0);
	Set &set = r_set;
	set = Set();
	// What the proxy puts in the TLAS for the same surfaces: a set never has fewer triangles (it is
	// nowhere coarser), which the coarser_sets counter checks.
	const uint32_t excluded_surfaces = _excluded_surfaces(r_mesh.mesh);
	if (const LocalVector<MeshStorage::NextGIRTPart> *proxy_parts = mesh_storage->mesh_nextgi_rt_get_parts(r_mesh.mesh)) {
		for (const MeshStorage::NextGIRTPart &proxy_part : *proxy_parts) {
			for (const NextGIRTPartGeometry &geometry : proxy_part.geometries) {
				if (proxy_part.is_proxy && geometry.is_proxy_surface && !(geometry.surface < 32u && (excluded_surfaces & (1u << geometry.surface)) != 0u)) {
					set.proxy_triangles += geometry.triangle_count;
				}
			}
		}
	}
	const bool want_attributes = r_mesh.want_attributes;
	for (const NextGeoRTNativePartPlan &plan : plans) {
		NextGeoRTNativePartLayout layout;
		nextgeo_rt_native_part_layout(plan, layout, want_attributes);
		const bool with_attributes = want_attributes && layout.attribute_offset != UINT32_MAX && layout.attribute_scratch_offset != UINT32_MAX;
		const NextGIArenaAllocator::Allocation allocation = mesh_storage->mesh_nextgi_rt_arena_allocate(layout.size);
		const RID chunk_buffer = allocation.is_valid() ? mesh_storage->mesh_nextgi_rt_get_chunk_buffer(allocation.chunk) : RID();
		if (chunk_buffer.is_null()) {
			mesh_storage->mesh_nextgi_rt_arena_free(allocation);
			for (Part &done : set.parts) {
				_part_free_range(r_mesh.mesh, done, false); // Their extraction is recorded this frame.
			}
			set = Set();
			return false;
		}
		MeshStorage::NextGIRTPart part;
		part.is_proxy = true;
		part.chunk = allocation.chunk;
		part.allocation_offset = allocation.offset;
		part.allocation_size = allocation.size;
		part.triangle_count = plan.triangle_count;
		part.first_surface = plan.runs.is_empty() ? 0u : plan.runs[0].surface;
		part.surface_count = plan.runs.size();

		// The words of rt_cut_extract.glsl: extract jobs, then (with cards) the choices, their
		// candidates, a seed per triangle and the card descriptors.
		const bool with_cards = has_cards && !layout.run_group_offsets.is_empty();
		const uint32_t job_words = plan.cluster_count * 8u;
		const uint32_t choice_word = job_words;
		const uint32_t candidate_word = choice_word + (with_cards ? choices.size() * 4u : 0u);
		const uint32_t seed_word = candidate_word + (with_cards ? candidates.size() : 0u);
		const uint32_t card_word = seed_word + (with_cards ? plan.triangle_count : 0u);
		// Only chosen triangles read the descriptors.
		const uint32_t card_words = (with_cards && !choices.is_empty()) ? card_count * 8u : 0u;
		const uint32_t word_count = card_word + card_words;
		Vector<uint8_t> word_bytes;
		word_bytes.resize(int64_t(word_count) * sizeof(uint32_t));
		uint32_t *words = reinterpret_cast<uint32_t *>(word_bytes.ptrw());
		if (with_cards) {
			for (uint32_t c = 0; c < choices.size(); c++) {
				words[choice_word + c * 4u + 0u] = candidate_word + choices[c].candidate_first;
				words[choice_word + c * 4u + 1u] = choices[c].candidate_count;
				words[choice_word + c * 4u + 2u] = choices[c].fallback;
				words[choice_word + c * 4u + 3u] = 0u;
			}
			for (uint32_t c = 0; c < candidates.size(); c++) {
				words[candidate_word + c] = candidates[c];
			}
			if (card_words > 0) {
				memcpy(words + card_word, cards, size_t(card_count) * NATIVE_CARD_BYTES);
			}
		}
		uint32_t vertex_base = 0;
		uint32_t triangle_base = 0;
		for (uint32_t k = 0; k < plan.cluster_count; k++) {
			const uint32_t cut_index = plan.first_cluster + k;
			const RendererVirtualMeshStorage::ClusterGPUDesc &desc = descs[p_cut.clusters[cut_index]];
			uint32_t *job = words + k * 8;
			job[0] = desc.pos_word_offset;
			job[1] = desc.tri_byte_offset;
			job[2] = desc.stride_and_counts; // Keep bits 24..31 (vertex_layout) for attribute extraction (#303).
			job[3] = 0;
			job[4] = (allocation.offset + vertex_base * 3u * uint32_t(sizeof(float))) / 4u;
			job[5] = (allocation.offset + layout.index_offset + triangle_base * 3u * uint32_t(sizeof(uint32_t))) / 4u;
			job[6] = vertex_base;
			job[7] = 0;
			if (with_cards) {
				const uint32_t proxy_base = cluster_proxy_base[cut_index];
				const uint32_t choice = cluster_choice[cut_index];
				for (uint32_t t = 0; t < triangles[cut_index]; t++) {
					uint32_t seed = NEXTGEO_RT_NATIVE_NO_CARD;
					if (proxy_base != UINT32_MAX && proxy_base + t < proxy_group_count) {
						seed = _native_u16(proxy_groups, (proxy_base + t) * 2u);
						set.proxy_card_triangles++;
					} else if (choice != UINT32_MAX) {
						seed = 0x80000000u | choice;
						set.chosen_card_triangles++;
						if (choices[choice].candidate_count == 0 && choices[choice].fallback == NEXTGEO_RT_NATIVE_NO_CARD && card_count > uint32_t(NEXTGEO_RT_NATIVE_CARD_SEARCH_ALL_LIMIT)) {
							set.no_card_triangles++;
						}
					}
					if (seed == NEXTGEO_RT_NATIVE_NO_CARD) {
						set.no_card_triangles++;
					}
					words[seed_word + triangle_base + t] = seed;
				}
			}
			vertex_base += vertices[cut_index];
			triangle_base += triangles[cut_index];
		}

		for (uint32_t r = 0; r < plan.runs.size(); r++) {
			const NextGeoRTNativeRun &plan_run = plan.runs[r];
			NextGIRTPartGeometry geometry;
			geometry.surface = plan_run.surface;
			geometry.first_triangle = 0;
			geometry.triangle_count = plan_run.triangle_count;
			geometry.is_proxy_surface = true;
			geometry.is_legacy_surface = false;
			geometry.has_stage2_cards = with_cards;
			geometry.vertex_offset = allocation.offset;
			geometry.vertex_count = plan.vertex_count;
			geometry.index_offset = allocation.offset + layout.index_offset + plan_run.first_triangle * 3u * uint32_t(sizeof(uint32_t));
			geometry.group_offset = with_cards ? allocation.offset + layout.run_group_offsets[r] : 0u;
			if (with_attributes) {
				geometry.attribute_chunk = allocation.chunk;
				geometry.attribute_offset = allocation.offset + layout.attribute_offset;
			}
			// The cut's own error; select() gives each bucket's ray origin eps (it may share the set).
			geometry.eps = p_cut.max_error;
			part.geometries.push_back(geometry);
		}

		if (with_attributes) {
			const uint32_t init_scratch[8] = {
				0xFFFFFFFFu, 0xFFFFFFFFu, 0u, 0u,
				0xFFFFFFFFu, 0xFFFFFFFFu, 0u, 0u,
			};
			rd->buffer_update(chunk_buffer, allocation.offset + layout.attribute_scratch_offset, sizeof(init_scratch), init_scratch);
		}
		const RID word_buffer = rd->storage_buffer_create(word_bytes.size(), word_bytes);
		RD::Uniform u_pages(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, page_buffer);
		RD::Uniform u_words(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, word_buffer);
		RD::Uniform u_arena(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, chunk_buffer);
		RD::Uniform uniforms[3] = { u_pages, u_words, u_arena };
		const RID uniform_set = word_buffer.is_valid() ? rd->uniform_set_create(VectorView<RD::Uniform>(uniforms, 3), shader, 0) : RID();
		if (uniform_set.is_null()) {
			if (word_buffer.is_valid()) {
				rd->free_rid(word_buffer);
			}
			mesh_storage->mesh_nextgi_rt_arena_free(allocation);
			for (Part &done : set.parts) {
				_part_free_range(r_mesh.mesh, done, false);
			}
			set = Set();
			return false;
		}
		const RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, extractor->pipeline);
		rd->compute_list_bind_uniform_set(list, uniform_set, 0);
		uint32_t push_constants[12] = {};
		const uint32_t scratch_word = with_attributes ? (allocation.offset + layout.attribute_scratch_offset) / 4u : 0u;
		for (uint32_t offset = 0; offset < plan.cluster_count; offset += NEXTGEO_RT_NATIVE_MAX_JOBS_PER_DISPATCH) {
			const uint32_t count = MIN(plan.cluster_count - offset, uint32_t(NEXTGEO_RT_NATIVE_MAX_JOBS_PER_DISPATCH));
			push_constants[0] = 0u; // Extract.
			push_constants[1] = offset;
			push_constants[2] = count;
			push_constants[3] = scratch_word;
			rd->compute_list_set_push_constant(list, push_constants, sizeof(push_constants));
			rd->compute_list_dispatch(list, count, 1, 1);
		}
		if (with_cards) {
			// The cards pass reads the positions and indices the extract pass wrote.
			rd->compute_list_add_barrier(list);
			const float tolerance = 2.0f * proxy_eps;
			for (uint32_t r = 0; r < plan.runs.size(); r++) {
				const NextGeoRTNativeRun &plan_run = plan.runs[r];
				const uint32_t word_total = (plan_run.triangle_count + 1u) / 2u;
				if (word_total == 0) {
					continue;
				}
				push_constants[0] = 1u; // Cards.
				push_constants[1] = seed_word + plan_run.first_triangle;
				push_constants[2] = plan_run.triangle_count;
				push_constants[3] = (allocation.offset + layout.run_group_offsets[r]) / 4u;
				push_constants[4] = allocation.offset / 4u;
				push_constants[5] = (allocation.offset + layout.index_offset) / 4u + plan_run.first_triangle * 3u;
				push_constants[6] = choice_word;
				push_constants[7] = card_word;
				push_constants[8] = card_count;
				push_constants[9] = uint32_t(NEXTGEO_RT_NATIVE_CARD_SEARCH_ALL_LIMIT);
				memcpy(&push_constants[10], &tolerance, sizeof(float));
				push_constants[11] = 0u;
				rd->compute_list_set_push_constant(list, push_constants, sizeof(push_constants));
				rd->compute_list_dispatch(list, (word_total + 63u) / 64u, 1, 1);
			}
		}
		if (with_attributes) {
			// Order after Mode 0's UV scratch writes (and any Mode 1 arena writes) before packing #130 attributes.
			rd->compute_list_add_barrier(list);
			const uint32_t attr_word = (allocation.offset + layout.attribute_offset) / 4u;
			memset(push_constants, 0, sizeof(push_constants));
			for (uint32_t offset = 0; offset < plan.cluster_count; offset += NEXTGEO_RT_NATIVE_MAX_JOBS_PER_DISPATCH) {
				const uint32_t count = MIN(plan.cluster_count - offset, uint32_t(NEXTGEO_RT_NATIVE_MAX_JOBS_PER_DISPATCH));
				push_constants[0] = 2u; // Attributes (#303).
				push_constants[1] = offset;
				push_constants[2] = count;
				push_constants[3] = scratch_word;
				push_constants[4] = attr_word;
				rd->compute_list_set_push_constant(list, push_constants, sizeof(push_constants));
				rd->compute_list_dispatch(list, count, 1, 1);
			}
		}
		rd->compute_list_end();
		rd->free_rid(uniform_set);
		rd->free_rid(word_buffer);
		set.parts.push_back(part);
	}
	return true;
}

bool NextGIRTNative::_mesh_alive(RID p_mesh) {
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	RendererVirtualMeshStorage *virtual_storage = RSG::virtual_mesh_storage;
	return mesh_storage != nullptr && mesh_storage->owns_mesh(p_mesh) && virtual_storage != nullptr && virtual_storage->mesh_has_virtual_geometry(p_mesh) && virtual_storage->mesh_get_live_instance_count(p_mesh) > 0;
}

bool NextGIRTNative::_mesh_cut_inputs(RID p_mesh, float &r_proxy_eps, uint64_t &r_generation) {
	RendererVirtualMeshStorage *virtual_storage = RSG::virtual_mesh_storage;
	if (virtual_storage == nullptr || virtual_storage->get_page_buffer().is_null()) {
		return false;
	}
	const RendererVirtualMeshStorage::MeshInfo *info = virtual_storage->get_meshes().getptr(p_mesh);
	if (info == nullptr) {
		return false;
	}
	r_proxy_eps = info->desc.rt_proxy_eps;
	r_generation = info->residency_generation;
	return true;
}

bool NextGIRTNative::_mesh_cut(RID p_mesh, float p_error, NextGeoRTNativeCut &r_cut) {
	RendererVirtualMeshStorage *virtual_storage = RSG::virtual_mesh_storage;
	const RendererVirtualMeshStorage::MeshInfo *info = virtual_storage != nullptr ? virtual_storage->get_meshes().getptr(p_mesh) : nullptr;
	return info != nullptr && nextgeo_rt_native_select_cut(*virtual_storage, *info, p_error, _excluded_surfaces(p_mesh), r_cut);
}

bool NextGIRTNative::_part_create(Part &r_part, uint64_t &r_bytes) {
	RenderingDevice *rd = RD::get_singleton();
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (rd == nullptr || mesh_storage == nullptr) {
		return false;
	}
	const RID chunk_buffer = mesh_storage->mesh_nextgi_rt_get_chunk_buffer(r_part.chunk);
	if (chunk_buffer.is_null()) {
		return false;
	}
	LocalVector<RD::AccelerationStructureGeometry> geometries;
	for (const NextGIRTPartGeometry &part_geometry : r_part.geometries) {
		RD::AccelerationStructureGeometry geometry;
		geometry.flags = RD::ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT;
		geometry.vertex_buffer = chunk_buffer;
		geometry.vertex_offset = part_geometry.vertex_offset;
		geometry.vertex_stride = sizeof(float) * 3;
		geometry.vertex_count = part_geometry.vertex_count;
		geometry.vertex_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;
		geometry.index_buffer = chunk_buffer;
		geometry.index_offset = part_geometry.index_offset;
		geometry.index_count = part_geometry.triangle_count * 3;
		geometry.index_format = RD::INDEX_BUFFER_FORMAT_UINT32;
		geometries.push_back(geometry);
	}
	const BitField<RD::AccelerationStructureFlagBits> flags = RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT | RD::ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT;
	r_part.blas = rd->blas_create(Span<RD::AccelerationStructureGeometry>(geometries.ptr(), geometries.size()), flags);
	if (r_part.blas.is_null()) {
		return false;
	}
	r_bytes = rd->acceleration_structure_get_size_bytes(r_part.blas);
	return true;
}

void NextGIRTNative::_part_build(Part &r_part) {
	RD::get_singleton()->blas_build(r_part.blas);
}

bool NextGIRTNative::_part_compact(Part &r_part) {
	RenderingDevice *rd = RD::get_singleton();
	return rd != nullptr && rd->blas_compact(r_part.blas) == OK;
}

uint64_t NextGIRTNative::_part_bytes(const Part &p_part) const {
	RenderingDevice *rd = RD::get_singleton();
	return rd != nullptr ? rd->acceleration_structure_get_size_bytes(p_part.blas) : 0u;
}

void NextGIRTNative::_part_free_blas(Part &r_part) {
	RenderingDevice *rd = RD::get_singleton();
	if (rd != nullptr) {
		rd->free_rid(r_part.blas);
	}
	r_part.blas = RID();
}

void NextGIRTNative::_part_free_range(RID p_mesh, Part &r_part, bool p_now) {
	if (r_part.allocation_size == 0 || r_part.chunk == UINT32_MAX) {
		return;
	}
	NextGIArenaAllocator::Allocation allocation;
	allocation.chunk = r_part.chunk;
	allocation.offset = r_part.allocation_offset;
	allocation.size = r_part.allocation_size;
	if (p_now) {
		MeshStorage *mesh_storage = MeshStorage::get_singleton();
		if (mesh_storage != nullptr) {
			mesh_storage->mesh_nextgi_rt_arena_free(allocation);
		}
	} else {
		// Hit shading of frames still in flight reads the positions; reuse the range later.
		Retired entry;
		entry.allocation = allocation;
		entry.frame = frame;
		entry.mesh = p_mesh;
		retired.push_back(entry);
	}
	r_part.chunk = UINT32_MAX;
	r_part.allocation_size = 0;
}

void NextGIRTNative::_mesh_released(RID p_mesh, bool p_now) {
	card_states.erase(p_mesh);
	if (!p_now) {
		return;
	}
	// Ranges of the mesh's replaced or refused sets go with it too: no NextGI frame may follow.
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	for (uint32_t i = 0; i < retired.size();) {
		if (retired[i].mesh == p_mesh) {
			if (mesh_storage != nullptr) {
				mesh_storage->mesh_nextgi_rt_arena_free(retired[i].allocation);
			}
			retired.remove_at_unordered(i);
		} else {
			i++;
		}
	}
}

void NextGIRTNative::_update_card_totals() {
	stats.card_meshes = 0;
	stats.direct_card_meshes = 0;
	stats.proxy_card_meshes = 0;
	for (const KeyValue<RID, CardState> &entry : card_states) {
		if (entry.value.cards_checked) {
			stats.card_meshes++;
			stats.direct_card_meshes += entry.value.cards_direct ? 1u : 0u;
		}
		stats.proxy_card_meshes += entry.value.proxy_map ? 1u : 0u;
	}
}

void NextGIRTNative::begin_frame(const ::RenderDataRD *p_render_data, uint64_t p_frame) {
	float per_meter = 0.0f;
	bool view_orthogonal = false;
	if (p_render_data != nullptr && p_render_data->scene_data != nullptr) {
		view_orthogonal = p_render_data->scene_data->cam_orthogonal;
		float internal_height = 0.0f;
		if (p_render_data->render_buffers.is_valid()) {
			internal_height = float(p_render_data->render_buffers->get_internal_size().y);
		}
		const float max_pixel_error = GLOBAL_GET_CACHED(float, "rendering/nextgeo/max_pixel_error");
		per_meter = nextgeo_rt_native_error_per_meter(max_pixel_error, internal_height, Math::abs(p_render_data->scene_data->cam_projection.columns[1][1]));
	}
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	for (uint32_t i = 0; i < retired.size();) {
		if (retired[i].frame + NEXTGEO_RT_NATIVE_RETIRE_FRAMES <= p_frame) {
			if (mesh_storage != nullptr) {
				mesh_storage->mesh_nextgi_rt_arena_free(retired[i].allocation);
			}
			retired.remove_at_unordered(i);
		} else {
			i++;
		}
	}
	begin_frame_sets(p_frame, per_meter, view_orthogonal);
}

void NextGIRTNative::free() {
	MeshStorage *storage = MeshStorage::get_singleton();
	if (storage != nullptr && storage->mesh_nextgi_get_virtual_geometry_changed_userdata() == this) {
		storage->mesh_nextgi_set_virtual_geometry_changed_callback(nullptr, nullptr);
	}
	release_all();
	card_states.clear();
	for (const Retired &entry : retired) {
		if (storage != nullptr) {
			storage->mesh_nextgi_rt_arena_free(entry.allocation);
		}
	}
	retired.clear();
	if (extractor != nullptr) {
		RenderingDevice *rd = RD::get_singleton();
		if (rd != nullptr && extractor->pipeline.is_valid()) {
			rd->free_rid(extractor->pipeline);
		}
		if (extractor->version.is_valid()) {
			extractor->shader.version_free(extractor->version);
		}
		memdelete(extractor);
		extractor = nullptr;
	}
}

} // namespace RendererRD
