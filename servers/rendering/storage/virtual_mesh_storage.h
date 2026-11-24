/**************************************************************************/
/*  virtual_mesh_storage.h                                                */
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

#include "core/math/vector2.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/safe_refcount.h"
#include "servers/rendering/rendering_server_types.h"
#include "servers/rendering/storage/spline_deform.h"

// GodotNext: nextgeo_instance_records (#75, plan §6.3, §9), nextgeo_residency (#78)
//
// The persistent GPU instance/mesh records and fixed-slot streaming page arena
// that NextGeo's cull and raster read. Record layout is a std430 contract:
// change it in the GLSL copy (shaders/nextgeo/nextgeo_records_inc.glsl) in the
// same commit.
class RendererVirtualMeshStorage {
public:
	// Per-instance record. 224 B, 256 B under REAL_T_IS_DOUBLE (plan §6.3).
	enum RecordFlags : uint32_t {
		RECORD_VALID = 1u << 0,
		RECORD_VISIBLE = 1u << 1,
		RECORD_CAST_SHADOW_OFF = 0u << 2,
		RECORD_CAST_SHADOW_ON = 1u << 2,
		RECORD_CAST_SHADOW_DOUBLE_SIDED = 2u << 2,
		RECORD_CAST_SHADOW_SHADOWS_ONLY = 3u << 2,
		RECORD_CAST_SHADOW_MASK = 3u << 2,
		RECORD_MOVED = 1u << 4,
		RECORD_TELEPORTED = 1u << 5,
		RECORD_HAS_SKELETON = 1u << 6,
		RECORD_HAS_INSTANCE_UNIFORMS = 1u << 7,
		RECORD_RT_STATIC = 1u << 8,
		RECORD_RT_SKINNED = 1u << 9,
		RECORD_RT_ALPHA_AS_OPAQUE = 1u << 10,
		RECORD_RT_FAR = 1u << 11,
		// GodotNext: vsm_caching (#212, plan §21.3 contract S5). Per-instance VSM cache behavior
		// (bits 12-13, matching RSE::ShadowCacheInvalidation) and resolved dynamic layer bit (bit 14).
		RECORD_VSM_CACHE_AUTO = 0u << 12,
		RECORD_VSM_CACHE_ALWAYS = 1u << 12,
		RECORD_VSM_CACHE_RIGID = 2u << 12,
		RECORD_VSM_CACHE_STATIC = 3u << 12,
		RECORD_VSM_CACHE_MODE_SHIFT = 12u,
		RECORD_VSM_CACHE_MODE_MASK = 3u << 12,
		RECORD_VSM_DYNAMIC = 1u << 14,
		// GodotNext: nextgeo_vertex_offset (#164, plan §21.4 N5). Per-instance opt-in RT proxy WPO evaluation.
		RECORD_RT_EVALUATE_VERTEX_OFFSET = 1u << 15,
		// GodotNext: spline_meshes (#161, plan §21.4 contract N12). Bit 16 marks an active spline record;
		// bits 17..31 store the 15-bit index into the compact spline deformation SSBO (up to 32,767).
		RECORD_HAS_SPLINE = 1u << 16,
		RECORD_SPLINE_INDEX_SHIFT = 17u,
		RECORD_SPLINE_INDEX_MASK = 0xFFFE0000u,
	};

	// gi_flags.
	enum GIFlags : uint32_t {
		GI_MODE_NONE = 0u << 0,
		GI_MODE_STATIC = 1u << 0,
		GI_MODE_DYNAMIC = 2u << 0,
		GI_MODE_MASK = 3u << 0,
		GI_LIGHTMAPPED = 1u << 2,
	};

	struct Record {
		float transform[3][4] = { { 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f } };
		float prev_transform[3][4] = { { 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f } };
		float world_aabb_min[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // w = max scale.
		float world_aabb_max[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // w = lod_bias.
		float prev_world_aabb_min[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // w = programmable_raster_distance (#153).
		float prev_world_aabb_max[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // w = displacement_bound (#150).
		uint32_t virtual_mesh = UINT32_MAX;
		uint32_t page_table_offset = 0; // floatBitsToUint(vertex_offset_disable_distance) (#164; 0 = never).
		uint32_t surface_mask = 0;
		uint32_t bucket_base = 0;
		uint32_t layer_mask = 0xFFFFFFFFu;
		uint32_t flags = 0;
		uint32_t gi_flags = 0; // Low 16 bits: GI flags; high 16 bits: float16 vertex_offset_bound (#164).
		uint32_t pad0 = 0; // Fixed-function shadow surface_mask (#155).
		uint32_t tlas_instance_index = UINT32_MAX;
		uint32_t blas_handle = UINT32_MAX;
		uint32_t rt_arena_chunk = UINT32_MAX;
		uint32_t rt_arena_offset = 0;
		uint32_t src_page_table = UINT32_MAX;
		uint32_t card_set = UINT32_MAX;
		uint32_t pad1[2] = { 0, 0 }; // [0] = per-surface cull_disabled_mask, [1] = per-surface cull_front_mask (#82).
#ifdef REAL_T_IS_DOUBLE
		float model_precision[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		float prev_model_precision[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
#endif
	};

	// Per-mesh record, 32 B. It exists only while the mesh has live instances.
	struct MeshRecord {
		uint32_t bvh_node_offset = 0;
		uint32_t bvh_level_count = 0;
		uint32_t cluster_offset = 0;
		uint32_t cluster_count = 0;
		uint32_t group_offset = 0;
		uint32_t page_table_offset = 0;
		uint32_t page_count = 0;
		uint32_t surface_count = 0;
	};

	// GodotNext: nextgeo_sw_raster (#162, plan §21.4 N3). The raster-feature table. A raster bin is keyed
	// by (feature mask, material shader version); the fixed-function bin has no feature bit. Each entry
	// names the pass it rasterizes for, so the VSM page raster (#217) shares the table, and every later
	// feature adds its VSM entry in the same change. The GLSL copy is
	// shaders/nextgeo/raster_features_inc.glsl: change both in the same commit.
	enum RasterFeature : uint32_t {
		RASTER_FEATURE_MASKED = 1u << 0,
		RASTER_FEATURE_VERTEX_OFFSET = 1u << 1,
		RASTER_FEATURE_DEPTH_OFFSET = 1u << 2,
		RASTER_FEATURE_TWO_SIDED = 1u << 3,
		RASTER_FEATURE_SKINNED = 1u << 4,
		RASTER_FEATURE_DISPLACED = 1u << 5,
		RASTER_FEATURE_SPLINE = 1u << 6,
		RASTER_FEATURE_VOXEL = 1u << 7,
		RASTER_FEATURE_ASSEMBLY = 1u << 8,
		RASTER_FEATURE_ALL = (1u << 9) - 1u,
	};

	enum RasterPass : uint32_t {
		RASTER_PASS_VISIBILITY = 0, // The main-view visibility buffer (#162).
		RASTER_PASS_VSM = 1, // The virtual shadow map page pool (#217).
		RASTER_PASS_MAX,
	};

	// Bins of the visibility pass. Cluster cull compacts one software list and one hardware list per bin
	// and phase; each bin is one indirect software dispatch and one indirect-count hardware draw.
	// GodotNext: nextgeo_raster_bins (#155, plan §21.4 N3, N4). RASTER_BIN_COUNT counts the bins with
	// material-free pipelines (FIXED). RASTER_BIN_PROGRAMMABLE marks a table entry whose surfaces get a
	// programmable bin, 1 to RASTER_BIN_PROGRAMMABLE_MAX, from the RasterBinRegistry below.
	enum RasterBin : uint32_t {
		RASTER_BIN_FIXED = 0,
		RASTER_BIN_COUNT,
		RASTER_BIN_PROGRAMMABLE = 0xFFu,
	};
	static constexpr uint32_t RASTER_BIN_PROGRAMMABLE_MAX = 64;
	static constexpr uint32_t RASTER_BIN_LIMIT = RASTER_BIN_PROGRAMMABLE_MAX + 1;
	// The features that need the material's raster program (vertex(), displacement(), and the coverage subset of
	// fragment()). TWO_SIDED needs none: every bin flips facing from the per-surface cull bits.
	// GodotNext: nextgeo_displacement (#150, plan §21.4 N11). Include RASTER_FEATURE_DISPLACED.
	// GodotNext: nextgeo_masked_two_sided (#153). Split pixel-programmable (demotable past programmable_raster_distance)
	// from vertex-programmable features.
	static constexpr uint32_t RASTER_PIXEL_PROGRAMMABLE_FEATURES = RASTER_FEATURE_MASKED | RASTER_FEATURE_DEPTH_OFFSET;
	static constexpr uint32_t RASTER_VERTEX_PROGRAMMABLE_FEATURES = RASTER_FEATURE_VERTEX_OFFSET | RASTER_FEATURE_DISPLACED;
	static constexpr uint32_t RASTER_PROGRAM_FEATURES = RASTER_PIXEL_PROGRAMMABLE_FEATURES | RASTER_VERTEX_PROGRAMMABLE_FEATURES;

	struct RasterBinEntry {
		uint32_t features;
		RasterPass pass;
		RasterBin bin;
		const char *name;
	};

	static constexpr RasterBinEntry RASTER_BIN_TABLE[] = {
		{ 0u, RASTER_PASS_VISIBILITY, RASTER_BIN_FIXED, "FIXED" },
		// #217's page raster: the same triangle setup (sw_raster_setup_inc.glsl), 32-bit depth.
		{ 0u, RASTER_PASS_VSM, RASTER_BIN_FIXED, "VSM_FIXED" },
		// #155: one entry per feature; a surface's bin rasterizes all of its features. The VSM entries use
		// the raster target's depth-only mode (raster_material_inc.glsl, NEXTGEO_RASTER_DEPTH_ONLY).
		{ RASTER_FEATURE_TWO_SIDED, RASTER_PASS_VISIBILITY, RASTER_BIN_FIXED, "TWO_SIDED" },
		{ RASTER_FEATURE_TWO_SIDED, RASTER_PASS_VSM, RASTER_BIN_FIXED, "VSM_TWO_SIDED" },
		{ RASTER_FEATURE_MASKED, RASTER_PASS_VISIBILITY, RASTER_BIN_PROGRAMMABLE, "MASKED" },
		{ RASTER_FEATURE_MASKED, RASTER_PASS_VSM, RASTER_BIN_PROGRAMMABLE, "VSM_MASKED" },
		{ RASTER_FEATURE_VERTEX_OFFSET, RASTER_PASS_VISIBILITY, RASTER_BIN_PROGRAMMABLE, "VERTEX_OFFSET" },
		{ RASTER_FEATURE_VERTEX_OFFSET, RASTER_PASS_VSM, RASTER_BIN_PROGRAMMABLE, "VSM_VERTEX_OFFSET" },
		// GodotNext: depth_offset (#149, plan §21.4 contract N6). Pixel depth offset (DEPTH_OFFSET) in visibility and VSM bins.
		{ RASTER_FEATURE_DEPTH_OFFSET, RASTER_PASS_VISIBILITY, RASTER_BIN_PROGRAMMABLE, "DEPTH_OFFSET" },
		{ RASTER_FEATURE_DEPTH_OFFSET, RASTER_PASS_VSM, RASTER_BIN_PROGRAMMABLE, "VSM_DEPTH_OFFSET" },
		// GodotNext: nextgeo_displacement (#150). Displaced surfaces route through programmable raster bins (#163 adds dicing).
		{ RASTER_FEATURE_DISPLACED, RASTER_PASS_VISIBILITY, RASTER_BIN_PROGRAMMABLE, "DISPLACED" },
		{ RASTER_FEATURE_DISPLACED, RASTER_PASS_VSM, RASTER_BIN_PROGRAMMABLE, "VSM_DISPLACED" },
		// GodotNext: spline_meshes (#161, plan §21.4 contract N12). Fixed-function Hermite spline deform
		// runs at vertex fetch in both visibility and VSM fixed and programmable bins.
		{ RASTER_FEATURE_SPLINE, RASTER_PASS_VISIBILITY, RASTER_BIN_FIXED, "SPLINE" },
		{ RASTER_FEATURE_SPLINE, RASTER_PASS_VSM, RASTER_BIN_FIXED, "VSM_SPLINE" },
	};
	static constexpr uint32_t RASTER_BIN_TABLE_SIZE = sizeof(RASTER_BIN_TABLE) / sizeof(RASTER_BIN_TABLE[0]);

	// The table entry that rasterizes exactly p_features for p_pass, or -1 when no entry does.
	static constexpr int32_t find_raster_bin_entry(uint32_t p_features, RasterPass p_pass) {
		for (uint32_t i = 0; i < RASTER_BIN_TABLE_SIZE; i++) {
			if (RASTER_BIN_TABLE[i].features == p_features && RASTER_BIN_TABLE[i].pass == p_pass) {
				return int32_t(i);
			}
		}
		return -1;
	}

	// GodotNext: nextgeo_raster_bins (#155). The bin kind that rasterizes a surface with p_features for
	// p_pass: RASTER_BIN_FIXED, RASTER_BIN_PROGRAMMABLE, or -1 when a feature has no entry for that
	// pass yet (the surface keeps its legacy route).
	static constexpr int32_t resolve_raster_bin(uint32_t p_features, RasterPass p_pass) {
		int32_t bin = int32_t(RASTER_BIN_FIXED);
		for (uint32_t bit = 1u; bit != 0u && bit <= RASTER_FEATURE_ALL; bit <<= 1u) {
			if ((p_features & bit) == 0u) {
				continue;
			}
			const int32_t entry = find_raster_bin_entry(bit, p_pass);
			if (entry < 0) {
				return -1;
			}
			if (RASTER_BIN_TABLE[entry].bin == RASTER_BIN_PROGRAMMABLE) {
				bin = int32_t(RASTER_BIN_PROGRAMMABLE);
			}
		}
		if ((p_features & ~uint32_t(RASTER_FEATURE_ALL)) != 0u) {
			return -1;
		}
		return bin;
	}

	// GodotNext: nextgeo_raster_bins (#155, plan §21.4 N3). Programmable raster bins. A bin is keyed by
	// (feature mask, material shader version) as N3 states, plus the material: a bin binds one material's
	// parameters (its uniform buffer and textures) per dispatch. The (instance, surface) records that
	// route through a bin hold a reference; the last release frees the bin index for reuse.
	struct RasterBinKey {
		uint32_t features = 0;
		RID shader_version;
		RID material;

		bool operator==(const RasterBinKey &p_other) const {
			return features == p_other.features && shader_version == p_other.shader_version && material == p_other.material;
		}
	};

	class RasterBinRegistry {
		RasterBinKey keys[RASTER_BIN_LIMIT];
		uint32_t refcounts[RASTER_BIN_LIMIT] = {};
		uint32_t active_count = 0;

	public:
		// The bin of p_key, with one more reference: the bin that already holds the key, else the lowest
		// free bin. 0 (RASTER_BIN_FIXED) when the key has no program feature or every bin is taken.
		uint32_t acquire(const RasterBinKey &p_key) {
			if ((p_key.features & RASTER_PROGRAM_FEATURES) == 0u) {
				return RASTER_BIN_FIXED;
			}
			uint32_t free_bin = 0;
			for (uint32_t bin = 1; bin < RASTER_BIN_LIMIT; bin++) {
				if (refcounts[bin] > 0u && keys[bin] == p_key) {
					refcounts[bin]++;
					return bin;
				}
				if (refcounts[bin] == 0u && free_bin == 0u) {
					free_bin = bin;
				}
			}
			if (free_bin == 0u) {
				return RASTER_BIN_FIXED;
			}
			keys[free_bin] = p_key;
			refcounts[free_bin] = 1u;
			active_count++;
			return free_bin;
		}

		void release(uint32_t p_bin) {
			if (p_bin == RASTER_BIN_FIXED || p_bin >= RASTER_BIN_LIMIT || refcounts[p_bin] == 0u) {
				return;
			}
			refcounts[p_bin]--;
			if (refcounts[p_bin] == 0u) {
				keys[p_bin] = RasterBinKey();
				active_count--;
			}
		}

		uint32_t get_refcount(uint32_t p_bin) const { return p_bin < RASTER_BIN_LIMIT ? refcounts[p_bin] : 0u; }
		const RasterBinKey &get_key(uint32_t p_bin) const { return keys[p_bin < RASTER_BIN_LIMIT ? p_bin : 0u]; }
		uint32_t get_active_count() const { return active_count; }
	};

	static constexpr uint32_t RECORD_BYTES = sizeof(Record);
	static constexpr uint32_t MESH_RECORD_BYTES = sizeof(MeshRecord);
	static constexpr uint32_t INITIAL_RECORD_CAPACITY = 4096;
	static constexpr uint32_t INVALID_PAGE_SLOT = 0xFFFFFFFFu;
	static constexpr uint32_t DEFAULT_GEOMETRY_CACHE_MB = 512;
	static constexpr uint32_t DEFAULT_PAGE_UPLOAD_KIB_PER_FRAME = 4096;
	static constexpr uint64_t DEFAULT_IO_DELAY_SEED = 0x6E676D3738ULL;

	struct UploadRun {
		uint32_t first = 0;
		uint32_t count = 0;
	};

	// GodotNext: nextgeo_visibility_raster (#83). 16 B per-cluster GPU lookup
	// pointing into the global page arena for vertex-pulling hardware raster.
	struct ClusterGPUDesc {
		uint32_t pos_word_offset = 0;
		uint32_t tri_byte_offset = 0;
		uint32_t stride_and_counts = 0; // bits 0..7 = float stride, 8..15 = verts, 16..23 = tris, 24..31 = vertex_layout.
		uint32_t surface_and_page = 0; // bits 0..15 = surface_id, 16..31 = page_id.
	};

	// GodotNext: nextgeo_residency (#78). CPU mirror of resident cluster
	// refinement metadata so CPU_cut_hash matches cluster_cull.glsl bit-for-bit.
	struct ClusterRefineInfo {
		float group_lod[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		float group_error = 0.0f;
		int32_t refined_group = -1;
		bool valid = false;
	};

	// GodotNext: nextgeo_residency (#78). Main-view page request entry.
	struct PageRequest {
		uint32_t mesh_index = UINT32_MAX;
		uint32_t page_id = UINT32_MAX;
		bool visible = true;
		float distance = 0.0f;
		float px_error = 0.0f;
	};

	struct MeshInfo {
		RenderingServerTypes::VirtualGeometryDesc desc;
		MeshRecord record;
		RID mesh;
		uint32_t mesh_index = UINT32_MAX;
		uint64_t generation = 1;
		uint32_t live_instances = 0;
		bool record_dirty = false;
		// GodotNext: nextgeo_gpu_cull (#76). The mesh's DAG blob in the global
		// DAG buffer; the mesh record's offsets are relative to its base.
		uint32_t dag_offset = 0;
		uint32_t dag_size = 0;
		// GodotNext: nextgeo_visibility_raster (#83) / nextgeo_residency (#78).
		uint32_t cluster_table_offset = 0;
		uint32_t cluster_table_size = 0;
		Vector<uint8_t> cluster_blob;

		// Residency & streaming (#78).
		uint32_t total_page_count = 0;
		uint32_t root_page_count = 0;
		uint32_t group_count = 0;
		uint32_t layout_stride = 0;
		uint32_t vertex_layout = 0;
		LocalVector<uint32_t> group_page_ids;
		LocalVector<LocalVector<uint32_t>> group_pages; // Distinct pages holding clusters of group g.
		LocalVector<uint32_t> page_first_cluster;
		LocalVector<uint32_t> page_cluster_counts;
		LocalVector<uint32_t> page_to_slot; // Fixed arena slot, or INVALID_PAGE_SLOT (0xFFFFFFFFu).
		LocalVector<uint32_t> resident_bitset;
		LocalVector<uint32_t> refine_ok_bitset;
		LocalVector<LocalVector<uint32_t>> page_parents;
		LocalVector<LocalVector<uint32_t>> page_children;
		LocalVector<uint32_t> page_topo_depth;
		LocalVector<uint32_t> resident_child_count;
		LocalVector<ClusterRefineInfo> cluster_refine;
		uint32_t residency_refine_word_offset = 0;
		uint32_t residency_page_table_word_offset = 0;
		bool page_dag_ready = false;
		bool page_dag_scanning = false;
		// GodotNext: nextgeo_rt_native (#156). Bumped whenever refine_ok is recomputed, that is on
		// every residency change, so native RT buckets know when to take their cut again.
		uint64_t residency_generation = 0;
	};

	RendererVirtualMeshStorage();
	virtual ~RendererVirtualMeshStorage();

	// Mesh records.
	void mesh_set_virtual_geometry(RID p_mesh, const RenderingServerTypes::VirtualGeometryDesc &p_desc);
	void mesh_clear_virtual_geometry(RID p_mesh, uint32_t p_reason);
	bool mesh_has_virtual_geometry(RID p_mesh) const;
	uint32_t mesh_get_live_instance_count(RID p_mesh) const;
	const RenderingServerTypes::VirtualGeometryDesc *mesh_get_virtual_geometry_desc(RID p_mesh) const;
	// GodotNext: nextgeo_exclusive_dispatch (#79). The surface is inside the
	// mesh's eligible_surface_mask (index < 32).
	bool mesh_surface_is_virtual(RID p_mesh, uint32_t p_surface) const;
	bool mesh_is_stripped_to_coarse(RID p_mesh) const;
	// 0 while the mesh still has VGD or never had it; the clear reason after
	// a mesh_clear_virtual_geometry.
	uint32_t mesh_clear_reason(RID p_mesh) const;
	bool is_nextgeo_draw_ready() const { return nextgeo_draw_ready; }
	void set_nextgeo_draw_ready(bool p_ready) { nextgeo_draw_ready = p_ready; }
	void nextgeo_reset_counters() {
		nextgeo_double_dispatch = 0;
		nextgeo_missing = 0;
	}
	void nextgeo_count_double_dispatch() { nextgeo_double_dispatch++; }
	void nextgeo_count_missing() { nextgeo_missing++; }
	uint32_t nextgeo_get_double_dispatch() const { return nextgeo_double_dispatch; }
	uint32_t nextgeo_get_missing() const { return nextgeo_missing; }
	void nextgeo_count_vgd_ignored_warning() { nextgeo_vgd_ignored_warnings++; }
	uint32_t nextgeo_get_vgd_ignored_warnings() const { return nextgeo_vgd_ignored_warnings; }
	void nextgeo_count_auxiliary_proxy_draw() { nextgeo_auxiliary_proxy_draws++; }
	void nextgeo_count_auxiliary_authored_virtual_draw() { nextgeo_auxiliary_authored_virtual_draws++; }
	void nextgeo_count_uv2_authored_draw() { nextgeo_uv2_authored_draws++; }
	void nextgeo_count_uv2_proxy_draw() { nextgeo_uv2_proxy_draws++; }
	uint64_t nextgeo_get_auxiliary_proxy_draws() const { return nextgeo_auxiliary_proxy_draws; }
	uint64_t nextgeo_get_auxiliary_authored_virtual_draws() const { return nextgeo_auxiliary_authored_virtual_draws; }
	uint64_t nextgeo_get_uv2_authored_draws() const { return nextgeo_uv2_authored_draws; }
	uint64_t nextgeo_get_uv2_proxy_draws() const { return nextgeo_uv2_proxy_draws; }
	// GodotNext: nextgeo_raster_bins (#155). The programmable raster bins (render thread only).
	uint32_t raster_bin_acquire(const RasterBinKey &p_key) { return raster_bins.acquire(p_key); }
	void raster_bin_release(uint32_t p_bin) { raster_bins.release(p_bin); }
	const RasterBinRegistry &get_raster_bins() const { return raster_bins; }

	// Instance records.
	uint32_t instance_record_allocate(RID p_instance, RID p_mesh);
	void instance_record_free(uint32_t p_record);
	void instance_record_set_transform(uint32_t p_record, const Transform3D &p_transform, const AABB &p_world_aabb, bool p_visible, bool p_teleport = false, float p_lod_bias = 0.0f);
	void instance_record_set_state(uint32_t p_record, uint32_t p_flags);
	void instance_record_set_layer_mask(uint32_t p_record, uint32_t p_layer_mask);
	// GodotNext: nextgeo_displacement (#150, plan §21.4 N11). Per-instance max displacement magnitude in meters.
	void instance_record_set_displacement_bound(uint32_t p_record, float p_magnitude);
	// GodotNext: nextgeo_masked_two_sided (#153, plan §21.4.1). Per-instance programmable raster cutoff distance in meters (0 = always).
	void instance_record_set_programmable_raster_distance(uint32_t p_record, float p_distance);
	// GodotNext: nextgeo_vertex_offset (#164, plan §21.4 N5). Per-instance WPO bound (packed as conservative f16
	// in gi_flags[31:16]), disable distance (float bits in page_table_offset; 0 = never), and RT evaluation flag.
	static _FORCE_INLINE_ uint16_t pack_vertex_offset_bound_half(float p_bound) {
		if (!Math::is_finite(p_bound) || !(p_bound > 0.0f)) {
			return 0u;
		}
		const float clamped = MIN(p_bound, 65504.0f);
		uint16_t h = Math::make_half_float(clamped);
		if (Math::half_to_float(h) < clamped && h < 0x7BFFu) {
			h++;
		}
		return h;
	}
	static _FORCE_INLINE_ float unpack_vertex_offset_bound_half(uint32_t p_gi_flags) {
		return Math::half_to_float(uint16_t((p_gi_flags >> 16u) & 0xFFFFu));
	}
	static _FORCE_INLINE_ float get_record_vertex_offset_bound(const Record &p_record) {
		return unpack_vertex_offset_bound_half(p_record.gi_flags);
	}
	static _FORCE_INLINE_ float get_record_vertex_offset_disable_distance(const Record &p_record) {
		float dist = 0.0f;
		memcpy(&dist, &p_record.page_table_offset, sizeof(float));
		return (Math::is_finite(dist) && dist > 0.0f) ? dist : 0.0f;
	}
	void instance_record_set_vertex_offset_bound(uint32_t p_record, float p_bound);
	void instance_record_set_vertex_offset_disable_distance(uint32_t p_record, float p_distance);
	void instance_record_set_rt_evaluate_vertex_offset(uint32_t p_record, bool p_enable);
	static _FORCE_INLINE_ AABB inflate_aabb_for_displacement(const AABB &p_aabb, float p_magnitude) {
		return (Math::is_finite(p_magnitude) && p_magnitude > 0.0f) ? p_aabb.grow(p_magnitude) : p_aabb;
	}
	static _FORCE_INLINE_ float inflate_sphere_radius_for_displacement(float p_radius, float p_magnitude) {
		return (Math::is_finite(p_magnitude) && p_magnitude > 0.0f) ? (p_radius + p_magnitude) : p_radius;
	}
	void instance_record_set_gi_flags(uint32_t p_record, uint32_t p_gi_flags);
	void instance_record_set_surface_mask(uint32_t p_record, uint32_t p_surface_mask, uint32_t p_shadow_surface_mask = UINT32_MAX);
	void instance_record_set_surface_cull_masks(uint32_t p_record, uint32_t p_cull_disabled_mask, uint32_t p_cull_front_mask);
	RID instance_record_get_instance(uint32_t p_record) const;
	// tlas_instance_index, blas_handle, rt_arena_chunk, rt_arena_offset, src_page_table, card_set.
	void instance_record_set_rt(uint32_t p_record, const uint32_t p_rt_fields[6]);
	void instance_record_set_rt_flags(uint32_t p_record, uint32_t p_rt_flags);
	// GodotNext: vsm_caching (#212). Updates RECORD_VSM_DYNAMIC without touching other record flags.
	void instance_record_set_vsm_dynamic(uint32_t p_record, bool p_dynamic);
	// GodotNext: spline_meshes (#161, plan §21.4 contract N12). Compact per-record spline pose & history.
	void instance_record_set_spline_deform(uint32_t p_record, bool p_active, const SplineDeformPose &p_pose, bool p_teleport = false);
	uint32_t get_live_spline_count() const { return live_splines; }
	uint32_t get_spline_entry_count() const { return spline_entries.size(); }
	const SplineDeformEntry &get_spline_entry(uint32_t p_index) const;

	// Per frame: advance the free list and the temporal history, process page
	// streaming uploads under the per-frame KiB cap, then upload dirty records.
	void begin_frame();
	void process_streaming();
	void flush();

	// GodotNext: vsm_max_storage_buffer_range (#306). Layout chosen for the NextGeo page arena
	// storage buffer against the device's maxStorageBufferRange limit.
	struct PageArenaLayout {
		uint32_t arena_bytes = 0;
		uint32_t slot_count = 0;
		uint64_t requested_bytes = 0;
		uint64_t effective_limit_bytes = 0;
		bool clamped_to_device_limit = false;
		bool valid = false;
	};

	static PageArenaLayout compute_page_arena_layout(uint64_t p_desired_bytes, uint32_t p_slot_page_size, uint32_t p_required_pinned_slots, uint64_t p_max_storage_buffer_range);

	// --- Residency & Streaming (#78) ---
	void set_page_arena_bytes_override(uint64_t p_bytes);
	void set_max_storage_buffer_range_override(uint64_t p_bytes);
	uint64_t get_effective_max_storage_buffer_range() const;
	PageArenaLayout get_page_arena_layout() const;
	void set_page_upload_bytes_per_frame_override(uint64_t p_bytes);
	void set_io_delay_seed(uint64_t p_seed);
	uint64_t get_io_delay_seed() const { return io_delay_seed; }
	static uint64_t compute_debug_io_delay_usec(uint64_t p_seed, uint64_t p_request_index, Vector2 p_delay_ms);
	static Vector2 parse_debug_io_delay_setting(const Variant &p_value);

	// Set explicit page-parent edges and/or in-memory non-root page payloads (for unit tests).
	void mesh_set_page_parents(RID p_mesh, const LocalVector<LocalVector<uint32_t>> &p_page_parents);
	void mesh_set_in_memory_pages(RID p_mesh, const Vector<Vector<uint8_t>> &p_pages);

	// Submit page requests (only p_is_main_view == true schedules loads; non-main views are rejected and counted).
	void submit_page_requests(const PageRequest *p_requests, uint32_t p_count, bool p_is_main_view);
	// Mark resident page slots reported as drawn by the main-view GPU cluster cull.
	void touch_used_page_slots(const uint32_t *p_used_words, uint32_t p_word_count);
	// Wait for all currently in-flight worker IO tasks (p_cancel=true aborts sleeps/reads on shutdown).
	void wait_for_streaming_tasks(bool p_cancel = false);

	bool mesh_is_page_resident(RID p_mesh, uint32_t p_page_id) const;
	uint32_t mesh_get_page_slot(RID p_mesh, uint32_t p_page_id) const;
	bool mesh_is_group_refine_ok(uint32_t p_mesh_index, uint32_t p_group_index) const;
	bool mesh_is_group_refine_ok_rid(RID p_mesh, uint32_t p_group_index) const;

	// CPU mirror of refine_ok[g]: root groups are always ok; non-root group g is ok iff every
	// page holding a cluster whose parent_lod is group g is resident.
	static void compute_refine_ok_bits(uint32_t p_group_count, const LocalVector<LocalVector<uint32_t>> &p_group_pages, uint32_t p_root_page_count, const uint8_t *p_page_resident, uint32_t p_page_count, LocalVector<uint32_t> &r_refine_ok_bits);

	// Residency events (#78 step 7, #82): record slots whose virtual mesh changed residency this frame.
	const LocalVector<uint32_t> &get_residency_changed_records() const { return residency_changed_records; }
	uint64_t get_residency_event_epoch() const { return residency_event_epoch; }

	// Residency & streaming counters / diagnostics.
	uint64_t get_child_resident_without_parent_count() const { return child_resident_without_parent; }
	uint64_t get_evictions_with_resident_child_count() const { return evictions_with_resident_child; }
	uint64_t get_non_main_view_request_count() const { return non_main_view_requests; }
	uint64_t get_total_page_uploads() const { return total_page_uploads; }
	uint64_t get_total_page_evictions() const { return total_page_evictions; }
	uint64_t get_total_page_upload_bytes() const { return total_page_upload_bytes; }
	uint32_t get_last_frame_upload_bytes() const { return last_frame_upload_bytes; }
	uint32_t get_resident_page_count() const { return occupied_slot_count; }
	uint32_t get_pinned_page_count() const { return pinned_slot_count; }
	uint32_t get_page_slot_count() const { return uint32_t(page_slots.size()); }
	uint32_t get_page_slot_size_bytes() const { return slot_page_size; }
	uint64_t get_page_arena_allocation_bytes() const { return uint64_t(page_capacity); }
	uint64_t get_dag_allocation_bytes() const { return uint64_t(dag_capacity); }
	double get_last_page_upload_ms() const { return last_page_upload_ms; }
	Vector<uint64_t> get_recorded_io_delays_usec() const;
	void clear_recorded_io_delays();

	// Introspection (doctests and the debug overlay).
	const Record &get_record(uint32_t p_record) const;
	const MeshRecord &get_mesh_record(RID p_mesh) const;
	uint32_t get_record_capacity() const { return record_capacity; }
	uint32_t get_live_record_count() const { return live_records; }
	uint32_t get_frame() const { return frame; }
	const HashMap<RID, MeshInfo> &get_meshes() const { return meshes; }
	const MeshInfo *get_mesh_info(uint32_t p_mesh_index) const;
	virtual RID get_record_buffer() const { return RID(); }
	virtual RID get_mesh_record_buffer() const { return RID(); }
	virtual RID get_dag_buffer() const { return RID(); }
	virtual RID get_page_buffer() const { return RID(); }
	virtual RID get_cluster_buffer() const { return RID(); }
	virtual RID get_residency_buffer() const { return RID(); }
	virtual RID get_spline_buffer() const { return RID(); }
	uint32_t get_record_count() const { return used_slots; }
	uint32_t get_mesh_record_count() const { return mesh_records.size(); }
	uint32_t get_max_bvh_levels() const;

protected:
	virtual void _upload_records(uint32_t p_first, uint32_t p_count) = 0;
	virtual void _upload_mesh_records(uint32_t p_first, uint32_t p_count) = 0;
	// GodotNext: nextgeo_gpu_cull (#76). The DAG buffer lives in the renderer.
	virtual uint64_t _get_max_storage_buffer_range() const { return 0; }
	virtual bool _resize_dag(uint32_t p_bytes) { return false; }
	virtual void _upload_dag(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) {}
	virtual void _reset_dag() {}
	// GodotNext: nextgeo_visibility_raster (#83) / nextgeo_residency (#78).
	virtual bool _resize_pages(uint32_t p_bytes) { return true; }
	virtual void _upload_pages(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) {}
	virtual void _reset_pages() {}
	virtual bool _resize_clusters(uint32_t p_bytes) { return true; }
	virtual void _upload_clusters(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) {}
	virtual void _reset_clusters() {}
	// GodotNext: nextgeo_residency (#78). GPU residency table (refine_ok + page_to_slot).
	virtual bool _resize_residency(uint32_t p_bytes) { return true; }
	virtual void _upload_residency(uint32_t p_offset, const uint8_t *p_data, uint32_t p_size) {}
	virtual void _reset_residency() {}
	// GodotNext: spline_meshes (#161). Compact spline deformation buffer.
	virtual void _upload_splines(uint32_t p_first, uint32_t p_count) {}
	virtual void _reset_splines() {}
	// Renderer-specific bookkeeping after a record slot is reused.
	virtual void _record_reused(uint32_t p_record) {}
	// Renderer-specific refusal (for example an out-of-memory RD buffer).
	virtual bool _can_allocate_record() const { return true; }

	// One CPU-side slot. A freed slot is reusable on the next frame.
	struct SlotState {
		RID instance;
		bool allocated = false;
		bool dirty = false;
		bool dirty_last = false;
		bool initialized = false;
		bool rewritten = false;
		uint32_t free_frame = UINT32_MAX;
	};

	struct PageSlot {
		RID mesh;
		uint32_t page_id = UINT32_MAX;
		uint64_t last_used_frame = 0;
		uint32_t lru_prev = UINT32_MAX;
		uint32_t lru_next = UINT32_MAX;
		bool occupied = false;
		bool pinned = false;
		bool in_lru = false;
	};

	struct QueuedPageKey {
		RID mesh;
		uint32_t page_id = 0;
		bool operator==(const QueuedPageKey &p_other) const {
			return mesh == p_other.mesh && page_id == p_other.page_id;
		}
	};

	struct QueuedPageKeyHasher {
		static _FORCE_INLINE_ uint32_t hash(const QueuedPageKey &p_key) {
			uint32_t h = hash_murmur3_one_64(p_key.mesh.get_id());
			return hash_fmix32(hash_murmur3_one_32(p_key.page_id, h));
		}
	};

	struct PrioritizedPageRequest {
		RID mesh;
		uint32_t mesh_index = UINT32_MAX;
		uint64_t generation = 0;
		uint32_t page_id = 0;
		uint32_t topo_depth = 0;
		bool visible = true;
		float distance = 0.0f;
		float px_error = 0.0f;
	};

	struct StagedPage {
		RID mesh;
		uint64_t generation = 0;
		uint32_t page_id = 0;
		uint32_t topo_depth = 0;
		bool visible = true;
		float distance = 0.0f;
		float px_error = 0.0f;
		Vector<uint8_t> raw;
	};

	struct IOBatchItem {
		RID mesh;
		uint64_t generation = 0;
		String ngm_path;
		uint32_t page_id = 0;
		uint32_t topo_depth = 0;
		bool visible = true;
		float distance = 0.0f;
		float px_error = 0.0f;
		uint64_t request_index = 0;
		uint64_t delay_seed = 0;
		Vector2 delay_ms;
		Vector<uint8_t> in_memory_raw;
		bool has_in_memory = false;
	};

	struct IODagScanItem {
		RID mesh;
		uint64_t generation = 0;
		String ngm_path;
		uint32_t total_page_count = 0;
		uint32_t group_count = 0;
		LocalVector<uint32_t> group_page_ids;
		LocalVector<LocalVector<uint32_t>> page_parents;
		bool ok = false;
	};

	struct IOWorkerBatch {
		RendererVirtualMeshStorage *owner = nullptr;
		LocalVector<IODagScanItem> dag_scans;
		LocalVector<IOBatchItem> items;
		LocalVector<StagedPage> results;
		LocalVector<uint64_t> delays_usec;
		WorkerThreadPool::TaskID task_id = WorkerThreadPool::INVALID_TASK_ID;
		SafeFlag done;
	};

	void _ensure_capacity(uint32_t p_records);
	void _mark_dirty(uint32_t p_record);
	uint32_t _alloc_dag(uint32_t p_size);
	void _free_dag(uint32_t p_offset, uint32_t p_size);
	uint32_t _alloc_clusters(uint32_t p_size);
	void _free_clusters(uint32_t p_offset, uint32_t p_size);
	MeshRecord _parse_dag(const Vector<uint8_t> &p_dag, uint32_t p_base);
	void _write_transform(Record &r_record, const Transform3D &p_transform, const AABB &p_world_aabb, float p_lod_bias);

	// Residency internal helpers (#78).
	uint64_t _desired_page_arena_bytes() const;
	uint64_t _desired_page_upload_budget_bytes() const;
	bool _ensure_page_arena();
	void _lru_unlink(uint32_t p_slot);
	void _lru_push_back(uint32_t p_slot);
	void _touch_page_slot(uint32_t p_slot);
	uint32_t _allocate_page_slot(bool p_pinned);
	void _free_mesh_page_slots(MeshInfo &r_info);
	bool _evict_slot(uint32_t p_slot, bool p_record_event);
	void _evict_all_non_pinned_leaf_first();
	bool _install_page_in_slot(MeshInfo &r_info, uint32_t p_page_id, uint32_t p_slot, const Vector<uint8_t> &p_raw, bool p_pinned, bool p_record_event);
	bool _all_parent_pages_resident(const MeshInfo &p_info, uint32_t p_page_id) const;
	void _rebuild_page_children_and_depths(MeshInfo &r_info);
	void _recompute_mesh_refine_ok(MeshInfo &r_info);
	void _rebuild_residency_gpu_table();
	void _sync_residency_gpu_mesh(const MeshInfo &p_info);
	void _process_streaming_frame();
	void _poll_worker_batches();
	void _dispatch_worker_batches();
	static void _io_worker_entry(void *p_userdata);

	LocalVector<Record> records;
	LocalVector<MeshRecord> mesh_records;
	LocalVector<SlotState> slot_states;
	LocalVector<uint32_t> free_now;
	LocalVector<uint32_t> free_next;
	// GodotNext: spline_meshes (#161). Compact spline entry pool with per-frame temporal history.
	LocalVector<SplineDeformEntry> spline_entries;
	LocalVector<uint8_t> spline_allocated;
	LocalVector<uint8_t> spline_dirty;
	LocalVector<uint8_t> spline_dirty_last;
	LocalVector<uint8_t> spline_rewritten;
	LocalVector<uint32_t> spline_free_now;
	LocalVector<uint32_t> spline_free_next;
	uint32_t live_splines = 0;

	HashMap<RID, MeshInfo> meshes;
	HashMap<RID, uint32_t> mesh_indices;
	LocalVector<RID> mesh_order; // mesh index -> RID
	HashMap<RID, uint32_t> mesh_clear_reasons;
	HashMap<RID, Vector<Vector<uint8_t>>> in_memory_mesh_pages;
	bool nextgeo_draw_ready = false;
	uint32_t nextgeo_double_dispatch = 0;
	uint32_t nextgeo_missing = 0;
	RasterBinRegistry raster_bins;
	uint32_t nextgeo_vgd_ignored_warnings = 0;
	uint64_t nextgeo_auxiliary_proxy_draws = 0;
	uint64_t nextgeo_auxiliary_authored_virtual_draws = 0;
	uint64_t nextgeo_uv2_authored_draws = 0;
	uint64_t nextgeo_uv2_proxy_draws = 0;
	LocalVector<RID> mesh_rids;
	LocalVector<uint8_t> mesh_record_dirty;

	uint32_t record_capacity = 0;
	uint32_t used_slots = 0;
	uint32_t dag_used = 0;
	uint32_t dag_capacity = 0;
	uint32_t page_capacity = 0;
	uint32_t cluster_used = 0;
	uint32_t cluster_capacity = 0;
	struct DAGFree {
		uint32_t offset = 0;
		uint32_t size = 0;
	};
	LocalVector<DAGFree> dag_free;
	LocalVector<DAGFree> cluster_free;
	uint32_t live_records = 0;
	uint32_t frame = 0;
	uint32_t streamed_frame = UINT32_MAX;
	bool mesh_records_dirty = false;
	// Teleport threshold, in meters. A move larger than this sets prev = cur.
	float teleport_distance = 100.0f;

	// Residency & streaming state (#78).
	uint32_t slot_page_size = 64u * 1024u;
	uint64_t page_arena_override_bytes = 0;
	uint64_t max_storage_buffer_range_override = 0;
	PageArenaLayout last_page_arena_layout;
	uint64_t page_upload_override_bytes = 0;
	LocalVector<PageSlot> page_slots;
	LocalVector<uint32_t> free_page_slots;
	LocalVector<uint32_t> pending_used_slots;
	uint32_t lru_head = UINT32_MAX;
	uint32_t lru_tail = UINT32_MAX;
	uint32_t occupied_slot_count = 0;
	uint32_t pinned_slot_count = 0;

	LocalVector<uint32_t> residency_words;
	uint32_t residency_capacity_bytes = 0;
	bool residency_layout_dirty = true;
	bool residency_data_dirty = false;

	HashMap<QueuedPageKey, PrioritizedPageRequest, QueuedPageKeyHasher> pending_requests;
	HashSet<QueuedPageKey, QueuedPageKeyHasher> in_flight_pages;
	LocalVector<StagedPage> staged_pages;
	LocalVector<IOWorkerBatch *> active_batches;
	SafeFlag shutting_down;

	HashSet<uint32_t> residency_changed_meshes;
	LocalVector<uint32_t> residency_changed_records;
	uint64_t residency_event_epoch = 0;

	uint64_t io_delay_seed = DEFAULT_IO_DELAY_SEED;
	uint64_t next_io_request_index = 0;
	mutable Mutex delay_history_mutex;
	LocalVector<uint64_t> recorded_io_delays_usec;

	uint64_t child_resident_without_parent = 0;
	uint64_t evictions_with_resident_child = 0;
	uint64_t non_main_view_requests = 0;
	uint64_t total_page_uploads = 0;
	uint64_t total_page_evictions = 0;
	uint64_t total_page_upload_bytes = 0;
	uint32_t last_frame_upload_bytes = 0;
	double last_page_upload_ms = 0.0;
	double last_cpu_page_upload_ms = 0.0;
};
