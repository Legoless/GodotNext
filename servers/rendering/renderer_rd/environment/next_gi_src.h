/**************************************************************************/
/*  next_gi_src.h                                                         */
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

#include "core/math/rect2i.h"
#include "core/math/transform_3d.h"
#include "core/math/vector3.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

class RenderDataRD;
class RenderGeometryInstance;

namespace RendererRD {

// Surface Radiance Cache (SRC) Stage-1 cards (#30, plan §8.3 and D6): up to 12 normal-clustered
// cards per mesh, each captured once with _render_material into the material atlas. Everything
// above NextGISRC is device-free so doctests run it without a GPU.

enum {
	NEXTGI_SRC_MAX_CARDS_PER_MESH = 12,
	NEXTGI_SRC_CARD_MAX_TEXELS = 128, // Per axis; also the divisor of the texel size.
	NEXTGI_SRC_CARD_MIN_TEXELS = 4, // Per axis.
	NEXTGI_SRC_KMEANS_ITERATIONS = 8,
	NEXTGI_SRC_ATLAS_SIZE = 2048,
	NEXTGI_SRC_ATLAS_GUTTER = 1, // Texels kept free right of and below each card.
	NEXTGI_SRC_MATERIAL_BUDGET_HIGH = 128 * 1024 * 1024, // Material atlas budget at High (§8.3).
	NEXTGI_SRC_MATERIAL_BUDGET_EPIC = 256 * 1024 * 1024,
	NEXTGI_SRC_CAPTURE_CARD_CAP = 64,
	NEXTGI_SRC_CAPTURE_TEXEL_CAP = 1000000,
	NEXTGI_SRC_PAGE_MIN = 16,
	NEXTGI_SRC_PAGE_LEVEL_MAX = 3,
};

// A face normal within this angle of a seed adds no seed of its own.
constexpr float NEXTGI_SRC_SEED_MIN_ANGLE_DEGREES = 15.0f;
// Clusters with less than this fraction of the mesh's triangle area merge into their nearest neighbor.
constexpr float NEXTGI_SRC_MERGE_AREA_FRACTION = 0.02f;

// std430 GPU records (§8.3, D6). The sizes are part of the contract; do not repack them.
//
// The card frame: a point at capture-view coordinates (x, y, z) is the object-space point
// origin + x * axis_u + y * axis_v + z * axis_n. The capture camera sits at origin and looks along
// -axis_n, so a covered texel's depth is -z, the distance from the card plane, in object units
// (capture_scale is 1 in P1a because the capture view carries the instance transform). Texel
// (tx, ty) of atlas_rect has its center at x = ((tx + 0.5) / w * 2 - 1) * half_u and
// y = ((ty + 0.5) / h * 2 - 1) * half_v, ty = 0 being the v = -half_v row. The normal target holds
// the capture-view normal as encode24(n) * 0.5 + 0.5: decode with normalize(rgb * 2 - 1), then
// rotate by the card frame (n.x * axis_u + n.y * axis_v + n.z * axis_n) to get it in object space.
struct NextGICard {
	float origin[4]; // Object space; w unused.
	float axis_u[4]; // xyz unit; w = half extent along u.
	float axis_v[4]; // xyz unit; w = half extent along v.
	float axis_n[4]; // xyz unit, outward; w = depth range.
	uint32_t atlas_rect[4]; // x, y, w, h in material-atlas texels.
};
static_assert(sizeof(NextGICard) == 80, "NextGICard must stay 80 bytes (plan §8.3).");

struct NextGICardSet {
	uint32_t first_card = 0;
	uint32_t card_count = 0;
	float texel_size = 0.0f; // Object units per card texel, max_extent / 128.
	float capture_scale = 1.0f; // Object units per card depth unit.
};
static_assert(sizeof(NextGICardSet) == 16, "NextGICardSet must stay 16 bytes (plan §8.3).");

// One clustered card in object space, before atlas placement.
struct NextGICardDesc {
	Vector3 origin; // Rect center on the cluster's outermost plane along axis_n, one texel out.
	Vector3 axis_u;
	Vector3 axis_v;
	Vector3 axis_n; // Unit, the cluster's area-weighted mean normal. u x v = n.
	float half_u = 0.0f;
	float half_v = 0.0f;
	float depth_range = 0.0f; // From origin to one texel past the mesh's far side along -axis_n.
	uint32_t texels_u = 0;
	uint32_t texels_v = 0;
	float area = 0.0f; // Triangle area in the cluster.
	uint32_t triangle_count = 0;

	// Card frame to object space; the capture camera's local transform.
	Transform3D get_transform() const;
};

struct NextGICardClusterResult {
	LocalVector<NextGICardDesc> cards;
	float max_extent = 0.0f; // Longest axis of the AABB of the referenced vertices.
	float texel_size = 0.0f; // max_extent / 128.
	float total_area = 0.0f;
};

// Card generation (§8.3 Stage 1). p_positions holds xyz float triples; p_indices whole triangles.
// Area-weighted k-means on face normals with k <= 12: seeded with +X, -X, +Y, -Y, +Z, -Z, then
// with the face normals of largest residual angle (above NEXTGI_SRC_SEED_MIN_ANGLE_DEGREES); 8
// iterations; clusters under NEXTGI_SRC_MERGE_AREA_FRACTION of the total area merge into their
// nearest neighbor. Card texels per axis are clamp(ceil(extent / texel_size), 4, 128).
// Degenerate triangles are ignored.
void nextgi_src_cluster_cards(const float *p_positions, uint32_t p_vertex_count, const uint32_t *p_indices, uint32_t p_index_count, NextGICardClusterResult &r_result);

// The material atlas: albedo, normal and orm RGBA8, emission RGBA16F, depth R32F, plus the
// framebuffer depth (D16 when supported, else X8_D24).
uint64_t nextgi_src_atlas_memory_bytes(uint32_t p_size, uint32_t p_fb_depth_bytes_per_texel);

// Coverage = the fraction of a card's texels with depth > 0 (the clear value is 0).
float nextgi_src_card_coverage(const float *p_depth, uint32_t p_atlas_width, uint32_t p_atlas_height, const Rect2i &p_rect);

// A shelf packer for card rects. Each rect gets NEXTGI_SRC_ATLAS_GUTTER free texels to its right
// and below it. Placement never moves.
class NextGIShelfPacker {
	struct Shelf {
		uint32_t y = 0;
		uint32_t height = 0;
		uint32_t used_width = 0;
	};

	uint32_t width = NEXTGI_SRC_ATLAS_SIZE;
	uint32_t height = NEXTGI_SRC_ATLAS_SIZE;
	uint32_t top = 0;
	uint64_t used_texels = 0;
	LocalVector<Shelf> shelves;

public:
	explicit NextGIShelfPacker(uint32_t p_width = NEXTGI_SRC_ATLAS_SIZE, uint32_t p_height = NEXTGI_SRC_ATLAS_SIZE) :
			width(p_width), height(p_height) {}

	// First shelf that fits; otherwise a new shelf of the rect's height. False when full.
	bool allocate(uint32_t p_width, uint32_t p_height, Rect2i &r_rect);
	// Places every size or none of them, tallest first. r_rects keeps the input order.
	bool allocate_all(const LocalVector<Vector2i> &p_sizes, LocalVector<Rect2i> &r_rects);
	void reset();

	uint64_t get_used_texels() const { return used_texels; }
	uint32_t get_width() const { return width; }
	uint32_t get_height() const { return height; }
};

// High is 4096² radiance (64 MiB at 4 bytes) and a 128 MiB material budget. Epic doubles both:
// radiance becomes 4096×8192.
struct NextGIAtlasBudget {
	uint32_t radiance_width = 4096;
	uint32_t radiance_height = 4096;
	uint64_t radiance_bytes = 0;
	uint64_t material_bytes = 0;
};

NextGIAtlasBudget nextgi_src_atlas_budget(bool p_epic);

// Ω ≈ π r²/d². Distance 0 is a full hemisphere.
float nextgi_src_solid_angle(float p_radius, float p_distance);

// L = clamp(floor(log2(full / needed)), 0, 3). needed grows with solid angle; L = 0 is full resolution.
uint32_t nextgi_src_page_level(float p_solid_angle, uint32_t p_full_texels);

// FNV-1a of the mesh, the override and the surface materials. Equal keys share one material capture.
uint64_t nextgi_src_material_key(uint64_t p_mesh_id, uint64_t p_override_id, const uint64_t *p_surface_ids, uint32_t p_surface_count);

// One capture id per distinct material key.
class NextGIMaterialCaptures {
	HashMap<uint64_t, uint32_t> ids;
	uint32_t count = 0;

public:
	uint32_t capture(uint64_t p_key, bool &r_new);
	uint32_t get_count() const { return count; }
	uint32_t get_id(uint64_t p_key) const;
};

struct NextGICaptureCandidate {
	uint32_t id = 0;
	float solid_angle = 0.0f;
	uint32_t texels = 0;
	bool first_residency = false;
};

// Highest solid angle first, first residency before a level change. Stops at the card or texel cap.
uint32_t nextgi_src_select_captures(const NextGICaptureCandidate *p_items, uint32_t p_count, uint32_t *r_accepted, uint32_t p_card_cap, uint32_t p_texel_cap);

struct NextGIPageUse {
	uint32_t id = 0;
	uint64_t last_used = 0;
};

// Oldest last_used first. Writes up to p_need ids.
uint32_t nextgi_src_lru_evict(const NextGIPageUse *p_pages, uint32_t p_count, uint32_t p_need, uint32_t *r_evict);

// Quad buddy allocator. Blocks are squares, minimum 16, split into four. Free siblings merge.
class NextGIBuddyAllocator {
	uint32_t size = 4096;
	uint32_t min_block = NEXTGI_SRC_PAGE_MIN;
	static constexpr uint32_t LEVELS = 16;
	LocalVector<Rect2i> free_lists[LEVELS];

	uint32_t _level(uint32_t p_side) const;
	uint32_t _round(uint32_t p_side) const;
	bool _ensure(uint32_t p_side);
	void _merge(const Rect2i &p_block);

public:
	explicit NextGIBuddyAllocator(uint32_t p_size = 4096, uint32_t p_min = NEXTGI_SRC_PAGE_MIN);
	bool allocate(uint32_t p_width, uint32_t p_height, Rect2i &r_rect);
	bool free_block(const Rect2i &p_rect);
	void reset();
};

// The SRC card sets and material atlas, owned by NextGI and created by the frame block's first run
// under the HWRT tier. Card sets are per mesh in P1a, captured with one representative instance
// (the shared (mesh, override set) atlas and the per-instance LRU are P1b, #41).
class NextGISRC {
public:
	enum AtlasTarget {
		ATLAS_ALBEDO,
		ATLAS_NORMAL,
		ATLAS_ORM,
		ATLAS_EMISSION,
		ATLAS_DEPTH,
		ATLAS_TARGET_MAX,
	};

	struct Stats {
		uint32_t card_set_count = 0;
		uint32_t card_count = 0;
		uint32_t captured_card_count = 0;
		uint64_t capture_count = 0; // _render_material calls; equals card_count when each card is captured once.
		uint32_t max_captures_per_card = 0;
		uint64_t capture_frames = 0; // Frames with at least one capture.
		uint64_t shadow_batch_refusals = 0; // Capture requests inside a shadow batch; must stay 0.
		uint32_t failed_mesh_count = 0; // Meshes with no cards (no triangles) or no atlas space.
		uint32_t capture_draw_lists = 0; // Last frame. One batched list, or 0 when nothing was queued.
		uint32_t capture_texels = 0;
		float capture_cpu_ms = 0.0f;
		uint32_t material_capture_count = 0; // Distinct (mesh, material) keys.
	};

private:
	static NextGISRC *singleton;

	RID atlas[ATLAS_TARGET_MAX];
	RID atlas_fb_depth;
	RID atlas_fb;
	uint64_t atlas_memory = 0;

	RID card_buffer;
	RID card_set_buffer;
	uint32_t card_capacity = 0;
	uint32_t card_set_capacity = 0;
	bool gpu_dirty = false;

	struct CardSetState {
		RID mesh;
		uint32_t captured = 0;
		bool released = false;
	};

	NextGIShelfPacker packer;
	LocalVector<NextGICardDesc> card_descs;
	LocalVector<NextGICard> cards;
	LocalVector<uint32_t> card_capture_counts;
	LocalVector<NextGICardSet> card_sets;
	LocalVector<CardSetState> card_set_states;
	HashMap<RID, uint32_t> mesh_card_sets;
	HashMap<uint64_t, uint32_t> key_card_sets;
	HashSet<RID> failed_meshes;
	Stats stats;

	enum DumpState {
		DUMP_OFF,
		DUMP_WAITING, // For every known card to be captured.
		DUMP_REQUESTED, // Downloads in flight.
		DUMP_DONE,
	};
	DumpState dump_state = DUMP_OFF;
	uint32_t dump_pending = 0;
	LocalVector<float> card_coverage; // Per card, filled by the depth download.

	void _create_atlas();
	bool _build_card_set(RID p_mesh, uint64_t p_key);
	void _upload_gpu_data();
	void _request_dump();
	void _on_dump_data(const Vector<uint8_t> &p_data, int p_target);
	static void _dump_data_received(const Vector<uint8_t> &p_data, int p_target);

public:
	static NextGISRC *get_singleton() { return singleton; }
	// The card set of a mesh for NextGIInstanceRecord::card_set; UINT32_MAX when it has none yet.
	static uint32_t get_mesh_card_set(RID p_mesh);
	// The card set for this instance's mesh and materials. UINT32_MAX until that key is captured.
	static uint32_t get_instance_card_set(RenderGeometryInstance *p_instance);

	static String get_atlas_target_name(AtlasTarget p_target);

	void init();
	void free();

	// Line 2a. Builds card sets for meshes in the v0 RT feed and captures every uncaptured card,
	// each exactly once. Refuses to run inside a shadow batch, which shares RENDER_LIST_SECONDARY.
	void capture(RenderDataRD *p_render_data, bool p_shadow_batch_open);

	const Stats &get_stats() const { return stats; }
	uint64_t get_atlas_memory() const { return atlas_memory; }
	uint32_t get_card_count() const { return cards.size(); }
	uint32_t get_card_set_count() const { return card_sets.size(); }
	bool copy_card(uint32_t p_index, NextGICard &r_card) const {
		if (p_index >= cards.size()) {
			return false;
		}
		r_card = cards[p_index];
		return true;
	}
	bool copy_card_set(uint32_t p_index, NextGICardSet &r_set) const {
		if (p_index >= card_sets.size()) {
			return false;
		}
		r_set = card_sets[p_index];
		return true;
	}
	RID get_atlas_texture(AtlasTarget p_target) const;
	RID get_card_buffer() const { return card_buffer; }
	RID get_card_set_buffer() const { return card_set_buffer; }
	// Adds the src_* counters to nextgi_get_tier_info().
	void fill_info(Dictionary &r_info) const;

	NextGISRC();
	~NextGISRC();
};

} // namespace RendererRD
