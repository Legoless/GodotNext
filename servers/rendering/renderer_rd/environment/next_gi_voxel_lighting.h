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

class RenderDataRD;
class RenderGeometryInstance;

namespace RendererRD {
class NextGISRC;
class NextGIRelight;
class NextGIRTScene;
} // namespace RendererRD

// GodotNext: ps_voxel_lighting (#188, plan §21.2 L5, gate GS-1)
//
// Global-SDF hits have no instance id, so they cannot pick an SRC card. The
// SRC is resampled into four world clipmaps, each voxel twice the matching
// global-SDF voxel. A voxel stores radiance for six axis directions plus a
// coverage weight. Direction d stores the surface found by tracing from the
// voxel along +d, so that surface faces -d. Lookup reads the opposite slot:
// the face whose stored surface faces the query. Only geometry dirty bricks
// are re-traced. A lookup weight below 0.5 falls back to the WRC.
//
// The GPU volume is radiance plus weights. There is no visibility buffer.
// Shading resolves instance -> page_table + card -> relight page rects.
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
	static constexpr uint32_t TRACE_REACH_VOXELS = 4;
	static constexpr float WEIGHT_FALLBACK = 0.5f;
	static constexpr float CARD_DEPTH_TEXELS = 2.0f;
	static constexpr uint32_t SDF_BRICK_VOXELS = 4;
	static constexpr uint32_t PAGE_EVICTED_BIT = 0x80000000u;
	// Shared with nextgi_voxel_lighting_inc.glsl. Stored exponent 30 is 32768, not overflow.
	static constexpr int R11G11B10_BIAS = 15;
	static constexpr int R11G11B10_MAX_STORED = 30;
	static constexpr float R11G11B10_MAX_11 = 65024.0f;

	// Bytes passed to storage_buffer_create. Visibility is not allocated.
	static constexpr uint64_t radiance_buffer_bytes() { return uint64_t(VOXEL_COUNT) * DIRECTIONS * 4u; }
	static constexpr uint64_t weight_buffer_bytes() { return uint64_t(VOXEL_COUNT) * DIRECTIONS; }
	static constexpr uint64_t staging_buffer_bytes() { return 2ull * 1024ull * 1024ull; }
	static constexpr uint64_t visibility_buffer_bytes() { return 0; }
	static constexpr uint64_t allocated_volume_bytes() {
		return radiance_buffer_bytes() + weight_buffer_bytes() + staging_buffer_bytes() + visibility_buffer_bytes();
	}

	static float level_voxel_size(uint32_t p_level);
	static Vector3 voxel_center(uint32_t p_level, const Vector3i &p_sdf_origin, const Vector3i &p_voxel);
	static uint32_t voxel_index(const Vector3i &p_voxel);
	static Vector3i voxel_coord(uint32_t p_index);
	static uint32_t direction_index(uint32_t p_level, uint32_t p_voxel, uint32_t p_direction);
	static Vector3 direction_vector(uint32_t p_direction);
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

	static float shade_weight(bool p_missing, float p_projection_cosine, float p_depth_error_texels);
	// taps are +X, -X, +Y, -Y, +Z, -Z. The result is the central difference.
	static Vector3 gradient_from_taps(const float p_taps[6], float p_step);

	struct CardQuery {
		Vector3 origin;
		Vector3 axis_u;
		Vector3 axis_v;
		Vector3 axis_n;
		float half_u = 0.0f;
		float half_v = 0.0f;
		float texel_size = 0.0f;
	};
	struct CardChoice {
		uint32_t card = NO_CARD;
		float u = 0.0f; // 0..1 across the card.
		float v = 0.0f;
		float projection = 0.0f;
		float depth_error = 0.0f;
		bool missing = true;
	};
	// D6: every card, containment, best alignment, depth within 2 texels of the card plane.
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

	struct TraceInstance {
		uint32_t id = 0;
		Transform3D transform;
		Transform3D inverse;
		float world_scale = 1.0f;
		const MeshSDFData *field = nullptr;
		AABB world_aabb;
		CardQuery cards[MAX_CARDS];
		uint32_t card_count = 0;
		uint32_t page_table = EMPTY_VISIBILITY;
		Color debug_radiance;
	};
	struct AxisHit {
		bool hit = false;
		uint32_t packed = EMPTY_VISIBILITY;
		uint32_t instance = 0;
		uint32_t card = NO_CARD;
		uint32_t hit_t = 0;
		float distance = 0.0f;
		Vector3 position;
		Vector3 normal;
		float projection = 0.0f;
		float depth_error = 0.0f;
		float card_u = 0.0f;
		float card_v = 0.0f;
		uint32_t missing = 1;
	};
	static AxisHit trace_axis(const TraceInstance *p_instances, uint32_t p_count, const Vector3 &p_origin, const Vector3 &p_direction, float p_max_distance);

	// std140-sized clipmap record a consumer can upload. origin xyz is the
	// global-SDF origin; origin.w is the lighting voxel size as float bits.
	struct ClipmapLevelGPU {
		int32_t origin_x = 0;
		int32_t origin_y = 0;
		int32_t origin_z = 0;
		uint32_t voxel_size_bits = 0;
	};
	static_assert(sizeof(ClipmapLevelGPU) == 16, "Clipmap level must stay one ivec4.");

	NextGIVoxelLighting();
	~NextGIVoxelLighting();
	NextGIVoxelLighting(const NextGIVoxelLighting &) = delete;
	NextGIVoxelLighting &operator=(const NextGIVoxelLighting &) = delete;

	void set_clipmap_origins(const Vector3i *p_origins, uint32_t p_count);
	void set_clipmap_origins_from_camera(const Vector3 &p_camera);
	// Geometry dirty volumes only. Light volumes are not an input.
	void enqueue_dirty_volumes(const AABB *p_volumes, uint32_t p_count);
	uint32_t pending_bricks() const { return queue.size(); }
	uint32_t pending_bricks_at_level(uint32_t p_level) const;
	// Traces a budget of axis rays. Returns the rays traced this call.
	uint32_t retrace(const TraceInstance *p_instances, uint32_t p_count);
	// Re-resolves every stored hit against p_instances and compacts them all.
	void shade(const TraceInstance *p_instances, uint32_t p_count);
	// Reads the CPU mirror. Same direction rule and weight fallback as the GLSL include.
	LookupResult lookup_at(const Vector3 &p_world, const Vector3 &p_gradient, const Color &p_wrc) const;

	uint32_t get_retrace_rays() const { return retrace_rays; }
	uint32_t get_shaded_hits() const { return shaded_hits; }
	uint32_t get_covered_hits() const { return covered_hits; }
	uint32_t get_stored_hits() const { return hits.size(); }
	uint32_t get_resampled_nonzero() const { return resampled_nonzero; }
	float get_coverage() const;
	int64_t get_coverage_ppm() const;
	uint64_t get_allocated_bytes() const { return allocated_bytes; }
	void copy_clipmap(ClipmapLevelGPU *r_levels, uint32_t p_count) const;
	RID get_radiance_buffer() const;
	RID get_weight_buffer() const;

	// Line 2b, and the software-tier hook. p_rt_scene is null off HWRT.
	void update_render(RenderDataRD *p_render_data, RendererRD::NextGIRTScene *p_rt_scene, RendererRD::NextGIRelight *p_relight, RendererRD::NextGISRC *p_src);
	void reset();

private:
	struct StoredHit {
		uint32_t packed = EMPTY_VISIBILITY;
		uint32_t instance = 0;
		uint32_t card = NO_CARD;
		uint32_t page_table = EMPTY_VISIBILITY;
		float card_u = 0.0f;
		float card_v = 0.0f;
		float weight = 0.0f;
		float projection = 0.0f;
		float depth_error = 0.0f;
		uint32_t missing = 1;
		Vector3 position;
		Vector3 normal;
		Color cpu_radiance;
	};
	struct DirtyBrick {
		uint32_t level = 0;
		Vector3i brick;
	};
	struct GPUState;
	struct RuntimeRef {
		uint32_t id = 0;
		Transform3D transform;
		Transform3D inverse;
		float world_scale = 1.0f;
		AABB world_aabb;
		RID mesh;
		RenderGeometryInstance *geometry = nullptr;
		uint32_t element = UINT32_MAX;
	};

	static uint32_t _brick_key(uint32_t p_level, const Vector3i &p_brick);
	static Vector3i _lighting_origin(const Vector3i &p_sdf_origin);
	AABB _brick_aabb(uint32_t p_level, const Vector3i &p_brick) const;
	Vector3i _physical_voxel(uint32_t p_level, const Vector3i &p_local) const;
	uint32_t _stable_id(RenderGeometryInstance *p_geometry, uint32_t p_element);
	void _fill_cards(RendererRD::NextGISRC *p_src, RenderGeometryInstance *p_geometry, const RID &p_mesh, const Transform3D &p_transform, float p_world_scale, TraceInstance &r_trace) const;
	const MeshSDFData *_cached_field(const RID &p_mesh);
	void _enqueue_aabb(const AABB &p_aabb);
	void _enqueue_brick(uint32_t p_level, const Vector3i &p_brick);
	void _scroll_level(uint32_t p_level, const Vector3i &p_new_origin);
	void _drop_level(uint32_t p_level);
	void _clear_voxel(uint32_t p_level, const Vector3i &p_physical);
	void _note_voxel(uint32_t p_voxel_key, bool p_any_hit);
	void _trace_brick(const DirtyBrick &p_brick, const TraceInstance *p_instances, uint32_t p_count, uint32_t &r_overlapping);
	void _rebuild_queue_index();
	void _apply_stored_hit(StoredHit &r_hit, const TraceInstance *p_instance);
	void _free_gpu();
	static void _uniform_set_invalidated(void *p_userdata);
	bool _dispatch_gpu(const RID &p_radiance_atlas, RID p_page_rects, uint32_t p_page_rect_count);
	bool _upload_and_dispatch(uint32_t p_mode, const uint32_t *p_words, uint32_t p_count);

	HashMap<uint32_t, StoredHit> hits;
	LocalVector<DirtyBrick> queue;
	HashSet<uint32_t> queued;
	Vector3i origins[LEVELS];
	bool origin_set[LEVELS] = {};
	uint32_t retrace_rays = 0;
	uint32_t shaded_hits = 0;
	uint32_t covered_hits = 0;
	uint32_t resampled_nonzero = 0;
	// Production shade rejects page indices past this count. Unit tests leave it off.
	uint32_t shade_page_rect_count = 0;
	bool shade_reject_pages = false;
	uint64_t allocated_bytes = 0;
	GPUState *gpu = nullptr;
	HashMap<RID, MeshSDFData> field_cache;
	uint64_t field_generation = UINT64_MAX;
	HashMap<uint64_t, uint32_t> stable_ids;
	uint32_t next_stable_id = 1;

	LocalVector<uint32_t> gpu_clear_voxels;
	LocalVector<uint32_t> gpu_shade_words;
	HashSet<uint32_t> shaded_voxels;
};
