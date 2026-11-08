/**************************************************************************/
/*  next_gi_src.cpp                                                       */
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

#include "next_gi_src.h"

#include "core/config/project_settings.h"
#include "core/io/image.h"
#include "core/io/marshalls.h"
#include "core/math/aabb.h"
#include "core/math/math_funcs.h"
#include "core/math/projection.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/variant/variant.h"
#include "servers/rendering/renderer_geometry_instance.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/environment/next_gi_far_field.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/virtual_mesh_storage.h"

#include <algorithm>

namespace RendererRD {

static const Vector3 _nextgi_src_axis_seeds[6] = {
	Vector3(1, 0, 0),
	Vector3(-1, 0, 0),
	Vector3(0, 1, 0),
	Vector3(0, -1, 0),
	Vector3(0, 0, 1),
	Vector3(0, 0, -1),
};

Transform3D NextGICardDesc::get_transform() const {
	return Transform3D(Basis(axis_u, axis_v, axis_n), origin);
}

static uint32_t _nextgi_src_card_texels(float p_extent, float p_texel_size) {
	// The small tolerance keeps an exact multiple of the texel size from rounding up.
	const float texels = Math::ceil(p_extent / p_texel_size - 0.001f);
	return uint32_t(CLAMP(texels, float(NEXTGI_SRC_CARD_MIN_TEXELS), float(NEXTGI_SRC_CARD_MAX_TEXELS)));
}

void nextgi_src_cluster_cards(const float *p_positions, uint32_t p_vertex_count, const uint32_t *p_indices, uint32_t p_index_count, NextGICardClusterResult &r_result) {
	r_result.cards.clear();
	r_result.max_extent = 0.0f;
	r_result.texel_size = 0.0f;
	r_result.total_area = 0.0f;
	if (p_positions == nullptr || p_indices == nullptr || p_vertex_count == 0) {
		return;
	}

	struct Triangle {
		Vector3 normal;
		float area = 0.0f;
		uint32_t index[3] = {};
	};

	auto position = [p_positions](uint32_t p_index) {
		return Vector3(p_positions[p_index * 3 + 0], p_positions[p_index * 3 + 1], p_positions[p_index * 3 + 2]);
	};

	LocalVector<Triangle> triangles;
	triangles.reserve(p_index_count / 3);
	AABB bounds;
	bool has_bounds = false;
	for (uint32_t i = 0; i + 2 < p_index_count; i += 3) {
		const uint32_t a = p_indices[i + 0];
		const uint32_t b = p_indices[i + 1];
		const uint32_t c = p_indices[i + 2];
		if (a >= p_vertex_count || b >= p_vertex_count || c >= p_vertex_count) {
			continue;
		}
		const Vector3 pa = position(a);
		const Vector3 pb = position(b);
		const Vector3 pc = position(c);
		if (!has_bounds) {
			bounds = AABB(pa, Vector3());
			has_bounds = true;
		} else {
			bounds.expand_to(pa);
		}
		bounds.expand_to(pb);
		bounds.expand_to(pc);

		// Godot's front faces wind clockwise (Plane(a, b, c) with CLOCKWISE), so the outward normal
		// is (c - a) x (b - a).
		const Vector3 cross = (pc - pa).cross(pb - pa);
		const real_t length = cross.length();
		if (!(length > CMP_EPSILON2)) {
			continue;
		}
		Triangle triangle;
		triangle.normal = cross / length;
		triangle.area = float(length) * 0.5f;
		triangle.index[0] = a;
		triangle.index[1] = b;
		triangle.index[2] = c;
		triangles.push_back(triangle);
		r_result.total_area += triangle.area;
	}
	if (triangles.is_empty() || !has_bounds) {
		return;
	}
	r_result.max_extent = float(bounds.get_longest_axis_size());
	if (!(r_result.max_extent > 0.0f)) {
		return;
	}
	const float texel_size = r_result.max_extent / float(NEXTGI_SRC_CARD_MAX_TEXELS);
	r_result.texel_size = texel_size;

	// Seeds: the six axes, then the face normals farthest from every seed so far.
	LocalVector<Vector3> centers;
	for (const Vector3 &axis : _nextgi_src_axis_seeds) {
		centers.push_back(axis);
	}
	const real_t seed_cos = Math::cos(Math::deg_to_rad(real_t(NEXTGI_SRC_SEED_MIN_ANGLE_DEGREES)));
	while (centers.size() < NEXTGI_SRC_MAX_CARDS_PER_MESH) {
		real_t worst_dot = 2.0;
		uint32_t worst = 0;
		for (uint32_t t = 0; t < triangles.size(); t++) {
			real_t best_dot = -2.0;
			for (const Vector3 &center : centers) {
				best_dot = MAX(best_dot, triangles[t].normal.dot(center));
			}
			if (best_dot < worst_dot) {
				worst_dot = best_dot;
				worst = t;
			}
		}
		if (worst_dot >= seed_cos) {
			break;
		}
		centers.push_back(triangles[worst].normal);
	}

	// Area-weighted k-means on the face normals.
	const uint32_t cluster_count = centers.size();
	LocalVector<uint32_t> assignment;
	assignment.resize(triangles.size());
	LocalVector<Vector3> sums;
	LocalVector<float> areas;
	sums.resize(cluster_count);
	areas.resize(cluster_count);
	for (uint32_t iteration = 0; iteration < NEXTGI_SRC_KMEANS_ITERATIONS; iteration++) {
		for (uint32_t c = 0; c < cluster_count; c++) {
			sums[c] = Vector3();
			areas[c] = 0.0f;
		}
		for (uint32_t t = 0; t < triangles.size(); t++) {
			uint32_t best = 0;
			real_t best_dot = -2.0;
			for (uint32_t c = 0; c < cluster_count; c++) {
				const real_t d = triangles[t].normal.dot(centers[c]);
				if (d > best_dot) {
					best_dot = d;
					best = c;
				}
			}
			assignment[t] = best;
			sums[best] += triangles[t].normal * triangles[t].area;
			areas[best] += triangles[t].area;
		}
		for (uint32_t c = 0; c < cluster_count; c++) {
			if (areas[c] > 0.0f && sums[c].length() > CMP_EPSILON) {
				centers[c] = sums[c].normalized();
			}
		}
	}

	// Merge clusters under the area fraction into the neighbor with the closest mean normal,
	// smallest first, until none is left or one cluster remains.
	LocalVector<bool> alive;
	alive.resize(cluster_count);
	uint32_t alive_count = 0;
	for (uint32_t c = 0; c < cluster_count; c++) {
		alive[c] = areas[c] > 0.0f;
		alive_count += alive[c] ? 1 : 0;
	}
	const float merge_area = r_result.total_area * NEXTGI_SRC_MERGE_AREA_FRACTION;
	while (alive_count > 1) {
		uint32_t smallest = UINT32_MAX;
		for (uint32_t c = 0; c < cluster_count; c++) {
			if (alive[c] && areas[c] < merge_area && (smallest == UINT32_MAX || areas[c] < areas[smallest])) {
				smallest = c;
			}
		}
		if (smallest == UINT32_MAX) {
			break;
		}
		uint32_t nearest = UINT32_MAX;
		real_t nearest_dot = -2.0;
		for (uint32_t c = 0; c < cluster_count; c++) {
			if (c == smallest || !alive[c]) {
				continue;
			}
			const real_t d = centers[c].dot(centers[smallest]);
			if (d > nearest_dot) {
				nearest_dot = d;
				nearest = c;
			}
		}
		for (uint32_t t = 0; t < triangles.size(); t++) {
			if (assignment[t] == smallest) {
				assignment[t] = nearest;
			}
		}
		sums[nearest] += sums[smallest];
		areas[nearest] += areas[smallest];
		if (sums[nearest].length() > CMP_EPSILON) {
			centers[nearest] = sums[nearest].normalized();
		}
		alive[smallest] = false;
		alive_count--;
	}

	// One card per remaining cluster, in seed order.
	for (uint32_t c = 0; c < cluster_count; c++) {
		if (!alive[c]) {
			continue;
		}
		NextGICardDesc card;
		const Vector3 n = centers[c];
		// Gram-Schmidt against the world axis least aligned with n; u x v = n.
		int reference_axis = 0;
		for (int axis = 1; axis < 3; axis++) {
			if (Math::abs(n[axis]) < Math::abs(n[reference_axis])) {
				reference_axis = axis;
			}
		}
		Vector3 reference;
		reference[reference_axis] = 1.0;
		const Vector3 u = (reference - n * n.dot(reference)).normalized();
		const Vector3 v = n.cross(u);

		real_t u_min = Math::INF, u_max = -Math::INF;
		real_t v_min = Math::INF, v_max = -Math::INF;
		real_t n_max = -Math::INF;
		real_t mesh_n_min = Math::INF;
		for (uint32_t t = 0; t < triangles.size(); t++) {
			for (uint32_t corner = 0; corner < 3; corner++) {
				const Vector3 p = position(triangles[t].index[corner]);
				const real_t pn = p.dot(n);
				mesh_n_min = MIN(mesh_n_min, pn);
				if (assignment[t] != c) {
					continue;
				}
				const real_t pu = p.dot(u);
				const real_t pv = p.dot(v);
				u_min = MIN(u_min, pu);
				u_max = MAX(u_max, pu);
				v_min = MIN(v_min, pv);
				v_max = MAX(v_max, pv);
				n_max = MAX(n_max, pn);
			}
			if (assignment[t] == c) {
				card.area += triangles[t].area;
				card.triangle_count++;
			}
		}

		// One texel of margin in front of the outermost plane, so geometry on it is not clipped by
		// the near plane and gets a depth above 0, and one past the mesh's far side.
		const real_t margin = texel_size;
		card.axis_u = u;
		card.axis_v = v;
		card.axis_n = n;
		card.half_u = MAX(float(u_max - u_min) * 0.5f, texel_size * 0.5f);
		card.half_v = MAX(float(v_max - v_min) * 0.5f, texel_size * 0.5f);
		card.origin = u * ((u_min + u_max) * 0.5) + v * ((v_min + v_max) * 0.5) + n * (n_max + margin);
		card.depth_range = float(n_max - mesh_n_min + margin * 2.0);
		card.texels_u = _nextgi_src_card_texels(card.half_u * 2.0f, texel_size);
		card.texels_v = _nextgi_src_card_texels(card.half_v * 2.0f, texel_size);
		r_result.cards.push_back(card);
	}
}

uint64_t nextgi_src_atlas_memory_bytes(uint32_t p_size, uint32_t p_fb_depth_bytes_per_texel) {
	// albedo, normal and orm (4 B each), emission (8 B), depth (4 B), framebuffer depth.
	const uint64_t bytes_per_texel = 4 + 4 + 4 + 8 + 4 + uint64_t(p_fb_depth_bytes_per_texel);
	return uint64_t(p_size) * uint64_t(p_size) * bytes_per_texel;
}

float nextgi_src_card_coverage(const float *p_depth, uint32_t p_atlas_width, uint32_t p_atlas_height, const Rect2i &p_rect) {
	ERR_FAIL_NULL_V(p_depth, 0.0f);
	if (p_rect.size.x <= 0 || p_rect.size.y <= 0) {
		return 0.0f;
	}
	ERR_FAIL_COND_V(p_rect.position.x < 0 || p_rect.position.y < 0, 0.0f);
	ERR_FAIL_COND_V(uint32_t(p_rect.position.x + p_rect.size.x) > p_atlas_width || uint32_t(p_rect.position.y + p_rect.size.y) > p_atlas_height, 0.0f);
	uint64_t covered = 0;
	for (int y = p_rect.position.y; y < p_rect.position.y + p_rect.size.y; y++) {
		const float *row = p_depth + uint64_t(y) * p_atlas_width;
		for (int x = p_rect.position.x; x < p_rect.position.x + p_rect.size.x; x++) {
			covered += row[x] > 0.0f ? 1 : 0;
		}
	}
	return float(double(covered) / double(int64_t(p_rect.size.x) * p_rect.size.y));
}

bool NextGIShelfPacker::allocate(uint32_t p_width, uint32_t p_height, Rect2i &r_rect) {
	ERR_FAIL_COND_V(p_width == 0 || p_height == 0, false);
	const uint32_t w = p_width + NEXTGI_SRC_ATLAS_GUTTER;
	const uint32_t h = p_height + NEXTGI_SRC_ATLAS_GUTTER;
	if (p_width > width || p_height > height) {
		return false;
	}
	for (uint32_t i = 0; i < free_rects.size(); i++) {
		const Rect2i &free = free_rects[i];
		if (free.size.x < int(p_width) || free.size.y < int(p_height)) {
			continue;
		}
		r_rect = Rect2i(free.position, Vector2i(int(p_width), int(p_height)));
		const Rect2i right(free.position.x + int(p_width), free.position.y, free.size.x - int(p_width), free.size.y);
		const Rect2i bottom(free.position.x, free.position.y + int(p_height), free.size.x, free.size.y - int(p_height));
		free_rects.remove_at(i);
		if (right.size.x > 0) {
			free_rects.push_back(right);
		}
		if (bottom.size.y > 0) {
			free_rects.push_back(bottom);
		}
		used_texels += uint64_t(p_width) * p_height;
		return true;
	}
	for (Shelf &shelf : shelves) {
		// The shelf must be tall enough, and the rect must fit what is left of its width (the
		// gutter may run past the atlas edge).
		if (shelf.height >= h && shelf.used_width + p_width <= width) {
			r_rect = Rect2i(int(shelf.used_width), int(shelf.y), int(p_width), int(p_height));
			shelf.used_width += w;
			used_texels += uint64_t(p_width) * p_height;
			return true;
		}
	}
	if (top + p_height > height) {
		return false;
	}
	Shelf shelf;
	shelf.y = top;
	shelf.height = h;
	shelf.used_width = w;
	shelves.push_back(shelf);
	top += h;
	r_rect = Rect2i(0, int(shelf.y), int(p_width), int(p_height));
	used_texels += uint64_t(p_width) * p_height;
	return true;
}

bool NextGIShelfPacker::allocate_all(const LocalVector<Vector2i> &p_sizes, LocalVector<Rect2i> &r_rects) {
	LocalVector<uint32_t> order;
	order.resize(p_sizes.size());
	for (uint32_t i = 0; i < p_sizes.size(); i++) {
		order[i] = i;
	}
	// Tallest first, then widest; stable on the input order for equal sizes.
	for (uint32_t i = 1; i < order.size(); i++) {
		const uint32_t key = order[i];
		uint32_t j = i;
		while (j > 0) {
			const Vector2i &a = p_sizes[order[j - 1]];
			const Vector2i &b = p_sizes[key];
			if (a.y > b.y || (a.y == b.y && a.x >= b.x)) {
				break;
			}
			order[j] = order[j - 1];
			j--;
		}
		order[j] = key;
	}

	NextGIShelfPacker trial = *this;
	LocalVector<Rect2i> rects;
	rects.resize(p_sizes.size());
	for (uint32_t i : order) {
		if (p_sizes[i].x <= 0 || p_sizes[i].y <= 0 || !trial.allocate(uint32_t(p_sizes[i].x), uint32_t(p_sizes[i].y), rects[i])) {
			return false;
		}
	}
	*this = trial;
	r_rects = rects;
	return true;
}

void NextGIShelfPacker::free(const Rect2i &p_rect) {
	if (p_rect.size.x <= 0 || p_rect.size.y <= 0) {
		return;
	}
	used_texels -= MIN(used_texels, uint64_t(p_rect.size.x) * uint64_t(p_rect.size.y));
	Rect2i rect = p_rect;
	for (uint32_t i = 0; i < free_rects.size();) {
		const Rect2i &other = free_rects[i];
		if (other.position.y != rect.position.y || other.size.y != rect.size.y) {
			i++;
			continue;
		}
		if (other.position.x + other.size.x == rect.position.x) {
			rect.position.x = other.position.x;
			rect.size.x += other.size.x;
			free_rects.remove_at(i);
		} else if (rect.position.x + rect.size.x == other.position.x) {
			rect.size.x += other.size.x;
			free_rects.remove_at(i);
		} else {
			i++;
		}
	}
	free_rects.push_back(rect);
}

void NextGIShelfPacker::reset() {
	top = 0;
	used_texels = 0;
	shelves.clear();
	free_rects.clear();
}

// NextGI entry points (next_gi.h) for the cards.

void nextgi_src_axis_cards(const AABB &p_bounds, NextGICardClusterResult &r_result) {
	r_result.cards.clear();
	r_result.max_extent = float(p_bounds.get_longest_axis_size());
	r_result.texel_size = r_result.max_extent / float(NEXTGI_SRC_CARD_MAX_TEXELS);
	r_result.total_area = 0.0f;
	if (!(r_result.max_extent > 0.0f)) {
		return;
	}
	const Vector3 center = p_bounds.get_center();
	for (const Vector3 &axis : _nextgi_src_axis_seeds) {
		const int normal_axis = axis.x != 0.0f ? 0 : (axis.y != 0.0f ? 1 : 2);
		const int u_axis = (normal_axis + 1) % 3;
		NextGICardDesc card;
		card.axis_n = axis;
		card.axis_u[u_axis] = 1.0f;
		card.axis_v = card.axis_n.cross(card.axis_u);
		card.half_u = MAX(p_bounds.size[u_axis] * 0.5f, r_result.texel_size * 0.5f);
		const int v_axis = (u_axis + 1) % 3;
		card.half_v = MAX(p_bounds.size[v_axis] * 0.5f, r_result.texel_size * 0.5f);
		card.origin = center + axis * (p_bounds.size[normal_axis] * 0.5f + r_result.texel_size);
		card.depth_range = p_bounds.size[normal_axis] + r_result.texel_size * 2.0f;
		card.texels_u = _nextgi_src_card_texels(card.half_u * 2.0f, r_result.texel_size);
		card.texels_v = _nextgi_src_card_texels(card.half_v * 2.0f, r_result.texel_size);
		card.area = card.half_u * card.half_v * 4.0f;
		r_result.total_area += card.area;
		r_result.cards.push_back(card);
	}
}

void NextGI::cluster_cards(const float *p_positions, uint32_t p_vertex_count, const uint32_t *p_indices, uint32_t p_index_count, NextGICardClusterResult &r_result) {
	nextgi_src_cluster_cards(p_positions, p_vertex_count, p_indices, p_index_count, r_result);
}

void NextGI::_capture_src_cards(RenderDataRD *p_render_data) {
	if (src == nullptr) {
		src = memnew(NextGISRC);
		src->init();
	}
	src->capture(p_render_data, shadow_batch_open);
}

NextGIAtlasBudget nextgi_src_atlas_budget(bool p_epic) {
	NextGIAtlasBudget budget;
	budget.radiance_width = 4096;
	budget.radiance_height = p_epic ? 8192 : 4096;
	budget.radiance_bytes = uint64_t(budget.radiance_width) * budget.radiance_height * 4;
	budget.material_bytes = p_epic ? uint64_t(NEXTGI_SRC_MATERIAL_BUDGET_EPIC) : uint64_t(NEXTGI_SRC_MATERIAL_BUDGET_HIGH);
	return budget;
}

float nextgi_src_solid_angle(float p_radius, float p_distance) {
	if (p_distance <= 0.0f) {
		return Math::PI;
	}
	const float ratio = p_radius / p_distance;
	return Math::PI * ratio * ratio;
}

uint32_t nextgi_src_page_level(float p_solid_angle, uint32_t p_full_texels) {
	if (p_full_texels <= 1) {
		return 0;
	}
	const float fraction = CLAMP(p_solid_angle / Math::PI, 0.0f, 1.0f);
	const float needed = MAX(1.0f, float(p_full_texels) * Math::sqrt(fraction));
	if (needed >= float(p_full_texels)) {
		return 0;
	}
	const int level = int(Math::floor(Math::log(float(p_full_texels) / needed) / Math::log(2.0f)));
	return uint32_t(CLAMP(level, 0, int(NEXTGI_SRC_PAGE_LEVEL_MAX)));
}

uint64_t nextgi_src_material_key(uint64_t p_mesh_id, uint64_t p_override_id, const uint64_t *p_surface_ids, uint32_t p_surface_count) {
	uint64_t hash = 14695981039346656037ull;
	auto mix = [&](uint64_t p_value) {
		hash ^= p_value;
		hash *= 1099511628211ull;
	};
	mix(p_mesh_id);
	mix(p_override_id);
	mix(p_surface_count);
	for (uint32_t i = 0; i < p_surface_count; i++) {
		mix(p_surface_ids != nullptr ? p_surface_ids[i] : 0);
	}
	return hash;
}

uint32_t NextGIMaterialCaptures::capture(uint64_t p_key, bool &r_new) {
	if (uint32_t *found = ids.getptr(p_key)) {
		r_new = false;
		return *found;
	}
	r_new = true;
	const uint32_t id = count++;
	ids.insert(p_key, id);
	return id;
}

uint32_t NextGIMaterialCaptures::get_id(uint64_t p_key) const {
	const uint32_t *found = ids.getptr(p_key);
	return found != nullptr ? *found : UINT32_MAX;
}

uint32_t nextgi_src_select_captures(const NextGICaptureCandidate *p_items, uint32_t p_count, uint32_t *r_accepted, uint32_t p_card_cap, uint32_t p_texel_cap) {
	LocalVector<uint32_t> order;
	order.resize(p_count);
	for (uint32_t i = 0; i < p_count; i++) {
		order[i] = i;
	}
	std::sort(order.ptr(), order.ptr() + p_count, [&](uint32_t p_a, uint32_t p_b) {
		const NextGICaptureCandidate &a = p_items[p_a];
		const NextGICaptureCandidate &b = p_items[p_b];
		if (a.first_residency != b.first_residency) {
			return a.first_residency;
		}
		return a.solid_angle > b.solid_angle;
	});
	uint32_t accepted = 0;
	uint32_t texels = 0;
	for (uint32_t index : order) {
		if (accepted >= p_card_cap) {
			break;
		}
		const uint32_t cost = p_items[index].texels;
		if (accepted > 0 && texels + cost > p_texel_cap) {
			break;
		}
		r_accepted[accepted++] = p_items[index].id;
		texels += cost;
	}
	return accepted;
}

uint32_t nextgi_src_lru_evict(const NextGIPageUse *p_pages, uint32_t p_count, uint32_t p_need, uint32_t *r_evict) {
	LocalVector<uint32_t> order;
	order.resize(p_count);
	for (uint32_t i = 0; i < p_count; i++) {
		order[i] = i;
	}
	std::sort(order.ptr(), order.ptr() + p_count, [&](uint32_t p_a, uint32_t p_b) {
		return p_pages[p_a].last_used < p_pages[p_b].last_used;
	});
	const uint32_t count = MIN(p_need, p_count);
	for (uint32_t i = 0; i < count; i++) {
		r_evict[i] = p_pages[order[i]].id;
	}
	return count;
}

NextGIBuddyAllocator::NextGIBuddyAllocator(uint32_t p_size, uint32_t p_min) :
		size(p_size), min_block(MAX(p_min, 1u)) {
	reset();
}

uint32_t NextGIBuddyAllocator::_level(uint32_t p_side) const {
	uint32_t level = 0;
	uint32_t side = min_block;
	while (side < p_side && level + 1 < LEVELS) {
		side <<= 1;
		level++;
	}
	return level;
}

uint32_t NextGIBuddyAllocator::_round(uint32_t p_side) const {
	uint32_t side = min_block;
	const uint32_t need = MAX(p_side, min_block);
	while (side < need && side < size) {
		side <<= 1;
	}
	return side;
}

void NextGIBuddyAllocator::reset() {
	for (uint32_t i = 0; i < LEVELS; i++) {
		free_lists[i].clear();
	}
	if (size >= min_block) {
		free_lists[_level(size)].push_back(Rect2i(0, 0, int(size), int(size)));
	}
}

bool NextGIBuddyAllocator::_ensure(uint32_t p_side) {
	const uint32_t level = _level(p_side);
	if (!free_lists[level].is_empty()) {
		return true;
	}
	if (p_side >= size) {
		return false;
	}
	if (!_ensure(p_side << 1)) {
		return false;
	}
	const uint32_t parent_level = _level(p_side << 1);
	const Rect2i parent = free_lists[parent_level][free_lists[parent_level].size() - 1];
	free_lists[parent_level].resize(free_lists[parent_level].size() - 1);
	const int x = parent.position.x;
	const int y = parent.position.y;
	const int side = int(p_side);
	free_lists[level].push_back(Rect2i(x, y, side, side));
	free_lists[level].push_back(Rect2i(x + side, y, side, side));
	free_lists[level].push_back(Rect2i(x, y + side, side, side));
	free_lists[level].push_back(Rect2i(x + side, y + side, side, side));
	return true;
}

bool NextGIBuddyAllocator::allocate(uint32_t p_width, uint32_t p_height, Rect2i &r_rect) {
	const uint32_t side = _round(MAX(p_width, p_height));
	if (side > size || !_ensure(side)) {
		return false;
	}
	const uint32_t level = _level(side);
	r_rect = free_lists[level][free_lists[level].size() - 1];
	free_lists[level].resize(free_lists[level].size() - 1);
	return true;
}

void NextGIBuddyAllocator::_merge(const Rect2i &p_block) {
	const uint32_t side = uint32_t(p_block.size.x);
	if (side == 0 || side >= size) {
		return;
	}
	const uint32_t parent = side << 1;
	const int origin_x = p_block.position.x - (p_block.position.x % int(parent));
	const int origin_y = p_block.position.y - (p_block.position.y % int(parent));
	const Rect2i kids[4] = {
		Rect2i(origin_x, origin_y, int(side), int(side)),
		Rect2i(origin_x + int(side), origin_y, int(side), int(side)),
		Rect2i(origin_x, origin_y + int(side), int(side), int(side)),
		Rect2i(origin_x + int(side), origin_y + int(side), int(side), int(side)),
	};
	const uint32_t level = _level(side);
	uint32_t found[4] = { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
	for (uint32_t i = 0; i < free_lists[level].size(); i++) {
		for (uint32_t k = 0; k < 4; k++) {
			if (free_lists[level][i] == kids[k]) {
				found[k] = i;
			}
		}
	}
	for (uint32_t k = 0; k < 4; k++) {
		if (found[k] == UINT32_MAX) {
			return;
		}
	}
	uint32_t drop[4] = { found[0], found[1], found[2], found[3] };
	std::sort(drop, drop + 4);
	for (int i = 3; i >= 0; i--) {
		free_lists[level].remove_at(drop[i]);
	}
	const Rect2i merged(origin_x, origin_y, int(parent), int(parent));
	free_lists[_level(parent)].push_back(merged);
	_merge(merged);
}

bool NextGIBuddyAllocator::free_block(const Rect2i &p_rect) {
	if (p_rect.size.x <= 0 || p_rect.size.x != p_rect.size.y) {
		return false;
	}
	free_lists[_level(uint32_t(p_rect.size.x))].push_back(p_rect);
	_merge(p_rect);
	return true;
}

// NextGISRC

NextGISRC *NextGISRC::singleton = nullptr;

static const char *_nextgi_src_target_names[NextGISRC::ATLAS_TARGET_MAX] = {
	"albedo",
	"normal",
	"orm",
	"emission",
	"depth",
};

String NextGISRC::get_atlas_target_name(AtlasTarget p_target) {
	ERR_FAIL_INDEX_V(p_target, ATLAS_TARGET_MAX, String());
	return _nextgi_src_target_names[p_target];
}

uint32_t NextGISRC::get_mesh_card_set(RID p_mesh) {
	if (singleton == nullptr) {
		return UINT32_MAX;
	}
	const uint32_t *card_set = singleton->mesh_card_sets.getptr(p_mesh);
	return card_set != nullptr ? *card_set : UINT32_MAX;
}

static uint64_t _nextgi_src_instance_key(RenderGeometryInstance *p_instance) {
	RenderGeometryInstanceBase *geometry = static_cast<RenderGeometryInstanceBase *>(p_instance);
	if (geometry == nullptr || geometry->data == nullptr) {
		return 0;
	}
	LocalVector<uint64_t> surfaces;
	surfaces.resize(geometry->data->surface_materials.size());
	for (uint32_t i = 0; i < surfaces.size(); i++) {
		surfaces[i] = geometry->data->surface_materials[i].get_id();
	}
	return nextgi_src_material_key(geometry->data->base.get_id(), geometry->data->material_override.get_id(), surfaces.ptr(), surfaces.size());
}

uint32_t NextGISRC::get_instance_card_set(RenderGeometryInstance *p_instance) {
	if (singleton == nullptr || p_instance == nullptr) {
		return UINT32_MAX;
	}
	const uint64_t key = _nextgi_src_instance_key(p_instance);
	const uint32_t *card_set = singleton->key_card_sets.getptr(key);
	return card_set != nullptr ? *card_set : UINT32_MAX;
}

RID NextGISRC::get_atlas_texture(AtlasTarget p_target) const {
	ERR_FAIL_INDEX_V(p_target, ATLAS_TARGET_MAX, RID());
	return atlas[p_target];
}

void NextGISRC::_create_atlas() {
	RD *rd = RD::get_singleton();
	static const RD::DataFormat formats[ATLAS_TARGET_MAX] = {
		RD::DATA_FORMAT_R8G8B8A8_UNORM, // albedo
		RD::DATA_FORMAT_R8G8B8A8_UNORM, // normal
		RD::DATA_FORMAT_R8G8B8A8_UNORM, // orm
		RD::DATA_FORMAT_R16G16B16A16_SFLOAT, // emission
		RD::DATA_FORMAT_R32_SFLOAT, // depth
	};
	static const char *resource_names[ATLAS_TARGET_MAX] = {
		"NextGI SRC Atlas Albedo",
		"NextGI SRC Atlas Normal",
		"NextGI SRC Atlas ORM",
		"NextGI SRC Atlas Emission",
		"NextGI SRC Atlas Depth",
	};

	RD::TextureFormat tf;
	tf.width = NEXTGI_SRC_ATLAS_SIZE;
	tf.height = NEXTGI_SRC_ATLAS_SIZE;
	tf.texture_type = RD::TEXTURE_TYPE_2D;
	tf.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	Vector<RID> fb_textures;
	for (int i = 0; i < ATLAS_TARGET_MAX; i++) {
		tf.format = formats[i];
		atlas[i] = rd->texture_create(tf, RD::TextureView());
		ERR_FAIL_COND(atlas[i].is_null());
		rd->set_resource_name(atlas[i], resource_names[i]);
		// Coverage reads depth > 0, and the dumps read the whole atlas, so start from 0.
		rd->texture_clear(atlas[i], Color(0, 0, 0, 0), 0, 1, 0, 1);
		fb_textures.push_back(atlas[i]);
	}

	// The VoxelGI dynamic-map choice: 16-bit framebuffer depth when supported.
	const bool d16 = rd->texture_is_format_supported_for_usage(RD::DATA_FORMAT_D16_UNORM, RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
	tf.format = d16 ? RD::DATA_FORMAT_D16_UNORM : RD::DATA_FORMAT_X8_D24_UNORM_PACK32;
	tf.usage_bits = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	atlas_fb_depth = rd->texture_create(tf, RD::TextureView());
	ERR_FAIL_COND(atlas_fb_depth.is_null());
	rd->set_resource_name(atlas_fb_depth, "NextGI SRC Atlas FB Depth");
	fb_textures.push_back(atlas_fb_depth);

	atlas_fb = rd->framebuffer_create(fb_textures);
	ERR_FAIL_COND(atlas_fb.is_null());

	atlas_memory = nextgi_src_atlas_memory_bytes(NEXTGI_SRC_ATLAS_SIZE, d16 ? 2 : 4);
	// GodotNext: nextgi_quality_presets (#55). The material budget follows the preset.
	const bool epic = NextGI::get_singleton() != nullptr && NextGI::get_quality_params(NextGI::get_singleton()->get_quality_setting()).src_material_mib > 128;
	const uint64_t material_budget = epic ? uint64_t(NEXTGI_SRC_MATERIAL_BUDGET_EPIC) : uint64_t(NEXTGI_SRC_MATERIAL_BUDGET_HIGH);
	// Worded without the "NextGI:" prefix, so the tier stays the only such line in the log.
	print_line(vformat("NextGI SRC: material atlas %dx%d, %.1f MiB of the %d MiB %s budget (framebuffer depth %s).", NEXTGI_SRC_ATLAS_SIZE, NEXTGI_SRC_ATLAS_SIZE, double(atlas_memory) / (1024.0 * 1024.0), int(material_budget / (1024 * 1024)), epic ? "Epic" : "High", d16 ? "D16" : "X8_D24"));
	ERR_FAIL_COND_MSG(atlas_memory > material_budget, "The NextGI SRC material atlas is over the preset budget.");
}

void NextGISRC::init() {
	ERR_FAIL_COND(atlas_fb.is_valid());
	ERR_FAIL_NULL(RD::get_singleton());
	_create_atlas();
	dump_state = bool(GLOBAL_GET("rendering/global_illumination/nextgi/debug/dump_material_atlas")) ? DUMP_WAITING : DUMP_OFF;
}

void NextGISRC::_release_stage2_proxy_instances(CardSetState &r_state) {
	if (!r_state.proxy_instances.is_empty()) {
		RendererSceneRenderRD *scene = RendererSceneRenderRD::get_singleton();
		for (RenderGeometryInstance *instance : r_state.proxy_instances) {
			if (instance != nullptr && scene != nullptr) {
				scene->geometry_instance_free(instance);
			}
		}
		r_state.proxy_instances.clear();
	}
	if (!r_state.proxy_meshes.is_empty()) {
		MeshStorage *mesh_storage = MeshStorage::get_singleton();
		for (const RID &proxy_mesh : r_state.proxy_meshes) {
			if (proxy_mesh.is_valid() && mesh_storage != nullptr) {
				mesh_storage->free(proxy_mesh);
			}
		}
		r_state.proxy_meshes.clear();
	}
}

void NextGISRC::free() {
	for (CardSetState &state : card_set_states) {
		_release_stage2_proxy_instances(state);
	}
	card_set_states.clear();
	card_sets.clear();
	cards.clear();
	card_descs.clear();
	card_capture_counts.clear();
	mesh_card_sets.clear();
	key_card_sets.clear();
	failed_meshes.clear();
	emissive_warned_meshes.clear();
	card_coverage.clear();
	packer.reset();
	stats = Stats();
	gpu_dirty = false;
	atlas_memory = 0;

	RD *rd = RD::get_singleton();
	if (rd == nullptr) {
		return;
	}
	if (atlas_fb.is_valid() && rd->framebuffer_is_valid(atlas_fb)) {
		rd->free_rid(atlas_fb);
	}
	atlas_fb = RID();
	for (int i = 0; i < ATLAS_TARGET_MAX; i++) {
		if (atlas[i].is_valid()) {
			rd->free_rid(atlas[i]);
			atlas[i] = RID();
		}
	}
	if (atlas_fb_depth.is_valid()) {
		rd->free_rid(atlas_fb_depth);
		atlas_fb_depth = RID();
	}
	if (card_buffer.is_valid()) {
		rd->free_rid(card_buffer);
		card_buffer = RID();
	}
	if (card_set_buffer.is_valid()) {
		rd->free_rid(card_set_buffer);
		card_set_buffer = RID();
	}
	card_capacity = 0;
	card_set_capacity = 0;
}

// GodotNext: nextgi_emissive_contract (#60). Emissive surfaces smaller than four card texels
// cannot be resolved by the SRC; the contract asks for a light instead (plan §8.5). Card texels are
// authored at 1 cm for emissive content, so the threshold is 4 cm, the same rule the testbed
// content uses (build_p3_content.gd, under_four_card_texels).
static constexpr float NEXTGI_SRC_EMISSIVE_MIN_TEXEL_METERS = 0.01f;

static bool _nextgi_src_material_is_emissive(RID p_material) {
	MaterialStorage *storage = MaterialStorage::get_singleton();
	if (storage == nullptr || !storage->owns_material(p_material)) {
		return false;
	}
	const Variant emission = storage->material_get_param(p_material, "emission");
	if (emission.get_type() != Variant::COLOR) {
		return false;
	}
	const Color color = emission;
	if (color.r <= 0.0f && color.g <= 0.0f && color.b <= 0.0f) {
		return false;
	}
	const Variant energy = storage->material_get_param(p_material, "emission_energy");
	const float multiplier = (energy.get_type() == Variant::FLOAT || energy.get_type() == Variant::INT) ? float(energy) : 1.0f;
	return (color.r + color.g + color.b) * multiplier > 0.0f;
}

void NextGISRC::_check_emissive_contract(RenderGeometryInstance *p_instance, uint32_t p_set_index) {
	RenderGeometryInstanceBase *geometry = static_cast<RenderGeometryInstanceBase *>(p_instance);
	if (geometry == nullptr || geometry->data == nullptr || p_set_index >= card_set_states.size()) {
		return;
	}
	const CardSetState &state = card_set_states[p_set_index];
	const float min_extent = NEXTGI_SRC_EMISSIVE_MIN_TEXEL_METERS * 4.0f;
	bool below = false;
	if (geometry->data->material_override.is_valid()) {
		below = _nextgi_src_material_is_emissive(geometry->data->material_override);
		if (below) {
			// An override applies to every surface; the largest one decides whether it is resolved.
			float largest = 0.0f;
			for (const KeyValue<uint32_t, float> &entry : state.surface_extents) {
				largest = MAX(largest, entry.value);
			}
			below = largest > 0.0f && largest < min_extent;
		}
	} else {
		for (uint32_t i = 0; i < geometry->data->surface_materials.size(); i++) {
			const float *extent = state.surface_extents.getptr(i);
			if (extent != nullptr && *extent > 0.0f && *extent < min_extent && _nextgi_src_material_is_emissive(geometry->data->surface_materials[i])) {
				below = true;
				break;
			}
		}
	}
	if (!below) {
		return;
	}
	stats.emissive_below_card_res++;
	if (!emissive_warned_meshes.has(state.mesh)) {
		emissive_warned_meshes.insert(state.mesh);
		WARN_PRINT(vformat("NextGI SRC: mesh RID %d has an emissive surface smaller than 4 card texels; author it as a light (plan 8.5).", state.mesh.get_id()));
	}
}

bool NextGISRC::_build_stage2_card_set(RID p_mesh, uint64_t p_key) {
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (mesh_storage == nullptr || !mesh_storage->owns_mesh(p_mesh) || RSG::virtual_mesh_storage == nullptr) {
		return false;
	}
	const RenderingServerTypes::VirtualGeometryDesc *desc = RSG::virtual_mesh_storage->mesh_get_virtual_geometry_desc(p_mesh);
	if (desc == nullptr || desc->card_descriptors.is_empty()) {
		return false;
	}
	const LocalVector<MeshStorage::NextGIRTPart> *parts = mesh_storage->mesh_nextgi_rt_get_parts(p_mesh);
	if (parts == nullptr) {
		return false;
	}
	bool has_built_part = false;
	for (const MeshStorage::NextGIRTPart &part : *parts) {
		if (!part.geometries.is_empty()) {
			has_built_part = true;
			break;
		}
	}
	if (!has_built_part) {
		return false; // No parts yet: the RT scene has not built them; try again next frame.
	}
	if ((desc->card_descriptors.size() % 32) != 0) {
		failed_meshes.insert(p_mesh);
		stats.failed_mesh_count++;
		return false;
	}

	const uint8_t *raw = desc->card_descriptors.ptr();
	const uint32_t descriptor_count = uint32_t(desc->card_descriptors.size() / 32);
	const float texel_size = MAX(2.0f * desc->rt_proxy_eps, 1e-6f);

	LocalVector<NextGICardDesc> built_descs;
	LocalVector<uint16_t> built_groups;
	LocalVector<uint8_t> built_axes;
	LocalVector<Vector2i> sizes;
	built_descs.reserve(descriptor_count);
	built_groups.reserve(descriptor_count);
	built_axes.reserve(descriptor_count);
	sizes.reserve(descriptor_count);

	for (uint32_t i = 0; i < descriptor_count; i++) {
		const uint8_t *rec = raw + i * 32;
		const uint16_t group = decode_uint16(rec + 0);
		const uint8_t axis = rec[2];
		if (axis >= 6) {
			failed_meshes.insert(p_mesh);
			stats.failed_mesh_count++;
			return false;
		}
		const Vector3 center(decode_float(rec + 4), decode_float(rec + 8), decode_float(rec + 12));
		const Vector3 half_extents(decode_float(rec + 16), decode_float(rec + 20), decode_float(rec + 24));
		const uint16_t res_u = decode_uint16(rec + 28);
		const uint16_t res_v = decode_uint16(rec + 30);

		const Vector3 axis_n = _nextgi_src_axis_seeds[axis];
		const int normal_axis = int(axis / 2);
		const int u_axis = (normal_axis + 1) % 3;
		const int v_axis = (u_axis + 1) % 3;

		NextGICardDesc card_desc;
		card_desc.axis_n = axis_n;
		card_desc.axis_u[u_axis] = 1.0f;
		card_desc.axis_v = card_desc.axis_n.cross(card_desc.axis_u);
		card_desc.half_u = MAX(half_extents[u_axis], texel_size * 0.5f);
		card_desc.half_v = MAX(half_extents[v_axis], texel_size * 0.5f);
		card_desc.origin = center + axis_n * (half_extents[normal_axis] + texel_size);
		card_desc.depth_range = half_extents[normal_axis] * 2.0f + texel_size * 2.0f;
		card_desc.texels_u = CLAMP(uint32_t(res_u), uint32_t(NEXTGI_SRC_CARD_MIN_TEXELS), uint32_t(NEXTGI_SRC_CARD_MAX_TEXELS));
		card_desc.texels_v = CLAMP(uint32_t(res_v), uint32_t(NEXTGI_SRC_CARD_MIN_TEXELS), uint32_t(NEXTGI_SRC_CARD_MAX_TEXELS));
		card_desc.area = card_desc.half_u * card_desc.half_v * 4.0f;

		built_descs.push_back(card_desc);
		built_groups.push_back(group);
		built_axes.push_back(axis);
		sizes.push_back(Vector2i(int(card_desc.texels_u), int(card_desc.texels_v)));
	}
	if (built_descs.is_empty()) {
		failed_meshes.insert(p_mesh);
		stats.failed_mesh_count++;
		return false;
	}

	LocalVector<Rect2i> rects;
	if (!packer.allocate_all(sizes, rects)) {
		WARN_PRINT_ONCE("The NextGI SRC material atlas is full; meshes without space get no cards (the P1b LRU, #41, evicts).");
		failed_meshes.insert(p_mesh);
		stats.failed_mesh_count++;
		return false;
	}

	HashMap<uint32_t, float> surface_extents;
	const int mesh_surface_count = mesh_storage->mesh_get_surface_count(p_mesh);
	const uint32_t part_count = uint32_t(desc->rt_proxy_parts.size() / 4);
	const uint32_t vgd_surface_count = part_count > 0 ? uint32_t(desc->rt_proxy_surface_ranges.size() / (part_count * 2)) : 0u;
	const float *pos_ptr = desc->rt_proxy_positions.ptr();
	const uint32_t total_verts = uint32_t(desc->rt_proxy_positions.size() / 3);
	const uint8_t *idx_bytes = desc->rt_proxy_indices.ptr();
	const uint32_t total_indices = uint32_t(desc->rt_proxy_indices.size() / 4);
	for (int s = 0; s < mesh_surface_count; s++) {
		const bool is_virtual = s < 32 && ((desc->eligible_surface_mask & (1u << uint32_t(s))) != 0u);
		if (is_virtual && uint32_t(s) < vgd_surface_count) {
			Vector3 lo(Math::INF, Math::INF, Math::INF);
			Vector3 hi(-Math::INF, -Math::INF, -Math::INF);
			for (uint32_t p = 0; p < part_count; p++) {
				const uint32_t v_off = uint32_t(MAX(desc->rt_proxy_parts[p * 4 + 0], 0));
				const uint32_t i_off = uint32_t(MAX(desc->rt_proxy_parts[p * 4 + 2], 0));
				const uint32_t first_idx = uint32_t(MAX(desc->rt_proxy_surface_ranges[(p * vgd_surface_count + uint32_t(s)) * 2 + 0], 0));
				const uint32_t idx_cnt = uint32_t(MAX(desc->rt_proxy_surface_ranges[(p * vgd_surface_count + uint32_t(s)) * 2 + 1], 0));
				if (uint64_t(i_off) + first_idx + idx_cnt > total_indices) {
					continue;
				}
				for (uint32_t k = 0; k < idx_cnt; k++) {
					const uint32_t local_idx = decode_uint32(idx_bytes + (i_off + first_idx + k) * 4u);
					if (uint64_t(v_off) + local_idx >= total_verts) {
						continue;
					}
					const uint64_t base = (uint64_t(v_off) + local_idx) * 3u;
					const Vector3 vertex(pos_ptr[base + 0], pos_ptr[base + 1], pos_ptr[base + 2]);
					lo = lo.min(vertex);
					hi = hi.max(vertex);
				}
			}
			if (lo.x <= hi.x) {
				const Vector3 extent = hi - lo;
				surface_extents.insert(uint32_t(s), MAX(extent.x, MAX(extent.y, extent.z)));
			}
		} else {
			Vector<float> surface_positions;
			Vector<uint32_t> surface_indices;
			if (mesh_storage->mesh_nextgi_get_cpu_geometry(p_mesh, uint32_t(s), surface_positions, surface_indices)) {
				Vector3 lo(Math::INF, Math::INF, Math::INF);
				Vector3 hi(-Math::INF, -Math::INF, -Math::INF);
				for (int i = 0; i + 2 < surface_positions.size(); i += 3) {
					const Vector3 vertex(surface_positions[i], surface_positions[i + 1], surface_positions[i + 2]);
					lo = lo.min(vertex);
					hi = hi.max(vertex);
				}
				if (lo.x <= hi.x) {
					const Vector3 extent = hi - lo;
					surface_extents.insert(uint32_t(s), MAX(extent.x, MAX(extent.y, extent.z)));
				}
			}
		}
	}

	const uint32_t set_index = card_sets.size();
	NextGICardSet set;
	set.first_card = cards.size();
	set.card_count = built_descs.size();
	set.texel_size = texel_size;
	set.capture_scale = 1.0f;
	for (uint32_t i = 0; i < built_descs.size(); i++) {
		const NextGICardDesc &card_desc = built_descs[i];
		NextGICard card;
		card.origin[0] = card_desc.origin.x;
		card.origin[1] = card_desc.origin.y;
		card.origin[2] = card_desc.origin.z;
		card.origin[3] = float(uint32_t(built_groups[i]) + 1u);
		card.axis_u[0] = card_desc.axis_u.x;
		card.axis_u[1] = card_desc.axis_u.y;
		card.axis_u[2] = card_desc.axis_u.z;
		card.axis_u[3] = card_desc.half_u;
		card.axis_v[0] = card_desc.axis_v.x;
		card.axis_v[1] = card_desc.axis_v.y;
		card.axis_v[2] = card_desc.axis_v.z;
		card.axis_v[3] = card_desc.half_v;
		card.axis_n[0] = card_desc.axis_n.x;
		card.axis_n[1] = card_desc.axis_n.y;
		card.axis_n[2] = card_desc.axis_n.z;
		card.axis_n[3] = card_desc.depth_range;
		card.atlas_rect[0] = uint32_t(rects[i].position.x);
		card.atlas_rect[1] = uint32_t(rects[i].position.y);
		card.atlas_rect[2] = uint32_t(rects[i].size.x);
		card.atlas_rect[3] = uint32_t(rects[i].size.y);
		cards.push_back(card);
		card_descs.push_back(card_desc);
		card_capture_counts.push_back(0);
	}
	card_sets.push_back(set);
	CardSetState state;
	state.mesh = p_mesh;
	state.surface_extents = surface_extents;
	state.is_stage2 = true;
	state.card_groups = built_groups;
	state.card_axes = built_axes;
	card_set_states.push_back(state);
	if (!mesh_card_sets.has(p_mesh)) {
		mesh_card_sets.insert(p_mesh, set_index);
	}
	key_card_sets.insert(p_key, set_index);
	stats.material_capture_count = key_card_sets.size();
	stats.card_set_count++;
	stats.card_count += set.card_count;
	gpu_dirty = true;
	const float max_extent = float(mesh_storage->mesh_get_aabb(p_mesh).get_longest_axis_size());
	print_line(vformat("NextGI SRC: Stage-2 card set %d: %d cards, texel %.4f, mesh extent %.3f.", set_index, set.card_count, set.texel_size, max_extent));
	return true;
}

static bool _nextgi_stage2_dominant_face_axis(const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_c, uint8_t &r_axis) {
	// Godot front-facing triangles wind clockwise, so the outward face normal is (c - a) x (b - a),
	// matching nextgeo_rt_proxy.cpp:dominant_face_axis.
	const double ux = double(p_c.x) - double(p_a.x);
	const double uy = double(p_c.y) - double(p_a.y);
	const double uz = double(p_c.z) - double(p_a.z);
	const double vx = double(p_b.x) - double(p_a.x);
	const double vy = double(p_b.y) - double(p_a.y);
	const double vz = double(p_b.z) - double(p_a.z);
	const double nx = uy * vz - uz * vy;
	const double ny = uz * vx - ux * vz;
	const double nz = ux * vy - uy * vx;
	const double len2 = nx * nx + ny * ny + nz * nz;
	if (!(len2 > 1e-24)) {
		return false;
	}
	const double anx = Math::abs(nx);
	const double any = Math::abs(ny);
	const double anz = Math::abs(nz);
	if (anx >= any && anx >= anz) {
		r_axis = nx >= 0.0 ? 0 : 1;
	} else if (any >= anz) {
		r_axis = ny >= 0.0 ? 2 : 3;
	} else {
		r_axis = nz >= 0.0 ? 4 : 5;
	}
	return true;
}

bool NextGISRC::_ensure_stage2_proxy_instances(CardSetState &r_state) {
	const uint32_t card_count = r_state.card_groups.size();
	if (card_count == 0 || r_state.card_axes.size() != card_count) {
		return false;
	}
	if (r_state.proxy_instances.size() == card_count) {
		return true;
	}
	_release_stage2_proxy_instances(r_state);

	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	RenderingServer *server = RenderingServer::get_singleton();
	RendererSceneRenderRD *scene = RendererSceneRenderRD::get_singleton();
	if (mesh_storage == nullptr || server == nullptr || scene == nullptr || RSG::virtual_mesh_storage == nullptr || !mesh_storage->owns_mesh(r_state.mesh)) {
		return false;
	}
	const RenderingServerTypes::VirtualGeometryDesc *desc = RSG::virtual_mesh_storage->mesh_get_virtual_geometry_desc(r_state.mesh);
	if (desc == nullptr) {
		return false;
	}
	const int surface_count = mesh_storage->mesh_get_surface_count(r_state.mesh);
	if (surface_count <= 0) {
		return false;
	}

	HashMap<uint32_t, uint32_t> group_axis_to_card;
	for (uint32_t c = 0; c < card_count; c++) {
		const uint32_t ga_key = (uint32_t(r_state.card_groups[c]) << 8u) | uint32_t(r_state.card_axes[c]);
		group_axis_to_card.insert(ga_key, c);
	}

	struct CardTri {
		Vector3 pa;
		Vector3 pb;
		Vector3 pc;
		Vector3 fn;
	};
	struct CardSurfaceBucket {
		LocalVector<CardTri> tris;
	};
	LocalVector<CardSurfaceBucket> card_buckets;
	card_buckets.resize(uint64_t(card_count) * uint32_t(surface_count));

	const uint32_t part_count = uint32_t(desc->rt_proxy_parts.size() / 4);
	const uint32_t vgd_surface_count = part_count > 0 ? uint32_t(desc->rt_proxy_surface_ranges.size() / (part_count * 2)) : 0u;
	const float *pos_ptr = desc->rt_proxy_positions.ptr();
	const uint32_t total_verts = uint32_t(desc->rt_proxy_positions.size() / 3);
	const uint8_t *idx_bytes = desc->rt_proxy_indices.ptr();
	const uint32_t total_indices = uint32_t(desc->rt_proxy_indices.size() / 4);
	const uint8_t *grp_bytes = desc->rt_proxy_triangle_groups.ptr();
	const uint32_t total_tri_groups = uint32_t(desc->rt_proxy_triangle_groups.size() / 2);

	for (int s = 0; s < surface_count; s++) {
		const bool is_virtual = s < 32 && ((desc->eligible_surface_mask & (1u << uint32_t(s))) != 0u);
		if (!is_virtual || uint32_t(s) >= vgd_surface_count) {
			continue;
		}
		for (uint32_t p = 0; p < part_count; p++) {
			const uint32_t v_off = uint32_t(MAX(desc->rt_proxy_parts[p * 4 + 0], 0));
			const uint32_t i_off = uint32_t(MAX(desc->rt_proxy_parts[p * 4 + 2], 0));
			const uint32_t first_idx = uint32_t(MAX(desc->rt_proxy_surface_ranges[(p * vgd_surface_count + uint32_t(s)) * 2 + 0], 0));
			const uint32_t idx_cnt = uint32_t(MAX(desc->rt_proxy_surface_ranges[(p * vgd_surface_count + uint32_t(s)) * 2 + 1], 0));
			if (uint64_t(i_off) + first_idx + idx_cnt > total_indices) {
				continue;
			}
			for (uint32_t k = 0; k + 2 < idx_cnt; k += 3) {
				const uint32_t global_idx = i_off + first_idx + k;
				const uint32_t global_tri = global_idx / 3u;
				if (global_tri >= total_tri_groups) {
					continue;
				}
				const uint16_t tri_group = decode_uint16(grp_bytes + global_tri * 2u);
				const uint32_t ia = v_off + decode_uint32(idx_bytes + (global_idx + 0) * 4u);
				const uint32_t ib = v_off + decode_uint32(idx_bytes + (global_idx + 1) * 4u);
				const uint32_t ic = v_off + decode_uint32(idx_bytes + (global_idx + 2) * 4u);
				if (ia >= total_verts || ib >= total_verts || ic >= total_verts) {
					continue;
				}
				const Vector3 pa(pos_ptr[uint64_t(ia) * 3u + 0], pos_ptr[uint64_t(ia) * 3u + 1], pos_ptr[uint64_t(ia) * 3u + 2]);
				const Vector3 pb(pos_ptr[uint64_t(ib) * 3u + 0], pos_ptr[uint64_t(ib) * 3u + 1], pos_ptr[uint64_t(ib) * 3u + 2]);
				const Vector3 pc(pos_ptr[uint64_t(ic) * 3u + 0], pos_ptr[uint64_t(ic) * 3u + 1], pos_ptr[uint64_t(ic) * 3u + 2]);
				uint8_t tri_axis = 0;
				if (!_nextgi_stage2_dominant_face_axis(pa, pb, pc, tri_axis)) {
					continue;
				}
				const uint32_t ga_key = (uint32_t(tri_group) << 8u) | uint32_t(tri_axis);
				const uint32_t *card_slot = group_axis_to_card.getptr(ga_key);
				if (card_slot == nullptr) {
					continue;
				}
				const Vector3 cross = (pc - pa).cross(pb - pa);
				const real_t len = cross.length();
				const Vector3 fn = len > CMP_EPSILON2 ? (cross / len) : _nextgi_src_axis_seeds[tri_axis];
				CardTri ct;
				ct.pa = pa;
				ct.pb = pb;
				ct.pc = pc;
				ct.fn = fn;
				card_buckets[uint64_t(*card_slot) * uint32_t(surface_count) + uint32_t(s)].tris.push_back(ct);
			}
		}
	}

	r_state.proxy_meshes.reserve(card_count);
	r_state.proxy_instances.reserve(card_count);

	for (uint32_t c = 0; c < card_count; c++) {
		const RID proxy_mesh = mesh_storage->mesh_allocate();
		mesh_storage->mesh_initialize(proxy_mesh);
		mesh_storage->mesh_nextgi_set_skip_rt_arena(proxy_mesh, true);

		Vector<RID> card_surface_materials;
		for (int s = 0; s < surface_count; s++) {
			const LocalVector<CardTri> &tris = card_buckets[uint64_t(c) * uint32_t(surface_count) + uint32_t(s)].tris;
			if (tris.is_empty()) {
				continue;
			}
			const uint32_t vtx_count = tris.size() * 3u;
			PackedVector3Array positions;
			PackedVector3Array normals;
			PackedVector2Array uvs;
			PackedInt32Array indices;
			positions.resize(int(vtx_count));
			normals.resize(int(vtx_count));
			uvs.resize(int(vtx_count));
			indices.resize(int(vtx_count));
			Vector3 *pos_w = positions.ptrw();
			Vector3 *nrm_w = normals.ptrw();
			Vector2 *uv_w = uvs.ptrw();
			int32_t *idx_w = indices.ptrw();
			for (uint32_t t = 0; t < tris.size(); t++) {
				const CardTri &ct = tris[t];
				const uint32_t base = t * 3u;
				pos_w[base + 0] = ct.pa;
				pos_w[base + 1] = ct.pb;
				pos_w[base + 2] = ct.pc;
				nrm_w[base + 0] = ct.fn;
				nrm_w[base + 1] = ct.fn;
				nrm_w[base + 2] = ct.fn;
				uv_w[base + 0] = Vector2();
				uv_w[base + 1] = Vector2();
				uv_w[base + 2] = Vector2();
				idx_w[base + 0] = int32_t(base + 0);
				idx_w[base + 1] = int32_t(base + 1);
				idx_w[base + 2] = int32_t(base + 2);
			}
			Array arrays;
			arrays.resize(RSE::ARRAY_MAX);
			arrays[RSE::ARRAY_VERTEX] = positions;
			arrays[RSE::ARRAY_NORMAL] = normals;
			arrays[RSE::ARRAY_TEX_UV] = uvs;
			arrays[RSE::ARRAY_INDEX] = indices;
			RenderingServerTypes::SurfaceData sd;
			if (server->mesh_create_surface_data_from_arrays(&sd, RSE::PRIMITIVE_TRIANGLES, arrays) == OK) {
				const RID override_mat = s < r_state.surface_materials.size() ? r_state.surface_materials[s] : RID();
				sd.material = override_mat.is_valid() ? override_mat : mesh_storage->mesh_surface_get_material(r_state.mesh, s);
				mesh_storage->mesh_add_surface(proxy_mesh, sd);
				card_surface_materials.push_back(override_mat);
			}
		}

		if (mesh_storage->mesh_get_surface_count(proxy_mesh) == 0) {
			PackedVector3Array p_pos;
			PackedVector3Array p_nrm;
			PackedVector2Array p_uvs;
			PackedInt32Array p_idx;
			p_pos.resize(3);
			p_nrm.resize(3);
			p_uvs.resize(3);
			p_idx.resize(3);
			for (int k = 0; k < 3; k++) {
				p_pos.set(k, Vector3());
				p_nrm.set(k, Vector3(0, 1, 0));
				p_uvs.set(k, Vector2());
				p_idx.set(k, k);
			}
			Array arrays;
			arrays.resize(RSE::ARRAY_MAX);
			arrays[RSE::ARRAY_VERTEX] = p_pos;
			arrays[RSE::ARRAY_NORMAL] = p_nrm;
			arrays[RSE::ARRAY_TEX_UV] = p_uvs;
			arrays[RSE::ARRAY_INDEX] = p_idx;
			RenderingServerTypes::SurfaceData sd;
			if (server->mesh_create_surface_data_from_arrays(&sd, RSE::PRIMITIVE_TRIANGLES, arrays) == OK) {
				const RID override_mat = !r_state.surface_materials.is_empty() ? r_state.surface_materials[0] : RID();
				sd.material = override_mat.is_valid() ? override_mat : mesh_storage->mesh_surface_get_material(r_state.mesh, 0);
				mesh_storage->mesh_add_surface(proxy_mesh, sd);
				card_surface_materials.push_back(override_mat);
			}
		}

		RenderGeometryInstance *instance = scene->geometry_instance_create(proxy_mesh);
		if (instance == nullptr) {
			mesh_storage->free(proxy_mesh);
			_release_stage2_proxy_instances(r_state);
			return false;
		}
		const AABB local_bounds = mesh_storage->mesh_get_aabb(proxy_mesh);
		instance->set_material_override(r_state.material_override);
		instance->set_surface_materials(card_surface_materials);
		instance->set_transform(Transform3D(), local_bounds, local_bounds);
		r_state.proxy_meshes.push_back(proxy_mesh);
		r_state.proxy_instances.push_back(instance);
	}
	return true;
}

bool NextGISRC::_build_card_set(RID p_mesh, uint64_t p_key, bool p_axis_cards) {
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (mesh_storage == nullptr || !mesh_storage->owns_mesh(p_mesh)) {
		return false;
	}
	if (!p_axis_cards && RSG::virtual_mesh_storage != nullptr) {
		const RenderingServerTypes::VirtualGeometryDesc *vgd_desc = RSG::virtual_mesh_storage->mesh_get_virtual_geometry_desc(p_mesh);
		if (vgd_desc != nullptr && !vgd_desc->card_descriptors.is_empty()) {
			return _build_stage2_card_set(p_mesh, p_key);
		}
	}
	// The cards cover exactly the surfaces in the mesh's BLAS parts, so hits always find a card set.
	// No parts yet means the RT scene has not built them; try again next frame.
	const LocalVector<MeshStorage::NextGIRTPart> *parts = mesh_storage->mesh_nextgi_rt_get_parts(p_mesh);
	if (parts == nullptr) {
		return false;
	}
	LocalVector<uint32_t> surfaces;
	for (const MeshStorage::NextGIRTPart &part : *parts) {
		for (const NextGIRTPartGeometry &geometry : part.geometries) {
			if (!surfaces.has(geometry.surface)) {
				surfaces.push_back(geometry.surface);
			}
		}
	}
	if (surfaces.is_empty()) {
		// GodotNext: nextgi_skinned_actors (#59). A skinned mesh has no static BLAS parts; its
		// bind-pose cards come from the mesh's CPU geometry instead.
		if (!mesh_storage->mesh_nextgi_rt_is_skinned(p_mesh)) {
			return false; // No parts yet: the RT scene has not built them; try again next frame.
		}
		const int surface_count = mesh_storage->mesh_get_surface_count(p_mesh);
		for (int surface = 0; surface < surface_count; surface++) {
			surfaces.push_back(uint32_t(surface));
		}
	}

	LocalVector<float> positions;
	LocalVector<uint32_t> indices;
	HashMap<uint32_t, float> surface_extents;
	for (uint32_t surface : surfaces) {
		Vector<float> surface_positions;
		Vector<uint32_t> surface_indices;
		if (!mesh_storage->mesh_nextgi_get_cpu_geometry(p_mesh, surface, surface_positions, surface_indices)) {
			continue;
		}
		const uint32_t base = positions.size() / 3;
		// GodotNext: nextgi_emissive_contract (#60). Track the longest axis of this surface.
		Vector3 lo(Math::INF, Math::INF, Math::INF);
		Vector3 hi(-Math::INF, -Math::INF, -Math::INF);
		for (int i = 0; i + 2 < surface_positions.size(); i += 3) {
			const Vector3 vertex(surface_positions[i], surface_positions[i + 1], surface_positions[i + 2]);
			lo = lo.min(vertex);
			hi = hi.max(vertex);
		}
		if (lo.x <= hi.x) {
			const Vector3 extent = hi - lo;
			surface_extents.insert(surface, MAX(extent.x, MAX(extent.y, extent.z)));
		}
		for (int i = 0; i < surface_positions.size(); i++) {
			positions.push_back(surface_positions[i]);
		}
		for (int i = 0; i < surface_indices.size(); i++) {
			indices.push_back(surface_indices[i] + base);
		}
	}

	NextGICardClusterResult result;
	if (p_axis_cards) {
		AABB bounds;
		for (uint32_t i = 0; i < positions.size(); i += 3) {
			bounds.expand_to(Vector3(positions[i], positions[i + 1], positions[i + 2]));
		}
		nextgi_src_axis_cards(bounds, result);
	} else {
		nextgi_src_cluster_cards(positions.ptr(), positions.size() / 3, indices.ptr(), indices.size(), result);
	}
	if (result.cards.is_empty()) {
		// No CPU geometry (already released) or only degenerate triangles.
		failed_meshes.insert(p_mesh);
		stats.failed_mesh_count++;
		return false;
	}

	LocalVector<Vector2i> sizes;
	for (const NextGICardDesc &card : result.cards) {
		sizes.push_back(Vector2i(int(card.texels_u), int(card.texels_v)));
	}
	LocalVector<Rect2i> rects;
	if (!packer.allocate_all(sizes, rects)) {
		WARN_PRINT_ONCE("The NextGI SRC material atlas is full; meshes without space get no cards (the P1b LRU, #41, evicts).");
		failed_meshes.insert(p_mesh);
		stats.failed_mesh_count++;
		return false;
	}

	const uint32_t set_index = card_sets.size();
	NextGICardSet set;
	set.first_card = cards.size();
	set.card_count = result.cards.size();
	set.texel_size = result.texel_size;
	set.capture_scale = 1.0f;
	for (uint32_t i = 0; i < result.cards.size(); i++) {
		const NextGICardDesc &desc = result.cards[i];
		NextGICard card;
		card.origin[0] = desc.origin.x;
		card.origin[1] = desc.origin.y;
		card.origin[2] = desc.origin.z;
		card.origin[3] = 0.0f;
		card.axis_u[0] = desc.axis_u.x;
		card.axis_u[1] = desc.axis_u.y;
		card.axis_u[2] = desc.axis_u.z;
		card.axis_u[3] = desc.half_u;
		card.axis_v[0] = desc.axis_v.x;
		card.axis_v[1] = desc.axis_v.y;
		card.axis_v[2] = desc.axis_v.z;
		card.axis_v[3] = desc.half_v;
		card.axis_n[0] = desc.axis_n.x;
		card.axis_n[1] = desc.axis_n.y;
		card.axis_n[2] = desc.axis_n.z;
		card.axis_n[3] = desc.depth_range;
		card.atlas_rect[0] = uint32_t(rects[i].position.x);
		card.atlas_rect[1] = uint32_t(rects[i].position.y);
		card.atlas_rect[2] = uint32_t(rects[i].size.x);
		card.atlas_rect[3] = uint32_t(rects[i].size.y);
		cards.push_back(card);
		card_descs.push_back(desc);
		card_capture_counts.push_back(0);
	}
	card_sets.push_back(set);
	CardSetState state;
	state.mesh = p_mesh;
	state.surface_extents = surface_extents;
	card_set_states.push_back(state);
	if (!mesh_card_sets.has(p_mesh)) {
		mesh_card_sets.insert(p_mesh, set_index);
	}
	key_card_sets.insert(p_key, set_index);
	stats.material_capture_count = key_card_sets.size();
	stats.card_set_count++;
	stats.card_count += set.card_count;
	gpu_dirty = true;
	print_line(vformat("NextGI SRC: card set %d: %d cards, texel %.4f, mesh extent %.3f.", set_index, set.card_count, set.texel_size, result.max_extent));
	return true;
}

void NextGISRC::_upload_gpu_data() {
	RD *rd = RD::get_singleton();
	gpu_dirty = false;
	if (cards.size() > card_capacity) {
		if (card_buffer.is_valid()) {
			rd->free_rid(card_buffer);
		}
		card_capacity = MAX(cards.size(), card_capacity * 2);
		card_buffer = rd->storage_buffer_create(card_capacity * sizeof(NextGICard));
		rd->set_resource_name(card_buffer, "NextGI SRC Cards");
	}
	if (card_sets.size() > card_set_capacity) {
		if (card_set_buffer.is_valid()) {
			rd->free_rid(card_set_buffer);
		}
		card_set_capacity = MAX(card_sets.size(), card_set_capacity * 2);
		card_set_buffer = rd->storage_buffer_create(card_set_capacity * sizeof(NextGICardSet));
		rd->set_resource_name(card_set_buffer, "NextGI SRC Card Sets");
	}
	if (card_buffer.is_valid() && !cards.is_empty()) {
		rd->buffer_update(card_buffer, 0, cards.size() * sizeof(NextGICard), cards.ptr());
	}
	if (card_set_buffer.is_valid() && !card_sets.is_empty()) {
		rd->buffer_update(card_set_buffer, 0, card_sets.size() * sizeof(NextGICardSet), card_sets.ptr());
	}
}

void NextGISRC::capture(RenderDataRD *p_render_data, bool p_shadow_batch_open) {
	// Capture shares RENDER_LIST_SECONDARY with the shadow batch (§8.3), so it runs only after
	// _render_shadow_end.
	if (p_shadow_batch_open) {
		stats.shadow_batch_refusals++;
	}
	DEV_ASSERT(!p_shadow_batch_open);
	ERR_FAIL_COND_MSG(p_shadow_batch_open, "NextGI SRC capture must not run inside a shadow batch.");
	ERR_FAIL_NULL(p_render_data);
	if (atlas_fb.is_null()) {
		return;
	}

	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	const RendererSceneRender::RenderSDFGIUpdateData *update_data = p_render_data->sdfgi_update_data;
	struct PendingCard {
		RenderGeometryInstance *instance = nullptr;
		Transform3D instance_transform;
		uint32_t card = 0;
		uint32_t set_index = 0;
		float solid_angle = 0.0f;
		uint32_t texels = 0;
		RID mesh;
	};
	LocalVector<PendingCard> pending;
	HashSet<uint32_t> queued_cards;
	if (update_data != nullptr && update_data->nextgi_rt_instances != nullptr && mesh_storage->mesh_nextgi_rt_is_enabled()) {
		for (const RendererSceneRender::NextGIRTInstance &fed : *update_data->nextgi_rt_instances) {
			RenderGeometryInstanceBase *geometry_instance = static_cast<RenderGeometryInstanceBase *>(fed.geometry_instance);
			if (geometry_instance == nullptr || geometry_instance->data == nullptr) {
				continue;
			}
			const RID mesh = fed.mesh_override.is_valid() ? fed.mesh_override : geometry_instance->data->base;
			if (!mesh_storage->owns_mesh(mesh) || failed_meshes.has(mesh)) {
				continue;
			}
			const uint64_t key = _nextgi_src_instance_key(geometry_instance);
			const uint32_t *set_index = key_card_sets.getptr(key);
			if (set_index == nullptr) {
				if (!_build_card_set(mesh, key)) {
					continue;
				}
				set_index = key_card_sets.getptr(key);
				ERR_CONTINUE(set_index == nullptr);
				_check_emissive_contract(geometry_instance, *set_index);
			}
			CardSetState &state = card_set_states[*set_index];
			RenderGeometryInstance *capture_instance = geometry_instance;
			Transform3D capture_transform = fed.has_rt_transform ? fed.rt_transform : geometry_instance->transform;
			if (state.is_stage2) {
				state.material_override = geometry_instance->data->material_override;
				state.surface_materials = geometry_instance->data->surface_materials;
			}
			const NextGICardSet &set = card_sets[*set_index];
			for (uint32_t card = set.first_card; card < set.first_card + set.card_count; card++) {
				if (card_capture_counts[card] != 0 || queued_cards.has(card)) {
					continue;
				}
				if (state.is_stage2) {
					if (!_ensure_stage2_proxy_instances(state)) {
						break;
					}
					const uint32_t local_card = card - set.first_card;
					ERR_CONTINUE(local_card >= state.proxy_instances.size());
					capture_instance = state.proxy_instances[local_card];
					capture_transform = Transform3D();
				}
				queued_cards.insert(card);
				PendingCard item;
				item.instance = capture_instance;
				item.instance_transform = capture_transform;
				item.card = card;
				item.set_index = *set_index;
				item.solid_angle = nextgi_src_solid_angle(Math::sin(fed.angular_radius), 1.0f);
				item.texels = cards[card].atlas_rect[2] * cards[card].atlas_rect[3];
				item.mesh = mesh;
				pending.push_back(item);
			}
		}
	}
	NextGI *next_gi = NextGI::get_singleton();
	if (next_gi != nullptr && next_gi->get_rt_scene() != nullptr && next_gi->get_rt_scene()->get_far_field() != nullptr) {
		LocalVector<NextGIFarField::CaptureSource> far_sources;
		next_gi->get_rt_scene()->get_far_field()->capture_sources(far_sources);
		for (const NextGIFarField::CaptureSource &source : far_sources) {
			if (source.instance == nullptr || failed_meshes.has(source.mesh)) {
				continue;
			}
			const uint64_t key = _nextgi_src_instance_key(source.instance);
			const uint32_t *set_index = key_card_sets.getptr(key);
			if (set_index == nullptr) {
				if (!_build_card_set(source.mesh, key, true)) {
					continue;
				}
				set_index = key_card_sets.getptr(key);
				ERR_CONTINUE(set_index == nullptr);
				_check_emissive_contract(source.instance, *set_index);
			}
			const NextGICardSet &set = card_sets[*set_index];
			for (uint32_t card = set.first_card; card < set.first_card + set.card_count; card++) {
				if (card_capture_counts[card] != 0 || queued_cards.has(card)) {
					continue;
				}
				queued_cards.insert(card);
				PendingCard item;
				item.instance = source.instance;
				item.instance_transform = source.transform;
				item.card = card;
				item.set_index = *set_index;
				item.solid_angle = nextgi_src_solid_angle(Math::sin(source.angular_radius), 1.0f);
				item.texels = cards[card].atlas_rect[2] * cards[card].atlas_rect[3];
				item.mesh = source.mesh;
				pending.push_back(item);
			}
		}
	}
	LocalVector<NextGICaptureCandidate> candidates;
	candidates.resize(pending.size());
	for (uint32_t i = 0; i < pending.size(); i++) {
		candidates[i].id = i;
		candidates[i].solid_angle = pending[i].solid_angle;
		candidates[i].texels = pending[i].texels;
		candidates[i].first_residency = true;
	}
	LocalVector<uint32_t> accepted;
	accepted.resize(pending.size());
	const uint32_t accepted_count = pending.is_empty() ? 0 : nextgi_src_select_captures(candidates.ptr(), candidates.size(), accepted.ptr(), NEXTGI_SRC_CAPTURE_CARD_CAP, NEXTGI_SRC_CAPTURE_TEXEL_CAP);
	LocalVector<RendererSceneRenderRD::NextGICardCapture> batch;
	uint32_t capture_texels = 0;
	for (uint32_t i = 0; i < accepted_count; i++) {
		const PendingCard &item = pending[accepted[i]];
		const NextGICardDesc &desc = card_descs[item.card];
		RendererSceneRenderRD::NextGICardCapture capture_card;
		capture_card.instance = item.instance;
		capture_card.cam_transform = item.instance_transform * desc.get_transform();
		const double z_near = MIN(0.0001, double(desc.depth_range) * 0.01);
		capture_card.projection.set_orthogonal(-desc.half_u, desc.half_u, -desc.half_v, desc.half_v, z_near, MAX(double(desc.depth_range), z_near * 2.0));
		const NextGICard &card = cards[item.card];
		capture_card.rect = Rect2i(int(card.atlas_rect[0]), int(card.atlas_rect[1]), int(card.atlas_rect[2]), int(card.atlas_rect[3]));
		batch.push_back(capture_card);
		capture_texels += item.texels;
	}
	stats.capture_texels = capture_texels;
	stats.capture_draw_lists = 0;
	stats.capture_cpu_ms = 0.0f;
	if (!batch.is_empty()) {
		RendererSceneRenderRD *scene_render = RendererSceneRenderRD::get_singleton();
		const uint64_t ticks = OS::get_singleton() != nullptr ? OS::get_singleton()->get_ticks_usec() : 0;
		const bool capturing_timestamps = RSG::utilities->capturing_timestamps;
		RSG::utilities->capturing_timestamps = false;
		stats.capture_draw_lists = scene_render->_render_nextgi_card_batch(batch.ptr(), batch.size(), atlas_fb);
		RSG::utilities->capturing_timestamps = capturing_timestamps;
		if (OS::get_singleton() != nullptr && ticks != 0) {
			stats.capture_cpu_ms = float(OS::get_singleton()->get_ticks_usec() - ticks) / 1000.0f;
		}
		for (uint32_t i = 0; i < accepted_count; i++) {
			const PendingCard &item = pending[accepted[i]];
			card_capture_counts[item.card]++;
			stats.capture_count++;
			stats.max_captures_per_card = MAX(stats.max_captures_per_card, card_capture_counts[item.card]);
			CardSetState &state = card_set_states[item.set_index];
			if (card_capture_counts[item.card] == 1) {
				stats.captured_card_count++;
				if (state.is_stage2) {
					stats.stage2_captured_card_count++;
				}
			}
			state.captured++;
			if (state.captured >= card_sets[item.set_index].card_count) {
				if (state.is_stage2) {
					_release_stage2_proxy_instances(state);
				}
				if (!state.released) {
					mesh_storage->mesh_nextgi_release_cpu_geometry(item.mesh);
					state.released = true;
				}
			}
		}
		stats.capture_frames++;
	}
	if (gpu_dirty) {
		_upload_gpu_data();
	}
	if (dump_state == DUMP_WAITING && stats.card_count > 0 && stats.captured_card_count == stats.card_count) {
		_request_dump();
	}
}

void NextGISRC::_request_dump() {
	dump_state = DUMP_REQUESTED;
	dump_pending = 0;
	RD *rd = RD::get_singleton();
	for (int i = 0; i < ATLAS_TARGET_MAX; i++) {
		if (rd->texture_get_data_async(atlas[i], 0, callable_mp_static(&NextGISRC::_dump_data_received).bind(i)) == OK) {
			dump_pending++;
		}
	}
	if (dump_pending == 0) {
		dump_state = DUMP_DONE;
	}
}

void NextGISRC::_dump_data_received(const Vector<uint8_t> &p_data, int p_target) {
	// The download may finish after the SRC is gone.
	if (singleton != nullptr) {
		singleton->_on_dump_data(p_data, p_target);
	}
}

void NextGISRC::_on_dump_data(const Vector<uint8_t> &p_data, int p_target) {
	ERR_FAIL_INDEX(p_target, ATLAS_TARGET_MAX);
	if (dump_pending > 0) {
		dump_pending--;
	}
	if (dump_pending == 0) {
		dump_state = DUMP_DONE;
	}

	static const Image::Format formats[ATLAS_TARGET_MAX] = {
		Image::FORMAT_RGBA8,
		Image::FORMAT_RGBA8,
		Image::FORMAT_RGBA8,
		Image::FORMAT_RGBAH,
		Image::FORMAT_RF,
	};
	const int size = NEXTGI_SRC_ATLAS_SIZE;
	ERR_FAIL_COND_MSG(p_data.size() != Image::get_image_data_size(size, size, formats[p_target], false), vformat("Unexpected NextGI SRC %s atlas download size %d.", _nextgi_src_target_names[p_target], p_data.size()));

	const String path = vformat("user://nextgi_atlas_%s.exr", _nextgi_src_target_names[p_target]);
	const Ref<Image> image = Image::create_from_data(size, size, false, formats[p_target], p_data);
	const Error err = image.is_valid() ? image->save_exr(path) : ERR_CANT_CREATE;
	if (err == OK) {
		print_line(vformat("NextGI SRC: saved %s.", path));
	} else {
		ERR_PRINT(vformat("Could not save the NextGI SRC atlas dump %s (error %d).", path, int(err)));
	}

	if (p_target != ATLAS_DEPTH) {
		return;
	}
	const float *depth = reinterpret_cast<const float *>(p_data.ptr());
	card_coverage.resize(cards.size());
	float coverage_min = 1.0f;
	for (uint32_t i = 0; i < cards.size(); i++) {
		const Rect2i rect(int(cards[i].atlas_rect[0]), int(cards[i].atlas_rect[1]), int(cards[i].atlas_rect[2]), int(cards[i].atlas_rect[3]));
		card_coverage[i] = nextgi_src_card_coverage(depth, size, size, rect);
		coverage_min = MIN(coverage_min, card_coverage[i]);
	}
	for (uint32_t s = 0; s < card_sets.size(); s++) {
		String list;
		for (uint32_t i = card_sets[s].first_card; i < card_sets[s].first_card + card_sets[s].card_count; i++) {
			list += (list.is_empty() ? "" : " ") + String::num(card_coverage[i], 3);
		}
		print_line(vformat("NextGI SRC: card set %d coverage: %s.", s, list));
	}
	print_line(vformat("NextGI SRC: %d cards in %d card sets, minimum coverage %.3f.", cards.size(), card_sets.size(), coverage_min));
}

void NextGISRC::invalidate_captures(uint32_t p_card_set) {
	if (p_card_set >= card_sets.size()) {
		return;
	}
	const NextGICardSet &set = card_sets[p_card_set];
	CardSetState &state = card_set_states[p_card_set];
	for (uint32_t i = 0; i < set.card_count; i++) {
		const uint32_t card = set.first_card + i;
		if (card < card_capture_counts.size() && card_capture_counts[card] > 0) {
			if (stats.captured_card_count > 0) {
				stats.captured_card_count--;
			}
			if (state.is_stage2 && stats.stage2_captured_card_count > 0) {
				stats.stage2_captured_card_count--;
			}
			card_capture_counts[card] = 0;
		}
	}
	if (state.is_stage2) {
		_release_stage2_proxy_instances(state);
	}
	state.captured = 0;
	state.released = false;
}

void NextGISRC::fill_info(Dictionary &r_info) const {
	r_info["src_emissive_below_card_res"] = int64_t(stats.emissive_below_card_res);
	r_info["src_card_set_count"] = int64_t(stats.card_set_count);
	r_info["src_card_count"] = int64_t(stats.card_count);
	r_info["src_captured_card_count"] = int64_t(stats.captured_card_count);
	r_info["src_stage2_cards_captured"] = int64_t(stats.stage2_captured_card_count);
	r_info["src_capture_count"] = int64_t(stats.capture_count);
	r_info["src_max_captures_per_card"] = int64_t(stats.max_captures_per_card);
	r_info["src_capture_frames"] = int64_t(stats.capture_frames);
	r_info["src_shadow_batch_refusals"] = int64_t(stats.shadow_batch_refusals);
	r_info["src_failed_mesh_count"] = int64_t(stats.failed_mesh_count);
	r_info["src_atlas_memory"] = int64_t(atlas_memory);
	r_info["src_capture_texels"] = int64_t(stats.capture_texels);
	r_info["src_capture_draw_lists"] = int64_t(stats.capture_draw_lists);
	r_info["capture_cpu_ms"] = stats.capture_cpu_ms;
	r_info["src_material_captures"] = int64_t(stats.material_capture_count);
	r_info["src_atlas_used_texels"] = int64_t(packer.get_used_texels());
	r_info["src_dump_done"] = dump_state == DUMP_DONE;

	PackedInt32Array set_card_counts;
	for (const NextGICardSet &set : card_sets) {
		set_card_counts.push_back(int32_t(set.card_count));
	}
	r_info["src_card_set_card_counts"] = set_card_counts;
	PackedInt32Array card_sizes;
	for (const NextGICard &card : cards) {
		card_sizes.push_back(int32_t(card.atlas_rect[2]));
		card_sizes.push_back(int32_t(card.atlas_rect[3]));
	}
	r_info["src_card_sizes"] = card_sizes; // w, h per card.
	PackedFloat32Array coverage;
	for (float value : card_coverage) {
		coverage.push_back(value);
	}
	r_info["src_card_coverage"] = coverage; // Empty until the debug dump's depth download.
}

NextGISRC::NextGISRC() {
	if (singleton == nullptr) {
		singleton = this;
	}
}

NextGISRC::~NextGISRC() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

} // namespace RendererRD
