/**************************************************************************/
/*  nextgeo_vsm.h                                                         */
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

#include "core/math/math_funcs.h"
#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "core/math/vector4.h"
#include "servers/rendering/renderer_rd/forward_clustered/nextgeo_sw_raster.h"
#include "servers/rendering/storage/virtual_mesh_storage.h"
#include "servers/rendering/storage/virtual_shadow_map_storage.h"

#include <cmath>
#include <cstddef>

// GodotNext: vsm_nanite (#217, plan §21.3 contract S4, GV-3, GV-6).
//
// CPU reference and shared GPU buffer layouts for VSM x Nanite: per-page multi-view cluster culling,
// page-mip error metric (<= 1 shadow texel), 1-level per-page max-depth Hi-Z (8x8 cells per 64x64
// physical page), hybrid HW/SW rasterization with 32-bit imageAtomicMax, static/dynamic layer hook
// ready for #212, and programmable raster-bin integration shared with #155.
// Shaders: cluster_cull.glsl (VSM_VIEW), vsm_raster_nextgeo.glsl, vsm_raster_sw.glsl,
// raster_material_{hw,sw,inc}.glsl (NEXTGEO_RASTER_DEPTH_ONLY).
namespace RendererRD {

// Provisional values (documented in docs/godotnext/source-log/vsm.md; measured and tuned in PQ).
static constexpr float NEXTGEO_VSM_DEFAULT_SW_MAX_EDGE_TEXELS = 8.0f;
static constexpr float NEXTGEO_VSM_MAX_SW_EDGE_TEXELS = float(NEXTGEO_SW_FULL_PRECISION_EXTENT_PX);
static constexpr float NEXTGEO_VSM_DEFAULT_MAX_TEXEL_ERROR = 1.0f;

// Per-page 1-level Hi-Z: one 8x8 max-depth reduction per 64x64 physical page (8x8 cells per page,
// 256 B per physical page = 2 MiB at High / 8192 pages). Each cell holds the minimum reversed-Z
// packed uint32 (farthest linear depth) over its 8x8 static-layer texel block; 0 means empty/invalid.
static constexpr uint32_t NEXTGEO_VSM_HIZ_BLOCK_SHIFT = 3u;
static constexpr uint32_t NEXTGEO_VSM_HIZ_BLOCK_SIZE = 1u << NEXTGEO_VSM_HIZ_BLOCK_SHIFT; // 8 texels.
static constexpr uint32_t NEXTGEO_VSM_HIZ_PAGE_SIZE = RendererVirtualShadowMapStorage::PAGE_SIZE >> NEXTGEO_VSM_HIZ_BLOCK_SHIFT; // 8 cells.
static_assert(NEXTGEO_VSM_HIZ_PAGE_SIZE == 8u, "VSM Hi-Z page grid must stay 8x8 (64 threads per page).");

// Per-chain (directional / local) GPU compaction capacities. Sized so real-world 10M+ triangle scenes
// (Intel Sponza, Bistro) warm-cache all initial dirty pages in a single frame without BVH node overflow
// triggering permanent VSM_ENTRY_SCROLLED redraw loops.
static constexpr uint32_t NEXTGEO_VSM_NODE_CAPACITY = 1u << 21; // 2,097,152 (record, node, dirty_idx) entries.
static constexpr uint32_t NEXTGEO_VSM_GROUP_CAPACITY = 1u << 20; // 1,048,576 (record, group, dirty_idx) entries.
static constexpr uint32_t NEXTGEO_VSM_CLUSTER_CAPACITY = 1u << 20; // 1,048,576 (cluster, page) pairs (< 2^21 for #155 bin packing).
static_assert(NEXTGEO_VSM_CLUSTER_CAPACITY <= (1u << 21), "VSM cluster capacity must fit in 21-bit raster-bin entry index.");

// Adaptive per-chain dirty-page slice bounds for Nanite VSM culling. Starts at MAX (4096 pages/frame)
// so non-overflowing frames rasterize every cleared dirty page immediately without leaving unshadowed
// tiles, halves from the active page count whenever any VSM queue overflows, holds while draining a
// dense burst cleanly, and ramps back up to MAX once the burst finishes. Prevents both post-clear
// unshadowed tile flashes and overflow livelock with zero extra VRAM.
static constexpr uint32_t NEXTGEO_VSM_DIRTY_PAGE_SLICE_MAX = 4096u;
static constexpr uint32_t NEXTGEO_VSM_DIRTY_PAGE_SLICE_MIN = 1u;

enum NextGeoVSMChain : uint32_t {
	NEXTGEO_VSM_CHAIN_DIRECTIONAL = 0u,
	NEXTGEO_VSM_CHAIN_LOCAL = 1u,
	NEXTGEO_VSM_CHAIN_COUNT = 2u,
};

enum NextGeoVSMCullFlags : uint32_t {
	NEXTGEO_VSM_CULL_FLAG_HIZ = 1u << 0, // Test cached pages against previous-frame static Hi-Z.
	NEXTGEO_VSM_CULL_FLAG_PROGRAMMABLE_BINS = 1u << 1, // Route programmable-bin surfaces via #155 bins.
	NEXTGEO_VSM_CULL_FLAG_DYNAMIC_HOOK = 1u << 2, // Step 6 hook (#212): route moving/WPO instances to layer 1.
	NEXTGEO_VSM_CULL_FLAG_COUNT_SW_WRITES = 1u << 3, // Accumulate SW texel writes for telemetry.
	NEXTGEO_VSM_CULL_FLAG_TESSELLATION = 1u << 4, // GodotNext: nextgeo_tessellation (#163): dice displaced clusters on this chain.
	NEXTGEO_VSM_CULL_FLAG_VOXELS = 1u << 5, // GodotNext: nextgeo_voxels (#165): swap coarse voxelized groups for sparse 4^3 bricks.
};

enum NextGeoVSMMapMetaFlags : uint32_t {
	NEXTGEO_VSM_MAP_META_REVERSE_CULL = 1u << 0,
	NEXTGEO_VSM_MAP_META_ACTIVE = 1u << 1,
};

struct NextGeoVSMMapMeta {
	uint32_t cull_mask = 0u;
	uint32_t flags = 0u;
	uint32_t pad0 = 0u;
	uint32_t pad1 = 0u;
};
static_assert(sizeof(NextGeoVSMMapMeta) == 16, "NextGeoVSMMapMeta must stay 16 bytes.");

struct NextGeoVSMParams {
	uint32_t table_maps = 0u;
	uint32_t pool_pages = 0u;
	uint32_t grid_width = 0u;
	uint32_t record_count = 0u;

	uint32_t dirty_begin = 0u;
	uint32_t dirty_end = 0u;
	uint32_t node_capacity = 0u;
	uint32_t group_capacity = 0u;

	uint32_t cluster_capacity = 0u;
	uint32_t cull_flags = 0u;
	uint32_t chain_index = 0u;
	uint32_t bucket_table_words = 0u;

	float max_texel_error = NEXTGEO_VSM_DEFAULT_MAX_TEXEL_ERROR;
	float lod_bias = 1.0f;
	float sw_max_edge_texels = NEXTGEO_VSM_DEFAULT_SW_MAX_EDGE_TEXELS;
	float dicing_rate = 2.0f; // GodotNext: nextgeo_tessellation (#163). Shadow texels per diced edge.

	float camera_position[4] = {};
	uint32_t bin_features[68] = {}; // Indexed by bin 0..64, padded to 16 bytes.
};
static_assert(sizeof(NextGeoVSMParams) == 352, "NextGeoVSMParams must match GLSL std430 layout (352 bytes).");

struct NextGeoVSMCounters {
	uint32_t dirty_pages = 0u; // 0
	uint32_t seeded_pairs = 0u; // 1
	uint32_t bvh_level_counts[8] = {}; // 2..9
	uint32_t group_count = 0u; // 10
	uint32_t cluster_count = 0u; // 11
	uint32_t sw_clusters = 0u; // 12
	uint32_t hw_clusters = 0u; // 13
	uint32_t prog_sw_clusters = 0u; // 14
	uint32_t prog_hw_clusters = 0u; // 15
	uint32_t hiz_culled_nodes = 0u; // 16
	uint32_t hiz_culled_clusters = 0u; // 17
	uint32_t overflow = 0u; // 18
	uint32_t redraw_marks = 0u; // 19
	uint32_t sw_writes = 0u; // 20
	uint32_t sink = 0u; // 21 (#277 D3D12 GBV atomic sink)
	uint32_t hiz_pages_built = 0u; // 22
	// GodotNext: vsm_caching (#212). Per-layer cluster counts and ALWAYS-mode layer-0 invariant counter.
	uint32_t static_clusters = 0u; // 23
	uint32_t dynamic_clusters = 0u; // 24
	uint32_t always_layer0_clusters = 0u; // 25
	// GodotNext: nextgeo_masked_two_sided (#153). Clusters demoted from programmable to FIXED bin past programmable_raster_distance.
	uint32_t distance_fixed_clusters = 0u; // 26
	// GodotNext: nextgeo_vertex_offset (#164). WPO clusters demoted to FIXED past vertex_offset_disable_distance and clamped vertices.
	uint32_t vertex_offset_distance_fixed = 0u; // 27
	uint32_t vertex_offset_clamped = 0u; // 28
	// GodotNext: nextgeo_voxels (#165). Voxel brick-page pairs emitted and SW texels written.
	uint32_t voxel_bricks = 0u; // 29
	uint32_t voxel_writes = 0u; // 30
	// Persistent adaptive dirty-page slice for this chain (updated in VSM_SETUP across frames).
	uint32_t dirty_page_slice = 0u; // 31
};
static_assert(sizeof(NextGeoVSMCounters) == 128, "NextGeoVSMCounters must match GLSL layout (128 bytes).");
static_assert(offsetof(NextGeoVSMCounters, cluster_count) == 11 * sizeof(uint32_t), "cluster_count word index must stay 11.");
static_assert(offsetof(NextGeoVSMCounters, sw_clusters) == 12 * sizeof(uint32_t), "sw_clusters word index must stay 12.");
static_assert(offsetof(NextGeoVSMCounters, sw_writes) == 20 * sizeof(uint32_t), "sw_writes word index must stay 20.");
static_assert(offsetof(NextGeoVSMCounters, sink) == 21 * sizeof(uint32_t), "sink word index must stay 21.");
static_assert(offsetof(NextGeoVSMCounters, hiz_pages_built) == 22 * sizeof(uint32_t), "hiz_pages_built word index must stay 22.");
static_assert(offsetof(NextGeoVSMCounters, vertex_offset_distance_fixed) == 27 * sizeof(uint32_t), "vertex_offset_distance_fixed word index must stay 27.");
static_assert(offsetof(NextGeoVSMCounters, vertex_offset_clamped) == 28 * sizeof(uint32_t), "vertex_offset_clamped word index must stay 28.");
static_assert(offsetof(NextGeoVSMCounters, voxel_bricks) == 29 * sizeof(uint32_t), "voxel_bricks word index must stay 29.");
static_assert(offsetof(NextGeoVSMCounters, voxel_writes) == 30 * sizeof(uint32_t), "voxel_writes word index must stay 30.");
static_assert(offsetof(NextGeoVSMCounters, dirty_page_slice) == 31 * sizeof(uint32_t), "dirty_page_slice word index must stay 31.");

static constexpr uint32_t NEXTGEO_VSM_COUNTER_CLUSTER_COUNT = 11u;
static constexpr uint32_t NEXTGEO_VSM_COUNTER_SW_CLUSTERS = 12u;
static constexpr uint32_t NEXTGEO_VSM_COUNTER_SW_WRITES = 20u;
static constexpr uint32_t NEXTGEO_VSM_COUNTER_SINK = 21u;
static constexpr uint32_t NEXTGEO_VSM_COUNTER_HIZ_PAGES_BUILT = 22u;
static constexpr uint32_t NEXTGEO_VSM_COUNTER_VOXEL_BRICKS = 29u;
static constexpr uint32_t NEXTGEO_VSM_COUNTER_VOXEL_WRITES = 30u;

// Matches cluster_cull.glsl VSM_SETUP adaptive dirty-page slice state machine.
inline uint32_t nextgeo_vsm_next_dirty_page_slice(uint32_t p_prev_slice, uint32_t p_prev_dirty_pages, uint32_t p_prev_overflow) {
	const uint32_t prev_slice = (p_prev_slice == 0u)
			? NEXTGEO_VSM_DIRTY_PAGE_SLICE_MAX
			: CLAMP(p_prev_slice, NEXTGEO_VSM_DIRTY_PAGE_SLICE_MIN, NEXTGEO_VSM_DIRTY_PAGE_SLICE_MAX);
	if (p_prev_overflow > 0u) {
		const uint32_t active_prev = MIN(prev_slice, MAX(p_prev_dirty_pages, NEXTGEO_VSM_DIRTY_PAGE_SLICE_MIN));
		return MAX(active_prev >> 1u, NEXTGEO_VSM_DIRTY_PAGE_SLICE_MIN);
	}
	if (p_prev_dirty_pages > prev_slice) {
		return prev_slice;
	}
	return MIN(prev_slice << 1u, NEXTGEO_VSM_DIRTY_PAGE_SLICE_MAX);
}

// Clamps the provisional SW/HW edge threshold to the software setup's full-precision extent.
inline float nextgeo_vsm_sw_switch_texels(float p_setting_texels) {
	if (!std::isfinite(p_setting_texels) || p_setting_texels <= 0.0f) {
		return 0.0f;
	}
	return MIN(p_setting_texels, NEXTGEO_VSM_MAX_SW_EDGE_TEXELS);
}

// Step 6: static (layer 0) vs dynamic (layer 1) layer routing hook ready for #212.
// Defaults every NextGeo instance to static (layer 0) when NEXTGEO_VSM_CULL_FLAG_DYNAMIC_HOOK is unset.
// When enabled, matches #218's runtime rule: an instance whose transform changed this frame
// (transform != prev_transform), is RT-skinned, or uses a WPO material routes to layer 1; stationary
// instances stay on static layer 0.
inline bool nextgeo_vsm_transform_moved(const float p_transform[3][4], const float p_prev_transform[3][4]) {
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 4; c++) {
			if (p_transform[r][c] != p_prev_transform[r][c]) {
				return true;
			}
		}
	}
	return false;
}

inline uint32_t nextgeo_vsm_instance_layer(uint32_t p_record_flags, bool p_moved_this_frame, uint32_t p_vsm_cull_flags, bool p_dynamic_material, bool p_has_foliage_wind = false) {
	if ((p_vsm_cull_flags & NEXTGEO_VSM_CULL_FLAG_DYNAMIC_HOOK) == 0u) {
		return 0u;
	}
	// GodotNext: vsm_caching (#212, plan §21.3 contract S5).
	const uint32_t mode = p_record_flags & RendererVirtualMeshStorage::RECORD_VSM_CACHE_MODE_MASK;
	if (mode == RendererVirtualMeshStorage::RECORD_VSM_CACHE_ALWAYS) {
		return 1u;
	}
	if (mode == RendererVirtualMeshStorage::RECORD_VSM_CACHE_STATIC || mode == RendererVirtualMeshStorage::RECORD_VSM_CACHE_RIGID) {
		return 0u;
	}
	const bool is_dynamic = ((p_record_flags & (RendererVirtualMeshStorage::RECORD_VSM_DYNAMIC | RendererVirtualMeshStorage::RECORD_RT_SKINNED | RendererVirtualMeshStorage::RECORD_HAS_SKELETON | RendererVirtualMeshStorage::RECORD_SKINNED_PALETTE)) != 0u) || p_has_foliage_wind || p_moved_this_frame || p_dynamic_material;
	return is_dynamic ? 1u : 0u;
}

inline Vector3 nextgeo_vsm_xform_point(const float p_transform[3][4], const Vector3 &p_local) {
	return Vector3(
			p_transform[0][0] * float(p_local.x) + p_transform[0][1] * float(p_local.y) + p_transform[0][2] * float(p_local.z) + p_transform[0][3],
			p_transform[1][0] * float(p_local.x) + p_transform[1][1] * float(p_local.y) + p_transform[1][2] * float(p_local.z) + p_transform[1][3],
			p_transform[2][0] * float(p_local.x) + p_transform[2][1] * float(p_local.y) + p_transform[2][2] * float(p_local.z) + p_transform[2][3]);
}

inline float nextgeo_vsm_instance_scale(const float p_transform[3][4]) {
	const Vector3 c0(p_transform[0][0], p_transform[1][0], p_transform[2][0]);
	const Vector3 c1(p_transform[0][1], p_transform[1][1], p_transform[2][1]);
	const Vector3 c2(p_transform[0][2], p_transform[1][2], p_transform[2][2]);
	const float d01 = Math::abs(float(c0.dot(c1)));
	const float d02 = Math::abs(float(c0.dot(c2)));
	const float d12 = Math::abs(float(c1.dot(c2)));
	return Math::sqrt(MAX(float(c0.dot(c0)) + d01 + d02, MAX(float(c1.dot(c1)) + d01 + d12, float(c2.dot(c2)) + d02 + d12)));
}

// Reconstructs the light origin of a perspective local face from MapData (matches vsm_map_view()).
inline Vector3 nextgeo_vsm_light_origin(const RendererVirtualShadowMapStorage::MapData &p_md) {
	const Vector3 r0(p_md.rows[0][0], p_md.rows[0][1], p_md.rows[0][2]);
	const Vector3 r1(p_md.rows[1][0], p_md.rows[1][1], p_md.rows[1][2]);
	const float len0 = float(r0.length());
	const float inv_cot = len0 > 1.0e-20f ? (1.0f / len0) : 0.0f;
	const Vector3 x_axis = r0 * inv_cot;
	const Vector3 y_axis = r1 * inv_cot;
	const Vector3 z_axis(-p_md.rows[3][0], -p_md.rows[3][1], -p_md.rows[3][2]);
	const float tx = p_md.rows[0][3] * inv_cot;
	const float ty = p_md.rows[1][3] * inv_cot;
	const float tz = -p_md.rows[3][3];
	return -(x_axis * tx + y_axis * ty + z_axis * tz);
}

// Step 2: Projected error in shadow texels of one page's mip (matches P5's texel rule at mip 0,
// scaled by 2^mip for coarser pages). Uses nextgeo_vsm_instance_scale() so mirrored non-uniformly
// scaled transforms never underestimate the maximum axis scale.
inline float nextgeo_vsm_page_projected_error(const float p_transform[3][4], const float p_lod_sphere[4], float p_error, const RendererVirtualShadowMapStorage::MapData &p_md, uint32_t p_mip, float p_lod_bias, float p_tau) {
	if (p_error >= 3.4e38f) {
		return p_tau + 1.0f;
	}
	const float scale = nextgeo_vsm_instance_scale(p_transform);
	const float page_texel = p_md.texel_scale * float(1u << p_mip);
	if (!(page_texel > 0.0f)) {
		return p_tau + 1.0f;
	}
	if (p_md.kind == uint32_t(RendererVirtualShadowMapStorage::MAP_KIND_DIRECTIONAL)) {
		return (p_error * scale / page_texel) * p_lod_bias;
	}
	const Vector3 center = nextgeo_vsm_xform_point(p_transform, Vector3(p_lod_sphere[0], p_lod_sphere[1], p_lod_sphere[2]));
	const float radius = p_lod_sphere[3] * scale;
	const Vector3 light_origin = nextgeo_vsm_light_origin(p_md);
	const float distance = MAX(float(center.distance_to(light_origin)) - radius, MAX(p_md.near_w, 1.0e-5f));
	return (p_error * scale / (page_texel * distance)) * p_lod_bias;
}

// Step 5: Triangle-size switch for one cluster on one VSM page's mip. Routes to software when the
// cluster's longest edge projects below p_threshold_texels shadow texels and (for perspective faces)
// its culling sphere lies strictly in front of the face near plane.
inline bool nextgeo_vsm_sw_route(const float p_transform[3][4], float p_scale, const float p_bounds[4], float p_max_edge, const RendererVirtualShadowMapStorage::MapData &p_md, uint32_t p_mip, float p_threshold_texels) {
	if (!(p_threshold_texels > 0.0f) || !(p_scale > 0.0f) || !(p_max_edge >= 0.0f) || !std::isfinite(p_max_edge)) {
		return false;
	}
	const float page_texel = p_md.texel_scale * float(1u << p_mip);
	if (!(page_texel > 0.0f)) {
		return false;
	}
	if (p_md.kind == uint32_t(RendererVirtualShadowMapStorage::MAP_KIND_DIRECTIONAL)) {
		const float edge_texels = (p_max_edge * p_scale) / page_texel;
		return edge_texels < p_threshold_texels;
	}
	const Vector3 center = nextgeo_vsm_xform_point(p_transform, Vector3(p_bounds[0], p_bounds[1], p_bounds[2]));
	const float w_center = RendererVirtualShadowMapStorage::row_dot(p_md.rows[3], center);
	const float w_min = w_center - p_bounds[3] * p_scale;
	if (!(w_min > p_md.near_w)) {
		return false;
	}
	const float edge_texels = (p_max_edge * p_scale) / MAX(page_texel * w_min, 1.0e-20f);
	return edge_texels < p_threshold_texels;
}

// Projects 8 world corners onto one VSM dirty page (mip, slot), returning whether the convex hull can
// overlap the page, plus the page-local pixel bounds [0, PAGE_SIZE], minimum linear depth, and whether
// the box straddles a local face's near plane. Matches cluster_cull.glsl VSM_VIEW.
inline bool nextgeo_vsm_corners_page_bounds(const Vector3 p_corners[8], const RendererVirtualShadowMapStorage::MapData &p_md, uint32_t p_mip, const Vector2i &p_slot, Vector2 &r_px_min, Vector2 &r_px_max, float &r_min_depth, bool &r_straddles_near) {
	using VSM = RendererVirtualShadowMapStorage;
	r_straddles_near = false;
	if (p_md.kind >= uint32_t(VSM::MAP_KIND_MAX) || p_mip >= VSM::MIP_COUNT) {
		return false;
	}
	if (p_md.kind == uint32_t(VSM::MAP_KIND_DIRECTIONAL)) {
		float min_tx = 0.0f;
		float max_tx = 0.0f;
		float min_ty = 0.0f;
		float max_ty = 0.0f;
		float min_d = 0.0f;
		for (int c = 0; c < 8; c++) {
			const float tx = VSM::row_dot(p_md.rows[0], p_corners[c]);
			const float ty = VSM::row_dot(p_md.rows[1], p_corners[c]);
			const float tz = VSM::row_dot(p_md.rows[2], p_corners[c]);
			if (c == 0) {
				min_tx = max_tx = tx;
				min_ty = max_ty = ty;
				min_d = tz;
			} else {
				min_tx = MIN(min_tx, tx);
				max_tx = MAX(max_tx, tx);
				min_ty = MIN(min_ty, ty);
				max_ty = MAX(max_ty, ty);
				min_d = MIN(min_d, tz);
			}
		}
		const uint32_t mask = (VSM::PAGES_PER_SIDE >> p_mip) - 1u;
		const int32_t mip_ox = p_md.origin[0] >> int32_t(p_mip);
		const int32_t mip_oy = p_md.origin[1] >> int32_t(p_mip);
		const int32_t abs_px = mip_ox + int32_t((uint32_t(p_slot.x) - uint32_t(mip_ox)) & mask);
		const int32_t abs_py = mip_oy + int32_t((uint32_t(p_slot.y) - uint32_t(mip_oy)) & mask);
		const float page_texels = float(VSM::PAGE_SIZE << p_mip);
		const float inv_mip_scale = 1.0f / float(1u << p_mip);
		const float pmin_x = float(abs_px) * page_texels;
		const float pmin_y = float(abs_py) * page_texels;
		r_px_min = Vector2((min_tx - pmin_x) * inv_mip_scale, (min_ty - pmin_y) * inv_mip_scale);
		r_px_max = Vector2((max_tx - pmin_x) * inv_mip_scale, (max_ty - pmin_y) * inv_mip_scale);
		r_min_depth = min_d;
		return r_px_max.x >= 0.0f && r_px_min.x <= float(VSM::PAGE_SIZE) && r_px_max.y >= 0.0f && r_px_min.y <= float(VSM::PAGE_SIZE);
	}

	const float n = float(VSM::PAGES_PER_SIDE >> p_mip);
	const float ox = n - 2.0f * float(p_slot.x) - 1.0f;
	const float oy = n - 2.0f * float(p_slot.y) - 1.0f;
	uint32_t out_near = 0u;
	uint32_t out_far = 0u;
	uint32_t out_left = 0u;
	uint32_t out_right = 0u;
	uint32_t out_bottom = 0u;
	uint32_t out_top = 0u;
	float cx[8];
	float cy[8];
	float cw[8];
	float min_w = 0.0f;
	for (int c = 0; c < 8; c++) {
		const float x = VSM::row_dot(p_md.rows[0], p_corners[c]);
		const float y = VSM::row_dot(p_md.rows[1], p_corners[c]);
		const float w = VSM::row_dot(p_md.rows[3], p_corners[c]);
		const float xc = x * n + w * ox;
		const float yc = y * n + w * oy;
		cx[c] = xc;
		cy[c] = yc;
		cw[c] = w;
		min_w = c == 0 ? w : MIN(min_w, w);
		out_near += w < p_md.near_w ? 1u : 0u;
		out_far += w > p_md.max_distance ? 1u : 0u;
		out_left += xc < -w ? 1u : 0u;
		out_right += xc > w ? 1u : 0u;
		out_bottom += yc < -w ? 1u : 0u;
		out_top += yc > w ? 1u : 0u;
	}
	if (out_near == 8u || out_far == 8u || out_left == 8u || out_right == 8u || out_bottom == 8u || out_top == 8u) {
		return false;
	}
	if (min_w <= p_md.near_w) {
		r_straddles_near = true;
		r_px_min = Vector2(0.0f, 0.0f);
		r_px_max = Vector2(float(VSM::PAGE_SIZE), float(VSM::PAGE_SIZE));
		r_min_depth = p_md.near_w;
		return true;
	}
	float min_px = 0.0f;
	float max_px = 0.0f;
	float min_py = 0.0f;
	float max_py = 0.0f;
	for (int c = 0; c < 8; c++) {
		const float px = ((cx[c] / cw[c]) * 0.5f + 0.5f) * float(VSM::PAGE_SIZE);
		const float py = ((cy[c] / cw[c]) * 0.5f + 0.5f) * float(VSM::PAGE_SIZE);
		if (c == 0) {
			min_px = max_px = px;
			min_py = max_py = py;
		} else {
			min_px = MIN(min_px, px);
			max_px = MAX(max_px, px);
			min_py = MIN(min_py, py);
			max_py = MAX(max_py, py);
		}
	}
	r_px_min = Vector2(min_px, min_py);
	r_px_max = Vector2(max_px, max_py);
	r_min_depth = min_w;
	return max_px >= 0.0f && min_px <= float(VSM::PAGE_SIZE) && max_py >= 0.0f && min_py <= float(VSM::PAGE_SIZE);
}

inline bool nextgeo_vsm_aabb_page_bounds(const Vector3 &p_min, const Vector3 &p_max, const RendererVirtualShadowMapStorage::MapData &p_md, uint32_t p_mip, const Vector2i &p_slot, Vector2 &r_px_min, Vector2 &r_px_max, float &r_min_depth, bool &r_straddles_near) {
	if (!(p_max.x >= p_min.x && p_max.y >= p_min.y && p_max.z >= p_min.z)) {
		return false;
	}
	Vector3 corners[8];
	for (int c = 0; c < 8; c++) {
		corners[c] = Vector3((c & 1) ? p_max.x : p_min.x, (c & 2) ? p_max.y : p_min.y, (c & 4) ? p_max.z : p_min.z);
	}
	return nextgeo_vsm_corners_page_bounds(corners, p_md, p_mip, p_slot, r_px_min, r_px_max, r_min_depth, r_straddles_near);
}

inline bool nextgeo_vsm_local_aabb_page_bounds(const float p_transform[3][4], const Vector3 &p_local_min, const Vector3 &p_local_max, const RendererVirtualShadowMapStorage::MapData &p_md, uint32_t p_mip, const Vector2i &p_slot, Vector2 &r_px_min, Vector2 &r_px_max, float &r_min_depth, bool &r_straddles_near) {
	if (!(p_local_max.x >= p_local_min.x && p_local_max.y >= p_local_min.y && p_local_max.z >= p_local_min.z)) {
		return false;
	}
	Vector3 corners[8];
	for (int c = 0; c < 8; c++) {
		const Vector3 local((c & 1) ? p_local_max.x : p_local_min.x, (c & 2) ? p_local_max.y : p_local_min.y, (c & 4) ? p_local_max.z : p_local_min.z);
		corners[c] = nextgeo_vsm_xform_point(p_transform, local);
	}
	return nextgeo_vsm_corners_page_bounds(corners, p_md, p_mip, p_slot, r_px_min, r_px_max, r_min_depth, r_straddles_near);
}

// Step 3: Build a 1-level 8x8 max-depth Hi-Z mip for one cached 64x64 physical page (static layer 0).
// Packed reversed-Z uint32: larger uint = nearer depth, 0 = empty (+INF). So the farthest depth in an
// 8x8 block is the minimum packed uint32; any empty texel yields 0 (never occludes).
inline void nextgeo_vsm_hiz_build_page(const uint32_t *p_static_page_64x64, uint32_t *r_hiz_8x8) {
	constexpr uint32_t PS = RendererVirtualShadowMapStorage::PAGE_SIZE;
	for (uint32_t cy = 0; cy < NEXTGEO_VSM_HIZ_PAGE_SIZE; cy++) {
		for (uint32_t cx = 0; cx < NEXTGEO_VSM_HIZ_PAGE_SIZE; cx++) {
			uint32_t min_packed = 0xFFFFFFFFu;
			const uint32_t bx = cx << NEXTGEO_VSM_HIZ_BLOCK_SHIFT;
			const uint32_t by = cy << NEXTGEO_VSM_HIZ_BLOCK_SHIFT;
			for (uint32_t dy = 0; dy < NEXTGEO_VSM_HIZ_BLOCK_SIZE; dy++) {
				const uint32_t row = (by + dy) * PS + bx;
				for (uint32_t dx = 0; dx < NEXTGEO_VSM_HIZ_BLOCK_SIZE; dx++) {
					min_packed = MIN(min_packed, p_static_page_64x64[row + dx]);
				}
			}
			r_hiz_8x8[cy * NEXTGEO_VSM_HIZ_PAGE_SIZE + cx] = min_packed == 0xFFFFFFFFu ? 0u : min_packed;
		}
	}
}

// Step 3: Conservative Hi-Z occlusion test on one physical page. Uncached, scrolled, static-dirty,
// unowned or near-straddling bounds always skip the Hi-Z test and return false (visible).
inline bool nextgeo_vsm_hiz_occluded(const uint32_t *p_hiz_8x8, uint32_t p_dirty_flags, bool p_owner_matches, const Vector2 &p_px_min, const Vector2 &p_px_max, float p_min_depth, bool p_straddles_near) {
	using VSM = RendererVirtualShadowMapStorage;
	if (p_hiz_8x8 == nullptr || !p_owner_matches || p_straddles_near) {
		return false;
	}
	if ((p_dirty_flags & (VSM::ENTRY_UNCACHED | VSM::ENTRY_SCROLLED | VSM::ENTRY_DIRTY_STATIC)) != 0u) {
		return false;
	}
	const uint32_t nearest_packed = VSM::depth_pack(p_min_depth);
	if (nearest_packed == 0u) {
		return false;
	}
	const int32_t x0 = CLAMP(int32_t(Math::floor(float(p_px_min.x))) >> int32_t(NEXTGEO_VSM_HIZ_BLOCK_SHIFT), 0, int32_t(NEXTGEO_VSM_HIZ_PAGE_SIZE) - 1);
	const int32_t y0 = CLAMP(int32_t(Math::floor(float(p_px_min.y))) >> int32_t(NEXTGEO_VSM_HIZ_BLOCK_SHIFT), 0, int32_t(NEXTGEO_VSM_HIZ_PAGE_SIZE) - 1);
	const int32_t x1 = CLAMP(int32_t(Math::floor(float(p_px_max.x))) >> int32_t(NEXTGEO_VSM_HIZ_BLOCK_SHIFT), 0, int32_t(NEXTGEO_VSM_HIZ_PAGE_SIZE) - 1);
	const int32_t y1 = CLAMP(int32_t(Math::floor(float(p_px_max.y))) >> int32_t(NEXTGEO_VSM_HIZ_BLOCK_SHIFT), 0, int32_t(NEXTGEO_VSM_HIZ_PAGE_SIZE) - 1);
	uint32_t min_hiz = 0xFFFFFFFFu;
	for (int32_t cy = y0; cy <= y1; cy++) {
		for (int32_t cx = x0; cx <= x1; cx++) {
			min_hiz = MIN(min_hiz, p_hiz_8x8[uint32_t(cy) * NEXTGEO_VSM_HIZ_PAGE_SIZE + uint32_t(cx)]);
		}
	}
	return nearest_packed < min_hiz;
}

// Step 5: CPU reference of SW vertex projection and depth recovery (matches vsm_raster_sw.glsl).
// Returns xy = page pixel [0, PAGE_SIZE], z = affine interpolant (linear depth for directional, 1/w
// for perspective local faces), w = 1 when valid (w > 0 for perspective, always 1 for directional).
inline Vector4 nextgeo_vsm_sw_project_vertex(const RendererVirtualShadowMapStorage::MapData &p_md, uint32_t p_mip, const Vector2i &p_slot, const Vector3 &p_world) {
	using VSM = RendererVirtualShadowMapStorage;
	if (p_md.kind == uint32_t(VSM::MAP_KIND_DIRECTIONAL)) {
		const uint32_t mask = (VSM::PAGES_PER_SIDE >> p_mip) - 1u;
		const int32_t mip_ox = p_md.origin[0] >> int32_t(p_mip);
		const int32_t mip_oy = p_md.origin[1] >> int32_t(p_mip);
		const int32_t abs_px = mip_ox + int32_t((uint32_t(p_slot.x) - uint32_t(mip_ox)) & mask);
		const int32_t abs_py = mip_oy + int32_t((uint32_t(p_slot.y) - uint32_t(mip_oy)) & mask);
		const float page_texels = float(VSM::PAGE_SIZE << p_mip);
		const float tx = VSM::row_dot(p_md.rows[0], p_world);
		const float ty = VSM::row_dot(p_md.rows[1], p_world);
		const float px = ((tx - float(abs_px) * page_texels) / page_texels) * float(VSM::PAGE_SIZE);
		const float py = ((ty - float(abs_py) * page_texels) / page_texels) * float(VSM::PAGE_SIZE);
		const float depth = VSM::row_dot(p_md.rows[2], p_world);
		return Vector4(px, py, depth, 1.0f);
	}
	const float x = VSM::row_dot(p_md.rows[0], p_world);
	const float y = VSM::row_dot(p_md.rows[1], p_world);
	const float w = VSM::row_dot(p_md.rows[3], p_world);
	if (!(w > 0.0f)) {
		return Vector4(0.0f, 0.0f, 0.0f, 0.0f);
	}
	const float n = float(VSM::PAGES_PER_SIDE >> p_mip);
	const float xc = x * n + w * (n - 2.0f * float(p_slot.x) - 1.0f);
	const float yc = y * n + w * (n - 2.0f * float(p_slot.y) - 1.0f);
	const float inv_w = 1.0f / w;
	const float px = ((xc * inv_w) * 0.5f + 0.5f) * float(VSM::PAGE_SIZE);
	const float py = ((yc * inv_w) * 0.5f + 0.5f) * float(VSM::PAGE_SIZE);
	return Vector4(px, py, inv_w, 1.0f);
}

inline bool nextgeo_vsm_sw_recover_depth(const RendererVirtualShadowMapStorage::MapData &p_md, float p_interp_z, float &r_depth) {
	if (p_md.kind == uint32_t(RendererVirtualShadowMapStorage::MAP_KIND_DIRECTIONAL)) {
		r_depth = p_interp_z;
		return std::isfinite(r_depth);
	}
	if (!(p_interp_z > 0.0f)) {
		return false;
	}
	const float depth = 1.0f / p_interp_z;
	if (!(depth >= p_md.near_w && depth <= p_md.max_distance)) {
		return false;
	}
	r_depth = depth;
	return true;
}

} // namespace RendererRD
