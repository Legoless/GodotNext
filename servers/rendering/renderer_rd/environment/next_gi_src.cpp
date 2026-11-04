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
#include "core/math/aabb.h"
#include "core/math/projection.h"
#include "core/object/callable_mp.h"
#include "core/string/print_string.h"
#include "core/variant/variant.h"
#include "servers/rendering/renderer_geometry_instance.h"
#include "servers/rendering/renderer_rd/environment/next_gi.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_globals.h"

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

void NextGIShelfPacker::reset() {
	top = 0;
	used_texels = 0;
	shelves.clear();
}

// NextGI entry points (next_gi.h) for the cards.

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
	// Worded without the "NextGI:" prefix, so the tier stays the only such line in the log.
	print_line(vformat("NextGI SRC: material atlas %dx%d, %.1f MiB of the %d MiB High budget (framebuffer depth %s).", NEXTGI_SRC_ATLAS_SIZE, NEXTGI_SRC_ATLAS_SIZE, double(atlas_memory) / (1024.0 * 1024.0), NEXTGI_SRC_MATERIAL_BUDGET_HIGH / (1024 * 1024), d16 ? "D16" : "X8_D24"));
	ERR_FAIL_COND_MSG(atlas_memory > uint64_t(NEXTGI_SRC_MATERIAL_BUDGET_HIGH), "The NextGI SRC material atlas is over the High budget.");
}

void NextGISRC::init() {
	ERR_FAIL_COND(atlas_fb.is_valid());
	ERR_FAIL_NULL(RD::get_singleton());
	_create_atlas();
	dump_state = bool(GLOBAL_GET("rendering/global_illumination/nextgi/debug/dump_material_atlas")) ? DUMP_WAITING : DUMP_OFF;
}

void NextGISRC::free() {
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

bool NextGISRC::_build_card_set(RID p_mesh) {
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (mesh_storage == nullptr || !mesh_storage->owns_mesh(p_mesh)) {
		return false;
	}
	// The cards cover exactly the surfaces in the mesh's BLAS parts, so hits always find a card set.
	// No parts yet means the RT scene has not built them; try again next frame.
	const LocalVector<MeshStorage::NextGIRTPart> *parts = mesh_storage->mesh_nextgi_rt_get_parts(p_mesh);
	if (parts == nullptr || parts->is_empty()) {
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

	LocalVector<float> positions;
	LocalVector<uint32_t> indices;
	for (uint32_t surface : surfaces) {
		Vector<float> surface_positions;
		Vector<uint32_t> surface_indices;
		if (!mesh_storage->mesh_nextgi_get_cpu_geometry(p_mesh, surface, surface_positions, surface_indices)) {
			continue;
		}
		const uint32_t base = positions.size() / 3;
		for (int i = 0; i < surface_positions.size(); i++) {
			positions.push_back(surface_positions[i]);
		}
		for (int i = 0; i < surface_indices.size(); i++) {
			indices.push_back(surface_indices[i] + base);
		}
	}

	NextGICardClusterResult result;
	nextgi_src_cluster_cards(positions.ptr(), positions.size() / 3, indices.ptr(), indices.size(), result);
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
	card_set_states.push_back(state);
	mesh_card_sets.insert(p_mesh, set_index);
	stats.card_set_count++;
	stats.card_count += set.card_count;
	gpu_dirty = true;
	print_line(vformat("NextGI SRC: card set %d: %d cards, texel %.4f, mesh extent %.3f.", set_index, set.card_count, set.texel_size, result.max_extent));
	return true;
}

void NextGISRC::_capture_card(RenderGeometryInstance *p_instance, const Transform3D &p_instance_transform, uint32_t p_card) {
	const NextGICardDesc &desc = card_descs[p_card];
	const NextGICard &card = cards[p_card];

	// The VoxelGI dynamic-object pattern (GI::VoxelGIInstance::update).
	Projection cm;
	cm.set_orthogonal(-desc.half_u, desc.half_u, -desc.half_v, desc.half_v, 0.0001, desc.depth_range);
	RendererSceneRenderRD *scene_render = RendererSceneRenderRD::get_singleton();
	if (scene_render->cull_argument.size() == 0) {
		scene_render->cull_argument.push_back(nullptr);
	}
	scene_render->cull_argument[0] = p_instance;

	// _render_material clears its region and draws the one instance. Its own timestamps would split
	// line 2a into several intervals (Q11), so they are off for the call.
	const Rect2i rect(int(card.atlas_rect[0]), int(card.atlas_rect[1]), int(card.atlas_rect[2]), int(card.atlas_rect[3]));
	const bool capturing_timestamps = RSG::utilities->capturing_timestamps;
	RSG::utilities->capturing_timestamps = false;
	scene_render->_render_material(p_instance_transform * desc.get_transform(), cm, true, scene_render->cull_argument, atlas_fb, rect, 1.0);
	RSG::utilities->capturing_timestamps = capturing_timestamps;

	card_capture_counts[p_card]++;
	stats.capture_count++;
	stats.max_captures_per_card = MAX(stats.max_captures_per_card, card_capture_counts[p_card]);
	if (card_capture_counts[p_card] == 1) {
		stats.captured_card_count++;
	}
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
	bool captured = false;
	if (update_data != nullptr && update_data->nextgi_rt_instances != nullptr && mesh_storage->mesh_nextgi_rt_is_enabled()) {
		for (const RendererSceneRender::NextGIRTInstance &fed : *update_data->nextgi_rt_instances) {
			RenderGeometryInstanceBase *geometry_instance = static_cast<RenderGeometryInstanceBase *>(fed.geometry_instance);
			if (geometry_instance == nullptr || geometry_instance->data == nullptr) {
				continue;
			}
			const RID mesh = geometry_instance->data->base;
			if (!mesh_storage->owns_mesh(mesh)) {
				continue;
			}
			const uint32_t *set_index = mesh_card_sets.getptr(mesh);
			if (set_index == nullptr) {
				if (failed_meshes.has(mesh) || !_build_card_set(mesh)) {
					continue;
				}
				set_index = mesh_card_sets.getptr(mesh);
				ERR_CONTINUE(set_index == nullptr);
			}

			// P1a captures each card exactly once, with this frame's instance as the representative,
			// and never invalidates (dynamic invalidation is P1b).
			CardSetState &state = card_set_states[*set_index];
			const NextGICardSet &set = card_sets[*set_index];
			if (state.captured < set.card_count) {
				for (uint32_t card = set.first_card; card < set.first_card + set.card_count; card++) {
					if (card_capture_counts[card] == 0) {
						_capture_card(geometry_instance, geometry_instance->transform, card);
						captured = true;
					}
				}
				state.captured = set.card_count;
			}
			if (!state.released) {
				mesh_storage->mesh_nextgi_release_cpu_geometry(mesh);
				state.released = true;
			}
		}
	}
	if (captured) {
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

void NextGISRC::fill_info(Dictionary &r_info) const {
	r_info["src_card_set_count"] = int64_t(stats.card_set_count);
	r_info["src_card_count"] = int64_t(stats.card_count);
	r_info["src_captured_card_count"] = int64_t(stats.captured_card_count);
	r_info["src_capture_count"] = int64_t(stats.capture_count);
	r_info["src_max_captures_per_card"] = int64_t(stats.max_captures_per_card);
	r_info["src_capture_frames"] = int64_t(stats.capture_frames);
	r_info["src_shadow_batch_refusals"] = int64_t(stats.shadow_batch_refusals);
	r_info["src_failed_mesh_count"] = int64_t(stats.failed_mesh_count);
	r_info["src_atlas_memory"] = int64_t(atlas_memory);
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
