/**************************************************************************/
/*  next_gi_rt_native.h                                                   */
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
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "servers/rendering/renderer_rd/environment/next_gi_rt_scene.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/storage/virtual_mesh_storage.h"

class RenderDataRD;

namespace RendererRD {

// GodotNext: nextgeo_rt_native (#156, plan §21.4 N13, GN-9, N7, MN4).
//
// Native Nanite ray tracing: rendering/nextgeo/ray_tracing_mode = native (EXPERIMENTAL, off by
// default). The fallback mode, the default, is the fixed D13 proxy and runs none of this code.
//
// In native mode a NextGeo instance enters the TLAS through the BLAS of its (mesh, camera-distance
// band) bucket. The bucket's cut is taken on the CPU from the resident pages (the residency mirror
// of #78) at a mesh-space error that grows with the band, never coarser than the fixed proxy. The
// CPU knows every count, so a compute pass (rt_cut_extract.glsl) only scatters the cut's positions
// and indices from the page arena into the RT arena, and the BLAS is built with exact primitive
// counts: no indirect AS build. Rebuilds follow residency changes, are capped per frame (N7) and
// share line 1b's triangle budget; native BLAS memory stays within MN4 (twice the fallback, inside the
// AS memory line) at every TLAS phase and every admission. An instance whose bucket is not built yet
// keeps its previous native BLAS, or the proxy when it has none; when MN4 has room for the new set
// only without that previous one, the previous one leaves first and the instance takes the proxy
// until the new set is built.
//
// Everything above NextGIRTNative is device-free so the doctests can run it without a GPU,
// including the bucket state machine (NextGeoRTNativeSets).

enum NextGeoRTMode {
	NEXTGEO_RT_MODE_FALLBACK = 0,
	NEXTGEO_RT_MODE_NATIVE = 1,
	NEXTGEO_RT_MODE_MAX,
};

_FORCE_INLINE_ NextGeoRTMode nextgeo_rt_mode_from_setting(int p_value) {
	return p_value == NEXTGEO_RT_MODE_NATIVE ? NEXTGEO_RT_MODE_NATIVE : NEXTGEO_RT_MODE_FALLBACK;
}

// Provisional values. The plan does not fix the band rule, so these are the one named set that a
// lead decision or PQ (N7, MN4, GN-9) may change.
// Band b covers effective camera distances (distance / max scale) [base * 2^(b - 1), base * 2^b),
// band 0 everything nearer than the base. A band's error target is the world error of
// max_pixel_error pixels at the band's near distance (band 0: half the base), capped at the proxy's
// eps_rt, so native is never coarser than the fallback.
static constexpr float NEXTGEO_RT_NATIVE_BAND_BASE_DISTANCE = 4.0f;
enum {
	NEXTGEO_RT_NATIVE_BAND_COUNT = 8,
	NEXTGEO_RT_NATIVE_REBUILD_TRIANGLE_CAP = 250000, // N7; report-only, measured in PQ.
	NEXTGEO_RT_NATIVE_MEMORY_RATIO = 2, // MN4: native BLAS bytes <= 2x the fallback BLAS bytes.
	NEXTGEO_RT_NATIVE_RETIRE_FRAMES = 4, // An RT-arena range of a replaced set is reused after this many frames.
	NEXTGEO_RT_NATIVE_ABSENT_FRAMES = 120, // A bucket no instance used for this long is released.
	NEXTGEO_RT_NATIVE_MAX_JOBS_PER_DISPATCH = 65535,
	// A triangle that no card of its proxy-level groups holds searches every card of the mesh, when
	// the mesh has at most this many (rt_cut_extract.glsl).
	NEXTGEO_RT_NATIVE_CARD_SEARCH_ALL_LIMIT = 4096,
};
static constexpr uint16_t NEXTGEO_RT_NATIVE_NO_CARD = 0xFFFFu;

// The band of an instance at p_distance (to its bounds) and p_max_scale. Orthographic views use band 0.
uint32_t nextgeo_rt_native_band(float p_distance, float p_max_scale);

// World error of max_pixel_error pixels per meter of distance (perspective), or per view
// (orthographic), quantized down to a power of two so a resize does not rebuild every bucket.
// p_projection_y is |P[1][1]|. Returns 0 when the inputs are unusable.
float nextgeo_rt_native_error_per_meter(float p_max_pixel_error, float p_internal_height, float p_projection_y);

// The mesh-space error target of a band: min(proxy eps, error per meter * near distance). With no
// usable error per meter, every band takes the proxy's eps.
float nextgeo_rt_native_band_error(uint32_t p_band, float p_error_per_meter, float p_proxy_eps, bool p_orthogonal);

// Instances that keep the proxy in native mode (plan §21.4 N13, issue step 8). Displaced and spline
// instances join this list with the features that create them (they are not on master yet).
enum NextGeoRTNativeExclusion {
	NEXTGEO_RT_NATIVE_INCLUDED = 0,
	NEXTGEO_RT_NATIVE_EXCLUDED_SKINNED,
	NEXTGEO_RT_NATIVE_EXCLUDED_VERTEX_OFFSET,
	// A mesh whose proxy also carries legacy (non-virtual) surfaces: the native cut has only the
	// virtual ones, so the mesh keeps its proxy rather than lose those surfaces from the TLAS.
	NEXTGEO_RT_NATIVE_EXCLUDED_LEGACY_SURFACES,
	NEXTGEO_RT_NATIVE_EXCLUSION_MAX,
};

NextGeoRTNativeExclusion nextgeo_rt_native_exclusion(bool p_skinned, bool p_vertex_offset, bool p_legacy_surfaces);

// Which BLAS an instance traces in native mode.
enum NextGeoRTNativeChoice {
	NEXTGEO_RT_NATIVE_USE_BAND = 0, // Its band's bucket is built.
	NEXTGEO_RT_NATIVE_USE_PREVIOUS, // The native set it used before (another band), still alive.
	NEXTGEO_RT_NATIVE_USE_PROXY, // Neither: the fixed proxy.
};

_FORCE_INLINE_ NextGeoRTNativeChoice nextgeo_rt_native_choose(bool p_band_ready, bool p_previous_ready) {
	if (p_band_ready) {
		return NEXTGEO_RT_NATIVE_USE_BAND;
	}
	return p_previous_ready ? NEXTGEO_RT_NATIVE_USE_PREVIOUS : NEXTGEO_RT_NATIVE_USE_PROXY;
}

// MN4 (plan §21.4): native RT BLASes take at most twice the fallback BLAS memory and sit inside
// the AS memory line; over either, instances keep the proxy. The native cap is the smaller of the
// two: twice p_fallback_bytes (proxy build bytes of the meshes native mode covers), and what the AS
// line has left after every proxy and legacy BLAS (p_as_proxy_bytes of p_as_budget_bytes).
_FORCE_INLINE_ uint64_t nextgeo_rt_native_memory_cap(uint64_t p_fallback_bytes, uint64_t p_as_budget_bytes, uint64_t p_as_proxy_bytes) {
	const uint64_t mn4 = p_fallback_bytes * uint64_t(NEXTGEO_RT_NATIVE_MEMORY_RATIO);
	const uint64_t as_room = p_as_budget_bytes > p_as_proxy_bytes ? p_as_budget_bytes - p_as_proxy_bytes : 0u;
	return MIN(mn4, as_room);
}

// A whole set fits while all native BLAS bytes stay within the cap. Both sides are build sizes
// (before compaction): a set is admitted with every part's build size reserved, so compaction only
// ever makes room.
_FORCE_INLINE_ bool nextgeo_rt_native_fits_memory(uint64_t p_native_bytes, uint64_t p_new_bytes, uint64_t p_cap) {
	return p_native_bytes + p_new_bytes <= p_cap;
}

// MN4 refused a set. The same mesh's sets that no instance traces as its own band's set (held only
// as a previous set while instances wait for their band, or unused) are reclaimable: when releasing
// them makes room, they leave at the next TLAS phase, their instances take the proxy, and the
// refused set is admitted then. Native memory never holds both.
_FORCE_INLINE_ bool nextgeo_rt_native_reclaim_makes_room(uint64_t p_native_bytes, uint64_t p_reclaimable_bytes, uint64_t p_new_bytes, uint64_t p_cap) {
	return p_reclaimable_bytes > 0 && p_reclaimable_bytes <= p_native_bytes && nextgeo_rt_native_fits_memory(p_native_bytes - p_reclaimable_bytes, p_new_bytes, p_cap);
}

// N7: native BLAS builds spend what line 1b left this frame, at most the native rebuild cap.
_FORCE_INLINE_ uint32_t nextgeo_rt_native_build_budget(uint32_t p_line_1b_remaining) {
	return MIN(p_line_1b_remaining, uint32_t(NEXTGEO_RT_NATIVE_REBUILD_TRIANGLE_CAP));
}

// Read-only view of a DAG header (8 bytes, then 60-byte group records; plan §6.3).
struct NextGeoRTNativeDag {
	const uint8_t *data = nullptr;
	uint32_t size = 0;
	uint32_t group_count = 0;

	bool init(const uint8_t *p_data, uint32_t p_size);
	uint32_t group_cluster_offset(uint32_t p_group) const;
	uint32_t group_cluster_count(uint32_t p_group) const;
	float group_parent_error(uint32_t p_group) const;
	AABB group_aabb(uint32_t p_group) const;
};

// A cut over the resident pages: the local clusters of the mesh, ordered by (surface, cluster).
struct NextGeoRTNativeCut {
	LocalVector<uint32_t> clusters;
	uint32_t triangle_count = 0;
	uint32_t vertex_count = 0;
	float max_error = 0.0f; // Largest own error of the selected clusters (0 for source clusters).
	uint64_t hash = 0;
};

// The error-only cut at p_error, the GPU cut's rule with the projected error replaced by the
// mesh-space error: a group is entered when it is refine-ok and its parent error exceeds p_error;
// a cluster is taken unless its own error exceeds p_error and the group it refines is refine-ok.
// Clusters of surfaces in p_excluded_surfaces (bit per surface) are left out. Returns false when
// the mesh has no usable DAG or cluster table.
bool nextgeo_rt_native_select_cut(const RendererVirtualMeshStorage &p_storage, const RendererVirtualMeshStorage::MeshInfo &p_info, float p_error, uint32_t p_excluded_surfaces, NextGeoRTNativeCut &r_cut);

// A cut is used only when it has triangles and nowhere is coarser than the fixed proxy.
_FORCE_INLINE_ bool nextgeo_rt_native_cut_usable(const NextGeoRTNativeCut &p_cut, float p_proxy_eps) {
	return p_cut.triangle_count > 0 && p_cut.max_error <= p_proxy_eps;
}

// One BLAS geometry of a native part: a run of one surface's triangles.
struct NextGeoRTNativeRun {
	uint32_t surface = 0;
	uint32_t first_triangle = 0; // In the part.
	uint32_t triangle_count = 0;
};

// One BLAS part: whole clusters up to the 128k-triangle cap, then surface runs (geometry index =
// run, in surface order, like the proxy).
struct NextGeoRTNativePartPlan {
	uint32_t first_cluster = 0; // In the cut's cluster list.
	uint32_t cluster_count = 0;
	uint32_t triangle_count = 0;
	uint32_t vertex_count = 0;
	LocalVector<NextGeoRTNativeRun> runs;
};

// p_triangles, p_vertices and p_surfaces hold one entry per cut cluster, in cut order (surfaces
// non-decreasing). A part never splits a cluster and never exceeds p_max_triangles unless a single
// cluster does.
void nextgeo_rt_native_split_parts(const uint32_t *p_triangles, const uint32_t *p_vertices, const uint32_t *p_surfaces, uint32_t p_count, uint32_t p_max_triangles, LocalVector<NextGeoRTNativePartPlan> &r_parts);

// RT-arena layout of a part: float3 positions, then uint32 indices, then a u16 Stage-2 card group
// per triangle for each run, every block 16-byte aligned (the surface record keeps the group
// table's offset in its upper 28 bits, plan §6.3).
struct NextGeoRTNativePartLayout {
	uint32_t index_offset = 0;
	LocalVector<uint32_t> run_group_offsets;
	uint32_t size = 0;
};

void nextgeo_rt_native_part_layout(const NextGeoRTNativePartPlan &p_part, NextGeoRTNativePartLayout &r_layout);

// Stage-2 card groups (#77). The proxy's cards are keyed by the group of the proxy cut; when no
// group needed depth layers those keys are the DAG group indices themselves (direct).
// Returns true when every card lies in the bounds of the DAG group its key names, a proxy-cut
// level group (parent error above the proxy eps).
bool nextgeo_rt_native_cards_are_dag_groups(const NextGeoRTNativeDag &p_dag, const uint8_t *p_cards, uint32_t p_card_count, float p_proxy_eps);

// The card group of a proxy-cut level group: the group itself when direct, else the first card
// group whose bounds center lies in the group's bounds, else NEXTGEO_RT_NATIVE_NO_CARD (the hit
// falls back to the WRC).
uint16_t nextgeo_rt_native_card_of_proxy_group(const NextGeoRTNativeDag &p_dag, uint32_t p_group, bool p_direct, const uint8_t *p_cards, uint32_t p_card_count, float p_proxy_eps);

// Walks up the DAG (p_parent_of: the group of a cluster that refines each group, or UINT32_MAX)
// from p_group to the first group whose parent error exceeds the proxy eps: the proxy-cut level
// group that covers p_group. Returns UINT32_MAX when the chain is broken.
uint32_t nextgeo_rt_native_proxy_group(const NextGeoRTNativeDag &p_dag, uint32_t p_group, const uint32_t *p_parent_of, uint32_t p_parent_count, float p_proxy_eps);

// The proxy's triangle order (#68, #77): whole clusters packed into parts of at most
// p_max_part_triangles in cluster-list order, then a stable sort by surface inside each part. Gives
// each proxy cluster's first triangle in that order, where its Stage-2 card groups start in
// rt_proxy_triangle_groups. False when the counts do not add up to p_total_triangles.
bool nextgeo_rt_native_proxy_triangle_bases(const uint32_t *p_triangles, const uint32_t *p_surfaces, uint32_t p_count, uint32_t p_max_part_triangles, uint32_t p_total_triangles, LocalVector<uint32_t> &r_bases);

// The most common card group among p_count u16 groups from p_first (ties: the lowest group).
uint16_t nextgeo_rt_native_majority_card(const uint8_t *p_groups, uint32_t p_group_count, uint32_t p_first, uint32_t p_count);

// The proxy-level groups (parent error above the proxy eps) above p_group, over every parent
// (p_parents[p_parent_first[g]] up to p_parent_first[g + 1]: the groups holding clusters simplified
// from g). A group's simplified clusters land in several parent groups, and together they cover
// it, so these groups' proxy triangles cover every finer cluster of p_group. Sorted; p_group itself
// when it is at the proxy level; empty when no parent is known.
void nextgeo_rt_native_proxy_ancestors(const NextGeoRTNativeDag &p_dag, uint32_t p_group, const uint32_t *p_parent_first, const uint32_t *p_parents, uint32_t p_group_count, float p_proxy_eps, LocalVector<uint32_t> &r_groups);

// The descriptors of card group p_group, [r_first, r_end) of the descriptors sorted by (group, axis).
void nextgeo_rt_native_card_range(const uint8_t *p_cards, uint32_t p_card_count, uint16_t p_group, uint32_t &r_first, uint32_t &r_end);

// A triangle's dominant face axis as the proxy's card layers use it (0..5: +X, -X, +Y, -Y, +Z,
// -Z); false when it has no area.
bool nextgeo_rt_native_triangle_axis(const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_c, uint32_t &r_axis);

// The Stage-2 card group of one native triangle that the cut does not share with the proxy: of the
// cards on its dominant axis, the one whose box grown by p_tolerance is nearest its centroid, first
// among p_candidates (descriptor indices), then, when none holds it and the mesh has at most
// p_search_all_limit cards, among all of them; ties take the thinner box along the axis, then the
// smaller one. p_default when no card is on its axis or it has no area. The CPU copy of
// rt_cut_extract.glsl's cards pass.
uint16_t nextgeo_rt_native_triangle_card(const uint8_t *p_cards, uint32_t p_card_count, const uint32_t *p_candidates, uint32_t p_candidate_count, uint32_t p_search_all_limit, const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_c, float p_tolerance, uint16_t p_default);

// The bucket state machine of the native mode, device-free so the doctests can run it: every
// device, storage and setting access goes through the hooks below, which NextGIRTNative implements.
//
// Sets live in a per-mesh pool keyed by the cut's hash, so bands whose cuts are equal (the far bands
// often cap at the proxy's error) share one set by construction. A bucket holds the set its
// instances trace (current) and the set it waits for (pending); a pending set replaces the current
// one at the TLAS phase once it is built, so a residency change never takes an instance off its
// native set. A set is admitted against the memory cap as a whole, with the build size of every
// part reserved, before any part is built; a refused set is cut again only when the cap has room
// for its size. The cap is held at every TLAS phase: when it shrinks (a mesh leaves, the AS line
// fills), sets leave until native memory is within it again.
class NextGeoRTNativeSets {
public:
	typedef MeshStorage::NextGIRTPart Part;

	struct Stats {
		uint32_t buckets = 0; // Buckets with a current or pending set.
		uint32_t current_sets = 0; // Distinct sets some bucket traces.
		uint32_t pending_sets = 0; // Distinct sets no bucket traces yet.
		uint32_t parts = 0; // In current sets.
		uint64_t triangles = 0; // In current sets.
		uint64_t blas_bytes = 0; // Every set's BLASes as they are now (compacted or not).
		uint64_t build_bytes = 0; // Admitted sets at build size (the MN4 side).
		uint64_t fallback_bytes = 0; // Proxy BLAS build bytes of the meshes native mode covers (the MN4 base).
		uint64_t fallback_compacted_bytes = 0; // The same proxies as they are now.
		uint64_t memory_cap_bytes = 0; // nextgeo_rt_native_memory_cap() at the last TLAS or builds phase.
		uint64_t as_budget_bytes = 0; // The AS line and its proxy and legacy BLAS bytes (set_as_line()).
		uint64_t as_proxy_bytes = 0;
		uint32_t band_mask = 0; // Bands with a current set, over all meshes.
		uint32_t shared_buckets = 0; // Buckets whose current set another bucket of the mesh also traces.
		uint32_t coarser_sets = 0; // Current sets with fewer triangles than their mesh's proxy.
		int32_t last_refusal_band = -1; // The last MN4 refusal: band, set triangles, BLAS bytes, headroom.
		uint32_t last_refusal_triangles = 0;
		uint64_t last_refusal_bytes = 0;
		uint64_t last_refusal_headroom = 0;
		uint32_t instances_band = 0; // This frame's TLAS instances on their band's set.
		uint32_t instances_previous = 0; // On a previous band's set.
		uint32_t instances_proxy = 0; // On the proxy while their set is pending or refused.
		uint32_t traced_triangles_max = 0; // This frame: the most and fewest triangles of a set an instance traces.
		uint32_t traced_triangles_min = 0;
		uint32_t excluded[NEXTGEO_RT_NATIVE_EXCLUSION_MAX] = {};
		uint32_t coarse_buckets = 0; // This frame: cuts coarser than the proxy (missing pages).
		uint32_t memory_refusals = 0; // Since the native mode started.
		uint32_t reclaims = 0; // Since the native mode started: MN4 released a mesh's sets for another of its sets.
		uint32_t cap_releases = 0; // Since the native mode started: sets released because the cap shrank.
		uint32_t build_triangles = 0; // This frame's native BLAS builds.
		uint32_t build_count = 0;
		uint32_t extract_triangles = 0; // This frame's extractions.
		uint32_t extract_count = 0;
		uint32_t swaps = 0; // Sets that replaced a bucket's set at this frame's TLAS phase.
		uint32_t compactions = 0; // This frame.
		uint64_t no_card_triangles = 0; // In current sets, triangles that can find no Stage-2 card group.
		uint64_t chosen_card_triangles = 0; // In current sets, triangles whose card group the GPU chose.
		uint32_t card_meshes = 0; // Meshes whose Stage-2 card keys were checked.
		uint32_t direct_card_meshes = 0; // Of those, meshes whose card keys are DAG groups.
		uint32_t proxy_card_meshes = 0; // Meshes whose native triangles take the proxy's card groups.
		uint64_t proxy_card_triangles = 0; // In current sets, triangles with the proxy's exact group.
	};

protected:
	struct Set {
		LocalVector<Part> parts;
		uint64_t hash = 0;
		float max_error = 0.0f; // The cut's largest own cluster error.
		uint32_t triangles = 0;
		uint32_t proxy_triangles = 0; // The mesh's proxy triangles in the TLAS when the set was cut.
		uint32_t built = 0; // Parts [0, built) are built.
		uint64_t bytes = 0; // Build size of every part, reserved at admission.
		bool admitted = false;
		bool reclaim = false; // MN4 released it for another set of the mesh: it leaves at the next TLAS phase.
		uint32_t refs = 0; // Buckets holding it as current or pending.
		uint64_t last_band_use_frame = 0; // Some instance traced it as its own band's set.
		uint64_t no_card_triangles = 0;
		uint64_t chosen_card_triangles = 0;
		uint64_t proxy_card_triangles = 0;
		bool ready() const { return admitted && !parts.is_empty() && built == parts.size(); }
	};

	struct Bucket {
		uint64_t current = 0; // Set hashes, valid with has_current / has_pending.
		uint64_t pending = 0;
		bool has_current = false;
		bool has_pending = false;
		// The ray origin eps of each: the larger of the band's error target when it took the set and
		// the set's own error (missing pages can leave a usable cut coarser than the target).
		float current_eps = 0.0f;
		float pending_eps = 0.0f;
		float target_error = -1.0f;
		uint64_t generation = UINT64_MAX;
		bool coarse = false;
		// The cap refused (or released) this bucket's cut: it is cut again only once the cap has room
		// for refused_bytes, or when the cut changes.
		bool refused = false;
		uint64_t refused_hash = 0;
		uint64_t refused_bytes = 0;
		uint64_t last_demand_frame = 0;
		uint64_t last_use_frame = 0;
	};

	struct MeshState {
		RID mesh;
		Bucket buckets[NEXTGEO_RT_NATIVE_BAND_COUNT];
		HashMap<uint64_t, Set> sets;
		uint64_t last_demand_frame = 0;
		// The largest proxy build size seen while the mesh has native state: the MN4 base keeps it while
		// the proxy is rebuilt (dirty, defragmented), so a rebuild does not release native sets.
		uint64_t proxy_build_bytes = 0;
	};

	struct InstanceUse {
		RID mesh;
		uint32_t band = 0;
		uint64_t frame = 0;
	};

	struct PendingCompact {
		RID mesh;
		uint64_t hash = 0;
		uint32_t part = 0;
		uint32_t triangles = 0;
	};

	HashMap<RID, MeshState> meshes;
	HashMap<uint64_t, InstanceUse> instance_uses;
	LocalVector<PendingCompact> pending_compactions;
	Stats stats;
	uint64_t frame = 0;
	uint64_t previous_frame = 0;
	float error_per_meter = 0.0f;
	bool orthogonal = false;
	uint64_t as_budget_bytes = UINT64_MAX;
	uint64_t as_proxy_bytes = 0;

	// Hooks (NextGIRTNative: the rendering device, mesh storage and virtual mesh storage).
	// The mesh still has virtual geometry and live instances.
	virtual bool _mesh_alive(RID p_mesh) = 0;
	// The proxy's eps and the residency generation; false when the mesh cannot be cut.
	virtual bool _mesh_cut_inputs(RID p_mesh, float &r_proxy_eps, uint64_t &r_generation) = 0;
	virtual bool _mesh_cut(RID p_mesh, float p_error, NextGeoRTNativeCut &r_cut) = 0;
	// Extracts the cut into new RT-arena ranges: r_set.parts with their triangle counts, ranges and
	// geometries, proxy_triangles and the card counters. False (nothing held) on failure.
	virtual bool _set_extract(MeshState &r_state, const NextGeoRTNativeCut &p_cut, Set &r_set) = 0;
	// Creates the part's BLAS without building it; r_bytes is its build size.
	virtual bool _part_create(Part &r_part, uint64_t &r_bytes) = 0;
	virtual void _part_build(Part &r_part) = 0;
	virtual bool _part_compact(Part &r_part) = 0;
	virtual uint64_t _part_bytes(const Part &p_part) const = 0;
	virtual void _part_free_blas(Part &r_part) = 0;
	// p_now: the range is free for reuse at once; else after the frames that may read it.
	virtual void _part_free_range(RID p_mesh, Part &r_part, bool p_now) = 0;
	virtual void _mesh_proxy_bytes(RID p_mesh, uint64_t &r_build_bytes, uint64_t &r_current_bytes) = 0;
	// The mesh's native state is gone; p_now as in _part_free_range().
	virtual void _mesh_released(RID, bool) {}
	virtual void _update_card_totals() {}

	void _free_set(RID p_mesh, Set &r_set, bool p_now);
	void _unref(MeshState &r_state, uint64_t p_hash, bool p_now);
	void _drop_current(MeshState &r_state, Bucket &r_bucket, bool p_now);
	void _drop_pending(MeshState &r_state, Bucket &r_bucket, bool p_now);
	void _release_state(MeshState &r_state, bool p_now);
	uint64_t _reclaimable_bytes(MeshState &r_state, uint64_t p_exclude_hash, bool p_mark);
	void _refuse(MeshState &r_state, uint64_t p_hash, uint64_t p_bytes, uint32_t p_triangles, uint64_t p_headroom, uint32_t p_band, bool p_memory);
	uint64_t _memory(uint64_t &r_native_bytes);
	void _hold_cap();
	void _update_totals();

public:
	// The TLAS phase (line 1a), before any select(): reclaimed and unused sets leave, finished sets
	// replace the ones they were cut to replace, and sets leave while native memory is over the cap.
	void begin_frame_sets(uint64_t p_frame, float p_error_per_meter, bool p_orthogonal);
	// The TLAS phase, per NextGeo instance whose mesh enters through its proxy. Returns the native
	// parts the instance traces this frame, or nullptr to keep the proxy. r_eps is the ray origin
	// eps of those parts for this instance's bucket.
	const LocalVector<Part> *select(RID p_mesh, uint64_t p_instance_key, const AABB &p_world_bounds, const Transform3D &p_transform, const Vector3 &p_camera, NextGeoRTNativeExclusion p_exclusion, float &r_eps);
	// The builds phase (line 1b): cuts again where the residency or the band's target changed,
	// extracts new sets, admits them against the cap and builds their parts within p_triangle_budget.
	void build(uint32_t p_triangle_budget);
	// After this frame's traces: compacts built native BLASes within the shared per-frame cap.
	// Returns true when a compaction was queued (the TLAS referencing it becomes stale).
	bool compact_after_traces(uint32_t &r_triangles_this_frame, uint32_t p_triangle_cap);
	// The AS memory line after this frame's proxy and legacy BLASes; the cap of the next phases.
	void set_as_line(uint64_t p_budget_bytes, uint64_t p_proxy_bytes);
	// The mesh's virtual geometry changed or its last instance left: its sets are freed now, as its
	// proxy is (between frames; see release_mesh in next_gi_rt_native.cpp).
	void release_mesh(RID p_mesh);
	void release_all();

	uint64_t get_blas_bytes() const { return stats.blas_bytes; }
	const Stats &get_stats() const { return stats; }

	virtual ~NextGeoRTNativeSets() {}
};

class NextGIRTNative : public NextGeoRTNativeSets {
	struct CardState {
		LocalVector<uint32_t> cluster_group;
		LocalVector<uint32_t> parent_of;
		// Every parent of each group (the groups holding its simplified resident clusters), as
		// parents[parent_first[g]] up to parent_first[g + 1].
		LocalVector<uint32_t> parent_first;
		LocalVector<uint32_t> parents;
		LocalVector<uint16_t> group_card; // Memo per DAG group; UINT16_MAX - 1 is unknown.
		bool cards_checked = false;
		bool cards_direct = false;
		// The proxy's card groups (#77) per resident proxy cluster: exact for the clusters a native
		// cut shares with the proxy. A finer cluster's triangles choose among the card groups of the
		// proxy-level groups above it on the GPU (nextgeo_rt_native_triangle_card), each on its own:
		// a proxy-level group's cards are depth layers (parallel walls of one group never share a card).
		HashMap<uint32_t, uint32_t> proxy_base; // Local cluster -> first triangle in the proxy order.
		// Per DAG group g: its proxy triangles' card groups, most common first, at
		// group_candidates[group_candidate_first[g]] up to group_candidate_first[g + 1].
		LocalVector<uint32_t> group_candidate_first;
		LocalVector<uint16_t> group_candidates;
		bool proxy_map = false;
		uint64_t proxy_map_tried = UINT64_MAX;
	};

	struct Retired {
		NextGIArenaAllocator::Allocation allocation;
		uint64_t frame = 0;
		RID mesh; // release_mesh() frees the mesh's retired ranges with its proxy's.
	};

	struct Extractor;

	HashMap<RID, CardState> card_states;
	LocalVector<Retired> retired;
	Extractor *extractor = nullptr;

	bool _ensure_extractor();
	uint16_t _card_group(CardState &r_state, const RendererVirtualMeshStorage::MeshInfo &p_info, const NextGeoRTNativeDag &p_dag, uint32_t p_group, float p_proxy_eps);
	bool _map_proxy(CardState &r_state, const RendererVirtualMeshStorage::MeshInfo &p_info);
	void _refresh_parents(CardState &r_state, const RendererVirtualMeshStorage::MeshInfo &p_info, const NextGeoRTNativeDag &p_dag);
	static uint32_t _excluded_surfaces(RID p_mesh);
	static void _virtual_geometry_changed(void *p_userdata, RID p_mesh);

protected:
	virtual bool _mesh_alive(RID p_mesh) override;
	virtual bool _mesh_cut_inputs(RID p_mesh, float &r_proxy_eps, uint64_t &r_generation) override;
	virtual bool _mesh_cut(RID p_mesh, float p_error, NextGeoRTNativeCut &r_cut) override;
	virtual bool _set_extract(MeshState &r_state, const NextGeoRTNativeCut &p_cut, Set &r_set) override;
	virtual bool _part_create(Part &r_part, uint64_t &r_bytes) override;
	virtual void _part_build(Part &r_part) override;
	virtual bool _part_compact(Part &r_part) override;
	virtual uint64_t _part_bytes(const Part &p_part) const override;
	virtual void _part_free_blas(Part &r_part) override;
	virtual void _part_free_range(RID p_mesh, Part &r_part, bool p_now) override;
	virtual void _mesh_proxy_bytes(RID p_mesh, uint64_t &r_build_bytes, uint64_t &r_current_bytes) override;
	virtual void _mesh_released(RID p_mesh, bool p_now) override;
	virtual void _update_card_totals() override;

public:
	// The TLAS phase (line 1a): the view's error per meter, the RT-arena ranges whose frames are done,
	// then begin_frame_sets().
	void begin_frame(const ::RenderDataRD *p_render_data, uint64_t p_frame);
	void free();

	NextGIRTNative();
	~NextGIRTNative();
};

} // namespace RendererRD
