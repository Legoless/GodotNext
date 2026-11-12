/**************************************************************************/
/*  next_gi_voxel_lighting.h                                              */
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
#include "core/math/color.h"
#include "core/math/mesh_sdf_builder.h"
#include "core/math/transform_3d.h"
#include "core/math/vector3.h"
#include "core/math/vector3i.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/variant/dictionary.h"

class RenderDataRD;
class RenderGeometryInstance;

namespace RendererRD {
class NextGISRC;
class NextGIRelight;
} // namespace RendererRD

// GodotNext: ps_voxel_lighting (#188, plan §21.2 L5, gate GS-1)
//
// Global-SDF hits have no instance id, so they cannot pick an SRC card. The
// SRC is resampled into four world clipmaps, each voxel twice the matching
// global-SDF voxel. A voxel stores radiance for six axis directions plus a
// coverage weight. Slot d holds the nearest surface on the voxel's axis line
// that faces -d: an entry found tracing along +d from outside, or, for a
// voxel center inside a solid, the exit found tracing along -d when the
// solid's exit along +d is not nearer. Lookups read a voxel from both sides of
// its center, so a voxel inside a wall serves only the face it is nearer to on
// each axis; the far face's slot stays empty. A surface the
// center touches goes only to the slots of the axes it faces. A center just
// inside a solid (within eps of its surface, as in any thin wall) takes no
// surface found past the solid along the axes that surface faces. Lookup reads
// the slot whose stored surface faces the query, then rejects that slot when
// the sample lies behind the stored surface. The surface is a plane through
// the hit, perpendicular to the slot axis, so a wall of 10 cm or more does not
// leak a face into the room on its far side (plan §21.2 L5, §21.2.6). Only
// geometry dirty bricks are re-traced, plus every instance's bricks when a
// level is created and an instance's bricks when its mesh SDF arrives. Dirty
// bricks go before those fill bricks. A new or refilled volume traces its fill
// bricks nearest the camera first, then outward, still within the per-frame
// ray budget. A lookup weight below 0.5 falls back to the WRC.
//
// Hits are stored in instance-local space. Every frame the GPU resamples the
// SRC with the D6 rule: the best-aligned card whose captured material depth
// matches the hit within 2 card texels. The GPU volume is radiance plus
// weights. Each radiance word keeps a 4-bit signed offset of its surface along
// the slot axis (in voxel units, clamped to [-1, 1]) in its top bits, and the
// color in the low 28 bits, so the volume stays within 32 MiB. There is no
// visibility buffer: the per-voxel hits live on the CPU.
class NextGIVoxelLighting {
public:
	static constexpr uint32_t LEVELS = 4;
	static constexpr uint32_t LEVEL_SIZE = 64;
	static constexpr uint32_t DIRECTIONS = 6;
	static constexpr uint32_t VOXELS_PER_LEVEL = LEVEL_SIZE * LEVEL_SIZE * LEVEL_SIZE;
	static constexpr uint32_t VOXEL_COUNT = LEVELS * VOXELS_PER_LEVEL;
	static constexpr uint32_t MAX_CARDS = 12;
	static constexpr uint32_t NO_CARD = 15;
	static constexpr uint32_t EMPTY_VISIBILITY = 0xFFFFFFFFu;
	static constexpr uint32_t MAX_HIT_T = 254;
	static constexpr uint32_t RAYS_PER_FRAME = 1536;
	static constexpr uint32_t MAX_TRACE_INSTANCES = 32;
	static constexpr uint32_t MAX_MARCH_STEPS = 16;
	static constexpr uint32_t REFINE_STEPS = 10;
	static constexpr uint32_t TRACE_REACH_VOXELS = 4;
	static constexpr float WEIGHT_FALLBACK = 0.5f;
	static constexpr float CARD_DEPTH_TEXELS = 2.0f;
	// A surface the voxel center touches, or that a ray only grazes, goes to slot d only when its
	// normal faces -d by at least this cosine. A surface running along an axis is in no slot of it.
	static constexpr float SLOT_FACING_MIN = 0.25f;
	static constexpr uint32_t SDF_BRICK_VOXELS = 4;
	static constexpr uint32_t PAGE_EVICTED_BIT = 0x80000000u;
	// Debug readback: lookups probed at stored hits, at most this many per frame.
	static constexpr uint32_t MAX_DEBUG_PROBES = 4096;
	// Shared with nextgi_voxel_lighting_inc.glsl. Stored exponent 30 is 32768, not overflow.
	static constexpr int R11G11B10_BIAS = 15;
	static constexpr int R11G11B10_MAX_STORED = 30;
	static constexpr float R11G11B10_MAX_11 = 65024.0f;
	// Top bits of a stored radiance word. The offset is in voxel units along the
	// surface normal (-direction), clamped to [-1, 1]. One voxel of reach is all
	// a trilinear lookup can see, so a farther surface clamps to the same side.
	static constexpr uint32_t SIDE_OFFSET_BITS = 4;
	static constexpr uint32_t SIDE_OFFSET_MAX = (1u << SIDE_OFFSET_BITS) - 1u;
	static constexpr uint32_t SIDE_OFFSET_SHIFT = 28;
	// Accept a sample this far behind the quantized plane. Shared with the GLSL lookup.
	static constexpr float SIDE_ACCEPT_EPSILON = 1.0e-4f;

	// Bytes passed to storage_buffer_create. Visibility is not allocated.
	static constexpr uint64_t radiance_buffer_bytes() { return uint64_t(VOXEL_COUNT) * DIRECTIONS * 4u; }
	static constexpr uint64_t weight_buffer_bytes() { return uint64_t(VOXEL_COUNT) * DIRECTIONS; }
	static constexpr uint64_t staging_buffer_bytes() { return 2ull * 1024ull * 1024ull; }
	static constexpr uint64_t visibility_buffer_bytes() { return 0; }
	static constexpr uint64_t allocated_volume_bytes() {
		return radiance_buffer_bytes() + weight_buffer_bytes() + staging_buffer_bytes() + visibility_buffer_bytes();
	}
	// Created only while the debug readback setting is on; not part of the volume.
	static constexpr uint64_t debug_buffer_bytes() { return 16u + uint64_t(MAX_DEBUG_PROBES) * 16u; }

	static float level_voxel_size(uint32_t p_level);
	static Vector3 voxel_center(uint32_t p_level, const Vector3i &p_sdf_origin, const Vector3i &p_voxel);
	static uint32_t voxel_index(const Vector3i &p_voxel);
	static Vector3i voxel_coord(uint32_t p_index);
	static uint32_t direction_index(uint32_t p_level, uint32_t p_voxel, uint32_t p_direction);
	static Vector3 direction_vector(uint32_t p_direction);
	static uint32_t opposite_direction(uint32_t p_direction) { return (p_direction + 3u) % DIRECTIONS; }
	static int wrap_coord(int p_value);
	// Lighting voxels covered by one global-SDF brick, clipped to the volume.
	static uint32_t voxels_for_sdf_brick(const Vector3i &p_brick, Vector3i *r_voxels, uint32_t p_max);
	// Slot whose stored surface faces +axis when p_positive is true.
	static uint32_t facing_direction(uint32_t p_axis, bool p_positive);

	static uint32_t pack_visibility(uint32_t p_instance, uint32_t p_card, uint32_t p_hit_t);
	static void unpack_visibility(uint32_t p_packed, uint32_t &r_instance, uint32_t &r_card, uint32_t &r_hit_t);
	static uint32_t atomic_min_visibility(uint32_t p_current, uint32_t p_candidate);
	static uint32_t quantize_hit_t(float p_distance, float p_max_distance);

	static uint32_t pack_r11g11b10(const Color &p_color);
	static Color unpack_r11g11b10(uint32_t p_packed);
	// Signed axis offset, in voxel units, rounded toward -1 so the stored plane
	// is never in front of the true surface. Code 0 is -1, code 15 is +1.
	static uint32_t pack_side_offset(float p_voxels);
	static float unpack_side_offset(uint32_t p_code);
	// Radiance word: reduced positive floats in bits 0-27 (R and G with 5
	// mantissa bits, B with 3) and the side-offset code in bits 28-31.
	static uint32_t pack_slot_word(const Color &p_color, uint32_t p_side_code);
	static void unpack_slot_word(uint32_t p_packed, Color &r_color, float &r_side_offset);
	// True when p_sample is on the front of the plane, or on it. The plane sits
	// p_offset_voxels along p_face_normal from p_center. p_face_normal is -direction.
	static bool accepts_surface(const Vector3 &p_sample, const Vector3 &p_center, const Vector3 &p_face_normal, float p_offset_voxels, float p_voxel_size);
	// Unit normal to two unorm16 octahedral components, as the GPU decodes it.
	static uint32_t pack_normal(const Vector3 &p_normal);
	static Vector3 unpack_normal(uint32_t p_packed);

	static float shade_weight(bool p_missing, float p_projection_cosine, float p_depth_error_texels);
	// taps are +X, -X, +Y, -Y, +Z, -Z. The result is the central difference.
	static Vector3 gradient_from_taps(const float p_taps[6], float p_step);

	// A card in the card's own (instance-local) space, as NextGICard stores it.
	struct CardQuery {
		Vector3 origin;
		Vector3 axis_u;
		Vector3 axis_v;
		Vector3 axis_n;
		float half_u = 0.0f;
		float half_v = 0.0f;
		float texel_size = 0.0f;
		// Distance from the card plane to one texel past the mesh's far side.
		float depth_range = 0.0f;
		// Captured material depth per card texel, row 0 at v = -half_v; 0 is uncovered.
		const float *depth = nullptr;
		uint32_t depth_width = 0;
		uint32_t depth_height = 0;
	};
	struct CardChoice {
		uint32_t card = NO_CARD;
		float u = 0.0f; // 0..1 across the card.
		float v = 0.0f;
		float projection = 0.0f;
		float depth_error = 0.0f;
		bool missing = true;
	};
	// D6, the CPU twin of the GPU shade: every card, containment, then the best-aligned card
	// whose captured depth matches the hit within 2 texels. Without captured depth the card's
	// capture volume (0..depth_range, +-2 texels) stands in. Position and normal are in the
	// cards' space.
	static CardChoice choose_card(const CardQuery *p_cards, uint32_t p_count, const Vector3 &p_position, const Vector3 &p_normal);

	struct DirectionSample {
		Color radiance;
		float weight = 0.0f;
	};
	struct LookupResult {
		Color radiance;
		float weight = 0.0f;
		bool used_wrc = true;
	};
	// Three axis directions whose stored surfaces face the gradient, blended by |normal|.
	static LookupResult lookup(const Vector3 &p_gradient, const DirectionSample p_samples[DIRECTIONS], const Color &p_wrc);
	// lookup() after the side test. p_offsets[d] is the stored offset in voxel units
	// along -direction(d). A rejected slot adds nothing, and the weight is the blend
	// of the slots that remain. Below 0.5 the result is p_wrc.
	static LookupResult lookup_accepted(const Vector3 &p_sample, const Vector3 &p_center, float p_voxel_size, const Vector3 &p_gradient, const DirectionSample p_samples[DIRECTIONS], const float p_offsets[DIRECTIONS], const Color &p_wrc);

	struct TraceInstance {
		uint32_t id = 0;
		Transform3D transform;
		Transform3D inverse;
		const MeshSDFData *field = nullptr;
		AABB world_aabb;
		uint32_t page_table = EMPTY_VISIBILITY;
		uint32_t card_set = UINT32_MAX;
	};
	struct AxisHit {
		bool hit = false;
		// The surface was left, not entered: the voxel center is inside that solid.
		bool exit = false;
		uint32_t packed = EMPTY_VISIBILITY;
		uint32_t instance = 0;
		uint32_t hit_t = 0;
		float distance = 0.0f;
		Vector3 position; // World.
		Vector3 normal; // World, outward.
		Vector3 object_position; // Instance-local, the space of the cards.
		Vector3 object_normal;
	};
	// Traces the six axis rays of one voxel. r_slots[d] receives the nearest surface facing -d
	// (see the class comment). Returns the number of filled slots.
	static uint32_t trace_voxel(const TraceInstance *p_instances, uint32_t p_count, const Vector3 &p_center, float p_max_distance, AxisHit r_slots[DIRECTIONS]);

	// std140-sized clipmap record a consumer can upload. origin xyz is the
	// global-SDF origin; origin.w is the lighting voxel size as float bits.
	struct ClipmapLevelGPU {
		int32_t origin_x = 0;
		int32_t origin_y = 0;
		int32_t origin_z = 0;
		uint32_t voxel_size_bits = 0;
	};
	static_assert(sizeof(ClipmapLevelGPU) == 16, "Clipmap level must stay one ivec4.");

	// One instance in the render feed, for filling new levels and late mesh SDFs.
	struct InstanceBounds {
		uint64_t mesh = 0;
		AABB world_aabb;
		const void *field = nullptr; // Identity of the mesh SDF; null while it is not built.
	};

	NextGIVoxelLighting();
	~NextGIVoxelLighting();
	NextGIVoxelLighting(const NextGIVoxelLighting &) = delete;
	NextGIVoxelLighting &operator=(const NextGIVoxelLighting &) = delete;

	void set_clipmap_origins(const Vector3i *p_origins, uint32_t p_count);
	void set_clipmap_origins_from_camera(const Vector3 &p_camera);
	// Geometry dirty volumes only. Light volumes are not an input.
	void enqueue_dirty_volumes(const AABB *p_volumes, uint32_t p_count);
	// Levels created or dropped since the last refresh, one bit per level.
	uint32_t get_levels_to_fill() const { return levels_to_fill; }
	// Queues the bricks of every instance for the levels to fill, and on every level the
	// bricks of instances whose mesh SDF arrived or changed since the last call. They queue as
	// fill bricks, behind every dirty or scrolled-in brick, nearest the camera first.
	void refresh_instances(const InstanceBounds *p_instances, uint32_t p_count);
	// update_render calls this once a frame with the mesh-SDF atlas generation, which changes
	// when a mesh SDF is built, rebuilt or freed (a late SDF has no registry event), and passes
	// the feed to refresh_instances when it returns true: a level waits for its fill or the
	// generation changed. A changed generation also drops the cached fields.
	bool begin_frame(uint64_t p_field_generation);
	uint32_t pending_bricks() const { return queue.size(); }
	uint32_t pending_bricks_at_level(uint32_t p_level) const;
	// The brick retrace will take at p_index. False when p_index is past the queue.
	bool get_pending_brick(uint32_t p_index, uint32_t &r_level, Vector3i &r_brick, bool &r_fill) const;
	// Squared distance from the camera to the closest point of that brick's voxels.
	// Negative when p_index is past the queue. The trace-reach margin is not part of it.
	float get_pending_brick_distance_sq(uint32_t p_index) const;
	// Traces a budget of axis rays. Returns the rays traced this call.
	uint32_t retrace(const TraceInstance *p_instances, uint32_t p_count);
	// Packs every stored hit for the GPU shade.
	void shade(const TraceInstance *p_instances, uint32_t p_count);
	// The hit stored in one slot of a voxel given in level-local coordinates.
	bool get_stored_hit(uint32_t p_level, const Vector3i &p_local, uint32_t p_direction, AxisHit &r_hit) const;
	// Keys of the stored hits the debug readback probes: at most p_max, all slots of a voxel or
	// none, so each direction keeps its share of the probes.
	static void select_debug_probes(const HashMap<uint32_t, AxisHit> &p_hits, uint32_t p_max, LocalVector<uint32_t> &r_keys);
	// Ids of the instances _trace_brick keeps for a brick: the nearest MAX_TRACE_INSTANCES.
	void select_brick_instances(uint32_t p_level, const Vector3i &p_brick, const TraceInstance *p_instances, uint32_t p_count, LocalVector<uint32_t> &r_indices, uint32_t &r_overlapping) const;

	uint32_t get_retrace_rays() const { return retrace_rays; }
	uint32_t get_shaded_hits() const { return shaded_hits; }
	// Shaded hits whose instance has no card set or no resident relight pages: weight 0.
	uint32_t get_unpaged_hits() const { return unpaged_hits; }
	uint32_t get_stored_hits() const { return hits.size(); }
	// GPU counters. -1 unless debug/voxel_lighting_readback read them this frame.
	int64_t get_covered_hits() const { return covered_hits; }
	int64_t get_resampled_nonzero() const { return resampled_nonzero; }
	float get_coverage() const;
	int64_t get_coverage_ppm() const;
	uint64_t get_debug_readbacks() const { return debug_readbacks; }
	uint64_t get_allocated_bytes() const { return allocated_bytes; }
	void copy_clipmap(ClipmapLevelGPU *r_levels, uint32_t p_count) const;
	RID get_radiance_buffer() const;
	RID get_weight_buffer() const;
	// Adds the voxel_lighting_* entries to nextgi_get_tier_info().
	void fill_info(Dictionary &r_info) const;

	// Line 2b, after the SRC relight, on both tiers.
	void update_render(RenderDataRD *p_render_data, RendererRD::NextGIRelight *p_relight, RendererRD::NextGISRC *p_src);
	void reset();

private:
	struct DirtyBrick {
		uint32_t level = 0;
		Vector3i brick;
		// Queued by refresh_instances. Fill bricks wait behind dirty and scrolled-in bricks.
		bool fill = false;
	};
	struct GPUState;
	struct RuntimeRef {
		uint32_t id = 0;
		Transform3D transform;
		Transform3D inverse;
		AABB world_aabb;
		RID mesh;
		const void *field = nullptr;
		RenderGeometryInstance *geometry = nullptr;
		uint32_t element = UINT32_MAX;
	};
	struct ShadeBindings {
		RID radiance_atlas;
		RID page_rects;
		uint32_t page_rect_count = 0;
		RID material_depth;
		RID cards;
		RID card_sets;
		uint32_t card_count = 0;
		uint32_t card_set_count = 0;
	};

	static uint32_t _brick_key(uint32_t p_level, const Vector3i &p_brick);
	static Vector3i _lighting_origin(const Vector3i &p_sdf_origin);
	AABB _brick_aabb(uint32_t p_level, const Vector3i &p_brick) const;
	Vector3i _physical_voxel(uint32_t p_level, const Vector3i &p_local) const;
	uint32_t _stable_id(RenderGeometryInstance *p_geometry, uint32_t p_element);
	const MeshSDFData *_cached_field(const RID &p_mesh);
	void _enqueue_aabb(const AABB &p_aabb, uint32_t p_level_mask = (1u << LEVELS) - 1u, bool p_fill = false);
	void _enqueue_brick(uint32_t p_level, const Vector3i &p_brick, bool p_fill = false);
	void _count_fill_begin();
	// Fill bricks only, nearest the camera first. Dirty bricks stay in front of them.
	void _order_fill_bricks();
	float _fill_distance_sq(const DirtyBrick &p_brick) const;
	void _scroll_level(uint32_t p_level, const Vector3i &p_new_origin);
	void _drop_level(uint32_t p_level);
	void _clear_voxel(uint32_t p_level, const Vector3i &p_physical);
	void _note_voxel(uint32_t p_voxel_key, bool p_any_hit);
	void _trace_brick(const DirtyBrick &p_brick, const TraceInstance *p_instances, uint32_t p_count, uint32_t &r_overlapping);
	void _rebuild_queue_index();
	void _free_gpu();
	static void _uniform_set_invalidated(void *p_userdata);
	bool _dispatch_gpu(const ShadeBindings &p_bindings, bool p_readback);
	bool _upload_and_dispatch(uint32_t p_mode, const uint32_t *p_words, uint32_t p_count, uint32_t p_stride, const ShadeBindings &p_bindings, uint32_t p_flags, uint32_t p_header_words = 0);
	void _collect_debug_probes();

	HashMap<uint32_t, AxisHit> hits;
	// Dirty and scrolled-in bricks first, in arrival order, then fill bricks from fill_begin on,
	// those nearest the camera first.
	LocalVector<DirtyBrick> queue;
	uint32_t fill_begin = 0;
	Vector3 camera_position;
	// Queued brick key to its fill flag.
	HashMap<uint32_t, bool> queued;
	Vector3i origins[LEVELS];
	bool origin_set[LEVELS] = {};
	uint32_t levels_to_fill = 0;
	HashMap<uint64_t, const void *> seen_fields;
	uint32_t retrace_rays = 0;
	uint32_t shaded_hits = 0;
	uint32_t unpaged_hits = 0;
	int64_t covered_hits = -1;
	int64_t gpu_shaded_hits = -1;
	int64_t resampled_nonzero = -1;
	uint64_t debug_readbacks = 0;
	uint64_t allocated_bytes = 0;
	GPUState *gpu = nullptr;
	HashMap<RID, MeshSDFData> field_cache;
	uint64_t field_generation = UINT64_MAX;
	HashMap<uint64_t, uint32_t> stable_ids;
	uint32_t next_stable_id = 1;

	LocalVector<uint32_t> gpu_clear_voxels;
	LocalVector<uint32_t> gpu_shade_words;
	// Debug readback only: lookups at a subset of the stored hits.
	LocalVector<Vector3> debug_probe_positions;
	LocalVector<Vector3> debug_probe_normals;
	LocalVector<Color> debug_probe_results; // rgb radiance, a = weight.
};
